#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QUrl>
#include <optional>

class QXmlStreamReader;

// Feed bodies are untrusted input. Titles, summaries, and authors come out as
// plain text; `content` keeps the feed's HTML as it was, so the window in 1b
// must show it with Text.PlainText or through the sanitizer, never as raw rich
// text.
struct ParsedItem {
    QString guid;
    QString title;
    QString url;
    QString author;
    QString summary;
    QString content;
    // Invalid when the feed gives no date that can be read.
    QDateTime published;
};

struct ParsedFeed {
    QString title;
    QString siteUrl;
    QString description;
    QList<ParsedItem> items;
};

// RSS 0.9x/1.0/2.0, Atom, and JSON Feed. Documents larger than maxBytes are
// refused, items beyond maxItems are dropped, and an XML document whose DTD
// declares entities is refused rather than expanded (billion laughs); external
// entities are never fetched.
namespace FeedParser {
inline constexpr qint64 maxBytes = 10 * 1024 * 1024;
inline constexpr int maxItems = 1000;
inline constexpr int maxSummaryLength = 500;

// `base` is the URL the document came from, used to resolve relative links.
std::optional<ParsedFeed> parse(const QByteArray &data, const QUrl &base, QString *error = nullptr);
// RFC 822/2822 (RSS) and RFC 3339 (Atom, JSON Feed) dates, returned in UTC.
QDateTime parseDate(const QString &text);
// Drops tags, comments, scripts, and styles, decodes entities, and collapses
// whitespace.
QString plainText(const QString &html);
// True when the reader sits on a DTD that declares entities, which
// QXmlStreamReader would expand. Every XML reader here refuses such documents.
bool declaresEntities(const QXmlStreamReader &xml);
}
