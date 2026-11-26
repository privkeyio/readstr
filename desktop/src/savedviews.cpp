#include "savedviews.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSet>

#include "feedparser.h"
#include "sync.h"

namespace {
using SavedViews::maxWordLength;
using SavedViews::maxWords;

constexpr char context[] = "SavedViews";
QString tr(const char *text) { return QCoreApplication::translate(context, text); }

// The strings of a JSON array, as the web app's sanitizeStrings keeps them.
// Tags are kept whole rather than cut, so a view's tag is the same string as
// the feed's tag it names, which can be longer than a word. One no feed can
// carry is left for the store to list nothing for (Sync::storableTag()), as
// dropping it would list more.
QStringList strings(const QJsonValue &value, bool tags = false) {
    QStringList out;
    for (const QJsonValue &item : value.toArray()) {
        if (out.size() >= maxWords)
            break;
        // Cut by UTF-16 units, as the web app's slice() cuts, but never
        // through a surrogate pair: half of one is no character, and the
        // word would not match as itself.
        QString text = item.toString();
        if (!tags) {
            text.truncate(maxWordLength);
            if (text.size() == maxWordLength && text.back().isHighSurrogate())
                text.chop(1);
        }
        if (!text.isEmpty())
            out << text;
    }
    return out;
}
}

namespace SavedViews {
Evaluation evaluate(const SavedView &saved) {
    Evaluation result;
    const QJsonObject &view = saved.view;
    // organizationMode only says how the web app's sidebar groups the
    // source; it picks no articles, so there is nothing to leave out.
    static const QSet<QString> known{"id",       "name", "icon",     "order",           "source",
                                     "readState", "sort", "keywords", "organizationMode"};
    for (auto it = view.constBegin(); it != view.constEnd(); ++it)
        if (!known.contains(it.key()))
            result.unsupported << it.key();

    // A source without what it names lists every feed, in the web app too.
    const QJsonObject source = view.value("source").toObject();
    const QString kind = source.value("kind").toString(QStringLiteral("all"));
    const auto unknown = [&result](const char *field) {
        result.unsupported << QString::fromLatin1(field);
        result.unknownSource = true;
    };
    if (kind == QLatin1String("favorites"))
        result.filter.starredOnly = true;
    else if (kind == QLatin1String("feed") && !source.value("feedId").toString().isEmpty())
        unknown("source.feedId");
    else if (kind == QLatin1String("category") && !source.value("categoryId").toString().isEmpty())
        unknown("source.categoryId");
    else if (kind == QLatin1String("tags"))
        result.filter.tags = strings(source.value("tags"), true);
    else if (!QStringList{"all", "feed", "category", "tags"}.contains(kind))
        result.unsupported << QStringLiteral("source.kind");

    const QString readState = view.value("readState").toString(QStringLiteral("all"));
    if (readState == QLatin1String("unread"))
        result.filter.unreadOnly = true;
    else if (readState == QLatin1String("read"))
        result.filter.readOnly = true;
    else if (readState != QLatin1String("all"))
        result.unsupported << QStringLiteral("readState");

    const QString sort = view.value("sort").toString(QStringLiteral("newest"));
    if (sort == QLatin1String("oldest"))
        result.filter.oldestFirst = true;
    else if (sort != QLatin1String("newest"))
        result.unsupported << QStringLiteral("sort");

    // The mute words are kept apart (SavedView::exclude), but a view that
    // still holds its own, as the web app keeps them, has them applied too.
    const QJsonObject keywords = view.value("keywords").toObject();
    for (auto it = keywords.constBegin(); it != keywords.constEnd(); ++it)
        if (it.key() != QLatin1String("include") && it.key() != QLatin1String("exclude"))
            result.unsupported << QStringLiteral("keywords.") + it.key();
    result.filter.includeWords = strings(keywords.value("include"));
    QStringList exclude = strings(keywords.value("exclude"));
    for (const QString &word : saved.exclude)
        if (const QString text = word.left(maxWordLength); !text.isEmpty())
            exclude << text;
    exclude.removeDuplicates();
    // Each is looked for in every article the view reads.
    if (exclude.size() > maxWords)
        exclude.resize(maxWords);
    result.filter.excludeWords = exclude;
    return result;
}

bool listsSame(const SavedView &a, const SavedView &b) {
    const Evaluation first = evaluate(a), second = evaluate(b);
    const ArticleFilter &x = first.filter, &y = second.filter;
    return first.unknownSource == second.unknownSource && x.unreadOnly == y.unreadOnly
           && x.starredOnly == y.starredOnly && x.readOnly == y.readOnly && x.oldestFirst == y.oldestFirst
           && x.includeWords == y.includeWords && x.excludeWords == y.excludeWords && x.tags == y.tags;
}

QString name(const SavedView &view) {
    const QString name = FeedParser::cleanText(view.view.value("name").toString());
    return name.isEmpty() ? FeedParser::cleanText(view.id) : name;
}

QString icon(const SavedView &view) { return FeedParser::cleanText(view.view.value("icon").toString()); }
}

namespace {
// The kinds of source the dialog can set, as the web app names them.
const QStringList editableSources{QStringLiteral("all"), QStringLiteral("favorites"), QStringLiteral("tags")};

enum class Fit { Fits, TooMany, TooLong };

// `list` without empty or repeated entries, and whether it fits the web
// app's caps: at most maxWords, each of at most maxWordLength.
Fit fitWords(QStringList &list) {
    list.removeAll(QString());
    list.removeDuplicates();
    if (list.size() > maxWords)
        return Fit::TooMany;
    for (const QString &word : std::as_const(list))
        if (word.size() > maxWordLength)
            return Fit::TooLong;
    return Fit::Fits;
}

// The view at `at` is `expected`, or no view is expected. Its order is left
// out, as every save and every pull numbers the views again.
bool unchanged(const QList<SavedView> &views, qsizetype at, const SavedView *expected) {
    if (!expected)
        return true;
    QJsonObject now = views.at(at).view, then = expected->view;
    now.remove("order");
    then.remove("order");
    return now == then && views.at(at).exclude == expected->exclude;
}
}

namespace SavedViews {
Draft draftOf(const SavedView &saved) {
    const QJsonObject &view = saved.view;
    Draft draft;
    draft.name = view.value("name").toString();
    draft.icon = view.value("icon").toString();
    const QJsonObject source = view.value("source").toObject();
    draft.source = source.value("kind").toString(QStringLiteral("all"));
    if (draft.source == QLatin1String("tags"))
        draft.tags = strings(source.value("tags"), true);
    draft.readState = view.value("readState").toString(QStringLiteral("all"));
    draft.sort = view.value("sort").toString(QStringLiteral("newest"));
    const QJsonObject keywords = view.value("keywords").toObject();
    draft.include = strings(keywords.value("include"));
    draft.exclude = strings(keywords.value("exclude")) + saved.exclude;
    draft.exclude.removeDuplicates();
    return draft;
}

Draft edited(const SavedView *base, const QVariantMap &fields) {
    const Draft before = base ? draftOf(*base) : Draft{};
    Draft draft = before;
    if (fields.contains("name"))
        draft.name = fields.value("name").toString();
    if (fields.contains("icon"))
        draft.icon = fields.value("icon").toString();
    if (fields.contains("source"))
        draft.source = fields.value("source").toString();
    else if (!base)
        draft.source = fields.contains("tags") ? QStringLiteral("tags") : QStringLiteral("all");
    if (fields.contains("tags"))
        draft.tags = fields.value("tags").toStringList();
    if (fields.contains("readState"))
        draft.readState = fields.value("readState").toString();
    if (fields.contains("sort"))
        draft.sort = fields.value("sort").toString();
    if (fields.contains("include"))
        draft.include = words(fields.value("include").toString(), before.include);
    if (fields.contains("exclude"))
        draft.exclude = words(fields.value("exclude").toString(), before.exclude);
    // Left as it was, the source stays byte for byte, a feed's or a
    // category's included, which only the web app can set.
    if (base && draft.source == before.source
        && (draft.source != QLatin1String("tags") || draft.tags == before.tags))
        draft.source.clear();
    return draft;
}

bool viewableTag(const QString &tag) { return tag.size() <= maxWordLength && Sync::storableTag(tag); }

bool sourceEditable(const SavedView &view) {
    return editableSources.contains(view.view.value("source").toObject().value("kind").toString(QStringLiteral("all")));
}

QString newId() {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    QString random;
    for (int i = 0; i < 11; ++i)
        random += QLatin1Char(digits[QRandomGenerator::global()->bounded(36)]);
    return QString::number(QDateTime::currentMSecsSinceEpoch()) + QLatin1Char('-') + random;
}

QStringList words(const QString &text, const QStringList &before) {
    if (!before.isEmpty() && text == before.join(QStringLiteral(", ")))
        return before;
    QStringList out;
    for (const QString &part : text.split(QLatin1Char(',')))
        if (const QString word = part.trimmed(); !word.isEmpty() && !out.contains(word))
            out << word;
    return out;
}

std::optional<SavedView> build(const Draft &draft, const SavedView *base, QString *error) {
    const auto refuse = [error](const QString &why) -> std::optional<SavedView> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    // Trimmed, as the web app's sanitizeView and its save dialog trim them.
    const QString name = draft.name.trimmed();
    QString icon = draft.icon.trimmed();
    // The icon field cuts what is typed or pasted at maxIconLength UTF-16
    // units, which can leave half of an emoji, no character at all.
    if (!icon.isEmpty() && icon.back().isHighSurrogate())
        icon.chop(1);
    if (name.isEmpty())
        return refuse(tr("A view needs a name"));
    if (name.size() > maxNameLength)
        return refuse(tr("A view's name can be at most %1 characters").arg(maxNameLength));
    if (icon.size() > maxIconLength)
        return refuse(tr("A view's icon can be at most %1 characters").arg(maxIconLength));
    if (!QStringList{"all", "unread", "read"}.contains(draft.readState))
        return refuse(tr("A view lists all, unread, or read articles"));
    if (!QStringList{"newest", "oldest"}.contains(draft.sort))
        return refuse(tr("A view sorts newest or oldest first"));

    QJsonObject source;
    if (draft.source.isEmpty()) {
        if (!base)
            return refuse(tr("A view needs a source"));
        source = base->view.value("source").toObject();
    } else if (base && !sourceEditable(*base)) {
        return refuse(tr("This view's source is set in the web app"));
    } else if (!editableSources.contains(draft.source)) {
        return refuse(tr("A view lists all articles, starred ones, or those of tags"));
    } else if (draft.source == QLatin1String("tags")) {
        // Kept whole, unlike words: a tag matches the feeds' own only as itself.
        QStringList tags = draft.tags;
        const Fit fit = fitWords(tags);
        if (fit == Fit::TooMany)
            return refuse(tr("A view can have at most %1 tags").arg(maxWords));
        if (fit == Fit::TooLong)
            return refuse(tr("A tag can be at most %1 characters").arg(maxWordLength));
        if (tags.isEmpty())
            return refuse(tr("Pick at least one tag"));
        for (const QString &tag : std::as_const(tags))
            if (!Sync::storableTag(tag))
                return refuse(tr("No feed can carry the tag %1").arg(FeedParser::cleanText(tag)));
        source = QJsonObject{{"kind", draft.source}, {"tags", QJsonArray::fromStringList(tags)}};
    } else {
        source = QJsonObject{{"kind", draft.source}};
    }

    QStringList include = draft.include, exclude = draft.exclude;
    switch (fitWords(include)) {
    case Fit::TooMany: return refuse(tr("A view can have at most %1 keywords").arg(maxWords));
    case Fit::TooLong: return refuse(tr("A keyword can be at most %1 characters").arg(maxWordLength));
    case Fit::Fits: break;
    }
    switch (fitWords(exclude)) {
    case Fit::TooMany: return refuse(tr("A view can have at most %1 mute words").arg(maxWords));
    case Fit::TooLong: return refuse(tr("A mute word can be at most %1 characters").arg(maxWordLength));
    case Fit::Fits: break;
    }

    SavedView saved = base ? *base : SavedView{newId(), {}, {}};
    QJsonObject &view = saved.view;
    view.insert("id", saved.id);
    view.insert("name", name);
    if (icon.isEmpty())
        view.remove("icon");
    else
        view.insert("icon", icon);
    if (!view.contains("order"))
        view.insert("order", 0);
    view.insert("source", source);
    view.insert("readState", draft.readState);
    view.insert("sort", draft.sort);
    // The mute words never go into the view, which is what is published.
    QJsonObject keywords = view.value("keywords").toObject();
    keywords.remove("exclude");
    if (include.isEmpty())
        keywords.remove("include");
    else
        keywords.insert("include", QJsonArray::fromStringList(include));
    if (keywords.isEmpty())
        view.remove("keywords");
    else
        view.insert("keywords", keywords);
    saved.exclude = exclude;
    return saved;
}

qsizetype indexOf(const QList<SavedView> &views, const QString &id) {
    for (qsizetype i = 0; i < views.size(); ++i)
        if (views.at(i).id == id)
            return i;
    return -1;
}

QString changedMeanwhile() { return tr("The view changed; try again"); }

QString full(const QList<SavedView> &views) {
    return views.size() >= Sync::maxViews ? tr("There can be at most %1 saved views").arg(Sync::maxViews) : QString();
}

QString add(QList<SavedView> &views, const SavedView &view, qsizetype index) {
    if (const QString why = full(views); !why.isEmpty())
        return why;
    if (indexOf(views, view.id) >= 0)
        return tr("A view with this id is there already");
    views.insert(index < 0 || index > views.size() ? views.size() : index, view);
    return {};
}

QString replace(QList<SavedView> &views, const SavedView &view, const SavedView *expected) {
    const qsizetype at = indexOf(views, view.id);
    if (at < 0)
        return tr("The view is gone");
    if (!unchanged(views, at, expected))
        return changedMeanwhile();
    views[at] = view;
    return {};
}

QString remove(QList<SavedView> &views, const QString &id, const SavedView *expected) {
    const qsizetype at = indexOf(views, id);
    if (at < 0)
        return tr("The view is gone");
    if (!unchanged(views, at, expected))
        return changedMeanwhile();
    views.removeAt(at);
    return {};
}

QString move(QList<SavedView> &views, const QString &id, qsizetype index) {
    const qsizetype at = indexOf(views, id);
    if (at < 0)
        return tr("The view is gone");
    views.move(at, qBound<qsizetype>(0, index, views.size() - 1));
    return {};
}

int save(Store &store, const std::function<QString(QList<SavedView> &)> &change, QString *problem) {
    problem->clear();
    const bool saved = store.transaction([&] {
        QList<SavedView> views = store.savedViews();
        if (!store.lastError().isEmpty())
            return false;
        *problem = change(views);
        if (!problem->isEmpty())
            return false;
        // As the web app's normalizeViews numbers them.
        for (qsizetype i = 0; i < views.size(); ++i)
            views[i].view.insert("order", int(i));
        // Read in this transaction, so the mark goes to the account the
        // views were read under; with none set there is nothing to mark.
        const SyncAccount account = store.syncAccount();
        if (!store.lastError().isEmpty())
            return false;
        return store.replaceSavedViews(views) && (account.pubkey.isEmpty() || store.markViewsEdited(account.pubkey));
    });
    if (saved)
        return 1;
    return problem->isEmpty() ? -1 : 0;
}
}
