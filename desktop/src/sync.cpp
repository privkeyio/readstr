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
#include "nostrevent.h"

namespace Sync {
namespace {
QString tr(const char *text, int n = -1) { return QCoreApplication::translate("Sync", text, nullptr, n); }
}

bool hasLoneSurrogate(const QString &text) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate())
            ++i;
        else if (text.at(i).isSurrogate())
            return true;
    }
    return false;
}

QString shownName(const QString &name) {
    QString shown;
    shown.reserve(name.size());
    for (qsizetype i = 0; i < name.size(); ++i) {
        if (name.at(i).isHighSurrogate() && i + 1 < name.size() && name.at(i + 1).isLowSurrogate()) {
            shown += name.at(i);
            shown += name.at(++i);
        } else {
            shown += name.at(i).isSurrogate() ? QChar(QChar::ReplacementCharacter) : name.at(i);
        }
    }
    return FeedParser::cleanText(shown);
}

QString tagName(const QString &tag) {
    const QString cleaned = FeedParser::cleanText(tag);
    QString name;
    name.reserve(cleaned.size());
    for (qsizetype i = 0; i < cleaned.size();) {
        const bool pair =
            cleaned.at(i).isHighSurrogate() && i + 1 < cleaned.size() && cleaned.at(i + 1).isLowSurrogate();
        const char32_t code = pair ? QChar::surrogateToUcs4(cleaned.at(i), cleaned.at(i + 1)) : cleaned.at(i).unicode();
        const qsizetype length = pair ? 2 : 1;
        if (QChar::category(code) != QChar::Other_Format || code == 0x200D || (code >= 0xE0020 && code <= 0xE007F))
            name += QStringView(cleaned).mid(i, length);
        i += length;
    }
    // A space each side of what was taken out is one space.
    return name.simplified();
}

QString categoryNameProblem(const QString &name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return tr("A category needs a name");
    if (trimmed.size() > maxCategoryNameLength)
        return tr("A category name can be at most %1 characters").arg(maxCategoryNameLength);
    // The import files a pulled feed under the name cleaned, so one that
    // cleans to something else would come back from the web app as another
    // category.
    if (hasLoneSurrogate(trimmed) || FeedParser::cleanText(trimmed) != trimmed)
        return tr("A category name cannot hold control characters, direction marks, or several spaces in a row");
    return {};
}

QString tagProblem(const QString &tag) {
    const QString trimmed = tag.trimmed();
    if (trimmed.isEmpty())
        return tr("A tag needs a name");
    if (trimmed.size() > maxTagLength)
        return tr("A tag can be at most %1 characters").arg(maxTagLength);
    // A tag given here goes out to the web app as it is typed, so it has to
    // show as it is: tagName() takes out the control characters, direction
    // marks, invisible format characters, and runs of spaces that would
    // make it read or match as something else. The message names it as
    // shownName() has it, so it carries no direction mark or broken text
    // of its own.
    if (!storableTag(trimmed) || tagName(trimmed) != trimmed)
        return tr(
                   "The tag %1 cannot be kept: it holds control characters, direction marks, invisible characters, or several spaces in a row, or shows as nothing")
            .arg(shownName(trimmed));
    return {};
}

QString formatTagList(const QStringList &tags) {
    QStringList parts;
    parts.reserve(tags.size());
    for (const QString &tag : tags) {
        // One the list would split, take for a quoted one, or trim goes
        // between quotes, so parseTagList() reads it back as it is.
        if (tag.contains(QLatin1Char(',')) || tag.startsWith(QLatin1Char('"')) || tag.trimmed() != tag)
            parts << QLatin1Char('"') + QString(tag).replace(QLatin1Char('"'), QLatin1String("\"\""))
                         + QLatin1Char('"');
        else
            parts << tag;
    }
    return parts.join(QLatin1String(", "));
}

namespace {
// The entries of a list as formatTagList() writes them: each one between
// commas trimmed, or, when it starts with a quote, what is between that
// quote and the next one on its own, as it is, with a doubled quote for
// each quote in it. Null when a quote is not closed, or is followed by
// more than spaces before the next comma.
std::optional<QStringList> splitTagList(const QString &text) {
    QStringList entries;
    const qsizetype size = text.size();
    for (qsizetype i = 0;;) {
        while (i < size && text.at(i).isSpace())
            ++i;
        if (i < size && text.at(i) == QLatin1Char('"')) {
            QString entry;
            for (++i;; ++i) {
                if (i >= size)
                    return std::nullopt;
                if (text.at(i) != QLatin1Char('"')) {
                    entry += text.at(i);
                } else if (i + 1 < size && text.at(i + 1) == QLatin1Char('"')) {
                    entry += QLatin1Char('"');
                    ++i;
                } else {
                    ++i;
                    break;
                }
            }
            while (i < size && text.at(i).isSpace())
                ++i;
            if (i < size && text.at(i) != QLatin1Char(','))
                return std::nullopt;
            entries << entry;
        } else {
            qsizetype end = text.indexOf(QLatin1Char(','), i);
            if (end < 0)
                end = size;
            entries << text.mid(i, end - i).trimmed();
            i = end;
        }
        if (i >= size)
            return entries;
        ++i;
    }
}
}

std::optional<QStringList> parseTagList(const QString &text, QString *problem, const QStringList &current) {
    const std::optional<QStringList> entries = splitTagList(text);
    if (!entries) {
        if (problem)
            *problem = tr("A tag in quotes needs its closing quote before the next comma, and each quote in it written twice");
        return std::nullopt;
    }
    // A tag the feed carries already, left as the list showed it, is kept
    // exactly, even one from the web app that could not be typed here, so
    // saving another change never respells it, nor drops it for one that
    // shows the same; only those new or changed are checked.
    const QSet<QString> kept(current.cbegin(), current.cend());
    QSet<QString> names, seen;
    for (const QString &entry : *entries)
        if (kept.contains(entry))
            names.insert(tagName(entry));
    QStringList tags;
    for (const QString &entry : *entries) {
        const bool existing = kept.contains(entry);
        const QString tag = existing ? entry : entry.trimmed();
        if (tag.isEmpty() || seen.contains(tag))
            continue;
        if (!existing) {
            if (const QString why = tagProblem(tag); !why.isEmpty()) {
                if (problem)
                    *problem = why;
                return std::nullopt;
            }
            // Spellings that show the same are one tag in the sources pane,
            // so the first stands for them here too.
            const QString name = tagName(tag);
            if (names.contains(name))
                continue;
            names.insert(name);
        }
        seen.insert(tag);
        tags << tag;
    }
    if (tags.size() > maxTagsPerFeed) {
        if (problem)
            *problem = tr("A feed can have at most %1 tags").arg(maxTagsPerFeed);
        return std::nullopt;
    }
    return tags;
}

QString safeColor(const QString &color) {
    if ((color.size() != 4 && color.size() != 7) || color.at(0) != QLatin1Char('#'))
        return {};
    QString out(1, QLatin1Char('#'));
    for (qsizetype i = 1; i < color.size(); ++i) {
        const QChar digit = color.at(i).toLower();
        if (!(digit >= QLatin1Char('0') && digit <= QLatin1Char('9'))
            && !(digit >= QLatin1Char('a') && digit <= QLatin1Char('f')))
            return {};
        out += color.size() == 4 ? QString(2, digit) : QString(digit);
    }
    return out;
}

QString safeIcon(const QString &icon) {
    const QString text = FeedParser::cleanText(icon);
    return text.size() <= maxIconLength && !hasLoneSurrogate(text) ? text : QString();
}

QString accountChanged() { return tr("the sync account changed"); }
QString organizationChanged() { return tr("the feeds or categories have changed since"); }
QString noSuchCategory(const QString &name) { return tr("There is no category %1").arg(shownName(name)); }
QString noSuchTag(const QString &name) { return tr("No feed has the tag %1").arg(shownName(name)); }

bool storableTag(const QString &tag) {
    if (tag.size() > maxTagLength || tag.trimmed().isEmpty() || hasLoneSurrogate(tag))
        return false;
    return std::none_of(tag.cbegin(), tag.cend(), [](QChar c) { return c.unicode() < 0x20; });
}

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
    // The whole list, which fits the schema, for mirrorList().
    QJsonObject object;
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
    list.object = *object;
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

namespace {
// A view's rules and names, without its place, which every pull and every
// save here numbers again.
QJsonObject withoutOrder(QJsonObject view) {
    view.remove("order");
    return view;
}
}

QList<SavedView> mergeViews(const QList<SavedView> &local, const QList<SavedView> &remote,
                            const QList<SavedView> &previous) {
    // A view the relays held before and the newer list lacks was deleted
    // in the web app. Kept, it would go out again with the next edit here
    // and come back there too; so it goes, unless it changed here since,
    // which makes it a view the user still wants.
    QHash<QString, QJsonObject> before;
    for (const SavedView &view : previous)
        before.insert(view.id, withoutOrder(view.view));
    QSet<QString> listed;
    for (const SavedView &view : remote)
        listed.insert(view.id);
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
        if (!listed.contains(view.id) && before.contains(view.id) && before.value(view.id) == withoutOrder(view.view))
            continue;
        merged << view;
    }
    return normalize(merged);
}

namespace {
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

// A feed as the subscription list names it, so a local feed and an entry of
// the list compare: a web feed by Store::urlKey, an author by their npub.
QString webKey(const QString &url) { return QStringLiteral("web ") + Store::urlKey(url); }

std::optional<QString> authorKey(const QString &entry) {
    const auto profile = Nip19::decodeProfile(entry.trimmed());
    if (!profile)
        return std::nullopt;
    return QStringLiteral("npub ") + Nip19::npub(profile->pubkey);
}

// The tags of a feed in the list, when they fit the web app's schema.
std::optional<QJsonArray> tagsFitting(const QJsonValue &value) {
    if (!stringArray(value, maxTagsPerFeed, 256))
        return std::nullopt;
    return value.toArray();
}

// Each tag or category entry of a list's map under both keys its name may
// be, as the list does not say which kind of feed it is about; one lookup
// per map, as the two may spell the same feed differently.
QHash<QString, QString> entriesOf(const QJsonObject &map) {
    QHash<QString, QString> entryOf;
    for (auto it = map.begin(); it != map.end(); ++it) {
        entryOf.insert(webKey(it.key()), it.key());
        if (const auto key = authorKey(it.key()))
            entryOf.insert(*key, it.key());
    }
    return entryOf;
}

// The feeds a list lists, in "rss" or "nostr", by the keys a feed here is
// found under, as entriesOf() keys them.
QSet<QString> listedKeys(const QJsonObject &list) {
    QSet<QString> keys;
    for (const QJsonValue &entry : list.value("rss").toArray())
        if (entry.isString())
            keys.insert(webKey(entry.toString()));
    for (const QJsonValue &entry : list.value("nostr").toArray())
        if (const auto key = authorKey(entry.toString()))
            keys.insert(*key);
    return keys;
}

// A category's color and icon, when they fit the web app's schema.
QJsonObject categoryStyle(const QJsonObject &category) {
    QJsonObject style;
    if (isString(category.value("color"), 64))
        style.insert("color", category.value("color"));
    if (isString(category.value("icon"), 256))
        style.insert("icon", category.value("icon"));
    return style;
}

// The category entries of a list's map by name as the import files a feed
// under it (FeedParser::cleanText()), so a name the web app spells with two
// spaces, a control character, or a bidirectional override still finds its
// entry once the library holds it cleaned. For each name, the first entry,
// by feed, with a color or an icon that fits the schema, else the first of
// that name. mirrorList() shows that entry's style and
// subscriptionsContent() carries it on, so the sidebar and a publish from
// here always agree on it.
QHash<QString, QJsonObject> stylesByName(const QJsonObject &categories) {
    QHash<QString, QJsonObject> byName;
    for (auto it = categories.begin(); it != categories.end(); ++it) {
        const QJsonObject category = it.value().toObject();
        if (!isString(category.value("name"), 256))
            continue;
        const QString name = FeedParser::cleanText(category.value("name").toString());
        if (name.isEmpty())
            continue;
        const auto found = byName.constFind(name);
        if (found == byName.cend() || (categoryStyle(*found).isEmpty() && !categoryStyle(category).isEmpty()))
            byName.insert(name, category);
    }
    return byName;
}

// A feed as entriesOf() and listedKeys() key it.
QString feedKey(const Feed &feed) {
    return Library::isNostrFeed(feed) ? QStringLiteral("npub ") + feed.url : webKey(feed.url);
}

// The tags of a list's entry as a feed here keeps them: each once, and none
// storableTag() refuses.
QStringList storableTags(const QJsonValue &entry) {
    QStringList tags;
    for (const QJsonValue &value : entry.toArray()) {
        const QString tag = value.toString();
        if (storableTag(tag) && !tags.contains(tag))
            tags << tag;
    }
    return tags;
}

// A subscription list (one parseSubscriptions() took) looked up the way
// mirrorList() reads it.
struct ListIndex {
    explicit ListIndex(const QJsonObject &list)
        : tagMap(list.value("tags").toObject()), tagEntryOf(entriesOf(tagMap)), listed(listedKeys(list)),
          categories(list.value("categories").toObject()), categoryEntryOf(entriesOf(categories)),
          byName(stylesByName(categories)) {}
    QJsonObject tagMap;
    QHash<QString, QString> tagEntryOf;
    QSet<QString> listed;
    QJsonObject categories;
    QHash<QString, QString> categoryEntryOf;
    QHash<QString, QJsonObject> byName;
};

// The tags the list gives `feed`, found as subscriptionsContent() finds
// them, or null when it neither lists the feed nor has tags for it, which
// leaves the feed its own: the list may be one published here while an
// unsubscribe was undone, which says nothing of the feed, and a list that
// took a feed's tags away from it still lists it.
std::optional<QStringList> mirroredTags(const ListIndex &list, const Feed &feed) {
    const QString key = feedKey(feed);
    // Looked up, not taken by value, so a feed with no entry is never given
    // the entry of an empty key.
    const auto entry = list.tagEntryOf.constFind(key);
    if (entry == list.tagEntryOf.cend() && !list.listed.contains(key))
        return std::nullopt;
    return entry == list.tagEntryOf.cend() ? QStringList() : storableTags(list.tagMap.value(*entry));
}

// The color and icon the list gives each category, by its name cleaned:
// the ones stylesByName() picks, safe to show, and for a category it names
// nowhere that a feed it neither lists nor has tags for is in, the style the
// library has, as mirroredTags() leaves such a feed its own.
QHash<QString, CategoryStyle> mirroredStyles(const ListIndex &list, const QList<Feed> &feeds) {
    QHash<QString, CategoryStyle> styles;
    for (auto it = list.byName.cbegin(); it != list.byName.cend(); ++it) {
        const CategoryStyle style{safeColor(it->value("color").toString()), safeIcon(it->value("icon").toString())};
        if (!style.color.isEmpty() || !style.icon.isEmpty())
            styles.insert(it.key(), style);
    }
    for (const Feed &feed : feeds) {
        const QString key = feedKey(feed);
        if (list.tagEntryOf.contains(key) || list.listed.contains(key))
            continue;
        const QString category = FeedParser::cleanText(feed.category);
        if (!category.isEmpty() && !list.byName.contains(category)
            && (!feed.categoryColor.isEmpty() || !feed.categoryIcon.isEmpty()))
            styles.insert(category, CategoryStyle{feed.categoryColor, feed.categoryIcon});
    }
    return styles;
}

// A category's name as a subscription list carries it: the first
// maxCategoryNameLength UTF-16 units, without the half of a surrogate pair
// the cut would leave, as an OPML file can name a category at any length.
QString cappedName(const QString &name) {
    QString capped = name.left(maxCategoryNameLength);
    if (capped.size() < name.size() && !capped.isEmpty() && capped.back().isHighSurrogate())
        capped.chop(1);
    return capped;
}

// Whether the library's category `local` is the one a list names `listed`,
// cleaned as the import files it: the same name cleaned, or cut as a
// publish from here cuts it (cappedName()), so a long name the list was
// given from here is not taken for another category.
bool sameCategory(const QString &local, const QString &listed) {
    return FeedParser::cleanText(local) == listed || FeedParser::cleanText(cappedName(local)) == listed;
}

// The category the list files `feed` under, cleaned as the import files a
// new feed, empty for none, or null when the list says nothing of it, which
// leaves the feed its own. The web app says a feed it lists has no category
// only by giving it no entry, so a feed it lists with none, or with a name
// that cleans to nothing, has none; but a list whose map of categories is
// full may be one a publish cut short (subscriptionsContent() stops at
// maxFeeds entries), so a missing entry there says nothing. A feed the list
// does not list keeps its own, as for its tags (mirroredTags()).
std::optional<QString> mirroredCategory(const ListIndex &list, const Feed &feed) {
    const QString key = feedKey(feed);
    const auto entry = list.categoryEntryOf.constFind(key);
    if (entry != list.categoryEntryOf.cend())
        return FeedParser::cleanText(list.categories.value(*entry).toObject().value("name").toString());
    if (!list.listed.contains(key) || list.categories.size() >= maxFeeds)
        return std::nullopt;
    return QString();
}

// Gives every feed here the tags mirroredTags() finds for it, the category
// mirroredCategory() does, and every category the color and icon
// mirroredStyles() does, so the sidebar shows what a publish from here
// builds on. A feed the list lists or has tags for, and a category it names,
// has only what the list gives, as they come from a list unless changed
// here. A tag that is listed twice is shown once, and one that
// storableTag() refuses not at all; a color or an icon that is not safe to
// show is left out. Only what differs is written, and `wrote` says whether
// anything was.
//
// What was changed here and not published yet (PendingEdits), a feed's
// tags, the category it is in, or a category's style, each apart, stays as
// it is: the list, the same one again or a newer one from the web app, was
// made without that change, which the next publish carries
// (subscriptionsContent()). Everything else follows the list, whatever was
// changed elsewhere, so nothing here depends on which list came before.
bool mirrorList(Store &store, const QJsonObject &list, bool *wrote = nullptr) {
    if (wrote)
        *wrote = false;
    const ListIndex index(list);
    const QList<Feed> feeds = store.feeds();
    if (!store.lastError().isEmpty())
        return false;
    for (const Feed &feed : feeds) {
        const std::optional<QStringList> tags = mirroredTags(index, feed);
        if (!tags)
            continue;
        if (!feed.tagsEdit && *tags != feed.tags) {
            if (!store.setFeedTags(feed.id, *tags))
                return false;
            if (wrote)
                *wrote = true;
        }
        // A category the library keeps under a name the list spells
        // cleaned, as an OPML file can name it, is the same one.
        if (const auto category = mirroredCategory(index, feed);
            !feed.placementEdit && category
            && (category->isEmpty() ? !feed.category.isEmpty() : !sameCategory(feed.category, *category))) {
            if (!store.placeFeed(feed.id, *category))
                return false;
            if (wrote)
                *wrote = true;
        }
    }
    const QHash<QString, CategoryStyle> styles = mirroredStyles(index, feeds);
    bool restyled = false;
    if (!store.replaceCategoryStyles(styles, &restyled))
        return false;
    if (wrote && restyled)
        *wrote = true;
    return true;
}
}

bool namesCategory(const QString &content, const QString &name) {
    const QString cleaned = FeedParser::cleanText(name);
    return !cleaned.isEmpty()
           && stylesByName(parseObject(content).value_or(QJsonObject{}).value("categories").toObject())
                  .contains(cleaned);
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
    outcome.settled = result.settled();
    for (const QList<NostrEvent> *events : {&result.events, &result.oversized})
        for (const NostrEvent &event : *events) {
            const char *tag = Sync::dTag(event.kind);
            if (event.pubkey == account.pubkey && tag && event.tag(QStringLiteral("d")) == QLatin1String(tag))
                outcome.newest[event.kind] = qMax(outcome.newest.value(event.kind), event.createdAt);
        }

    if (beforeWrites)
        beforeWrites();

    bool refused = false;
    const auto refuse = [&](int kind) {
        refused = true;
        if (outcome.refused.contains(kind))
            return;
        outcome.refused << kind;
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

    std::optional<SubscriptionList> listed;
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
            listed = parseSubscriptions(subscriptions->content);
            if (const auto &list = listed) {
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

    // The tags and the category colors and icons of the list applied, just
    // now or before, which a library from before they were kept lacks, and
    // which one published here gives when the relays send it back. Checked
    // under the write, so a newer list applied meanwhile is never mirrored
    // over.
    if (subscriptions && !outcome.refused.contains(subscriptionsKind)) {
        const SyncAccount now = store.syncAccount();
        if (!store.lastError().isEmpty())
            return storeFailed();
        const QString tag = QString::fromLatin1(subscriptionsTag);
        if (now.pubkey == account.pubkey && now.appliedIds.value(tag) == subscriptions->id) {
            if (!listed)
                listed = parseSubscriptions(subscriptions->content);
            if (listed) {
#ifdef READSTR_TESTS
                if (mirrorHook)
                    mirrorHook();
#endif
                const bool mirrored = store.transaction([&] {
                    const SyncAccount after = store.syncAccount();
                    if (!store.lastError().isEmpty())
                        return false;
                    changed = after.pubkey != account.pubkey;
                    if (changed || after.appliedIds.value(tag) != subscriptions->id)
                        return true;
                    return mirrorList(store, listed->object);
                });
                if (!mirrored)
                    return storeFailed();
                if (changed)
                    return npubChanged();
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
                if (!store.lastError().isEmpty())
                    return false;
                // The list the relays held before this one, which apply()
                // caches only after this write, so a view it held and this
                // one lacks can be told deleted in the web app. Only when it
                // is the list applied last: one cached behind that, or none
                // cached, says nothing of what this one dropped, so nothing
                // is taken for deleted.
                const auto previous = store.syncRemote(QString::fromLatin1(viewsTag));
                if (!store.lastError().isEmpty())
                    return false;
                const SyncAccount now = store.syncAccount();
                if (!store.lastError().isEmpty())
                    return false;
                const bool current =
                    previous && previous->eventId == now.appliedIds.value(QString::fromLatin1(viewsTag));
                const QList<SavedView> held =
                    current ? parseViews(previous->content).value_or(QList<SavedView>()) : QList<SavedView>();
                return store.replaceSavedViews(mergeViews(local, *remote, held))
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
        const char *tag = Sync::dTag(event.kind);
        if (!tag || event.tag(QStringLiteral("d")) != QLatin1String(tag))
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

    // What the relays hold now, when it is what was applied here, for the
    // next publish to build on: the event just applied, or the one applied
    // before, which a library from before publishing has no content for.
    // Checked under the write, so another process switching the account or
    // applying a newer one meanwhile is never cached over.
#ifdef READSTR_TESTS
    if (cacheHook)
        cacheHook();
#endif
    const bool cached = store.transaction([&] {
        const SyncAccount after = store.syncAccount();
        if (!store.lastError().isEmpty())
            return false;
        changed = after.pubkey != account.pubkey;
        if (changed)
            return true;
        for (const NostrEvent *event : {subscriptions, readStatus, views}) {
            if (!event)
                continue;
            const QString tag = QString::fromLatin1(Sync::dTag(event->kind));
            if (after.appliedIds.value(tag) == event->id && !store.setSyncRemote(tag, event->id, event->content))
                return false;
        }
        return true;
    });
    if (!cached)
        return storeFailed();
    if (changed)
        return npubChanged();

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

const char *dTag(int kind) {
    switch (kind) {
    case subscriptionsKind:
        return subscriptionsTag;
    case readStatusKind:
        return readStatusTag;
    case viewsKind:
        return viewsTag;
    }
    return nullptr;
}

NostrEvent draft(int kind, const QString &content, qint64 createdAt) {
    NostrEvent event;
    event.kind = kind;
    event.createdAt = createdAt;
    event.tags = {{QStringLiteral("d"), QString::fromLatin1(dTag(kind))},
                  {QStringLiteral("client"), QString::fromLatin1(clientTag)}};
    event.content = content;
    return event;
}

qint64 nextCreatedAt(qint64 now, qint64 lastSeen) { return qMax(now, lastSeen + 1); }

void ReadGuidCollector::add(const QString &key) {
    if (full() || key.isEmpty() || key.size() > maxGuidLength || m_seen.contains(key))
        return;
    // JSON.stringify(JSON.stringify(key)), less one of its quotes.
    const qsizetype cost =
        NostrEvent::jsonString(QString::fromUtf8(NostrEvent::jsonString(key))).size() - 1;
    if (m_size + cost > maxPublishedReadContent) {
        m_done = true;
        return;
    }
    m_size += cost;
    m_seen.insert(key);
    m_guids << key;
}

QStringList publishedReadGuids(const QStringList &local, const QStringList &remote) {
    ReadGuidCollector collector;
    for (const QString &key : local)
        collector.add(key);
    for (const QString &key : remote)
        collector.add(key);
    return collector.guids();
}

namespace {
QString compact(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// The UTF-8 bytes `text` takes in an event, escaped as its content.
qsizetype eventBytes(const QString &text) { return NostrEvent::jsonString(text).size() - 2; }

// A URL with a user name or password, which never goes into the public list,
// even when the last list carried it.
bool hasUserInfo(const QString &entry) { return !QUrl(entry).userInfo().isEmpty(); }
}

std::optional<QString> subscriptionsContent(const QList<Feed> &feeds, const QString &remote, qint64 remoteStoredAt,
                                            qint64 now, int *tooLong) {
    const QJsonObject list = parseObject(remote).value_or(QJsonObject{});
    const QJsonObject remoteTags = list.value("tags").toObject(),
                      remoteCategories = list.value("categories").toObject();
    const QHash<QString, QString> tagEntryOf = entriesOf(remoteTags), categoryEntryOf = entriesOf(remoteCategories);
    const QHash<QString, QJsonObject> byName = stylesByName(remoteCategories);
    const QSet<QString> remoteKeys = listedKeys(list);
    // What another device unsubscribed from.
    QSet<QString> remoteDeleted;
    for (const QJsonValue &value : list.value("deleted").toArray()) {
        const QString entry = value.toString();
        if (entry.isEmpty())
            continue;
        remoteDeleted.insert(webKey(entry));
        if (const auto key = authorKey(entry))
            remoteDeleted.insert(*key);
    }

    QJsonArray rss, nostr;
    QJsonObject tags, categories;
    // Feeds listed, and local feeds kept off the list.
    QSet<QString> listed, kept;
    QStringList deleted;
    if (tooLong)
        *tooLong = 0;
    for (const Feed &feed : feeds) {
        const bool author = Library::isNostrFeed(feed);
        const QString key = author ? QStringLiteral("npub ") + feed.url : webKey(feed.url);
        QJsonArray &into = author ? nostr : rss;
        if (!author) {
            // The list is public: a URL with a user name or password, or one
            // on the local network, says what only this machine should know.
            const QUrl url(feed.url);
            if (!url.userInfo().isEmpty() || Fetcher::isLocalHost(url.host())) {
                kept.insert(key);
                continue;
            }
        }
        if (feed.url.size() > maxUrlLength) {
            if (tooLong)
                ++*tooLong;
            continue;
        }
        if (into.size() >= maxFeeds || listed.contains(key))
            continue;
        // Unsubscribed elsewhere, and not subscribed here since that list
        // came: the pull never removes a feed, but this machine does not
        // bring it back either.
        if (remoteDeleted.contains(key)
            && !(feed.createdAt.isValid() && feed.createdAt.toSecsSinceEpoch() >= remoteStoredAt))
            continue;
        listed.insert(key);
        into << feed.url;
        // A feed the last list neither lists nor has tags for, such as one
        // whose unsubscribe was undone after a list without it was
        // published, keeps the tags it has here, which only ever came from
        // a list (mirrorList()). One the list lists with no tags has none.
        // A feed whose tags were changed here (PendingEdits) carries its
        // own, even none: they are the ones changed here.
        const auto tagEntry = tagEntryOf.constFind(key);
        const bool remoteListed = remoteKeys.contains(key);
        if (tags.size() < maxFeeds) {
            if (tagEntry != tagEntryOf.cend() && !feed.tagsEdit) {
                if (const auto fitting = tagsFitting(remoteTags.value(*tagEntry)); fitting && !fitting->isEmpty())
                    tags.insert(feed.url, *fitting);
            } else if ((feed.tagsEdit || !remoteListed) && !feed.tags.isEmpty()) {
                tags.insert(feed.url, QJsonArray::fromStringList(feed.tags));
            }
        }
        const QString name = cappedName(feed.category);
        if (name.isEmpty() || categories.size() >= maxFeeds)
            continue;
        // The category as the last list names it, so the publish neither
        // renames it nor drops its style when the library holds its name
        // cleaned: the feed's own entry when it is of this category, else
        // the one stylesByName() picks, as the sidebar shows it. A feed with
        // no entry of its own is never given the entry of an empty key.
        const QString cleaned = FeedParser::cleanText(name);
        const auto ownEntry = categoryEntryOf.constFind(key);
        const QJsonObject own =
            ownEntry == categoryEntryOf.cend() ? QJsonObject() : remoteCategories.value(*ownEntry).toObject();
        const bool ownIsThisCategory = isString(own.value("name"), 256) && !cleaned.isEmpty()
                                       && FeedParser::cleanText(own.value("name").toString()) == cleaned;
        const QJsonObject web = ownIsThisCategory ? own : byName.value(cleaned);
        QJsonObject category = categoryStyle(web);
        // A category whose color and icon, or whose being, were changed
        // here (PendingEdits) carries them as they are here: renamed or
        // merged here, it keeps the style it had, or takes the one it
        // joined.
        if (feed.styleEdit) {
            category = QJsonObject();
            if (!feed.categoryColor.isEmpty())
                category.insert("color", feed.categoryColor);
            if (!feed.categoryIcon.isEmpty())
                category.insert("icon", feed.categoryIcon);
        } else if (web.isEmpty() && !remoteListed && tagEntry == tagEntryOf.cend()) {
            // A category the list names nowhere keeps the style it has here
            // for a feed the list neither lists nor has tags for, as its
            // tags are kept, and as mirrorList() keeps it, so the sidebar and
            // the publish agree on it.
            if (!feed.categoryColor.isEmpty())
                category.insert("color", feed.categoryColor);
            if (!feed.categoryIcon.isEmpty())
                category.insert("icon", feed.categoryIcon);
        }
        category.insert("name", web.isEmpty() ? name : web.value("name").toString());
        categories.insert(feed.url, category);
    }
    // What the last list holds that is not here: kept when this machine
    // cannot follow it or keeps it off the list, and marked deleted when it
    // could follow it.
    struct Carried {
        QString entry;
        bool author = false;
        QJsonValue tags, category;
    };
    QList<Carried> carried;
    const auto remoteOnly = [&](const QString &entry, bool author) {
        if (entry.size() > maxUrlLength || hasUserInfo(entry))
            return;
        const auto npubKey = authorKey(entry);
        const QString key = author ? npubKey.value_or(QStringLiteral("npub ") + entry.toLower()) : webKey(entry);
        if (listed.contains(key))
            return;
        const QUrl url(entry, QUrl::StrictMode);
        const bool followable = !kept.contains(key)
            && (author ? npubKey.has_value() : Library::isWebUrl(url) && !Fetcher::isLocalHost(url.host()));
        if (followable) {
            deleted << entry;
            return;
        }
        listed.insert(key);
        Carried item{entry, author, QJsonValue::Undefined, QJsonValue::Undefined};
        if (const auto fitting = tagsFitting(remoteTags.value(entry)); fitting && !fitting->isEmpty())
            item.tags = *fitting;
        if (remoteCategories.value(entry).isObject())
            item.category = remoteCategories.value(entry);
        carried << item;
    };
    for (const QJsonValue &entry : list.value("rss").toArray())
        if (entry.isString())
            remoteOnly(entry.toString(), false);
    for (const QJsonValue &entry : list.value("nostr").toArray())
        if (entry.isString())
            remoteOnly(entry.toString(), true);
    for (const QJsonValue &value : list.value("deleted").toArray()) {
        const QString entry = value.toString();
        if (entry.isEmpty() || entry.size() > maxUrlLength || hasUserInfo(entry) || listed.contains(webKey(entry))
            || listed.contains(authorKey(entry).value_or(QString())))
            continue;
        deleted << entry;
    }
    deleted.removeDuplicates();
    if (deleted.size() > maxFeeds)
        deleted.resize(maxFeeds);

    // With the first `carriedCount` carried entries and `deletedCount`
    // deleted ones.
    const auto build = [&](qsizetype carriedCount, qsizetype deletedCount) {
        QJsonArray allRss = rss, allNostr = nostr;
        QJsonObject allTags = tags, allCategories = categories;
        for (qsizetype i = 0; i < carriedCount; ++i) {
            const Carried &item = carried.at(i);
            QJsonArray &into = item.author ? allNostr : allRss;
            if (into.size() >= maxFeeds)
                continue;
            into << item.entry;
            if (!item.tags.isUndefined() && allTags.size() < maxFeeds)
                allTags.insert(item.entry, item.tags);
            if (!item.category.isUndefined() && allCategories.size() < maxFeeds)
                allCategories.insert(item.entry, item.category);
        }
        QJsonObject content{{"rss", allRss}, {"nostr", allNostr}, {"lastUpdated", now}};
        if (!allTags.isEmpty())
            content.insert("tags", allTags);
        if (!allCategories.isEmpty())
            content.insert("categories", allCategories);
        if (deletedCount > 0)
            content.insert("deleted", QJsonArray::fromStringList(deleted.mid(0, deletedCount)));
        return compact(content);
    };
    // How far the content is past what common relays take, counted as the
    // read status is: escaped in the event, with room for its other fields.
    const auto over = [](const QString &content) {
        return eventBytes(content) + readContentReserve - maxPublishedReadContent;
    };
    qsizetype keepCarried = carried.size(), keepDeleted = deleted.size();
    QString content = build(keepCarried, keepDeleted);
    qsizetype excess = over(content);
    if (excess <= 0)
        return content;
    // This machine's own feeds go whole or not at all.
    if (over(build(0, 0)) > 0)
        return std::nullopt;
    // An entry's bytes in an array of the content: quoted, with its comma.
    const auto arrayEntryBytes = [](const QString &entry) {
        return eventBytes(QString::fromUtf8(NostrEvent::jsonString(entry))) + 1;
    };
    // What the last list carried goes from the end, `deleted` first, then
    // the feeds this machine cannot follow, until it fits.
    while (excess > 0) {
        while (excess > 0 && keepDeleted > 0)
            excess -= arrayEntryBytes(deleted.at(--keepDeleted));
        while (excess > 0 && keepCarried > 0) {
            const Carried &item = carried.at(--keepCarried);
            excess -= arrayEntryBytes(item.entry);
            if (!item.tags.isUndefined())
                excess -= eventBytes(compact({{item.entry, item.tags}})) - 1;
            if (!item.category.isUndefined())
                excess -= eventBytes(compact({{item.entry, item.category}})) - 1;
        }
        content = build(keepCarried, keepDeleted);
        excess = over(content);
    }
    return content;
}

QString unsettledHold(const Outcome &outcome, int kind) {
    if (outcome.settled)
        return {};
    if (kind == readStatusKind)
        return tr("not every relay answered, so the read status is not replaced");
    if (kind == viewsKind)
        return tr("not every relay answered, so the saved views are not replaced");
    return {};
}

QString readStatusContent(const QStringList &guids, qint64 now) {
    return compact({{"itemGuids", QJsonArray::fromStringList(guids)}, {"lastUpdated", now}});
}

namespace {
// The views as kind 30406 carries them, without their mute words.
QJsonArray publishedViews(const QList<SavedView> &views) {
    QJsonArray array;
    for (const SavedView &view : views) {
        if (array.size() >= maxRemoteViews)
            break;
        QJsonObject published = view.view;
        // Never stored here, but never sent either way.
        QJsonObject keywords = published.value("keywords").toObject();
        if (keywords.contains("exclude")) {
            keywords.remove("exclude");
            if (keywords.value("include").toArray().isEmpty())
                published.remove("keywords");
            else
                published.insert("keywords", keywords);
        }
        array << published;
    }
    return array;
}
}

QString viewsContent(const QList<SavedView> &views, qint64 now) {
    return compact({{"views", publishedViews(views)}, {"lastUpdated", now}});
}

namespace {
// drafts(), meant for inside one transaction.
Drafts draftsInTransaction(Store &store, const QString &pubkey, const QList<int> &kinds,
                           const QHash<int, qint64> &seen, qint64 now) {
    Drafts out;
    const auto failed = [&] {
        out.events.clear();
        out.error = store.lastError();
        return out;
    };
    const SyncAccount account = store.syncAccount();
    if (!store.lastError().isEmpty())
        return failed();
    if (account.pubkey.isEmpty() || account.pubkey != pubkey) {
        out.error = tr("the npub changed before publishing");
        return out;
    }
#ifdef READSTR_TESTS
    if (draftsHook)
        draftsHook();
#endif
    for (int kind : kinds) {
        const QString tag = QString::fromLatin1(dTag(kind));
        const qint64 createdAt = nextCreatedAt(now, qMax(account.applied.value(tag), seen.value(kind)));
        const auto remote = store.syncRemote(tag);
        if (!store.lastError().isEmpty())
            return failed();
        const QString remoteContent = remote ? remote->content : QString();
        // The watermark and the content were read apart, and a pull may
        // have applied a newer event between them, or applied one and not
        // cached it yet: a draft dated by one and built on the other could
        // drop what the relays hold, so it waits for the next pull.
        const bool cacheBehind = remote && remote->eventId != account.appliedIds.value(tag);
        QString content;
        if (kind == subscriptionsKind) {
            // A library from before publishing knows which list it applied
            // but not what that list held, and a list built without it would
            // drop what only another device keeps.
            if ((!remote && account.applied.contains(tag)) || cacheBehind) {
                out.waiting << kind;
                out.notes << tr("the last subscription list applied here has not been pulled again yet, so it is not replaced");
                continue;
            }
            const QList<Feed> feeds = store.feeds();
            if (!store.lastError().isEmpty())
                return failed();
            int tooLong = 0;
            const auto built =
                subscriptionsContent(feeds, remoteContent, remote ? remote->storedAt : 0, now, &tooLong);
            if (!built) {
                out.failed << kind;
                out.errors << tr("the subscription list is too large for the relays (over 60,000 bytes)");
                continue;
            }
            if (tooLong > 0)
                out.notes << tr("%n feeds are left out of the subscription list, as their addresses are too long",
                                tooLong);
            content = *built;
            // Every mark, read in the same transaction as the feeds, counts
            // as carried: what the content holds of a marked row is what
            // was changed here, and a row it leaves out (a feed on the local
            // network, with an address past maxUrlLength, past maxFeeds, or
            // unsubscribed on another device; a category none of whose
            // feeds goes out) is one no list from here will ever carry, so
            // its mark would otherwise hold it against every pull, and keep
            // the list to publish, for good.
            const std::optional<PendingEdits> pending = store.pendingEdits();
            if (!pending)
                return failed();
            out.subscriptionEdits = *pending;
        } else if (kind == readStatusKind) {
            // Allowed when the run began, but maybe not any more.
            if (!account.publishReads) {
                out.unchanged << kind;
                continue;
            }
            if (cacheBehind) {
                out.waiting << kind;
                continue;
            }
            QStringList remoteGuids;
            if (remote) {
                remoteGuids = parseReadStatus(remoteContent).value_or(QStringList());
            } else {
                // No content kept, as in a library from before publishing:
                // the GUIDs of the last list applied, in no order.
                const auto remembered = store.rememberedReadGuids();
                if (!remembered)
                    return failed();
                remoteGuids = *remembered;
            }
            ReadGuidCollector collector;
            if (!store.eachReadGuid([&](const QString &guid) {
                    collector.add(guid);
                    return !collector.full();
                }))
                return failed();
            // cleanPulledGuids: no blank keys from another device's list.
            for (const QString &guid : std::as_const(remoteGuids))
                if (!guid.trimmed().isEmpty())
                    collector.add(guid);
            const QStringList guids = collector.guids();
            const QSet<QString> held(remoteGuids.cbegin(), remoteGuids.cend());
            if (std::all_of(guids.cbegin(), guids.cend(), [&](const QString &guid) { return held.contains(guid); })) {
                out.unchanged << kind;
                continue;
            }
            content = readStatusContent(guids, now);
        } else if (kind == viewsKind) {
            const QList<SavedView> views = store.savedViews();
            if (!store.lastError().isEmpty())
                return failed();
            // Deletions do not sync, so publishing views not edited here
            // over a list the relays hold would only bring back views
            // deleted in the web app. Edited, they go out as they are, even
            // none: the pull before this merged what the relays hold.
            const bool edited = account.publishPending & viewsEditedBit;
            if (!edited && (views.isEmpty() || remote || account.applied.contains(tag))) {
                out.unchanged << kind;
                continue;
            }
            // A list applied but not cached, as in a library from before
            // publishing, or a newer one applied and not cached yet, is what
            // the next pull tells views deleted in the web app by
            // (mergeViews()), so these wait for that pull to cache it.
            if ((!remote && account.applied.contains(tag)) || cacheBehind) {
                out.waiting << kind;
                out.notes << tr("the last saved views applied here have not been pulled again yet, so they are not replaced");
                continue;
            }
            content = viewsContent(views, now);
            out.viewsEdits = account.viewsEdits;
        } else {
            continue;
        }
        out.events << draft(kind, content, createdAt);
    }
    return out;
}
}

Drafts drafts(Store &store, const QString &pubkey, const QList<int> &kinds, const QHash<int, qint64> &seen,
              qint64 now) {
    // Read in one transaction, so the account, its watermarks, the cache,
    // and the library are one snapshot: a login as another npub that drops
    // and replaces the views in between would otherwise have that npub's
    // views go out under this one's key, dated by this one's watermark.
    Drafts out;
    if (!store.transaction([&] {
            out = draftsInTransaction(store, pubkey, kinds, seen, now);
            return true;
        })) {
        out = Drafts();
        out.error = store.lastError();
    }
    return out;
}

QString recordPublished(Store &store, const QString &pubkey, const NostrEvent &event, qint64 builtAt,
                        bool *superseded, bool *mirrored, qint64 viewsEdits, const PendingEdits &carried) {
    const QString tag = QString::fromLatin1(dTag(event.kind));
    bool changed = false, lost = false, wrote = false;
    const bool saved = store.transaction([&] {
        const SyncAccount account = store.syncAccount();
        if (!store.lastError().isEmpty())
            return false;
        changed = account.pubkey != pubkey;
        if (changed)
            return true;
        if (!store.setSyncApplied(tag, event.createdAt, event.id))
            return false;
        // A newer version applied meanwhile is what the relays hold, and its
        // read GUIDs are the ones to remember.
        const SyncAccount after = store.syncAccount();
        if (!store.lastError().isEmpty())
            return false;
        lost = after.appliedIds.value(tag) != event.id;
        if (lost)
            return true;
        if (event.kind == readStatusKind
            && !store.rememberReadGuids(parseReadStatus(event.content).value_or(QStringList())))
            return false;
        // The views are published as edited only when no edit came after
        // the draft was built: one edited again while it was out still
        // waits to go out. Counted rather than compared with the library,
        // which a view that does not come back from JSON as it went in
        // would never match, so every start would publish the views again.
        if (event.kind == viewsKind && (account.publishPending & viewsEditedBit) && viewsEdits >= 0
            && viewsEdits == account.viewsEdits && !store.markPublishPending(pubkey, viewsEditedBit, false))
            return false;
        // So are the feeds and categories it carried as changed here, each
        // unless it was changed again after the draft, after which a pull
        // mirrors them as the list has them (mirrorList()), this one first.
        // One it left out, as a feed another device unsubscribed from, stays
        // marked, as what was changed of it has not gone out.
        if (event.kind == subscriptionsKind && !store.clearPendingEdits(carried))
            return false;
        // Stamped with when the content was built, not now: a feed
        // subscribed while this run signed and published is not in it.
        return store.setSyncRemote(tag, event.id, event.content, builtAt);
    });
    if (!saved)
        return store.lastError();
    if (superseded)
        *superseded = lost;
    // What it carried on for the feeds here is kept at once, and `mirrored`
    // tells the window to show it. That is written apart from the record,
    // so a failure here never loses the record of an event the relays have
    // taken, and checked again under its own write, so a newer list applied
    // in between is never mirrored over. A list not mirrored now is on the
    // next pull, which mirrors the list applied, so the failure is only
    // logged.
    if (!changed && !lost && event.kind == subscriptionsKind)
        if (const auto list = parseSubscriptions(event.content)) {
#ifdef READSTR_TESTS
            if (mirrorHook)
                mirrorHook();
#endif
            // A newer version applied between the record and this write won
            // all the same, so `superseded` says so, as when the record finds
            // it; and another account by then fails as it does in the record.
            const bool kept = store.transaction([&] {
                const SyncAccount account = store.syncAccount();
                if (!store.lastError().isEmpty())
                    return false;
                changed = account.pubkey != pubkey;
                lost = !changed && account.appliedIds.value(tag) != event.id;
                if (changed || lost)
                    return true;
                return mirrorList(store, list->object, &wrote);
            });
            if (superseded)
                *superseded = lost;
            if (!kept) {
                wrote = false;
                qWarning("readstr: publish: cannot show the subscription list's tags: %s",
                         qPrintable(store.lastError()));
            }
        }
    if (mirrored)
        *mirrored = wrote;
    return changed ? tr("the npub changed during publishing") : QString();
}
}
