#pragma once

#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

#include "relaypool.h"
#include "store.h"

// Read-only Nostr sync with the Readstr web app (src/lib/nostr-sync.ts):
// the replaceable events it publishes in cleartext for an npub, pulled and
// merged into the library the way the web app merges them. Nothing here
// signs or publishes. Every event is untrusted: RelayPool has checked its
// id, signature, author, kind, d tag, and date, and its content must also
// fit the web app's schema and caps, or that kind is refused whole, as the
// web app refuses it.
namespace Sync {
inline constexpr int subscriptionsKind = 30404;
inline constexpr int readStatusKind = 30405;
inline constexpr int viewsKind = 30406;
inline constexpr char subscriptionsTag[] = "readstr-subscriptions";
inline constexpr char readStatusTag[] = "readstr-read-status";
inline constexpr char viewsTag[] = "readstr-views";

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
// applies, but counts as a problem and is not recorded as the last sync.
// beforeWrites runs after the first freshness check and before any write;
// tests use it to commit a competing write in between.
Outcome apply(Store &store, const SyncAccount &account, const NostrResult &result,
              const std::function<void()> &beforeWrites = {});
}
