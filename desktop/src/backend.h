#pragma once

#include <QObject>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <functional>
#include <memory>

#include "articlemodel.h"
#include "fetcher.h"
#include "library.h"
#include "sourcemodel.h"
#include "store.h"

class FilePicker;
class LibraryWorker;

// The facade QML talks to as `backend`: the sources and articles models, the
// article in the reader, and every action the window offers.
//
// Reads and the small writes (read, starred) use a Store on this thread; the
// library is in WAL mode, so they never wait on a refresh. Refreshing and
// subscribing fetch on this thread's event loop through Fetcher, never in a
// nested loop, and hand each response to a worker thread with its own Store,
// which parses and saves it with the same Library code the headless commands
// use. The models reload once a batch is done.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(SourceModel *sources READ sources CONSTANT)
    Q_PROPERTY(ArticleModel *articles READ articles CONSTANT)
    Q_PROPERTY(int feedCount READ feedCount NOTIFY libraryChanged)

    Q_PROPERTY(int sourceIndex READ sourceIndex NOTIFY sourceChanged)
    Q_PROPERTY(QString sourceKind READ sourceKind NOTIFY sourceChanged)
    Q_PROPERTY(QString sourceTitle READ sourceTitle NOTIFY sourceChanged)
    Q_PROPERTY(qint64 selectedFeedId READ selectedFeedId NOTIFY sourceChanged)

    Q_PROPERTY(int articleIndex READ articleIndex NOTIFY articleChanged)
    Q_PROPERTY(bool hasArticle READ hasArticle NOTIFY articleChanged)
    Q_PROPERTY(qint64 articleId READ articleId NOTIFY articleChanged)
    Q_PROPERTY(QString articleTitle READ articleTitle NOTIFY articleChanged)
    Q_PROPERTY(QString articleFeed READ articleFeed NOTIFY articleChanged)
    Q_PROPERTY(QString articleAuthor READ articleAuthor NOTIFY articleChanged)
    Q_PROPERTY(QString articleUrl READ articleUrl NOTIFY articleChanged)
    Q_PROPERTY(QVariant articlePublished READ articlePublished NOTIFY articleChanged)
    Q_PROPERTY(QString articleHtml READ articleHtml NOTIFY articleChanged)
    Q_PROPERTY(bool articleRead READ articleRead NOTIFY articleChanged)
    Q_PROPERTY(bool articleStarred READ articleStarred NOTIFY articleChanged)

    Q_PROPERTY(bool refreshing READ refreshing NOTIFY refreshChanged)
    Q_PROPERTY(int refreshDone READ refreshDone NOTIFY refreshChanged)
    Q_PROPERTY(int refreshTotal READ refreshTotal NOTIFY refreshChanged)

    Q_PROPERTY(bool adding READ adding NOTIFY addChanged)
    Q_PROPERTY(QString addError READ addError NOTIFY addChanged)

    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool statusError READ statusError NOTIFY statusChanged)

public:
    // The window shows at most this many articles per view, newest first.
    static constexpr int viewLimit = 2000;
    static constexpr int autoRefreshMinutes = 30;

    // `picker` answers the OPML dialogs; without one they report that.
    explicit Backend(FilePicker *picker = nullptr, QObject *parent = nullptr);
    ~Backend() override;

    SourceModel *sources() { return &m_sources; }
    ArticleModel *articles() { return &m_articles; }
    int feedCount() const { return m_feedCount; }

    int sourceIndex() const { return m_sourceIndex; }
    QString sourceKind() const;
    QString sourceTitle() const;
    qint64 selectedFeedId() const;

    int articleIndex() const { return m_articleIndex; }
    bool hasArticle() const { return m_article.id != 0; }
    qint64 articleId() const { return m_article.id; }
    QString articleTitle() const { return m_article.title; }
    QString articleFeed() const { return m_article.feedTitle; }
    QString articleAuthor() const { return m_article.author; }
    QString articleUrl() const { return m_article.url; }
    QVariant articlePublished() const;
    QString articleHtml() const { return m_articleHtml; }
    bool articleRead() const { return m_article.read; }
    bool articleStarred() const { return m_article.starred; }

    bool refreshing() const { return m_refreshing; }
    int refreshDone() const { return m_refreshDone; }
    int refreshTotal() const { return m_refreshTotal; }

    bool adding() const { return m_adding; }
    QString addError() const { return m_addError; }

    QString status() const { return m_status; }
    bool statusError() const { return m_statusError; }

    Q_INVOKABLE void selectSource(int row);
    Q_INVOKABLE void moveSource(int step);
    Q_INVOKABLE void selectArticle(int row);
    Q_INVOKABLE void moveArticle(int step);
    Q_INVOKABLE void nextUnread();
    Q_INVOKABLE void toggleRead();
    Q_INVOKABLE void toggleStar();
    Q_INVOKABLE void openOriginal();
    // Opens a link from an article, but only an http(s) one.
    Q_INVOKABLE bool openLink(const QString &link);
    Q_INVOKABLE void markViewRead();
    Q_INVOKABLE void addFeed(const QString &address);
    Q_INVOKABLE void clearAddError();
    Q_INVOKABLE bool unsubscribe(qint64 feedId);
    Q_INVOKABLE void refreshAll();
    Q_INVOKABLE void refreshFeed(qint64 feedId);
    Q_INVOKABLE void importOpml();
    Q_INVOKABLE void exportOpml();
    // Refreshes now and then every autoRefreshMinutes, give or take 10%,
    // for as long as the window is open.
    Q_INVOKABLE void startAutoRefresh();

    void importFrom(const QUrl &url);
    void exportTo(const QUrl &url);
    // The delay before the next automatic refresh, for `unit` in [0, 1].
    static int autoRefreshDelayMs(double unit);
    int nextAutoRefreshMs() const { return m_autoRefresh.isActive() ? m_autoRefresh.remainingTime() : -1; }

    // How links leave the app; tests replace it.
    void setUrlOpener(std::function<bool(const QUrl &)> opener) { m_openUrl = std::move(opener); }
    void setFetchLimits(const Fetcher::Limits &limits) { m_fetcher->setLimits(limits); }

signals:
    void libraryChanged();
    void sourceChanged();
    void articleChanged();
    void refreshChanged();
    void refreshFinished();
    void addChanged();
    void subscribed(qint64 feedId);
    void imported();
    void statusChanged();

private:
    // Runs `work` on the worker thread with its Store, then `done` with the
    // result back on this thread.
    template <typename Result>
    void onWorker(std::function<Result(Store &)> work, std::function<void(const Result &)> done);

    void setStatus(const QString &message, bool error = false);
    void reloadLibrary();
    void reloadCounts();
    void loadArticles();
    ArticleFilter currentFilter() const;
    void showArticle(const Article &article);
    void setFlags(bool read, bool starred);
    void refresh(const QList<Feed> &targets);
    void refreshApplied(const Feed &feed, const Library::Refresh &outcome);
    void searchRound(const std::shared_ptr<Discovery::Search> &search);
    void subscribeFinished(const Library::Subscription &subscription);
    void scheduleAutoRefresh();

    Store m_store;
    bool m_storeOpen = false;
    std::unique_ptr<Fetcher> m_fetcher;
    QThread m_thread;
    LibraryWorker *m_worker = nullptr;
    FilePicker *m_picker = nullptr;
    std::function<bool(const QUrl &)> m_openUrl;
    bool m_closing = false;

    SourceModel m_sources;
    ArticleModel m_articles;
    int m_feedCount = 0;
    int m_sourceIndex = 0;
    int m_articleIndex = -1;
    Article m_article;
    QString m_articleHtml;
    qint64 m_viewLastId = 0;

    bool m_refreshing = false;
    int m_refreshDone = 0;
    int m_refreshTotal = 0;
    int m_refreshFailed = 0;
    int m_refreshAdded = 0;
    QString m_firstFailure;
    QSet<qint64> m_removedDuringRefresh;
    bool m_fetchNewFeeds = false;
    bool m_autoRefreshOn = false;
    QTimer m_autoRefresh;

    bool m_adding = false;
    QString m_addError;

    QString m_status;
    bool m_statusError = false;
};
