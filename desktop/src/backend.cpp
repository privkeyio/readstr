#include "backend.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>

#include "filepicker.h"
#include "markdown.h"
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
    if (article.content.isEmpty())
        return Sanitizer::plainToRichText(article.summary);
    // Nostr long-form is Markdown.
    if (!article.address.isEmpty())
        return Markdown::toRichText(article.content);
    return Sanitizer::toRichText(article.content, QUrl(article.url));
}
}

Backend::Backend(FilePicker *picker, QObject *parent)
    : QObject(parent), m_fetcher(std::make_unique<Fetcher>()), m_relays(std::make_unique<RelayPool>()),
      m_syncRelays(std::make_unique<RelayPool>()), m_thumbnails(std::make_unique<Thumbnails>()), m_picker(picker) {
    m_openUrl = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
    m_writeClipboard = [](const QString &text) { QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard); };
    m_launch = &Backend::launchDetached;

    m_worker = new LibraryWorker;
    m_worker->moveToThread(&m_thread);
    // Deleted on its own thread as that thread ends, so its connection to
    // the library closes where it was opened.
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.setObjectName(QStringLiteral("readstr-library"));
    m_thread.start();

    m_syncRelays->setLimits(Sync::limits());
    m_autoRefresh.setSingleShot(true);
    connect(&m_autoRefresh, &QTimer::timeout, this, &Backend::refreshAll);

    if (m_picker) {
        connect(m_picker, &FilePicker::openSelected, this, &Backend::importFrom);
        connect(m_picker, &FilePicker::saveSelected, this, &Backend::exportTo);
        connect(m_picker, &FilePicker::failed, this, [this](const QString &message) { setStatus(message, true); });
    }

    // The reader's TextEdit puts a mouse selection in the primary selection
    // itself as the button goes up, zero-width spaces and all; they come out
    // once it has. Queued, so this never runs inside the clipboard's own call.
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard->supportsSelection())
        connect(
            clipboard, &QClipboard::selectionChanged, this,
            [clipboard] {
                if (!clipboard->ownsSelection())
                    return;
                const QString text = clipboard->text(QClipboard::Selection);
                if (text.contains(QChar(0x200B)))
                    clipboard->setText(copyableText(text), QClipboard::Selection);
            },
            Qt::QueuedConnection);

    m_storeOpen = m_store.open();
    if (!m_storeOpen)
        setStatus(tr("Cannot open the library: %1").arg(m_store.lastError()), true);
    reloadLibrary();
    reloadSyncAccount();
}

Backend::~Backend() {
    // Replies still in flight are dropped with the fetcher; their callbacks
    // see m_closing and do nothing. A job already running on the worker
    // finishes, and the ones queued behind it never start.
    m_closing = true;
    m_autoRefresh.stop();
    m_fetcher.reset();
    m_relays.reset();
    m_syncRelays.reset();
    m_thumbnails.reset();
    m_thread.quit();
    m_thread.wait();
    // The user's own changes still queued behind a refresh are not lost
    // with it; nothing else writes now, so this store does not wait. Those
    // the worker saved are not done twice, and as the window can no longer
    // say that one failed, stderr does.
    for (const std::shared_ptr<UserWrite> &write : std::as_const(m_userWrites)) {
        QString error;
        if (write->state == UserWrite::Failed)
            error = write->error;
        else if (write->state == UserWrite::Queued && write->work(m_store) < 0)
            error = m_store.lastError();
        if (!error.isEmpty())
            qWarning("readstr: %s", qPrintable(write->failure.arg(error)));
    }
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

void Backend::writeOnWorker(std::function<int(Store &)> work, const QString &failure,
                            std::function<void(const WriteResult &)> done) {
    auto write = std::make_shared<UserWrite>();
    write->work = std::move(work);
    write->failure = failure;
    m_userWrites << write;
    onWorker<WriteResult>(
        [write](Store &store) {
            WriteResult result;
            result.changed = write->work(store);
            if (result.changed < 0) {
                result.error = store.lastError();
                write->error = result.error;
            }
            write->state = result.changed < 0 ? UserWrite::Failed : UserWrite::Saved;
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
    if (row == m_sourceIndex) {
        // Renamed in place, by another process or a refresh: the title
        // shown for it follows.
        if (m_sources.row(row).title != selected.title)
            emit sourceChanged();
        return;
    }
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
    updateThumbnail();
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
    updateThumbnail();
}

void Backend::updateThumbnail() {
    const QString source = articleIsVideo() ? m_article.thumbnailUrl : QString();
    if (source == m_thumbnailSource)
        return;
    m_thumbnailSource = source;
    const quint64 request = ++m_thumbnailRequest;
    const QUrl url(source);
    const QUrl hit = source.isEmpty() ? QUrl() : Thumbnails::cached(url);
    setThumbnail(hit);
    // Only the open video's thumbnail is fetched: leaving it, or opening one
    // whose thumbnail is cached, drops the one on its way, and so does the
    // fetch below.
    if (source.isEmpty() || hit.isValid()) {
        m_thumbnails->cancel();
        return;
    }
    // A failed one shows no thumbnail, and nothing else: the video still plays.
    m_thumbnails->fetch(url, [this, request](const QUrl &file) {
        if (m_closing || request != m_thumbnailRequest)
            return;
        // Forgotten when it failed, so choosing the video again tries again
        // rather than finding its URL already handled.
        if (!file.isValid())
            m_thumbnailSource.clear();
        setThumbnail(file);
    });
}

void Backend::setThumbnail(const QUrl &file) {
    if (file == m_thumbnail)
        return;
    m_thumbnail = file;
    emit thumbnailChanged();
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
        failure,
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
            // where a write failed. An article that is gone (its feed
            // unsubscribed meanwhile) leaves the list and the reader instead.
            if (const auto saved = m_store.article(id)) {
                showFlags(id, saved->read, saved->starred);
            } else {
                reloadCounts();
                loadArticles();
                return;
            }
            reloadCounts();
        });
}

void Backend::toggleRead() { setFlags(!m_article.read, m_article.starred); }
void Backend::toggleStar() { setFlags(m_article.read, !m_article.starred); }

void Backend::openOriginal() {
    if (m_article.id && !m_article.url.isEmpty())
        openLink(m_article.url);
}

void Backend::playVideo() {
    const QUrl url(m_article.mediaUrl);
    if (!m_article.id || !Library::isWebUrl(url)) {
        setStatus(tr("This article has no video to play"));
        return;
    }
    // Someone else's event chose the URL, so it must not have the player
    // reach into this machine or the local network. NostrSource drops such
    // URLs already; this covers a library saved before it did.
    if (Fetcher::isLocalHost(url.host())) {
        setStatus(tr("Cannot play the video: it is on this machine or the local network"), true);
        return;
    }
    // "--" ends mpv's options, so no URL can pass for one. mpv still follows
    // what the URL leads to, redirects and the entries of a playlist or an
    // HLS stream, to any http(s) host (it refuses local files a network
    // playlist names). --access-references=no would stop playlists but stops
    // HLS too, which mpv opens the same way, so checking those hops waits for
    // a proxy that can tell them apart.
    const QStringList arguments = {QStringLiteral("--"), url.toString(QUrl::FullyEncoded)};
    switch (m_launch(QStringLiteral("mpv"), arguments)) {
    case Launch::Started:
        setStatus(tr("Playing %1 in mpv").arg(m_article.title));
        break;
    case Launch::Missing:
        setStatus(tr("Cannot play the video: mpv is not installed"), true);
        break;
    case Launch::Failed:
        setStatus(tr("Cannot play the video: mpv did not start"), true);
        break;
    }
}

Backend::Launch Backend::launchDetached(const QString &program, const QStringList &arguments) {
    // QProcess reports a missing program as any other failure to start,
    // and a missing player deserves its own message.
    const QString path = QStandardPaths::findExecutable(program);
    if (path.isEmpty())
        return Launch::Missing;
    return QProcess::startDetached(path, arguments) ? Launch::Started : Launch::Failed;
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
    const QString failure = tr("Cannot mark the view read: %1");
    writeOnWorker([filter, upToId](Store &store) { return store.markRead(filter, upToId); }, failure,
                  [this, failure](const WriteResult &result) {
                      if (!result.error.isEmpty()) {
                          setStatus(failure.arg(result.error), true);
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
    const auto author = Library::nostrAuthor(text);
    // A bare host is what people paste most; the web means https now.
    if (!author && !text.isEmpty() && !text.contains(QLatin1String("://")))
        text.prepend(QLatin1String("https://"));
    const QUrl url(text, QUrl::StrictMode);
    if (!author && !Library::isWebUrl(url)) {
        m_addError = tr("Enter a web address, like https://example.com/feed.xml, or an npub");
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
    if (!author) {
        searchRound(std::make_shared<Discovery::Search>(url));
        return;
    }
    const Nip19::Profile profile = *author;
    // The first of the two results to come gives its slot back at once and
    // waits beside the other, which keeps its own until the feed is saved.
    auto pairs = std::make_shared<Library::NostrPairs>();
    m_relays->queryEachHeld(
        Library::nostrQueries(profile),
        [this, profile, pairs](qsizetype index, const NostrResult &found, const RelayPool::Release &release) {
            if (m_closing)
                return;
            const auto both = pairs->take(index, found);
            if (!both) {
                release();
                return;
            }
            onWorker<Library::Subscription>(
                [profile, articles = both->first, videos = both->second](Store &store) {
                    return Library::subscribeNostr(store, profile, articles, videos);
                },
                [this, release](const Library::Subscription &subscription) {
                    release();
                    subscribeFinished(subscription);
                });
        });
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
    const Feed feed = m_store.feed(subscription.id).value_or(Feed{});
    const QString title = Nip19::shortNpub(feed.title, feed.url);
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
    const QString failure = tr("Cannot unsubscribe: %1");
    writeOnWorker([feedId](Store &store) { return store.removeFeed(feedId) ? 1 : -1; }, failure,
                  [this, feedId, title = Nip19::shortNpub(feed->title, feed->url), failure](const WriteResult &result) {
                      m_removing.remove(feedId);
                      if (!result.error.isEmpty()) {
                          // Still subscribed, so its refresh errors count again.
                          m_unsubscribed.remove(feedId);
                          setStatus(failure.arg(result.error), true);
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
        // A new library may get its feeds from the sync.
        sync();
        return;
    }
    m_syncAfterRefresh = true;
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

    QList<Feed> web, nostr;
    QList<FetchRequest> requests;
    QList<NostrQuery> queries;
    for (const Feed &feed : targets) {
        if (Library::isNostrFeed(feed)) {
            nostr << feed;
            queries << Library::nostrQueries(feed);
        } else {
            web << feed;
            requests << FetchRequest{QUrl(feed.url), feed.etag, feed.lastModified};
        }
    }
    // Each response goes to the worker as it arrives and keeps its fetch
    // slot until the worker has saved it, so at most maxConcurrent bodies
    // are held at once, in flight or waiting there, however many feeds. The
    // relays' answers are held the same way, a feed's two at once: the first
    // to come gives its slot back and waits for the other, which the pool
    // opens next, since it opens queries in order.
    m_fetcher->fetchEachHeld(
        requests, [this, web](qsizetype index, const FetchResult &fetched, const Fetcher::Release &release) {
            if (m_closing)
                return;
            const Feed feed = web.at(index);
            onWorker<Library::Refresh>(
                [feed, fetched](Store &store) { return Library::apply(store, feed, fetched); },
                [this, feed, release](const Library::Refresh &outcome) {
                    release();
                    refreshApplied(feed, outcome);
                });
        });
    auto pairs = std::make_shared<Library::NostrPairs>();
    m_relays->queryEachHeld(
        queries, [this, nostr, pairs](qsizetype index, const NostrResult &found, const RelayPool::Release &release) {
            if (m_closing)
                return;
            const auto both = pairs->take(index, found);
            if (!both) {
                release();
                return;
            }
            const Feed feed = nostr.at(index / 2);
            onWorker<Library::Refresh>(
                [feed, articles = both->first, videos = both->second](Store &store) {
                    return Library::applyNostr(store, feed, articles, videos);
                },
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
            m_firstFailure = QStringLiteral("%1: %2").arg(
                Nip19::shortNpub(outcome.title.isEmpty() ? feed.url : outcome.title, feed.url), outcome.error);
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
    if (std::exchange(m_fetchNewFeeds, false) && refreshUnfetched())
        return;
    if (std::exchange(m_syncAfterRefresh, false)) {
        m_refreshStatus = {m_status, m_statusError};
        sync();
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

QVariant Backend::syncedAt() const {
    return m_syncAccount.syncedAt.isValid() ? QVariant(m_syncAccount.syncedAt.toLocalTime()) : QVariant();
}

void Backend::reloadSyncAccount() {
    if (!m_storeOpen)
        return;
    const SyncAccount account = m_store.syncAccount();
    if (!m_store.lastError().isEmpty()) {
        setStatus(tr("Cannot read the library: %1").arg(m_store.lastError()), true);
        return;
    }
    m_syncAccount = account;
    emit syncChanged();
}

bool Backend::setSyncNpub(const QString &text) {
    const auto account = Sync::account(text);
    if (!account || !m_storeOpen)
        return false;
    writeOnWorker(
        [account = *account](Store &store) {
            return store.setSyncAccount(account.npub, account.pubkey, account.relays) ? 1 : -1;
        },
        tr("Cannot sync with %1: %2").arg(Nip19::shortNpub(account->npub, account->npub)),
        [this, npub = account->npub](const WriteResult &result) {
            if (!result.error.isEmpty()) {
                setStatus(tr("Cannot sync with %1: %2").arg(Nip19::shortNpub(npub, npub), result.error), true);
                return;
            }
            reloadSyncAccount();
            sync();
        });
    return true;
}

void Backend::stopSync() {
    if (!m_storeOpen || m_syncAccount.npub.isEmpty())
        return;
    const QString npub = m_syncAccount.npub;
    writeOnWorker([](Store &store) { return store.clearSyncAccount() ? 1 : -1; },
                  tr("Cannot stop syncing: %1"), [this, npub](const WriteResult &result) {
                      if (!result.error.isEmpty()) {
                          setStatus(tr("Cannot stop syncing: %1").arg(result.error), true);
                          return;
                      }
                      reloadSyncAccount();
                      setStatus(tr("Stopped syncing with %1").arg(Nip19::shortNpub(npub, npub)));
                  });
}

void Backend::sync() {
    if (m_syncing) {
        m_syncAgain = true;
        return;
    }
    if (!m_storeOpen || m_syncAccount.pubkey.isEmpty()) {
        m_refreshStatus.reset();
        return;
    }
    m_syncing = true;
    emit syncChanged();
    setStatus(tr("Syncing with Nostr…"));
    const SyncAccount account = m_syncAccount;
    m_syncRelays->queryEachHeld({Sync::query(account)}, [this, account](qsizetype, const NostrResult &found,
                                                                       const RelayPool::Release &release) {
        if (m_closing)
            return;
        onWorker<Sync::Outcome>([account, found](Store &store) { return Sync::apply(store, account, found); },
                                [this, release, pubkey = account.pubkey](const Sync::Outcome &outcome) {
                                    release();
                                    syncApplied(outcome, pubkey);
                                });
    });
}

void Backend::syncApplied(const Sync::Outcome &outcome, const QString &pubkey) {
    m_syncing = false;
    emit syncChanged();
    reloadSyncAccount();
    const bool again = std::exchange(m_syncAgain, false);
    // An npub set while the pull was out is pulled for now.
    if (!m_syncAccount.pubkey.isEmpty() && m_syncAccount.pubkey != pubkey) {
        m_syncAddedBefore = m_syncMarkedBefore = 0;
        sync();
        return;
    }
    reloadLibrary();
    QStringList errors = outcome.problems;
    if (!outcome.error.isEmpty())
        errors.prepend(outcome.error);
    const int added = std::exchange(m_syncAddedBefore, 0) + outcome.added;
    const int markedRead = std::exchange(m_syncMarkedBefore, 0) + outcome.markedRead;
    if (again && !m_syncAccount.pubkey.isEmpty()) {
        // The follow-up says what both changed, and keeps the status of a
        // refresh that finished while this one was out, which it was asked
        // for. Its own problems are said there, if they persist.
        for (const QString &error : std::as_const(errors))
            qWarning("readstr: sync: %s", qPrintable(error));
        m_syncAddedBefore = added;
        m_syncMarkedBefore = markedRead;
        sync();
        return;
    }
    const std::optional<std::pair<QString, bool>> refreshStatus = std::exchange(m_refreshStatus, std::nullopt);
    // Anything said while the relays were asked is newer than the refresh's
    // message, so that comes back only over the syncing one.
    const bool stillSyncing = m_status == tr("Syncing with Nostr…");
    if (m_syncAccount.pubkey.isEmpty()) {
        // Syncing was stopped while the pull was out, which is no failure.
        // The window's own stop says so; a stop from elsewhere (`readstr
        // sync logout`) must not leave "Syncing" up.
        if (stillSyncing)
            setStatus(refreshStatus ? refreshStatus->first : QString(), refreshStatus && refreshStatus->second);
    } else if (!errors.isEmpty()) {
        // Said on stderr too, since the status bar keeps only the latest.
        for (const QString &error : std::as_const(errors))
            qWarning("readstr: sync: %s", qPrintable(error));
        setStatus(tr("Cannot sync with Nostr: %1").arg(errors.join("; ")), true);
    } else if (refreshStatus && stillSyncing && added == 0 && markedRead == 0) {
        // A sync that changed nothing would only hide what the refresh
        // before it said, such as a feed that cannot be refreshed.
        setStatus(refreshStatus->first, refreshStatus->second);
    } else {
        setStatus(tr("Synced with Nostr: %n feeds added, %1 articles marked read", nullptr, added).arg(markedRead));
    }
    emit syncFinished();
    if (added == 0)
        return;
    // The feeds the sync added have never been fetched.
    if (m_refreshing)
        m_fetchNewFeeds = true;
    else
        refreshUnfetched();
}

bool Backend::refreshUnfetched() {
    QList<Feed> fresh;
    for (const Feed &candidate : m_store.feeds())
        if (!candidate.lastFetchedAt.isValid())
            fresh << candidate;
    if (fresh.isEmpty())
        return false;
    refresh(fresh);
    return true;
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

QString Backend::copyableText(const QString &text) {
    QString plain;
    plain.reserve(text.size());
    for (const QChar character : text) {
        const char16_t code = character.unicode();
        if (code == 0x200B)
            continue;
        // Paragraph and line separators, and where a table or its cells
        // begin and end.
        plain += code == 0x2029 || code == 0x2028 || code == 0xFDD0 || code == 0xFDD1 ? QChar('\n') : character;
    }
    return plain;
}

void Backend::copyText(const QString &text) {
    const QString plain = copyableText(text);
    if (plain.isEmpty())
        return;
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(plain, QClipboard::Clipboard);
    if (clipboard->supportsSelection())
        clipboard->setText(plain, QClipboard::Selection);
}

QString Backend::npubOf(qint64 feedId) const {
    const SourceModel::Row row = m_sources.row(m_sources.rowOf(SourceModel::Kind::Feed, feedId));
    if (!row.nostr)
        return {};
    // Encoded again from the key, so only a well-formed npub goes out.
    const auto author = Nip19::decodeProfile(row.url);
    return author ? Nip19::npub(author->pubkey) : QString();
}

void Backend::openProfile(qint64 feedId) {
    const QString npub = npubOf(feedId);
    if (!npub.isEmpty())
        openLink(QStringLiteral("https://njump.me/") + npub);
}

void Backend::copyNpub(qint64 feedId) {
    const QString npub = npubOf(feedId);
    if (npub.isEmpty())
        return;
    m_writeClipboard(npub);
    setStatus(tr("Copied %1 to the clipboard").arg(Nip19::shortNpub(npub, npub)));
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
