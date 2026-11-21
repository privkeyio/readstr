#include "nostrsource.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QUrl>
#include <algorithm>
#include <cmath>

#include "fetcher.h"
#include "nip19.h"
#include "sanitizer.h"

namespace {
QString textField(const QJsonObject &object, const char *key, int maxLength) {
    return FeedParser::cleanText(object.value(QLatin1String(key)).toString().left(maxLength * 2)).left(maxLength);
}

QString webLink(const QString &text) {
    const QUrl url(text, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == "http" || scheme == "https") && !url.host().isEmpty()
        ? url.toString(QUrl::FullyEncoded)
        : QString();
}

// A first-publication date in the future or before Nostr existed is the
// author's mistake; the event's own time is the better guess.
QDateTime publishedAt(const NostrEvent &event) {
    bool ok = false;
    const qint64 published = event.tag(QStringLiteral("published_at")).toLongLong(&ok);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    return QDateTime::fromSecsSinceEpoch(ok && published > 0 && published <= now + 86400 ? published
                                                                                        : event.createdAt,
                                         QTimeZone::UTC);
}

// An http(s) media URL, encoded, or empty when it is not one, is too long,
// or names this machine or the local network: someone else's event must
// not have the player, or the thumbnail fetch, ask the user's router.
QString mediaLink(const QString &text) {
    const QUrl url = Sanitizer::webUrl(text, QUrl());
    if (url.isEmpty() || Fetcher::isLocalHost(url.host()))
        return {};
    const QString encoded = url.toString(QUrl::FullyEncoded);
    return encoded.size() > NostrSource::maxMediaUrlLength ? QString() : encoded;
}

// The duration an imeta tag gives, in seconds, or -1 when it gives none that
// is believable: a finite number above 0 and at most maxDuration. Zero counts
// as unknown, since clients write it when they have no length to give.
double durationOf(const QString &text) {
    bool ok = false;
    const double duration = text.toDouble(&ok);
    return ok && std::isfinite(duration) && duration > 0 && duration <= NostrSource::maxDuration ? duration : -1;
}

// Newest first, and at most FeedParser::maxItems of them.
QList<ParsedItem> newestFirst(QList<ParsedItem> items) {
    std::sort(items.begin(), items.end(),
              [](const ParsedItem &a, const ParsedItem &b) { return a.published > b.published; });
    if (items.size() > FeedParser::maxItems)
        items.resize(FeedParser::maxItems);
    return items;
}

// A NIP-92 imeta tag's "key value" entries; the first of a key wins.
QHash<QString, QString> imetaEntries(const QStringList &tag) {
    QHash<QString, QString> entries;
    for (qsizetype i = 1; i < tag.size(); ++i) {
        const QString entry = tag.at(i).trimmed();
        const qsizetype space = entry.indexOf(' ');
        if (space <= 0)
            continue;
        const QString key = entry.left(space);
        if (!entries.contains(key))
            entries.insert(key, entry.mid(space + 1).trimmed());
    }
    return entries;
}
}

namespace NostrSource {
std::optional<Profile> profile(const QList<NostrEvent> &events, const QString &pubkey) {
    const NostrEvent *latest = nullptr;
    for (const NostrEvent &event : events)
        if (event.kind == profileKind && event.pubkey == pubkey && (!latest || event.isNewerThan(*latest)))
            latest = &event;
    if (!latest || latest->content.size() > maxProfileBytes)
        return std::nullopt;
    // An empty object still counts: it is the author clearing their details.
    const QJsonDocument document = QJsonDocument::fromJson(latest->content.toUtf8());
    if (!document.isObject())
        return std::nullopt;
    const QJsonObject object = document.object();
    ParsedFeed feed;
    feed.title = textField(object, "display_name", 200);
    if (feed.title.isEmpty())
        feed.title = textField(object, "name", 200);
    feed.description = FeedParser::summarize(textField(object, "about", 4 * FeedParser::maxSummaryLength));
    feed.siteUrl = webLink(object.value("website").toString().trimmed());
    if (feed.siteUrl.isEmpty())
        feed.siteUrl = QStringLiteral("https://njump.me/") + Nip19::npub(pubkey);
    return Profile{feed, latest->createdAt, latest->id};
}

QList<ParsedItem> longForm(const QList<NostrEvent> &events, const QString &author) {
    QHash<QString, const NostrEvent *> latest;
    for (const NostrEvent &event : events) {
        if (event.kind != longFormKind)
            continue;
        const QString key = event.pubkey + ':' + event.tag(QStringLiteral("d"));
        const NostrEvent *&slot = latest[key];
        if (!slot || event.isNewerThan(*slot))
            slot = &event;
    }

    QList<ParsedItem> items;
    for (const NostrEvent *event : std::as_const(latest)) {
        const QString d = event->tag(QStringLiteral("d"));
        ParsedItem item;
        item.guid = event->id;
        item.title = FeedParser::cleanText(event->tag(QStringLiteral("title")).left(1000));
        if (item.title.isEmpty())
            item.title = QStringLiteral("Untitled");
        item.author = author;
        item.content = event->content;
        const QString summary = FeedParser::cleanText(event->tag(QStringLiteral("summary")).left(4000));
        item.summary = FeedParser::summarize(summary.isEmpty() ? FeedParser::cleanText(event->content.left(4000))
                                                               : summary);
        item.published = publishedAt(*event);
        const QString naddr = Nip19::naddr(longFormKind, event->pubkey, d);
        if (!naddr.isEmpty())
            item.url = QStringLiteral("https://njump.me/") + naddr;
        item.address = QStringLiteral("%1:%2:%3").arg(longFormKind).arg(event->pubkey, d);
        item.revision = event->createdAt;
        items << item;
    }
    return newestFirst(std::move(items));
}

QList<ParsedItem> videos(const QList<NostrEvent> &events, const QString &author) {
    QList<ParsedItem> items;
    for (const NostrEvent &event : events) {
        if (event.kind != videoKind && event.kind != shortVideoKind)
            continue;
        ParsedItem item;
        item.guid = event.id;
        item.title = FeedParser::cleanText(event.tag(QStringLiteral("title")).left(1000));
        if (item.title.isEmpty())
            item.title = QStringLiteral("Untitled Video");
        item.author = author;
        // Plain text, not Markdown: shown as written, its line breaks kept.
        item.content = Sanitizer::linesToRichText(event.content);
        item.summary = FeedParser::summarize(FeedParser::cleanText(event.content.left(4000)));
        item.published = publishedAt(event);
        const QString nevent = Nip19::nevent(event.id, event.pubkey, event.kind);
        if (!nevent.isEmpty())
            item.url = QStringLiteral("https://njump.me/") + nevent;

        QString anyImage;
        bool chosen = false;
        for (const QStringList &tag : event.tags) {
            if (tag.value(0) != QLatin1String("imeta"))
                continue;
            const QHash<QString, QString> entries = imetaEntries(tag);
            const QString image = mediaLink(entries.value(QStringLiteral("image")));
            if (anyImage.isEmpty())
                anyImage = image;
            if (chosen)
                continue;
            const QString media = mediaLink(entries.value(QStringLiteral("url")));
            const QString type = entries.value(QStringLiteral("m")).toLower();
            if (media.isEmpty() || (!type.isEmpty() && !type.startsWith(QLatin1String("video/"))))
                continue;
            chosen = true;
            item.mediaUrl = media;
            item.thumbnailUrl = image;
            item.duration = durationOf(entries.value(QStringLiteral("duration")));
        }
        if (item.thumbnailUrl.isEmpty())
            item.thumbnailUrl = anyImage;
        items << item;
    }
    return newestFirst(std::move(items));
}

QList<ParsedItem> articles(const QList<NostrEvent> &events, const QString &author) {
    return newestFirst(longForm(events, author) + videos(events, author));
}
}
