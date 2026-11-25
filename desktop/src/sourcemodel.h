#pragma once

#include <QAbstractListModel>
#include <QList>

#include "store.h"

// The sources pane: "All articles", "Unread", and "Starred", then the saved
// views under their heading, in the web app's order, when there are any,
// then the feeds, those without a category first and then each category
// under its own heading, by name. Counts are unread articles, except
// Starred's; views have none.
class SourceModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum class Kind { All, Unread, Starred, Category, Feed, ViewHeading, View };

    struct Row {
        Kind kind = Kind::All;
        qint64 feedId = 0;
        QString title;
        QString url;
        QString category;
        QString lastError;
        int count = 0;
        // A Nostr author's feed, whose url is the npub.
        bool nostr = false;
        // Its last refresh missed something, and no error since says more.
        bool incomplete = false;
        // A saved view's id, its icon, and the rules it holds that cannot
        // be applied here (SavedViews::Evaluation::unsupported).
        QString viewId = {};
        QString icon = {};
        QStringList unsupported = {};
        // A view that cannot list here (SavedViews::Evaluation::unknownSource),
        // shown disabled and never selected.
        bool unknownSource = false;
        // Same place in the list: a reload that keeps every row's identity
        // updates rows in place instead of resetting the view.
        bool sameSource(const Row &other) const {
            return kind == other.kind && feedId == other.feedId && category == other.category
                   && viewId == other.viewId;
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
        NostrRole,
        IncompleteRole,
        ViewIdRole,
        IconRole,
        UnsupportedRole,
        UnknownSourceRole,
    };

    explicit SourceModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_rows.size()); }
    void setLibrary(const QList<Feed> &feeds, const LibraryCounts &counts, const QList<SavedView> &views = {});
    Row row(int index) const { return m_rows.value(index); }
    bool isSelectable(int index) const;
    int rowOf(Kind kind, qint64 feedId = 0) const;
    int rowOfView(const QString &viewId) const;
    // The row of the source `row` describes, or -1 when it is gone or can no
    // longer be selected.
    int rowOf(const Row &row) const;
    // The nearest selectable row `step` rows away, or `from` when none.
    int step(int from, int step) const;

signals:
    void countChanged();

private:
    QList<Row> m_rows;
};
