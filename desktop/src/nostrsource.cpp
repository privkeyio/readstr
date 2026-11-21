#include "nostrsource.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QUrl>
#include <algorithm>

#include "nip19.h"

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
        bool ok = false;
        const qint64 published = event->tag(QStringLiteral("published_at")).toLongLong(&ok);
        // A first-publication date in the future or before Nostr existed is
        // the author's mistake; the event's own time is the better guess.
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        item.published = QDateTime::fromSecsSinceEpoch(ok && published > 0 && published <= now + 86400
                                                           ? published
                                                           : event->createdAt,
                                                       QTimeZone::UTC);
        const QString naddr = Nip19::naddr(longFormKind, event->pubkey, d);
        if (!naddr.isEmpty())
            item.url = QStringLiteral("https://njump.me/") + naddr;
        item.address = QStringLiteral("%1:%2:%3").arg(longFormKind).arg(event->pubkey, d);
        item.revision = event->createdAt;
        items << item;
    }
    std::sort(items.begin(), items.end(),
              [](const ParsedItem &a, const ParsedItem &b) { return a.published > b.published; });
    if (items.size() > FeedParser::maxItems)
        items.resize(FeedParser::maxItems);
    return items;
}
}
