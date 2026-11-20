#include "store.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "paths.h"

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
};

std::atomic<int> connectionCounter{0};

const char feedColumns[] =
    "SELECT f.id, f.url, f.title, f.site_url, f.description, c.name, f.etag, f.last_modified, f.last_error,"
    " f.last_fetched_at,"
    " (SELECT COUNT(*) FROM items i WHERE i.feed_id = f.id AND i.is_read = 0),"
    " (SELECT COUNT(*) FROM items i WHERE i.feed_id = f.id)"
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
    feed.unread = query.value(10).toInt();
    feed.total = query.value(11).toInt();
    return feed;
}

QVariant timestamp(const QDateTime &time) {
    return time.isValid() ? QVariant(time.toSecsSinceEpoch()) : QVariant(QMetaType::fromType<qint64>());
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
    }
    if (!query.exec(QStringLiteral("PRAGMA user_version = %1").arg(migrations.size()))) {
        rollback();
        return fail(QStringLiteral("cannot record the schema version: %1").arg(query.lastError().text()));
    }
    query.finish();
    return commit();
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

int Store::saveItems(qint64 feedId, const QList<ParsedItem> &items) {
    if (!begin())
        return -1;
    QSqlQuery exists(m_db), upsert(m_db);
    exists.prepare(QStringLiteral("SELECT 1 FROM items WHERE feed_id = ? AND guid = ?"));
    upsert.prepare(QStringLiteral(
        "INSERT INTO items (feed_id, guid, title, url, author, summary, content, published_at, created_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(feed_id, guid) DO UPDATE SET title = excluded.title, url = excluded.url,"
        " author = excluded.author, summary = excluded.summary, content = excluded.content,"
        " published_at = COALESCE(excluded.published_at, items.published_at)"));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    int added = 0;
    for (const ParsedItem &item : items) {
        exists.addBindValue(feedId);
        exists.addBindValue(item.guid);
        if (!exists.exec()) {
            const QString message = exists.lastError().text();
            rollback();
            return fail(message), -1;
        }
        const bool isNew = !exists.next();
        exists.finish();
        upsert.addBindValue(feedId);
        upsert.addBindValue(item.guid);
        upsert.addBindValue(item.title);
        upsert.addBindValue(item.url);
        upsert.addBindValue(item.author);
        upsert.addBindValue(item.summary);
        upsert.addBindValue(item.content);
        upsert.addBindValue(timestamp(item.published));
        upsert.addBindValue(now);
        if (!upsert.exec()) {
            const QString message = upsert.lastError().text();
            rollback();
            return fail(message), -1;
        }
        added += isNew;
    }
    exists.finish();
    upsert.finish();
    return commit() ? added : -1;
}

namespace {
const char articleColumns[] =
    "SELECT i.id, i.feed_id, f.title, i.guid, i.title, i.url, i.author, i.summary, %1,"
    " i.published_at, i.is_read, i.is_starred FROM items i JOIN feeds f ON f.id = i.feed_id";

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
    return article;
}

// The WHERE conditions a filter adds, after "WHERE 1".
QString filterSql(const ArticleFilter &filter, QVariantList &values) {
    QString sql;
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
    const QString order = QStringLiteral(" ORDER BY COALESCE(i.published_at, i.created_at) DESC, i.id DESC");
    QString sql = QString::fromLatin1(articleColumns).arg(filter.withContent ? "i.content" : "NULL");
    if (!filter.alsoId) {
        sql += " WHERE 1" + filterSql(filter, values) + order + " LIMIT ?";
        values << (filter.limit > 0 ? filter.limit : -1);
    } else {
        // The limit applies to the rest, so the open article is listed even
        // when it is older than every one of them, still within feedId.
        ArticleFilter rest = filter;
        rest.alsoId = 0;
        sql += " WHERE i.id IN (SELECT id FROM (SELECT i.id FROM items i WHERE 1" + filterSql(rest, values)
               + " AND i.id <> ?" + order + " LIMIT ?) UNION SELECT i.id FROM items i WHERE i.id = ?";
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
    if (!query.exec(QStringLiteral("SELECT COUNT(*), COALESCE(SUM(is_read = 0), 0),"
                                   " (SELECT COUNT(*) FROM items WHERE is_starred = 1), COALESCE(MAX(id), 0) FROM items"))
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
    query.prepare(QStringLiteral("UPDATE items AS i SET is_read = 1 WHERE i.is_read = 0 AND i.id <= ?")
                  + filterSql(filter, values));
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

bool Store::setRead(qint64 articleId, bool read) { return setFlag("is_read", articleId, read); }
bool Store::setStarred(qint64 articleId, bool starred) { return setFlag("is_starred", articleId, starred); }
