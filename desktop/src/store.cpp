#include "store.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "feedparser.h"
#include "markdown.h"
#include "paths.h"
#include "sanitizer.h"
#include "sync.h"

namespace {
// Each entry moves the schema up one version (PRAGMA user_version), inside a
// transaction. Append new ones; never edit a shipped one. Feed and item ids
// are AUTOINCREMENT so a deleted row's id never comes back: the headless
// commands act on ids a user read from an earlier listing.
const QList<QStringList> migrations = {
    {
        "CREATE TABLE categories ("
        " id INTEGER PRIMARY KEY,"
        " name TEXT NOT NULL UNIQUE)",
        "CREATE TABLE feeds ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " url TEXT NOT NULL,"
        " url_key TEXT NOT NULL UNIQUE,"
        " title TEXT NOT NULL DEFAULT '',"
        " site_url TEXT,"
        " description TEXT,"
        " category_id INTEGER REFERENCES categories(id) ON DELETE SET NULL,"
        " etag TEXT,"
        " last_modified TEXT,"
        " last_fetched_at INTEGER,"
        " last_error TEXT,"
        " created_at INTEGER NOT NULL)",
        "CREATE TABLE items ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " feed_id INTEGER NOT NULL REFERENCES feeds(id) ON DELETE CASCADE,"
        " guid TEXT NOT NULL,"
        " title TEXT NOT NULL DEFAULT '',"
        " url TEXT,"
        " author TEXT,"
        " summary TEXT,"
        " content TEXT,"
        " published_at INTEGER,"
        " created_at INTEGER NOT NULL,"
        " is_read INTEGER NOT NULL DEFAULT 0,"
        " is_starred INTEGER NOT NULL DEFAULT 0,"
        " UNIQUE(feed_id, guid))",
        "CREATE INDEX items_by_date ON items(COALESCE(published_at, created_at) DESC, id DESC)",
        "CREATE INDEX items_by_feed_unread ON items(feed_id, is_read)",
        "CREATE INDEX items_starred ON items(is_starred) WHERE is_starred = 1",
        "CREATE INDEX feeds_by_category ON feeds(category_id)",
    },
    {
        // Nostr long-form articles are replaceable: one address, many
        // versions, of which the latest (by revision) is kept.
        "ALTER TABLE items ADD COLUMN address TEXT",
        "ALTER TABLE items ADD COLUMN revision INTEGER",
        "CREATE INDEX items_by_address ON items(feed_id, address) WHERE address IS NOT NULL",
        // A Nostr author's relay hints (a JSON array), the created_at and id
        // of the profile the feed's details came from (the id breaks a tie
        // between two from the same second), the title that profile gave the
        // feed (which a newer profile may replace, where a title from OPML
        // stays), and when every relay last answered a refresh.
        "ALTER TABLE feeds ADD COLUMN relay_hints TEXT",
        "ALTER TABLE feeds ADD COLUMN profile_revision INTEGER",
        "ALTER TABLE feeds ADD COLUMN profile_event_id TEXT",
        "ALTER TABLE feeds ADD COLUMN profile_title TEXT",
        "ALTER TABLE feeds ADD COLUMN complete_at INTEGER",
    },
    {
        // A Nostr video's (NIP-71) media and thumbnail URLs, and its length
        // in seconds.
        "ALTER TABLE items ADD COLUMN media_url TEXT",
        "ALTER TABLE items ADD COLUMN thumbnail_url TEXT",
        "ALTER TABLE items ADD COLUMN duration REAL",
        // Nostr feeds were asked only for long-form articles before, so the
        // first refresh after this one asks for everything again, or the
        // videos older than the last complete refresh would never come.
        "UPDATE feeds SET complete_at = NULL WHERE url LIKE 'npub1%'",
    },
    {
        // Whether a Nostr author's last saved refresh missed something, so
        // the window can say so. complete_at cannot tell: it keeps the time
        // of the last complete refresh, however many incomplete ones came
        // after it.
        "ALTER TABLE feeds ADD COLUMN incomplete INTEGER NOT NULL DEFAULT 0",
    },
    {
        // The npub whose sync events are pulled (a public key only, as
        // Readstr never signs), its nprofile's relay hints, and when a pull
        // last succeeded. One row at most.
        "CREATE TABLE sync_account ("
        " id INTEGER PRIMARY KEY CHECK (id = 1),"
        " npub TEXT NOT NULL,"
        " pubkey TEXT NOT NULL,"
        " relay_hints TEXT,"
        " synced_at INTEGER)",
        // The created_at and id of the last event applied per d tag, so an
        // older or repeated one is never applied again, as the web app keeps
        // it. The id breaks a tie in created_at the way NIP-01 does.
        "CREATE TABLE sync_applied ("
        " d_tag TEXT PRIMARY KEY,"
        " created_at INTEGER NOT NULL,"
        " event_id TEXT NOT NULL)",
        // The read GUIDs of the last kind 30405 event applied (when the web
        // app publishes one; it does not yet), so articles that arrive later
        // are marked read too.
        "CREATE TABLE sync_read (guid TEXT PRIMARY KEY) WITHOUT ROWID",
        // Read GUIDs are matched across feeds, as the web app keys them.
        "CREATE INDEX items_by_guid ON items(guid)",
        // Saved views from kind 30406, in order: the view as the web app
        // publishes it (without keywords.exclude) as JSON, and the mute words
        // (a JSON array), which are local only and never come from a relay.
        "CREATE TABLE saved_views ("
        " id TEXT PRIMARY KEY,"
        " position INTEGER NOT NULL,"
        " view TEXT NOT NULL,"
        " exclude TEXT)",
    },
    {
        // When an article was last marked read here, so the read status
        // published lists the newest reads first, as the web app does.
        "ALTER TABLE items ADD COLUMN read_at INTEGER",
        // Whether this machine publishes its read articles (kind 30405),
        // which is public; off until asked for, as on the web.
        "ALTER TABLE sync_account ADD COLUMN publish_reads INTEGER NOT NULL DEFAULT 0",
        // The content of the newest event per d tag the relays are known to
        // hold, pulled and applied or published here, which a publish builds
        // on so it keeps what only another device knows, and when it was
        // stored here, so a feed subscribed here after a list marking it
        // deleted came can be told from one that list deleted.
        "CREATE TABLE sync_remote ("
        " d_tag TEXT PRIMARY KEY,"
        " event_id TEXT NOT NULL,"
        " content TEXT NOT NULL,"
        " stored_at INTEGER NOT NULL DEFAULT 0)",
        // Whether "Publish now" confirmed the account, which automatic
        // publishing waits for, and the kinds (a bit mask) changed here and
        // not published yet, so they survive a restart. A new account starts
        // with neither, as clearSyncState() deletes the row; one from before
        // publishing starts unconfirmed too.
        "ALTER TABLE sync_account ADD COLUMN publish_confirmed INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE sync_account ADD COLUMN publish_pending INTEGER NOT NULL DEFAULT 0",
    },
    {
        // Full-text search over each item's title, author, and body: its
        // summary and the plain text of its content, as the reader shows it
        // (searchBody()), so no tag, attribute, or link address it hides
        // matches. SQL cannot strip HTML, so an external-content table,
        // which triggers keep from the item's own columns, would index the
        // markup; and a stored plain-text column would hold every article's
        // text twice.
        // So the index is contentless (it keeps only the index, rowid being
        // the item's id) and saveItems() writes it, in the same transaction
        // as the item, whenever the text changes. A deleted item leaves it
        // by this trigger, which a feed's cascade fires as well, whatever
        // deletes it. Contentless deletes need SQLite 3.43; Store::open()
        // checks first. The tokenizer folds case and diacritics (creme
        // finds crème). The last word of a search is a prefix, and each
        // keystroke searches, so prefixes of two and three characters have
        // an index of their own; it is set here because a contentless table
        // cannot be rebuilt with one later. migrate() fills it from the
        // existing items.
        "CREATE VIRTUAL TABLE items_fts USING fts5(title, author, body, content='', contentless_delete=1,"
        " prefix='2 3', tokenize='unicode61 remove_diacritics 2')",
        "CREATE TRIGGER items_fts_delete AFTER DELETE ON items BEGIN"
        " DELETE FROM items_fts WHERE rowid = old.id; END",
    },
    {
        // Keyword filter rules, in order, each as the JSON object the web
        // app keeps in localStorage (KeywordFilter::toJson()), so a rule is
        // checked as it is read, as there. They stay in this file: no sync
        // event carries them. Here rather than in settings.json, whose 64 KiB
        // cap 200 rules of 200 characters can pass, and so that the headless
        // refresh applies them too.
        "CREATE TABLE filter_rules (position INTEGER PRIMARY KEY, rule TEXT NOT NULL)",
        // What the rules make of each item (KeywordFilter::Outcome::flags()),
        // worked out as it is saved and again for every item when the rules
        // change, so lists and unread counts leave hidden items out in SQL
        // rather than running every rule over the library on each reload.
        "ALTER TABLE items ADD COLUMN filter_flags INTEGER NOT NULL DEFAULT 0",
        // The rules every item's flags were last worked out for
        // (KeywordFilter::flagsKey()), or NULL while a pass over the library
        // is under way, so a change that alters no flag is not applied
        // again, and a pass cut short is finished later. No rules hide
        // anything at first.
        "CREATE TABLE filter_state (id INTEGER PRIMARY KEY CHECK (id = 1), applied TEXT)",
        "INSERT INTO filter_state (id, applied) VALUES (1, '[]')",
    },
};

// The condition that leaves out an item a filter rule hides.
#define NOT_HIDDEN(alias) "(" alias "filter_flags & 1) = 0"
static_assert(KeywordFilter::Outcome::Hidden == 1, "NOT_HIDDEN tests the Hidden bit");

// The migration that creates items_fts, after which migrate() indexes the
// items already there.
constexpr int searchIndexVersion = 7;

// Title, author, and body, as bm25() weighs a match in each.
const char searchScore[] = "bm25(items_fts, 10.0, 5.0, 1.0)";

// The text of a Markdown article without its link and image addresses,
// "[text](address)" keeping only the text: one pass, and a "](" that is not
// closed on its line is kept as written, so hostile input stays linear.
QString withoutLinkTargets(const QString &markdown) {
    QString text;
    text.reserve(markdown.size());
    const qsizetype size = markdown.size();
    for (qsizetype i = 0; i < size;) {
        if (markdown.at(i) != ']' || i + 1 >= size || markdown.at(i + 1) != '(') {
            text += markdown.at(i++);
            continue;
        }
        qsizetype end = i + 2;
        while (end < size && markdown.at(end) != ')' && markdown.at(end) != '\n')
            ++end;
        if (end < size && markdown.at(end) == ')') {
            text += QLatin1String("] ");
            i = end + 1;
        } else {
            text += QStringView(markdown).mid(i, end - i);
            i = end;
        }
    }
    // Reference definitions, "[label]: address", show nothing.
    static const QRegularExpression definition(QStringLiteral("^ {0,3}\\[[^\\]\\n]{1,999}\\]:[^\\n]*$"),
                                               QRegularExpression::MultilineOption);
    text.remove(definition);
    return text;
}

// What the search index holds as an item's body: its summary, which is
// plain text already, and the words of its content as the reader shows
// them. HTML goes through the sanitizer first, so what it drops (scripts,
// styles, forms, every attribute) is not indexed either, and then loses its
// tags. Markdown (a Nostr long-form article's) loses only its link
// addresses: the reader renders it without HTML, showing any tag, comment,
// or autolink as the text it is, so stripping tags would drop words the
// reader shows (all of an article after an unclosed <style>, an <https://…>
// autolink, the text of `a<b && c>d`). Its entities are decoded, as the
// reader shows them, and its Markdown punctuation the tokenizer skips. Both
// are cut where the reader cuts them.
QString searchBody(const QString &summary, const QString &content, const QString &address, const QString &url) {
    QString text;
    if (!content.isEmpty()) {
        if (!address.isEmpty()) {
            QString markdown = content.left(Markdown::maxInputLength);
            if (content.size() > Markdown::maxInputLength && markdown.back().isHighSurrogate())
                markdown.chop(1);
            text = FeedParser::cleanText(FeedParser::decodeEntities(withoutLinkTargets(markdown)));
        } else {
            Sanitizer::Options options;
            options.imageLabels = false;
            // The sanitizer keeps only Qt's tags, and of those an inline
            // one (a, b, em, code, ...) is no break in the reader's text,
            // so it is none in the index; its blocks and <br> still part
            // words, as do the spaces it puts around an image's description.
            text = FeedParser::plainText(Sanitizer::toRichText(content, QUrl(url), options),
                                         FeedParser::InlineTags::Join);
        }
        // The sanitizer's breaks in long words, which would split them.
        text.remove(QChar(0x200B));
    }
    if (summary.isEmpty())
        return text;
    return text.isEmpty() ? summary : summary + QLatin1Char('\n') + text;
}

std::atomic<int> connectionCounter{0};

const char feedColumns[] =
    "SELECT f.id, f.url, f.title, f.site_url, f.description, c.name, f.etag, f.last_modified, f.last_error,"
    " f.last_fetched_at, f.relay_hints, f.complete_at,"
    " (SELECT COUNT(*) FROM items i WHERE i.feed_id = f.id AND i.is_read = 0 AND " NOT_HIDDEN("i.") "),"
    " (SELECT COUNT(*) FROM items i WHERE i.feed_id = f.id AND " NOT_HIDDEN("i.") "), f.incomplete, f.created_at"
    " FROM feeds f LEFT JOIN categories c ON c.id = f.category_id";

Feed feedFrom(const QSqlQuery &query) {
    Feed feed;
    feed.id = query.value(0).toLongLong();
    feed.url = query.value(1).toString();
    feed.title = query.value(2).toString();
    feed.siteUrl = query.value(3).toString();
    feed.description = query.value(4).toString();
    feed.category = query.value(5).toString();
    feed.etag = query.value(6).toString();
    feed.lastModified = query.value(7).toString();
    feed.lastError = query.value(8).toString();
    if (!query.value(9).isNull())
        feed.lastFetchedAt = QDateTime::fromSecsSinceEpoch(query.value(9).toLongLong(), QTimeZone::UTC);
    feed.relayHints = QJsonDocument::fromJson(query.value(10).toString().toUtf8()).toVariant().toStringList();
    if (!query.value(11).isNull())
        feed.completeAt = QDateTime::fromSecsSinceEpoch(query.value(11).toLongLong(), QTimeZone::UTC);
    feed.unread = query.value(12).toInt();
    feed.total = query.value(13).toInt();
    feed.incomplete = query.value(14).toBool();
    feed.createdAt = QDateTime::fromSecsSinceEpoch(query.value(15).toLongLong(), QTimeZone::UTC);
    return feed;
}

QVariant timestamp(const QDateTime &time) {
    return time.isValid() ? QVariant(time.toSecsSinceEpoch()) : QVariant(QMetaType::fromType<qint64>());
}

QByteArray sqliteVersion(const QSqlDatabase &db) {
    QSqlQuery query(db);
    return query.exec(QStringLiteral("SELECT sqlite_version()")) && query.next() ? query.value(0).toByteArray()
                                                                              : QByteArrayLiteral("unknown version");
}

// NULL for none.
QVariant text(const QString &value) {
    return value.isEmpty() ? QVariant(QMetaType::fromType<QString>()) : QVariant(value);
}

// NULL for none, otherwise a JSON array.
QVariant jsonList(const QStringList &values) {
    if (values.isEmpty())
        return QVariant(QMetaType::fromType<QString>());
    return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(values)).toJson(QJsonDocument::Compact));
}
}

Store::Store() : m_connection(QStringLiteral("readstr-store-%1").arg(connectionCounter++)) {}

Store::~Store() {
    if (m_db.isValid()) {
        m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connection);
    }
}

bool Store::fail(const QString &message) {
    m_error = message;
    return false;
}

bool Store::open() {
    const QString path = Paths::databaseFile();
    if (path.isEmpty())
        return fail(QStringLiteral("no data directory: READSTR_HOME must be an absolute path, or unset it and set "
                                   "XDG_DATA_HOME"));
    return open(path);
}

bool Store::open(const QString &path) {
    // The library lists what the user reads, so only they may read it: the
    // directory is 0700 and the database 0600 (SQLite gives its -wal and -shm
    // files the database's mode). Neither may be a symlink or belong to
    // someone else, so nobody can point the library, or the chmod, elsewhere.
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory))
        return fail(QStringLiteral("cannot create %1").arg(directory));
    const auto failErrno = [this](const QString &what, const QString &where) {
        return fail(QStringLiteral("%1 %2: %3").arg(what, where, QString::fromLocal8Bit(std::strerror(errno))));
    };
    const int directoryFd =
        ::open(QFile::encodeName(directory).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryFd < 0)
        return failErrno(QStringLiteral("cannot open"), directory);
    struct stat info;
    if (::fstat(directoryFd, &info) != 0 || info.st_uid != ::getuid()) {
        ::close(directoryFd);
        return fail(QStringLiteral("%1 is not a directory owned by this user").arg(directory));
    }
    if (::fchmod(directoryFd, 0700) != 0) {
        ::close(directoryFd);
        return fail(QStringLiteral("cannot restrict %1").arg(directory));
    }
    // SQLite opens the file again by path below, so these checks hold only
    // while nobody else can swap the path's directories: true under the
    // default XDG location, which the user owns all the way down, and the
    // reason a READSTR_HOME should not sit in a directory others can write.
    // O_NOFOLLOW refuses a symlink even where O_CREAT would create the file,
    // which is what O_EXCL would guard against, while an existing library
    // still opens.
    const int fd = ::openat(directoryFd, QFile::encodeName(QFileInfo(path).fileName()).constData(),
                            O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    const int openErrno = errno;
    ::close(directoryFd);
    if (fd < 0) {
        errno = openErrno;
        return failErrno(QStringLiteral("cannot open"), path);
    }
    const bool fileOk = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::getuid();
    const bool restricted = fileOk && ::fchmod(fd, 0600) == 0;
    ::close(fd);
    if (!fileOk)
        return fail(QStringLiteral("%1 is not a regular file owned by this user").arg(path));
    if (!restricted)
        return fail(QStringLiteral("cannot restrict %1").arg(path));

    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        return fail(QStringLiteral("Qt's SQLite driver is not installed"));
    if (!m_db.isValid())
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    m_db.close();
    m_db.setDatabaseName(path);
    if (!m_db.open())
        return fail(QStringLiteral("cannot open %1: %2").arg(path, m_db.lastError().text()));

    QSqlQuery query(m_db);
    // The window and the headless commands may use the database at the same
    // time; WAL lets one read while the other writes, and the busy timeout
    // waits out a short write instead of failing.
    if (!query.exec(QStringLiteral("PRAGMA busy_timeout = 5000")))
        return fail(query.lastError().text());
    // Switching a fresh file to WAL needs it to itself, and SQLite answers
    // with the old mode rather than waiting when another connection is
    // opening it too, so the switch is retried for as long as a busy write.
    // Any other error (a read-only file, a disk error) fails at once.
    const QDeadlineTimer walDeadline(5000);
    for (;;) {
        QString problem;
        if (!query.exec(QStringLiteral("PRAGMA journal_mode = WAL"))) {
            problem = query.lastError().text();
            if (query.lastError().nativeErrorCode() != QLatin1String("5"))
                return fail(QStringLiteral("cannot switch %1 to WAL mode: %2").arg(path, problem));
        } else if (query.next() && query.value(0).toString() == QLatin1String("wal")) {
            break;
        } else {
            problem = QStringLiteral("SQLite kept the %1 journal mode").arg(query.value(0).toString());
        }
        query.finish();
        if (walDeadline.hasExpired())
            return fail(QStringLiteral("cannot switch %1 to WAL mode: %2").arg(path, problem));
        QThread::msleep(20);
    }
    query.finish();
    for (const char *pragma : {"PRAGMA foreign_keys = ON", "PRAGMA synchronous = NORMAL"})
        if (!query.exec(QString::fromLatin1(pragma)))
            return fail(query.lastError().text());
    // Search needs FTS5 with contentless deletes in the SQLite Qt's driver
    // loaded, which a distribution may build without; asked of a throwaway
    // table in the connection's own temporary schema, so the library is
    // neither locked nor touched, and refused with the reason rather than
    // with a migration or a search failing on it later.
    if (!query.exec(QStringLiteral("CREATE VIRTUAL TABLE temp.readstr_fts_check USING fts5(text, content='',"
                                   " contentless_delete=1, tokenize='unicode61 remove_diacritics 2')")))
        return fail(QStringLiteral("this SQLite (%1) cannot search: Readstr needs SQLite 3.43 or later with FTS5 (%2)")
                        .arg(QString::fromLatin1(sqliteVersion(m_db)), query.lastError().text()));
    if (!query.exec(QStringLiteral("DROP TABLE temp.readstr_fts_check")))
        return fail(query.lastError().text());
    return migrate();
}

int Store::schemaVersion() {
    QSqlQuery query(m_db);
    return query.exec(QStringLiteral("PRAGMA user_version")) && query.next() ? query.value(0).toInt() : -1;
}

bool Store::begin() {
    // IMMEDIATE takes the write lock up front, so what a transaction reads
    // cannot go stale before it writes (SQLITE_BUSY_SNAPSHOT under WAL); a
    // writer in another process is waited out by the busy timeout.
    QSqlQuery query(m_db);
    return query.exec(QStringLiteral("BEGIN IMMEDIATE")) || fail(query.lastError().text());
}

bool Store::commit() {
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("COMMIT")))
        return true;
    const QString message = query.lastError().text();
    rollback();
    return fail(message);
}

void Store::rollback() { QSqlQuery(m_db).exec(QStringLiteral("ROLLBACK")); }

bool Store::transaction(const std::function<bool()> &work) {
    if (!begin())
        return false;
    bool ok = false;
    try {
        ok = work();
    } catch (...) {
        // Otherwise the open transaction holds the write lock until this
        // connection closes, and the next begin() on it fails.
        rollback();
        throw;
    }
    if (ok)
        return commit();
    rollback();
    return false;
}

bool Store::migrate() {
    // The common case, an up-to-date library, takes no write lock.
    int current = schemaVersion();
    if (current == migrations.size())
        return true;
    // Another process may be migrating the same file, so the version that
    // decides what to run is read again under the write lock.
    if (!begin())
        return false;
    current = schemaVersion();
    if (current < 0) {
        rollback();
        return fail(QStringLiteral("cannot read the schema version"));
    }
    if (current > migrations.size()) {
        rollback();
        return fail(QStringLiteral("the library was written by a newer readstr (schema %1, this one knows %2)")
                        .arg(current)
                        .arg(migrations.size()));
    }
    QSqlQuery query(m_db);
    for (int version = current; version < migrations.size(); ++version) {
        for (const QString &statement : migrations.at(version)) {
            if (!query.exec(statement)) {
                const QString message = query.lastError().text();
                rollback();
                return fail(QStringLiteral("migration %1 failed: %2").arg(version + 1).arg(message));
            }
        }
        if (version + 1 == searchIndexVersion && !backfillSearchIndex()) {
            const QString message = m_error;
            rollback();
            return fail(QStringLiteral("migration %1 failed: %2").arg(version + 1).arg(message));
        }
    }
    if (!query.exec(QStringLiteral("PRAGMA user_version = %1").arg(migrations.size()))) {
        rollback();
        return fail(QStringLiteral("cannot record the schema version: %1").arg(query.lastError().text()));
    }
    query.finish();
    return commit();
}

bool Store::prepareIndex(QSqlQuery &insert) {
    return insert.prepare(QStringLiteral("INSERT OR REPLACE INTO items_fts (rowid, title, author, body)"
                                         " VALUES (?, ?, ?, ?)"))
           || fail(insert.lastError().text());
}

bool Store::indexItem(QSqlQuery &insert, qint64 id, const QString &title, const QString &author,
                      const QString &body) {
    insert.addBindValue(id);
    insert.addBindValue(title);
    insert.addBindValue(author);
    insert.addBindValue(body);
    return insert.exec() || fail(insert.lastError().text());
}

bool Store::backfillSearchIndex() {
    QSqlQuery items(m_db), insert(m_db);
    items.setForwardOnly(true);
    if (!prepareIndex(insert))
        return false;
    if (!items.exec(QStringLiteral("SELECT id, title, author, summary, content, address, url FROM items")))
        return fail(items.lastError().text());
    while (items.next())
        if (!indexItem(insert, items.value(0).toLongLong(), items.value(1).toString(), items.value(2).toString(),
                       searchBody(items.value(3).toString(), items.value(4).toString(), items.value(5).toString(),
                                  items.value(6).toString())))
            return false;
    return !items.lastError().isValid() || fail(items.lastError().text());
}

QString Store::matchExpression(const QString &query) {
    QString limited = query.left(maxQueryLength);
    // Never half of a surrogate pair, which the cut (or a text field's own
    // limit) can leave at the end.
    if (!limited.isEmpty() && limited.back().isHighSurrogate())
        limited.chop(1);
    // Control characters become spaces and bidirectional formatting goes,
    // as in every text field, so none reaches FTS5's parser.
    const QStringList chunks = FeedParser::cleanText(limited).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList terms;
    bool lastIsOneLetter = false;
    for (const QString &chunk : chunks) {
        // A term of only punctuation would be an empty phrase, which matches
        // nothing; the tokenizer splits words where the term has punctuation.
        const QList<uint> points = chunk.toUcs4();
        const auto letters =
            std::count_if(points.begin(), points.end(), [](uint point) { return QChar::isLetterOrNumber(point); });
        if (letters == 0)
            continue;
        // The term is rebuilt from its code points, in which toUcs4() made
        // every unpaired surrogate U+FFFD: Qt binds the text as UTF-16, and
        // SQLite reads a lone high surrogate together with the unit after
        // it, which would swallow the closing quote.
        QString quoted;
        quoted.reserve(chunk.size() + 2);
        quoted += QLatin1Char('"');
        for (const uint point : points)
            if (point == '"')
                quoted += QLatin1String("\"\"");
            else
                quoted += QStringView(QChar::fromUcs4(char32_t(point)));
        quoted += QLatin1Char('"');
        terms << quoted;
        // The prefix applies to the last word the tokenizer finds in the
        // term (the "b" of "foo-b"), so that is the word measured: its run of
        // letters and digits, with combining marks kept inside it. A lone
        // letter of a script written without spaces (Chinese, Japanese,
        // Thai) still matches as a prefix, as the tokenizer keeps a whole
        // run of such text as one word and a single one would find nothing.
        auto end = points.rbegin();
        while (!QChar::isLetterOrNumber(*end))
            ++end;
        qsizetype lastWord = 0;
        uint lastLetter = 0;
        for (auto it = end; it != points.rend() && (QChar::isLetterOrNumber(*it) || QChar::isMark(*it)); ++it)
            if (QChar::isLetterOrNumber(*it)) {
                ++lastWord;
                lastLetter = *it;
            }
        lastIsOneLetter = lastWord == 1 && QChar::category(lastLetter) != QChar::Letter_Other;
        if (terms.size() == maxQueryTerms)
            break;
    }
    if (terms.isEmpty())
        return {};
    // A last word of a single letter or digit is matched whole: as a prefix it
    // would merge every word it starts, which no prefix index covers, on
    // each keystroke.
    if (!lastIsOneLetter)
        terms.last() += QLatin1Char('*');
    return terms.join(QLatin1Char(' '));
}

QString Store::urlKey(const QString &url) {
    const QUrl parsed(url.trimmed());
    if (!parsed.isValid() || parsed.host().isEmpty()) {
        QString key = url.trimmed().toLower();
        while (key.endsWith('/'))
            key.chop(1);
        return key;
    }
    QString path = parsed.path(QUrl::FullyEncoded);
    while (path.endsWith('/'))
        path.chop(1);
    // The web app drops the port; this keeps it, so feeds served from
    // different ports on one host stay distinct.
    QString key = parsed.host().toLower();
    // An IPv6 host is bracketed, as in the URL, so its last group cannot be
    // read as the port.
    if (key.contains(':'))
        key = '[' + key + ']';
    if (parsed.port() != -1)
        key += ':' + QString::number(parsed.port());
    key += path;
    if (parsed.hasQuery()) {
        auto items = QUrlQuery(parsed).queryItems(QUrl::FullyEncoded);
        std::stable_sort(items.begin(), items.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        QStringList pairs;
        for (const auto &[name, value] : items)
            pairs << name + '=' + value;
        if (!pairs.isEmpty())
            key += '?' + pairs.join('&');
    }
    return key;
}

QList<Feed> Store::feeds() {
    m_error.clear();
    QList<Feed> result;
    QSqlQuery query(m_db);
    if (!query.exec(QString::fromLatin1(feedColumns) + " ORDER BY lower(f.title), f.id")) {
        fail(query.lastError().text());
        return result;
    }
    while (query.next())
        result << feedFrom(query);
    if (query.lastError().isValid()) {
        fail(query.lastError().text());
        return {};
    }
    return result;
}

std::optional<Feed> Store::feed(qint64 id) {
    QSqlQuery query(m_db);
    query.prepare(QString::fromLatin1(feedColumns) + " WHERE f.id = ?");
    query.addBindValue(id);
    if (!query.exec()) {
        fail(query.lastError().text());
        return std::nullopt;
    }
    if (!query.next()) {
        fail(QStringLiteral("no feed with id %1").arg(id));
        return std::nullopt;
    }
    return feedFrom(query);
}

qint64 Store::categoryId(const QString &name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return 0;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT INTO categories (name) VALUES (?) ON CONFLICT(name) DO NOTHING"));
    query.addBindValue(trimmed);
    if (!query.exec())
        return fail(query.lastError().text()), -1;
    query.prepare(QStringLiteral("SELECT id FROM categories WHERE name = ?"));
    query.addBindValue(trimmed);
    if (!query.exec() || !query.next())
        return fail(query.lastError().text()), -1;
    return query.value(0).toLongLong();
}

qint64 Store::addFeed(const QString &url, const QString &title, const QString &category, bool *created) {
    if (created)
        *created = false;
    const qint64 category_ = categoryId(category);
    if (category_ < 0)
        return 0;
    const QString key = urlKey(url);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT INTO feeds (url, url_key, title, category_id, created_at)"
                                 " VALUES (?, ?, ?, ?, ?) ON CONFLICT(url_key) DO NOTHING"));
    query.addBindValue(url.trimmed());
    query.addBindValue(key);
    query.addBindValue(title.trimmed().isEmpty() ? url.trimmed() : title.trimmed());
    query.addBindValue(category_ ? QVariant(category_) : QVariant(QMetaType::fromType<qint64>()));
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    if (!query.exec())
        return fail(query.lastError().text()), 0;
    if (created)
        *created = query.numRowsAffected() == 1;
    query.prepare(QStringLiteral("SELECT id FROM feeds WHERE url_key = ?"));
    query.addBindValue(key);
    if (!query.exec() || !query.next())
        return fail(query.lastError().text()), 0;
    return query.value(0).toLongLong();
}

bool Store::removeFeed(qint64 id) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM feeds WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec())
        return fail(query.lastError().text());
    return query.numRowsAffected() == 1 || fail(QStringLiteral("no feed with id %1").arg(id));
}

bool Store::updateFeedDetails(qint64 id, const ParsedFeed &parsed) {
    // The feed's own title replaces a placeholder (the URL), but never a name
    // the user brought along from OPML.
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE feeds SET title = CASE WHEN (title = '' OR title = url) AND ? <> '' THEN ? ELSE title END,"
        " site_url = ?, description = ? WHERE id = ?"));
    query.addBindValue(parsed.title);
    query.addBindValue(parsed.title);
    query.addBindValue(parsed.siteUrl);
    query.addBindValue(parsed.description);
    query.addBindValue(id);
    return query.exec() || fail(query.lastError().text());
}

bool Store::updateProfile(qint64 id, const ParsedFeed &profile, qint64 revision, const QString &eventId) {
    // Only a newer profile than the one the details came from, so a relay
    // sending an old one cannot bring back what the author has changed. Of
    // two from the same second the lower id wins, as NIP-01 says. The title
    // is replaced when it is a placeholder or an earlier profile's name,
    // never when the user brought it along from OPML.
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE feeds SET"
        " title = CASE WHEN (title = '' OR title = url OR title = profile_title) AND ? <> '' THEN ? ELSE title END,"
        " profile_title = CASE WHEN (title = '' OR title = url OR title = profile_title) AND ? <> ''"
        " THEN ? ELSE profile_title END,"
        " site_url = ?, description = ?, profile_revision = ?, profile_event_id = ?"
        " WHERE id = ? AND (profile_revision IS NULL OR profile_revision < ?"
        " OR (profile_revision = ? AND (profile_event_id IS NULL OR profile_event_id > ?)))"));
    for (int i = 0; i < 4; ++i)
        query.addBindValue(profile.title);
    query.addBindValue(profile.siteUrl);
    query.addBindValue(profile.description);
    query.addBindValue(revision);
    query.addBindValue(eventId);
    query.addBindValue(id);
    query.addBindValue(revision);
    query.addBindValue(revision);
    query.addBindValue(eventId);
    return query.exec() || fail(query.lastError().text());
}

bool Store::setRelayHints(qint64 id, const QStringList &relays) {
    QSqlQuery query(m_db);
    // NULL for none, otherwise a JSON array.
    QVariant hints(QMetaType::fromType<QString>());
    if (!relays.isEmpty())
        hints = QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(relays)).toJson(QJsonDocument::Compact));
    query.prepare(QStringLiteral("UPDATE feeds SET relay_hints = ? WHERE id = ?"));
    query.addBindValue(hints);
    query.addBindValue(id);
    return query.exec() || fail(query.lastError().text());
}

bool Store::recordComplete(qint64 id, const QDateTime &time) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE feeds SET complete_at = ? WHERE id = ?"));
    query.addBindValue(time.toSecsSinceEpoch());
    query.addBindValue(id);
    return query.exec() || fail(query.lastError().text());
}

bool Store::setIncomplete(qint64 id, bool incomplete) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE feeds SET incomplete = ? WHERE id = ?"));
    query.addBindValue(incomplete ? 1 : 0);
    query.addBindValue(id);
    return query.exec() || fail(query.lastError().text());
}

bool Store::recordFetch(qint64 id, const QString &etag, const QString &lastModified, const QString &error) {
    QSqlQuery query(m_db);
    if (error.isEmpty()) {
        query.prepare(QStringLiteral("UPDATE feeds SET etag = ?, last_modified = ?, last_error = NULL,"
                                     " last_fetched_at = ? WHERE id = ?"));
        query.addBindValue(etag.isEmpty() ? QVariant(QMetaType::fromType<QString>()) : QVariant(etag));
        query.addBindValue(lastModified.isEmpty() ? QVariant(QMetaType::fromType<QString>()) : QVariant(lastModified));
    } else {
        query.prepare(QStringLiteral("UPDATE feeds SET last_error = ?, last_fetched_at = ? WHERE id = ?"));
        query.addBindValue(error);
    }
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.addBindValue(id);
    return query.exec() || fail(query.lastError().text());
}

int Store::saveItems(qint64 feedId, const QList<ParsedItem> &items, int *markedRead) {
    if (markedRead)
        *markedRead = 0;
    if (!begin())
        return -1;
    const KeywordFilter::Rules *rules = compiledFilterRules();
    if (!rules) {
        rollback();
        return -1;
    }
    QSqlQuery exists(m_db), upsert(m_db), version(m_db), replace(m_db), index(m_db), synced(m_db);
    synced.prepare(QStringLiteral("SELECT 1 FROM sync_read WHERE guid = ?"));
    exists.prepare(QStringLiteral("SELECT id, title, author, summary, content, address, url, filter_flags FROM items"
                                  " WHERE feed_id = ? AND guid = ?"));
    upsert.prepare(QStringLiteral(
        "INSERT INTO items (feed_id, guid, title, url, author, summary, content, published_at, created_at, address,"
        " revision, media_url, thumbnail_url, duration, is_read, read_at, filter_flags)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ? OR EXISTS (SELECT 1 FROM sync_read WHERE guid = ?), ?, ?)"
        " ON CONFLICT(feed_id, guid) DO UPDATE SET title = excluded.title, url = excluded.url,"
        " author = excluded.author, summary = excluded.summary, content = excluded.content,"
        " published_at = COALESCE(excluded.published_at, items.published_at), media_url = excluded.media_url,"
        " thumbnail_url = excluded.thumbnail_url, duration = excluded.duration,"
        " filter_flags = excluded.filter_flags"));
    version.prepare(QStringLiteral(
        "SELECT id, revision, guid FROM items WHERE feed_id = ? AND address = ? ORDER BY revision DESC LIMIT 1"));
    // A newer version takes the older one's row, so the article keeps its
    // place and its read and starred flags.
    replace.prepare(QStringLiteral(
        "UPDATE items SET guid = ?, title = ?, url = ?, author = ?, summary = ?, content = ?,"
        " published_at = COALESCE(?, published_at), revision = ?, filter_flags = ? WHERE id = ?"));
    auto failed = [this](QSqlQuery &query) {
        const QString message = query.lastError().text();
        rollback();
        fail(message);
        return -1;
    };
    auto indexFailed = [this] {
        const QString message = m_error;
        rollback();
        fail(message);
        return -1;
    };
    if (!prepareIndex(index)) {
        rollback();
        return -1;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    int added = 0, marked = 0;
    const auto outcome = [rules](const ParsedItem &item, const QString &body) {
        return rules->isEmpty() ? KeywordFilter::Outcome{} : rules->evaluate(item.title, body, item.author);
    };
    for (const ParsedItem &item : items) {
        if (!item.address.isEmpty()) {
            version.addBindValue(feedId);
            version.addBindValue(item.address);
            if (!version.exec())
                return failed(version);
            if (version.next()) {
                const qint64 rowId = version.value(0).toLongLong();
                const qint64 revision = version.value(1).toLongLong();
                const QString guid = version.value(2).toString();
                version.finish();
                // An older or the same version, perhaps from a slower relay.
                // Of two from the same second the lower id wins, as NIP-01
                // says, so every reader settles on the same one.
                if (revision > item.revision || (revision == item.revision && guid <= item.guid))
                    continue;
                replace.addBindValue(item.guid);
                replace.addBindValue(item.title);
                replace.addBindValue(item.url);
                replace.addBindValue(item.author);
                replace.addBindValue(item.summary);
                replace.addBindValue(item.content);
                replace.addBindValue(timestamp(item.published));
                // The new version keeps the old one's read flag, so a rule
                // that marks read does not reach it.
                const QString body = searchBody(item.summary, item.content, item.address, item.url);
                replace.addBindValue(item.revision);
                replace.addBindValue(outcome(item, body).flags());
                replace.addBindValue(rowId);
                if (!replace.exec())
                    return failed(replace);
                if (!indexItem(index, rowId, item.title, item.author, body))
                    return indexFailed();
                continue;
            }
            version.finish();
        }
        exists.addBindValue(feedId);
        exists.addBindValue(item.guid);
        if (!exists.exec())
            return failed(exists);
        const bool isNew = !exists.next();
        qint64 rowId = 0;
        // A refresh sends most items again as they were; only new or changed
        // text is indexed again, sparing the sanitizer. The URL counts too:
        // the sanitizer resolves relative images against it, so a new one
        // can bring an image's description into the index or take it out.
        bool textChanged = true;
        QString address = item.address;
        int flags = 0;
        if (!isNew) {
            rowId = exists.value(0).toLongLong();
            textChanged = exists.value(1).toString() != item.title || exists.value(2).toString() != item.author
                          || exists.value(3).toString() != item.summary
                          || exists.value(4).toString() != item.content || exists.value(6).toString() != item.url;
            // The upsert keeps the address the row has, which says how its
            // content reads.
            address = exists.value(5).toString();
            flags = exists.value(7).toInt();
        }
        exists.finish();
        QString body;
        bool markRead = false;
        if (textChanged) {
            body = searchBody(item.summary, item.content, address, item.url);
            const KeywordFilter::Outcome filtered = outcome(item, body);
            flags = filtered.flags();
            markRead = isNew && filtered.markRead;
        }
        // A new item the last synced reads list is read already: a rule
        // that would mark it read changes nothing, so it is neither counted
        // nor given a read time, and publishes nothing.
        if (markRead) {
            synced.addBindValue(item.guid);
            if (!synced.exec())
                return failed(synced);
            markRead = !synced.next();
            synced.finish();
        }
        upsert.addBindValue(feedId);
        upsert.addBindValue(item.guid);
        upsert.addBindValue(item.title);
        upsert.addBindValue(item.url);
        upsert.addBindValue(item.author);
        upsert.addBindValue(item.summary);
        upsert.addBindValue(item.content);
        upsert.addBindValue(timestamp(item.published));
        upsert.addBindValue(now);
        upsert.addBindValue(item.address.isEmpty() ? QVariant(QMetaType::fromType<QString>()) : QVariant(item.address));
        upsert.addBindValue(item.address.isEmpty() ? QVariant(QMetaType::fromType<qint64>()) : QVariant(item.revision));
        upsert.addBindValue(text(item.mediaUrl));
        upsert.addBindValue(text(item.thumbnailUrl));
        upsert.addBindValue(item.duration >= 0 ? QVariant(item.duration) : QVariant(QMetaType::fromType<double>()));
        upsert.addBindValue(markRead ? 1 : 0);
        upsert.addBindValue(item.guid);
        // Marked read here, so it is published with the user's own reads.
        upsert.addBindValue(markRead ? QVariant(now) : QVariant(QMetaType::fromType<qint64>()));
        upsert.addBindValue(flags);
        if (!upsert.exec())
            return failed(upsert);
        if (isNew)
            rowId = upsert.lastInsertId().toLongLong();
        if (textChanged && !indexItem(index, rowId, item.title, item.author, body))
            return indexFailed();
        // Only what a list can show is news: an article a rule hides is not.
        added += isNew && !(flags & KeywordFilter::Outcome::Hidden);
        marked += markRead;
    }
    // As Backend::markPending() would for a read the user made, so the
    // window publishes it, even after a headless refresh.
    if (marked > 0) {
        QSqlQuery pending(m_db);
        pending.prepare(QStringLiteral(
            "UPDATE sync_account SET publish_pending = publish_pending | ? WHERE publish_reads = 1"));
        pending.addBindValue(publishBit(Sync::readStatusKind));
        if (!pending.exec())
            return failed(pending);
    }
    exists.finish();
    upsert.finish();
    version.finish();
    replace.finish();
    index.finish();
    synced.finish();
    if (!commit())
        return -1;
    if (markedRead)
        *markedRead = marked;
    return added;
}

namespace {
const char articleColumns[] =
    "SELECT i.id, i.feed_id, f.title, i.guid, i.title, i.url, i.author, i.summary, %1,"
    " i.published_at, i.is_read, i.is_starred, i.address, i.media_url, i.thumbnail_url, i.duration, f.url,"
    " i.filter_flags FROM items i JOIN feeds f ON f.id = i.feed_id";

Article articleFrom(const QSqlQuery &query) {
    Article article;
    article.id = query.value(0).toLongLong();
    article.feedId = query.value(1).toLongLong();
    article.feedTitle = query.value(2).toString();
    article.guid = query.value(3).toString();
    article.title = query.value(4).toString();
    article.url = query.value(5).toString();
    article.author = query.value(6).toString();
    article.summary = query.value(7).toString();
    article.content = query.value(8).toString();
    if (!query.value(9).isNull())
        article.published = QDateTime::fromSecsSinceEpoch(query.value(9).toLongLong(), QTimeZone::UTC);
    article.read = query.value(10).toBool();
    article.starred = query.value(11).toBool();
    article.address = query.value(12).toString();
    article.mediaUrl = query.value(13).toString();
    article.thumbnailUrl = query.value(14).toString();
    if (!query.value(15).isNull())
        article.duration = query.value(15).toDouble();
    article.feedUrl = query.value(16).toString();
    article.highlighted = query.value(17).toInt() & KeywordFilter::Outcome::Highlighted;
    return article;
}

// The WHERE conditions a filter adds, after "WHERE 1". An item a filter
// rule hides is never in a view.
QString filterSql(const ArticleFilter &filter, QVariantList &values) {
    QString sql = QStringLiteral(" AND " NOT_HIDDEN("i."));
    if (filter.feedId) {
        sql += " AND i.feed_id = ?";
        values << filter.feedId;
    }
    QStringList only;
    if (filter.unreadOnly)
        only << QStringLiteral("i.is_read = 0");
    if (filter.starredOnly)
        only << QStringLiteral("i.is_starred = 1");
    if (only.isEmpty())
        return sql;
    if (!filter.alsoId)
        return sql + " AND " + only.join(" AND ");
    values << filter.alsoId;
    return sql + " AND ((" + only.join(" AND ") + ") OR i.id = ?)";
}
}

QList<Article> Store::articles(const ArticleFilter &filter) {
    m_error.clear();
    QVariantList values;
    // A search lists only the matches, best first, and newest first among
    // equally good ones.
    const QString match = matchExpression(filter.query);
    const QString searchJoin = match.isEmpty() ? QString()
                                               : QStringLiteral(" JOIN (SELECT rowid AS id, %1 AS score FROM items_fts"
                                                                " WHERE items_fts MATCH ?) s ON s.id = i.id")
                                                     .arg(QLatin1String(searchScore));
    const QString order = QStringLiteral(" ORDER BY ") + (match.isEmpty() ? QString() : QStringLiteral("s.score, "))
                          + QStringLiteral("COALESCE(i.published_at, i.created_at) DESC, i.id DESC");
    QString sql = QString::fromLatin1(articleColumns).arg(filter.withContent ? "i.content" : "NULL") + searchJoin;
    if (!match.isEmpty())
        values << match;
    if (!filter.alsoId) {
        sql += " WHERE 1" + filterSql(filter, values) + order + " LIMIT ?";
        values << (filter.limit > 0 ? filter.limit : -1);
    } else {
        // The limit applies to the rest, so the open article is listed even
        // when it is older than every one of them, still within feedId (and
        // the search, which the join outside holds it to).
        ArticleFilter rest = filter;
        rest.alsoId = 0;
        if (!match.isEmpty())
            values << match;
        sql += " WHERE i.id IN (SELECT id FROM (SELECT i.id FROM items i" + searchJoin + " WHERE 1"
               + filterSql(rest, values) + " AND i.id <> ?" + order
               + " LIMIT ?) UNION SELECT i.id FROM items i WHERE i.id = ? AND " NOT_HIDDEN("i.");
        values << filter.alsoId << (filter.limit > 0 ? filter.limit : -1) << filter.alsoId;
        if (filter.feedId) {
            sql += " AND i.feed_id = ?";
            values << filter.feedId;
        }
        sql += ')' + order;
    }

    QList<Article> result;
    QSqlQuery query(m_db);
    query.prepare(sql);
    for (const QVariant &value : values)
        query.addBindValue(value);
    if (!query.exec()) {
        fail(query.lastError().text());
        return result;
    }
    while (query.next())
        result << articleFrom(query);
    if (query.lastError().isValid()) {
        fail(query.lastError().text());
        return {};
    }
    return result;
}

std::optional<Article> Store::article(qint64 id) {
    QSqlQuery query(m_db);
    query.prepare(QString::fromLatin1(articleColumns).arg("i.content") + " WHERE i.id = ?");
    query.addBindValue(id);
    if (!query.exec()) {
        fail(query.lastError().text());
        return std::nullopt;
    }
    if (!query.next()) {
        fail(QStringLiteral("no article with id %1").arg(id));
        return std::nullopt;
    }
    return articleFrom(query);
}

std::optional<LibraryCounts> Store::counts() {
    QSqlQuery query(m_db);
    // Hidden items count for none of them but the last id, which bounds
    // marking a view read.
    if (!query.exec(QStringLiteral("SELECT COALESCE(SUM(" NOT_HIDDEN("") "), 0),"
                                   " COALESCE(SUM(is_read = 0 AND " NOT_HIDDEN("") "), 0),"
                                   " (SELECT COUNT(*) FROM items WHERE is_starred = 1 AND " NOT_HIDDEN("") "),"
                                   " COALESCE(MAX(id), 0) FROM items"))
        || !query.next()) {
        fail(query.lastError().text());
        return std::nullopt;
    }
    return LibraryCounts{query.value(0).toInt(), query.value(1).toInt(), query.value(2).toInt(),
                         query.value(3).toLongLong()};
}

int Store::markRead(const ArticleFilter &filter, qint64 upToId) {
    QVariantList values;
    QSqlQuery query(m_db);
    QString sql = QStringLiteral("UPDATE items AS i SET is_read = 1, read_at = ? WHERE i.is_read = 0 AND i.id <= ?")
                  + filterSql(filter, values);
    // A search marks only its matches read, as only they are in the view.
    const QString match = matchExpression(filter.query);
    if (!match.isEmpty()) {
        sql += QStringLiteral(" AND i.id IN (SELECT rowid FROM items_fts WHERE items_fts MATCH ?)");
        values << match;
    }
    query.prepare(sql);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.addBindValue(upToId);
    for (const QVariant &value : values)
        query.addBindValue(value);
    if (!query.exec())
        return fail(query.lastError().text()), -1;
    return query.numRowsAffected();
}

bool Store::setFlag(const char *column, qint64 articleId, bool value) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE items SET %1 = ? WHERE id = ?").arg(QLatin1String(column)));
    query.addBindValue(value ? 1 : 0);
    query.addBindValue(articleId);
    if (!query.exec())
        return fail(query.lastError().text());
    return query.numRowsAffected() == 1 || fail(QStringLiteral("no article with id %1").arg(articleId));
}

bool Store::setRead(qint64 articleId, bool read) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE items SET is_read = ?, read_at = ? WHERE id = ?"));
    query.addBindValue(read ? 1 : 0);
    query.addBindValue(read ? QVariant(QDateTime::currentSecsSinceEpoch()) : QVariant(QMetaType::fromType<qint64>()));
    query.addBindValue(articleId);
    if (!query.exec())
        return fail(query.lastError().text());
    return query.numRowsAffected() == 1 || fail(QStringLiteral("no article with id %1").arg(articleId));
}
bool Store::setStarred(qint64 articleId, bool starred) { return setFlag("is_starred", articleId, starred); }

SyncAccount Store::syncAccount() {
    m_error.clear();
    SyncAccount account;
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral(
            "SELECT npub, pubkey, relay_hints, synced_at, publish_reads, publish_confirmed, publish_pending"
            " FROM sync_account WHERE id = 1"))) {
        fail(query.lastError().text());
        return account;
    }
    if (query.next()) {
        account.npub = query.value(0).toString();
        account.pubkey = query.value(1).toString();
        account.relays = QJsonDocument::fromJson(query.value(2).toString().toUtf8()).toVariant().toStringList();
        if (!query.value(3).isNull())
            account.syncedAt = QDateTime::fromSecsSinceEpoch(query.value(3).toLongLong(), QTimeZone::UTC);
        account.publishReads = query.value(4).toBool();
        account.publishConfirmed = query.value(5).toBool();
        account.publishPending = query.value(6).toInt();
    }
    query.finish();
    if (account.pubkey.isEmpty())
        return account;
    if (!query.exec(QStringLiteral("SELECT d_tag, created_at, event_id FROM sync_applied"))) {
        fail(query.lastError().text());
        return {};
    }
    while (query.next()) {
        account.applied.insert(query.value(0).toString(), query.value(1).toLongLong());
        account.appliedIds.insert(query.value(0).toString(), query.value(2).toString());
    }
    return account;
}

bool Store::setSyncAccount(const QString &npub, const QString &pubkey, const QStringList &relays) {
    return transaction([&] {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral("SELECT 1 FROM sync_account WHERE pubkey = ?"));
        query.addBindValue(pubkey);
        if (!query.exec())
            return fail(query.lastError().text());
        const bool same = query.next();
        query.finish();
        // Another account's watermarks and read GUIDs say nothing about this
        // one's events.
        if (!same && !clearSyncState())
            return false;
        query.prepare(QStringLiteral("INSERT INTO sync_account (id, npub, pubkey, relay_hints) VALUES (1, ?, ?, ?)"
                                     " ON CONFLICT(id) DO UPDATE SET npub = excluded.npub, pubkey = excluded.pubkey,"
                                     " relay_hints = excluded.relay_hints"));
        query.addBindValue(npub);
        query.addBindValue(pubkey);
        query.addBindValue(jsonList(relays));
        return query.exec() || fail(query.lastError().text());
    });
}

bool Store::clearSyncState() {
    QSqlQuery query(m_db);
    for (const char *statement :
         {"DELETE FROM sync_account", "DELETE FROM sync_applied", "DELETE FROM sync_read", "DELETE FROM sync_remote"})
        if (!query.exec(QString::fromLatin1(statement)))
            return fail(query.lastError().text());
    return true;
}

bool Store::clearSyncAccount() {
    return transaction([this] { return clearSyncState(); });
}

bool Store::setSyncApplied(const QString &dTag, qint64 createdAt, const QString &eventId) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO sync_applied (d_tag, created_at, event_id) VALUES (?, ?, ?) ON CONFLICT(d_tag)"
        " DO UPDATE SET created_at = excluded.created_at, event_id = excluded.event_id"
        " WHERE excluded.created_at > sync_applied.created_at OR (excluded.created_at = sync_applied.created_at"
        " AND excluded.event_id < sync_applied.event_id)"));
    query.addBindValue(dTag);
    query.addBindValue(createdAt);
    query.addBindValue(eventId);
    return query.exec() || fail(query.lastError().text());
}

bool Store::recordSynced(const QDateTime &time) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE sync_account SET synced_at = ?"));
    query.addBindValue(time.toSecsSinceEpoch());
    return query.exec() || fail(query.lastError().text());
}

int publishBit(int kind) {
    switch (kind) {
    case 30404:
        return 1;
    case 30405:
        return 2;
    case 30406:
        return 4;
    }
    return 0;
}

bool Store::updateSyncAccount(const QString &pubkey, const QString &assignments, const QVariantList &values) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE sync_account SET %1 WHERE pubkey = ?").arg(assignments));
    for (const QVariant &value : values)
        query.addBindValue(value);
    query.addBindValue(pubkey);
    if (!query.exec())
        return fail(query.lastError().text());
    return query.numRowsAffected() == 1 || fail(QStringLiteral("the sync account changed"));
}

bool Store::setPublishReads(const QString &pubkey, bool publish) {
    return updateSyncAccount(pubkey, QStringLiteral("publish_reads = ?"), {publish ? 1 : 0});
}

bool Store::setPublishConfirmed(const QString &pubkey) {
    return updateSyncAccount(pubkey, QStringLiteral("publish_confirmed = 1"), {});
}

bool Store::markPublishPending(const QString &pubkey, int mask, bool pending) {
    return updateSyncAccount(pubkey,
                             pending ? QStringLiteral("publish_pending = publish_pending | ?")
                                     : QStringLiteral("publish_pending = publish_pending & ~?"),
                             {mask});
}

bool Store::rememberReadGuids(const QStringList &guids) {
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("DELETE FROM sync_read")))
        return fail(query.lastError().text());
    query.prepare(QStringLiteral("INSERT INTO sync_read (guid) VALUES (?) ON CONFLICT DO NOTHING"));
    for (const QString &guid : guids) {
        query.addBindValue(guid);
        if (!query.exec())
            return fail(query.lastError().text());
    }
    return true;
}

std::optional<QStringList> Store::rememberedReadGuids() {
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT guid FROM sync_read"))) {
        fail(query.lastError().text());
        return std::nullopt;
    }
    QStringList guids;
    while (query.next())
        guids << query.value(0).toString();
    return guids;
}

bool Store::eachReadGuid(const std::function<bool(const QString &)> &take) {
    QSqlQuery query(m_db);
    query.setForwardOnly(true);
    if (!query.exec(QStringLiteral(
            "SELECT guid FROM items WHERE is_read = 1 ORDER BY read_at IS NULL, read_at DESC, id DESC")))
        return fail(query.lastError().text());
    while (query.next())
        if (!take(query.value(0).toString()))
            break;
    return true;
}

bool Store::setSyncRemote(const QString &dTag, const QString &eventId, const QString &content, qint64 storedAt) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO sync_remote (d_tag, event_id, content, stored_at) VALUES (?, ?, ?, ?) ON CONFLICT(d_tag)"
        " DO UPDATE SET event_id = excluded.event_id, content = excluded.content, stored_at = excluded.stored_at"
        " WHERE sync_remote.event_id <> excluded.event_id"));
    query.addBindValue(dTag);
    query.addBindValue(eventId);
    query.addBindValue(content);
    query.addBindValue(storedAt < 0 ? QDateTime::currentSecsSinceEpoch() : storedAt);
    return query.exec() || fail(query.lastError().text());
}

std::optional<SyncRemote> Store::syncRemote(const QString &dTag) {
    m_error.clear();
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT event_id, content, stored_at FROM sync_remote WHERE d_tag = ?"));
    query.addBindValue(dTag);
    if (!query.exec()) {
        fail(query.lastError().text());
        return std::nullopt;
    }
    if (!query.next())
        return std::nullopt;
    return SyncRemote{query.value(0).toString(), query.value(1).toString(), query.value(2).toLongLong()};
}

int Store::applyReadGuids(const QStringList &guids) {
    if (!rememberReadGuids(guids))
        return -1;
    QSqlQuery query(m_db);
    // Only ever from unread to read: a GUID missing from the list says
    // nothing about an article read here.
    if (!query.exec(QStringLiteral(
            "UPDATE items SET is_read = 1 WHERE is_read = 0 AND guid IN (SELECT guid FROM sync_read)")))
        return fail(query.lastError().text()), -1;
    return query.numRowsAffected();
}

int Store::pendingReadCount() {
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM sync_read s"
                                   " WHERE NOT EXISTS (SELECT 1 FROM items i WHERE i.guid = s.guid)"))
        || !query.next())
        return fail(query.lastError().text()), -1;
    return query.value(0).toInt();
}

QList<SavedView> Store::savedViews() {
    m_error.clear();
    QList<SavedView> result;
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT id, view, exclude FROM saved_views ORDER BY position, id"))) {
        fail(query.lastError().text());
        return result;
    }
    while (query.next())
        result << SavedView{query.value(0).toString(),
                            QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object(),
                            QJsonDocument::fromJson(query.value(2).toString().toUtf8()).toVariant().toStringList()};
    return result;
}

bool Store::replaceSavedViews(const QList<SavedView> &views) {
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("DELETE FROM saved_views")))
        return fail(query.lastError().text());
    query.prepare(QStringLiteral("INSERT INTO saved_views (id, position, view, exclude) VALUES (?, ?, ?, ?)"));
    for (qsizetype i = 0; i < views.size(); ++i) {
        const SavedView &view = views.at(i);
        query.addBindValue(view.id);
        query.addBindValue(i);
        query.addBindValue(QString::fromUtf8(QJsonDocument(view.view).toJson(QJsonDocument::Compact)));
        query.addBindValue(jsonList(view.exclude));
        if (!query.exec())
            return fail(query.lastError().text());
    }
    return true;
}

std::optional<QList<KeywordFilter::Rule>> Store::filterRules(QStringList *dropped) {
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT rule FROM filter_rules ORDER BY position"))) {
        fail(query.lastError().text());
        return std::nullopt;
    }
    QList<KeywordFilter::Rule> rules;
    QSet<QString> ids;
    int entry = 0;
    const auto drop = [dropped, &entry](const QString &why) {
        if (dropped)
            *dropped << QStringLiteral("rule %1: %2").arg(entry).arg(why);
    };
    while (query.next()) {
        ++entry;
        if (rules.size() == KeywordFilter::maxRules) {
            drop(QStringLiteral("more than %1 rules").arg(KeywordFilter::maxRules));
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(query.value(0).toString().toUtf8());
        if (!document.isObject()) {
            drop(QStringLiteral("not a JSON object"));
            continue;
        }
        QString problem;
        auto rule = KeywordFilter::fromJson(document.object(), &problem);
        if (!rule) {
            drop(problem);
            continue;
        }
        if (ids.contains(rule->id))
            rule->id = KeywordFilter::newId();
        ids.insert(rule->id);
        rules << *rule;
    }
    if (query.lastError().isValid()) {
        fail(query.lastError().text());
        return std::nullopt;
    }
    return rules;
}

QStringList Store::filterSource(bool *ok) {
    QSqlQuery query(m_db);
    QStringList source;
    *ok = query.exec(QStringLiteral("SELECT rule FROM filter_rules ORDER BY position"));
    if (!*ok) {
        fail(query.lastError().text());
        return source;
    }
    while (query.next())
        source << query.value(0).toString();
    *ok = !query.lastError().isValid() || fail(query.lastError().text());
    return source;
}

const KeywordFilter::Rules *Store::compiledFilterRules() {
    bool ok = false;
    const QStringList source = filterSource(&ok);
    if (!ok)
        return nullptr;
    if (source != m_filterSource) {
        // What cannot be read is left out here too; the window says so.
        const auto rules = filterRules();
        if (!rules)
            return nullptr;
        m_filterRules = KeywordFilter::Rules(*rules);
        m_filterSource = source;
    }
    return &m_filterRules;
}

namespace {
// Items read, worked out, and written per short transaction when the rules
// change, so no write of the window's or of a headless refresh waits
// longer than one batch takes.
constexpr int refilterBatch = 500;

// The columns searchBody() reads besides the title and author, or NULLs in
// their place when no rule reads the text, so the query keeps its shape.
QString contentColumns(bool needed) {
    return needed ? QStringLiteral("summary, content, address, url") : QStringLiteral("NULL, NULL, NULL, NULL");
}
}

bool Store::applyFilterRules(const std::function<bool()> &superseded, bool *ran) {
    if (ran)
        *ran = false;
    const auto stopped = [&superseded] { return superseded && superseded(); };
    bool ok = false;
    const QStringList source = filterSource(&ok);
    if (!ok)
        return false;
    const auto rules = filterRules();
    if (!rules)
        return false;
    const QString key = KeywordFilter::flagsKey(*rules);
    QSqlQuery state(m_db);
    if (!state.exec(QStringLiteral("SELECT applied FROM filter_state WHERE id = 1")))
        return fail(state.lastError().text());
    if (state.next() && !state.value(0).isNull() && state.value(0).toString() == key)
        return true;
    state.finish();
    if (ran)
        *ran = true;
    const KeywordFilter::Rules compiled(*rules);
    // What an item's flags depend on, and so what is read, and what must be
    // as it was read for the flags worked out from it to be written: with
    // no rules, nothing (only items with flags are read, to clear them);
    // without a rule that reads the text, the title and the author.
    int compared = 2;
    if (compiled.isEmpty())
        compared = 0;
    else if (compiled.needsContent())
        compared = 6;
    QSqlQuery read(m_db);
    read.setForwardOnly(true);
    read.prepare(QStringLiteral("SELECT id, filter_flags, title, author, %1 FROM items WHERE id > ?%2"
                                " ORDER BY id LIMIT ?")
                     .arg(contentColumns(compared == 6),
                          compared == 0 ? QStringLiteral(" AND filter_flags <> 0") : QString()));
    static const char *const textColumns[] = {"title", "author", "summary", "content", "address", "url"};
    QString updateSql = QStringLiteral("UPDATE items SET filter_flags = ? WHERE id = ?");
    for (int column = 0; column < compared; ++column)
        updateSql += QStringLiteral(" AND %1 IS ?").arg(QLatin1String(textColumns[column]));
    struct Change {
        qint64 id = 0;
        int flags = 0;
        // The text the flags were worked out from, as read.
        QVariantList text;
    };
    qint64 last = 0;
    for (;;) {
        if (stopped())
            return true;
        // Read and worked out with no transaction open, so other
        // connections write meanwhile.
        QList<Change> changes;
        int rows = 0;
        read.addBindValue(last);
        read.addBindValue(refilterBatch);
        if (!read.exec())
            return fail(read.lastError().text());
        while (read.next()) {
            ++rows;
            last = read.value(0).toLongLong();
            // The text as search finds it, made only when a rule reads it.
            const auto body = [&read] {
                return searchBody(read.value(4).toString(), read.value(5).toString(), read.value(6).toString(),
                                  read.value(7).toString());
            };
            const int flags =
                compiled.isEmpty()
                    ? 0
                    : compiled.evaluateLazily(read.value(2).toString(), body, read.value(3).toString()).flags();
            if (flags == read.value(1).toInt())
                continue;
            Change change{last, flags, {}};
            for (int column = 0; column < compared; ++column)
                change.text << read.value(2 + column);
            changes << change;
        }
        if (read.lastError().isValid())
            return fail(read.lastError().text());
        read.finish();
        const bool done = rows < refilterBatch;
        if (stopped())
            return true;
        // A pass takes the write lock once per batch, so a writer that
        // keeps it nearly all the time (a long refresh in another process)
        // could outlast one busy timeout; holding nothing, the pass can
        // wait a few in turn.
        bool began = false;
        for (int attempt = 0; attempt < 3 && !began; ++attempt)
            began = begin();
        if (!began)
            return false;
        const bool written = [&] {
            // Newer rules were saved meanwhile, and whoever saved them
            // works the flags out for them; these would only be overwritten.
            const QStringList now = filterSource(&ok);
            if (!ok || now != source)
                return ok;
            QSqlQuery update(m_db);
            // An item whose text changed since it was read was saved since,
            // with these rules or newer ones, and keeps the flags that save
            // gave it.
            update.prepare(updateSql);
            for (const Change &change : std::as_const(changes)) {
                update.addBindValue(change.flags);
                update.addBindValue(change.id);
                for (const QVariant &value : change.text)
                    update.addBindValue(value);
                if (!update.exec())
                    return fail(update.lastError().text());
            }
            if (done) {
                update.prepare(QStringLiteral("UPDATE filter_state SET applied = ? WHERE id = 1"));
                update.addBindValue(key);
                if (!update.exec())
                    return fail(update.lastError().text());
            }
            return true;
        }();
        if (!written) {
            rollback();
            return false;
        }
        if (!commit())
            return false;
        if (done || filterSource(&ok) != source)
            return ok;
    }
}

bool Store::setFilterRules(const QList<KeywordFilter::Rule> &rules, const std::function<bool()> &superseded,
                           bool *saved) {
    if (saved)
        *saved = false;
    if (rules.size() > KeywordFilter::maxRules)
        return fail(QStringLiteral("more than %1 filter rules").arg(KeywordFilter::maxRules));
    for (const KeywordFilter::Rule &rule : rules)
        if (KeywordFilter::check(rule.type, rule.pattern) != KeywordFilter::Problem::None || rule.id.isEmpty())
            return fail(QStringLiteral("not a valid filter rule: %1").arg(rule.pattern));
    const QString key = KeywordFilter::flagsKey(rules);
    const bool kept = transaction([&] {
        QSqlQuery query(m_db);
        if (!query.exec(QStringLiteral("DELETE FROM filter_rules")))
            return fail(query.lastError().text());
        query.prepare(QStringLiteral("INSERT INTO filter_rules (position, rule) VALUES (?, ?)"));
        for (qsizetype i = 0; i < rules.size(); ++i) {
            query.addBindValue(i);
            query.addBindValue(QString::fromUtf8(
                QJsonDocument(KeywordFilter::toJson(rules.at(i), int(i))).toJson(QJsonDocument::Compact)));
            if (!query.exec())
                return fail(query.lastError().text());
        }
        // Until the pass below is done, the flags are those of no one list
        // of rules; a change that leaves them as they are (a disabled rule
        // edited, rules renamed) needs no pass at all.
        query.prepare(QStringLiteral("UPDATE filter_state SET applied = NULL WHERE id = 1 AND applied IS NOT ?"));
        query.addBindValue(key);
        return query.exec() || fail(query.lastError().text());
    });
    if (saved)
        *saved = kept;
    return kept && applyFilterRules(superseded);
}

int Store::markReadByFilter(const QString &ruleId) {
    const auto rules = filterRules();
    if (!rules)
        return -1;
    const auto rule = std::find_if(rules->cbegin(), rules->cend(),
                                   [&ruleId](const KeywordFilter::Rule &each) { return each.id == ruleId; });
    if (rule == rules->cend() || rule->action != KeywordFilter::Action::MarkRead)
        return 0;
    const KeywordFilter::Rules alone({*rule});
    int marked = -1;
    const bool ok = transaction([&] {
        // The flags say which items some rule marks read, and none hidden;
        // of those, the ones this rule matches.
        QSqlQuery query(m_db);
        query.setForwardOnly(true);
        // The text only for a rule that reads it.
        if (!query.exec(QStringLiteral("SELECT id, title, author, %1 FROM items"
                                       " WHERE is_read = 0 AND (filter_flags & 5) = 4")
                            .arg(contentColumns(alone.needsContent()))))
            return fail(query.lastError().text());
        QList<qint64> ids;
        while (query.next()) {
            const auto body = [&query] {
                return searchBody(query.value(3).toString(), query.value(4).toString(), query.value(5).toString(),
                                  query.value(6).toString());
            };
            if (alone.evaluateLazily(query.value(1).toString(), body, query.value(2).toString()).markRead)
                ids << query.value(0).toLongLong();
        }
        if (query.lastError().isValid())
            return fail(query.lastError().text());
        query.finish();
        QSqlQuery update(m_db);
        update.prepare(QStringLiteral("UPDATE items SET is_read = 1, read_at = ? WHERE id = ? AND is_read = 0"));
        marked = 0;
        for (const qint64 id : std::as_const(ids)) {
            update.addBindValue(QDateTime::currentSecsSinceEpoch());
            update.addBindValue(id);
            if (!update.exec())
                return fail(update.lastError().text());
            marked += update.numRowsAffected();
        }
        return true;
    });
    return ok ? marked : -1;
}
