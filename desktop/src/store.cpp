#include "store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
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
// transaction. Append new ones; never edit a shipped one.
const QList<QStringList> migrations = {
    {
        "CREATE TABLE categories ("
        " id INTEGER PRIMARY KEY,"
        " name TEXT NOT NULL UNIQUE)",
        "CREATE TABLE feeds ("
        " id INTEGER PRIMARY KEY,"
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
        " id INTEGER PRIMARY KEY,"
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
        return fail(QStringLiteral("no data directory: set XDG_DATA_HOME or an absolute READSTR_HOME"));
    return open(path);
}

bool Store::open(const QString &path) {
    // The library lists what the user reads, so only they may read it: the
    // directory is 0700 and the database 0600 (SQLite gives its -wal and -shm
    // files the database's mode).
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory))
        return fail(QStringLiteral("cannot create %1").arg(directory));
    const QByteArray encodedDirectory = QFile::encodeName(directory);
    if (::chmod(encodedDirectory.constData(), 0700) != 0)
        return fail(QStringLiteral("cannot restrict %1: %2").arg(directory, QString::fromLocal8Bit(std::strerror(errno))));
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return fail(QStringLiteral("cannot open %1: %2").arg(path, QString::fromLocal8Bit(std::strerror(errno))));
    const bool restricted = ::fchmod(fd, 0600) == 0;
    ::close(fd);
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
    if (!query.exec(QStringLiteral("PRAGMA journal_mode = WAL")) || !query.next()
        || query.value(0).toString() != QLatin1String("wal"))
        return fail(QStringLiteral("cannot switch %1 to WAL mode").arg(path));
    for (const char *pragma : {"PRAGMA foreign_keys = ON", "PRAGMA busy_timeout = 5000", "PRAGMA synchronous = NORMAL"})
        if (!query.exec(QString::fromLatin1(pragma)))
            return fail(query.lastError().text());
    return migrate();
}

int Store::schemaVersion() {
    QSqlQuery query(m_db);
    return query.exec(QStringLiteral("PRAGMA user_version")) && query.next() ? query.value(0).toInt() : -1;
}

bool Store::migrate() {
    const int current = schemaVersion();
    if (current < 0)
        return fail(QStringLiteral("cannot read the schema version"));
    if (current > migrations.size())
        return fail(QStringLiteral("the library was written by a newer readstr (schema %1, this one knows %2)")
                        .arg(current)
                        .arg(migrations.size()));
    for (int version = current; version < migrations.size(); ++version) {
        if (!m_db.transaction())
            return fail(m_db.lastError().text());
        QSqlQuery query(m_db);
        for (const QString &statement : migrations.at(version)) {
            if (!query.exec(statement)) {
                const QString message = query.lastError().text();
                m_db.rollback();
                return fail(QStringLiteral("migration %1 failed: %2").arg(version + 1).arg(message));
            }
        }
        if (!query.exec(QStringLiteral("PRAGMA user_version = %1").arg(version + 1)) || !m_db.commit()) {
            m_db.rollback();
            return fail(QStringLiteral("migration %1 failed to commit").arg(version + 1));
        }
    }
    return true;
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
    if (!m_db.transaction())
        return fail(m_db.lastError().text()), -1;
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
            m_db.rollback();
            return fail(exists.lastError().text()), -1;
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
            m_db.rollback();
            return fail(message), -1;
        }
        added += isNew;
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return fail(m_db.lastError().text()), -1;
    }
    return added;
}

QList<Article> Store::articles(const ArticleFilter &filter) {
    m_error.clear();
    QString sql = QStringLiteral(
        "SELECT i.id, i.feed_id, f.title, i.guid, i.title, i.url, i.author, i.summary, i.content,"
        " i.published_at, i.is_read, i.is_starred FROM items i JOIN feeds f ON f.id = i.feed_id WHERE 1");
    QVariantList values;
    if (filter.feedId) {
        sql += " AND i.feed_id = ?";
        values << filter.feedId;
    }
    if (filter.unreadOnly)
        sql += " AND i.is_read = 0";
    if (filter.starredOnly)
        sql += " AND i.is_starred = 1";
    sql += " ORDER BY COALESCE(i.published_at, i.created_at) DESC, i.id DESC LIMIT ?";
    values << (filter.limit > 0 ? filter.limit : -1);

    QList<Article> result;
    QSqlQuery query(m_db);
    query.prepare(sql);
    for (const QVariant &value : values)
        query.addBindValue(value);
    if (!query.exec()) {
        fail(query.lastError().text());
        return result;
    }
    while (query.next()) {
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
        result << article;
    }
    return result;
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
