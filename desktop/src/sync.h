#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

#include "relaypool.h"
#include "store.h"

// Nostr sync with the Readstr web app (src/lib/nostr-sync.ts): the
// replaceable events it publishes in cleartext for an npub, pulled and
// merged into the library the way the web app merges them, and the same
// events built from the library for the window to sign and publish. Every
// event pulled is untrusted: RelayPool has checked its id, signature,
// author, kind, d tag, and date, and its content must also fit the web
// app's schema and caps, or that kind is refused whole, as the web app
// refuses it.
namespace Sync {
inline constexpr int subscriptionsKind = 30404;
inline constexpr int readStatusKind = 30405;
inline constexpr int viewsKind = 30406;
inline constexpr char subscriptionsTag[] = "readstr-subscriptions";
inline constexpr char readStatusTag[] = "readstr-read-status";
inline constexpr char viewsTag[] = "readstr-views";
// The client tag on every event published here; the web app's is "readstr".
inline constexpr char clientTag[] = "readstr-omarchy";

// The web app's caps. Content past maxContentLength is passed over while
// the newest version is picked, so it never hides an older one that fits.
inline constexpr qsizetype maxContentLength = 8 * 1024 * 1024;
inline constexpr int maxFeeds = 5000;
inline constexpr int maxReadGuids = 100000;
inline constexpr int maxUrlLength = 2048;
inline constexpr int maxGuidLength = 256;
inline constexpr int maxTagsPerFeed = 100;
inline constexpr int maxRemoteViews = 100;
// How many saved views are kept, as the web app's normalizeViews keeps them.
inline constexpr int maxViews = 50;
// The web app's MAX_PUBLISHED_READ_CONTENT (src/lib/read-sync.ts): common
// relays refuse events over 64 KiB, so a read status published holds only
// the newest reads that fit, with room kept for the rest of the event.
inline constexpr qsizetype maxPublishedReadContent = 60000;
inline constexpr qsizetype readContentReserve = 1024;

// A category icon longer than this, in UTF-16 units, is not shown.
inline constexpr int maxIconLength = 16;

// A category's color from a subscription list as the sidebar may draw it:
// "#rrggbb", from "#rgb" or "#rrggbb" in either case, and empty for anything
// else, so no color name, rgba(), or markup reaches the window.
QString safeColor(const QString &color);
// A category's icon from a subscription list as text to show: cleaned as a
// feed's title is, and empty when that leaves more than maxIconLength
// units, rather than cut, which could split an emoji.
QString safeIcon(const QString &icon);
// A tag longer than this, in UTF-16 units, does not fit the web app's schema.
inline constexpr int maxTagLength = 256;
// Whether a feed here can carry `tag`, as mirroring a subscription list
// keeps them: not blank, no longer than maxTagLength, and with no C0
// control character or half of a surrogate pair. Older SQLite can cut a
// value at an escaped U+0000 when it reads the JSON a feed's tags are kept
// in, so "news\u0000local" would match "news", and half of a surrogate pair
// is bound to SQLite as broken UTF-8. A tag that fails this is never stored,
// so a view that names one lists nothing, and SQLite is never given it.
bool storableTag(const QString &tag);
// Whether `text` holds half of a surrogate pair on its own, as a relay's
// JSON can ("\ud800"). Qt binds such a string to SQLite as broken UTF-8, so
// once stored it would never read back as itself, nor match as a tag.
bool hasLoneSurrogate(const QString &text);
// `name` as a message shows it: cleaned (FeedParser::cleanText()), and each
// half of a surrogate pair on its own as U+FFFD, so the message is valid
// text wherever it goes.
QString shownName(const QString &name);
// A tag as the sources pane names it, and groups the spellings that show
// the same under: cleaned like a category's name, and without the
// invisible format characters (zero-width spaces, byte order marks, soft
// hyphens) that would make two different tags look the same, save the
// zero-width joiner and the tag characters that emoji are built from.
QString tagName(const QString &tag);

// The longest category name a subscription list carries (the web app's
// schema).
inline constexpr int maxCategoryNameLength = 256;
// Why `name`, with the spaces around it trimmed, cannot be a category's name
// made here, or empty when it can: it must not be blank, nor longer than
// maxCategoryNameLength, and must read back from a subscription list as it
// is, which FeedParser::cleanText() would not leave it if it held a control
// character, a bidirectional override, or a run of spaces.
QString categoryNameProblem(const QString &name);
// Why `tag`, with the spaces around it trimmed, cannot be a tag given here,
// or empty when it can: storableTag() must take it, and it must show as it
// is in the sources pane (tagName()), with no control character, direction
// mark, invisible format character, or run of spaces, as a category's name
// must read back as it is (categoryNameProblem()). Tags pulled from the web
// app are held only to storableTag().
QString tagProblem(const QString &tag);
// `tags` as a comma-separated list that parseTagList() reads back as they
// are: a tag that holds a comma, starts with a quote, or has spaces around
// it is put between quotes, each quote in it doubled.
QString formatTagList(const QStringList &tags);
// The tags of a comma-separated list, as formatTagList() writes it, as a
// feed is to carry them. Of `current`, the tags the feed carries now, each
// one the list has as formatTagList() wrote it is kept as it is, unchecked;
// every other one is trimmed and checked (tagProblem()), blanks are left
// out, and of those that show the same (tagName()) as one kept or one
// before, the first. Null, with why in `problem`, when a quote is left
// open, a tag is refused, or they are more than maxTagsPerFeed.
std::optional<QStringList> parseTagList(const QString &text, QString *problem, const QStringList &current = {});
// Why a change to the categories or tags (Store::renameCategory() and the
// like) was refused: the sync account it was worked out under is no longer
// the one set, the feeds or categories it would undo or do again changed
// since, or there is no such category or tag.
QString accountChanged();
QString organizationChanged();
QString noSuchCategory(const QString &name);
QString noSuchTag(const QString &name);

// Whether the subscription list `content` names the category `name` for
// any feed, as mirrorList() finds a category's entry: by the name as
// FeedParser::cleanText() leaves it, whether or not it gives a style.
bool namesCategory(const QString &content, const QString &name);

// The account for an npub or nprofile (with or without "nostr:"), with the
// nprofile's relay hints, or null.
std::optional<SyncAccount> account(const QString &text);
// One request for the account's three events.
NostrQuery query(const SyncAccount &account);
// RelayPool's limits, raised so the three events at their caps fit.
RelayPool::Limits limits();

struct Outcome {
    // Subscriptions added, already there, and dropped for being neither an
    // http(s) feed nor a valid npub, or for being on this machine or the
    // local network.
    int added = 0;
    int skipped = 0;
    int unsupported = 0;
    // Articles marked read now, and read GUIDs kept for articles not here yet.
    int markedRead = 0;
    int pendingRead = 0;
    // Saved views in the library after the merge.
    int views = 0;
    // The kinds refused, and why; the others still applied.
    QStringList problems;
    QList<int> refused;
    // The created_at of the newest version of each kind the relays sent,
    // whether applied, already applied, or past the size cap.
    QHash<int, qint64> newest;
    // Every relay that connected answered and nothing was cut off
    // (NostrResult::settled()).
    bool settled = false;
    // Set when nothing could be pulled or saved.
    QString error;
};
// Merges what the relays returned for query(account) into the library.
// The subscription list applied, just now or before, also gives each feed
// here its tags and each category its color and icon, as a publish from
// here carries them on (subscriptionsContent()); one published here gives
// them when the relays send it back.
// Each kind applies only when its event is newer than the last one applied
// (the subscription list also when there are no feeds yet, as on the web),
// and never removes a subscription or a read mark: subscriptions are only
// added, articles only marked read, and saved views added or updated,
// keeping their mute words (keywords.exclude), which never come from a
// relay, and removed only when the list the relays held before had them as
// they are here (mergeViews()). Like the web app's mergeViewLists, the
// merged views are cut to maxViews with the remote ones first, so a full
// remote list drops the views only here, and their mute words with them.
// A truncated result still
// applies, but counts as a problem and is not recorded as the last sync, and
// neither is one with a payload refused, though the kinds that fit apply.
// beforeWrites runs after the first freshness check and before any write;
// tests use it to commit a competing write in between.
// The content of each kind's newest event the relays sent that is the last
// one applied (just now or before) is kept, for publishing to build on.
Outcome apply(Store &store, const SyncAccount &account, const NostrResult &result,
              const std::function<void()> &beforeWrites = {});
// Why `kind` must not be published after the pull of `outcome`, or empty:
// one that not every relay answered in full may have missed a newer read
// status or saved views from another device, which publishing would
// replace, as on the web.
QString unsettledHold(const Outcome &outcome, int kind);

// Kind 30406's content as the web app's viewListSchema and normalizeViews
// take it: null when it does not fit, else at most maxViews views, each
// through the web app's sanitizeView (less keywords.exclude), numbered
// again from 0 in their order.
std::optional<QList<SavedView>> parseViews(const QString &content);
// The web app's mergeViewLists: the remote views first, keeping each one's
// local mute words, then the local ones the remote lacks, since deletions
// do not sync, cut to maxViews. A local one `previous`, the list the relays
// held before `remote`, holds as it is here, less its place, was deleted in
// the web app and is left out; one changed or made here stays.
QList<SavedView> mergeViews(const QList<SavedView> &local, const QList<SavedView> &remote,
                            const QList<SavedView> &previous = {});

// The d tag of one of the three kinds, or null.
const char *dTag(int kind);
// An unsigned event of `kind`, with its d tag and the client tag.
NostrEvent draft(int kind, const QString &content, qint64 createdAt);
// A created_at that replaces the version dated `lastSeen`: now, or a second
// past it when the clock is behind it, since of two versions in the same
// second a relay may keep either.
qint64 nextCreatedAt(qint64 now, qint64 lastSeen);

// The web app's ReadSyncGuidCollector: keys in the order given (newest read
// first), each once, none empty or past maxGuidLength, until the next one
// would take the published event past maxPublishedReadContent, counted as
// the UTF-8 of the key escaped once in the content and again in the event.
// It stops at the first that does not fit, so the result is a prefix.
class ReadGuidCollector {
public:
    void add(const QString &key);
    bool full() const { return m_done || m_guids.size() >= maxReadGuids; }
    QStringList guids() const { return m_guids; }

private:
    QStringList m_guids;
    QSet<QString> m_seen;
    qsizetype m_size = readContentReserve;
    bool m_done = false;
};
// buildPublishedReadList: the local keys first, then the remote ones the
// local list lacks, under the cap.
QStringList publishedReadGuids(const QStringList &local, const QStringList &remote);

// Kind 30404's content: this library's feeds, and from `remote`, the last
// list the relays held (which may be empty), each listed feed's tags and its
// category's color and icon, which come from that list as it was rather than
// from the library, so what the library leaves out (a tag storableTag()
// refuses, a color safeColor() does not show) is not lost (save for a
// local feed that list neither lists nor has tags for, as when an unsubscribe
// was undone after a list without it went out, which keeps the ones the
// library has from an earlier list), the feeds it lists that this machine
// cannot follow (on the local network, or not a
// web feed or npub), so they are not dropped, and its `deleted`. A feed it
// lists that this machine could follow but does not was unsubscribed here,
// since the pull before a publish adds every one, so it goes in `deleted`.
// A local feed in its `deleted` stays off the list, unless subscribed here
// after `remoteStoredAt`, when `remote` was stored. A web feed whose URL has
// a user name or password, or whose host is on the local network, is never
// listed, nor marked deleted. What `remote` carries is cut, `deleted` first,
// so the event fits what common relays take (maxPublishedReadContent); null
// when this library's own feeds do not fit. What `remote` carries with a user
// name or password is dropped too. A local feed whose URL is past
// maxUrlLength is left out, and counted in `tooLong`.
// A feed whose tags were changed here, and a category whose color and icon
// or being were, not published yet (Feed::tagsEdit and styleEdit,
// PendingEdits), carries the library's instead, which are the ones changed
// here. A feed's category is the library's, which a pull leaves the list's
// unless it was changed here (mirrorList()); a name longer than
// maxCategoryNameLength is cut, never through a surrogate pair.
std::optional<QString> subscriptionsContent(const QList<Feed> &feeds, const QString &remote, qint64 remoteStoredAt,
                                            qint64 now, int *tooLong = nullptr);
QString readStatusContent(const QStringList &guids, qint64 now);
// Kind 30406's content: the views without their mute words, which never
// leave this machine.
QString viewsContent(const QList<SavedView> &views, qint64 now);

struct Drafts {
    QList<NostrEvent> events;
    // Kinds left out with nothing to publish: a read status with no read the
    // last one lacks, or that the account no longer publishes, or saved
    // views not edited here (viewsEditedBit) when any list of them is known:
    // publishing those over it would only bring back views deleted in the
    // web app, since deletions do not sync, so they go out unedited only
    // when no relay holds a list any more.
    QList<int> unchanged;
    // Kinds that cannot be published now, and why.
    QList<int> failed;
    QStringList errors;
    // Kinds waiting for a pull to bring back what the relays hold, and why:
    // tried again after each pull, but not an error worth a status line
    // every refresh.
    QList<int> waiting;
    QStringList notes;
    QString error;
    // SyncAccount::viewsEdits when the saved views were drafted, for
    // recordPublished(); -1 when they were not.
    qint64 viewsEdits = -1;
    // Every mark (PendingEdits) when the subscription list was drafted, with
    // the number of each part's last change, for recordPublished(): all of
    // them count as carried, as the list holds what was changed here of
    // each row it lists, and a row it leaves out no list from here carries.
    PendingEdits subscriptionEdits;
};
// Unsigned events of `kinds` from the library, each dated past the newest
// version seen of it: its watermark, or what `seen` (Outcome::newest) says
// the last pull found. Fails when the account is no longer `pubkey`. All
// of it is read in one transaction, so it must not be called inside one.
Drafts drafts(Store &store, const QString &pubkey, const QList<int> &kinds, const QHash<int, qint64> &seen,
              qint64 now);
// Records `event`, published, as the newest of its kind: its watermark,
// its content to build the next one on, for a read status its GUIDs as the
// last list (marking nothing read), and for saved views that they went out
// as edited (viewsEditedBit) when `viewsEdits`, Drafts::viewsEdits, says
// no edit came after the draft, and for a subscription list that the rows
// of `carried`, Drafts::subscriptionEdits, went out as changed here, which
// clears each mark not changed again since (Store::clearPendingEdits()).
// Empty on success, else why not.
// `superseded` says whether a newer version applied meanwhile won instead,
// so what this one carried may be missing from what the relays hold, and
// `mirrored` whether a subscription list changed the feeds' tags or a
// category's color or icon here, which the window then shows.
QString recordPublished(Store &store, const QString &pubkey, const NostrEvent &event, qint64 builtAt,
                        bool *superseded = nullptr, bool *mirrored = nullptr, qint64 viewsEdits = -1,
                        const PendingEdits &carried = {});
#ifdef READSTR_TESTS
// Only in the test build: called by apply() once it has found the list it
// mirrors still the one applied, and by recordPublished() once it has
// recorded a subscription list, before the transaction that checks so again
// and mirrors it, on its thread.
inline std::function<void()> mirrorHook;
// Only in the test build: called by drafts() once it has read the account,
// before it reads the library, on its thread.
inline std::function<void()> draftsHook;
// Only in the test build: called by apply() once it has mirrored what it
// applied, before the transaction that caches it for publishing, on its
// thread.
inline std::function<void()> cacheHook;
#endif
}
