#include "articlemodel.h"

#include "feedparser.h"

ArticleModel::ArticleModel(QObject *parent) : QAbstractListModel(parent) {}

int ArticleModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(m_articles.size());
}

QHash<int, QByteArray> ArticleModel::roleNames() const {
    return {{ArticleIdRole, "articleId"}, {FeedIdRole, "feedId"},   {FeedTitleRole, "feedTitle"},
            {TitleRole, "title"},         {UrlRole, "url"},         {AuthorRole, "author"},
            {SummaryRole, "summary"},     {PublishedRole, "published"}, {ReadRole, "read"},
            {StarredRole, "starred"}};
}

QVariant ArticleModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_articles.size())
        return {};
    const Article &article = m_articles.at(index.row());
    switch (role) {
    case ArticleIdRole: return article.id;
    case FeedIdRole: return article.feedId;
    // A feed name from OPML is stored as the file had it.
    case FeedTitleRole: return FeedParser::cleanText(article.feedTitle);
    case TitleRole: return article.title;
    case UrlRole: return article.url;
    case AuthorRole: return article.author;
    case SummaryRole: return article.summary;
    // An unknown date reaches QML as null rather than an invalid Date.
    case PublishedRole: return article.published.isValid() ? QVariant(article.published) : QVariant();
    case ReadRole: return article.read;
    case StarredRole: return article.starred;
    default: return {};
    }
}

void ArticleModel::setArticles(const QList<Article> &articles) {
    const bool countChanges = articles.size() != m_articles.size();
    beginResetModel();
    m_articles = articles;
    endResetModel();
    if (countChanges)
        emit countChanged();
}

int ArticleModel::rowOf(qint64 articleId) const {
    for (qsizetype i = 0; i < m_articles.size(); ++i)
        if (m_articles.at(i).id == articleId)
            return int(i);
    return -1;
}

void ArticleModel::setFlags(int row, bool read, bool starred) {
    if (row < 0 || row >= m_articles.size())
        return;
    Article &article = m_articles[row];
    if (article.read == read && article.starred == starred)
        return;
    article.read = read;
    article.starred = starred;
    emit dataChanged(index(row), index(row), {ReadRole, StarredRole});
}

int ArticleModel::nextUnread(int from) const {
    for (qsizetype i = qMax<qsizetype>(0, qsizetype(from) + 1); i < m_articles.size(); ++i)
        if (!m_articles.at(i).read)
            return int(i);
    return -1;
}
