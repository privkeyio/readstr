#include "savedviews.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include "feedparser.h"

namespace {
// The web app's sanitizeStrings keeps at most 50 words of 200 characters.
constexpr qsizetype maxWords = 50, maxWordLength = 200;

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
