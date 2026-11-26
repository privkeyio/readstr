#include "backend.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QStandardPaths>
#include <algorithm>

#include "filepicker.h"
#include "markdown.h"
#include "qrcode.h"
#include "sanitizer.h"
#include "savedviews.h"
#include "wipe.h"

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

QString richText(const Article &article, const Sanitizer::Images &images) {
    if (article.content.isEmpty())
        return Sanitizer::plainToRichText(article.summary);
    // Nostr long-form is Markdown.
    if (!article.address.isEmpty())
        return Markdown::toRichText(article.content, images);
    Sanitizer::Options options;
    options.images = images;
    return Sanitizer::toRichText(article.content, QUrl(article.url), options);
}

// What a keyring call would have answered had there been a keyring to ask.
KeyStore::Result keyStoreUnavailable() {
    KeyStore::Result result;
    result.error = KeyStore::Error::Unavailable;
    return result;
}

QString noKeyStored(const QString &npub) {
    return Backend::tr("No signing key is stored for %1").arg(Nip19::shortNpub(npub, npub));
}
}

Backend::Backend(FilePicker *picker, QObject *parent)
    : QObject(parent), m_fetcher(std::make_unique<Fetcher>()), m_relays(std::make_unique<RelayPool>()),
      m_syncRelays(std::make_unique<RelayPool>()), m_thumbnails(std::make_unique<Thumbnails>()),
      m_images(std::make_unique<Thumbnails>(QStringLiteral("images"), imageLimits())), m_picker(picker) {
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
    m_publishTimer.setSingleShot(true);
    connect(&m_publishTimer, &QTimer::timeout, this, [this] { startPublish(); });

    // A new article is new text too.
    connect(this, &Backend::articleChanged, this, &Backend::articleHtmlChanged);
    // Images coming in together are shown together, rather than the text
    // being laid out again for each.
    m_imagesArrived.setSingleShot(true);
    m_imagesArrived.setInterval(100);
    connect(&m_imagesArrived, &QTimer::timeout, this, [this] {
        renderArticle();
        emit articleHtmlChanged();
    });
    connect(&m_settings, &Settings::loadRemoteImagesChanged, this, [this] {
        // articleImagesShown changed even when the text did not.
        if (!updateImages())
            emit articleHtmlChanged();
    });
    if (!m_settings.error().isEmpty())
        setStatus(tr("Cannot read the settings, so the defaults apply: %1").arg(m_settings.error()), true);
    // The file is read again whenever it changes, so it may turn out
    // unreadable later as well.
    connect(&m_settings, &Settings::errorChanged, this, [this] {
        if (m_settings.error().isEmpty())
            return;
        if (m_settings.unreadable())
            setStatus(tr("Cannot read the settings, so the defaults apply: %1").arg(m_settings.error()), true);
        else
            setStatus(tr("Cannot save the settings: %1").arg(m_settings.error()), true);
    });

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
    loadFilterRules();
    reloadLibrary();
    reloadSyncAccount();
    // Finishes a pass over the library that the last session cut short, so
    // the lists show what the rules make of every article. Not one of the
    // user's writes: one left at closing is finished by the next session.
    if (m_storeOpen)
        onWorker<WriteResult>(
            [quitting = m_quitting](Store &store) {
                WriteResult result;
                bool ran = false;
                if (!store.applyFilterRules([quitting] { return quitting->load(); }, &ran))
                    result.error = store.lastError();
                result.changed = ran;
                return result;
            },
            [this](const WriteResult &result) {
                // Batches committed before an error still changed flags.
                if (result.changed > 0)
                    reloadLibrary();
                if (!result.error.isEmpty())
                    setStatus(tr("Cannot apply the filter rules: %1").arg(result.error), true);
            });
}

Backend::~Backend() {
    // First, so a filter pass under way, or one still queued, stops before
    // the wait for the worker below.
    m_quitting->store(true);
    // Replies still in flight are dropped with the fetcher; their callbacks
    // see m_closing and do nothing. A job already running on the worker
    // finishes, and the ones queued behind it never start.
    m_closing = true;
    m_autoRefresh.stop();
    m_publishTimer.stop();
    m_fetcher.reset();
    m_relays.reset();
    m_syncRelays.reset();
    m_thumbnails.reset();
    m_images.reset();
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
        else if (write->state == UserWrite::Queued)
            error = runWrite(*write, m_store).error;
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
                            std::function<void(const WriteResult &)> done, const std::shared_ptr<UndoState> &undo) {
    auto write = std::make_shared<UserWrite>();
    write->work = std::move(work);
    write->failure = failure;
    write->undo = undo;
    m_userWrites << write;
    onWorker<WriteResult>(
        [write](Store &store) { return runWrite(*write, store); },
        [this, write, done = std::move(done)](const WriteResult &result) {
            m_userWrites.removeOne(write);
            done(result);
        });
}

Backend::WriteResult Backend::runWrite(UserWrite &write, Store &store) {
    WriteResult result;
    // An undo or redo queued before the write it reverses failed has
    // nothing to reverse.
    if (write.undo && write.undo->failed) {
        result.skipped = true;
        write.state = UserWrite::Saved;
        return result;
    }
    result.changed = write.work(store);
    if (result.changed < 0) {
        result.error = store.lastError();
        write.error = result.error;
        if (write.undo)
            write.undo->failed = true;
    }
    write.state = result.changed < 0 ? UserWrite::Failed : UserWrite::Saved;
    return result;
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

QString Backend::selectedViewId() const {
    const SourceModel::Row row = m_sources.row(m_sourceIndex);
    return row.kind == SourceModel::Kind::View ? row.viewId : QString();
}

const SavedView *Backend::savedView(const QString &id) const {
    for (const SavedView &view : m_views)
        if (view.id == id)
            return &view;
    return nullptr;
}

bool Backend::loadViews() {
    if (!m_storeOpen)
        return false;
    const QList<SavedView> views = m_store.savedViews();
    if (!m_store.lastError().isEmpty()) {
        setStatus(tr("Cannot read the saved views: %1").arg(m_store.lastError()), true);
        return false;
    }
    const bool same = std::equal(views.begin(), views.end(), m_views.begin(), m_views.end(),
                                 [](const SavedView &a, const SavedView &b) {
                                     return a.id == b.id && a.view == b.view && a.exclude == b.exclude;
                                 });
    if (same)
        return false;
    m_views = views;
    return true;
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
    else if (row.kind == SourceModel::Kind::Tag) {
        filter.tags = row.tags;
        filter.anyTag = true;
    } else if (row.kind == SourceModel::Kind::View)
        if (const SavedView *view = savedView(row.viewId)) {
            const ArticleFilter rules = SavedViews::evaluate(*view).filter;
            filter.tags = rules.tags;
            filter.unreadOnly = rules.unreadOnly;
            filter.starredOnly = rules.starredOnly;
            filter.readOnly = rules.readOnly;
            filter.oldestFirst = rules.oldestFirst;
            filter.includeWords = rules.includeWords;
            filter.excludeWords = rules.excludeWords;
            filter.maxScanned = m_maxViewScanned;
        }
    filter.query = m_searchQuery;
    return filter;
}

void Backend::setSearchQuery(const QString &query) {
    if (query == m_searchQuery)
        return;
    const QString before = Store::matchExpression(m_searchQuery);
    m_searchQuery = query;
    emit searchChanged();
    // A space typed after a word, or a stray quote, searches for the same.
    if (Store::matchExpression(query) != before)
        loadArticles();
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
        loadViews();
    }
    m_sources.setLibrary(feeds, counts, m_views);
    if (m_feedCount != feeds.size()) {
        m_feedCount = int(feeds.size());
        emit libraryChanged();
    }
    // A view or feed that is gone leaves All articles selected; one that
    // changed lists what it holds now, as the articles are read again.
    const int row = m_sources.rowOf(selected);
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
    // Another process can add, rename, or remove feeds, or pull saved
    // views, which moves rows; the selection follows its source rather
    // than its row.
    const SourceModel::Row selected = m_sources.row(m_sourceIndex);
    std::optional<SavedView> viewBefore;
    if (const SavedView *view = savedView(selected.viewId); view && selected.kind == SourceModel::Kind::View)
        viewBefore = *view;
    bool rulesChanged = false;
    if (loadViews() && viewBefore) {
        const SavedView *view = savedView(selected.viewId);
        // A view renamed, given another icon, or moved lists the same, and
        // reading it again would scan for its words for nothing.
        rulesChanged = view && !SavedViews::listsSame(*view, *viewBefore);
    }
    m_sources.setLibrary(feeds, *counts, m_views);
    const int row = m_sources.rowOf(selected);
    if (m_feedCount != feeds.size()) {
        m_feedCount = int(feeds.size());
        emit libraryChanged();
    }
    if (row == m_sourceIndex) {
        // Renamed in place, by another process or a refresh: the title
        // shown for it follows.
        if (m_sources.row(row).title != selected.title)
            emit sourceChanged();
        // A view whose rules changed lists what they hold now.
        if (rulesChanged)
            loadArticles();
        return;
    }
    // A feed or view that is gone leaves All articles selected, as after a
    // reload, and the list it showed goes with it.
    m_sourceIndex = row < 0 ? 0 : row;
    emit sourceChanged();
    if (row < 0 || rulesChanged)
        loadArticles();
}

void Backend::loadArticles() {
    QList<Article> list;
    bool capped = false;
    if (m_storeOpen) {
        // Taken before the list, so marking the view read never reaches an
        // article that arrived after it was shown.
        m_viewLastId = m_store.counts().value_or(LibraryCounts{}).lastId;
        list = m_store.articles(currentFilter(), &capped, &m_viewScanned);
        if (!m_store.lastError().isEmpty())
            setStatus(tr("Cannot read the library: %1").arg(m_store.lastError()), true);
    }
    if (capped != m_viewScanCapped) {
        m_viewScanCapped = capped;
        emit viewScanCappedChanged();
    }
    if (!m_pendingFlags.isEmpty())
        for (Article &article : list)
            applyPending(article);
    // A feed being unsubscribed takes its articles with it right away.
    if (!m_removing.isEmpty()) {
        list.removeIf([this](const Article &article) { return m_removing.contains(article.feedId); });
        if (m_removing.contains(m_article.feedId)) {
            m_article = Article{};
            renderArticle();
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
                renderArticle();
        } else {
            m_article = Article{};
            renderArticle();
        }
    }
    m_articleIndex = m_article.id ? m_articles.rowOf(m_article.id) : -1;
    // The cached images go into the text before it is announced, so the
    // reader never shows their links first.
    updateImages(false);
    emit articleChanged();
    updateThumbnail();
}

void Backend::selectSource(int row) {
    if (!m_sources.isSelectable(row))
        return;
    m_sourceIndex = row;
    m_article = Article{};
    renderArticle();
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
    renderArticle();
    // As in loadArticles(): cached images are in the first text shown.
    updateImages(false);
    emit articleChanged();
    updateThumbnail();
}

Thumbnails::Limits Backend::imageLimits() {
    Thumbnails::Limits limits;
    limits.maxBytes = 4 * 1024 * 1024;
    limits.maxWidth = Sanitizer::imageWidth;
    // 1600 by 2500 pixels, 16 MiB once decoded: a tall infographic stays
    // legible, at less than its full width, while a strip of 1600 by 8192
    // pixels is no longer 52 MiB in the window for each copy of it.
    limits.maxStoredPixels = 4 * 1000 * 1000;
    // Well past a photograph of that size at quality 85, so only noise,
    // which JPEG cannot compress, is refused; 30 of them still fit in the
    // cache together.
    limits.maxStoredBytes = 2 * 1024 * 1024;
    limits.maxConcurrent = 4;
    limits.maxImages = 30;
    limits.maxTotalBytes = 24 * 1024 * 1024;
    limits.cacheMaxBytes = 128 * 1024 * 1024;
    return limits;
}

bool Backend::articleImagesShown() const {
    return hasArticle() && (m_settings.loadRemoteImages() || m_imagesFor == m_article.id);
}

void Backend::clearLocalImages() {
    m_localImages.clear();
    m_imagePixels = 0;
}

void Backend::renderArticle() {
    // The images fetched for another article are not this one's.
    if (m_imagesAskedFor != m_article.id)
        clearLocalImages();
    QStringList found;
    m_articleHtml = richText(m_article, {articleImagesShown() ? &m_localImages : nullptr, &found});
    m_articleImages = found;
}

void Backend::loadArticleImages() {
    if (!hasArticle() || articleImagesShown())
        return;
    m_imagesFor = m_article.id;
    // articleImagesShown changed even when the text did not.
    if (!updateImages())
        emit articleHtmlChanged();
}

bool Backend::updateImages(bool announce) {
    // Asking for one article's images is for that article alone.
    if (m_imagesFor != m_article.id)
        m_imagesFor = 0;
    if (!articleImagesShown() || m_articleImages.isEmpty()) {
        // Nothing is fetched for an article it is not shown for, and what
        // was is shown no longer.
        if (!m_imagesAsked.isEmpty()) {
            m_images->cancel();
            m_imagesAsked.clear();
            m_imagesAskedFor = 0;
            ++m_imagesRequest;
        }
        m_imagesArrived.stop();
        if (!m_localImages.isEmpty()) {
            clearLocalImages();
            renderArticle();
            if (announce)
                emit articleHtmlChanged();
            return true;
        }
        return false;
    }
    const int most = m_images->limits().maxImages;
    const QStringList sources = most > 0 ? m_articleImages.mid(0, most) : m_articleImages;
    if (m_imagesAskedFor == m_article.id && m_imagesAsked == sources)
        return false;
    m_imagesAsked = sources;
    m_imagesAskedFor = m_article.id;
    const quint64 request = ++m_imagesRequest;
    // A batch of another article's images, or of sources no longer asked
    // for, is not this request's to show.
    m_imagesArrived.stop();
    clearLocalImages();
    // Kept by the sanitizer's own string for it, the key it looks the local
    // copy up by. Past the article's budget of pixels the rest stay links,
    // since the window holds every image shown decoded.
    const auto keep = [this](const QString &source, const QUrl &file) {
        if (m_localImages.contains(source))
            return false;
        const QSize size = QImageReader(file.toLocalFile(), "jpeg").size();
        const qint64 pixels = size.isValid() ? qint64(size.width()) * size.height() : 0;
        if (m_imagePixels + pixels > maxArticleImagePixels)
            return false;
        m_imagePixels += pixels;
        m_localImages.insert(source, {file, size.width()});
        return true;
    };
    // Each source as the URL it is fetched and cached by, with every source
    // that reads as the same URL, so a cache hit and a fetched copy are kept
    // under the same key.
    QList<QUrl> urls;
    QHash<QUrl, QStringList> sourcesOf;
    for (const QString &source : sources) {
        const QUrl url(source, QUrl::StrictMode);
        if (!sourcesOf.contains(url))
            urls << url;
        sourcesOf[url] << source;
    }
    // The cached ones show at once, and only the rest are fetched.
    bool missing = false;
    for (const QUrl &url : std::as_const(urls)) {
        const QUrl hit = Thumbnails::cached(url, m_images->cacheName());
        if (!hit.isValid()) {
            missing = true;
            continue;
        }
        for (const QString &source : sourcesOf.value(url))
            keep(source, hit);
    }
    // The article was laid out with its links before this, and must be
    // again with the copies, even when nothing more arrives.
    const bool rendered = !m_localImages.isEmpty();
    if (rendered) {
        renderArticle();
        if (announce)
            emit articleHtmlChanged();
    }
    if (!missing) {
        m_images->cancel();
        return rendered;
    }
    // The cached ones are asked for too, though they come back at once and
    // are already kept, so the cache's prunes during this fetch spare them.
    // One that fails stays a link, and says nothing.
    m_images->fetchEach(urls, [this, request, keep, sourcesOf](const QUrl &url, const QUrl &file) {
        if (m_closing || request != m_imagesRequest || !file.isValid())
            return;
        bool kept = false;
        for (const QString &source : sourcesOf.value(url))
            kept = keep(source, file) || kept;
        if (kept && !m_imagesArrived.isActive())
            m_imagesArrived.start();
    });
    return rendered;
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

void Backend::saveFlags(const Article &before, bool read, bool starred, const QString &failure,
                        const std::function<bool(Store &)> &writeRead, const Finished &finished,
                        const std::shared_ptr<UndoState> &undo, const std::function<bool(Store &)> &writeStar) {
    const qint64 id = before.id;
    // An undo puts back the read time too, which a write nobody can undo
    // (opening the article) may have changed under the same flag.
    const bool readChanged = read != before.read || writeRead,
               starredChanged = starred != before.starred;
    if (!readChanged && !starredChanged) {
        if (finished)
            finished(true);
        return;
    }
    PendingFlags &pending = m_pendingFlags[id];
    pending.read = read;
    pending.starred = starred;
    ++pending.writes;
    showFlags(id, read, starred);
    // The worker takes writes in order, so each only needs what it changes.
    writeOnWorker(
        [id, read, starred, readChanged, starredChanged, writeRead, writeStar,
         readsPending = readChanged ? readsPublishedFor() : QString()](Store &store) {
            if ((readChanged && !(writeRead ? writeRead(store) : store.setRead(id, read)))
                || (starredChanged && !(writeStar ? writeStar(store) : store.setStarred(id, starred))))
                return -1;
            markPending(store, readsPending, Sync::readStatusKind);
            return 1;
        },
        failure,
        [this, id, failure, readChanged, finished](const WriteResult &result) {
            if (!result.error.isEmpty())
                setStatus(failure.arg(result.error), true);
            else if (readChanged && !result.skipped)
                schedulePublish(Sync::readStatusKind);
            if (finished)
                finished(result.error.isEmpty() && !result.skipped);
            const auto pending = m_pendingFlags.find(id);
            bool toggle = false;
            if (pending != m_pendingFlags.end()) {
                if (--pending->writes > 0)
                    return;
                toggle = pending->toggleRead;
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
            // M pressed while an undo's guess was shown toggles the flag the
            // library has.
            if (toggle && m_article.id == id)
                toggleRead();
        },
        undo);
}

void Backend::toggleRead() {
    if (!m_storeOpen || !m_article.id)
        return;
    const qint64 id = m_article.id;
    const auto pending = m_pendingFlags.find(id);
    if (pending != m_pendingFlags.end() && pending->readGuessed) {
        pending->toggleRead = !pending->toggleRead;
        return;
    }
    const bool read = !m_article.read;
    // The read times before and after, which only the worker knows.
    auto change = std::make_shared<ReadChange>(ReadChange{id, {!read, std::nullopt}, {read, std::nullopt}});
    // Set here once the worker has filled in `change`.
    auto captured = std::make_shared<bool>(false);
    auto command = std::make_shared<UndoCommand>();
    command->label = [read] { return read ? tr("marked an article read") : tr("marked an article unread"); };
    command->apply = [this, id, read, change, captured, state = command->state](bool undo,
                                                                               const Finished &finished) {
        restoreRead(id, undo ? !read : read, change, *captured, undo, state, finished);
    };
    pushUndo(command);
    saveFlags(
        m_article, read, m_article.starred, tr("Cannot change the article: %1"),
        [change](Store &store) { return store.setRead(change->id, change->after.read, change.get()); },
        [this, command, captured](bool ok) {
            if (ok)
                *captured = true;
            else
                dropUndo(command);
        },
        command->state);
}

void Backend::toggleStar() {
    if (!m_storeOpen || !m_article.id)
        return;
    const qint64 id = m_article.id;
    const bool starred = !m_article.starred;
    auto command = std::make_shared<UndoCommand>();
    command->label = [starred] { return starred ? tr("starred an article") : tr("unstarred an article"); };
    command->apply = [this, id, starred, state = command->state](bool undo, const Finished &finished) {
        restoreStar(id, undo ? !starred : starred, undo, state, finished);
    };
    pushUndo(command);
    // The flag as stored, which another window may have set already: then
    // this changed nothing, and an undo has nothing to take back.
    saveFlags(
        m_article, m_article.read, starred, tr("Cannot change the article: %1"), {},
        [this, command](bool ok) {
            if (!ok || command->state->failed)
                dropUndo(command);
        },
        command->state,
        [id, starred, state = command->state](Store &store) {
            bool was = !starred;
            if (!store.setStarred(id, starred, &was))
                return false;
            if (was == starred)
                state->failed = true;
            return true;
        });
}

Article Backend::flagsOf(qint64 id, bool read, bool starred) {
    std::optional<Article> article = m_storeOpen ? m_store.article(id) : std::nullopt;
    if (!article) {
        Article gone;
        gone.id = id;
        gone.read = !read;
        gone.starred = !starred;
        return gone;
    }
    applyPending(*article);
    return *article;
}

void Backend::restoreRead(qint64 id, bool read, const std::shared_ptr<const ReadChange> &change, bool captured,
                          bool undo, const std::shared_ptr<UndoState> &state, const Finished &finished) {
    // Another window may have changed the flag before the action read it.
    if (captured)
        read = (undo ? change->before : change->after).read;
    const Article before = flagsOf(id, read, false);
    saveFlags(
        before, read, before.starred, undoFailure(undo),
        [change, undo](Store &store) { return store.setReadState(change->id, undo ? change->before : change->after); },
        finished, state);
    const auto pending = m_pendingFlags.find(id);
    if (!captured && undo && pending != m_pendingFlags.end())
        pending->readGuessed = true;
}

void Backend::restoreStar(qint64 id, bool starred, bool undo, const std::shared_ptr<UndoState> &state,
                          const Finished &finished) {
    const Article before = flagsOf(id, false, starred);
    saveFlags(before, before.read, starred, undoFailure(undo), {}, finished, state);
}

void Backend::restoreReads(const std::shared_ptr<const QList<ReadChange>> &changes, bool undo,
                           const std::shared_ptr<UndoState> &state, const Finished &finished) {
    const QString failure = undoFailure(undo);
    writeOnWorker(
        [changes, undo, readsPending = readsPublishedFor()](Store &store) {
            const int restored = store.restoreReadStates(*changes, undo);
            if (restored > 0)
                markPending(store, readsPending, Sync::readStatusKind);
            return restored;
        },
        failure,
        [this, failure, finished, undo](const WriteResult &result) {
            if (!result.error.isEmpty())
                setStatus(failure.arg(result.error), true);
            else if (result.changed > 0)
                schedulePublish(Sync::readStatusKind);
            else if (!result.skipped)
                // Every article it marked is gone, so it is forgotten.
                setStatus(undo ? tr("Nothing left to undo: those articles are gone")
                               : tr("Nothing left to redo: those articles are gone"));
            finished(result.error.isEmpty() && !result.skipped && result.changed > 0);
            reloadCounts();
            loadArticles();
        },
        state);
}

QString Backend::undoFailure(bool undo) { return undo ? tr("Cannot undo: %1") : tr("Cannot redo: %1"); }

void Backend::pushUndo(const std::shared_ptr<UndoCommand> &command) {
    ++m_pushes;
    m_undo << command;
    if (m_undo.size() > maxUndo)
        m_undo.removeFirst();
    m_redo.clear();
    emit undoChanged();
}

void Backend::dropUndo(const std::shared_ptr<UndoCommand> &command) {
    // Its writes still queued skip, and say nothing.
    command->state->failed = true;
    if (m_undo.removeOne(command) || m_redo.removeOne(command))
        emit undoChanged();
}

Backend::Finished Backend::dropUndoOnFailure(const std::shared_ptr<UndoCommand> &command) {
    return [this, command](bool ok) {
        if (!ok)
            dropUndo(command);
    };
}

std::shared_ptr<Backend::UndoCommand> Backend::pushReadsUndo(const std::shared_ptr<const QList<ReadChange>> &changes,
                                                             const std::shared_ptr<const qsizetype> &count) {
    auto command = std::make_shared<UndoCommand>();
    command->label = [count] { return tr("marked %n articles read", nullptr, int(*count)); };
    command->apply = [this, changes, state = command->state](bool undo, const Finished &finished) {
        restoreReads(changes, undo, state, finished);
    };
    pushUndo(command);
    return command;
}

bool Backend::keepReadsUndo(const std::shared_ptr<UndoCommand> &command, qsizetype count, bool tooLarge) {
    if (count == 0 || tooLarge) {
        dropUndo(command);
    } else {
        command->state->bytes = count * readChangeBytes;
        trimUndo();
    }
    return tooLarge;
}

void Backend::trimUndo() {
    const auto bytesOf = [](const QList<std::shared_ptr<UndoCommand>> &commands) {
        qsizetype bytes = 0;
        for (const auto &command : commands)
            bytes += command->state->bytes;
        return bytes;
    };
    bool trimmed = false;
    // One that could never fit goes alone, rather than after all the others.
    for (QList<std::shared_ptr<UndoCommand>> *commands : {&m_undo, &m_redo})
        trimmed = commands->removeIf([this](const auto &command) { return command->state->bytes > m_undoBytes; })
                  || trimmed;
    qsizetype bytes = bytesOf(m_undo) + bytesOf(m_redo);
    // The undo history's oldest first, then the redo history's furthest.
    for (QList<std::shared_ptr<UndoCommand>> *commands : {&m_undo, &m_redo}) {
        for (qsizetype i = 0; i < commands->size() && bytes > m_undoBytes;) {
            const qsizetype held = commands->at(i)->state->bytes;
            if (held == 0) {
                ++i;
                continue;
            }
            commands->removeAt(i);
            bytes -= held;
            trimmed = true;
        }
    }
    if (trimmed)
        emit undoChanged();
}

void Backend::undo() { undoOrRedo(true); }
void Backend::redo() { undoOrRedo(false); }

void Backend::undoOrRedo(bool undo) {
    // Until a restore that may fail for now is done, the history cannot
    // move past it: were it to fail, it goes back where it was.
    if (m_restoring) {
        setStatus(tr("Still undoing: %1").arg(m_restoring->label()));
        return;
    }
    QList<std::shared_ptr<UndoCommand>> &from = undo ? m_undo : m_redo;
    if (from.isEmpty()) {
        setStatus(undo ? tr("Nothing to undo") : tr("Nothing to redo"));
        return;
    }
    // Moved over at once, so the next Ctrl+Z reaches the command before it
    // even while this one's write is still on the worker, which takes the
    // writes in order.
    const std::shared_ptr<UndoCommand> command = from.takeLast();
    (undo ? m_redo : m_undo) << command;
    emit undoChanged();
    if (undo && command->mayRetry)
        m_restoring = command;
    command->apply(undo, [this, command, undo, pushes = m_pushes](bool ok) {
        if (m_restoring == command)
            m_restoring.reset();
        if (ok && !command->state->failed) {
            setStatus(undo ? tr("Undid: %1").arg(command->label()) : tr("Redid: %1").arg(command->label()));
            trimUndo();
            return;
        }
        // A restore that failed for now keeps the only copy of what the
        // unsubscribe took, to be undone again.
        // It goes under the actions asked for since, which cleared it from
        // the redo history, as it is still done.
        if (std::exchange(command->state->retry, false) && !command->state->failed) {
            m_redo.removeOne(command);
            m_undo.insert(qMax(qsizetype(0), m_undo.size() - qsizetype(m_pushes - pushes)), command);
            emit undoChanged();
            trimUndo();
            return;
        }
        dropUndo(command);
    });
}

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
    ArticleFilter filter = currentFilter();
    filter.scannedIds = m_viewScanned;
    const qint64 upToId = m_viewLastId;
    const QString failure = tr("Cannot mark the view read: %1");
    // The articles marked, which an undo reverses whatever came since.
    auto changes = std::make_shared<QList<ReadChange>>();
    auto count = std::make_shared<qsizetype>(0);
    auto overflow = std::make_shared<bool>(false);
    const auto command = pushReadsUndo(changes, count);
    writeOnWorker(
        [filter, upToId, changes, overflow, maxChanges = maxReadChanges(), readsPending = readsPublishedFor(),
         state = command->state](Store &store) {
            const int marked = store.markRead(filter, upToId, changes.get(), maxChanges, overflow.get());
            // An undo queued meanwhile has nothing to put back, and skips.
            if (*overflow)
                state->failed = true;
            if (marked > 0)
                markPending(store, readsPending, Sync::readStatusKind);
            return marked;
        },
        failure,
        [this, failure, changes, count, overflow, command](const WriteResult &result) {
            // The worker is done with the list until an undo.
            *count = changes->size();
            const bool tooLarge = keepReadsUndo(command, *count, *overflow);
            if (!result.error.isEmpty()) {
                setStatus(failure.arg(result.error), true);
                return;
            }
            setStatus(tooLarge ? tr("Marked %n articles read (too large to undo)", nullptr, result.changed)
                               : tr("Marked %n articles read", nullptr, result.changed));
            if (result.changed > 0)
                schedulePublish(Sync::readStatusKind);
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
                [profile, articles = both->first, videos = both->second,
                 pending = subscriptionsPublishedFor()](Store &store) {
                    const Library::Subscription subscription =
                        Library::subscribeNostr(store, profile, articles, videos);
                    if (subscription.created)
                        markPending(store, pending, Sync::subscriptionsKind);
                    return subscription;
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
                [search, results, pending = subscriptionsPublishedFor()](Store &store) {
                    SearchStep step;
                    search->examine(*results);
                    step.finished = search->finished();
                    if (step.finished) {
                        step.subscription = Library::subscribe(store, search->result());
                        if (step.subscription.created)
                            markPending(store, pending, Sync::subscriptionsKind);
                    }
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
    if (subscription.created) {
        setStatus(tr("Subscribed to %1 (%n articles)", nullptr, subscription.added).arg(title));
        schedulePublish(Sync::subscriptionsKind);
        if (subscription.markedRead > 0)
            schedulePublish(Sync::readStatusKind);
    } else {
        setStatus(tr("Already subscribed to %1").arg(title));
    }
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
    const QString title = Nip19::shortNpub(feed->title, feed->url);
    // An undo puts back what the delete took, as it took it: deleting for
    // real, rather than only once the undo can no longer come, keeps the
    // library, the headless commands, and the subscription list published
    // meanwhile telling the same story, and the undo subscribes again.
    auto snapshot = std::make_shared<FeedSnapshot>();
    auto command = std::make_shared<UndoCommand>();
    command->label = [title] { return tr("unsubscribed from %1").arg(title); };
    command->mayRetry = true;
    command->apply = [this, feedId, title, snapshot, state = command->state](bool undo, const Finished &finished) {
        if (undo)
            restoreSubscription(feedId, snapshot, state, finished);
        else
            removeSubscription(feedId, title, snapshot, state, finished);
    };
    pushUndo(command);
    removeSubscription(feedId, title, snapshot, command->state, [this, command](bool ok) {
        if (!ok)
            dropUndo(command);
        else
            trimUndo();
    });
    return true;
}

void Backend::removeSubscription(qint64 feedId, const QString &title, const std::shared_ptr<FeedSnapshot> &snapshot,
                                 const std::shared_ptr<UndoState> &state, const Finished &finished) {
    // The feed leaves the window at once and comes back if the delete fails.
    ++m_removing[feedId];
    m_unsubscribed.insert(feedId);
    reloadLibrary();
    const QString failure = tr("Cannot unsubscribe: %1");
    // Whether the snapshot fit the budget, told apart from the snapshot
    // itself, which a write queued after this one may be filling already.
    auto undoable = std::make_shared<bool>(false);
    writeOnWorker(
        [feedId, snapshot, state, undoable, maxBytes = m_snapshotBytes, maxItems = m_snapshotItems,
         pending = subscriptionsPublishedFor()](Store &store) {
            if (!store.removeFeed(feedId, snapshot.get(), maxBytes, maxItems))
                return -1;
            markPending(store, pending, Sync::subscriptionsKind);
            *undoable = !snapshot->feedColumns.isEmpty();
            state->bytes = snapshot->bytes;
            // Too large to keep: an undo queued meanwhile has nothing to
            // put back.
            if (!*undoable)
                state->failed = true;
            return 1;
        },
        failure,
        [this, feedId, title, failure, finished, undoable](const WriteResult &result) {
            if (--m_removing[feedId] <= 0)
                m_removing.remove(feedId);
            if (!result.error.isEmpty() || result.skipped) {
                // Still subscribed, so its refresh errors count again, unless
                // another delete of it is on its way.
                if (!m_removing.contains(feedId))
                    m_unsubscribed.remove(feedId);
                if (!result.error.isEmpty())
                    setStatus(failure.arg(result.error), true);
            } else {
                setStatus(*undoable ? tr("Unsubscribed from %1").arg(title)
                                    : tr("Unsubscribed from %1 (too large to undo)").arg(title));
                schedulePublish(Sync::subscriptionsKind);
            }
            finished(result.error.isEmpty() && !result.skipped && *undoable);
            reloadLibrary();
        },
        state);
}

void Backend::restoreSubscription(qint64 feedId, const std::shared_ptr<const FeedSnapshot> &snapshot,
                                  const std::shared_ptr<UndoState> &state, const Finished &finished) {
    const QString failure = undoFailure(true);
    // Whether the restore failed for good, so the undo cannot come again.
    auto lasting = std::make_shared<bool>(false);
    auto skipped = std::make_shared<bool>(false);
    // Marked to publish as subscribing marks it: the list published next
    // has the feed again, and none of the deleted marks the unsubscribe
    // may have published for it (Store::restoreFeed()). Not through
    // writeOnWorker()'s own skipping, which would forget the snapshot on a
    // failure that may pass.
    writeOnWorker(
        [snapshot, state, lasting, skipped, pending = subscriptionsPublishedFor()](Store &store) {
            if (state->failed) {
                *skipped = true;
                return 0;
            }
            if (!store.restoreFeed(*snapshot, lasting.get())) {
                if (*lasting)
                    state->failed = true;
                return -1;
            }
            markPending(store, pending, Sync::subscriptionsKind);
            return 1;
        },
        failure,
        [this, feedId, failure, finished, state, lasting, skipped](const WriteResult &result) {
            if (*skipped) {
                finished(false);
                return;
            }
            if (!result.error.isEmpty()) {
                setStatus(failure.arg(result.error), true);
                state->retry = !*lasting;
                finished(false);
                return;
            }
            // Subscribed again, so its refresh errors count again, unless a
            // redo is deleting it once more.
            if (!m_removing.contains(feedId))
                m_unsubscribed.remove(feedId);
            schedulePublish(Sync::subscriptionsKind);
            finished(true);
            reloadLibrary();
        });
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
    m_refreshMarked = 0;
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
    m_refreshMarked += outcome.markedRead;
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
    // The library marked them to publish as it saved them.
    if (m_refreshMarked > 0)
        schedulePublish(Sync::readStatusKind);
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
    const bool changed = account.npub != m_syncAccount.npub;
    // Confirmed only ever goes from false to true for an account, and the
    // write saying so may still be on its way.
    const bool confirmed = !changed && m_syncAccount.publishConfirmed;
    m_syncAccount = account;
    m_syncAccount.publishConfirmed = account.publishConfirmed || confirmed;
    if (changed) {
        resetPublish();
        // What was changed here and not published before the last quit goes
        // out with the first publish.
        for (int kind : {Sync::subscriptionsKind, Sync::readStatusKind, Sync::viewsKind})
            if ((account.publishPending & publishBit(kind))
                && (kind != Sync::readStatusKind || account.publishReads))
                m_publishKinds << kind;
        resetSigning();
        checkSigningKey();
    }
    emit syncChanged();
}

void Backend::setKeyStore(KeyStore *keyStore) {
    m_keyStore = keyStore;
    // A job still out to the store this one replaces may never finish, so
    // the queue starts afresh rather than waiting on it.
    ++m_keyQueue;
    m_keyJobs.clear();
    m_keyJobRunning = false;
    resetSigning();
    checkSigningKey();
}

void Backend::setSigning(bool stored, const QString &error, bool checked) {
    const bool busy = m_keyJobRunning || !m_keyJobs.isEmpty();
    if (stored == m_signingKeyStored && checked == m_signingKeyChecked && busy == m_signingBusy
        && error == m_signingError)
        return;
    const bool found = stored && !m_signingKeyStored;
    m_signingKeyStored = stored;
    m_signingKeyChecked = checked;
    m_signingBusy = busy;
    m_signingError = error;
    emit signingChanged();
    // What waited for a key goes out once one is known to be there.
    if (found)
        resumePublish();
}

QString Backend::keyStoreError(const KeyStore::Result &result) const {
    switch (result.error) {
    case KeyStore::Error::None:
        return {};
    case KeyStore::Error::Unavailable:
        return tr("No Secret Service is running, so the signing key cannot be kept. Start a keyring and try again.");
    case KeyStore::Error::Dismissed:
        return tr("The keyring prompt was dismissed.");
    case KeyStore::Error::Locked:
        return tr("The keyring is locked. Unlock it and try again.");
    case KeyStore::Error::Failed:
        break;
    }
    return tr("The keyring failed: %1").arg(result.detail);
}

void Backend::queueKeyJob(KeyJob job) {
    m_keyJobs.append(std::move(job));
    runKeyJobs();
    setSigning(m_signingKeyStored, m_signingError);
}

void Backend::runKeyJobs() {
    // A loop rather than recursion, as a job with nothing left to do
    // finishes before it returns; the flag keeps such a job's `finished`
    // from starting the next one itself, inside this loop.
    if (std::exchange(m_startingKeyJobs, true))
        return;
    while (!m_keyJobRunning && !m_keyJobs.isEmpty()) {
        m_keyJobRunning = true;
        const KeyJob job = m_keyJobs.takeFirst();
        auto once = std::make_shared<bool>(false);
        job([self = QPointer(this), queue = m_keyQueue, once] {
            if (!self || std::exchange(*once, true) || queue != self->m_keyQueue)
                return;
            self->m_keyJobRunning = false;
            if (self->m_startingKeyJobs)
                return;
            self->runKeyJobs();
            self->setSigning(self->m_signingKeyStored, self->m_signingError);
        });
    }
    m_startingKeyJobs = false;
}

void Backend::resetSigning() {
    ++m_signingEpoch;
    dropSigner(tr("The sync account changed before the event was signed"));
    endPairing();
    setRemoteError({});
    setPairing(false, false);
    setSigning(false, {}, false);
}

void Backend::setPairing(bool stored, bool checked) {
    if (stored == m_pairingStored && checked == m_pairingChecked)
        return;
    const bool found = stored && !m_pairingStored;
    m_pairingStored = stored;
    m_pairingChecked = checked;
    emit signingChanged();
    if (found)
        resumePublish();
}

void Backend::dropSigner(const QString &error) {
    releaseSigner();
    ++m_signerLoad;
    m_loadingSigner = false;
    m_loadingPairing = false;
    failPendingSigns(error);
}

void Backend::releaseSigner() {
    if (m_signerRemote) {
        m_signer->disconnect(this);
        m_signer.release()->deleteLater();
    }
    m_signer.reset();
    m_signerRemote = false;
    m_signerStale = false;
    updateRemoteWaiting();
}

void Backend::preferKey() {
    if (!m_signerRemote)
        return;
    if (m_signing.isEmpty())
        releaseSigner();
    else
        m_signerStale = true;
}

RemoteSigner *Backend::remoteSigner() const {
    return m_signerRemote ? static_cast<RemoteSigner *>(m_signer.get()) : nullptr;
}

void Backend::adoptRemoteSigner(std::unique_ptr<RemoteSigner> signer) {
    connect(signer.get(), &RemoteSigner::waitingChanged, this, &Backend::updateRemoteWaiting);
    connect(signer.get(), &RemoteSigner::authUrl, this, &Backend::updateRemoteWaiting);
    m_signer = std::move(signer);
    m_signerRemote = true;
}

void Backend::updateRemoteWaiting() {
    const RemoteSigner *signer = remoteSigner();
    const bool waiting = signer && signer->waiting() > 0;
    const QString url = currentAuthUrl();
    if (waiting == m_remoteWaiting && url == m_remoteAuthUrl)
        return;
    m_remoteAuthUrl = url;
    if (waiting != m_remoteWaiting) {
        m_remoteWaiting = waiting;
        // In the status bar too, as a publish signs on its own, dialog or
        // not; what it said before comes back once the signer has answered.
        const QString approve = tr("Approve the request in your signer app");
        if (waiting) {
            m_statusBeforeApproval = {m_status, m_statusError};
            setStatus(approve);
        } else if (m_status == approve) {
            setStatus(m_statusBeforeApproval.first, m_statusBeforeApproval.second);
        }
    }
    emit remoteSignerChanged();
}

QString Backend::currentAuthUrl() const {
    const QString url = m_pairingSigner ? m_pairingSigner->pendingAuthUrl() : QString();
    if (!url.isEmpty())
        return url;
    const RemoteSigner *signer = remoteSigner();
    return signer ? signer->pendingAuthUrl() : QString();
}

QString Backend::signerKind() const {
    if (m_signingKeyStored)
        return QStringLiteral("key");
    return m_pairingStored ? QStringLiteral("remote") : QStringLiteral("none");
}

QString Backend::nostrConnectQr() const {
    return m_nostrConnectUri.isEmpty() ? QString() : QrCode::source(m_nostrConnectUri);
}

void Backend::setRemoteError(const QString &error) {
    if (error == m_remoteError)
        return;
    m_remoteError = error;
    emit remoteSignerChanged();
}

bool Backend::canPair() {
    if (m_syncAccount.npub.isEmpty()) {
        setRemoteError(tr("Sync with an npub before connecting a remote signer"));
        return false;
    }
    if (!m_keyStore) {
        setRemoteError(keyStoreError(keyStoreUnavailable()));
        return false;
    }
    return true;
}

bool Backend::connectBunker(const QString &uri) {
    if (!canPair())
        return false;
    // Relays the user configured are trusted as for publishing; any other
    // must be a public wss:// one, as the URI is pasted from elsewhere.
    auto pairing = RemoteSigner::parseBunkerUri(uri, m_syncRelays->relays());
    if (!pairing) {
        setRemoteError(tr("That is not a bunker:// link with a relay Readstr can use"));
        return false;
    }
    beginPairing(std::move(*pairing));
    return m_pairingSigner != nullptr;
}

bool Backend::startNostrConnect() {
    if (!canPair())
        return false;
    // The sync relays, which the user already trusts with the account's
    // events; two keep the QR code small enough to scan easily.
    auto pairing = RemoteSigner::newPairing(m_syncRelays->relays().mid(0, 2));
    if (!pairing) {
        setRemoteError(tr("There is no sync relay to reach a signer app through"));
        return false;
    }
    const QString uri = RemoteSigner::nostrConnectUri(*pairing);
    beginPairing(std::move(*pairing));
    if (!m_pairingSigner)
        return false;
    m_nostrConnectUri = uri;
    emit remoteSignerChanged();
    return true;
}

void Backend::beginPairing(RemoteSigner::Pairing pairing) {
    endPairing();
    std::unique_ptr<RemoteSigner> signer = RemoteSigner::create(pairing, m_syncAccount.pubkey);
    pairing.wipe();
    if (!signer) {
        setRemoteError(tr("Cannot use that remote signer"));
        return;
    }
    signer->setLimits(m_remoteLimits);
    connect(signer.get(), &RemoteSigner::authUrl, this, &Backend::updateRemoteWaiting);
    connect(signer.get(), &RemoteSigner::waitingChanged, this, &Backend::updateRemoteWaiting);
    m_pairingSigner = std::move(signer);
    m_remoteError.clear();
    emit remoteSignerChanged();
    m_pairingSigner->pair([self = QPointer(this), epoch = m_pairingEpoch](const QString &error) {
        if (self && epoch == self->m_pairingEpoch)
            self->pairingDone(error);
    });
}

void Backend::endPairing() {
    ++m_pairingEpoch;
    // From the event loop, as this may run inside one of its callbacks.
    if (m_pairingSigner) {
        m_pairingSigner->disconnect(this);
        m_pairingSigner.release()->deleteLater();
    }
    m_nostrConnectUri.clear();
    m_remoteAuthUrl = currentAuthUrl();
    emit remoteSignerChanged();
}

void Backend::cancelRemoteConnect() {
    endPairing();
    setRemoteError({});
}

void Backend::copyNostrConnectUri() {
    if (!m_nostrConnectUri.isEmpty())
        m_writeClipboard(m_nostrConnectUri);
}

void Backend::pairingDone(const QString &error) {
    if (!error.isEmpty()) {
        endPairing();
        return setRemoteError(error);
    }
    if (!m_keyStore) {
        endPairing();
        return setRemoteError(keyStoreError(keyStoreUnavailable()));
    }
    // The link has been answered, and the pairing waits only on the keyring.
    m_nostrConnectUri.clear();
    emit remoteSignerChanged();
    RemoteSigner::Pairing pairing = m_pairingSigner->pairing();
    // Held until the job's turn comes, in a buffer that is wiped however the
    // job ends, as the client key in it can ask the signer to sign.
    const std::shared_ptr<QByteArray> json(new QByteArray(pairing.toJson()), [](QByteArray *bytes) {
        wipe(*bytes);
        delete bytes;
    });
    pairing.wipe();
    queueKeyJob([this, npub = m_syncAccount.npub, json, epoch = m_pairingEpoch, signingEpoch = m_signingEpoch,
                 write = ++m_pairingWrites](const std::function<void()> &finished) {
        if (epoch != m_pairingEpoch || signingEpoch != m_signingEpoch || !m_keyStore)
            return finished();
        // The keyring item is replaced in place, so once the write starts an
        // earlier pairing is gone whatever happens next.
        const bool replacing = m_pairingStored;
        m_keyStore->storePairing(npub, *json, [self = QPointer(this), npub, epoch, signingEpoch, write, replacing,
                                               finished](const KeyStore::Result &result) {
            if (self && signingEpoch == self->m_signingEpoch) {
                const bool stored = result.error == KeyStore::Error::None;
                if (epoch != self->m_pairingEpoch) {
                    // Stopped while the keyring wrote it. Over an earlier
                    // pairing it is kept: that one is already overwritten,
                    // and this signer proved it holds the account's key, so
                    // removing it would leave no signer at all; the signer
                    // in memory gives way to it. Otherwise it is taken out
                    // again, unless a newer pairing is to be written over it.
                    if (stored && replacing) {
                        self->setPairing(true, true);
                        self->preferKey();
                    } else if (stored) {
                        self->unstorePairing(npub, write);
                    }
                } else if (!stored) {
                    self->endPairing();
                    self->setRemoteError(self->keyStoreError(result));
                } else {
                    self->setPairing(true, true);
                    std::unique_ptr<RemoteSigner> signer = std::move(self->m_pairingSigner);
                    self->endPairing();
                    signer->disconnect(self);
                    // A signer of an earlier pairing gives way to this one.
                    self->preferKey();
                    // The key, when there is one, signs first; a read of
                    // the keyring already under way finds the pairing
                    // itself.
                    if (!self->m_signer && !self->m_signingKeyStored && !self->m_loadingSigner)
                        self->adoptRemoteSigner(std::move(signer));
                    else
                        signer.release()->deleteLater();
                    self->setStatus(tr("Connected to the remote signer"));
                    self->resumePublish();
                }
            }
            finished();
        });
        wipe(*json);
    });
}

void Backend::disconnectSigner() {
    const QString npub = m_syncAccount.npub;
    endPairing();
    setRemoteError({});
    if (npub.isEmpty())
        return;
    // At once, before the keyring answers, as for "Forget key". A read of
    // the key goes on, and only the pairing is not read after it.
    const QString gone = tr("The remote signer was disconnected before the event was signed");
    if (m_signerRemote && m_signerStale && m_signingKeyStored) {
        // It was giving way to the key, which signs what waits behind it.
        releaseSigner();
        failPendingSigns(gone, false);
        if (!m_pendingSigns.isEmpty())
            loadSigner();
        if (m_loadingSigner)
            m_pairingDisconnectedLoad = m_signerLoad;
    } else if (m_signerRemote || m_loadingPairing) {
        dropSigner(gone);
    } else if (m_loadingSigner) {
        m_pairingDisconnectedLoad = m_signerLoad;
    }
    if (!m_keyStore)
        return setPairing(false, m_pairingChecked);
    queueKeyJob([this, npub, epoch = m_signingEpoch](const std::function<void()> &finished) {
        if (epoch != m_signingEpoch || !m_keyStore)
            return finished();
        m_keyStore->removePairing(npub, [self = QPointer(this), epoch, finished](const KeyStore::Result &result) {
            if (self && epoch == self->m_signingEpoch) {
                if (result.error == KeyStore::Error::None)
                    self->setPairing(false, true);
                else
                    self->setSigning(self->m_signingKeyStored, self->keyStoreError(result));
            }
            finished();
        });
    });
}

void Backend::unstorePairing(const QString &npub, quint64 write) {
    queueKeyJob([this, npub, write, epoch = m_signingEpoch](const std::function<void()> &finished) {
        if (epoch != m_signingEpoch || write != m_pairingWrites || !m_keyStore)
            return finished();
        m_keyStore->removePairing(npub, [self = QPointer(this), epoch, finished](const KeyStore::Result &result) {
            if (self && epoch == self->m_signingEpoch) {
                if (result.error == KeyStore::Error::None) {
                    self->setPairing(false, true);
                    self->preferKey();
                } else {
                    self->setSigning(self->m_signingKeyStored, self->keyStoreError(result));
                }
            }
            finished();
        });
    });
}

void Backend::failPendingSigns(const QString &error, bool queuedToo) {
    QList<Signer::Done> dones;
    if (queuedToo) {
        for (auto &[event, done] : std::exchange(m_pendingSigns, {}))
            dones << std::move(done);
    }
    for (auto &done : std::exchange(m_signing, {}))
        dones << std::move(done);
    if (dones.isEmpty())
        return;
    // From the event loop, so no caller's code runs in the middle of what
    // failed them, which may be switching the account.
    QTimer::singleShot(0, this, [dones = std::move(dones), error] {
        for (const Signer::Done &done : dones)
            done({}, error);
    });
}

void Backend::checkSigningKey() {
    const QString npub = m_syncAccount.npub;
    if (npub.isEmpty() || !m_keyStore) {
        setPairing(false, true);
        return setSigning(false, {}, true);
    }
    queueKeyJob([this, npub, epoch = m_signingEpoch](const std::function<void()> &finished) {
        if (epoch != m_signingEpoch || !m_keyStore)
            return finished();
        m_keyStore->contains(npub, [self = QPointer(this), epoch, npub, finished](const KeyStore::Result &result) {
            if (self && epoch == self->m_signingEpoch) {
                // Only a keyring that answered "no such key" retires the signer:
                // a slow, locked or restarting keyring says nothing about the
                // key. Signs still queued are left to their load job, which
                // runs after any write queued behind this check and knows
                // then whether a key is stored.
                if (result.error == KeyStore::Error::None && !result.found && self->m_signer
                    && !self->m_signerRemote) {
                    self->releaseSigner();
                    self->failPendingSigns(noKeyStored(npub), false);
                }
                self->setSigning(result.found, self->keyStoreError(result), result.error == KeyStore::Error::None);
                if (result.found)
                    self->preferKey();
            }
            finished();
        });
    });
    // The pairing is asked about the same way, after the key, without being
    // read.
    queueKeyJob([this, npub, epoch = m_signingEpoch](const std::function<void()> &finished) {
        if (epoch != m_signingEpoch || !m_keyStore)
            return finished();
        m_keyStore->containsPairing(npub, [self = QPointer(this), epoch, npub,
                                           finished](const KeyStore::Result &result) {
            if (self && epoch == self->m_signingEpoch) {
                if (result.error == KeyStore::Error::None) {
                    if (!result.found && self->m_signerRemote) {
                        self->releaseSigner();
                        self->failPendingSigns(noKeyStored(npub), false);
                        // Signs queued behind a signer that was giving way
                        // have no load job of their own.
                        if (!self->m_pendingSigns.isEmpty())
                            self->loadSigner();
                    }
                    self->setPairing(result.found, true);
                } else if (self->m_signingError.isEmpty()) {
                    self->setSigning(self->m_signingKeyStored, self->keyStoreError(result));
                }
            }
            finished();
        });
    });
}

bool Backend::addSigningKey(const QString &text) {
    const QString npub = m_syncAccount.npub;
    if (npub.isEmpty()) {
        setSigning(false, tr("Sync with an npub before adding its signing key"));
        return false;
    }
    std::optional<QByteArray> secret = Nip19::secretKey(text);
    const auto cleanup = qScopeGuard([&secret] {
        if (secret)
            wipe(*secret);
    });
    // Made only to learn the key's public key, and dropped at once: the
    // key is read back from the keyring when something is to be signed.
    const std::unique_ptr<LocalSigner> signer = secret ? LocalSigner::create(*secret) : nullptr;
    if (!signer) {
        setSigning(m_signingKeyStored, tr("That is not an nsec or a 64-character hex secret key"));
        return false;
    }
    if (signer->pubkey() != m_syncAccount.pubkey) {
        setSigning(m_signingKeyStored, tr("That key is not the one for %1").arg(Nip19::shortNpub(npub, npub)));
        return false;
    }
    if (!m_keyStore) {
        setSigning(false, keyStoreError(keyStoreUnavailable()));
        return false;
    }
    // Held until the job's turn comes, in a copy of its own that is wiped
    // however the job ends, even when it is dropped unrun. A signer already
    // made stays: it was checked against the same public key, so it signs
    // as this key would.
    const std::shared_ptr<QByteArray> key(new QByteArray(secret->constData(), secret->size()), [](QByteArray *bytes) {
        wipe(*bytes);
        delete bytes;
    });
    setSigning(m_signingKeyStored, {});
    queueKeyJob([this, npub, key, epoch = m_signingEpoch](const std::function<void()> &finished) {
        if (epoch != m_signingEpoch || !m_keyStore)
            return finished();
        m_keyStore->store(npub, *key, [self = QPointer(this), epoch, finished](const KeyStore::Result &result) {
            if (self && epoch == self->m_signingEpoch) {
                const bool stored = result.error == KeyStore::Error::None;
                self->setSigning(stored || self->m_signingKeyStored, self->keyStoreError(result),
                                 stored || self->m_signingKeyChecked);
                // What waited for a key, or for a key that could not be
                // read, goes out with it, signed with it.
                if (stored) {
                    self->preferKey();
                    self->resumePublish();
                }
            }
            finished();
        });
        wipe(*key);
    });
    return true;
}

void Backend::forgetSigningKey() {
    const QString npub = m_syncAccount.npub;
    if (npub.isEmpty())
        return;
    // At once, before the keyring answers: a read of the key still out, as
    // for a sign waiting on an unlock prompt, must not bring it back.
    dropSigner(tr("The signing key was forgotten before the event was signed"));
    if (!m_keyStore)
        return setSigning(false, {});
    setSigning(m_signingKeyStored, {});
    queueKeyJob([this, npub, epoch = m_signingEpoch](const std::function<void()> &finished) {
        if (epoch != m_signingEpoch || !m_keyStore)
            return finished();
        m_keyStore->remove(npub, [self = QPointer(this), epoch, finished](const KeyStore::Result &result) {
            if (self && epoch == self->m_signingEpoch) {
                const bool removed = result.error == KeyStore::Error::None;
                self->setSigning(!removed && self->m_signingKeyStored, self->keyStoreError(result),
                                 removed || self->m_signingKeyChecked);
            }
            finished();
        });
    });
}

void Backend::sign(const NostrEvent &event, Signer::Done done) {
    if (m_signer && !m_signerStale)
        return handToSigner(event, std::move(done));
    const QString npub = m_syncAccount.npub;
    if (npub.isEmpty() || !m_keyStore) {
        const QString error = npub.isEmpty() ? tr("Sync with an npub before adding its signing key")
                                             : noKeyStored(npub);
        QTimer::singleShot(0, this, [done = std::move(done), error] { done({}, error); });
        return;
    }
    // Queued even when no key is known to be stored, as the check at launch
    // may still be out: the read waits behind it and asks only then. A
    // remote signer that gave way waits first for what it still signs.
    m_pendingSigns.append({event, std::move(done)});
    if (!m_signer)
        loadSigner();
}

void Backend::handToSigner(const NostrEvent &event, Signer::Done done) {
    const quint64 id = ++m_signId;
    m_signing.insert(id, std::move(done));
    m_signer->sign(event, [self = QPointer(this), id](const NostrEvent &signedEvent, const QString &error) {
        if (!self)
            return;
        const Signer::Done done = self->m_signing.take(id);
        // A remote signer that gave way to a key or a newer pairing goes
        // once it has answered all it was given, and the keyring is read
        // again for what came since.
        if (self->m_signerStale && self->m_signing.isEmpty()) {
            self->releaseSigner();
            if (!self->m_pendingSigns.isEmpty())
                self->loadSigner();
        }
        if (done)
            done(signedEvent, error);
    });
}

void Backend::loadSigner() {
    if (std::exchange(m_loadingSigner, true))
        return;
    const QString npub = m_syncAccount.npub, pubkey = m_syncAccount.pubkey;
    const QString missing = noKeyStored(npub);
    queueKeyJob([this, load = ++m_signerLoad, npub, pubkey, missing](const std::function<void()> &finished) {
        if (load != m_signerLoad)
            return finished();
        // Every keyring call asked for before this one has answered, so
        // whether a key is stored is known now, unless the last check
        // failed: then the read itself finds out.
        if (!m_keyStore) {
            m_loadingSigner = false;
            failPendingSigns(missing);
            return finished();
        }
        // Without a key, the remote signer the pairing names, if any.
        const auto remote = [self = QPointer(this), load, pubkey, missing, npub, finished] {
            if (!self || load != self->m_signerLoad || !self->m_keyStore)
                return finished();
            if (load == self->m_pairingDisconnectedLoad) {
                self->m_loadingSigner = false;
                self->failPendingSigns(tr("The remote signer was disconnected before the event was signed"));
                return finished();
            }
            if (self->m_pairingChecked && !self->m_pairingStored) {
                self->m_loadingSigner = false;
                self->failPendingSigns(missing);
                return finished();
            }
            self->m_loadingPairing = true;
            self->m_keyStore->lookupPairing(npub, [self, load, pubkey, missing,
                                                   finished](const KeyStore::Result &result) {
                if (self && load == self->m_signerLoad) {
                    self->m_loadingSigner = false;
                    self->m_loadingPairing = false;
                    if (result.error != KeyStore::Error::None) {
                        const QString error = self->keyStoreError(result);
                        self->setSigning(self->m_signingKeyStored, error);
                        self->failPendingSigns(error);
                    } else if (!result.found) {
                        self->setPairing(false, true);
                        self->failPendingSigns(missing);
                    } else {
                        self->setPairing(true, true);
                        auto pairing = RemoteSigner::Pairing::fromJson(result.secret);
                        std::unique_ptr<RemoteSigner> signer =
                            pairing ? RemoteSigner::create(*pairing, pubkey) : nullptr;
                        if (pairing)
                            pairing->wipe();
                        if (!signer) {
                            self->failPendingSigns(tr("The remote signer pairing in the keyring cannot be used"));
                        } else {
                            signer->setLimits(self->m_remoteLimits);
                            self->adoptRemoteSigner(std::move(signer));
                            for (auto &[event, done] : std::exchange(self->m_pendingSigns, {}))
                                self->handToSigner(event, std::move(done));
                        }
                    }
                }
                finished();
            });
        };
        if (m_signingKeyChecked && !m_signingKeyStored)
            return remote();
        m_keyStore->lookup(npub, [self = QPointer(this), load, pubkey, missing, npub, finished,
                                  remote](const KeyStore::Result &result) {
            if (self && load == self->m_signerLoad) {
                // Another program of this user's can write the item, so the
                // key must still be the account's before anything is signed
                // with it.
                std::unique_ptr<LocalSigner> signer = result.found ? LocalSigner::create(result.secret) : nullptr;
                if (result.error == KeyStore::Error::None && !result.found) {
                    self->setSigning(false, {}, true);
                    return remote();
                }
                self->m_loadingSigner = false;
                if (result.error != KeyStore::Error::None) {
                    // Shown in the dialog too, which would otherwise say
                    // nothing of why every sign fails.
                    const QString error = self->keyStoreError(result);
                    self->setSigning(self->m_signingKeyStored, error);
                    self->failPendingSigns(error);
                } else {
                    self->setSigning(true, {}, true);
                    if (!signer || signer->pubkey() != pubkey) {
                        self->failPendingSigns(
                            tr("The key in the keyring is not the one for %1").arg(Nip19::shortNpub(npub, npub)));
                    } else {
                        self->m_signer = std::move(signer);
                        for (auto &[event, done] : std::exchange(self->m_pendingSigns, {}))
                            self->handToSigner(event, std::move(done));
                    }
                }
            }
            finished();
        });
    });
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

void Backend::stopSync(bool removeKey) {
    if (!m_storeOpen || m_syncAccount.npub.isEmpty())
        return;
    const QString npub = m_syncAccount.npub;
    writeOnWorker([](Store &store) { return store.clearSyncAccount() ? 1 : -1; },
                  tr("Cannot stop syncing: %1"), [this, npub, removeKey](const WriteResult &result) {
                      if (!result.error.isEmpty()) {
                          // The account stays, so its key stays with it.
                          setStatus(tr("Cannot stop syncing: %1").arg(result.error), true);
                          return;
                      }
                      reloadSyncAccount();
                      const QString shortNpub = Nip19::shortNpub(npub, npub);
                      if (!removeKey)
                          return setStatus(tr("Stopped syncing with %1").arg(shortNpub));
                      // Removed only once the account is gone, so a keyring
                      // prompt left open never keeps the account syncing.
                      const auto removed = [self = QPointer(this), shortNpub](const KeyStore::Result &outcome) {
                          if (!self)
                              return;
                          if (outcome.error == KeyStore::Error::None)
                              return self->setStatus(
                                  tr("Stopped syncing with %1 and removed its signing key").arg(shortNpub));
                          self->setStatus(tr("Stopped syncing with %1, but its signing key could not be removed: %2")
                                              .arg(shortNpub, self->keyStoreError(outcome)),
                                          true);
                      };
                      if (!m_keyStore)
                          return removed(keyStoreUnavailable());
                      setStatus(tr("Removing the signing key for %1…").arg(shortNpub));
                      // Queued like every keyring call, so a key added again
                      // meanwhile is stored after this removal, not deleted
                      // by it.
                      queueKeyJob([this, npub, removed](const std::function<void()> &finished) {
                          if (!m_keyStore) {
                              removed(keyStoreUnavailable());
                              return finished();
                          }
                          // The pairing with a remote signer goes too.
                          m_keyStore->remove(npub, [self = QPointer(this), npub, removed,
                                                    finished](const KeyStore::Result &outcome) {
                              if (!self || !self->m_keyStore) {
                                  removed(outcome);
                                  return finished();
                              }
                              self->m_keyStore->removePairing(npub, [outcome, removed,
                                                                     finished](const KeyStore::Result &pairing) {
                                  removed(outcome.error != KeyStore::Error::None ? outcome : pairing);
                                  finished();
                              });
                          });
                      });
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
    if (outcome.added > 0)
        m_syncFeedsAdded = true;
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
    // A pull for the account in the window: the publish that waited on it
    // goes on, and changes a failed one left go out again with it.
    if (!m_syncAccount.pubkey.isEmpty()
        && (std::exchange(m_publishPull, false)
            || (!m_publishKinds.isEmpty() && !m_publishing && !m_publishHeld && !m_publishTimer.isActive()
                && m_syncAccount.publishConfirmed && !keyMissing()))) {
        m_publishing = true;
        emit publishChanged();
        continuePublish(outcome);
    }
    if (!std::exchange(m_syncFeedsAdded, false))
        return;
    // The feeds the sync added have never been fetched.
    if (m_refreshing)
        m_fetchNewFeeds = true;
    else
        refreshUnfetched();
}

struct Backend::PublishRun {
    quint64 epoch = 0;
    SyncAccount account;
    QList<NostrEvent> events;
    qsizetype next = 0;
    int published = 0;
    QStringList errors;
    // Shown in the dialog only: kinds waiting on a pull, not failing.
    QStringList notes;
    QList<int> failed;
    // Published, or with nothing left to publish.
    QList<int> settled;
    bool signingFailed = false;
    // When the drafts were built: a feed subscribed after this is newer
    // than the list published from them.
    qint64 builtAt = 0;
};

namespace {
// Whole sentences per kind, so each language words them its own way.
QString unreadable(int kind) {
    switch (kind) {
    case Sync::subscriptionsKind:
        return Backend::tr("the relays hold a subscription list this app cannot read, so it is not replaced");
    case Sync::readStatusKind:
        return Backend::tr("the relays hold a read status this app cannot read, so it is not replaced");
    default:
        return Backend::tr("the relays hold saved views this app cannot read, so they are not replaced");
    }
}

QString refused(int kind, const QString &error) {
    switch (kind) {
    case Sync::subscriptionsKind:
        return Backend::tr("no relay took the subscription list: %1").arg(error);
    case Sync::readStatusKind:
        return Backend::tr("no relay took the read status: %1").arg(error);
    default:
        return Backend::tr("no relay took the saved views: %1").arg(error);
    }
}
}

QVariant Backend::publishedAt() const {
    return m_publishedAt.isValid() ? QVariant(m_publishedAt.toLocalTime()) : QVariant();
}

void Backend::setPublishReads(bool publish) {
    if (!m_storeOpen || m_syncAccount.pubkey.isEmpty() || publish == m_syncAccount.publishReads)
        return;
    m_syncAccount.publishReads = publish;
    emit syncChanged();
    const QString failure = tr("Cannot change what is published: %1");
    writeOnWorker([publish, pubkey = m_syncAccount.pubkey](Store &store) {
        return store.setPublishReads(pubkey, publish) ? 1 : -1;
    }, failure,
                  [this, failure, publish](const WriteResult &result) {
                      if (!result.error.isEmpty())
                          setStatus(failure.arg(result.error), true);
                      reloadSyncAccount();
                      if (result.error.isEmpty() && publish)
                          schedulePublish(Sync::readStatusKind);
                  });
}

void Backend::publishNow() {
    if (!m_storeOpen || m_syncAccount.pubkey.isEmpty())
        return;
    QList<int> kinds{Sync::subscriptionsKind, Sync::viewsKind};
    if (m_syncAccount.publishReads)
        kinds << Sync::readStatusKind;
    for (int kind : std::as_const(kinds))
        markPublishPending(kind);
    m_publishHeld = false;
    m_publishTimer.stop();
    // The user has seen which npub this library goes out under, so changes
    // may go out on their own from now on.
    if (!m_syncAccount.publishConfirmed && !keyMissing()) {
        m_syncAccount.publishConfirmed = true;
        emit syncChanged();
        const QString failure = tr("Cannot change what is published: %1");
        writeOnWorker([pubkey = m_syncAccount.pubkey](Store &store) {
            return store.setPublishConfirmed(pubkey) ? 1 : -1;
        }, failure, [this, failure](const WriteResult &result) {
            if (!result.error.isEmpty())
                setStatus(failure.arg(result.error), true);
        });
    }
    startPublish(true);
}

void Backend::markPublishPending(int kind) {
    m_publishKinds << kind;
    savePublishPending(m_syncAccount.pubkey, publishBit(kind), true);
}

void Backend::savePublishPending(const QString &pubkey, int mask, bool pending) {
    writeOnWorker([pubkey, mask, pending](Store &store) {
        return store.markPublishPending(pubkey, mask, pending) ? 1 : -1;
    }, tr("Cannot remember what is left to publish: %1"), [](const WriteResult &result) {
        if (!result.error.isEmpty())
            qWarning("readstr: cannot remember what is left to publish: %s", qPrintable(result.error));
    });
}

QString Backend::readsPublishedFor() const {
    return m_storeOpen && m_syncAccount.publishReads ? m_syncAccount.pubkey : QString();
}

QString Backend::subscriptionsPublishedFor() const {
    return m_storeOpen ? m_syncAccount.pubkey : QString();
}

void Backend::markPending(Store &store, const QString &pubkey, int kind) {
    // Failing only when the account changed meanwhile, as then the change is
    // not this account's to publish, or with the library, which the window
    // says when it marks the kind again.
    if (!pubkey.isEmpty())
        store.markPublishPending(pubkey, publishBit(kind), true);
}

void Backend::schedulePublish(int kind) {
    if (!m_storeOpen || m_syncAccount.pubkey.isEmpty()
        || (kind == Sync::readStatusKind && !m_syncAccount.publishReads))
        return;
    markPublishPending(kind);
    m_publishHeld = false;
    // Kept until "Publish now" confirms the account, and it goes out then.
    if (!m_syncAccount.publishConfirmed)
        return;
    const bool soon = std::any_of(m_publishKinds.cbegin(), m_publishKinds.cend(),
                                  [](int marked) { return marked != Sync::readStatusKind; });
    if (!m_publishWaiting.isValid())
        m_publishWaiting.start();
    // A change that keeps coming, as reading does, still goes out in time.
    const qint64 left = qMax<qint64>(0, m_publishMaxWaitMs - m_publishWaiting.elapsed());
    m_publishTimer.start(int(qMin<qint64>(soon ? m_publishDelayMs : m_readPublishDelayMs, left)));
}

void Backend::resumePublish() {
    if (m_publishKinds.isEmpty() || !m_syncAccount.publishConfirmed || m_publishing || m_publishTimer.isActive())
        return;
    m_publishHeld = false;
    m_publishTimer.start(m_publishDelayMs);
}

void Backend::startPublish(bool asked) {
    if (m_publishing) {
        m_publishAgain = true;
        return;
    }
    if (m_publishKinds.isEmpty() || m_syncAccount.pubkey.isEmpty() || (!asked && !m_syncAccount.publishConfirmed))
        return;
    // Nothing is signed without a key; what changed waits for one, and
    // only someone who asked hears why. A keyring that could not be checked
    // may hold one, and the read when signing finds out.
    if (keyMissing()) {
        if (asked) {
            m_publishError = noKeyStored(m_syncAccount.npub);
            emit publishChanged();
        }
        return;
    }
    m_publishWaiting.invalidate();
    m_publishing = true;
    m_publishPull = true;
    emit publishChanged();
    // The pull goes first, so a newer event from another device is merged
    // before it is replaced. One that changes nothing leaves what the
    // window said before it.
    if (!m_syncing && !m_refreshStatus)
        m_refreshStatus = {m_status, m_statusError};
    sync();
}

void Backend::continuePublish(const Sync::Outcome &outcome) {
    auto run = std::make_shared<PublishRun>();
    run->epoch = m_publishEpoch;
    run->account = m_syncAccount;
    QList<int> kinds(m_publishKinds.cbegin(), m_publishKinds.cend());
    m_publishKinds.clear();
    std::sort(kinds.begin(), kinds.end());
    if (!m_syncAccount.publishReads && kinds.removeAll(Sync::readStatusKind) > 0)
        run->settled << Sync::readStatusKind;
    if (keyMissing()) {
        run->failed = kinds;
        return publishDone(run);
    }
    if (!outcome.error.isEmpty()) {
        run->failed = kinds;
        run->errors << tr("the pull before it failed: %1").arg(outcome.error);
        return publishDone(run);
    }
    QList<int> ready;
    for (int kind : std::as_const(kinds)) {
        if (outcome.refused.contains(kind)) {
            // Replacing what cannot be read here would lose it.
            run->failed << kind;
            run->errors << unreadable(kind);
        } else if (kind == Sync::readStatusKind && !outcome.settled) {
            // As on the web: another device's reads may sit on a relay that
            // did not answer.
            run->failed << kind;
            run->errors << tr("not every relay answered, so the read status is not replaced");
        } else {
            ready << kind;
        }
    }
    if (ready.isEmpty())
        return publishDone(run);
    run->builtAt = QDateTime::currentSecsSinceEpoch();
    onWorker<Sync::Drafts>(
        [pubkey = run->account.pubkey, ready, newest = outcome.newest, now = run->builtAt](Store &store) {
            return Sync::drafts(store, pubkey, ready, newest, now);
        },
        [this, run, ready](const Sync::Drafts &drafts) {
            if (run->epoch != m_publishEpoch)
                return;
            if (!drafts.error.isEmpty()) {
                run->failed << ready;
                run->errors << drafts.error;
                return publishDone(run);
            }
            run->events = drafts.events;
            run->settled << drafts.unchanged;
            run->failed << drafts.failed << drafts.waiting;
            run->errors << drafts.errors;
            run->notes << drafts.notes;
            publishNext(run);
        });
}

void Backend::publishNext(const std::shared_ptr<PublishRun> &run) {
    if (run->next >= run->events.size())
        return publishDone(run);
    const NostrEvent draft = run->events.at(run->next++);
    // Read articles turned off since the run began are not published, nor
    // even signed.
    const auto readsOff = [](Backend *backend, int kind) {
        return kind == Sync::readStatusKind && !backend->m_syncAccount.publishReads;
    };
    if (readsOff(this, draft.kind)) {
        run->settled << draft.kind;
        return publishNext(run);
    }
    sign(draft, [self = QPointer(this), run, kind = draft.kind, readsOff](const NostrEvent &event,
                                                                          const QString &error) {
        if (!self || run->epoch != self->m_publishEpoch)
            return;
        if (readsOff(self, kind)) {
            run->settled << kind;
            return self->publishNext(run);
        }
        if (!error.isEmpty()) {
            // Signing the rest would fail the same way, or prompt again.
            run->signingFailed = true;
            run->errors << tr("cannot sign: %1").arg(error);
            run->failed << kind;
            for (qsizetype i = run->next; i < run->events.size(); ++i)
                run->failed << run->events.at(i).kind;
            return self->publishDone(run);
        }
        // Read articles turned off while a relay still connects are not sent
        // to it either.
        const auto stillWanted = [self, kind, readsOff] { return self && !readsOff(self, kind); };
        self->m_syncRelays->publish(event, run->account.relays, [self, run, event](const PublishResult &result) {
            if (!self || run->epoch != self->m_publishEpoch)
                return;
            if (!result.ok() && result.cancelled) {
                run->settled << event.kind;
                return self->publishNext(run);
            }
            for (const PublishResult::Relay &relay : result.relays)
                if (!relay.accepted)
                    qWarning("readstr: publish: %s: %s", qPrintable(relay.relay), qPrintable(relay.message));
            if (!result.ok()) {
                run->failed << event.kind;
                run->errors << refused(event.kind, result.error());
                return self->publishNext(run);
            }
            self->onWorker<WriteResult>(
                [pubkey = run->account.pubkey, event, builtAt = run->builtAt](Store &store) {
                    WriteResult written;
                    bool superseded = false;
                    written.error =
                        Sync::recordPublished(store, pubkey, event, builtAt, &superseded, &written.reload);
                    written.changed = superseded ? 1 : 0;
                    return written;
                },
                [self, run, event](const WriteResult &written) {
                    if (!self || run->epoch != self->m_publishEpoch)
                        return;
                    // The relays have it either way.
                    ++run->published;
                    // A newer version won meanwhile, which may lack what this
                    // one carried: built again on it after the next pull.
                    if (written.changed > 0)
                        run->failed << event.kind;
                    else
                        run->settled << event.kind;
                    if (!written.error.isEmpty())
                        run->errors << tr("published, but cannot record it: %1").arg(written.error);
                    // The tags and category styles the list carried on, when
                    // the library lacked them, show without waiting for a pull.
                    if (written.reload)
                        self->reloadLibrary();
                    self->publishNext(run);
                });
        }, stillWanted);
    });
}

void Backend::publishDone(const std::shared_ptr<PublishRun> &run) {
    if (run->epoch != m_publishEpoch)
        return;
    m_publishing = false;
    for (int kind : std::as_const(run->failed))
        m_publishKinds << kind;
    // Kinds done, unless changed again meanwhile, need not go out after a
    // restart.
    int done = 0;
    for (int kind : std::as_const(run->settled))
        if (!m_publishKinds.contains(kind))
            done |= publishBit(kind);
    if (done) {
        m_syncAccount.publishPending &= ~done;
        savePublishPending(run->account.pubkey, done, false);
    }
    m_publishHeld = run->signingFailed;
    if (run->published > 0)
        m_publishedAt = QDateTime::currentDateTimeUtc();
    const QStringList problems = run->errors + run->notes;
    m_publishError = problems.isEmpty() ? QString() : tr("Cannot publish to Nostr: %1").arg(problems.join("; "));
    emit publishChanged();
    if (!run->errors.isEmpty()) {
        qWarning("readstr: %s", qPrintable(m_publishError));
        setStatus(m_publishError, true);
    }
    if (std::exchange(m_publishAgain, false) && !m_publishHeld)
        startPublish();
}

void Backend::resetPublish() {
    ++m_publishEpoch;
    m_publishTimer.stop();
    m_publishKinds.clear();
    m_publishPull = m_publishAgain = m_publishHeld = false;
    m_publishWaiting.invalidate();
    m_publishedAt = {};
    m_publishError.clear();
    m_publishing = false;
    emit publishChanged();
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
    onWorker<Library::Import>(
        [path, pending = subscriptionsPublishedFor()](Store &store) {
            const Library::Import result = Library::importFile(store, path);
            if (result.error.isEmpty() && result.added > 0)
                markPending(store, pending, Sync::subscriptionsKind);
            return result;
        },
        [this](const Library::Import &result) {
            // The import is one transaction, so a failed one added nothing to
            // reload or fetch.
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
            schedulePublish(Sync::subscriptionsKind);
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

void Backend::loadFilterRules() {
    m_filterRules.clear();
    m_filterError.clear();
    if (m_storeOpen) {
        QStringList dropped;
        if (const auto rules = m_store.filterRules(&dropped))
            m_filterRules = *rules;
        else
            m_filterError = m_store.lastError();
        if (m_filterError.isEmpty())
            m_filterError = dropped.join(QLatin1String("; "));
    }
    if (!m_filterError.isEmpty())
        setStatus(tr("Some filter rules cannot be used and are left out: %1").arg(m_filterError), true);
    emit filtersChanged();
}

QVariantList Backend::filterRules() const {
    QVariantList list;
    for (const KeywordFilter::Rule &rule : m_filterRules)
        list << KeywordFilter::toJson(rule, int(list.size())).toVariantMap();
    return list;
}

QString Backend::filterProblem(const QString &type, const QString &pattern) const {
    const auto matchType = KeywordFilter::typeFrom(type);
    if (!matchType)
        return tr("Pick how the pattern is matched");
    QString detail;
    switch (KeywordFilter::check(*matchType, pattern, &detail)) {
    case KeywordFilter::Problem::None: return {};
    case KeywordFilter::Problem::Empty: return tr("Enter a word, a phrase, or a pattern");
    case KeywordFilter::Problem::TooLong:
        return tr("A pattern can be at most %1 characters long").arg(KeywordFilter::maxPatternLength);
    case KeywordFilter::Problem::Invalid: return tr("This is not a valid regular expression: %1").arg(detail);
    case KeywordFilter::Problem::Unsupported:
        return tr("Regular expressions here cannot use lookarounds or backreferences");
    }
    return {};
}

bool Backend::saveFilterRule(int index, const QVariantMap &map) {
    const bool adding = index < 0;
    if ((!adding && index >= m_filterRules.size()) || (adding && m_filterRules.size() >= KeywordFilter::maxRules))
        return false;
    QJsonObject object = QJsonObject::fromVariantMap(map);
    object.insert(QStringLiteral("id"), adding ? KeywordFilter::newId() : m_filterRules.at(index).id);
    const auto rule = KeywordFilter::fromJson(object);
    if (!rule)
        return false;
    const QList<KeywordFilter::Rule> before = m_filterRules;
    if (adding)
        m_filterRules << *rule;
    else if (m_filterRules.at(index) == *rule)
        return true;
    else
        m_filterRules[index] = *rule;
    if (adding)
        changeFilterRules(before, [] { return tr("added a filter rule"); });
    else
        changeFilterRules(before, [] { return tr("changed a filter rule"); });
    return true;
}

void Backend::setFilterRuleEnabled(int index, bool enabled) {
    if (index < 0 || index >= m_filterRules.size() || m_filterRules.at(index).enabled == enabled)
        return;
    const QList<KeywordFilter::Rule> before = m_filterRules;
    m_filterRules[index].enabled = enabled;
    changeFilterRules(before, [] { return tr("changed a filter rule"); });
}

void Backend::moveFilterRule(int index, int step) {
    const qint64 target = qint64(index) + step;
    if (index < 0 || index >= m_filterRules.size() || target < 0 || target >= m_filterRules.size() || step == 0)
        return;
    const QList<KeywordFilter::Rule> before = m_filterRules;
    m_filterRules.move(index, int(target));
    changeFilterRules(before, [] { return tr("changed a filter rule"); });
}

void Backend::removeFilterRule(int index) {
    if (index < 0 || index >= m_filterRules.size())
        return;
    const QList<KeywordFilter::Rule> before = m_filterRules;
    m_filterRules.removeAt(index);
    changeFilterRules(before, [] { return tr("removed a filter rule"); });
}

void Backend::changeFilterRules(const QList<KeywordFilter::Rule> &before, std::function<QString()> label) {
    auto command = std::make_shared<UndoCommand>();
    command->label = std::move(label);
    // Through the same save as the change, so the save counter moves on and
    // the library is filtered again.
    command->apply = [this, before, after = m_filterRules, state = command->state](bool undo,
                                                                                    const Finished &finished) {
        m_filterRules = undo ? before : after;
        saveFilterRules(finished, state);
    };
    pushUndo(command);
    saveFilterRules(dropUndoOnFailure(command), command->state);
}

void Backend::saveFilterRules(const Finished &finished, const std::shared_ptr<UndoState> &undo) {
    emit filtersChanged();
    if (!m_storeOpen) {
        if (finished)
            finished(false);
        return;
    }
    const QString failure = tr("Cannot save the filter rules: %1");
    // Quick edits queue several saves on the worker. Each saves the whole
    // list, so only the latest needs to run: an older one still waiting is
    // skipped, and one under way stops between its batches.
    // At closing, the rules are still saved, and the pass stops at once.
    const quint64 generation = ++*m_filterGeneration;
    const auto superseded = [latest = m_filterGeneration, generation] { return *latest != generation; };
    const auto stop = [superseded, quitting = m_quitting] { return superseded() || quitting->load(); };
    // Saved, but not applied to every article.
    const auto applyFailed = std::make_shared<bool>(false);
    const auto skipped = std::make_shared<bool>(false);
    // Not through writeOnWorker()'s own failing of `undo`: whether a change
    // was saved is settled here, by the save that commits the list, which
    // holds every change made before it, or by the last one failing; and a
    // list saved but not applied is saved.
    writeOnWorker(
        [rules = m_filterRules, superseded, stop, applyFailed, skipped, undo](Store &store) {
            if (undo && undo->failed) {
                *skipped = true;
                return 0;
            }
            if (superseded())
                return 0;
            bool saved = false;
            const bool ok = store.setFilterRules(rules, stop, &saved);
            *applyFailed = saved && !ok;
            return ok ? 1 : -1;
        },
        failure,
        [this, failure, superseded, applyFailed, skipped, finished](const WriteResult &result) {
            // Saved, if only the rules and not their pass over the library.
            const bool saved = (result.changed > 0 && result.error.isEmpty()) || *applyFailed;
            // Not saved, skipped or failed, with a later save to come, which
            // saves this one's change with its own, or fails to: that one
            // says which, so no command keeps a change the library never
            // had.
            if (!saved && superseded()) {
                if (finished)
                    m_filterSavesWaiting << finished;
                return;
            }
            for (const Finished &waiting : std::exchange(m_filterSavesWaiting, {}))
                waiting(saved);
            if (finished)
                finished(saved);
            // The latest save says how the library is left.
            if (superseded())
                return;
            if (*skipped) {
                // An undo or redo of a change never saved: the rules shown
                // are the ones the library keeps.
                loadFilterRules();
            } else if (*applyFailed) {
                // The next change, or the next start, applies them.
                setStatus(tr("Cannot apply the filter rules: %1").arg(result.error), true);
            } else if (!result.error.isEmpty()) {
                // The rules shown are the ones the library keeps.
                loadFilterRules();
                setStatus(failure.arg(result.error), true);
            } else if (!m_filterError.isEmpty()) {
                // The ones left out are gone with this save.
                m_filterError.clear();
                emit filtersChanged();
            }
            reloadLibrary();
        });
}

void Backend::markFilterMatchesRead(const QString &ruleId) {
    if (!m_storeOpen)
        return;
    const QString failure = tr("Cannot mark the matching articles read: %1");
    // The articles marked, which an undo reverses whatever came since.
    auto changes = std::make_shared<QList<ReadChange>>();
    auto count = std::make_shared<qsizetype>(0);
    auto overflow = std::make_shared<bool>(false);
    const auto command = pushReadsUndo(changes, count);
    writeOnWorker(
        [ruleId, changes, overflow, maxChanges = maxReadChanges(), readsPending = readsPublishedFor(),
         quitting = m_quitting, state = command->state](Store &store) {
            // Some batches may be written before one fails.
            int marked = 0;
            const int result =
                store.markReadByFilter(ruleId, [quitting] { return quitting->load(); }, &marked, changes.get(),
                                       maxChanges, overflow.get());
            // An undo queued meanwhile has nothing to put back, and skips.
            if (*overflow)
                state->failed = true;
            if (marked > 0)
                markPending(store, readsPending, Sync::readStatusKind);
            return result;
        },
        failure,
        [this, failure, changes, count, overflow, command](const WriteResult &result) {
            // The worker is done with the list until an undo; the batches
            // written before a failure can be undone too.
            *count = changes->size();
            const bool tooLarge = keepReadsUndo(command, *count, *overflow);
            if (!result.error.isEmpty()) {
                setStatus(failure.arg(result.error), true);
                reloadCounts();
                loadArticles();
                return;
            }
            setStatus(tooLarge ? tr("Marked %n articles read (too large to undo)", nullptr, result.changed)
                               : tr("Marked %n articles read", nullptr, result.changed));
            if (result.changed > 0)
                schedulePublish(Sync::readStatusKind);
            reloadCounts();
            loadArticles();
        });
}
