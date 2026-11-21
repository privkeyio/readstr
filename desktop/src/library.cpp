#include "library.h"

#include <QHash>
#include <QSaveFile>

#include "nostrsource.h"
#include "smallfile.h"

namespace Library {
bool isWebUrl(const QUrl &url) {
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == "http" || scheme == "https") && !url.host().isEmpty();
}

Subscription subscribe(Store &store, const Discovery::Result &found) {
    Subscription subscription;
    if (!found.found) {
        subscription.error = found.error;
        return subscription;
    }
    const QString feedUrl = found.feedUrl.toString(QUrl::FullyEncoded);
    subscription.id = store.addFeed(feedUrl, found.feed.title, {}, &subscription.created);
    if (!subscription.id) {
        subscription.error = store.lastError();
        return subscription;
    }
    if (subscription.created) {
        subscription.added = store.saveItems(subscription.id, found.feed.items);
        if (subscription.added < 0 || !store.updateFeedDetails(subscription.id, found.feed)
            || !store.recordFetch(subscription.id, found.fetch.etag, found.fetch.lastModified, {})) {
            subscription.error = store.lastError();
            store.removeFeed(subscription.id);
            subscription.id = 0;
            subscription.created = false;
            subscription.added = 0;
        }
    }
    return subscription;
}

std::optional<Nip19::Profile> nostrAuthor(const QString &text) {
    return Nip19::decodeProfile(text);
}

bool isNostrFeed(const Feed &feed) {
    return feed.url.startsWith(QLatin1String("npub1"));
}

namespace {
QList<NostrQuery> authorQueries(const QString &pubkey) {
    NostrQuery articles;
    articles.author = pubkey;
    articles.kinds = {NostrSource::profileKind, NostrSource::longFormKind};
    articles.limit = 100;
    NostrQuery videos = articles;
    videos.kinds = {NostrSource::videoKind, NostrSource::shortVideoKind};
    return {articles, videos};
}

// Saves what the relays returned for an author's feed: new and newer
// articles and videos, and the profile when one came.
bool saveNostr(Store &store, qint64 feedId, const QString &pubkey, const NostrResult &articles,
               const NostrResult &videos, int *added) {
    const QList<NostrEvent> events = articles.events + videos.events;
    const auto profile = NostrSource::profile(events, pubkey);
    if (profile && !store.updateProfile(feedId, profile->feed, profile->revision, profile->eventId))
        return false;
    const auto feed = store.feed(feedId);
    if (!feed)
        return false;
    const QString author = feed->title != feed->url ? feed->title : QString();
    *added = store.saveItems(feedId, NostrSource::articles(events, author));
    if (*added < 0 || !store.recordFetch(feedId, {}, {}, {}))
        return false;
    // A refresh that some relay missed, for either query, asks again from
    // the last one they all answered, so what only that relay holds is not
    // skipped for good. The earlier start covers both.
    if (!articles.complete() || !videos.complete())
        return true;
    return store.recordComplete(feedId, qMin(articles.startedAt, videos.startedAt));
}
}

std::optional<std::pair<NostrResult, NostrResult>> NostrPairs::take(qsizetype index, const NostrResult &result) {
    const auto other = m_waiting.constFind(index ^ 1);
    if (other == m_waiting.cend()) {
        m_waiting.insert(index, result);
        return std::nullopt;
    }
    std::pair<NostrResult, NostrResult> both = index % 2 ? std::pair(*other, result) : std::pair(result, *other);
    m_waiting.erase(other);
    return both;
}

QList<NostrQuery> nostrQueries(const Feed &feed) {
    const auto author = Nip19::decodeProfile(feed.url);
    QList<NostrQuery> queries = authorQueries(author ? author->pubkey : QString());
    for (NostrQuery &query : queries) {
        query.extraRelays = feed.relayHints;
        // A day's margin for relays' clocks and for events that reach them late.
        if (feed.completeAt.isValid())
            query.since = qMax<qint64>(0, feed.completeAt.toSecsSinceEpoch() - 24 * 3600);
    }
    return queries;
}

QList<NostrQuery> nostrQueries(const Nip19::Profile &author) {
    QList<NostrQuery> queries = authorQueries(author.pubkey);
    for (NostrQuery &query : queries)
        query.extraRelays = author.relays;
    return queries;
}

Subscription subscribeNostr(Store &store, const Nip19::Profile &author, const NostrResult &articles,
                            const NostrResult &videos) {
    Subscription subscription;
    if (!articles.ok()) {
        subscription.error = articles.error;
        return subscription;
    }
    const QString npub = Nip19::npub(author.pubkey);
    // Titled by its npub, a placeholder the profile's name replaces.
    subscription.id = store.addFeed(npub, npub, {}, &subscription.created);
    if (!subscription.id) {
        subscription.error = store.lastError();
        return subscription;
    }
    if (subscription.created
        && (!store.setRelayHints(subscription.id, author.relays)
            || !saveNostr(store, subscription.id, author.pubkey, articles, videos, &subscription.added))) {
        subscription.error = store.lastError();
        store.removeFeed(subscription.id);
        subscription.id = 0;
        subscription.created = false;
        subscription.added = 0;
    }
    return subscription;
}

Refresh applyNostr(Store &store, const Feed &feed, const NostrResult &articles, const NostrResult &videos) {
    Refresh row;
    row.title = feed.title;
    const auto author = Nip19::decodeProfile(feed.url);
    if (!author) {
        row.error = QStringLiteral("not an npub: %1").arg(feed.url);
    } else if (!articles.ok()) {
        row.error = articles.error;
    } else {
        row.status = QStringLiteral("updated");
        if (!saveNostr(store, feed.id, author->pubkey, articles, videos, &row.added))
            row.error = store.lastError();
        if (const auto updated = store.feed(feed.id))
            row.title = updated->title;
    }
    if (!row.error.isEmpty()) {
        row.status = QStringLiteral("error");
        row.added = 0;
        store.recordFetch(feed.id, {}, {}, row.error);
    }
    return row;
}

Refresh apply(Store &store, const Feed &feed, const FetchResult &fetched) {
    Refresh row;
    row.title = feed.title;
    if (!fetched.ok()) {
        row.error = fetched.error;
    } else if (fetched.notModified) {
        row.status = QStringLiteral("not-modified");
        // A 304 may leave out the validators; the stored ones still hold.
        if (!store.recordFetch(feed.id, fetched.etag.isEmpty() ? feed.etag : fetched.etag,
                               fetched.lastModified.isEmpty() ? feed.lastModified : fetched.lastModified, {}))
            row.error = store.lastError();
    } else if (const auto parsed = FeedParser::parse(fetched.body, fetched.finalUrl, &row.error)) {
        row.status = QStringLiteral("updated");
        row.added = store.saveItems(feed.id, parsed->items);
        if (row.added < 0 || !store.updateFeedDetails(feed.id, *parsed)
            || !store.recordFetch(feed.id, fetched.etag, fetched.lastModified, {}))
            row.error = store.lastError();
        if (const auto updated = store.feed(feed.id))
            row.title = updated->title;
    }
    if (!row.error.isEmpty()) {
        row.status = QStringLiteral("error");
        row.added = 0;
        store.recordFetch(feed.id, {}, {}, row.error);
    }
    return row;
}

std::optional<QList<Opml::Outline>> readOpml(const QString &path, QString *error) {
    const QByteArray data = readSmallFile(path, Opml::maxBytes);
    if (data.isNull()) {
        *error = QStringLiteral("cannot read %1 (missing, not a regular file, or larger than %2 MiB)")
                     .arg(path)
                     .arg(Opml::maxBytes / (1024 * 1024));
        return std::nullopt;
    }
    QString parseError;
    const QList<Opml::Outline> outlines = Opml::parse(data, &parseError);
    if (!parseError.isEmpty()) {
        *error = parseError;
        return std::nullopt;
    }
    return outlines;
}

Import importFile(Store &store, const QString &path) {
    Import result;
    const auto outlines = readOpml(path, &result.error);
    return outlines ? importOutlines(store, *outlines) : result;
}

Import importOutlines(Store &store, const QList<Opml::Outline> &outlines) {
    Import result;
    // Unsupported outlines are set aside before de-duplication, so one
    // cannot shadow a supported one with the same key. An nprofile or a
    // "nostr:" link is stored as the plain npub, and an nprofile's relay
    // hints are kept for the feed, from the first outline of each author as
    // the plan keeps that one.
    QList<Opml::Outline> supported;
    QHash<QString, QStringList> hints;
    for (Opml::Outline outline : outlines) {
        if (!outline.npub.isEmpty()) {
            const auto author = Nip19::decodeProfile(outline.npub);
            if (!author) {
                ++result.unsupported;
                continue;
            }
            outline.npub = Nip19::npub(author->pubkey);
            outline.xmlUrl = outline.npub;
            if (!hints.contains(outline.npub))
                hints.insert(outline.npub, author->relays);
            supported << outline;
        } else if (isWebUrl(QUrl(outline.xmlUrl, QUrl::StrictMode))) {
            supported << outline;
        } else {
            ++result.unsupported;
        }
    }

    // One transaction, so a failure part way leaves no feeds behind that
    // nothing would fetch, and the subscriptions it reads cannot change
    // before it writes.
    const bool saved = store.transaction([&] {
        const QList<Feed> feeds = store.feeds();
        if (!store.lastError().isEmpty())
            return false;
        QStringList existing, npubs;
        for (const Feed &feed : feeds)
            (isNostrFeed(feed) ? npubs : existing) << feed.url;
        const Opml::Plan plan = Opml::plan(supported, existing, npubs);
        result.skipped = plan.skipped;
        for (const Opml::Outline &outline : plan.toAdd) {
            bool created = false;
            const qint64 id = store.addFeed(outline.xmlUrl, outline.title, outline.folder, &created);
            if (!id)
                return false;
            const QStringList relays = hints.value(outline.npub);
            if (created && !relays.isEmpty() && !store.setRelayHints(id, relays))
                return false;
            result.added += created;
        }
        return true;
    });
    if (!saved) {
        result = Import{};
        result.error = store.lastError();
    }
    return result;
}

QByteArray exportOpml(Store &store, QString *error) {
    const QList<Feed> feeds = store.feeds();
    if (!store.lastError().isEmpty()) {
        if (error)
            *error = store.lastError();
        return {};
    }
    QList<Opml::ExportFeed> exported;
    for (const Feed &feed : feeds)
        exported << Opml::ExportFeed{isNostrFeed(feed) ? QStringLiteral("NOSTR") : QStringLiteral("RSS"), feed.url,
                                     feed.title, feed.category, {}};
    return Opml::build(exported);
}

bool writePrivately(const QString &path, const QByteArray &data, QString *error) {
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        && file.write(data) == data.size() && file.commit())
        return true;
    if (error)
        *error = QStringLiteral("cannot write %1: %2").arg(file.fileName(), file.errorString());
    return false;
}
}
