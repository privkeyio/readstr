#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVariant>
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
    // A Nostr author's last refresh that saved missed something: a relay
    // did not answer, or a cap cut the answers off.
    bool incomplete = false;
    // When it was subscribed to on this machine.
    QDateTime createdAt = {};
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
    // The feed's URL, an npub for a Nostr author, so its title can be shown
    // as Nip19::shortNpub shows it.
    QString feedUrl;
};

// The npub the library syncs with. Empty when none is set.
struct SyncAccount {
    QString npub;
    // 64 lowercase hex characters.
    QString pubkey;
    // The nprofile's relay hints, asked besides the configured relays.
    QStringList relays;
    // When a pull last reached a relay and was merged, invalid until one has.
    QDateTime syncedAt;
    // The created_at and the id of the last event applied, by d tag.
    QHash<QString, qint64> applied;
    QHash<QString, QString> appliedIds;
    // Whether this machine publishes its read articles too.
    bool publishReads = false;
    // Whether "Publish now" was pressed once for this account, which
    // automatic publishing waits for, so a library is never published under
    // an npub the user has not confirmed.
    bool publishConfirmed = false;
    // The kinds changed here and not published yet (publishBit()), kept
    // across restarts.
    int publishPending = 0;
};

// The bit of `kind` in SyncAccount::publishPending, 0 for an unknown kind.
int publishBit(int kind);

// The content of the newest event of a d tag the relays are known to hold,
// its id, and when it was stored here (seconds since the epoch).
struct SyncRemote {
    QString eventId;
    QString content;
    qint64 storedAt = 0;
};

// A saved view as kind 30406 carries it, without keywords.exclude, and the
// mute words that stay on this machine.
struct SavedView {
    QString id;
    QJsonObject view;
    QStringList exclude;
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
    // Whether the Nostr refresh just saved missed something (Feed::incomplete).
    bool setIncomplete(qint64 id, bool incomplete);
    // Records a fetch attempt: validators on success, the reason on failure.
    bool recordFetch(qint64 id, const QString &etag, const QString &lastModified, const QString &error);
    // Adds new items and refreshes existing ones (same guid in the same feed)
    // without touching their read and starred flags. A new one is read when
    // its guid is among the last synced read GUIDs. Returns how many are
    // new, or -1 on failure.
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

    // Empty, with lastError() set, when it cannot be read.
    SyncAccount syncAccount();
    // Sets the sync account. Another npub than the current one starts
    // afresh, forgetting what was applied for the last.
    bool setSyncAccount(const QString &npub, const QString &pubkey, const QStringList &relays);
    // Forgets the account and what was pulled for it; subscriptions, read
    // marks, and saved views stay.
    bool clearSyncAccount();
    // Moves the d tag's watermark up to the event `createdAt` and
    // `eventId`, never down, in NostrEvent::isNewerThan's order.
    bool setSyncApplied(const QString &dTag, qint64 createdAt, const QString &eventId);
    bool recordSynced(const QDateTime &time);
    // Remembers `guids` in place of the last list and marks the unread
    // articles with any of them read, as saveItems() does for articles that
    // come later. Returns how many changed, or -1 on failure.
    int applyReadGuids(const QStringList &guids);
    // The remembered read GUIDs no article has yet, or -1 on failure.
    int pendingReadCount();
    // Remembers `guids` in place of the last list without marking anything
    // read, as for a list published here, which holds only what is read.
    bool rememberReadGuids(const QStringList &guids);
    // The remembered read GUIDs, in no particular order.
    std::optional<QStringList> rememberedReadGuids();
    // Hands `take` the GUID of each read article, the one marked read here
    // last first, then those read some other way, newest first, until it
    // returns false.
    bool eachReadGuid(const std::function<bool(const QString &)> &take);
    // Each changes only the account `pubkey`, failing when it is not the
    // one set.
    bool setPublishReads(const QString &pubkey, bool publish);
    bool setPublishConfirmed(const QString &pubkey);
    // Adds the bits of `mask` to SyncAccount::publishPending, or clears them.
    bool markPublishPending(const QString &pubkey, int mask, bool pending);
    // Records the event of the d tag the relays are known to hold. storedAt
    // is when the content was current; by default, now.
    bool setSyncRemote(const QString &dTag, const QString &eventId, const QString &content, qint64 storedAt = -1);
    // The event of the d tag the relays are known to hold; null when there
    // is none or on failure (lastError() tells which).
    std::optional<SyncRemote> syncRemote(const QString &dTag);
    QList<SavedView> savedViews();
    // Meant for inside transaction(), like applyReadGuids(), so a failure
    // part way rolls the delete back with the rest.
    bool replaceSavedViews(const QList<SavedView> &views);

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
    // Runs "UPDATE sync_account SET `assignments`" with `values` bound, for
    // the account `pubkey` only, failing when it is not the one set.
    bool updateSyncAccount(const QString &pubkey, const QString &assignments, const QVariantList &values);
    qint64 categoryId(const QString &name);
    bool clearSyncState();

    QString m_connection;
    QSqlDatabase m_db;
    QString m_error;
};
