#include "backend.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QRandomGenerator>

#include "filepicker.h"
#include "sanitizer.h"

// Lives on the worker thread and owns that thread's connection to the
// library, opened on first use there.
class LibraryWorker : public QObject {
public:
    Store *store(QString *error) {
        if (!m_open) {
            m_open = m_store.open();
            if (!m_open) {
                *error = m_store.lastError();
                return nullptr;
            }
        }
        return &m_store;
    }

private:
    Store m_store;
    bool m_open = false;
};

namespace {
// One round of a subscription's discovery, examined on the worker.
struct SearchStep {
    bool finished = false;
    Library::Subscription subscription;
    QString error;
};
}

Backend::Backend(FilePicker *picker, QObject *parent)
    : QObject(parent), m_fetcher(std::make_unique<Fetcher>()), m_picker(picker) {
    m_openUrl = [](const QUrl &url) { return QDesktopServices::openUrl(url); };

    m_worker = new LibraryWorker;
    m_worker->moveToThread(&m_thread);
    // Deleted on its own thread as that thread ends, so its connection to
    // the library closes where it was opened.
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.setObjectName(QStringLiteral("readstr-library"));
    m_thread.start();

    m_autoRefresh.setSingleShot(true);
    connect(&m_autoRefresh, &QTimer::timeout, this, &Backend::refreshAll);

    if (m_picker) {
        connect(m_picker, &FilePicker::openSelected, this, &Backend::importFrom);
        connect(m_picker, &FilePicker::saveSelected, this, &Backend::exportTo);
        connect(m_picker, &FilePicker::failed, this, [this](const QString &message) { setStatus(message, true); });
    }

    m_storeOpen = m_store.open();
    if (!m_storeOpen)
        setStatus(tr("Cannot open the library: %1").arg(m_store.lastError()), true);
    reloadLibrary();
}

Backend::~Backend() {
    // Replies still in flight are dropped with the fetcher; their callbacks
    // see m_closing and do nothing. A job already running on the worker
    // finishes, and the ones queued behind it never start.
    m_closing = true;
    m_autoRefresh.stop();
    m_fetcher.reset();
    m_thread.quit();
    m_thread.wait();
}

template <typename Result>
void Backend::onWorker(std::function<Result(Store &)> work, std::function<void(const Result &)> done) {
    QMetaObject::invokeMethod(
        m_worker,
        [this, work = std::move(work), done = std::move(done)]() {
            Result result;
            QString error;
            if (Store *store = m_worker->store(&error))
                result = work(*store);
            else
                result.error = error;
            // The window waits for this thread before it goes, so it is
            // still there to take the result.
            QMetaObject::invokeMethod(
                this, [done, result] { done(result); }, Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void Backend::setStatus(const QString &message, bool error) {
    if (message == m_status && error == m_statusError)
        return;
    m_status = message;
    m_statusError = error;
    emit statusChanged();
}

QString Backend::sourceKind() const {
    return m_sources.data(m_sources.index(m_sourceIndex), SourceModel::KindRole).toString();
}

QString Backend::sourceTitle() const { return m_sources.row(m_sourceIndex).title; }

qint64 Backend::selectedFeedId() const {
    const SourceModel::Row row = m_sources.row(m_sourceIndex);
    return row.kind == SourceModel::Kind::Feed ? row.feedId : 0;
}

QVariant Backend::articlePublished() const {
    return m_article.published.isValid() ? QVariant(m_article.published) : QVariant();
}

ArticleFilter Backend::currentFilter() const {
    ArticleFilter filter;
    filter.limit = viewLimit;
    filter.withContent = false;
    const SourceModel::Row row = m_sources.row(m_sourceIndex);
    if (row.kind == SourceModel::Kind::Unread)
        filter.unreadOnly = true;
    else if (row.kind == SourceModel::Kind::Starred)
        filter.starredOnly = true;
    else if (row.kind == SourceModel::Kind::Feed)
        filter.feedId = row.feedId;
    return filter;
}

void Backend::reloadLibrary() {
    const SourceModel::Row selected = m_sources.row(m_sourceIndex);
    QList<Feed> feeds;
    LibraryCounts counts;
    if (m_storeOpen) {
        feeds = m_store.feeds();
        if (!m_store.lastError().isEmpty())
            setStatus(tr("Cannot read the library: %1").arg(m_store.lastError()), true);
        counts = m_store.counts().value_or(LibraryCounts{});
    }
    m_sources.setLibrary(feeds, counts);
    if (m_feedCount != feeds.size()) {
        m_feedCount = int(feeds.size());
        emit libraryChanged();
    }
    const int row = m_sources.rowOf(selected.kind, selected.feedId);
    m_sourceIndex = row < 0 ? 0 : row;
    emit sourceChanged();
    loadArticles();
}

void Backend::reloadCounts() {
    if (!m_storeOpen)
        return;
    const QList<Feed> feeds = m_store.feeds();
    if (const auto counts = m_store.counts(); counts && m_store.lastError().isEmpty())
        m_sources.setLibrary(feeds, *counts);
}

void Backend::loadArticles() {
    QList<Article> list;
    if (m_storeOpen) {
        // Taken before the list, so marking the view read never reaches an
        // article that arrived after it was shown.
        m_viewLastId = m_store.counts().value_or(LibraryCounts{}).lastId;
        list = m_store.articles(currentFilter());
        if (!m_store.lastError().isEmpty())
            setStatus(tr("Cannot read the library: %1").arg(m_store.lastError()), true);
    }
    m_articles.setArticles(list);
    // The reader keeps its article across a reload, with fresh flags, even
    // when the view no longer lists it (a read article in Unread).
    if (m_article.id) {
        if (const auto article = m_storeOpen ? m_store.article(m_article.id) : std::nullopt) {
            m_article.read = article->read;
            m_article.starred = article->starred;
        } else {
            m_article = Article{};
            m_articleHtml.clear();
        }
    }
    m_articleIndex = m_article.id ? m_articles.rowOf(m_article.id) : -1;
    emit articleChanged();
}

void Backend::selectSource(int row) {
    if (!m_sources.isSelectable(row))
        return;
    m_sourceIndex = row;
    m_article = Article{};
    m_articleHtml.clear();
    emit sourceChanged();
    loadArticles();
}

void Backend::moveSource(int step) {
    const int row = m_sources.step(m_sourceIndex, step);
    if (row != m_sourceIndex)
        selectSource(row);
}

void Backend::showArticle(const Article &article) {
    m_article = article;
    m_articleHtml = article.content.isEmpty() ? Sanitizer::plainToRichText(article.summary)
                                              : Sanitizer::toRichText(article.content, QUrl(article.url));
    emit articleChanged();
}

void Backend::selectArticle(int row) {
    if (!m_storeOpen || row < 0 || row >= m_articles.count())
        return;
    std::optional<Article> article = m_store.article(m_articles.at(row).id);
    if (!article) {
        setStatus(tr("Cannot read the article: %1").arg(m_store.lastError()), true);
        return;
    }
    m_articleIndex = row;
    if (!article->read) {
        if (m_store.setRead(article->id, true)) {
            article->read = true;
            m_articles.setFlags(row, true, article->starred);
            reloadCounts();
        } else {
            setStatus(tr("Cannot mark the article read: %1").arg(m_store.lastError()), true);
        }
    }
    showArticle(*article);
}

void Backend::moveArticle(int step) {
    const int count = m_articles.count();
    if (count == 0)
        return;
    const int target = m_articleIndex < 0 ? 0 : qBound(0, m_articleIndex + step, count - 1);
    if (target != m_articleIndex)
        selectArticle(target);
}

void Backend::nextUnread() {
    const int row = m_articles.nextUnread(m_articleIndex);
    if (row < 0)
        setStatus(tr("No more unread articles in this view"));
    else
        selectArticle(row);
}

void Backend::setFlags(bool read, bool starred) {
    if (!m_storeOpen || !m_article.id)
        return;
    if (read != m_article.read && !m_store.setRead(m_article.id, read)) {
        setStatus(tr("Cannot change the article: %1").arg(m_store.lastError()), true);
        return;
    }
    if (starred != m_article.starred && !m_store.setStarred(m_article.id, starred)) {
        setStatus(tr("Cannot change the article: %1").arg(m_store.lastError()), true);
        return;
    }
    m_article.read = read;
    m_article.starred = starred;
    m_articles.setFlags(m_articles.rowOf(m_article.id), read, starred);
    reloadCounts();
    emit articleChanged();
}

void Backend::toggleRead() { setFlags(!m_article.read, m_article.starred); }
void Backend::toggleStar() { setFlags(m_article.read, !m_article.starred); }

void Backend::openOriginal() {
    if (m_article.id && !m_article.url.isEmpty())
        openLink(m_article.url);
}

bool Backend::openLink(const QString &link) {
    const QUrl url(link.trimmed());
    if (!Library::isWebUrl(url)) {
        setStatus(tr("Only web links (http and https) can be opened"), true);
        return false;
    }
    if (!m_openUrl(url)) {
        setStatus(tr("Cannot open %1").arg(url.toDisplayString()), true);
        return false;
    }
    return true;
}

void Backend::markViewRead() {
    if (!m_storeOpen)
        return;
    const int changed = m_store.markRead(currentFilter(), m_viewLastId);
    if (changed < 0) {
        setStatus(tr("Cannot mark the view read: %1").arg(m_store.lastError()), true);
        return;
    }
    setStatus(tr("Marked %n articles read", nullptr, changed));
    reloadCounts();
    loadArticles();
}

void Backend::addFeed(const QString &address) {
    if (m_adding)
        return;
    QString text = address.trimmed();
    // A bare host is what people paste most; the web means https now.
    if (!text.isEmpty() && !text.contains(QLatin1String("://")))
        text.prepend(QLatin1String("https://"));
    const QUrl url(text, QUrl::StrictMode);
    if (!Library::isWebUrl(url)) {
        m_addError = tr("Enter a web address, like https://example.com/feed.xml");
        emit addChanged();
        return;
    }
    if (!m_storeOpen) {
        m_addError = tr("Cannot open the library: %1").arg(m_store.lastError());
        emit addChanged();
        return;
    }
    m_adding = true;
    m_addError.clear();
    emit addChanged();
    searchRound(std::make_shared<Discovery::Search>(url));
}

void Backend::clearAddError() {
    if (m_addError.isEmpty())
        return;
    m_addError.clear();
    emit addChanged();
}

void Backend::searchRound(const std::shared_ptr<Discovery::Search> &search) {
    // Fetched here, at most a few at a time; the bodies go to the worker,
    // which parses them, decides on the next round, and saves the feed.
    const QList<FetchRequest> requests = search->requests();
    auto results = std::make_shared<QList<FetchResult>>(requests.size());
    m_fetcher->fetchEachLater(
        requests, [results](qsizetype index, const FetchResult &result) { (*results)[index] = result; },
        [this, search, results] {
            if (m_closing)
                return;
            onWorker<SearchStep>(
                [search, results](Store &store) {
                    SearchStep step;
                    search->examine(*results);
                    step.finished = search->finished();
                    if (step.finished)
                        step.subscription = Library::subscribe(store, search->result());
                    return step;
                },
                [this, search](const SearchStep &step) {
                    if (!step.error.isEmpty()) {
                        subscribeFinished(Library::Subscription{0, false, 0, step.error});
                    } else if (step.finished) {
                        subscribeFinished(step.subscription);
                    } else {
                        searchRound(search);
                    }
                });
        });
}

void Backend::subscribeFinished(const Library::Subscription &subscription) {
    m_adding = false;
    if (!subscription.error.isEmpty()) {
        m_addError = subscription.error;
        emit addChanged();
        return;
    }
    reloadLibrary();
    const QString title = m_store.feed(subscription.id).value_or(Feed{}).title;
    if (subscription.created)
        setStatus(tr("Subscribed to %1 (%n articles)", nullptr, subscription.added).arg(title));
    else
        setStatus(tr("Already subscribed to %1").arg(title));
    const int row = m_sources.rowOf(SourceModel::Kind::Feed, subscription.id);
    if (row >= 0)
        selectSource(row);
    emit addChanged();
    emit subscribed(subscription.id);
}

bool Backend::unsubscribe(qint64 feedId) {
    if (!m_storeOpen)
        return false;
    const std::optional<Feed> feed = m_store.feed(feedId);
    if (!feed || !m_store.removeFeed(feedId)) {
        setStatus(tr("Cannot unsubscribe: %1").arg(m_store.lastError()), true);
        return false;
    }
    if (m_refreshing)
        m_removedDuringRefresh.insert(feedId);
    setStatus(tr("Unsubscribed from %1").arg(feed->title));
    reloadLibrary();
    return true;
}

void Backend::refreshAll() {
    if (!m_storeOpen || m_refreshing)
        return;
    const QList<Feed> feeds = m_store.feeds();
    if (!m_store.lastError().isEmpty()) {
        setStatus(tr("Cannot read the library: %1").arg(m_store.lastError()), true);
        return;
    }
    if (feeds.isEmpty()) {
        scheduleAutoRefresh();
        return;
    }
    refresh(feeds);
}

void Backend::refreshFeed(qint64 feedId) {
    if (!m_storeOpen || m_refreshing)
        return;
    const std::optional<Feed> feed = m_store.feed(feedId);
    if (!feed) {
        setStatus(tr("Cannot refresh: %1").arg(m_store.lastError()), true);
        return;
    }
    refresh({*feed});
}

void Backend::refresh(const QList<Feed> &targets) {
    m_refreshing = true;
    m_refreshDone = 0;
    m_refreshTotal = int(targets.size());
    m_refreshFailed = 0;
    m_refreshAdded = 0;
    m_firstFailure.clear();
    m_removedDuringRefresh.clear();
    emit refreshChanged();

    QList<FetchRequest> requests;
    for (const Feed &feed : targets)
        requests << FetchRequest{QUrl(feed.url), feed.etag, feed.lastModified};
    // Each response goes to the worker as it arrives, so only the few in
    // flight hold a body.
    m_fetcher->fetchEachLater(requests, [this, targets](qsizetype index, const FetchResult &fetched) {
        if (m_closing)
            return;
        const Feed feed = targets.at(index);
        onWorker<Library::Refresh>(
            [feed, fetched](Store &store) { return Library::apply(store, feed, fetched); },
            [this, feed](const Library::Refresh &outcome) { refreshApplied(feed, outcome); });
    });
}

void Backend::refreshApplied(const Feed &feed, const Library::Refresh &outcome) {
    ++m_refreshDone;
    // A feed unsubscribed while its fetch was out fails to save, which is
    // no news to anyone.
    if (!outcome.error.isEmpty() && !m_removedDuringRefresh.contains(feed.id)) {
        ++m_refreshFailed;
        if (m_firstFailure.isEmpty())
            m_firstFailure = QStringLiteral("%1: %2").arg(outcome.title.isEmpty() ? feed.url : outcome.title,
                                                           outcome.error);
    } else {
        m_refreshAdded += outcome.added;
    }
    emit refreshChanged();
    if (m_refreshDone < m_refreshTotal)
        return;

    m_refreshing = false;
    emit refreshChanged();
    reloadLibrary();
    if (m_refreshFailed == 1)
        setStatus(tr("Cannot refresh %1").arg(m_firstFailure), true);
    else if (m_refreshFailed > 1)
        setStatus(tr("%n feeds could not be refreshed, among them %1", nullptr, m_refreshFailed).arg(m_firstFailure),
                  true);
    else if (m_refreshAdded > 0)
        setStatus(tr("%n new articles", nullptr, m_refreshAdded));
    else
        setStatus(tr("No new articles"));
    if (!m_autoRefresh.isActive())
        scheduleAutoRefresh();
    emit refreshFinished();

    // Feeds imported while this refresh ran have never been fetched.
    if (m_fetchNewFeeds) {
        m_fetchNewFeeds = false;
        QList<Feed> fresh;
        for (const Feed &candidate : m_store.feeds())
            if (!candidate.lastFetchedAt.isValid())
                fresh << candidate;
        if (!fresh.isEmpty())
            refresh(fresh);
    }
}

void Backend::startAutoRefresh() {
    m_autoRefreshOn = true;
    refreshAll();
}

int Backend::autoRefreshDelayMs(double unit) {
    const double base = autoRefreshMinutes * 60.0 * 1000.0;
    return int(base * (0.9 + 0.2 * qBound(0.0, unit, 1.0)));
}

void Backend::scheduleAutoRefresh() {
    if (m_autoRefreshOn)
        m_autoRefresh.start(autoRefreshDelayMs(QRandomGenerator::global()->generateDouble()));
}

void Backend::importOpml() {
    if (!m_picker) {
        setStatus(tr("No file chooser is available"), true);
        return;
    }
    m_picker->openOpml();
}

void Backend::exportOpml() {
    if (!m_picker) {
        setStatus(tr("No file chooser is available"), true);
        return;
    }
    m_picker->saveOpml(QStringLiteral("readstr-subscriptions.opml"));
}

void Backend::importFrom(const QUrl &url) {
    if (!url.isLocalFile()) {
        setStatus(tr("Only local files can be imported"), true);
        return;
    }
    if (!m_storeOpen)
        return;
    const QString path = url.toLocalFile();
    setStatus(tr("Importing %1…").arg(QFileInfo(path).fileName()));
    onWorker<Library::Import>([path](Store &store) { return Library::importFile(store, path); },
                              [this](const Library::Import &result) {
                                  if (!result.error.isEmpty()) {
                                      setStatus(tr("Cannot import: %1").arg(result.error), true);
                                      return;
                                  }
                                  reloadLibrary();
                                  setStatus(tr("Imported %n feeds (%1 already subscribed, %2 not supported yet)",
                                               nullptr, result.added)
                                                .arg(result.skipped)
                                                .arg(result.unsupported));
                                  emit imported();
                                  if (result.added == 0)
                                      return;
                                  if (m_refreshing)
                                      m_fetchNewFeeds = true;
                                  else
                                      refreshAll();
                              });
}

void Backend::exportTo(const QUrl &url) {
    if (!url.isLocalFile()) {
        setStatus(tr("Only local files can be written"), true);
        return;
    }
    if (!m_storeOpen)
        return;
    const QString path = url.toLocalFile();
    QString error;
    const QByteArray opml = Library::exportOpml(m_store, &error);
    if (!error.isEmpty() || !Library::writePrivately(path, opml, &error)) {
        setStatus(tr("Cannot export: %1").arg(error), true);
        return;
    }
    setStatus(tr("Exported %n feeds to %1", nullptr, m_feedCount).arg(path));
}
