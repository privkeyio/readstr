#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include "discovery.h"
#include "fetcher.h"
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
};
// Saves the feed a discovery found, with its articles, or finds the existing
// subscription. A new feed that cannot be saved whole is removed again, since
// one left half saved would read as subscribed and never get its articles.
Subscription subscribe(Store &store, const Discovery::Result &found);

struct Refresh {
    // "updated", "not-modified", or "error".
    QString status;
    int added = 0;
    // The feed's title after the refresh.
    QString title;
    QString error;
};
// Applies one fetch of `feed`: keeps the validators of a 304, parses and
// saves a new body, or records why it failed on the feed.
Refresh apply(Store &store, const Feed &feed, const FetchResult &fetched);

struct Import {
    int added = 0;
    int skipped = 0;
    // Nostr and non-http(s) outlines, which phase 1 cannot follow.
    int unsupported = 0;
    QString error;
};
// Reads an OPML file (a regular file of at most Opml::maxBytes) and adds the
// feeds it lists that are not subscribed yet; folders become categories.
Import importFile(Store &store, const QString &path);
// The subscriptions as OPML, or null with the reason in `error`.
QByteArray exportOpml(Store &store, QString *error);
// Writes beside `path` and renames over it, so a failure never leaves half a
// file, with mode 0600 before any data goes in: the list is private.
bool writePrivately(const QString &path, const QByteArray &data, QString *error);
}
