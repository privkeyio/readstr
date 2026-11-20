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

QString richText(const Article &article) {
    return article.content.isEmpty() ? Sanitizer::plainToRichText(article.summary)
                                     : Sanitizer::toRichText(article.content, QUrl(article.url));
}
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
    // The user's own changes still queued behind a refresh are not lost
    // with it; nothing else writes now, so this store does not wait.
    if (m_storeOpen)
        for (const std::shared_ptr<UserWrite> &write : std::as_const(m_userWrites))
            if (!write->done)
                write->work(m_store);
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

void Backend::writeOnWorker(std::function<int(Store &)> work, std::function<void(const WriteResult &)> done) {
    auto write = std::make_shared<UserWrite>();
    write->work = std::move(work);
    m_userWrites << write;
    onWorker<WriteResult>(
        [write](Store &store) {
            WriteResult result;
            result.changed = write->work(store);
            if (result.changed < 0)
                result.error = store.lastError();
            write->done = true;
            return result;
        },
        [this, write, done = std::move(done)](const WriteResult &result) {
            m_userWrites.removeOne(write);
            done(result);
        });
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

QString Backend::articleUrl() const {
    // Credentials in a link only serve to disguise where it goes.
    return m_article.url.isEmpty() ? QString() : QUrl(m_article.url).adjusted(QUrl::RemoveUserInfo).toString();
}

ArticleFilter Backend::currentFilter() const {
    ArticleFilter filter;
    filter.limit = viewLimit;
    filter.withContent = false;
    // The open article stays in Unread or Starred after it no longer
    // belongs there, until the view changes, so a reload in the background
    // neither drops it from under the reader nor loses its place for J and K.
    filter.alsoId = m_article.id;
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
        hideRemoving(feeds);
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

void Backend::hideRemoving(QList<Feed> &feeds) const {
    if (!m_removing.isEmpty())
        feeds.removeIf([this](const Feed &feed) { return m_removing.contains(feed.id); });
}

void Backend::reloadCounts() {
    if (!m_storeOpen)
        return;
    QList<Feed> feeds = m_store.feeds();
    const auto counts = m_store.counts();
    if (!counts || !m_store.lastError().isEmpty())
        return;
    hideRemoving(feeds);
    // Another process can add, rename, or remove feeds, which moves rows;
    // the selection follows its source rather than its row.
    const SourceModel::Row selected = m_sources.row(m_sourceIndex);
    m_sources.setLibrary(feeds, *counts);
    const int row = m_sources.rowOf(selected.kind, selected.feedId);
    if (m_feedCount != feeds.size()) {
        m_feedCount = int(feeds.size());
        emit libraryChanged();
    }
    if (row == m_sourceIndex)
        return;
    // A feed that is gone leaves All articles selected, as after a reload,
    // and the list it showed goes with it.
    m_sourceIndex = row < 0 ? 0 : row;
    emit sourceChanged();
    if (row < 0)
        loadArticles();
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
    if (!m_pendingFlags.isEmpty())
        for (Article &article : list)
            applyPending(article);
    // A feed being unsubscribed takes its articles with it right away.
    if (!m_removing.isEmpty()) {
        list.removeIf([this](const Article &article) { return m_removing.contains(article.feedId); });
        if (m_removing.contains(m_article.feedId)) {
            m_article = Article{};
            m_articleHtml.clear();
        }
    }
    m_articles.setArticles(list);
    // The reader keeps its article across a reload, as the library has it
    // now: a refresh may have updated its text as well as its flags.
    if (m_article.id) {
        if (auto article = m_storeOpen ? m_store.article(m_article.id) : std::nullopt) {
            applyPending(*article);
            const bool textChanged = article->content != m_article.content || article->summary != m_article.summary
                                     || article->url != m_article.url;
            m_article = *article;
            if (textChanged)
                m_articleHtml = richText(m_article);
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
    m_articleHtml = richText(article);
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
    applyPending(*article);
    m_articleIndex = row;
    showArticle(*article);
    if (!article->read)
        saveFlags(*article, true, article->starred, tr("Cannot mark the article read: %1"));
}

void Backend::moveArticle(int step) {
    const int count = m_articles.count();
    if (count == 0)
        return;
    // Widened, so no step from QML overflows.
    const int target =
        m_articleIndex < 0 ? 0 : int(qBound(qint64(0), qint64(m_articleIndex) + step, qint64(count - 1)));
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
    saveFlags(m_article, read, starred, tr("Cannot change the article: %1"));
}

void Backend::applyPending(Article &article) const {
    const auto pending = m_pendingFlags.constFind(article.id);
    if (pending == m_pendingFlags.cend())
        return;
    article.read = pending->read;
    article.starred = pending->starred;
}

void Backend::showFlags(qint64 id, bool read, bool starred) {
    m_articles.setFlags(m_articles.rowOf(id), read, starred);
    if (m_article.id == id && (m_article.read != read || m_article.starred != starred)) {
        m_article.read = read;
        m_article.starred = starred;
        emit articleChanged();
    }
}

void Backend::saveFlags(const Article &before, bool read, bool starred, const QString &failure) {
    const qint64 id = before.id;
    const bool readChanged = read != before.read, starredChanged = starred != before.starred;
    if (!readChanged && !starredChanged)
        return;
    PendingFlags &pending = m_pendingFlags[id];
    pending.read = read;
    pending.starred = starred;
    ++pending.writes;
    showFlags(id, read, starred);
    // The worker takes writes in order, so each only needs what it changes.
    writeOnWorker(
        [id, read, starred, readChanged, starredChanged](Store &store) {
            return (!readChanged || store.setRead(id, read)) && (!starredChanged || store.setStarred(id, starred))
                       ? 1
                       : -1;
        },
        [this, id, failure](const WriteResult &result) {
            if (!result.error.isEmpty())
                setStatus(failure.arg(result.error), true);
            const auto pending = m_pendingFlags.find(id);
            if (pending != m_pendingFlags.end()) {
                if (--pending->writes > 0)
                    return;
                m_pendingFlags.erase(pending);
            }
            // The library has the flags asked for now, or the old ones back
            // where a write failed.
            if (const auto saved = m_store.article(id))
                showFlags(id, saved->read, saved->starred);
            reloadCounts();
        });
}

void Backend::toggleRead() { setFlags(!m_article.read, m_article.starred); }
void Backend::toggleStar() { setFlags(m_article.read, !m_article.starred); }

void Backend::openOriginal() {
    if (m_article.id && !m_article.url.isEmpty())
        openLink(m_article.url);
}

bool Backend::openLink(const QString &link) {
    // Credentials in a link only serve to disguise where it goes.
    const QUrl url = QUrl(link.trimmed()).adjusted(QUrl::RemoveUserInfo);
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
    const ArticleFilter filter = currentFilter();
    const qint64 upToId = m_viewLastId;
    writeOnWorker([filter, upToId](Store &store) { return store.markRead(filter, upToId); },
                  [this](const WriteResult &result) {
                      if (!result.error.isEmpty()) {
                          setStatus(tr("Cannot mark the view read: %1").arg(result.error), true);
                          return;
                      }
                      setStatus(tr("Marked %n articles read", nullptr, result.changed));
                      reloadCounts();
                      loadArticles();
                  });
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
    // Asked twice before the worker got to it: once is enough.
    if (m_removing.contains(feedId))
        return true;
    const std::optional<Feed> feed = m_store.feed(feedId);
    if (!feed) {
        setStatus(tr("Cannot unsubscribe: %1").arg(m_store.lastError()), true);
        return false;
    }
    // The feed leaves the window at once and comes back if the delete fails.
    m_removing.insert(feedId);
    m_unsubscribed.insert(feedId);
    reloadLibrary();
    writeOnWorker([feedId](Store &store) { return store.removeFeed(feedId) ? 1 : -1; },
                  [this, feedId, title = feed->title](const WriteResult &result) {
                      m_removing.remove(feedId);
                      if (!result.error.isEmpty()) {
                          // Still subscribed, so its refresh errors count again.
                          m_unsubscribed.remove(feedId);
                          setStatus(tr("Cannot unsubscribe: %1").arg(result.error), true);
                      } else {
                          setStatus(tr("Unsubscribed from %1").arg(title));
                      }
                      reloadLibrary();
                  });
    return true;
}

void Backend::refreshAll() {
    if (!m_storeOpen || m_refreshing)
        return;
    const QList<Feed> feeds = m_store.feeds();
    if (!m_store.lastError().isEmpty()) {
        setStatus(tr("Cannot read the library: %1").arg(m_store.lastError()), true);
        // Tried again later, like a refresh that failed.
        scheduleAutoRefresh();
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
    emit refreshChanged();

    QList<FetchRequest> requests;
    for (const Feed &feed : targets)
        requests << FetchRequest{QUrl(feed.url), feed.etag, feed.lastModified};
    // Each response goes to the worker as it arrives and keeps its fetch
    // slot until the worker has saved it, so at most maxConcurrent bodies
    // are held at once, in flight or waiting there, however many feeds.
    m_fetcher->fetchEachHeld(
        requests, [this, targets](qsizetype index, const FetchResult &fetched, const Fetcher::Release &release) {
            if (m_closing)
                return;
            const Feed feed = targets.at(index);
            onWorker<Library::Refresh>(
                [feed, fetched](Store &store) { return Library::apply(store, feed, fetched); },
                [this, feed, release](const Library::Refresh &outcome) {
                    release();
                    refreshApplied(feed, outcome);
                });
        });
}

void Backend::refreshApplied(const Feed &feed, const Library::Refresh &outcome) {
    ++m_refreshDone;
    // A feed unsubscribed while its fetch was out fails to save, which is
    // no news to anyone.
    if (!outcome.error.isEmpty() && !m_unsubscribed.contains(feed.id)) {
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

QString Backend::localPath(const QUrl &url) {
    if (!url.isLocalFile())
        return {};
    // file://host/path names a file on another machine, which toLocalFile()
    // turns into the network path //host/path.
    if (url.host().isEmpty())
        return url.toLocalFile();
    const bool here = url.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0;
    return here ? url.path(QUrl::FullyDecoded) : QString();
}

void Backend::importFrom(const QUrl &url) {
    const QString path = localPath(url);
    if (path.isEmpty()) {
        setStatus(tr("Only local files can be imported"), true);
        return;
    }
    if (!m_storeOpen)
        return;
    setStatus(tr("Importing %1…").arg(QFileInfo(path).fileName()));
    onWorker<Library::Import>([path](Store &store) { return Library::importFile(store, path); },
                              [this](const Library::Import &result) {
                                  // The import is one transaction, so a failed
                                  // one added nothing to reload or fetch.
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
    const QString path = localPath(url);
    if (path.isEmpty()) {
        setStatus(tr("Only local files can be written"), true);
        return;
    }
    if (!m_storeOpen)
        return;
    QString error;
    const QByteArray opml = Library::exportOpml(m_store, &error);
    if (!error.isEmpty() || !Library::writePrivately(path, opml, &error)) {
        setStatus(tr("Cannot export: %1").arg(error), true);
        return;
    }
    setStatus(tr("Exported %n feeds to %1", nullptr, m_feedCount).arg(path));
}
