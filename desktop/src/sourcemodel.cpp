#include "sourcemodel.h"

#include <QCollator>
#include <QMap>
#include <QSet>
#include <QUrl>
#include <limits>

#include "feedparser.h"
#include "library.h"
#include "nip19.h"
#include "savedviews.h"

namespace {
// A tag as the sources pane names it: cleaned like a category's name, and
// without the invisible format characters (zero-width spaces, byte order
// marks, soft hyphens) that would make two different tags look the same,
// save the zero-width joiner and the tag characters that emoji are built
// from. The tags themselves are kept as the web app stored them, since a
// saved view of tags matches them exactly.
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
}

SourceModel::SourceModel(QObject *parent) : QAbstractListModel(parent) {
    setLibrary({}, {});
}

int SourceModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }

QHash<int, QByteArray> SourceModel::roleNames() const {
    return {{KindRole, "kind"},         {FeedIdRole, "feedId"}, {TitleRole, "title"},
            {UrlRole, "url"},           {CategoryRole, "category"}, {CountRole, "unread"},
            {LastErrorRole, "lastError"}, {SelectableRole, "selectable"}, {NostrRole, "nostr"},
            {IncompleteRole, "incomplete"}, {ViewIdRole, "viewId"}, {IconRole, "icon"},
            {UnsupportedRole, "unsupported"}, {UnknownSourceRole, "unknownSource"}, {ColorRole, "categoryColor"}};
}

QVariant SourceModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case KindRole:
        switch (row.kind) {
        case Kind::All: return QStringLiteral("all");
        case Kind::Unread: return QStringLiteral("unread");
        case Kind::Starred: return QStringLiteral("starred");
        case Kind::Category: return QStringLiteral("category");
        case Kind::Feed: return QStringLiteral("feed");
        case Kind::ViewHeading: return QStringLiteral("views");
        case Kind::View: return QStringLiteral("view");
        case Kind::TagHeading: return QStringLiteral("tags");
        case Kind::Tag: return QStringLiteral("tag");
        }
        return {};
    case FeedIdRole: return row.feedId;
    case TitleRole: return row.title;
    case UrlRole: return row.url;
    case CategoryRole: return row.category;
    case CountRole: return row.count;
    case LastErrorRole: return row.lastError;
    case SelectableRole: return isSelectable(index.row());
    case NostrRole: return row.nostr;
    case IncompleteRole: return row.incomplete;
    case ViewIdRole: return row.viewId;
    case IconRole: return row.icon;
    case UnsupportedRole: return row.unsupported;
    case UnknownSourceRole: return row.unknownSource;
    case ColorRole: return row.color;
    default: return {};
    }
}

void SourceModel::setLibrary(const QList<Feed> &feeds, const LibraryCounts &counts, const QList<SavedView> &views) {
    QList<Row> rows{{Kind::All, 0, {}, {}, {}, {}, counts.unread},
                    {Kind::Unread, 0, {}, {}, {}, {}, counts.unread},
                    {Kind::Starred, 0, {}, {}, {}, {}, counts.starred}};
    if (!views.isEmpty()) {
        Row heading;
        heading.kind = Kind::ViewHeading;
        rows << heading;
    }
    for (const SavedView &view : views) {
        Row row;
        row.kind = Kind::View;
        row.title = SavedViews::name(view);
        row.viewId = view.id;
        row.icon = SavedViews::icon(view);
        const SavedViews::Evaluation evaluation = SavedViews::evaluate(view);
        row.unsupported = evaluation.unsupported;
        row.unknownSource = evaluation.unknownSource;
        rows << row;
    }

    // The feeds come sorted by title; categories and tags sort by name the
    // way the user's language does.
    QList<Feed> uncategorized;
    QMap<QString, QList<Feed>> byCategory;
    // Each tag by the name it shows as, with its spellings, and its feeds
    // and their unread count, which count a feed once whichever spellings
    // of it the feed carries. One that cleans away to nothing has no name to
    // show.
    struct TagFeeds {
        QSet<QString> spellings;
        int feeds = 0;
        int unread = 0;
    };
    QHash<QString, TagFeeds> byTag;
    for (const Feed &feed : feeds) {
        if (feed.category.isEmpty())
            uncategorized << feed;
        else
            byCategory[feed.category] << feed;
        QSet<QString> names;
        for (const QString &tag : feed.tags) {
            const QString name = tagName(tag);
            if (name.isEmpty())
                continue;
            TagFeeds &entry = byTag[name];
            entry.spellings.insert(tag);
            if (!names.contains(name)) {
                names.insert(name);
                ++entry.feeds;
                entry.unread += feed.unread;
            }
        }
    }
    QStringList categories = byCategory.keys();
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(categories.begin(), categories.end(), collator);

    // Past maxTagRows, those carried by the most feeds, ties by name.
    QStringList tagNames = byTag.keys();
    if (tagNames.size() > maxTagRows) {
        const auto before = [&byTag](const QString &a, const QString &b) {
            const int x = byTag.constFind(a)->feeds, y = byTag.constFind(b)->feeds;
            return x != y ? x > y : a < b;
        };
        std::nth_element(tagNames.begin(), tagNames.begin() + maxTagRows, tagNames.end(), before);
        tagNames.resize(maxTagRows);
    }
    QList<Row> tagRows;
    for (const QString &name : std::as_const(tagNames)) {
        const TagFeeds &entry = *byTag.constFind(name);
        Row row;
        row.kind = Kind::Tag;
        row.title = name;
        row.tag = name;
        row.tags = QStringList(entry.spellings.cbegin(), entry.spellings.cend());
        std::sort(row.tags.begin(), row.tags.end());
        row.count = entry.unread;
        tagRows << row;
    }
    std::sort(tagRows.begin(), tagRows.end(), [&collator](const Row &a, const Row &b) {
        const int order = collator.compare(a.title, b.title);
        return order != 0 ? order < 0 : a.tag < b.tag;
    });
    if (!tagRows.isEmpty()) {
        Row heading;
        heading.kind = Kind::TagHeading;
        rows << heading;
        rows << tagRows;
    }

    // Names from OPML are stored as the file had them, so they are cleaned
    // for display here like a feed's own text: no line breaks, and no
    // bidirectional overrides that would show them reversed.
    // A title that cleans away to nothing still needs a name in the list:
    // its address, without the credentials that only disguise it. A Nostr
    // author's npub, their title until the profile names them, shows
    // shortened. A refresh error says more than a note that the refresh
    // missed something, so it takes the note's place.
    auto add = [&rows](const Feed &feed) {
        QString title = FeedParser::cleanText(feed.title);
        if (title.isEmpty())
            title = FeedParser::cleanText(QUrl(feed.url).adjusted(QUrl::RemoveUserInfo).toString());
        const bool nostr = Library::isNostrFeed(feed);
        rows << Row{Kind::Feed,
                    feed.id,
                    Nip19::shortNpub(title, feed.url),
                    feed.url,
                    FeedParser::cleanText(feed.category),
                    feed.lastError,
                    feed.unread,
                    nostr,
                    nostr && feed.incomplete && feed.lastError.isEmpty()};
    };
    if (!uncategorized.isEmpty()) {
        rows << Row{Kind::Category, 0, {}, {}, {}, {}, 0};
        for (const Feed &feed : uncategorized)
            add(feed);
    }
    for (const QString &category : categories) {
        const QList<Feed> members = byCategory.value(category);
        int unread = 0;
        for (const Feed &feed : members)
            unread += feed.unread;
        const QString name = FeedParser::cleanText(category);
        Row heading{Kind::Category, 0, name, {}, name, {}, unread};
        // Every feed of a category has the category's own style.
        heading.icon = members.first().categoryIcon;
        heading.color = members.first().categoryColor;
        rows << heading;
        for (const Feed &feed : members)
            add(feed);
    }

    bool sameShape = rows.size() == m_rows.size();
    for (qsizetype i = 0; sameShape && i < rows.size(); ++i)
        sameShape = rows.at(i).sameSource(m_rows.at(i));
    if (!sameShape) {
        beginResetModel();
        m_rows = rows;
        endResetModel();
        emit countChanged();
        return;
    }
    for (qsizetype i = 0; i < rows.size(); ++i) {
        const Row &before = m_rows.at(i), &after = rows.at(i);
        if (before.title != after.title || before.url != after.url || before.count != after.count
            || before.lastError != after.lastError || before.nostr != after.nostr
            || before.incomplete != after.incomplete || before.icon != after.icon
            || before.unsupported != after.unsupported || before.unknownSource != after.unknownSource
            || before.color != after.color || before.tags != after.tags) {
            m_rows[i] = after;
            emit dataChanged(index(int(i)), index(int(i)));
        }
    }
}

bool SourceModel::isSelectable(int index) const {
    if (index < 0 || index >= m_rows.size())
        return false;
    const Row &row = m_rows.at(index);
    return row.kind != Kind::Category && row.kind != Kind::ViewHeading && row.kind != Kind::TagHeading
           && !row.unknownSource;
}

int SourceModel::rowOf(Kind kind, qint64 feedId) const {
    for (qsizetype i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).kind == kind && m_rows.at(i).feedId == feedId)
            return int(i);
    return -1;
}

int SourceModel::rowOfView(const QString &viewId) const {
    for (qsizetype i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).kind == Kind::View && m_rows.at(i).viewId == viewId)
            return int(i);
    return -1;
}

int SourceModel::rowOfTag(const QString &name) const {
    for (qsizetype i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).kind == Kind::Tag && m_rows.at(i).tag == name)
            return int(i);
    return -1;
}

int SourceModel::rowOf(const Row &row) const {
    int found = -1;
    if (row.kind == Kind::View)
        found = rowOfView(row.viewId);
    else if (row.kind == Kind::Tag)
        found = rowOfTag(row.tag);
    else
        found = rowOf(row.kind, row.feedId);
    // A view selected until its source turned to one that cannot list here
    // stays selected no more than it could be selected anew.
    return isSelectable(found) ? found : -1;
}

int SourceModel::step(int from, int step) const {
    const int direction = step < 0 ? -1 : 1;
    // qAbs(INT_MIN) overflows; no list has that many rows anyway.
    int remaining = step == std::numeric_limits<int>::min() ? std::numeric_limits<int>::max() : qAbs(step);
    int found = from;
    for (int i = from + direction; i >= 0 && i < m_rows.size() && remaining > 0; i += direction) {
        if (isSelectable(i)) {
            found = i;
            --remaining;
        }
    }
    return found;
}
