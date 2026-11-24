#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QUrl>
#include <optional>

class QXmlStreamReader;

// Feed bodies are untrusted input. Titles, summaries, and authors come out as
// plain text; `content` keeps the feed's HTML as it was, so the window shows
// it only through Sanitizer::toRichText, never as raw rich text.
struct ParsedItem {
    QString guid;
    QString title;
    QString url;
    QString author;
    QString summary;
    QString content;
    // Invalid when the feed gives no date that can be read.
    QDateTime published;
    // Set for a Nostr replaceable event ("30023:<pubkey>:<d>"): versions with
    // the same address are one article, and the highest `revision` (the
    // event's created_at) wins.
    QString address;
    qint64 revision = 0;
    // A Nostr video's (NIP-71): what plays it, its still, and its length in
    // seconds (negative when unknown). The URLs are http(s) only.
    QString mediaUrl;
    QString thumbnailUrl;
    double duration = -1;
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
// Whether an inline tag (b, em, a, span, and the like) parts the words on
// either side of it. Display text keeps the space every tag leaves; the
// search index joins them, as the reader shows "pro<em>gram</em>ming" as one
// word. Block, line-break, and unknown tags part words either way.
enum class InlineTags { Separate, Join };
// Drops tags, comments, scripts, and styles, decodes entities, and collapses
// whitespace.
QString plainText(const QString &html, InlineTags inlineTags = InlineTags::Separate);
// Decodes character references (&amp;, &#38;, &#x26;) and the common named
// entities; anything else is left as written.
QString decodeEntities(const QString &text);
// Shortens plain text to at most maxSummaryLength characters, at a word
// when one ends past the halfway point, with an ellipsis. It takes text that
// is already plain, so a literal '<' in a text field survives.
QString summarize(QString text);
// Bidirectional formatting characters (embeddings, overrides, isolates, and
// marks), which can make text read differently than it is.
bool isBidiFormat(QChar character);
// Control characters become spaces, bidirectional formatting characters go,
// and whitespace collapses, as for every text field a feed gives. The
// sanitizer drops the embeddings, overrides and isolates from an article's
// text and image labels but keeps the direction marks, which right-to-left
// prose relies on, and leaves the rest of its text as it is.
QString cleanText(const QString &text);
// True when the reader sits on a DTD that declares entities, which
// QXmlStreamReader would expand. Every XML reader here refuses such documents.
bool declaresEntities(const QXmlStreamReader &xml);
}
