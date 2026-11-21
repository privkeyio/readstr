#pragma once

#include <QDateTime>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <functional>
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
    // A Nostr author's: relays their nprofile named, and when every relay
    // asked last answered a refresh (invalid until one has).
    QStringList relayHints;
    QDateTime completeAt;
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
    // A Nostr long-form article's address; its content is Markdown.
    QString address;
    // A Nostr video's, as ParsedItem has them.
    QString mediaUrl;
    QString thumbnailUrl;
    double duration = -1;
};

struct ArticleFilter {
    qint64 feedId = 0;
    bool unreadOnly = false;
    bool starredOnly = false;
    int limit = 50;
    // Listed even when unreadOnly or starredOnly or the limit leaves it out
    // (still within feedId), so the article open in the window keeps its
    // place.
    qint64 alsoId = 0;
    // Lists in the window leave the content out; the reader asks article().
    bool withContent = true;
};

struct LibraryCounts {
    int total = 0;
    int unread = 0;
    int starred = 0;
    // The newest article's id, which markRead() takes as its bound.
    qint64 lastId = 0;
};

// The local library in SQLite: feeds, their items with read and starred flags,
// and categories. Synchronous on purpose: the headless commands use one
// directly, and the window keeps one on its own thread for reads and another
// on a worker thread for every write
// (each connection belongs to the thread that opened it), so it must stay
// free of GUI types. Every method that can fail returns false, 0, or nullopt
// and leaves the reason in lastError().
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
    // A Nostr author's profile as the feed's details, when `revision` (its
    // created_at) is newer than the profile they came from.
    bool updateProfile(qint64 id, const ParsedFeed &profile, qint64 revision, const QString &eventId);
    bool setRelayHints(qint64 id, const QStringList &relays);
    // Every relay answered a refresh that started at `time`.
    bool recordComplete(qint64 id, const QDateTime &time);
    // Records a fetch attempt: validators on success, the reason on failure.
    bool recordFetch(qint64 id, const QString &etag, const QString &lastModified, const QString &error);
    // Adds new items and refreshes existing ones (same guid in the same feed)
    // without touching their read and starred flags. Returns how many are new,
    // or -1 on failure.
    int saveItems(qint64 feedId, const QList<ParsedItem> &items);

    QList<Article> articles(const ArticleFilter &filter);
    std::optional<Article> article(qint64 id);
    std::optional<LibraryCounts> counts();
    bool setRead(qint64 articleId, bool read);
    // Marks every unread article the filter matches (its limit aside) read,
    // up to `upToId`, so articles that arrived after the view was listed stay
    // unread. Returns how many changed, or -1 on failure.
    int markRead(const ArticleFilter &filter, qint64 upToId);
    bool setStarred(qint64 articleId, bool starred);

    // Runs `work` in one transaction that commits when it returns true and
    // rolls back when it returns false, keeping its lastError(), or throws,
    // passing the exception on. `work` must
    // not call saveItems(), which runs its own.
    bool transaction(const std::function<bool()> &work);

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
