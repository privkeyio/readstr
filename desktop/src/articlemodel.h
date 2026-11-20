#pragma once

#include <QAbstractListModel>
#include <QList>

#include "store.h"

// The article list of the view picked in the sources pane, newest first,
// without the articles' content (the reader loads that for one article).
class ArticleModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        ArticleIdRole = Qt::UserRole + 1,
        FeedIdRole,
        FeedTitleRole,
        TitleRole,
        UrlRole,
        AuthorRole,
        SummaryRole,
        PublishedRole,
        ReadRole,
        StarredRole,
    };

    explicit ArticleModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_articles.size()); }
    void setArticles(const QList<Article> &articles);
    Article at(int row) const { return m_articles.value(row); }
    int rowOf(qint64 articleId) const;
    void setFlags(int row, bool read, bool starred);
    // The first unread row after `from`, or -1.
    int nextUnread(int from) const;

signals:
    void countChanged();

private:
    QList<Article> m_articles;
};
