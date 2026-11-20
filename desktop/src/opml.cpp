#include "opml.h"

#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "feedparser.h"
#include "store.h"

namespace {
struct Walk {
    QList<Opml::Outline> feeds;
    int count = 0;
    bool capped = false;
};

QString attribute(const QXmlStreamAttributes &attributes, const char *name) {
    return attributes.value(QLatin1String(name)).toString().trimmed();
}

QStringList tagsFrom(const QString &value) {
    QStringList tags;
    for (const QString &tag : value.split(','))
        if (!tag.trimmed().isEmpty())
            tags << tag.trimmed();
    return tags;
}

// Called with the reader on a parent element; consumes it to its end.
void walk(QXmlStreamReader &xml, const QString &folder, int depth, Walk &state) {
    if (depth > Opml::maxDepth) {
        xml.skipCurrentElement();
        return;
    }
    while (xml.readNextStartElement()) {
        if (depth == 0 && xml.name() == QLatin1String("body")) {
            walk(xml, folder, 0, state);
            continue;
        }
        if (state.capped || xml.name() != QLatin1String("outline")) {
            xml.skipCurrentElement();
            continue;
        }
        if (++state.count > Opml::maxOutlines) {
            state.capped = true;
            xml.skipCurrentElement();
            continue;
        }

        const auto attributes = xml.attributes();
        QString title = attribute(attributes, "title");
        if (title.isEmpty())
            title = attribute(attributes, "text");
        const QString xmlUrl = attribute(attributes, "xmlUrl");
        QString npub = attribute(attributes, "nostr");
        if (npub.isEmpty())
            npub = attribute(attributes, "npub");
        const QStringList tags = tagsFrom(attribute(attributes, "category"));

        if (attribute(attributes, "type").toLower() == QLatin1String("nostr") || !npub.isEmpty()) {
            const QString value = npub.isEmpty() ? xmlUrl : npub;
            if (!value.isEmpty())
                state.feeds << Opml::Outline{title, {}, value, folder, tags};
            xml.skipCurrentElement();
        } else if (!xmlUrl.isEmpty()) {
            state.feeds << Opml::Outline{title, xmlUrl, {}, folder, tags};
            walk(xml, folder, depth + 1, state);
        } else {
            walk(xml, title.isEmpty() ? folder : title, depth + 1, state);
        }
    }
}

QString normalizeNpub(const QString &value) {
    static const QRegularExpression npub(QStringLiteral("npub1[a-zA-Z0-9]+"));
    const auto match = npub.match(value);
    return match.hasMatch() ? match.captured().toLower() : value.trimmed().toLower();
}

// XML 1.0 cannot carry most control characters, even escaped, so they are
// dropped from what gets written.
QString xmlSafe(const QString &text) {
    QString safe;
    safe.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar character = text.at(i);
        if (character.isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            safe += character;
            safe += text.at(++i);
        } else if (character.isSurrogate() || character.unicode() == 0xFFFE || character.unicode() == 0xFFFF
                   || (character.unicode() < 0x20 && character != '\t' && character != '\n' && character != '\r')) {
            continue;
        } else {
            safe += character;
        }
    }
    return safe;
}

void writeFeed(QXmlStreamWriter &writer, const Opml::ExportFeed &feed) {
    const bool nostr = feed.type == QLatin1String("NOSTR") || feed.type == QLatin1String("NOSTR_VIDEO");
    const QString text = xmlSafe(feed.title.isEmpty() ? feed.url : feed.title);
    writer.writeEmptyElement("outline");
    writer.writeAttribute("type", nostr ? "nostr" : "rss");
    writer.writeAttribute("text", text);
    writer.writeAttribute("title", text);
    writer.writeAttribute(nostr ? "nostr" : "xmlUrl", xmlSafe(feed.url));
    if (!feed.tags.isEmpty())
        writer.writeAttribute("category", xmlSafe(feed.tags.join(',')));
}
}

namespace Opml {
QList<Outline> parse(const QByteArray &xml, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QList<Outline>();
    };
    if (xml.isEmpty())
        return fail(QStringLiteral("empty OPML"));
    if (xml.size() > maxBytes)
        return fail(QStringLiteral("OPML is larger than %1 MiB").arg(maxBytes / (1024 * 1024)));

    QXmlStreamReader reader(xml);
    while (!reader.atEnd() && !reader.isStartElement()) {
        reader.readNext();
        if (FeedParser::declaresEntities(reader))
            return fail(QStringLiteral("OPML declares XML entities, which are not allowed"));
    }
    if (reader.hasError() || reader.name() != QLatin1String("opml"))
        return fail(reader.hasError() ? reader.errorString() : QStringLiteral("not an OPML file"));
    Walk state;
    walk(reader, {}, 0, state);
    while (!reader.atEnd())
        reader.readNext();
    if (reader.hasError())
        return fail(QStringLiteral("malformed OPML: %1 (line %2)").arg(reader.errorString()).arg(reader.lineNumber()));
    return state.feeds;
}

QByteArray build(const QList<ExportFeed> &feeds) {
    QSet<QString> nostrNpubs;
    for (const ExportFeed &feed : feeds)
        if (feed.type == QLatin1String("NOSTR") && !feed.url.isEmpty())
            nostrNpubs.insert(normalizeNpub(feed.url));

    QList<ExportFeed> uncategorized;
    QStringList folderOrder;
    QHash<QString, QList<ExportFeed>> folders;
    for (const ExportFeed &feed : feeds) {
        if (feed.url.isEmpty())
            continue;
        // A video feed for an npub that is also followed is the same source.
        if (feed.type == QLatin1String("NOSTR_VIDEO") && nostrNpubs.contains(normalizeNpub(feed.url)))
            continue;
        if (feed.category.isEmpty()) {
            uncategorized << feed;
        } else {
            if (!folders.contains(feed.category))
                folderOrder << feed.category;
            folders[feed.category] << feed;
        }
    }

    QByteArray out;
    QXmlStreamWriter writer(&out);
    writer.setAutoFormatting(true);
    writer.setAutoFormattingIndent(2);
    writer.writeStartDocument();
    writer.writeStartElement("opml");
    writer.writeAttribute("version", "2.0");
    writer.writeStartElement("head");
    writer.writeTextElement("title", "Readstr Subscriptions");
    writer.writeEndElement();
    writer.writeStartElement("body");
    for (const ExportFeed &feed : uncategorized)
        writeFeed(writer, feed);
    for (const QString &name : folderOrder) {
        writer.writeStartElement("outline");
        writer.writeAttribute("text", xmlSafe(name));
        writer.writeAttribute("title", xmlSafe(name));
        for (const ExportFeed &feed : folders.value(name))
            writeFeed(writer, feed);
        writer.writeEndElement();
    }
    writer.writeEndDocument();
    return out;
}

Plan plan(const QList<Outline> &parsed, const QStringList &existingUrls, const QStringList &existingNpubs) {
    QSet<QString> urls, npubs;
    for (const QString &url : existingUrls)
        urls.insert(Store::urlKey(url));
    for (const QString &npub : existingNpubs)
        npubs.insert(normalizeNpub(npub));

    Plan result;
    for (const Outline &outline : parsed) {
        const bool nostr = !outline.npub.isEmpty();
        if (!nostr && outline.xmlUrl.isEmpty())
            continue;
        QSet<QString> &seen = nostr ? npubs : urls;
        const QString key = nostr ? normalizeNpub(outline.npub) : Store::urlKey(outline.xmlUrl);
        if (seen.contains(key)) {
            ++result.skipped;
            continue;
        }
        seen.insert(key);
        result.toAdd << outline;
    }
    return result;
}
}
