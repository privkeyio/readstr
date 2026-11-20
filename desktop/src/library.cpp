#include "library.h"

#include <QSaveFile>

#include "opml.h"
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

Import importFile(Store &store, const QString &path) {
    Import result;
    const QByteArray data = readSmallFile(path, Opml::maxBytes);
    if (data.isNull()) {
        result.error = QStringLiteral("cannot read %1 (missing, not a regular file, or larger than %2 MiB)")
                           .arg(path)
                           .arg(Opml::maxBytes / (1024 * 1024));
        return result;
    }
    const QList<Opml::Outline> outlines = Opml::parse(data, &result.error);
    if (!result.error.isEmpty())
        return result;

    // Nostr sources arrive in phase 2; until then they are counted, not
    // dropped silently. They are set aside before de-duplication, so an
    // unsupported outline cannot shadow a supported one with the same key.
    QList<Opml::Outline> supported;
    for (const Opml::Outline &outline : outlines) {
        if (!outline.npub.isEmpty() || !isWebUrl(QUrl(outline.xmlUrl, QUrl::StrictMode)))
            ++result.unsupported;
        else
            supported << outline;
    }

    const QList<Feed> feeds = store.feeds();
    if (!store.lastError().isEmpty()) {
        result.error = store.lastError();
        return result;
    }
    QStringList existing;
    for (const Feed &feed : feeds)
        existing << feed.url;
    const Opml::Plan plan = Opml::plan(supported, existing);
    result.skipped = plan.skipped;
    for (const Opml::Outline &outline : plan.toAdd) {
        bool created = false;
        if (!store.addFeed(outline.xmlUrl, outline.title, outline.folder, &created)) {
            result.error = store.lastError();
            return result;
        }
        result.added += created;
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
        exported << Opml::ExportFeed{QStringLiteral("RSS"), feed.url, feed.title, feed.category, {}};
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
