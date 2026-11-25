#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QUrl>
#include <functional>
#include <optional>
#include <utility>

#include "discovery.h"
#include "fetcher.h"
#include "nip19.h"
#include "opml.h"
#include "relaypool.h"
#include "store.h"

// What the headless commands and the window both do to the library, written
// once so the two save, refresh, import, and export the same way. None of it
// touches GUI types, so the window can run it on a worker thread with that
// thread's own Store.
namespace Library {
bool isWebUrl(const QUrl &url);

struct Subscription {
    qint64 id = 0;
    bool created = false;
    int added = 0;
    QString error;
    // How many of the new articles a keyword filter rule marked read.
    int markedRead = 0;
};
// Saves the feed a discovery found, with its articles, or finds the existing
// subscription. A new feed that cannot be saved whole is removed again, since
// one left half saved would read as subscribed and never get its articles.
Subscription subscribe(Store &store, const Discovery::Result &found);

// An npub or nprofile (with or without "nostr:"), or null.
std::optional<Nip19::Profile> nostrAuthor(const QString &text);
// True for a feed that follows a Nostr author, whose URL is the npub.
bool isNostrFeed(const Feed &feed);
// The two queries a refresh asks relays for, as the web app asks them: the
// author's profile and long-form articles first, then their videos, each
// with its own limit, so that many short videos cannot crowd the articles
// and the profile out. Both go from a day before the last refresh every
// relay answered both on, and to the author's relay hints too.
QList<NostrQuery> nostrQueries(const Feed &feed);
// The queries a new subscription to `author` asks, with its relay hints.
QList<NostrQuery> nostrQueries(const Nip19::Profile &author);

// The results of each author's two queries, which arrive in either order,
// paired up again: query `index` of a list of nostrQueries() belongs to
// author index / 2.
class NostrPairs {
public:
    // Keeps the result until its pair's other one comes, then returns both,
    // the articles' first.
    std::optional<std::pair<NostrResult, NostrResult>> take(qsizetype index, const NostrResult &result);

private:
    QHash<qsizetype, NostrResult> m_waiting;
};

// Saves a new Nostr author with the profile and articles `articles` holds
// and the videos `videos` holds, as subscribe() does for a web feed, or
// finds the existing subscription. It fails only when the articles' query
// did; a failed videos query leaves the refresh incomplete instead.
Subscription subscribeNostr(Store &store, const Nip19::Profile &author, const NostrResult &articles,
                            const NostrResult &videos);

struct Refresh {
    // "updated", "not-modified", or "error".
    QString status;
    int added = 0;
    // The feed's title after the refresh.
    QString title;
    QString error;
    // How many of the new articles a keyword filter rule marked read.
    int markedRead = 0;
};
// Applies one fetch of `feed`: keeps the validators of a 304, parses and
// saves a new body, or records why it failed on the feed.
Refresh apply(Store &store, const Feed &feed, const FetchResult &fetched);
// The same for a Nostr feed and what the relays returned for its two
// queries. It counts as complete, so the next refresh asks only for what is
// newer, only when both results are.
Refresh applyNostr(Store &store, const Feed &feed, const NostrResult &articles, const NostrResult &videos);

struct Import {
    int added = 0;
    int skipped = 0;
    // Outlines that are neither a web feed nor an npub.
    int unsupported = 0;
    // The gate said to add nothing, so nothing was.
    bool stopped = false;
    QString error;
};
// Reads and parses an OPML file (a regular file of at most Opml::maxBytes)
// without touching the library, so a bad file is refused before one is
// opened. Null with the reason in `error` when it cannot.
std::optional<QList<Opml::Outline>> readOpml(const QString &path, QString *error);
// Adds the feeds the outlines list that are not subscribed yet, all of them
// or, when one cannot be saved, none; folders become categories. `gate`,
// when set, runs first inside the same transaction, so what it reads cannot
// change before the feeds are added and what it writes commits with them:
// it returns false on a store failure, which rolls everything back, and
// sets *proceed to false to add nothing.
Import importOutlines(Store &store, const QList<Opml::Outline> &outlines,
                      const std::function<bool(bool *proceed)> &gate = {});
// readOpml() and then importOutlines().
Import importFile(Store &store, const QString &path);
// The subscriptions as OPML, or null with the reason in `error`.
QByteArray exportOpml(Store &store, QString *error);
// Writes beside `path` and renames over it, so a failure never leaves half a
// file, with mode 0600 before any data goes in: the list is private.
bool writePrivately(const QString &path, const QByteArray &data, QString *error);
}
