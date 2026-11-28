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
#include <limits>
#include <optional>

#include "feedparser.h"
#include "keywordfilter.h"

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
    // Its tags, and its category's color (Sync::safeColor()) and icon
    // (Sync::safeIcon()), as the last subscription list applied gave them,
    // or as they were changed here since.
    QStringList tags = {};
    QString categoryColor = {};
    QString categoryIcon = {};
    // Its category's id, 0 for none.
    qint64 categoryId = 0;
    // The number of the last change here (SyncAccount::subscriptionsEdits)
    // not published yet, or 0 for none (PendingEdits): to its tags, to the
    // category it is in, and to its category's color and icon or that
    // category's being.
    qint64 tagsEdit = 0;
    qint64 placementEdit = 0;
    qint64 styleEdit = 0;
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
    // A keyword filter rule highlights it.
    bool highlighted = false;
};

// An article's read flag and when it was marked read, null when it was not
// or the library kept no time for it.
struct ReadState {
    bool read = false;
    std::optional<qint64> readAt;
};

// What a write did to one article's read state, so that it can be undone,
// or done again, exactly.
struct ReadChange {
    qint64 id = 0;
    ReadState before, after;
};

// A feed as removeFeed() deleted it: its row and its items' rows, read and
// starred flags and read times included, column by column, and the name of
// its category, so that restoreFeed() can put it back under the same ids.
struct FeedSnapshot {
    qint64 id = 0;
    QString category;
    // The category's color and icon, and the pubkey of the sync account
    // then, empty for none, as both came from that account's lists.
    QString categoryColor;
    QString categoryIcon;
    QString account;
    QStringList feedColumns;
    QVariantList feed;
    QStringList itemColumns;
    QList<QVariantList> items;
    // About how much memory it takes: its text and a little for each value.
    qsizetype bytes = 0;
};

// A category's color and icon, empty for none.
struct CategoryStyle {
    QString color;
    QString icon;
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
    // How many times the saved views were edited here (Store::
    // markViewsEdited()), so a publish can tell whether the views it carried
    // are still the views as edited (Sync::recordPublished()). Counted for
    // the library, not the account, so it never repeats when syncing stops
    // and starts again, and an older session's publish never matches.
    qint64 viewsEdits = 0;
    // How many times the categories or tags were changed here
    // (Store::renameCategory() and the like), likewise, for the
    // subscription list, counted whether or not an account was set. Each
    // change marks the feeds and categories it changed with its number
    // (PendingEdits).
    qint64 subscriptionsEdits = 0;
};

// What was changed here and not published yet, each with the number of its
// last change (SyncAccount::subscriptionsEdits): the feeds, by id, whose
// tags were changed, those moved to another category, and the categories,
// by id, whose color and icon, or being, were. Each is marked apart, so a
// pull leaves only what was changed here as it is (Sync::mirrorList()) and
// a publish carries only that as it is here (Sync::subscriptionsContent()),
// while the rest follows the list. A published list clears each mark it
// carried, unless that part was changed again after the list was built
// (Store::clearPendingEdits()). Marks belong to the library, so they stay
// when syncing stops.
struct PendingEdits {
    QHash<qint64, qint64> tags;
    QHash<qint64, qint64> placements;
    QHash<qint64, qint64> styles;
    bool isEmpty() const { return tags.isEmpty() && placements.isEmpty() && styles.isEmpty(); }
};

// The bit of `kind` in SyncAccount::publishPending, 0 for an unknown kind.
int publishBit(int kind);
// Also in SyncAccount::publishPending: the saved views were edited here
// since they last went out. Unlike the views' own bit, which "Publish now"
// sets too, it lets the views be published over a list the relays hold
// (Sync::drafts()), and only a publish built after the last edit clears it
// (SyncAccount::viewsEdits, Sync::recordPublished()).
inline constexpr int viewsEditedBit = 8;
// Also in SyncAccount::publishPending, as Store::syncAccount() reads it:
// some feed or category is marked as changed here and not published yet
// (PendingEdits). It is never stored, but worked out from the marks each
// time, so it holds while any mark does, across restarts and sign-ins, and
// goes once the last one is cleared.
inline constexpr int subscriptionsEditedBit = 16;

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
    // Words to search the title, author, summary, and text for, as the user
    // typed them (Store::matchExpression()). When set, only the articles
    // with every word are listed, best match first; alsoId is listed only
    // if it matches too. A query without a word lists as though none was set.
    QString query = {};
    // The rules a saved view adds (SavedViews::evaluate()). Only read
    // articles, as unreadOnly lists only unread ones.
    bool readOnly = false;
    // Oldest first instead of newest first; a search still lists the best
    // matches first.
    bool oldestFirst = false;
    // An article is listed only when its title, summary, text, or author
    // contains, ignoring case, one of includeWords (when there are any) and
    // its title, text, or author none of excludeWords, as the web app's
    // matchesView() has it (Store::matchesWords()). alsoId is listed only
    // if it does.
    QStringList includeWords = {};
    QStringList excludeWords = {};
    // With words, at most this many articles that the other rules pick are
    // read for them, the first in the list's order, so a view of rare
    // words over a large library still lists at once; 0 reads them all.
    int maxScanned = 0;
    // With words, markRead() marks only among these when there are any:
    // the articles a list that maxScanned stopped read for them
    // (articles()' `scannedIds`), so it marks no further than the list
    // looked, whatever arrived or was read since.
    QList<qint64> scannedIds = {};
    // Only the articles of feeds that carry every one of these tags, as the
    // web app's tag filter has it (Prisma's hasEvery), so a view of tags no
    // feed carries lists nothing. alsoId is listed only within them.
    QStringList tags = {};
    // Any one of `tags` will do rather than every one: a tag in the sources
    // pane stands for every spelling of it that shows the same
    // (SourceModel::Row::tags).
    bool anyTag = false;
    // Only the articles of the feeds in the category of this name, as the
    // library keeps it; empty for any.
    QString category = {};
};

// The categories and tags of some feeds, and the categories of some names
// with their color and icon, as a change to them found them or left them,
// so that it can be undone, or done again, exactly (Store::reorganize()).
struct Organization {
    // A feed's category, empty for none, and its tags.
    struct Placement {
        QString category;
        QStringList tags;
    };
    QHash<qint64, Placement> feeds;
    // Null for a name no category has.
    QHash<QString, std::optional<CategoryStyle>> categories;
};

// What one change to the categories or tags did, under the sync account
// `account` (empty for none), as Store::restoreOrganization() takes it.
struct OrganizeChange {
    QString account;
    Organization before, after;
    // How many feeds it changed, and whether a renamed category joined one
    // of the new name.
    int feeds = 0;
    bool merged = false;
    // Whether it changed anything at all, so there is something to undo.
    bool changed = false;
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
    // With `snapshot`, keeps what it deletes there first, in the same
    // transaction, unless that is more than `maxBytes` or `maxItems`
    // articles: the feed is deleted all the same, and `snapshot` left empty.
    bool removeFeed(qint64 id, FeedSnapshot *snapshot = nullptr,
                    qsizetype maxBytes = std::numeric_limits<qsizetype>::max(),
                    int maxItems = std::numeric_limits<int>::max());
    // Puts a feed removeFeed() deleted back, with its items as they were,
    // as a subscription made now: its created_at is now, so a subscription
    // list published since, marking it deleted, does not keep it off the
    // next one. The filter rules as they are now decide what its items
    // hide and highlight, and an article the last synced reads list has is
    // read, as when a refresh brings it. Its tags, and its category's color
    // and icon when the category has none now and the subscription list
    // stored last (syncRemote()) names that category nowhere, come back
    // only under the sync account they came from; under another, or none,
    // the feed comes back with no tags. Fails when the feed's URL is
    // subscribed again, or `snapshot` holds no feed, setting `lasting`, as
    // no later try changes that.
    bool restoreFeed(const FeedSnapshot &snapshot, bool *lasting = nullptr);
    bool updateFeedDetails(qint64 id, const ParsedFeed &parsed);
    // A Nostr author's profile as the feed's details, when `revision` (its
    // created_at) is newer than the profile they came from.
    bool updateProfile(qint64 id, const ParsedFeed &profile, qint64 revision, const QString &eventId);
    bool setRelayHints(qint64 id, const QStringList &relays);
    // The feed's tags as a subscription list carries them; none clears them.
    bool setFeedTags(qint64 id, const QStringList &tags);
    // Puts the feed in the category `name`, made if need be, as a
    // subscription list files it, marking nothing (PendingEdits).
    bool placeFeed(qint64 id, const QString &name);
    // Gives each category not marked as changed here (PendingEdits) the
    // color and icon `styles` has for its name as FeedParser::cleanText()
    // leaves it, and the others none, writing only those that change, and
    // says in `wrote` whether any did. Neither is checked here: callers pass
    // them through Sync::safeColor() and Sync::safeIcon().
    bool replaceCategoryStyles(const QHash<QString, CategoryStyle> &styles, bool *wrote = nullptr);
    // Every relay answered a refresh that started at `time`.
    bool recordComplete(qint64 id, const QDateTime &time);
    // Whether the Nostr refresh just saved missed something (Feed::incomplete).
    bool setIncomplete(qint64 id, bool incomplete);
    // Records a fetch attempt: validators on success, the reason on failure.
    bool recordFetch(qint64 id, const QString &etag, const QString &lastModified, const QString &error);
    // Adds new items and refreshes existing ones (same guid in the same feed)
    // without touching their read and starred flags. A new one is read when
    // its guid is among the last synced read GUIDs, or when a keyword filter
    // rule marks it read, as though the user had: `markedRead` gets how many
    // a rule marked that were not read already, and the read status is
    // marked to publish for an account that publishes reads. The filter
    // rules hide and highlight new and changed items too. Returns how many
    // are new and not hidden by a rule, or -1 on failure.
    int saveItems(qint64 feedId, const QList<ParsedItem> &items, int *markedRead = nullptr);

    // Leaves out the articles a keyword filter rule hides, as counts(),
    // feeds()' counts, and markRead() do; article() still finds them.
    // `scanCapped` says whether the filter's maxScanned stopped the list
    // before it had its limit and before the articles ran out, and
    // `scannedIds` then gets every article it read for the words.
    QList<Article> articles(const ArticleFilter &filter, bool *scanCapped = nullptr,
                            QList<qint64> *scannedIds = nullptr);
    // Whether `article`, its content included, holds one of the filter's
    // includeWords, when it has any, and none of its excludeWords.
    static bool matchesWords(const ArticleFilter &filter, const Article &article);
    std::optional<Article> article(qint64 id);
    std::optional<LibraryCounts> counts();
    bool setRead(qint64 articleId, bool read);
    // Marks every unread article the filter matches (its limit aside) read,
    // up to `upToId`, so articles that arrived after the view was listed stay
    // unread. Returns how many changed, or -1 on failure; `changes` gets
    // each article it marked, with its read state before and after, unless
    // that is more than `maxChanges`: then it gets none, the articles are
    // marked all the same, and `overflow` is set.
    int markRead(const ArticleFilter &filter, qint64 upToId, QList<ReadChange> *changes = nullptr,
                 qsizetype maxChanges = std::numeric_limits<qsizetype>::max(), bool *overflow = nullptr);
    bool setStarred(qint64 articleId, bool starred);
    // As setRead(), with `change` getting the article's read state before
    // and after, read in the same transaction as the write.
    bool setRead(qint64 articleId, bool read, ReadChange *change);
    // As setStarred(), with `was` getting whether the article was starred,
    // read in the same transaction as the write.
    bool setStarred(qint64 articleId, bool starred, bool *was);
    // Null, with lastError() set, when there is no such article.
    std::optional<ReadState> readState(qint64 articleId);
    // Gives the article its read flag and read time as `state` has them;
    // fails when there is no such article.
    bool setReadState(qint64 articleId, const ReadState &state);
    // Gives each article of `changes` its read state from before the
    // change, with `undo`, or from after it, in one transaction, skipping
    // any that are gone. Returns how many it found, or -1 on failure.
    int restoreReadStates(const QList<ReadChange> &changes, bool undo);

    // Empty, with lastError() set, when it cannot be read.
    SyncAccount syncAccount();
    // Sets the sync account. Another npub than the current one starts
    // afresh, forgetting what was applied for the last. The saved views
    // become `pubkey`'s; when they are another account's
    // (savedViewsOfAnother()), it fails unless `dropOtherViews`, which
    // deletes them, mute words and unpublished edits included. The feeds'
    // tags and the categories' colors and icons become `pubkey`'s too
    // (subscriptions_owner): those of `pubkey` itself are kept, with their
    // marks (PendingEdits), which publish them; those of another account,
    // even one no longer set, are deleted with every mark, and when some
    // mark is pending (subscriptionEditsOfAnother()), only with
    // `dropOtherViews`. Of those whose account is not known, as for changes
    // made before any account was set, only the marked ones are kept, as
    // changes made here for the first account to take; the rest, which
    // could be an earlier account's, are deleted.
    bool setSyncAccount(const QString &npub, const QString &pubkey, const QStringList &relays,
                        bool dropOtherViews = false);
    // Whether the tags and category styles are those of an account other
    // than `pubkey`, even one no longer set, and some feed or category is
    // marked as changed here and not published (PendingEdits); null on
    // failure. Changes made before any account was set are nobody's.
    std::optional<bool> subscriptionEditsOfAnother(const QString &pubkey);
    // Whether there are saved views pulled or edited under an account other
    // than `pubkey`, even one no longer set; null on failure. Views made
    // before any account was set are nobody's.
    std::optional<bool> savedViewsOfAnother(const QString &pubkey);
    // Every feed and category marked as changed here and not published,
    // with the number of its last change; null on failure.
    std::optional<PendingEdits> pendingEdits();
    // Clears the marks of `carried`, the rows a published subscription list
    // carried, each only where it is still at or below the number recorded,
    // so a row changed again after that list was built stays marked.
    bool clearPendingEdits(const PendingEdits &carried);
    // Forgets the account and what was pulled for it; subscriptions, read
    // marks, saved views, tags, and category styles stay, still those of
    // the account (Store::savedViewsOfAnother(),
    // subscriptionEditsOfAnother()), with the marks of the changes made here
    // not published yet (PendingEdits).
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
    // Changes to the categories and tags, each in one transaction that
    // counts the change (SyncAccount::subscriptionsEdits), marks each feed
    // and category it changed with that number (PendingEdits), and marks
    // the subscription list to publish for the sync account set, if any
    // (publishBit()), or for the next one to sign in that takes the marks
    // (setSyncAccount()). With `expectedPubkey`, each fails
    // when the account set is another, saying so, as the change was worked
    // out under that one. `change` gets what each did, for an undo. A name
    // or a tag refused says why in lastError(): Sync::categoryNameProblem(),
    // Sync::tagProblem(), and Sync::parseTagList() check them, and the
    // spaces around them are trimmed.
    //
    // Renames the category `from` to `to`. When a category of that name has
    // feeds already, the feeds of `from` join it, and it keeps its own
    // color and icon when it has either, or takes those of `from`; a plain
    // rename keeps them.
    bool renameCategory(const QString &from, const QString &to, OrganizeChange *change = nullptr,
                        const std::optional<QString> &expectedPubkey = std::nullopt);
    // Deletes the category `name`; its feeds are left without one.
    bool deleteCategory(const QString &name, OrganizeChange *change = nullptr,
                        const std::optional<QString> &expectedPubkey = std::nullopt);
    // Puts the feed in the category `name`, made when there is none of that
    // name yet, or in none for an empty one (not one of spaces alone). Only
    // a name no category has yet is checked and trimmed: one there already
    // is taken as it is kept, even when it could not be typed here.
    bool setFeedCategory(qint64 feedId, const QString &name, OrganizeChange *change = nullptr,
                         const std::optional<QString> &expectedPubkey = std::nullopt);
    // Gives the feed the tags of the comma-separated `list`
    // (Sync::parseTagList()), or none for an empty one. The tags it has
    // already, where the list has them as Sync::formatTagList() writes
    // them, are kept as they are, unchecked.
    bool setFeedTagList(qint64 feedId, const QString &list, OrganizeChange *change = nullptr,
                        const std::optional<QString> &expectedPubkey = std::nullopt);
    // Renames, or deletes, the tag that shows as `name` (Sync::tagName()),
    // in every spelling of it, on every feed that carries it; a feed left
    // with two tags that show the same keeps the first.
    bool renameTag(const QString &name, const QString &to, OrganizeChange *change = nullptr,
                   const std::optional<QString> &expectedPubkey = std::nullopt);
    bool deleteTag(const QString &name, OrganizeChange *change = nullptr,
                   const std::optional<QString> &expectedPubkey = std::nullopt);
    // Puts back what `change` found, with `undo`, or what it left, in one
    // transaction marked to publish as the change was, the rows it changes
    // back marked as a change of their own, skipping the feeds
    // that are gone. Fails, setting `lasting` as no later try changes that,
    // when the sync account is not the one the change was made under, as
    // the tags and the styles are that account's, or when the feeds and
    // categories it touched are no longer as it left them (or found them,
    // to do it again), so it never puts back what a pull or another process
    // changed since.
    bool restoreOrganization(const OrganizeChange &change, bool undo, bool *lasting = nullptr);

    // Marks the saved views, edited here, to publish for the sync account
    // `pubkey` (publishBit() and viewsEditedBit), and counts the edit
    // (SyncAccount::viewsEdits); fails when the account set is another.
    // Meant for the transaction that edits them (SavedViews::save()).
    bool markViewsEdited(const QString &pubkey);

    // The keyword filter rules, in order. They stay in this library and
    // are never published. An entry that is not a rule this version would
    // save, or one past KeywordFilter::maxRules, is left out and described
    // in `dropped`; null on failure.
    std::optional<QList<KeywordFilter::Rule>> filterRules(QStringList *dropped = nullptr);
    // Keeps `rules` in place of the ones kept, then applyFilterRules().
    // Refuses an invalid rule or more than maxRules. `saved` says whether
    // the rules were kept, so a failure can be told from one to apply them.
    bool setFilterRules(const QList<KeywordFilter::Rule> &rules, const std::function<bool()> &superseded = {},
                        bool *saved = nullptr);
    // Works out again which articles the kept rules hide, highlight, or
    // mark read (which only marks articles still to come, or
    // markReadByFilter()), unless the flags are already theirs. Items are
    // read and evaluated with no transaction open and written in short
    // batches, so other connections write meanwhile; an item saved since it
    // was read keeps the flags its save gave it. Stops early, leaving the
    // rest to whoever comes next, when `superseded` returns true (checked
    // between batches) or when the rules were saved again meanwhile. `ran` says
    // whether any flag may have changed.
    bool applyFilterRules(const std::function<bool()> &superseded = {}, bool *ran = nullptr);
    // Marks read the unread articles the rule `ruleId` marks read (a hide
    // wins), as the user would, reading the rules and the text as they are
    // then rather than the flags. Like applyFilterRules(), it reads and
    // works out with no transaction open and writes in short batches, each
    // only while the rules are the ones it started with and to articles
    // still unread and as read; it stops when they are not, or when
    // `stopped` returns true. Returns how many changed, or -1; `marked`
    // says how many did either way, and `changes` which they were, with
    // their read state before and after, batch by committed batch. Past
    // `maxChanges` of those, `changes` is emptied and gets no more, the
    // marking goes on, and `overflow` is set.
    int markReadByFilter(const QString &ruleId, const std::function<bool()> &stopped = {}, int *marked = nullptr,
                         QList<ReadChange> *changes = nullptr,
                         qsizetype maxChanges = std::numeric_limits<qsizetype>::max(), bool *overflow = nullptr);
#ifdef READSTR_TESTS
    // Only in the test build: called by applyFilterRules() between two
    // batches, with what it checks for being superseded, on its thread.
    static inline std::function<void(const std::function<bool()> &stopped)> filterBatchHook;
    // Only in the test build: called by applyFilterRules(),
    // markReadByFilter(), and markRead() for a filter with words once a
    // batch is read and worked out, before it is written, on their thread.
    static inline std::function<void()> filterWriteHook;
    // Only in the test build: called by articles() for each article it
    // reads to look for a filter's words in.
    static inline std::function<void()> wordsReadHook;
#endif

    // Runs `work` in one transaction that commits when it returns true and
    // rolls back when it returns false, keeping its lastError(), or throws,
    // passing the exception on. `work` must
    // not call saveItems(), which runs its own.
    bool transaction(const std::function<bool()> &work);

    // The key two feed URLs share when they are the same subscription.
    static QString urlKey(const QString &url);
    // `query` as an FTS5 MATCH expression that is only ever words: every
    // whitespace-separated term with a letter or digit in it, quoted as a
    // string, so none can be an operator (AND, OR, NOT, NEAR), a column
    // filter, a parenthesis, or a ^ or *, and the last matched as a prefix,
    // as it may still be being typed, unless its last word is a single letter
    // or digit (of a script that separates words with spaces).
    // An unpaired surrogate becomes U+FFFD. Empty when no term is left. Input
    // past maxQueryLength characters or maxQueryTerms terms is ignored.
    static QString matchExpression(const QString &query);
    static constexpr int maxQueryLength = 256;
    static constexpr int maxQueryTerms = 16;

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
    // The categories and tags of `feedIds` that are still there, and the
    // categories of `names`, as Organization keeps them.
    std::optional<Organization> organization(const QList<qint64> &feedIds, const QStringList &names);
    // Gives the feeds and categories what `organization` holds, making and
    // deleting categories as it says.
    bool applyOrganization(const Organization &organization);
    // In one transaction: lets `plan` work out what the feeds and
    // categories it names are to become, and refuse (false, with why in
    // lastError()), then writes that with the mark to publish it and the
    // count of the change (renameCategory()), filling in `change`. Nothing
    // is marked when nothing changes.
    bool reorganize(const std::function<bool(Organization &target, OrganizeChange &change)> &plan,
                    OrganizeChange *change, const std::optional<QString> &expectedPubkey);
    // What renameTag() and deleteTag() share once `to` is checked: gives
    // every feed with the tag that shows as `name` the tag `to` in its place,
    // or takes it off for an empty `to`, failing when no feed carries it.
    bool retagFeeds(const QString &name, const QString &to, OrganizeChange *change,
                    const std::optional<QString> &expectedPubkey);
    // Counts a change to the categories or tags, marks the feeds and
    // categories of `after` that differ from `before` with its number
    // (PendingEdits), and marks the subscription list to publish for
    // `pubkey`, or, for an empty one, for the account that signs in next and
    // takes the marks.
    bool markSubscriptionsEdited(const QString &pubkey, const Organization &before, const Organization &after);
    bool clearSyncState();
    // Puts the item `id` in the search index, or puts it there again in
    // place of what it had, with `insert` prepared by prepareIndex().
    bool indexItem(QSqlQuery &insert, qint64 id, const QString &title, const QString &author, const QString &body);
    bool prepareIndex(QSqlQuery &insert);
    bool backfillSearchIndex();
    // The rules as saved, compiled once for as long as they stay the same.
    const KeywordFilter::Rules *compiledFilterRules();
    // The filter rules as kept, one JSON text each, in order.
    QStringList filterSource(bool *ok);
    // articles() and markRead() for a filter with words, `match` its
    // search, and `where` and `values` what markRead() has worked out.
    QList<Article> articlesByWords(const ArticleFilter &filter, const QString &match, bool *scanCapped,
                                   QList<qint64> *scannedIds);
    int markReadByWords(const ArticleFilter &filter, qint64 upToId, const QString &where, const QVariantList &values,
                        qint64 now, QList<ReadChange> *changes, qsizetype maxChanges, bool *overflow);

    QString m_connection;
    QSqlDatabase m_db;
    QString m_error;
    QStringList m_filterSource;
    KeywordFilter::Rules m_filterRules;
};
