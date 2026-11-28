#pragma once

#include <QAbstractListModel>
#include <QList>

#include "store.h"

// The sources pane: "All articles", "Unread", and "Starred", then the saved
// views under their heading, in the web app's order, when there are any,
// then the tags the feeds carry under theirs, by name, when there are any,
// then the feeds, those without a category first and then each category
// under its own heading, by name, with its color and icon. Counts are unread
// articles, except Starred's; views have none. A tag lists the articles of
// the feeds that carry it, in any spelling that shows the same, and a
// category's heading those of its feeds.
class SourceModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    // The most tags listed: those carried by the most feeds, ties by name. A
    // subscription list may give thousands, and every reload builds and
    // sorts their rows on the window's thread.
    static constexpr int maxTagRows = 500;

    enum class Kind { All, Unread, Starred, Category, Feed, ViewHeading, View, TagHeading, Tag };

    struct Row {
        Kind kind = Kind::All;
        qint64 feedId = 0;
        QString title;
        QString url;
        // A feed's category, cleaned to show; a category heading's name as
        // the library keeps it, empty for the heading of the feeds without
        // one, which is the only one that cannot be selected.
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
        // A tag row's name as it shows (its title, and its identity), and
        // every spelling of it the feeds carry, which select its articles,
        // sorted; and a category heading's color (Sync::safeColor()), whose
        // icon is in `icon`.
        QString tag = {};
        QStringList tags = {};
        QString color = {};
        // A tag row's feeds, or a category heading's, by id, sorted, so a
        // selected tag or category whose feeds change lists theirs, whether
        // or not its spellings change too.
        QList<qint64> feedIds = {};
        // Same place in the list: a reload that keeps every row's identity
        // updates rows in place instead of resetting the view.
        bool sameSource(const Row &other) const {
            return kind == other.kind && feedId == other.feedId && category == other.category
                   && viewId == other.viewId && tag == other.tag;
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
        ColorRole,
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
    // The row of the tag that shows as `name`.
    int rowOfTag(const QString &name) const;
    // The heading of the category the library names `name`.
    int rowOfCategory(const QString &name) const;
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
