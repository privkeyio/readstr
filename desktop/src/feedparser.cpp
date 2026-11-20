#include "feedparser.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimeZone>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace {
const QString atomNamespace = QStringLiteral("http://www.w3.org/2005/Atom");
const QString contentNamespace = QStringLiteral("http://purl.org/rss/1.0/modules/content/");
const QString dublinCoreNamespace = QStringLiteral("http://purl.org/dc/elements/1.1/");
const QString xmlNamespace = QStringLiteral("http://www.w3.org/XML/1998/namespace");

bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}

// xml:base on any element changes the base for links inside it (Atom uses it;
// a few RSS feeds do too).
QUrl baseOf(const QUrl &base, const QXmlStreamReader &xml) {
    const auto value = xml.attributes().value(xmlNamespace, QLatin1String("base"));
    return value.isEmpty() ? base : base.resolved(QUrl(value.toString().trimmed()));
}

// Only web links survive, so a feed cannot hand the window a javascript: or
// file: URL to open.
QString webLink(const QUrl &base, const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};
    const QUrl url = base.resolved(QUrl(trimmed));
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != "http" && scheme != "https") || url.host().isEmpty())
        return {};
    return url.toString(QUrl::FullyEncoded);
}

QString summarize(const QString &html) {
    QString text = FeedParser::plainText(html);
    if (text.size() <= FeedParser::maxSummaryLength)
        return text;
    text.truncate(FeedParser::maxSummaryLength);
    const qsizetype space = text.lastIndexOf(' ');
    if (space > FeedParser::maxSummaryLength / 2)
        text.truncate(space);
    return text + QChar(0x2026);
}

// Items need a stable key for de-duplication. Feeds without guid, id, or link
// are keyed by their title and content instead.
void finishItem(ParsedItem &item) {
    if (item.guid.isEmpty())
        item.guid = item.url;
    if (item.guid.isEmpty())
        item.guid = QString::fromLatin1(
            QCryptographicHash::hash((item.title + '\n' + item.content).toUtf8(), QCryptographicHash::Sha256)
                .toHex());
    if (item.title.isEmpty())
        item.title = QStringLiteral("Untitled");
}

QString escapeHtml(const QString &text) { return text.toHtmlEscaped(); }

// Atom text constructs: type "xhtml" holds markup as child elements, so it is
// copied out as XML; "html" is escaped markup; "text" (the default) is plain.
QString readInnerXml(QXmlStreamReader &xml) {
    QString out;
    QXmlStreamWriter writer(&out);
    int depth = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && depth == 0)
            break;
        if (xml.isStartElement())
            ++depth;
        else if (xml.isEndElement())
            --depth;
        writer.writeCurrentToken(xml);
    }
    return out;
}

struct AtomText {
    QString html;
    QString plain;
};

AtomText readAtomText(QXmlStreamReader &xml) {
    const QString type = xml.attributes().value("type").toString().toLower();
    if (type == "xhtml" || type.endsWith("xhtml+xml")) {
        const QString html = readInnerXml(xml);
        return {html, FeedParser::plainText(html)};
    }
    const QString text = xml.readElementText(QXmlStreamReader::IncludeChildElements);
    if (type == "html" || type == "text/html")
        return {text, FeedParser::plainText(text)};
    return {escapeHtml(text), text.simplified()};
}

QString readText(QXmlStreamReader &xml) {
    return xml.readElementText(QXmlStreamReader::IncludeChildElements);
}

bool isRssElement(const QXmlStreamReader &xml, const char *name) {
    // RSS 0.9x and 2.0 have no namespace; RSS 1.0 uses its own.
    return xml.name() == QLatin1String(name)
           && (xml.namespaceUri().isEmpty() || xml.namespaceUri() == QLatin1String("http://purl.org/rss/1.0/")
               || xml.namespaceUri() == QLatin1String("http://my.netscape.com/rdf/simple/0.9/"));
}

bool isAtom(const QXmlStreamReader &xml, const char *name) {
    return xml.name() == QLatin1String(name) && xml.namespaceUri() == atomNamespace;
}

ParsedItem readRssItem(QXmlStreamReader &xml, QUrl base) {
    base = baseOf(base, xml);
    ParsedItem item;
    QString description, encoded, atomLink;
    bool guidIsLink = true;
    QString date;
    while (xml.readNextStartElement()) {
        const QUrl elementBase = baseOf(base, xml);
        if (isRssElement(xml, "title")) {
            item.title = FeedParser::plainText(readText(xml));
        } else if (isRssElement(xml, "link")) {
            item.url = webLink(elementBase, readText(xml));
        } else if (isRssElement(xml, "description")) {
            description = readText(xml);
        } else if (xml.name() == QLatin1String("encoded") && xml.namespaceUri() == contentNamespace) {
            encoded = readText(xml);
        } else if (isRssElement(xml, "author")
                   || (xml.name() == QLatin1String("creator") && xml.namespaceUri() == dublinCoreNamespace)) {
            const QString author = FeedParser::plainText(readText(xml));
            if (item.author.isEmpty())
                item.author = author;
        } else if (isRssElement(xml, "pubDate")
                   || (xml.name() == QLatin1String("date") && xml.namespaceUri() == dublinCoreNamespace)) {
            date = readText(xml);
        } else if (isRssElement(xml, "guid")) {
            guidIsLink = xml.attributes().value("isPermaLink").compare(QLatin1String("false"), Qt::CaseInsensitive);
            item.guid = readText(xml).trimmed();
        } else if (isAtom(xml, "link")) {
            const auto rel = xml.attributes().value("rel");
            if (rel.isEmpty() || rel == QLatin1String("alternate"))
                atomLink = webLink(elementBase, xml.attributes().value("href").toString());
            xml.skipCurrentElement();
        } else {
            xml.skipCurrentElement();
        }
    }
    if (item.url.isEmpty())
        item.url = atomLink;
    if (item.url.isEmpty() && guidIsLink)
        item.url = webLink(base, item.guid);
    item.content = encoded.isEmpty() ? description : encoded;
    item.summary = summarize(description.isEmpty() ? encoded : description);
    item.published = FeedParser::parseDate(date);
    finishItem(item);
    return item;
}

void readRssChannel(QXmlStreamReader &xml, QUrl base, ParsedFeed &feed) {
    base = baseOf(base, xml);
    while (xml.readNextStartElement()) {
        if (isRssElement(xml, "title")) {
            feed.title = FeedParser::plainText(readText(xml));
        } else if (isRssElement(xml, "link")) {
            feed.siteUrl = webLink(baseOf(base, xml), readText(xml));
        } else if (isRssElement(xml, "description")) {
            feed.description = summarize(readText(xml));
        } else if (isRssElement(xml, "item")) {
            if (feed.items.size() < FeedParser::maxItems)
                feed.items << readRssItem(xml, base);
            else
                xml.skipCurrentElement();
        } else {
            xml.skipCurrentElement();
        }
    }
}

// RSS 2.0 nests items in the channel; RSS 1.0 puts them beside it.
void readRss(QXmlStreamReader &xml, const QUrl &base, ParsedFeed &feed) {
    const QUrl rootBase = baseOf(base, xml);
    while (xml.readNextStartElement()) {
        if (isRssElement(xml, "channel"))
            readRssChannel(xml, rootBase, feed);
        else if (isRssElement(xml, "item") && feed.items.size() < FeedParser::maxItems)
            feed.items << readRssItem(xml, rootBase);
        else
            xml.skipCurrentElement();
    }
}

QString readAtomLink(QXmlStreamReader &xml, const QUrl &base) {
    const auto attributes = xml.attributes();
    const auto rel = attributes.value("rel");
    QString href;
    if (rel.isEmpty() || rel == QLatin1String("alternate"))
        href = webLink(baseOf(base, xml), attributes.value("href").toString());
    xml.skipCurrentElement();
    return href;
}

QString readAtomAuthor(QXmlStreamReader &xml) {
    QString name;
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("name"))
            name = FeedParser::plainText(readText(xml));
        else
            xml.skipCurrentElement();
    }
    return name;
}

ParsedItem readAtomEntry(QXmlStreamReader &xml, QUrl base) {
    base = baseOf(base, xml);
    ParsedItem item;
    AtomText content, summary;
    QString published, updated;
    while (xml.readNextStartElement()) {
        const auto name = xml.name();
        if (name == QLatin1String("title")) {
            item.title = readAtomText(xml).plain;
        } else if (name == QLatin1String("link")) {
            const QString href = readAtomLink(xml, base);
            if (item.url.isEmpty())
                item.url = href;
        } else if (name == QLatin1String("id")) {
            item.guid = readText(xml).trimmed();
        } else if (name == QLatin1String("content")) {
            // Out-of-line content (src=) is not fetched.
            if (xml.attributes().hasAttribute("src"))
                xml.skipCurrentElement();
            else
                content = readAtomText(xml);
        } else if (name == QLatin1String("summary")) {
            summary = readAtomText(xml);
        } else if (name == QLatin1String("author")) {
            const QString author = readAtomAuthor(xml);
            if (item.author.isEmpty())
                item.author = author;
        } else if (name == QLatin1String("published") || name == QLatin1String("issued")) {
            published = readText(xml);
        } else if (name == QLatin1String("updated") || name == QLatin1String("modified")) {
            updated = readText(xml);
        } else {
            xml.skipCurrentElement();
        }
    }
    item.content = content.html.isEmpty() ? summary.html : content.html;
    item.summary = summarize(summary.plain.isEmpty() ? content.plain : summary.plain);
    item.published = FeedParser::parseDate(published.isEmpty() ? updated : published);
    finishItem(item);
    return item;
}

void readAtom(QXmlStreamReader &xml, const QUrl &base, ParsedFeed &feed) {
    const QUrl feedBase = baseOf(base, xml);
    while (xml.readNextStartElement()) {
        const auto name = xml.name();
        if (name == QLatin1String("title")) {
            feed.title = readAtomText(xml).plain;
        } else if (name == QLatin1String("subtitle") || name == QLatin1String("tagline")) {
            feed.description = summarize(readAtomText(xml).plain);
        } else if (name == QLatin1String("link")) {
            const QString href = readAtomLink(xml, feedBase);
            if (feed.siteUrl.isEmpty())
                feed.siteUrl = href;
        } else if (name == QLatin1String("entry")) {
            if (feed.items.size() < FeedParser::maxItems)
                feed.items << readAtomEntry(xml, feedBase);
            else
                xml.skipCurrentElement();
        } else {
            xml.skipCurrentElement();
        }
    }
}

std::optional<ParsedFeed> parseXml(const QByteArray &data, const QUrl &base, QString *error) {
    QXmlStreamReader xml(data);
    while (!xml.atEnd() && !xml.isStartElement()) {
        xml.readNext();
        if (FeedParser::declaresEntities(xml)) {
            fail(error, QStringLiteral("feed declares XML entities, which are not allowed"));
            return std::nullopt;
        }
    }
    if (xml.hasError() || !xml.isStartElement()) {
        fail(error, QStringLiteral("not a feed: %1").arg(xml.hasError() ? xml.errorString() : "empty document"));
        return std::nullopt;
    }

    ParsedFeed feed;
    const auto root = xml.name();
    if (root == QLatin1String("rss") || root == QLatin1String("RDF")) {
        readRss(xml, base, feed);
    } else if (root == QLatin1String("feed")) {
        readAtom(xml, base, feed);
    } else {
        fail(error, QStringLiteral("not a feed: unexpected <%1> element").arg(root.toString()));
        return std::nullopt;
    }
    if (xml.hasError()) {
        fail(error, QStringLiteral("malformed feed: %1 (line %2)").arg(xml.errorString()).arg(xml.lineNumber()));
        return std::nullopt;
    }
    return feed;
}

QString jsonString(const QJsonValue &value) {
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toDouble(), 'g', 17);
    return {};
}

QString jsonAuthor(const QJsonObject &object) {
    const QJsonArray authors = object.value("authors").toArray();
    const QJsonObject author = authors.isEmpty() ? object.value("author").toObject() : authors.first().toObject();
    return FeedParser::plainText(author.value("name").toString());
}

std::optional<ParsedFeed> parseJson(const QByteArray &data, const QUrl &base, QString *error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    const QJsonObject object = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !object.value("version").toString().startsWith(QLatin1String("https://jsonfeed.org/version/"))) {
        fail(error, QStringLiteral("not a feed: JSON without a JSON Feed version"));
        return std::nullopt;
    }

    ParsedFeed feed;
    feed.title = FeedParser::plainText(object.value("title").toString());
    feed.siteUrl = webLink(base, object.value("home_page_url").toString());
    feed.description = summarize(object.value("description").toString());
    for (const QJsonValue &value : object.value("items").toArray()) {
        if (feed.items.size() >= FeedParser::maxItems)
            break;
        const QJsonObject entry = value.toObject();
        ParsedItem item;
        item.guid = jsonString(entry.value("id")).trimmed();
        item.title = FeedParser::plainText(entry.value("title").toString());
        item.url = webLink(base, entry.value("url").toString());
        if (item.url.isEmpty())
            item.url = webLink(base, entry.value("external_url").toString());
        item.author = jsonAuthor(entry);
        if (item.author.isEmpty())
            item.author = jsonAuthor(object);
        const QString html = entry.value("content_html").toString();
        const QString text = entry.value("content_text").toString();
        item.content = html.isEmpty() ? escapeHtml(text) : html;
        const QString summary = entry.value("summary").toString();
        item.summary = summarize(!summary.isEmpty() ? summary : !html.isEmpty() ? html : escapeHtml(text));
        const QString published = entry.value("date_published").toString();
        item.published = FeedParser::parseDate(published.isEmpty() ? entry.value("date_modified").toString() : published);
        finishItem(item);
        feed.items << item;
    }
    return feed;
}

int monthNumber(const QString &name) {
    static const QStringList months{"jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec"};
    return months.indexOf(name.left(3).toLower()) + 1;
}

// RFC 822 zone names as offsets in hours. Unknown names read as UTC, which is
// what RFC 2822 says to do with them.
int zoneOffsetSeconds(const QString &zone, bool *ok) {
    *ok = true;
    static const QRegularExpression numeric(QStringLiteral("^([+-])(\\d{2}):?(\\d{2})$"));
    const auto match = numeric.match(zone);
    if (match.hasMatch()) {
        const int seconds = match.captured(2).toInt() * 3600 + match.captured(3).toInt() * 60;
        if (seconds > 18 * 3600)
            *ok = false;
        return match.captured(1) == "-" ? -seconds : seconds;
    }
    static const QHash<QString, int> names{{"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5},
                                           {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7}};
    return names.value(zone.toUpper(), 0) * 3600;
}

QDateTime parseRfc822(const QString &text) {
    static const QRegularExpression pattern(QStringLiteral(
        "^(?:[A-Za-z]+,?\\s*)?(\\d{1,2})[\\s-]+([A-Za-z]{3,})[\\s-]+(\\d{2,4})\\s+(\\d{1,2}):(\\d{2})(?::(\\d{2}))?"
        "(?:\\.\\d+)?\\s*([A-Za-z]+|[+-]\\d{2}:?\\d{2})?\\b"));
    const auto match = pattern.match(text);
    if (!match.hasMatch())
        return {};
    int year = match.captured(3).toInt();
    if (match.captured(3).size() == 2)
        year += year < 50 ? 2000 : 1900;
    const QDate date(year, monthNumber(match.captured(2)), match.captured(1).toInt());
    const QTime time(match.captured(4).toInt(), match.captured(5).toInt(), match.captured(6).toInt());
    bool ok = false;
    const int offset = zoneOffsetSeconds(match.captured(7), &ok);
    if (!date.isValid() || !time.isValid() || !ok)
        return {};
    return QDateTime(date, time, QTimeZone::fromSecondsAheadOfUtc(offset)).toUTC();
}

QDateTime parseIso(QString text) {
    static const QRegularExpression isoStart(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}"));
    if (!isoStart.match(text).hasMatch())
        return {};
    if (text.size() > 10 && text.at(10) == ' ')
        text[10] = 'T';
    // Qt reads at most milliseconds, so finer fractions are cut to three digits.
    static const QRegularExpression fraction(QStringLiteral("(\\.\\d{3})\\d+"));
    text.replace(fraction, QStringLiteral("\\1"));
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid())
        return {};
    // A date without an offset is taken as UTC rather than this machine's zone,
    // so the same feed sorts the same everywhere.
    if (parsed.timeSpec() == Qt::LocalTime)
        parsed = QDateTime(parsed.date(), parsed.time(), QTimeZone::UTC);
    return parsed.toUTC();
}

QString decodeEntity(const QString &name) {
    if (name.startsWith('#')) {
        bool ok = false;
        const uint code = name.at(1).toLower() == 'x' ? name.mid(2).toUInt(&ok, 16) : name.mid(1).toUInt(&ok, 10);
        if (!ok || code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return {};
        const char32_t character = code;
        return QString::fromUcs4(&character, 1);
    }
    static const QHash<QString, QString> named{
        {"amp", "&"},         {"lt", "<"},          {"gt", ">"},          {"quot", "\""},
        {"apos", "'"},        {"nbsp", " "},        {"ndash", "–"},  {"mdash", "—"},
        {"hellip", "…"}, {"lsquo", "‘"},  {"rsquo", "’"},  {"ldquo", "“"},
        {"rdquo", "”"},  {"copy", "©"},   {"reg", "®"},    {"trade", "™"},
        {"laquo", "«"},  {"raquo", "»"},  {"middot", "·"}, {"bull", "•"},
        {"euro", "€"},   {"pound", "£"},  {"deg", "°"},    {"times", "×"}};
    return named.value(name, QString());
}
}

namespace FeedParser {
std::optional<ParsedFeed> parse(const QByteArray &data, const QUrl &base, QString *error) {
    if (data.size() > maxBytes) {
        fail(error, QStringLiteral("feed is larger than %1 MiB").arg(maxBytes / (1024 * 1024)));
        return std::nullopt;
    }
    qsizetype start = data.startsWith("\xEF\xBB\xBF") ? 3 : 0;
    while (start < data.size() && QChar::isSpace(uchar(data.at(start))))
        ++start;
    if (start == data.size()) {
        fail(error, QStringLiteral("empty feed"));
        return std::nullopt;
    }
    return data.at(start) == '{' ? parseJson(data, base, error) : parseXml(data, base, error);
}

QDateTime parseDate(const QString &text) {
    const QString trimmed = text.simplified();
    if (trimmed.isEmpty())
        return {};
    const QDateTime iso = parseIso(trimmed);
    return iso.isValid() ? iso : parseRfc822(trimmed);
}

QString plainText(const QString &html) {
    static const QRegularExpression hidden(QStringLiteral("<!--.*?-->|<(script|style)\\b.*?</\\1\\s*>"),
                                           QRegularExpression::CaseInsensitiveOption
                                               | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression tag(QStringLiteral("<[A-Za-z/!?][^>]*>"));
    static const QRegularExpression entity(QStringLiteral("&(#[0-9]{1,7}|#[xX][0-9A-Fa-f]{1,6}|[A-Za-z]{2,8});"));

    QString text = html;
    text.replace(hidden, QStringLiteral(" "));
    text.replace(tag, QStringLiteral(" "));

    QString decoded;
    decoded.reserve(text.size());
    qsizetype last = 0;
    auto matches = entity.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const QString replacement = decodeEntity(match.captured(1));
        decoded += QStringView(text).mid(last, match.capturedStart() - last);
        decoded += replacement.isNull() ? match.captured() : replacement;
        last = match.capturedEnd();
    }
    decoded += QStringView(text).mid(last);

    // Control characters would make exported OPML invalid XML and garble the
    // terminal, so they go along with the markup.
    for (QChar &character : decoded)
        if (character.category() == QChar::Other_Control)
            character = ' ';
    return decoded.simplified();
}

bool declaresEntities(const QXmlStreamReader &xml) {
    // QXmlStreamReader expands entities declared in an internal DTD (billion
    // laughs), so a document declaring any is refused before they are used.
    return xml.isDTD()
           && (!xml.entityDeclarations().isEmpty()
               || xml.text().contains(QLatin1String("<!ENTITY"), Qt::CaseInsensitive));
}
}
