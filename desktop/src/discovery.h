#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

#include "feedparser.h"
#include "fetcher.h"

// Finds the feed behind a URL the way the web app does
// (src/lib/feed-discovery.ts): the URL itself if it is a feed, then the
// page's <link rel="alternate"> feeds, then the common feed paths on its host.
namespace Discovery {
struct Link {
    QUrl url;
    QString title;
    QString type;
};

struct Result {
    bool found = false;
    QUrl feedUrl;
    // False when feedUrl is the URL that was asked for.
    bool discovered = false;
    ParsedFeed feed;
    // The fetch that produced `feed`, for its validators.
    FetchResult fetch;
    QString error;
};

// Feed links advertised in a page, in document order, resolved against the
// page's URL (or its <base href>), keeping only http(s) ones.
QList<Link> feedLinks(const QByteArray &html, const QUrl &pageUrl);
// The paths sites commonly serve feeds from, on the page's origin.
QList<QUrl> commonFeedUrls(const QUrl &pageUrl);
Result discover(Fetcher &fetcher, const QUrl &url);
}
