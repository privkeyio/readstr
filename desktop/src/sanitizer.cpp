#include "sanitizer.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

#include "feedparser.h"

namespace {
// The bidirectional formatting characters that act on a span of text, not the
// single-position marks (ALM, LRM, RLM).
bool isDirectionOverride(QChar character) {
    const char16_t code = character.unicode();
    return FeedParser::isBidiFormat(character) && code != 0x061C && code != 0x200E && code != 0x200F;
}

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

// Kept, but only one level deep and within the caps below.
const QSet<QString> tableParts{"table", "tr", "td", "th"};
// Kept tags that do not start a block of their own.
const QSet<QString> inlines{"a", "code", "em", "i", "strong", "b"};

constexpr qsizetype maxAltLength = 200;
// Qt lays out a table inside a table in time exponential in the depth (21
// levels took minutes), and every cell and row costs layout time, so tables
// do not nest and stay this big; what does not fit becomes block text.
constexpr int maxTableRows = 1000;
constexpr int maxTableCells = 32;
// A link's address is not text, so the output cap only notices it after the
// fact; one this long is no link anyone follows.
constexpr qsizetype maxUrlLength = 8 * 1024;
// Qt's line layout slows down badly on long runs without a break opportunity
// (a 512K-character word took seconds), so text gets a zero-width space after
// this many characters without one. It shows as nothing.
constexpr int maxRunLength = 80;
// A run that long is one endless cluster (a letter under thousands of
// combining marks): it breaks anyway, inside the cluster.
constexpr int maxClusterRunLength = 2 * maxRunLength;

bool isSpace(QChar character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f';
}

// A break opportunity for Qt's line layout; a no-break space is none.
bool breaksLines(QChar character) {
    return isSpace(character) || (character.isSpace() && character != QChar::Nbsp);
}

// Whether text[at] belongs to the character before it: a combining mark
// (Thai, Lao, Khmer, and Burmese vowels and tones among them), a zero-width
// joiner, a variation selector, or the second half of a surrogate pair.
bool joinsPrevious(const QString &text, qsizetype at) {
    const QChar character = text.at(at);
    const char16_t code = character.unicode();
    if (character.isLowSurrogate() || code == 0x200D || (code >= 0xFE00 && code <= 0xFE0F))
        return true;
    if (character.isHighSurrogate() && at + 1 < text.size() && text.at(at + 1).isLowSurrogate()) {
        const char32_t point = QChar::surrogateToUcs4(character, text.at(at + 1));
        // Supplementary variation selectors, and the emoji skin tone
        // modifiers, which also join the emoji before them.
        return (point >= 0xE0100 && point <= 0xE01EF) || (point >= 0x1F3FB && point <= 0x1F3FF)
               || QChar::isMark(point);
    }
    return character.isMark();
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
        // Embeddings, overrides and isolates can make the article read
        // differently than it is. The direction marks stay: right-to-left
        // articles need them to place punctuation and numbers.
        decoded.removeIf(isDirectionOverride);
        for (const QChar character : decoded)
            if (!isSpace(character)) {
                m_boundary = false;
                break;
            }
        QString broken;
        broken.reserve(decoded.size());
        for (qsizetype k = 0; k < decoded.size(); ++k) {
            const QChar character = decoded.at(k);
            // Once the run is long enough the space goes before the next
            // character that starts a cluster of its own, never inside one
            // unless the run is far too long, and never inside a surrogate
            // pair.
            const QChar previous = k > 0 ? decoded.at(k - 1) : m_last;
            const bool startsCluster = !joinsPrevious(decoded, k) && previous != QChar(0x200D);
            if (!breaksLines(character)
                && ((m_run >= maxRunLength && startsCluster)
                    || (m_run >= maxClusterRunLength && !character.isLowSurrogate()))) {
                broken += QChar(0x200B);
                m_run = 0;
            }
            broken += character;
            m_run = breaksLines(character) ? 0 : m_run + 1;
        }
        if (!decoded.isEmpty())
            m_last = decoded.back();
        QString escaped = broken.toHtmlEscaped();
        const qsizetype room = Sanitizer::maxOutputLength - m_out.size();
        if (escaped.size() > room) {
            // Escaping makes a character at most six ("&quot;").
            broken.truncate(qMax<qsizetype>(0, room / 6));
            // Never half of a surrogate pair.
            if (!broken.isEmpty() && broken.back().isHighSurrogate())
                broken.chop(1);
            escaped = broken.toHtmlEscaped();
            m_full = true;
        }
        m_out += escaped;
    }

    bool full() const { return m_full || m_out.size() > Sanitizer::maxOutputLength; }

    void start(const Tag &tag) {
        const QString &name = tag.name;
        if (name == QLatin1String("br")) {
            m_out += QLatin1String("<br>");
            boundary();
        } else if (name == QLatin1String("hr")) {
            m_out += QLatin1String("<hr>");
            boundary();
        } else if (name == QLatin1String("img")) {
            image(tag);
        } else if (tableParts.contains(name)) {
            startTablePart(tag);
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
            boundary();
        } else if (!m_flatOpen.isEmpty() && tableParts.contains(name)) {
            const qsizetype at = m_flatOpen.lastIndexOf(name);
            if (at >= 0) {
                m_flatOpen.resize(at);
                separate();
            } else {
                // Not one of the flattened tables' own: it ends the open
                // table's cell, row, or the table itself, and the tables
                // left unclosed in there with it, as in a browser.
                m_flatOpen.clear();
                close(name);
            }
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
    void boundary() {
        m_boundary = true;
        m_run = 0;
    }

    void separate() {
        if (m_boundary)
            return;
        m_out += QLatin1String("<br>");
        boundary();
    }

    // A table inside an open table, a row or cell outside one, and rows and
    // cells past the caps become separators, so only one level of table
    // with bounded rows and cells ever reaches Qt.
    void startTablePart(const Tag &tag) {
        const QString &name = tag.name;
        const qsizetype table = m_open.lastIndexOf(QLatin1String("table"));
        if (!m_flatOpen.isEmpty() || (table >= 0 && name == QLatin1String("table"))) {
            if (m_flatOpen.size() < Sanitizer::maxDepth)
                m_flatOpen << name;
            separate();
            return;
        }
        if (name == QLatin1String("table")) {
            m_rows = 0;
            open(tag);
            return;
        }
        if (table < 0) {
            separate();
            return;
        }
        if (name == QLatin1String("tr")) {
            // A new row ends the one still open, as in a browser.
            if (m_open.lastIndexOf(QLatin1String("tr")) > table)
                close(QStringLiteral("tr"));
            if (++m_rows > maxTableRows) {
                separate();
                return;
            }
            m_cells = 0;
            open(tag);
            return;
        }
        const qsizetype row = m_open.lastIndexOf(QLatin1String("tr"));
        if (row < table || ++m_cells > maxTableCells) {
            separate();
            return;
        }
        // So does a new cell the one still open.
        const qsizetype cell = qMax(m_open.lastIndexOf(QLatin1String("td")), m_open.lastIndexOf(QLatin1String("th")));
        if (cell > row)
            close(QString(m_open.at(cell)));
        open(tag);
    }

    void open(const Tag &tag) {
        QString opening = QLatin1Char('<') + tag.name + QLatin1Char('>');
        if (tag.name == QLatin1String("a")) {
            // Links do not nest; a new one ends the one still open, as in a
            // browser.
            if (m_open.contains(QLatin1String("a")))
                close(QStringLiteral("a"));
            const QString url = linkTarget(tag.href);
            if (url.isEmpty())
                return;
            opening = QLatin1String("<a href=\"") + url.toHtmlEscaped() + QLatin1String("\">");
        }
        if (m_open.size() >= Sanitizer::maxDepth)
            return;
        m_open << tag.name;
        m_out += opening;
        if (!inlines.contains(tag.name))
            boundary();
    }

    void close(const QString &name) {
        const qsizetype at = m_open.lastIndexOf(name);
        if (at < 0)
            return;
        // Only the end of a block is a boundary: after "a<em>b</em>" a block
        // that Qt does not get still needs its separator.
        bool block = false;
        while (m_open.size() > at) {
            const QString closed = m_open.takeLast();
            block |= !inlines.contains(closed);
            m_out += QLatin1String("</") + closed + '>';
        }
        if (block)
            boundary();
        if (!m_open.contains(QLatin1String("table")))
            m_flatOpen.clear();
    }

    // The checked, encoded address for a link or image, or empty.
    QString linkTarget(const QString &href) const {
        const QUrl url = Sanitizer::webUrl(href, m_base);
        const QString encoded = url.isEmpty() ? QString() : url.toString(QUrl::FullyEncoded);
        return encoded.size() > maxUrlLength ? QString() : encoded;
    }

    void image(const Tag &tag) {
        QString url = linkTarget(tag.src);
        if (url.isEmpty())
            url = linkTarget(tag.dataSrc);
        if (url.isEmpty())
            return;
        QString alt = tag.alt;
        alt.removeIf(isDirectionOverride);
        alt = alt.simplified();
        if (alt.size() > maxAltLength)
            alt = alt.left(maxAltLength) + QChar(0x2026);
        const QString label = alt.isEmpty()
                                  ? QCoreApplication::translate("Sanitizer", "[image]")
                                  : QCoreApplication::translate("Sanitizer", "[image: %1]").arg(alt);
        // Inside a link the label is part of that link already.
        if (m_open.contains(QLatin1String("a")))
            m_out += label.toHtmlEscaped();
        else
            m_out += QLatin1String("<a href=\"") + url.toHtmlEscaped() + QLatin1String("\">") + label.toHtmlEscaped()
                     + QLatin1String("</a>");
        m_boundary = false;
    }

    QUrl m_base;
    QString m_out;
    QStringList m_open;
    bool m_boundary = true;
    bool m_full = false;
    // Characters since the last line break opportunity, and the last one.
    int m_run = 0;
    QChar m_last;
    // The table parts opened inside the open table, which are shown flat;
    // rows of the open table, and cells of its open row.
    QStringList m_flatOpen;
    int m_rows = 0;
    int m_cells = 0;
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
    QString clean = text.left(maxInputLength);
    clean.removeIf(isDirectionOverride);
    if (clean.isEmpty())
        return {};
    return QLatin1String("<p>") + clean.toHtmlEscaped() + QLatin1String("</p>");
}

QString linesToRichText(const QString &text) {
    QString clean = text.left(maxInputLength);
    clean.removeIf(isDirectionOverride);
    clean.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    QString out;
    static const QRegularExpression blank(QStringLiteral("\\n[ \\t]*\\n\\s*"));
    for (const QString &paragraph : clean.split(blank, Qt::SkipEmptyParts)) {
        const QString trimmed = paragraph.trimmed();
        if (!trimmed.isEmpty())
            out += QLatin1String("<p>") + trimmed.toHtmlEscaped().replace('\n', QLatin1String("<br>"))
                + QLatin1String("</p>");
    }
    return out;
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
