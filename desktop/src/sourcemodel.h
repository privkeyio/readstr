#pragma once

#include <QAbstractListModel>
#include <QList>

#include "store.h"

// The sources pane: "All articles", "Unread", and "Starred", then the feeds,
// those without a category first and then each category under its own
// heading, by name. Counts are unread articles, except Starred's.
class SourceModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum class Kind { All, Unread, Starred, Category, Feed };

    struct Row {
        Kind kind = Kind::All;
        qint64 feedId = 0;
        QString title;
        QString url;
        QString category;
        QString lastError;
        int count = 0;
        // Same place in the list: a reload that keeps every row's identity
        // updates rows in place instead of resetting the view.
        bool sameSource(const Row &other) const {
            return kind == other.kind && feedId == other.feedId && category == other.category;
        }
    };

    enum Role {
        KindRole = Qt::UserRole + 1,
        FeedIdRole,
        TitleRole,
        UrlRole,
        CategoryRole,
        CountRole,
        LastErrorRole,
        SelectableRole,
    };

    explicit SourceModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_rows.size()); }
    void setLibrary(const QList<Feed> &feeds, const LibraryCounts &counts);
    Row row(int index) const { return m_rows.value(index); }
    bool isSelectable(int index) const;
    int rowOf(Kind kind, qint64 feedId = 0) const;
    // The nearest selectable row `step` rows away, or `from` when none.
    int step(int from, int step) const;

signals:
    void countChanged();

private:
    QList<Row> m_rows;
};
