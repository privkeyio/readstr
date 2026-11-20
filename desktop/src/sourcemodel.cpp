#include "sourcemodel.h"

#include <QCollator>
#include <QMap>

SourceModel::SourceModel(QObject *parent) : QAbstractListModel(parent) {
    setLibrary({}, {});
}

int SourceModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }

QHash<int, QByteArray> SourceModel::roleNames() const {
    return {{KindRole, "kind"},         {FeedIdRole, "feedId"}, {TitleRole, "title"},
            {UrlRole, "url"},           {CategoryRole, "category"}, {CountRole, "unread"},
            {LastErrorRole, "lastError"}, {SelectableRole, "selectable"}};
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
        }
        return {};
    case FeedIdRole: return row.feedId;
    case TitleRole: return row.title;
    case UrlRole: return row.url;
    case CategoryRole: return row.category;
    case CountRole: return row.count;
    case LastErrorRole: return row.lastError;
    case SelectableRole: return row.kind != Kind::Category;
    default: return {};
    }
}

void SourceModel::setLibrary(const QList<Feed> &feeds, const LibraryCounts &counts) {
    QList<Row> rows{{Kind::All, 0, {}, {}, {}, {}, counts.unread},
                    {Kind::Unread, 0, {}, {}, {}, {}, counts.unread},
                    {Kind::Starred, 0, {}, {}, {}, {}, counts.starred}};

    // The feeds come sorted by title; categories sort by name the way the
    // user's language does.
    QList<Feed> uncategorized;
    QMap<QString, QList<Feed>> byCategory;
    for (const Feed &feed : feeds) {
        if (feed.category.isEmpty())
            uncategorized << feed;
        else
            byCategory[feed.category] << feed;
    }
    QStringList categories = byCategory.keys();
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(categories.begin(), categories.end(), collator);

    auto add = [&rows](const Feed &feed) {
        rows << Row{Kind::Feed, feed.id, feed.title, feed.url, feed.category, feed.lastError, feed.unread};
    };
    if (!uncategorized.isEmpty()) {
        rows << Row{Kind::Category, 0, {}, {}, {}, {}, 0};
        for (const Feed &feed : uncategorized)
            add(feed);
    }
    for (const QString &category : categories) {
        int unread = 0;
        for (const Feed &feed : byCategory.value(category))
            unread += feed.unread;
        rows << Row{Kind::Category, 0, category, {}, category, {}, unread};
        for (const Feed &feed : byCategory.value(category))
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
            || before.lastError != after.lastError) {
            m_rows[i] = after;
            emit dataChanged(index(int(i)), index(int(i)));
        }
    }
}

bool SourceModel::isSelectable(int index) const {
    return index >= 0 && index < m_rows.size() && m_rows.at(index).kind != Kind::Category;
}

int SourceModel::rowOf(Kind kind, qint64 feedId) const {
    for (qsizetype i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).kind == kind && m_rows.at(i).feedId == feedId)
            return int(i);
    return -1;
}

int SourceModel::step(int from, int step) const {
    const int direction = step < 0 ? -1 : 1;
    int remaining = qAbs(step), found = from;
    for (int i = from + direction; i >= 0 && i < m_rows.size() && remaining > 0; i += direction) {
        if (isSelectable(i)) {
            found = i;
            --remaining;
        }
    }
    return found;
}
