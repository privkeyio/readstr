#include "discovery.h"

#include <QRegularExpression>
#include <QSet>

namespace {
// Only the start of a page is searched: feed links live in <head>.
constexpr qsizetype scanBytes = 1024 * 1024;
// Pages advertising more feeds than this get only their first few checked.
constexpr int maxCandidates = 5;

const QStringList feedTypes{"application/rss+xml", "application/atom+xml", "application/feed+json"};

QHash<QString, QString> attributesOf(const QString &tag) {
    static const QRegularExpression attribute(
        QStringLiteral("([^\\s=/>\"']+)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>\"']+))"));
    QHash<QString, QString> attributes;
    auto matches = attribute.globalMatch(tag);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const QString name = match.captured(1).toLower();
        if (attributes.contains(name))
            continue;
        QString value = match.captured(2);
        if (match.capturedStart(3) >= 0)
            value = match.captured(3);
        else if (match.capturedStart(4) >= 0)
            value = match.captured(4);
        // Decodes &amp; and friends; URLs and titles never need markup.
        attributes.insert(name, FeedParser::plainText(value));
    }
    return attributes;
}

QUrl resolveWeb(const QUrl &base, const QString &href) {
    const QUrl url = base.resolved(QUrl(href.trimmed()));
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == "http" || scheme == "https") && !url.host().isEmpty() ? url : QUrl();
}

// The first of `results` that parses as a feed, in order.
bool firstFeed(const QList<FetchResult> &results, Discovery::Result &found) {
    for (const FetchResult &fetch : results) {
        if (!fetch.ok() || fetch.notModified)
            continue;
        if (auto feed = FeedParser::parse(fetch.body, fetch.finalUrl)) {
            found.found = true;
            found.feedUrl = fetch.url;
            found.feed = *feed;
            found.fetch = fetch;
            return true;
        }
    }
    return false;
}

QList<FetchRequest> requestsFor(const QList<QUrl> &urls) {
    QList<FetchRequest> requests;
    for (const QUrl &url : urls)
        requests << FetchRequest{url, {}, {}};
    return requests;
}
}

namespace Discovery {
QList<Link> feedLinks(const QByteArray &html, const QUrl &pageUrl) {
    static const QRegularExpression comment(QStringLiteral("<!--.*?-->"),
                                            QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression tag(QStringLiteral("<(link|base)\\b([^>]*)>"),
                                        QRegularExpression::CaseInsensitiveOption);
    QString text = QString::fromUtf8(html.left(scanBytes));
    text.remove(comment);

    QUrl base = pageUrl;
    bool sawBase = false;
    QList<Link> links;
    QSet<QUrl> seen;
    auto matches = tag.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto attributes = attributesOf(match.captured(2));
        if (match.captured(1).compare("base", Qt::CaseInsensitive) == 0) {
            const QUrl href = resolveWeb(pageUrl, attributes.value("href"));
            if (!sawBase && href.isValid()) {
                base = href;
                sawBase = true;
            }
            continue;
        }
        const QStringList rel = attributes.value("rel").toLower().split(' ', Qt::SkipEmptyParts);
        const QString type = attributes.value("type").section(';', 0, 0).trimmed().toLower();
        if (!rel.contains("alternate") || !feedTypes.contains(type))
            continue;
        const QUrl url = resolveWeb(base, attributes.value("href"));
        if (url.isValid() && !seen.contains(url)) {
            seen.insert(url);
            links << Link{url, attributes.value("title"), type};
        }
    }
    return links;
}

QList<QUrl> commonFeedUrls(const QUrl &pageUrl) {
    // Same list and order as tryCommonFeedLocations in feed-discovery.ts.
    static const QStringList paths{"/feed",     "/feed/",     "/rss",     "/rss.xml",    "/atom.xml",
                                   "/feed.xml", "/index.xml", "/blog/feed", "/blog/rss", "/?feed=rss2",
                                   "/feeds/posts/default"};
    const QString origin = pageUrl.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment
                                            | QUrl::RemoveUserInfo)
                               .toString(QUrl::FullyEncoded);
    QList<QUrl> urls;
    for (const QString &path : paths)
        urls << QUrl(origin + path);
    return urls;
}

Result discover(Fetcher &fetcher, const QUrl &url) {
    Result result;
    const FetchResult page = fetcher.get({url, {}, {}});
    if (!page.ok()) {
        result.error = QStringLiteral("cannot fetch %1: %2").arg(url.toString(), page.error);
        return result;
    }
    if (auto feed = FeedParser::parse(page.body, page.finalUrl)) {
        result.found = true;
        result.feedUrl = url;
        result.feed = *feed;
        result.fetch = page;
        return result;
    }

    QList<QUrl> candidates;
    for (const Link &link : feedLinks(page.body, page.finalUrl))
        if (candidates.size() < maxCandidates)
            candidates << link.url;
    if (!candidates.isEmpty() && firstFeed(fetcher.fetchAll(requestsFor(candidates)), result)) {
        result.discovered = true;
        return result;
    }
    if (firstFeed(fetcher.fetchAll(requestsFor(commonFeedUrls(page.finalUrl))), result)) {
        result.discovered = true;
        return result;
    }
    result.error = QStringLiteral("no RSS, Atom, or JSON feed found at %1").arg(url.toString());
    return result;
}
}
