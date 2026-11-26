#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "articlemodel.h"
#include "feedparser.h"
#include "fetcher.h"
#include "keystore.h"
#include "library.h"
#include "nip19.h"
#include "relaypool.h"
#include "sanitizer.h"
#include "settings.h"
#include "remotesigner.h"
#include "signer.h"
#include "sourcemodel.h"
#include "store.h"
#include "sync.h"
#include "thumbnails.h"

class FilePicker;
class LibraryWorker;

// The facade QML talks to as `backend`: the sources and articles models, the
// article in the reader, and every action the window offers.
//
// Reads use a Store on this thread; the library is in WAL mode, so they never
// wait on a refresh. Every write goes to a worker thread with its own Store,
// since a write on this thread would wait (up to the 5 s busy timeout) behind
// a refresh saving there. The user's own writes (read, starred, mark the view
// read, unsubscribe) show in the window at once and are reconciled with the
// library when the worker reports back; any still queued when the window
// closes are done then. Refreshing and subscribing fetch on this thread's
// event loop through Fetcher, never in a nested loop, and hand each response
// to the worker, which parses and saves it with the same Library code the
// headless commands use. The models reload once a batch is done.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(SourceModel *sources READ sources CONSTANT)
    Q_PROPERTY(ArticleModel *articles READ articles CONSTANT)
    Q_PROPERTY(int feedCount READ feedCount NOTIFY libraryChanged)

    Q_PROPERTY(int sourceIndex READ sourceIndex NOTIFY sourceChanged)
    Q_PROPERTY(QString sourceKind READ sourceKind NOTIFY sourceChanged)
    Q_PROPERTY(QString sourceTitle READ sourceTitle NOTIFY sourceChanged)
    Q_PROPERTY(qint64 selectedFeedId READ selectedFeedId NOTIFY sourceChanged)
    // The selected saved view's id, empty when the source is not one.
    Q_PROPERTY(QString selectedViewId READ selectedViewId NOTIFY sourceChanged)
    // Whether the selected view's keywords were looked for in no more than
    // its first maxViewScanned articles, which left some unread for them.
    Q_PROPERTY(bool viewScanCapped READ viewScanCapped NOTIFY viewScanCappedChanged)
    // Not CONSTANT: tests lower the limit, and the note re-reads it whenever
    // the list is capped again.
    Q_PROPERTY(int viewScanLimit READ viewScanLimit NOTIFY viewScanCappedChanged)

    // The words the article list is searched for, as typed; empty for none.
    // `searching` is whether they hold a word to search for, so the list
    // shows only the matches, best first.
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchChanged)

    Q_PROPERTY(int articleIndex READ articleIndex NOTIFY articleChanged)
    Q_PROPERTY(bool hasArticle READ hasArticle NOTIFY articleChanged)
    Q_PROPERTY(qint64 articleId READ articleId NOTIFY articleChanged)
    Q_PROPERTY(QString articleTitle READ articleTitle NOTIFY articleChanged)
    Q_PROPERTY(QString articleFeed READ articleFeed NOTIFY articleChanged)
    Q_PROPERTY(QString articleAuthor READ articleAuthor NOTIFY articleChanged)
    Q_PROPERTY(QString articleUrl READ articleUrl NOTIFY articleChanged)
    Q_PROPERTY(QVariant articlePublished READ articlePublished NOTIFY articleChanged)
    // The article's text as rich text. Its images are links unless images
    // are shown, when each becomes its local copy once that is fetched.
    Q_PROPERTY(QString articleHtml READ articleHtml NOTIFY articleHtmlChanged)
    // Whether the article has images, and whether they are shown: always
    // with Load remote images on, else once loadArticleImages() asks.
    Q_PROPERTY(bool articleHasImages READ articleHasImages NOTIFY articleHtmlChanged)
    Q_PROPERTY(bool articleImagesShown READ articleImagesShown NOTIFY articleHtmlChanged)
    Q_PROPERTY(bool articleRead READ articleRead NOTIFY articleChanged)
    Q_PROPERTY(bool articleStarred READ articleStarred NOTIFY articleChanged)
    // A Nostr video's: whether there is one to play, its length in seconds
    // (negative when unknown), and its thumbnail as a local file, empty
    // until it is fetched and cached or when there is none.
    Q_PROPERTY(bool articleIsVideo READ articleIsVideo NOTIFY articleChanged)
    Q_PROPERTY(double articleDuration READ articleDuration NOTIFY articleChanged)
    Q_PROPERTY(QUrl articleThumbnail READ articleThumbnail NOTIFY thumbnailChanged)
    Q_PROPERTY(Settings *settings READ settings CONSTANT)

    Q_PROPERTY(bool refreshing READ refreshing NOTIFY refreshChanged)
    Q_PROPERTY(int refreshDone READ refreshDone NOTIFY refreshChanged)
    Q_PROPERTY(int refreshTotal READ refreshTotal NOTIFY refreshChanged)

    Q_PROPERTY(bool adding READ adding NOTIFY addChanged)
    Q_PROPERTY(QString addError READ addError NOTIFY addChanged)

    // The npub whose Nostr sync events are pulled after each refresh, empty
    // for none, and when a pull last succeeded (null until one has).
    Q_PROPERTY(QString syncNpub READ syncNpub NOTIFY syncChanged)
    Q_PROPERTY(QVariant syncedAt READ syncedAt NOTIFY syncChanged)
    Q_PROPERTY(bool syncing READ syncing NOTIFY syncChanged)
    // Whether the keyring holds the sync account's signing key, which is
    // only looked up, not read, until something is to be signed; whether a
    // keyring call about it is out; and what the last one could not do.
    Q_PROPERTY(bool signingKeyStored READ signingKeyStored NOTIFY signingChanged)
    // Whether signingKeyStored is the keyring's own answer, rather than a
    // guess while no check has answered or after one failed.
    Q_PROPERTY(bool signingKeyChecked READ signingKeyChecked NOTIFY signingChanged)
    Q_PROPERTY(bool signingBusy READ signingBusy NOTIFY signingChanged)
    Q_PROPERTY(QString signingError READ signingError NOTIFY signingChanged)
    // What signs for the account: "key" for the nsec in the keyring, which
    // wins when there is one, "remote" for a paired remote signer (NIP-46),
    // or "none".
    Q_PROPERTY(QString signerKind READ signerKind NOTIFY signingChanged)
    Q_PROPERTY(bool remoteSignerPaired READ remoteSignerPaired NOTIFY signingChanged)
    // The keyring answered that it holds neither a key nor a pairing.
    Q_PROPERTY(bool signerMissing READ signerMissing NOTIFY signingChanged)
    // A connection to a remote signer is being made; for a nostrconnect://
    // link, the link and its QR code's image:// URL.
    Q_PROPERTY(bool remoteConnecting READ remoteConnecting NOTIFY remoteSignerChanged)
    Q_PROPERTY(QString nostrConnectUri READ nostrConnectUri NOTIFY remoteSignerChanged)
    Q_PROPERTY(QString nostrConnectQr READ nostrConnectQr NOTIFY remoteSignerChanged)
    // Why connecting failed, empty when it did not.
    Q_PROPERTY(QString remoteSignerError READ remoteSignerError NOTIFY remoteSignerChanged)
    // A request waits on the remote signer, likely for the user's approval
    // on the phone, and the page it asked the user to approve at, if any.
    Q_PROPERTY(bool remoteSignerWaiting READ remoteSignerWaiting NOTIFY remoteSignerChanged)
    Q_PROPERTY(QString remoteAuthUrl READ remoteAuthUrl NOTIFY remoteSignerChanged)
    // Whether read articles are published too (kind 30405), which makes
    // them public; off until turned on, as in the web app.
    Q_PROPERTY(bool publishReads READ publishReads WRITE setPublishReads NOTIFY syncChanged)
    // Whether "Publish now" was pressed once for this npub; until then
    // changes are kept, not published.
    Q_PROPERTY(bool publishConfirmed READ publishConfirmed NOTIFY syncChanged)
    // Whether a publish is out, when one last reached a relay (null until
    // one has), and why the last one failed, empty when it did not.
    Q_PROPERTY(bool publishing READ publishing NOTIFY publishChanged)
    Q_PROPERTY(QVariant publishedAt READ publishedAt NOTIFY publishChanged)
    Q_PROPERTY(QString publishError READ publishError NOTIFY publishChanged)

    // The keyword filter rules, in order, each a map of id, enabled,
    // target, type, pattern, caseSensitive, and action, named as in the web
    // app; and why some kept in the library could not be used and are left
    // out, empty when none was. They never leave this machine.
    Q_PROPERTY(QVariantList filterRules READ filterRules NOTIFY filtersChanged)
    Q_PROPERTY(QString filterError READ filterError NOTIFY filtersChanged)
    Q_PROPERTY(int maxFilterRules READ maxFilterRules CONSTANT)
    Q_PROPERTY(int maxFilterPatternLength READ maxFilterPatternLength CONSTANT)

    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool statusError READ statusError NOTIFY statusChanged)

    // Whether undo() and redo() have something to undo or redo.
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoChanged)

public:
    // The window shows at most this many articles per view, newest first.
    static constexpr int viewLimit = 2000;
    // A saved view with keywords reads at most this many articles for them
    // each time it lists (ArticleFilter::maxScanned), on this thread, so a
    // view of rare words over a large library never holds the window up
    // for long. Not on the worker, which runs the refreshes and the user's
    // writes one at a time: the scan would hold those up instead. Marking
    // the view read reads those articles again, no more, on the worker,
    // which holds up the writes queued behind it that once; accepted for
    // an action the user takes now and then.
    static constexpr int maxViewScanned = 20000;
    static constexpr int autoRefreshMinutes = 30;
    // The most actions undo() keeps, the oldest going first.
    static constexpr int maxUndo = 100;
    // About how much memory the undo and redo history may hold in all, the
    // feeds unsubscribed and the read changes it keeps to undo with: past
    // it, the oldest of those go first. A feed of more than snapshotBytes,
    // or snapshotItems articles, is unsubscribed with no undo, which also
    // keeps its restore, one transaction, short.
    static constexpr qsizetype undoBytes = 64 * 1024 * 1024;
    static constexpr qsizetype snapshotBytes = 32 * 1024 * 1024;
    static constexpr int snapshotItems = 20000;
    // What the history counts for each article a read change lists. A
    // marking of more than snapshotBytes' worth is done with no undo.
    static constexpr qsizetype readChangeBytes = 60;

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
    QString selectedViewId() const;
    bool viewScanCapped() const { return m_viewScanCapped; }
    int viewScanLimit() const { return m_maxViewScanned; }
    // For the tests, which cannot list maxViewScanned articles quickly.
    void setViewScanLimitForTests(int limit) { m_maxViewScanned = limit; }
    // The saved views as the sources show them, in order.
    const QList<SavedView> &savedViews() const { return m_views; }

    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString &query);
    bool searching() const { return !Store::matchExpression(m_searchQuery).isEmpty(); }

    int articleIndex() const { return m_articleIndex; }
    bool hasArticle() const { return m_article.id != 0; }
    qint64 articleId() const { return m_article.id; }
    QString articleTitle() const { return m_article.title; }
    QString articleFeed() const { return Nip19::shortNpub(FeedParser::cleanText(m_article.feedTitle), m_article.feedUrl); }
    QString articleAuthor() const { return m_article.author; }
    QString articleUrl() const;
    QVariant articlePublished() const;
    QString articleHtml() const { return m_articleHtml; }
    bool articleRead() const { return m_article.read; }
    bool articleStarred() const { return m_article.starred; }
    bool articleIsVideo() const { return !m_article.mediaUrl.isEmpty(); }
    bool articleHasImages() const { return !m_articleImages.isEmpty(); }
    bool articleImagesShown() const;
    Settings *settings() { return &m_settings; }
    double articleDuration() const { return articleIsVideo() ? m_article.duration : -1; }
    QUrl articleThumbnail() const { return m_thumbnail; }

    bool refreshing() const { return m_refreshing; }
    int refreshDone() const { return m_refreshDone; }
    int refreshTotal() const { return m_refreshTotal; }

    bool adding() const { return m_adding; }
    QString addError() const { return m_addError; }

    QString syncNpub() const { return m_syncAccount.npub; }
    QVariant syncedAt() const;
    bool syncing() const { return m_syncing; }
    bool signingKeyStored() const { return m_signingKeyStored; }
    bool signingKeyChecked() const { return m_signingKeyChecked; }
    bool signingBusy() const { return m_signingBusy; }
    QString signingError() const { return m_signingError; }
    QString signerKind() const;
    bool remoteSignerPaired() const { return m_pairingStored; }
    bool signerMissing() const { return keyMissing(); }
    bool remoteConnecting() const { return m_pairingSigner != nullptr; }
    QString nostrConnectUri() const { return m_nostrConnectUri; }
    QString nostrConnectQr() const;
    QString remoteSignerError() const { return m_remoteError; }
    bool remoteSignerWaiting() const { return m_remoteWaiting; }
    QString remoteAuthUrl() const { return m_remoteAuthUrl; }
    bool publishReads() const { return m_syncAccount.publishReads; }
    bool publishConfirmed() const { return m_syncAccount.publishConfirmed; }
    bool publishing() const { return m_publishing; }
    QVariant publishedAt() const;
    QString publishError() const { return m_publishError; }

    QVariantList filterRules() const;
    QString filterError() const { return m_filterError; }
    static int maxFilterRules() { return KeywordFilter::maxRules; }
    static int maxFilterPatternLength() { return KeywordFilter::maxPatternLength; }

    QString status() const { return m_status; }
    bool statusError() const { return m_statusError; }

    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }

    Q_INVOKABLE void selectSource(int row);
    Q_INVOKABLE void moveSource(int step);
    Q_INVOKABLE void selectArticle(int row);
    Q_INVOKABLE void moveArticle(int step);
    Q_INVOKABLE void nextUnread();
    Q_INVOKABLE void toggleRead();
    Q_INVOKABLE void toggleStar();
    Q_INVOKABLE void openOriginal();
    // Plays the open article's video in mpv, saying in the status bar when
    // mpv is missing or does not start.
    Q_INVOKABLE void playVideo();
    // Opens a link from an article, but only an http(s) one.
    Q_INVOKABLE bool openLink(const QString &link);
    // Fetches and shows the open article's images, this once, with Load
    // remote images off.
    Q_INVOKABLE void loadArticleImages();
    Q_INVOKABLE void markViewRead();
    Q_INVOKABLE void addFeed(const QString &address);
    Q_INVOKABLE void clearAddError();
    Q_INVOKABLE bool unsubscribe(qint64 feedId);
    Q_INVOKABLE void refreshAll();
    Q_INVOKABLE void refreshFeed(qint64 feedId);
    // Syncs with the npub or nprofile `text` from now on and pulls at once;
    // false, changing nothing, when it is neither.
    Q_INVOKABLE bool setSyncNpub(const QString &text);
    // Forgets the sync account and, with `removeKey`, then deletes its
    // signing key from the keyring, saying in the status bar if that fails.
    Q_INVOKABLE void stopSync(bool removeKey = false);
    // Asks the keyring again whether it holds the sync account's key.
    Q_INVOKABLE void checkSigningKey();
    // Keeps `text`, an nsec or 64 hex characters, in the keyring as the
    // sync account's signing key. False, keeping nothing and saying why in
    // signingError, when it is not a key or not the account's.
    Q_INVOKABLE bool addSigningKey(const QString &text);
    Q_INVOKABLE void forgetSigningKey();
    // Pairs with the remote signer a bunker:// URI names, and keeps the
    // pairing in the keyring once the signer holds the account's key.
    // False, starting nothing and saying why in remoteSignerError, when the
    // URI is not one Readstr can use.
    Q_INVOKABLE bool connectBunker(const QString &uri);
    // Makes a nostrconnect:// link over the account's sync relays, shown as
    // nostrConnectUri and its QR code, and pairs with the signer app that
    // answers it.
    Q_INVOKABLE bool startNostrConnect();
    Q_INVOKABLE void cancelRemoteConnect();
    Q_INVOKABLE void copyNostrConnectUri();
    // Forgets the pairing, here and in the keyring.
    Q_INVOKABLE void disconnectSigner();
    // Signs `event` as the sync account, reading its key from the keyring
    // the first time and keeping it in memory until the account or its key
    // changes. Nothing is published here. `done` runs exactly once, never
    // inside this call, unless the backend is destroyed first.
    void sign(const NostrEvent &event, Signer::Done done);
    Q_INVOKABLE void setPublishReads(bool publish);
    // Pulls, then publishes at once the subscriptions, the read articles
    // when publishReads, and the saved views when no list of them is known.
    // Confirms the account too, so its changes go out on their own after.
    Q_INVOKABLE void publishNow();
    Q_INVOKABLE void importOpml();
    Q_INVOKABLE void exportOpml();
    // Puts text selected in the reader on the clipboard (and the primary
    // selection), as plain text without the zero-width spaces the
    // sanitizer adds to break long words.
    Q_INVOKABLE void copyText(const QString &text);
    // A Nostr author's profile on njump.me, and their npub on the
    // clipboard; both do nothing for a web feed.
    Q_INVOKABLE void openProfile(qint64 feedId);
    Q_INVOKABLE void copyNpub(qint64 feedId);
    // `text` as a TextEdit showing rich text gives it (paragraph and line
    // separators, frame markers), as plain text without zero-width spaces;
    // ones the feed had itself go too.
    static QString copyableText(const QString &text);
    // What is wrong with a rule's pattern for the match type `type`, for
    // the rules editor to show, or empty when it can be used.
    Q_INVOKABLE QString filterProblem(const QString &type, const QString &pattern) const;
    // Adds `rule` (a map as filterRules has them, its id aside) at the end,
    // for `index` -1, or puts it in place of the rule at `index`. False,
    // changing nothing, when filterProblem() finds fault with it or the
    // list is full. Each change is saved at once, and the lists and counts
    // follow once the library has applied it; none marks an article read
    // that is already there, which markFilterMatchesRead() does when asked.
    Q_INVOKABLE bool saveFilterRule(int index, const QVariantMap &rule);
    Q_INVOKABLE void setFilterRuleEnabled(int index, bool enabled);
    Q_INVOKABLE void moveFilterRule(int index, int step);
    Q_INVOKABLE void removeFilterRule(int index);
    // Marks read the unread articles already in the library that the
    // mark-read rule `ruleId` matches, as the user would.
    Q_INVOKABLE void markFilterMatchesRead(const QString &ruleId);
    // Refreshes now and then every autoRefreshMinutes, give or take 10%,
    // for as long as the window is open.
    Q_INVOKABLE void startAutoRefresh();
    // Undoes the last action that can be undone (toggling read or starred,
    // marking the view or a rule's matches read, unsubscribing, changing
    // the filter rules), or redoes the last one undone, through the same
    // writes and publishing as the action, saying in the status bar what
    // was undone or redone. A new action clears what could be redone.
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    void importFrom(const QUrl &url);
    void exportTo(const QUrl &url);
    // The delay before the next automatic refresh, for `unit` in [0, 1].
    static int autoRefreshDelayMs(double unit);
    int nextAutoRefreshMs() const { return m_autoRefresh.isActive() ? m_autoRefresh.remainingTime() : -1; }

    // How links leave the app; tests replace it.
    void setUrlOpener(std::function<bool(const QUrl &)> opener) { m_openUrl = std::move(opener); }
    // How copyNpub() reaches the clipboard; tests replace it.
    void setClipboardWriter(std::function<void(const QString &)> writer) { m_writeClipboard = std::move(writer); }
    enum class Launch { Started, Missing, Failed };
    using Launcher = std::function<Launch(const QString &program, const QStringList &arguments)>;
    // How a video reaches its player; tests replace it.
    void setLauncher(Launcher launcher) { m_launch = std::move(launcher); }
    // Starts `program` from PATH on its own, with no shell, or says that it
    // is not there.
    static Launch launchDetached(const QString &program, const QStringList &arguments);
    // Where the sync account's signing key is kept. Without a store, as in
    // tests that do not set one, no key can be kept.
    void setKeyStore(KeyStore *keyStore);
    void setThumbnailLimits(const Thumbnails::Limits &limits) { m_thumbnails->setLimits(limits); }
    // An article's images: larger, more of them, and cached apart.
    static Thumbnails::Limits imageLimits();
    // The most pixels of one article's images shown at once: 160 MiB once
    // decoded, which the window holds while the article is open. Past it,
    // the rest stay links.
    static constexpr qint64 maxArticleImagePixels = 40 * 1000 * 1000;
    void setImageLimits(const Thumbnails::Limits &limits) { m_images->setLimits(limits); }
    void setFetchLimits(const Fetcher::Limits &limits) { m_fetcher->setLimits(limits); }
    void setRelayLimits(const RelayPool::Limits &limits) { m_relays->setLimits(limits); }
    // Lowers undoBytes, snapshotBytes, and snapshotItems, so tests reach them.
    void setUndoBudget(qsizetype history, qsizetype snapshot, int items) {
        m_undoBytes = history;
        m_snapshotBytes = snapshot;
        m_snapshotItems = items;
    }
    // How long a change waits for more before it is published: a read, which
    // comes in bursts and resends the whole list, longer, as on the web;
    // but never more than `maxWaitMs` after the first change waiting.
    void setPublishDelays(int changeMs, int readMs, int maxWaitMs = 5 * 60 * 1000) {
        m_publishDelayMs = changeMs;
        m_readPublishDelayMs = readMs;
        m_publishMaxWaitMs = maxWaitMs;
    }
    void setRemoteSignerLimits(const RemoteSigner::Limits &limits) { m_remoteLimits = limits; }
    void setRelays(const QStringList &relays) {
        m_relays->setRelays(relays);
        m_syncRelays->setRelays(relays);
    }

signals:
    void libraryChanged();
    void sourceChanged();
    void viewScanCappedChanged();
    void articleChanged();
    void articleHtmlChanged();
    void searchChanged();
    void thumbnailChanged();
    void refreshChanged();
    void refreshFinished();
    void addChanged();
    void subscribed(qint64 feedId);
    void imported();
    void syncChanged();
    void syncFinished();
    void signingChanged();
    void remoteSignerChanged();
    void publishChanged();
    void filtersChanged();
    void statusChanged();
    void undoChanged();

private:
    // Runs `work` on the worker thread with its Store, then `done` with the
    // result back on this thread.
    template <typename Result>
    void onWorker(std::function<Result(Store &)> work, std::function<void(const Result &)> done);

    struct WriteResult {
        int changed = 0;
        QString error;
        // Not run, as an earlier write of the same undo command failed.
        bool skipped = false;
        // The library's feeds or categories changed, so the sources are read
        // again.
        bool reload = false;
    };
    // What the worker and this thread share about an undo command.
    struct UndoState {
        // Set once the action, or an undo or redo of it, failed or left
        // nothing to undo: the writes of it queued after that skip, and the
        // command is forgotten without a word.
        std::atomic<bool> failed = false;
        // About how much memory what it keeps to undo with takes.
        std::atomic<qsizetype> bytes = 0;
        // Set, here, by an undo that failed in a way a later try may not,
        // so the command goes back to be undone again.
        bool retry = false;
    };
    // A write the user asked for; `work` returns how many rows it changed,
    // or -1 with the reason in the Store's lastError(). `failure` describes
    // a failed one, given the reason, when the window is gone to show it.
    struct UserWrite {
        enum State { Queued, Saved, Failed };
        std::function<int(Store &)> work;
        QString failure;
        // The undo command it is a step of, if any.
        std::shared_ptr<UndoState> undo;
        // Set on the worker before `state`, read once it has stopped.
        QString error;
        std::atomic<int> state = Queued;
    };
    // Runs `write` on `store`, on the worker or, at closing, here: skipped
    // once write.undo->failed is set, which it sets when `work` fails.
    static WriteResult runWrite(UserWrite &write, Store &store);
    // With `undo`, skips `work` once undo->failed is set, and sets it when
    // `work` fails.
    void writeOnWorker(std::function<int(Store &)> work, const QString &failure,
                       std::function<void(const WriteResult &)> done,
                       const std::shared_ptr<UndoState> &undo = {});
    // Flags shown before the worker has saved them.
    struct PendingFlags {
        bool read = false;
        bool starred = false;
        int writes = 0;
        // `read` is an undo's guess, made before the worker read what the
        // article held, so M waits for the flag as saved instead.
        bool readGuessed = false;
        // M was pressed while `read` was a guess.
        bool toggleRead = false;
    };
    // Tells an action, or its undo or redo, whether its write was saved.
    using Finished = std::function<void(bool ok)>;
    // An action the user can undo: what it did, as the status bar says it
    // after "Undid:" or "Redid:", and how to undo it (`undo` true) or do it
    // again. That runs through the worker like the action, says in the
    // status bar why it failed, if it did, and then calls the Finished.
    struct UndoCommand {
        std::function<QString()> label;
        std::function<void(bool undo, const Finished &finished)> apply;
        std::shared_ptr<UndoState> state = std::make_shared<UndoState>();
        // Whether its undo can fail in a way a later try may not, as a
        // restore can, which holds the history still until it is done.
        bool mayRetry = false;
    };
    // Keeps the command of an action just asked for, which clears what
    // could be redone.
    void pushUndo(const std::shared_ptr<UndoCommand> &command);
    // Forgets a command whose action, undo, or redo failed or changed
    // nothing, as there is nothing left that it could reverse.
    void dropUndo(const std::shared_ptr<UndoCommand> &command);
    // A Finished that drops `command` when the action's write failed.
    Finished dropUndoOnFailure(const std::shared_ptr<UndoCommand> &command);
    // Keeps the command of marking articles read, which an undo reverses
    // whatever came since: `changes` the articles the worker marks, and
    // `count` how many, once it is done.
    std::shared_ptr<UndoCommand> pushReadsUndo(const std::shared_ptr<const QList<ReadChange>> &changes,
                                               const std::shared_ptr<const qsizetype> &count);
    // Once the marking is done, forgets its command if it marked nothing or
    // more than maxReadChanges(), which the store says in `tooLarge`, or
    // else counts it in the history's bytes. Returns whether it was too
    // large to undo.
    bool keepReadsUndo(const std::shared_ptr<UndoCommand> &command, qsizetype count, bool tooLarge);
    // Forgets the oldest commands that keep something to undo with, undo's
    // before redo's, until the history is within m_undoBytes.
    void trimUndo();
    void undoOrRedo(bool undo);
    static QString undoFailure(bool undo);

    void applyPending(Article &article) const;
    // `writeRead`, when set, writes the read flag in place of setRead(), as
    // an undo does to put back the read time too. `finished` is called once
    // the write is done, or at once when there is nothing to write.
    // A write that is the read time's own, as an undo's is, is made even
    // when the flag is as asked already. `writeStar`, when set, writes the
    // starred flag in place of setStarred().
    void saveFlags(const Article &before, bool read, bool starred, const QString &failure,
                   const std::function<bool(Store &)> &writeRead = {}, const Finished &finished = {},
                   const std::shared_ptr<UndoState> &undo = {}, const std::function<bool(Store &)> &writeStar = {});
    void showFlags(qint64 id, bool read, bool starred);
    // Article `id` with the flags the window shows for it, or, once it is
    // gone, one with only its id and the flags opposite to `read` and
    // `starred`, so that the write giving it those fails as for any
    // missing article.
    Article flagsOf(qint64 id, bool read, bool starred);
    // Gives article `id` the read flag and the read time `change` recorded
    // from before it, with `undo`, or from after it. `change` is filled in
    // on the worker, so it is read here only once `captured` is set, and
    // until then `read` is shown, a guess with `undo`.
    void restoreRead(qint64 id, bool read, const std::shared_ptr<const ReadChange> &change, bool captured,
                     bool undo, const std::shared_ptr<UndoState> &state, const Finished &finished);
    void restoreStar(qint64 id, bool starred, bool undo, const std::shared_ptr<UndoState> &state,
                     const Finished &finished);
    // Gives every article of `changes` its read state from before them, or
    // from after them, marking the read status to publish as marking read
    // does.
    void restoreReads(const std::shared_ptr<const QList<ReadChange>> &changes, bool undo,
                      const std::shared_ptr<UndoState> &state, const Finished &finished);
    // Deletes the feed, keeping what it deletes in `snapshot` for an undo,
    // within the snapshot budget, and puts it back from there.
    void removeSubscription(qint64 feedId, const QString &title, const std::shared_ptr<FeedSnapshot> &snapshot,
                            const std::shared_ptr<UndoState> &state, const Finished &finished);
    void restoreSubscription(qint64 feedId, const std::shared_ptr<const FeedSnapshot> &snapshot,
                             const std::shared_ptr<UndoState> &state, const Finished &finished);

    void setStatus(const QString &message, bool error = false);
    void loadFilterRules();
    // Saves m_filterRules, then shows the library as they leave it.
    void saveFilterRules(const Finished &finished = {}, const std::shared_ptr<UndoState> &undo = {});
    // Saves m_filterRules, changed by the user from `before`, as an action
    // undo() can undo.
    void changeFilterRules(const QList<KeywordFilter::Rule> &before, std::function<QString()> label);
    void reloadLibrary();
    void reloadCounts();
    // Reads the saved views again; true when they changed. Editing views
    // (readstr-gp3.6.2) writes them through the worker and then reloads
    // the library, which shows them and moves the selection off a view
    // that is gone, as a sync pull does.
    bool loadViews();
    // The saved view `id`, or null when there is none.
    const SavedView *savedView(const QString &id) const;
    void loadArticles();
    ArticleFilter currentFilter() const;
    void showArticle(const Article &article);
    // Shows the open video's thumbnail, from the cache or once it is
    // fetched, when it is not the one already shown.
    void updateThumbnail();
    void setThumbnail(const QUrl &file);
    // The article as rich text, with the images fetched so far, and the
    // images it has.
    void renderArticle();
    // Fetches the open article's images when they are to be shown and are
    // not already asked for, and drops them when they are not. True when it
    // laid the article out again, which it says with articleHtmlChanged
    // unless `announce` is false, for a caller that announces the article
    // itself right after.
    bool updateImages(bool announce = true);
    void clearLocalImages();
    // A local file path for a file: URL, or empty for anything else
    // (another host's file: URL included).
    static QString localPath(const QUrl &url);
    void hideRemoving(QList<Feed> &feeds) const;
    // The npub of the Nostr feed `feedId` in the sources, or empty.
    QString npubOf(qint64 feedId) const;
    void refresh(const QList<Feed> &targets);
    void refreshApplied(const Feed &feed, const Library::Refresh &outcome);
    // Refreshes the feeds never fetched yet; false when there are none.
    bool refreshUnfetched();
    void searchRound(const std::shared_ptr<Discovery::Search> &search);
    void subscribeFinished(const Library::Subscription &subscription);
    void scheduleAutoRefresh();
    void reloadSyncAccount();
    // Publishing: a change marks its kind and (re)starts the debounce; when
    // it ends, the events are pulled first, then the kinds marked are built
    // from the library, signed, sent, and recorded one by one. A kind that
    // fails stays marked for the next change or pull; after a signing
    // failure only a change, publishNow(), or a new key retries. The kinds
    // marked are kept in the library too, so they go out after a restart,
    // and nothing goes out on its own until publishNow() confirmed the
    // account.
    struct PublishRun;
    void schedulePublish(int kind);
    // Marks `kind` to publish, here and in the library.
    void markPublishPending(int kind);
    // Sets or clears the bits of `mask` in the library's pending kinds.
    void savePublishPending(const QString &pubkey, int mask, bool pending);
    // The account a read or a subscription changed now is to be published
    // for, if any, and on the worker, in the change's own write, its kind
    // marked for it, so a change saved as the window closes still goes out
    // after a restart.
    QString readsPublishedFor() const;
    QString subscriptionsPublishedFor() const;
    static void markPending(Store &store, const QString &pubkey, int kind);
    // Starts the debounce for what is marked, when nothing is under way.
    void resumePublish();
    // The keyring said it holds neither a key nor a pairing for the account.
    bool keyMissing() const {
        return m_signingKeyChecked && !m_signingKeyStored && m_pairingChecked && !m_pairingStored;
    }
    void startPublish(bool asked = false);
    void continuePublish(const Sync::Outcome &outcome);
    void publishNext(const std::shared_ptr<PublishRun> &run);
    void publishDone(const std::shared_ptr<PublishRun> &run);
    void resetPublish();
    // Pulls the sync events, when there is an account and no pull is out.
    void sync();
    // `pubkey` is the account the pull was for.
    void syncApplied(const Sync::Outcome &outcome, const QString &pubkey);
    // Sets what the properties say; whether the keyring is busy follows
    // from the jobs still queued. Without `checked`, whether the keyring
    // answered stays as it was.
    void setSigning(bool stored, const QString &error, bool checked);
    void setSigning(bool stored, const QString &error) { setSigning(stored, error, m_signingKeyChecked); }
    QString keyStoreError(const KeyStore::Result &result) const;
    // Drops the signer and the signing state of an account no longer synced.
    void resetSigning();
    // What the keyring said of the pairing; like setSigning(), a pairing
    // newly known to be there lets what waited for a signer go out.
    void setPairing(bool stored, bool checked);
    // Lets go of the active signer at once, deleting it from the event loop,
    // as a remote one may be inside one of its own callbacks.
    void releaseSigner();
    // A key in the keyring wins over a remote signer, and a new pairing over
    // an old one: the remote signer is let go once nothing waits on it, so
    // the next sign reads the keyring again; signs that come before then
    // wait for that read.
    void preferKey();
    // Removes the pairing written as `write`, which a "Stop connecting"
    // came too late to stop, unless a newer one is to be written by then.
    void unstorePairing(const QString &npub, quint64 write);
    RemoteSigner *remoteSigner() const;
    void adoptRemoteSigner(std::unique_ptr<RemoteSigner> signer);
    // Starts pairing with `pairing`'s signer, in place of any pairing under
    // way, and keeps it once the signer holds the account's key.
    void beginPairing(RemoteSigner::Pairing pairing);
    void pairingDone(const QString &error);
    void endPairing();
    // Whether there is an account to pair for and a keyring to keep the
    // pairing in; if not, says why in remoteSignerError.
    bool canPair();
    void setRemoteError(const QString &error);
    // The approval page of a request still waiting, the pairing's first.
    QString currentAuthUrl() const;
    void updateRemoteWaiting();
    // Drops the signer and any read of the key still out, so neither signs
    // again, failing with `error` every sign not answered yet.
    void dropSigner(const QString &error);
    // Answers, from the event loop, every sign not answered yet.
    void failPendingSigns(const QString &error, bool queuedToo = true);
    void handToSigner(const NostrEvent &event, Signer::Done done);
    // Reads the key, when a sign waits for it, and hands it what waits.
    void loadSigner();
    // Runs `job` once every keyring job queued before it is done. The job
    // calls the function it is given once its own keyring call answers.
    using KeyJob = std::function<void(std::function<void()> finished)>;
    void queueKeyJob(KeyJob job);
    void runKeyJobs();

    Store m_store;
    bool m_storeOpen = false;
    std::unique_ptr<Fetcher> m_fetcher;
    std::unique_ptr<RelayPool> m_relays;
    // Its own pool, as the sync events are allowed to be far larger than
    // an author's.
    std::unique_ptr<RelayPool> m_syncRelays;
    std::unique_ptr<Thumbnails> m_thumbnails;
    std::unique_ptr<Thumbnails> m_images;
    Settings m_settings;
    QThread m_thread;
    LibraryWorker *m_worker = nullptr;
    FilePicker *m_picker = nullptr;
    std::function<bool(const QUrl &)> m_openUrl;
    std::function<void(const QString &)> m_writeClipboard;
    Launcher m_launch;
    bool m_closing = false;
    QList<std::shared_ptr<UserWrite>> m_userWrites;
    QHash<qint64, PendingFlags> m_pendingFlags;

    SourceModel m_sources;
    ArticleModel m_articles;
    QList<SavedView> m_views;
    int m_feedCount = 0;
    int m_sourceIndex = 0;
    QString m_searchQuery;
    int m_articleIndex = -1;
    Article m_article;
    QString m_articleHtml;
    // The remote thumbnail asked for, what shows, and a count that tells a
    // fetch for an article since left from the current one.
    QString m_thumbnailSource;
    QUrl m_thumbnail;
    quint64 m_thumbnailRequest = 0;
    // The open article's images, by source; their local copies so far; the
    // article loadArticleImages() asked for; the sources asked for, of
    // which article; a count that tells a fetch for another from the
    // current one; and the timer that shows images in batches as they come.
    QStringList m_articleImages;
    QHash<QString, Sanitizer::LocalImage> m_localImages;
    // Their pixels, counted against maxArticleImagePixels.
    qint64 m_imagePixels = 0;
    qint64 m_imagesFor = 0;
    QStringList m_imagesAsked;
    qint64 m_imagesAskedFor = 0;
    quint64 m_imagesRequest = 0;
    QTimer m_imagesArrived;
    qint64 m_viewLastId = 0;
    bool m_viewScanCapped = false;
    int m_maxViewScanned = maxViewScanned;
    // The articles the list read for its view's words when maxViewScanned
    // stopped it, which marking the view read stays within.
    QList<qint64> m_viewScanned;

    bool m_refreshing = false;
    int m_refreshDone = 0;
    int m_refreshTotal = 0;
    int m_refreshFailed = 0;
    int m_refreshAdded = 0;
    // New articles of this refresh a keyword filter rule marked read.
    int m_refreshMarked = 0;
    QString m_firstFailure;
    // Ids are never reused, so a failed save for one of these is always a
    // refresh racing its unsubscribe, which is no news to anyone. A failed
    // delete takes its id out again.
    QSet<qint64> m_unsubscribed;
    // Feeds whose delete the worker has not done yet, hidden meanwhile, with
    // how many deletes are waiting: an unsubscribe, its undo, and its redo
    // can all be queued, and the feed stays hidden until the last is done.
    QHash<qint64, int> m_removing;
    bool m_fetchNewFeeds = false;
    // A refresh of every feed pulls the sync events once it is done, so
    // their read GUIDs find the articles it brought.
    bool m_syncAfterRefresh = false;
    // What that refresh said, kept when the sync after it changes nothing.
    std::optional<std::pair<QString, bool>> m_refreshStatus;
    SyncAccount m_syncAccount;
    bool m_syncing = false;
    // A pull asked for while one was out, which the relays may already have
    // answered without what changed since, so one more runs after it.
    bool m_syncAgain = false;
    // What the pulls a follow-up replaced changed, said with its own.
    int m_syncAddedBefore = 0;
    int m_syncMarkedBefore = 0;
    // Some pull added feeds that have not been fetched yet. Kept apart from
    // the counts above, which a switch of account resets, so the feeds an
    // earlier account's pull added are still fetched.
    bool m_syncFeedsAdded = false;
    KeyStore *m_keyStore = nullptr;
    // The sync account's, once something has been signed.
    std::unique_ptr<Signer> m_signer;
    bool m_signerRemote = false;
    // The remote signer gave way, and goes once it has answered what it
    // was given.
    bool m_signerStale = false;
    // Whether the keyring holds a pairing with a remote signer, and whether
    // it has answered that.
    bool m_pairingStored = false;
    bool m_pairingChecked = false;
    // The signer being paired with, until it holds the account's key and the
    // pairing is stored, or that fails.
    std::unique_ptr<RemoteSigner> m_pairingSigner;
    // Bumped whenever a pairing starts or ends, which leaves the answers
    // still out for an earlier one unheard.
    quint64 m_pairingEpoch = 0;
    // Counts the pairings handed to the keyring to write.
    quint64 m_pairingWrites = 0;
    QString m_nostrConnectUri;
    QString m_remoteError;
    bool m_remoteWaiting = false;
    QString m_remoteAuthUrl;
    std::pair<QString, bool> m_statusBeforeApproval;
    RemoteSigner::Limits m_remoteLimits;
    bool m_signingKeyStored = false;
    bool m_signingKeyChecked = false;
    bool m_signingBusy = false;
    QString m_signingError;
    // Keyring calls go one at a time, in the order they were asked for, so
    // each answer is about the keyring as the ones before left it: a check
    // after a store sees the stored key, and a read after a removal finds
    // nothing.
    QList<KeyJob> m_keyJobs;
    bool m_keyJobRunning = false;
    bool m_startingKeyJobs = false;
    // Bumped when the store changes, as a job out to the old one may never
    // finish and must not hold up the new one's.
    quint64 m_keyQueue = 0;
    // Bumped when the account changes, which leaves the answers still out
    // for the old one unheard.
    quint64 m_signingEpoch = 0;
    // Bumped whenever the signer is dropped, which leaves a read of the key
    // still out unheard, so it cannot bring the dropped key back.
    quint64 m_signerLoad = 0;
    bool m_loadingSigner = false;
    // The read under way is past the key, reading the pairing.
    bool m_loadingPairing = false;
    // The read the remote signer was disconnected during, while it read
    // the key: it does not go on to the pairing.
    quint64 m_pairingDisconnectedLoad = 0;
    // Signs waiting for the key to be read.
    QList<std::pair<NostrEvent, Signer::Done>> m_pendingSigns;
    // Signs handed to the signer and not answered yet, kept here so that
    // dropping the signer, which drops what it would have answered, still
    // answers them.
    QMap<quint64, Signer::Done> m_signing;
    quint64 m_signId = 0;
    bool m_autoRefreshOn = false;
    QTimer m_autoRefresh;

    QSet<int> m_publishKinds;
    QTimer m_publishTimer;
    int m_publishDelayMs = 2 * 1000;
    int m_readPublishDelayMs = 30 * 1000;
    int m_publishMaxWaitMs = 5 * 60 * 1000;
    // Since the first change waiting to be published.
    QElapsedTimer m_publishWaiting;
    bool m_publishing = false;
    // The next pull to finish is the one a publish waits on.
    bool m_publishPull = false;
    bool m_publishAgain = false;
    // The last publish could not sign, which asking again at every pull
    // would not mend and might prompt for each time.
    bool m_publishHeld = false;
    // Bumped when the account changes, which leaves a run for the old one
    // unheard.
    quint64 m_publishEpoch = 0;
    QDateTime m_publishedAt;
    QString m_publishError;

    bool m_adding = false;
    QString m_addError;

    QList<KeywordFilter::Rule> m_filterRules;
    QString m_filterError;
    // Counts the saves of the rules, read on the worker to skip or stop one
    // a newer save makes pointless.
    std::shared_ptr<std::atomic<quint64>> m_filterGeneration = std::make_shared<std::atomic<quint64>>(0);
    // Set as the window closes, so a pass over the library stops at its
    // next batch instead of holding the window open; the next start
    // finishes it.
    std::shared_ptr<std::atomic<bool>> m_quitting = std::make_shared<std::atomic<bool>>(false);

    QString m_status;
    bool m_statusError = false;

    // The actions undo() undoes, newest last, at most maxUndo, and those it
    // undid, which redo() does again, the last undone last.
    QList<std::shared_ptr<UndoCommand>> m_undo;
    QList<std::shared_ptr<UndoCommand>> m_redo;
    qsizetype m_undoBytes = undoBytes;
    qsizetype m_snapshotBytes = snapshotBytes;
    int m_snapshotItems = snapshotItems;
    // The finishes of filter rule saves skipped for a later one, which say
    // whether that one saved them, as it saves their changes too.
    QList<Finished> m_filterSavesWaiting;
    // The undo of an unsubscribe the worker has not done yet, if any.
    std::shared_ptr<UndoCommand> m_restoring;
    // How many actions pushUndo() has kept, so a restore that failed for
    // now goes back under those asked for since.
    quint64 m_pushes = 0;
    qsizetype maxReadChanges() const { return m_snapshotBytes / readChangeBytes; }
};
