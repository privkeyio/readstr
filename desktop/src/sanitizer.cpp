#include "sanitizer.h"

#include <QCoreApplication>
#include <QSet>

#include "feedparser.h"

namespace {
// Each of these is emitted as a bare tag (a keeps its checked href).
const QSet<QString> kept{"p",  "h1", "h2", "h3",         "h4",  "h5",     "h6",   "ul", "ol", "li", "blockquote",
                         "pre", "code", "em", "i", "strong", "b", "a", "table", "tr", "td", "th"};
// HTML's void elements: they never have content or an end tag.
const QSet<QString> voids{"area", "base", "br",   "col",   "embed", "hr",     "img", "input",
                          "link", "meta", "param", "source", "track", "wbr", "keygen"};
// Raw text: nothing inside is markup, up to the matching end tag, and all of
// it goes.
const QSet<QString> rawDropped{"script", "style", "iframe", "noscript", "noembed", "noframes", "textarea", "title",
                               "xmp"};
// Ordinary markup whose content goes with it.
const QSet<QString> droppedWithContent{"object", "form", "svg", "math", "template", "select", "button", "audio",
                                       "video",  "canvas", "map", "head", "applet", "frameset", "dialog"};
// Blocks Qt does not get: their text stays, on a line of its own.
const QSet<QString> separating{"div",  "section", "article", "header", "footer",  "figure", "figcaption",
                               "main", "aside",   "nav",     "address", "details", "summary", "dl",
                               "dt",   "dd",      "center",  "caption", "hgroup",  "body",   "html"};

// Kept tags that do not start a block of their own.
const QSet<QString> inlines{"a", "code", "em", "i", "strong", "b"};

constexpr qsizetype maxAltLength = 200;

bool isSpace(QChar character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f';
}

bool isAsciiLetter(QChar character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

struct Tag {
    QString name;
    bool end = false;
    bool selfClosing = false;
    QString href, src, dataSrc, alt;
};

class Writer {
public:
    explicit Writer(const QUrl &base) : m_base(base) {}

    void text(QStringView raw) {
        if (raw.isEmpty())
            return;
        QString decoded = FeedParser::decodeEntities(raw.toString());
        decoded.replace(QChar(0), QChar(0xFFFD));
        for (const QChar character : decoded)
            if (!isSpace(character)) {
                m_boundary = false;
                break;
            }
        QString escaped = decoded.toHtmlEscaped();
        const qsizetype room = Sanitizer::maxOutputLength - m_out.size();
        if (escaped.size() > room) {
            // Escaping makes a character at most six ("&quot;").
            decoded.truncate(qMax<qsizetype>(0, room / 6));
            escaped = decoded.toHtmlEscaped();
            m_full = true;
        }
        m_out += escaped;
    }

    bool full() const { return m_full || m_out.size() > Sanitizer::maxOutputLength; }

    void start(const Tag &tag) {
        const QString &name = tag.name;
        if (name == QLatin1String("br")) {
            m_out += QLatin1String("<br>");
            m_boundary = true;
        } else if (name == QLatin1String("hr")) {
            m_out += QLatin1String("<hr>");
            m_boundary = true;
        } else if (name == QLatin1String("img")) {
            image(tag);
        } else if (separating.contains(name)) {
            separate();
        } else if (kept.contains(name)) {
            open(tag);
        }
    }

    void end(const QString &name) {
        if (name == QLatin1String("br")) {
            // Browsers read a stray </br> as <br>.
            m_out += QLatin1String("<br>");
            m_boundary = true;
        } else if (separating.contains(name)) {
            separate();
        } else if (kept.contains(name)) {
            close(name);
        }
    }

    QString finish() {
        while (!m_open.isEmpty())
            m_out += QLatin1String("</") + m_open.takeLast() + '>';
        return m_out;
    }

private:
    void separate() {
        if (m_boundary)
            return;
        m_out += QLatin1String("<br>");
        m_boundary = true;
    }

    void open(const Tag &tag) {
        QString opening = QLatin1Char('<') + tag.name + QLatin1Char('>');
        if (tag.name == QLatin1String("a")) {
            // Links do not nest; a new one ends the one still open, as in a
            // browser.
            if (m_open.contains(QLatin1String("a")))
                close(QStringLiteral("a"));
            const QUrl url = Sanitizer::webUrl(tag.href, m_base);
            if (url.isEmpty())
                return;
            opening = QLatin1String("<a href=\"") + url.toString(QUrl::FullyEncoded).toHtmlEscaped()
                      + QLatin1String("\">");
        }
        if (m_open.size() >= Sanitizer::maxDepth)
            return;
        m_open << tag.name;
        m_out += opening;
        if (!inlines.contains(tag.name))
            m_boundary = true;
    }

    void close(const QString &name) {
        const qsizetype at = m_open.lastIndexOf(name);
        if (at < 0)
            return;
        while (m_open.size() > at)
            m_out += QLatin1String("</") + m_open.takeLast() + '>';
        m_boundary = true;
    }

    void image(const Tag &tag) {
        QUrl url = Sanitizer::webUrl(tag.src, m_base);
        if (url.isEmpty())
            url = Sanitizer::webUrl(tag.dataSrc, m_base);
        if (url.isEmpty())
            return;
        QString alt = tag.alt.simplified();
        if (alt.size() > maxAltLength)
            alt = alt.left(maxAltLength) + QChar(0x2026);
        const QString label = alt.isEmpty()
                                  ? QCoreApplication::translate("Sanitizer", "[image]")
                                  : QCoreApplication::translate("Sanitizer", "[image: %1]").arg(alt);
        // Inside a link the label is part of that link already.
        if (m_open.contains(QLatin1String("a")))
            m_out += label.toHtmlEscaped();
        else
            m_out += QLatin1String("<a href=\"") + url.toString(QUrl::FullyEncoded).toHtmlEscaped()
                     + QLatin1String("\">") + label.toHtmlEscaped() + QLatin1String("</a>");
        m_boundary = false;
    }

    QUrl m_base;
    QString m_out;
    QStringList m_open;
    bool m_boundary = true;
    bool m_full = false;
};

// Reads the tag starting at html[i] == '<' (a start or end tag whose name
// begins with a letter). Returns the index just past its '>', or -1 when the
// text ends inside it. Quoted attribute values may hold '>'.
qsizetype readTag(const QString &html, qsizetype i, Tag &tag) {
    const qsizetype size = html.size();
    tag.end = html.at(i + 1) == '/';
    qsizetype j = tag.end ? i + 2 : i + 1;
    const qsizetype nameStart = j;
    while (j < size && !isSpace(html.at(j)) && html.at(j) != '/' && html.at(j) != '>')
        ++j;
    tag.name = html.mid(nameStart, j - nameStart).toLower();
    int seen = 0;
    while (j < size) {
        while (j < size && isSpace(html.at(j)))
            ++j;
        if (j >= size)
            return -1;
        if (html.at(j) == '>')
            return j + 1;
        if (html.at(j) == '/') {
            ++j;
            if (j < size && html.at(j) == '>') {
                tag.selfClosing = true;
                return j + 1;
            }
            continue;
        }
        const qsizetype attributeStart = j;
        while (j < size && !isSpace(html.at(j)) && html.at(j) != '=' && html.at(j) != '>' && html.at(j) != '/')
            ++j;
        if (j == attributeStart)
            ++j;
        const QString name = html.mid(attributeStart, j - attributeStart).toLower();
        while (j < size && isSpace(html.at(j)))
            ++j;
        QString value;
        if (j < size && html.at(j) == '=') {
            ++j;
            while (j < size && isSpace(html.at(j)))
                ++j;
            if (j < size && (html.at(j) == '"' || html.at(j) == '\'')) {
                const qsizetype close = html.indexOf(html.at(j), j + 1);
                if (close < 0)
                    return -1;
                value = html.mid(j + 1, close - j - 1);
                j = close + 1;
            } else {
                const qsizetype valueStart = j;
                while (j < size && !isSpace(html.at(j)) && html.at(j) != '>')
                    ++j;
                value = html.mid(valueStart, j - valueStart);
            }
        }
        // The first of a repeated attribute wins, as in a browser.
        const int which = name == QLatin1String("href")       ? 1
                          : name == QLatin1String("src")      ? 2
                          : name == QLatin1String("data-src") ? 4
                          : name == QLatin1String("alt")      ? 8
                                                              : 0;
        if (which && !(seen & which)) {
            seen |= which;
            const QString decoded = FeedParser::decodeEntities(value);
            (which == 1 ? tag.href : which == 2 ? tag.src : which == 4 ? tag.dataSrc : tag.alt) = decoded;
        }
    }
    return -1;
}

// Where the raw text of `name` (script, style, ...) ends: the index of its
// "</name" closer, or the end of the text when it has none.
qsizetype rawTextEnd(const QString &html, qsizetype from, const QString &name) {
    const QString closer = QLatin1String("</") + name;
    qsizetype at = from;
    while ((at = html.indexOf(closer, at, Qt::CaseInsensitive)) >= 0) {
        const qsizetype after = at + closer.size();
        if (after >= html.size() || isSpace(html.at(after)) || html.at(after) == '>' || html.at(after) == '/')
            return at;
        at = after;
    }
    return html.size();
}
}

namespace Sanitizer {
QUrl webUrl(const QString &href, const QUrl &base) {
    // Browsers drop tabs and newlines anywhere in a URL and trim controls and
    // spaces from its ends, so "java\tscript:" is javascript: to them; this
    // reads it the same way before checking the scheme.
    QString cleaned;
    cleaned.reserve(href.size());
    for (const QChar character : href)
        if (character != '\t' && character != '\n' && character != '\r')
            cleaned += character;
    qsizetype first = 0, last = cleaned.size();
    while (first < last && cleaned.at(first).unicode() <= 0x20)
        ++first;
    while (last > first && cleaned.at(last - 1).unicode() <= 0x20)
        --last;
    cleaned = cleaned.mid(first, last - first);
    if (cleaned.isEmpty())
        return {};
    const QUrl relative(cleaned);
    QUrl url = base.isValid() && !base.isRelative() ? base.resolved(relative) : relative;
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        || url.host().isEmpty())
        return {};
    // Credentials in a link only serve to disguise where it goes.
    return url.adjusted(QUrl::RemoveUserInfo);
}

QString plainToRichText(const QString &text) {
    if (text.isEmpty())
        return {};
    return QLatin1String("<p>") + text.left(maxInputLength).toHtmlEscaped() + QLatin1String("</p>");
}

QString toRichText(const QString &input, const QUrl &base) {
    const QString html = input.left(maxInputLength);
    const qsizetype size = html.size();
    Writer writer(base);
    // Inside an element dropped with its content: its name and how deeply it
    // nests in itself.
    QString skipping;
    int skipDepth = 0;

    qsizetype i = 0, textStart = 0;
    auto flush = [&](qsizetype end) {
        if (skipping.isEmpty() && end > textStart)
            writer.text(QStringView(html).mid(textStart, end - textStart));
    };
    bool cut = input.size() > maxInputLength;
    while (i < size) {
        // Every link and image carries a whole URL, so a few bytes of input
        // can become many of output; past the cap the rest goes.
        if (writer.full()) {
            cut = true;
            textStart = size;
            break;
        }
        i = html.indexOf('<', i);
        if (i < 0)
            break;
        const QChar next = i + 1 < size ? html.at(i + 1) : QChar();
        const QChar afterSlash = i + 2 < size ? html.at(i + 2) : QChar();
        if (QStringView(html).mid(i, 4) == QLatin1String("<!--")) {
            flush(i);
            const qsizetype end = html.indexOf(QLatin1String("-->"), i + 4);
            i = textStart = end < 0 ? size : end + 3;
            continue;
        }
        if (next == '!' || next == '?' || (next == '/' && !isAsciiLetter(afterSlash) && afterSlash != QChar())) {
            // Doctypes, processing instructions, CDATA, and "</ x>" are bogus
            // comments up to the next '>'.
            flush(i);
            const qsizetype end = html.indexOf('>', i + 1);
            i = textStart = end < 0 ? size : end + 1;
            continue;
        }
        if (!isAsciiLetter(next) && !(next == '/' && isAsciiLetter(afterSlash))) {
            // A '<' that starts no tag is text.
            ++i;
            continue;
        }
        flush(i);
        Tag tag;
        const qsizetype after = readTag(html, i, tag);
        if (after < 0) {
            // A tag still open at the end swallows the rest of the text.
            i = textStart = size;
            break;
        }
        i = textStart = after;

        if (!skipping.isEmpty()) {
            if (tag.name == skipping && !tag.selfClosing)
                skipDepth += tag.end ? -1 : 1;
            if (skipDepth == 0)
                skipping.clear();
            else if (!tag.end && rawDropped.contains(tag.name))
                i = textStart = rawTextEnd(html, i, tag.name);
            continue;
        }
        if (tag.end) {
            writer.end(tag.name);
            continue;
        }
        if (rawDropped.contains(tag.name)) {
            i = textStart = rawTextEnd(html, i, tag.name);
            continue;
        }
        if (droppedWithContent.contains(tag.name)) {
            if (!tag.selfClosing && !voids.contains(tag.name)) {
                skipping = tag.name;
                skipDepth = 1;
            }
            continue;
        }
        writer.start(tag);
    }
    flush(size);
    cut |= writer.full();
    QString out = writer.finish();
    if (cut)
        out += QLatin1String("<p>") + QChar(0x2026) + QLatin1String("</p>");
    return out;
}
}
