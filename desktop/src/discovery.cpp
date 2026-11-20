#include "discovery.h"

#include <QSet>

namespace {
// Only the start of a page is searched: feed links live in <head>.
constexpr qsizetype scanBytes = 1024 * 1024;
// Pages advertising more feeds than this get only their first few checked.
constexpr int maxCandidates = 5;

const QStringList feedTypes{"application/rss+xml", "application/atom+xml", "application/feed+json"};

struct Tag {
    QString name;
    QHash<QString, QString> attributes;
};

bool isSpace(QChar character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f';
}

// The <link> and <base> tags of a page, read the way an HTML tokenizer reads
// them, as far as feed links need: comments are skipped, a quoted attribute
// value may hold '>', and unquoted values end at whitespace or '>'. Every
// other tag is stepped over the same way, so a '<link' inside another tag's
// attribute is not mistaken for one. Each character is looked at a bounded
// number of times, so a hostile page cannot make this slow.
QList<Tag> linkTags(const QString &text) {
    QList<Tag> tags;
    const qsizetype size = text.size();
    qsizetype i = 0;
    while ((i = text.indexOf('<', i)) >= 0) {
        if (QStringView(text).mid(i, 4) == QLatin1String("<!--")) {
            const qsizetype end = text.indexOf(QLatin1String("-->"), i + 4);
            if (end < 0)
                break;
            i = end + 3;
            continue;
        }
        const bool endTag = i + 1 < size && text.at(i + 1) == '/';
        qsizetype j = endTag ? i + 2 : i + 1;
        const qsizetype nameStart = j;
        while (j < size && (text.at(j).isLetterOrNumber() || text.at(j) == '-'))
            ++j;
        if (j == nameStart || !text.at(nameStart).isLetter()) {
            i = nameStart;
            continue;
        }
        Tag tag{text.mid(nameStart, j - nameStart).toLower(), {}};
        const bool wanted = !endTag && (tag.name == QLatin1String("link") || tag.name == QLatin1String("base"));
        bool closed = false;
        while (j < size) {
            while (j < size && (isSpace(text.at(j)) || text.at(j) == '/'))
                ++j;
            if (j >= size)
                break;
            if (text.at(j) == '>') {
                ++j;
                closed = true;
                break;
            }
            const qsizetype attributeStart = j;
            while (j < size && !isSpace(text.at(j)) && text.at(j) != '=' && text.at(j) != '>' && text.at(j) != '/')
                ++j;
            if (j == attributeStart)
                ++j;
            const QString name = text.mid(attributeStart, j - attributeStart).toLower();
            while (j < size && isSpace(text.at(j)))
                ++j;
            QString value;
            if (j < size && text.at(j) == '=') {
                ++j;
                while (j < size && isSpace(text.at(j)))
                    ++j;
                if (j < size && (text.at(j) == '"' || text.at(j) == '\'')) {
                    const qsizetype end = text.indexOf(text.at(j), j + 1);
                    if (end < 0) {
                        j = size;
                        break;
                    }
                    value = text.mid(j + 1, end - j - 1);
                    j = end + 1;
                } else {
                    const qsizetype valueStart = j;
                    while (j < size && !isSpace(text.at(j)) && text.at(j) != '>')
                        ++j;
                    value = text.mid(valueStart, j - valueStart);
                }
            }
            // Decodes &amp; and friends; URLs and titles never need markup.
            if (wanted && !tag.attributes.contains(name))
                tag.attributes.insert(name, FeedParser::plainText(value));
        }
        // A tag still open at the end of the text swallows the rest of it.
        if (!closed)
            break;
        if (wanted)
            tags << tag;
        i = j;
        // Scripts and styles are raw text, where '<' and quotes mean nothing.
        if (!endTag && (tag.name == QLatin1String("script") || tag.name == QLatin1String("style"))) {
            i = text.indexOf(QLatin1String("</") + tag.name, i, Qt::CaseInsensitive);
            if (i < 0)
                break;
        }
    }
    return tags;
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
    const QString text = QString::fromUtf8(html.left(scanBytes));

    QUrl base = pageUrl;
    bool sawBase = false;
    QList<Link> links;
    QSet<QUrl> seen;
    for (const Tag &tag : linkTags(text)) {
        const auto &attributes = tag.attributes;
        if (tag.name == QLatin1String("base")) {
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

Search::Search(const QUrl &url) : m_url(url), m_urls{url} {}

QList<FetchRequest> Search::requests() const { return requestsFor(m_urls); }

void Search::examine(const QList<FetchResult> &results) {
    switch (m_stage) {
    case Stage::Page: {
        const FetchResult page = results.value(0);
        if (!page.ok()) {
            m_result.error = QStringLiteral("cannot fetch %1: %2").arg(m_urls.constFirst().toString(), page.error);
            m_stage = Stage::Done;
            return;
        }
        if (auto feed = FeedParser::parse(page.body, page.finalUrl)) {
            m_result.found = true;
            m_result.feedUrl = m_urls.constFirst();
            m_result.feed = *feed;
            m_result.fetch = page;
            m_stage = Stage::Done;
            return;
        }
        m_pageUrl = page.finalUrl;
        m_urls.clear();
        for (const Link &link : feedLinks(page.body, page.finalUrl))
            if (m_urls.size() < maxCandidates)
                m_urls << link.url;
        if (m_urls.isEmpty()) {
            m_urls = commonFeedUrls(m_pageUrl);
            m_stage = Stage::Common;
        } else {
            m_stage = Stage::Advertised;
        }
        return;
    }
    case Stage::Advertised:
        if (firstFeed(results, m_result)) {
            m_result.discovered = true;
            m_stage = Stage::Done;
            return;
        }
        m_urls = commonFeedUrls(m_pageUrl);
        m_stage = Stage::Common;
        return;
    case Stage::Common:
        if (firstFeed(results, m_result)) {
            m_result.discovered = true;
        } else {
            m_result.error = QStringLiteral("no RSS, Atom, or JSON feed found at %1").arg(m_url.toString());
        }
        m_stage = Stage::Done;
        return;
    case Stage::Done:
        return;
    }
}

Result discover(Fetcher &fetcher, const QUrl &url) {
    Search search(url);
    while (!search.finished())
        search.examine(fetcher.fetchAll(search.requests()));
    return search.result();
}
}
