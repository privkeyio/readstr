#include "sync.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <cmath>

#include "feedparser.h"
#include "fetcher.h"
#include "library.h"
#include "nip19.h"

namespace Sync {
std::optional<SyncAccount> account(const QString &text) {
    const auto profile = Nip19::decodeProfile(text.trimmed());
    if (!profile)
        return std::nullopt;
    SyncAccount account;
    account.pubkey = profile->pubkey;
    account.npub = Nip19::npub(profile->pubkey);
    account.relays = profile->relays;
    return account;
}

NostrQuery query(const SyncAccount &account) {
    NostrQuery query;
    query.author = account.pubkey;
    query.kinds = {subscriptionsKind, readStatusKind, viewsKind};
    // Only the pairs the web app publishes, so an event of one kind with
    // another's d tag cannot fill the byte cap and push the real one out.
    query.dTags = {{subscriptionsKind, QString::fromLatin1(subscriptionsTag)},
                   {readStatusKind, QString::fromLatin1(readStatusTag)},
                   {viewsKind, QString::fromLatin1(viewsTag)}};
    // No small limit: a relay that keeps old versions sends them too, newest
    // first, and the pool keeps only the newest of each.
    query.limit = 100;
    query.extraRelays = account.relays;
    query.maxContentLength = maxContentLength;
    return query;
}

RelayPool::Limits limits() {
    RelayPool::Limits limits;
    // UTF-8 takes at most three bytes per UTF-16 unit of content.
    limits.maxMessageBytes = 3 * maxContentLength + 256 * 1024;
    limits.maxBytesPerQuery = 3 * 2 * (maxContentLength + NostrEvent::maxTagCharacters + 4096);
    return limits;
}

namespace {
// Schema checks in the web app's zod terms: a field that is present must
// have the right type and fit its caps, or the whole payload is refused.
bool isString(const QJsonValue &value, int maxLength) {
    return value.isString() && value.toString().size() <= maxLength;
}

bool optionalString(const QJsonObject &object, const char *key, int maxLength) {
    return !object.contains(QLatin1String(key)) || isString(object.value(QLatin1String(key)), maxLength);
}

bool stringArray(const QJsonValue &value, int maxCount, int maxLength, QStringList *out = nullptr) {
    if (!value.isArray())
        return false;
    const QJsonArray array = value.toArray();
    if (array.size() > maxCount)
        return false;
    for (const QJsonValue &item : array) {
        if (!isString(item, maxLength))
            return false;
        if (out)
            *out << item.toString();
    }
    return true;
}

bool optionalStringArray(const QJsonObject &object, const char *key, int maxCount, int maxLength) {
    return !object.contains(QLatin1String(key)) || stringArray(object.value(QLatin1String(key)), maxCount, maxLength);
}

bool lastUpdatedFits(const QJsonObject &object) {
    if (!object.contains(QLatin1String("lastUpdated")))
        return true;
    const QJsonValue value = object.value(QLatin1String("lastUpdated"));
    // zod's int() is a safe integer, and checking that first keeps the cast
    // defined, which it is not for a double past qint64's range.
    const double number = value.toDouble();
    return value.isDouble() && number >= 0 && number <= 9007199254740991.0 && std::trunc(number) == number;
}

std::optional<QJsonObject> parseObject(const QString &content) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(content.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    return document.object();
}

struct SubscriptionList {
    QStringList rss;
    QStringList nostr;
    // Feed (as listed) to category name.
    QHash<QString, QString> categories;
};

std::optional<SubscriptionList> parseSubscriptions(const QString &content) {
    const auto object = parseObject(content);
    if (!object)
        return std::nullopt;
    SubscriptionList list;
    if (!stringArray(object->value("rss"), maxFeeds, maxUrlLength, &list.rss)
        || !stringArray(object->value("nostr"), maxFeeds, maxUrlLength, &list.nostr)
        || !optionalStringArray(*object, "deleted", maxFeeds, maxUrlLength) || !lastUpdatedFits(*object))
        return std::nullopt;
    if (object->contains("tags")) {
        const QJsonValue tags = object->value("tags");
        if (!tags.isObject() || tags.toObject().size() > maxFeeds)
            return std::nullopt;
        const QJsonObject map = tags.toObject();
        for (auto it = map.begin(); it != map.end(); ++it)
            if (it.key().size() > maxUrlLength || !stringArray(it.value(), maxTagsPerFeed, 256))
                return std::nullopt;
    }
    if (object->contains("categories")) {
        const QJsonValue categories = object->value("categories");
        if (!categories.isObject() || categories.toObject().size() > maxFeeds)
            return std::nullopt;
        const QJsonObject map = categories.toObject();
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (it.key().size() > maxUrlLength || !it.value().isObject())
                return std::nullopt;
            const QJsonObject category = it.value().toObject();
            if (!isString(category.value("name"), 256) || !optionalString(category, "color", 64)
                || !optionalString(category, "icon", 256))
                return std::nullopt;
            list.categories.insert(it.key(), category.value("name").toString());
        }
    }
    return list;
}

std::optional<QStringList> parseReadStatus(const QString &content) {
    const auto object = parseObject(content);
    QStringList guids;
    if (!object || !stringArray(object->value("itemGuids"), maxReadGuids, maxGuidLength, &guids)
        || !lastUpdatedFits(*object))
        return std::nullopt;
    return guids;
}

// The web app's syncViewSchema, which lets unknown fields through.
bool viewFits(const QJsonValue &value) {
    if (!value.isObject())
        return false;
    const QJsonObject view = value.toObject();
    if (!isString(view.value("name"), 64) || !optionalString(view, "id", 256) || !optionalString(view, "icon", 8)
        || !optionalString(view, "readState", 16) || !optionalString(view, "sort", 16)
        || !optionalString(view, "organizationMode", 16))
        return false;
    if (view.contains("order") && !view.value("order").isDouble())
        return false;
    if (view.contains("source")) {
        if (!view.value("source").isObject())
            return false;
        const QJsonObject source = view.value("source").toObject();
        if (!isString(source.value("kind"), 32) || !optionalString(source, "feedId", 256)
            || !optionalString(source, "categoryId", 256) || !optionalStringArray(source, "tags", maxFeeds, 256))
            return false;
    }
    if (view.contains("keywords")) {
        if (!view.value("keywords").isObject())
            return false;
        const QJsonObject keywords = view.value("keywords").toObject();
        if (!optionalStringArray(keywords, "include", maxFeeds, 256)
            || !optionalStringArray(keywords, "exclude", maxFeeds, 256))
            return false;
    }
    return true;
}

// sanitizeStrings in src/lib/saved-views.ts: strings only, each cut to 200
// characters, empty ones dropped, at most 50.
QJsonArray sanitizeStrings(const QJsonValue &value) {
    QJsonArray out;
    for (const QJsonValue &item : value.toArray()) {
        if (out.size() >= 50)
            break;
        const QString text = item.toString().left(200);
        if (!text.isEmpty())
            out << text;
    }
    return out;
}

QJsonObject sanitizeSource(const QJsonValue &value) {
    const QJsonObject source = value.toObject();
    const QString kind = source.value("kind").toString();
    const QJsonObject all{{"kind", "all"}};
    if (kind == QLatin1String("feed")) {
        const QString feedId = source.value("feedId").toString();
        return feedId.isEmpty() ? all : QJsonObject{{"kind", kind}, {"feedId", feedId}};
    }
    if (kind == QLatin1String("tags")) {
        const QJsonArray tags = sanitizeStrings(source.value("tags"));
        return tags.isEmpty() ? all : QJsonObject{{"kind", kind}, {"tags", tags}};
    }
    if (kind == QLatin1String("category")) {
        const QString categoryId = source.value("categoryId").toString();
        return categoryId.isEmpty() ? all : QJsonObject{{"kind", kind}, {"categoryId", categoryId}};
    }
    if (kind == QLatin1String("favorites"))
        return QJsonObject{{"kind", kind}};
    return all;
}

// The web app's sanitizeView gives a view without an id a random one, which
// it then publishes, so its next event carries it. The desktop never
// publishes, so a random id here would be new on every newer event and the
// view would pile up as copies; one derived from the view's place and name
// stays the same while the event does not change them.
QString derivedViewId(qsizetype index, const QString &name) {
    const QByteArray hash = QCryptographicHash::hash(QString::number(index).toUtf8() + '\n' + name.toUtf8(),
                                                     QCryptographicHash::Sha256);
    return QStringLiteral("synced-") + QString::fromLatin1(hash.toHex().left(16));
}

// sanitizeView in src/lib/saved-views.ts, less keywords.exclude, which a
// relay never gets to set here. `index` is the view's place in the event.
std::optional<SavedView> sanitizeView(const QJsonObject &raw, qsizetype index) {
    const QString name = raw.value("name").toString().left(60).trimmed();
    if (name.isEmpty())
        return std::nullopt;
    QString id = raw.value("id").toString();
    if (id.isEmpty())
        id = derivedViewId(index, name);
    static const QStringList readStates{"all", "unread", "read"}, sorts{"newest", "oldest"},
        organizationModes{"tags", "categories"};
    const QString readState = raw.value("readState").toString(), sort = raw.value("sort").toString();
    const double order = raw.value("order").toDouble();
    QJsonObject view{{"id", id},
                     {"name", name},
                     {"order", order},
                     {"source", sanitizeSource(raw.value("source"))},
                     {"readState", readStates.contains(readState) ? readState : QStringLiteral("all")},
                     {"sort", sorts.contains(sort) ? sort : QStringLiteral("newest")}};
    const QString icon = raw.value("icon").toString();
    if (!icon.isEmpty())
        view.insert("icon", icon.left(8));
    const QJsonArray include = sanitizeStrings(raw.value("keywords").toObject().value("include"));
    if (!include.isEmpty())
        view.insert("keywords", QJsonObject{{"include", include}});
    const QString organizationMode = raw.value("organizationMode").toString();
    if (organizationModes.contains(organizationMode))
        view.insert("organizationMode", organizationMode);
    return SavedView{id, view, {}};
}

// normalizeViews: the first maxViews, in order, numbered again from 0.
QList<SavedView> normalize(QList<SavedView> views) {
    if (views.size() > maxViews)
        views.resize(maxViews);
    std::stable_sort(views.begin(), views.end(), [](const SavedView &a, const SavedView &b) {
        return a.view.value("order").toDouble() < b.view.value("order").toDouble();
    });
    for (qsizetype i = 0; i < views.size(); ++i)
        views[i].view.insert("order", int(i));
    return views;
}

std::optional<QList<SavedView>> parseViews(const QString &content) {
    const auto object = parseObject(content);
    if (!object || !object->value("views").isArray() || !lastUpdatedFits(*object))
        return std::nullopt;
    const QJsonArray array = object->value("views").toArray();
    if (array.size() > maxRemoteViews)
        return std::nullopt;
    for (const QJsonValue &value : array)
        if (!viewFits(value))
            return std::nullopt;
    QList<SavedView> views;
    for (qsizetype i = 0; i < qMin<qsizetype>(array.size(), maxViews); ++i)
        if (const auto view = sanitizeView(array.at(i).toObject(), i))
            views << *view;
    return normalize(views);
}

// mergeViewLists: the remote views first, keeping each one's local mute
// words, then the local ones the remote lacks, since deletions do not sync.
QList<SavedView> mergeViews(const QList<SavedView> &local, const QList<SavedView> &remote) {
    QHash<QString, QStringList> excludes;
    for (const SavedView &view : local)
        excludes.insert(view.id, view.exclude);
    QSet<QString> seen;
    QList<SavedView> merged;
    for (SavedView view : remote) {
        if (seen.contains(view.id))
            continue;
        seen.insert(view.id);
        view.exclude = excludes.value(view.id);
        merged << view;
    }
    for (const SavedView &view : local) {
        if (seen.contains(view.id))
            continue;
        seen.insert(view.id);
        merged << view;
    }
    return normalize(merged);
}

// Newer than the last event applied in the order the pool picks versions
// by, so a tie in created_at goes to the lower id, and the same event
// replayed is never newer than itself.
bool isFresh(const SyncAccount &account, const char *dTag, const NostrEvent &event) {
    const QString key = QString::fromLatin1(dTag);
    const auto applied = account.applied.constFind(key);
    if (applied == account.applied.cend())
        return true;
    NostrEvent last;
    last.createdAt = *applied;
    last.id = account.appliedIds.value(key);
    return event.isNewerThan(last);
}

QString tr(const char *text) { return QCoreApplication::translate("Sync", text); }
}

Outcome apply(Store &store, const SyncAccount &account, const NostrResult &result,
              const std::function<void()> &beforeWrites) {
    Outcome outcome;
    if (!result.ok()) {
        outcome.error = result.error;
        return outcome;
    }
    // The account may have changed (in the window or from the command
    // line) while the relays were asked, and what came is not the new one's.
    const SyncAccount current = store.syncAccount();
    if (!store.lastError().isEmpty()) {
        outcome.error = store.lastError();
        return outcome;
    }
    const auto npubChanged = [&] {
        outcome.error = tr("the npub changed during the pull");
        return outcome;
    };
    if (current.pubkey != account.pubkey)
        return npubChanged();
    // The pool kept the newest version per kind and d tag; each kind must
    // come with its own d tag, as the web app asks for them in pairs.
    const NostrEvent *subscriptions = nullptr, *readStatus = nullptr, *views = nullptr;
    for (const NostrEvent &event : result.events) {
        if (event.pubkey != account.pubkey)
            continue;
        const QString dTag = event.tag(QStringLiteral("d"));
        if (event.kind == subscriptionsKind && dTag == QLatin1String(subscriptionsTag))
            subscriptions = &event;
        else if (event.kind == readStatusKind && dTag == QLatin1String(readStatusTag))
            readStatus = &event;
        else if (event.kind == viewsKind && dTag == QLatin1String(viewsTag))
            views = &event;
    }

    if (beforeWrites)
        beforeWrites();

    bool refused = false;
    const auto refuse = [&](int kind) {
        refused = true;
        if (kind == subscriptionsKind)
            outcome.problems << tr("the subscription list does not fit the web app's format");
        else if (kind == readStatusKind)
            outcome.problems << tr("the read status does not fit the web app's format");
        else
            outcome.problems << tr("the saved views do not fit the web app's format");
    };

    const auto storeFailed = [&] {
        outcome.error = store.lastError();
        return outcome;
    };
    // Another writer (the window and `readstr sync` on one library) may
    // apply a newer event, or the account be stopped and set again, while
    // this pull runs, so each kind is judged against the account as it is
    // now: once before its event is parsed, which also decides whether a
    // payload that does not fit is reported, and again inside the write's
    // own transaction. False on a store failure.
    bool changed = false;
    const auto stillFresh = [&](const char *dTag, const NostrEvent &event, bool emptyLibrary, bool *fresh) {
        const SyncAccount now = store.syncAccount();
        if (!store.lastError().isEmpty())
            return false;
        changed = now.pubkey != account.pubkey;
        *fresh = !changed && (emptyLibrary || isFresh(now, dTag, event));
        return true;
    };

    if (subscriptions) {
        const QList<Feed> feeds = store.feeds();
        if (!store.lastError().isEmpty())
            return storeFailed();
        // An empty library has nothing for an old list to roll back, so it
        // takes the list whatever the watermark says, as the web app does.
        bool fresh = false;
        if (!stillFresh(subscriptionsTag, *subscriptions, feeds.isEmpty(), &fresh))
            return storeFailed();
        if (changed)
            return npubChanged();
        if (fresh) {
            if (const auto list = parseSubscriptions(subscriptions->content)) {
                // The web app adds what is missing and never applies the
                // `deleted` list, so neither does this.
                QList<Opml::Outline> outlines;
                int local = 0;
                for (const QString &url : list->rss) {
                    // Whoever holds the npub's key picks these URLs, and the
                    // window fetches a new feed at once, so one on this
                    // machine or the local network would let them send
                    // requests there. OPML import keeps them, since the user
                    // picked that file.
                    if (Fetcher::isLocalHost(QUrl(url, QUrl::StrictMode).host())) {
                        ++local;
                        continue;
                    }
                    outlines << Opml::Outline{{}, url, {}, FeedParser::cleanText(list->categories.value(url)), {}};
                }
                for (const QString &npub : list->nostr)
                    outlines << Opml::Outline{{}, {}, npub, FeedParser::cleanText(list->categories.value(npub)), {}};
                const Library::Import imported = Library::importOutlines(store, outlines, [&](bool *proceed) {
                    const QList<Feed> now = store.feeds();
                    if (!store.lastError().isEmpty()
                        || !stillFresh(subscriptionsTag, *subscriptions, now.isEmpty(), proceed))
                        return false;
                    return !*proceed
                        || store.setSyncApplied(QString::fromLatin1(subscriptionsTag), subscriptions->createdAt,
                                                subscriptions->id);
                });
                if (!imported.error.isEmpty()) {
                    outcome.error = imported.error;
                    return outcome;
                }
                if (changed)
                    return npubChanged();
                if (!imported.stopped) {
                    outcome.added = imported.added;
                    outcome.skipped = imported.skipped;
                    outcome.unsupported = imported.unsupported + local;
                }
            } else {
                // Not marked applied, as on the web, so every sync says so
                // until a list that fits replaces it.
                refuse(subscriptionsKind);
            }
        }
    }

    const auto freshNow = [&](const char *dTag, const NostrEvent *event, bool *fresh) {
        *fresh = false;
        return !event || stillFresh(dTag, *event, false, fresh);
    };
    bool readStatusFresh = false;
    if (!freshNow(readStatusTag, readStatus, &readStatusFresh))
        return storeFailed();
    if (changed)
        return npubChanged();
    if (readStatusFresh) {
        if (const auto guids = parseReadStatus(readStatus->content)) {
            const bool saved = store.transaction([&] {
                bool fresh = false;
                if (!stillFresh(readStatusTag, *readStatus, false, &fresh))
                    return false;
                if (!fresh)
                    return true;
                outcome.markedRead = store.applyReadGuids(*guids);
                return outcome.markedRead >= 0
                    && store.setSyncApplied(QString::fromLatin1(readStatusTag), readStatus->createdAt, readStatus->id);
            });
            if (!saved) {
                outcome.markedRead = 0;
                return storeFailed();
            }
            if (changed)
                return npubChanged();
        } else {
            refuse(readStatusKind);
        }
    }

    bool viewsFresh = false;
    if (!freshNow(viewsTag, views, &viewsFresh))
        return storeFailed();
    if (changed)
        return npubChanged();
    if (viewsFresh) {
        if (const auto remote = parseViews(views->content)) {
            const bool saved = store.transaction([&] {
                bool fresh = false;
                if (!stillFresh(viewsTag, *views, false, &fresh))
                    return false;
                if (!fresh)
                    return true;
                const QList<SavedView> local = store.savedViews();
                return store.lastError().isEmpty() && store.replaceSavedViews(mergeViews(local, *remote))
                    && store.setSyncApplied(QString::fromLatin1(viewsTag), views->createdAt, views->id);
            });
            if (!saved)
                return storeFailed();
            if (changed)
                return npubChanged();
        } else {
            refuse(viewsKind);
        }
    }

    // A kind whose only versions were past the size cap is refused as one
    // that does not fit, since the web app refuses it the same way, but only
    // when the newest of them would have been applied had it fit: one no
    // newer than what is already applied is obsolete, as it is for a payload
    // that fits, and must not fail every pull.
    for (const NostrEvent &event : result.oversized) {
        if (event.pubkey != account.pubkey)
            continue;
        const QString dTag = event.tag(QStringLiteral("d"));
        const char *tag = nullptr;
        if (event.kind == subscriptionsKind && dTag == QLatin1String(subscriptionsTag))
            tag = subscriptionsTag;
        else if (event.kind == readStatusKind && dTag == QLatin1String(readStatusTag))
            tag = readStatusTag;
        else if (event.kind == viewsKind && dTag == QLatin1String(viewsTag))
            tag = viewsTag;
        if (!tag)
            continue;
        bool emptyLibrary = false;
        if (event.kind == subscriptionsKind) {
            emptyLibrary = store.feeds().isEmpty();
            if (!store.lastError().isEmpty())
                return storeFailed();
        }
        bool fresh = false;
        if (!stillFresh(tag, event, emptyLibrary, &fresh))
            return storeFailed();
        if (changed)
            return npubChanged();
        if (fresh)
            refuse(event.kind);
    }

    // The pool stopped keeping events at its size cap, so a newer one may
    // be missing. What came is still applied, as the watermarks keep it from
    // rolling anything back, but the pull is not recorded as the last sync,
    // so "Last synced" never claims one that missed something.
    if (result.truncated)
        outcome.problems << tr("the relays sent more than fits, so some sync events may be missing");

    outcome.pendingRead = store.pendingReadCount();
    if (outcome.pendingRead < 0)
        return storeFailed();
    const QList<SavedView> saved = store.savedViews();
    if (!store.lastError().isEmpty())
        return storeFailed();
    outcome.views = int(saved.size());
    // A refused payload leaves its kind as it was, so the pull is not
    // recorded as the last sync either, though what did fit stays applied.
    if (!result.truncated && !refused) {
        // Under the same check, so a pull for an account stopped meanwhile
        // never dates the next one's.
        const bool recorded = store.transaction([&] {
            const SyncAccount now = store.syncAccount();
            if (!store.lastError().isEmpty())
                return false;
            changed = now.pubkey != account.pubkey;
            return changed || store.recordSynced(QDateTime::currentDateTimeUtc());
        });
        if (!recorded)
            return storeFailed();
        if (changed)
            return npubChanged();
    }
    return outcome;
}
}
