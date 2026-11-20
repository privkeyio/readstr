#pragma once

#include <QDateTime>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <optional>

#include "feedparser.h"

struct Feed {
    qint64 id = 0;
    QString url;
    QString title;
    QString siteUrl;
    QString description;
    QString category;
    QString etag;
    QString lastModified;
    QString lastError;
    QDateTime lastFetchedAt;
    int unread = 0;
    int total = 0;
};

struct Article {
    qint64 id = 0;
    qint64 feedId = 0;
    QString feedTitle;
    QString guid;
    QString title;
    QString url;
    QString author;
    QString summary;
    QString content;
    // Invalid when the feed gave no date; lists sort those by when they arrived.
    QDateTime published;
    bool read = false;
    bool starred = false;
};

struct ArticleFilter {
    qint64 feedId = 0;
    bool unreadOnly = false;
    bool starredOnly = false;
    int limit = 50;
};

// The local library in SQLite: feeds, their items with read and starred flags,
// and categories. Synchronous on purpose for the headless commands; the window
// (phase 1b) moves a Store onto a worker thread that owns the connection, as
// docs/OMARCHY_APP_PLAN.md section 3 describes, so it must stay free of GUI
// types. Every method that can fail returns false, 0, or nullopt and leaves
// the reason in lastError().
class Store {
public:
    Store();
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    // Opens Paths::databaseFile(), creating its directory 0700 and the file
    // 0600, and refuses when Paths has no usable location.
    bool open();
    bool open(const QString &path);
    QString lastError() const { return m_error; }
    int schemaVersion();

    QList<Feed> feeds();
    std::optional<Feed> feed(qint64 id);
    // Returns the id of the feed with this URL, adding it when it is new.
    // URLs that differ only in scheme, host case, a trailing slash, or query
    // order count as the same feed, as in the web app.
    qint64 addFeed(const QString &url, const QString &title, const QString &category = {}, bool *created = nullptr);
    bool removeFeed(qint64 id);
    bool updateFeedDetails(qint64 id, const ParsedFeed &parsed);
    // Records a fetch attempt: validators on success, the reason on failure.
    bool recordFetch(qint64 id, const QString &etag, const QString &lastModified, const QString &error);
    // Adds new items and refreshes existing ones (same guid in the same feed)
    // without touching their read and starred flags. Returns how many are new,
    // or -1 on failure.
    int saveItems(qint64 feedId, const QList<ParsedItem> &items);

    QList<Article> articles(const ArticleFilter &filter);
    bool setRead(qint64 articleId, bool read);
    bool setStarred(qint64 articleId, bool starred);

    // The key two feed URLs share when they are the same subscription.
    static QString urlKey(const QString &url);

private:
    bool migrate();
    bool begin();
    bool commit();
    void rollback();
    bool fail(const QString &message);
    bool setFlag(const char *column, qint64 articleId, bool value);
    qint64 categoryId(const QString &name);

    QString m_connection;
    QSqlDatabase m_db;
    QString m_error;
};
