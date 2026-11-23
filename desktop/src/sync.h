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
// Each kind applies only when its event is newer than the last one applied
// (the subscription list also when there are no feeds yet, as on the web),
// and never removes a subscription or a read mark: subscriptions are only
// added, articles only marked read, and saved views added or updated,
// keeping their mute words (keywords.exclude), which never come from a
// relay. Like the web app's mergeViewLists, the merged views are cut to
// maxViews with the remote ones first, so a full remote list drops the
// views only here, and their mute words with them. A truncated result still
// applies, but counts as a problem and is not recorded as the last sync, and
// neither is one with a payload refused, though the kinds that fit apply.
// beforeWrites runs after the first freshness check and before any write;
// tests use it to commit a competing write in between.
// The content of each kind's newest event the relays sent that is the last
// one applied (just now or before) is kept, for publishing to build on.
Outcome apply(Store &store, const SyncAccount &account, const NostrResult &result,
              const std::function<void()> &beforeWrites = {});

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
// category's color and icon, which the desktop does not keep, the feeds it
// lists that this machine cannot follow (on the local network, or not a
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
    // views when any list of them is known (the desktop cannot edit views,
    // so it publishes them only when no relay holds a list any more).
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
};
// Unsigned events of `kinds` from the library, each dated past the newest
// version seen of it: its watermark, or what `seen` (Outcome::newest) says
// the last pull found. Fails when the account is no longer `pubkey`.
Drafts drafts(Store &store, const QString &pubkey, const QList<int> &kinds, const QHash<int, qint64> &seen,
              qint64 now);
// Records `event`, published, as the newest of its kind: its watermark,
// its content to build the next one on, and for a read status its GUIDs as
// the last list (marking nothing read). Empty on success, else why not.
// `superseded` says whether a newer version applied meanwhile won instead,
// so what this one carried may be missing from what the relays hold.
QString recordPublished(Store &store, const QString &pubkey, const NostrEvent &event, qint64 builtAt,
                        bool *superseded = nullptr);
}
