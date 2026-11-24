#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDeadlineTimer>
#include <QTextCursor>
#include <QGuiApplication>
#include <QClipboard>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTextDocument>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QImageWriter>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <atomic>
#include <cstdio>
#include <climits>
#include <optional>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>

#include "articlemodel.h"
#include "backend.h"
#include "cli.h"
#include "discovery.h"
#include "fakehttpserver.h"
#include "fakerelay.h"
#include "feedparser.h"
#include "fetcher.h"
#include "keystore.h"
#include "filepicker.h"
#include "library.h"
#include "localnetwork.h"
#include "markdown.h"
#include "nip19.h"
#include "nip44.h"
#include "nostrevent.h"
#include "nostrsource.h"
#include "opml.h"
#include "paths.h"
#include "portalfilepicker.h"
#include "qrcode.h"
#include "relaypool.h"
#include "remotesigner.h"
#include "sanitizer.h"
#include "signer.h"
#include "sourcemodel.h"
#include "store.h"
#include "sync.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"
#include "wipe.h"

// Answers the OPML dialogs the way a user would, without a portal.
class FakePicker : public FilePicker {
public:
    QUrl openUrl, saveUrl;
    QString failure;
    QString suggestedName;

    void openOpml() override {
        if (!failure.isEmpty())
            emit failed(failure);
        else if (!openUrl.isEmpty())
            emit openSelected(openUrl);
    }
    void saveOpml(const QString &name) override {
        suggestedName = name;
        if (!failure.isEmpty())
            emit failed(failure);
        else if (!saveUrl.isEmpty())
            emit saveSelected(saveUrl);
    }
};

// The portal picker with its D-Bus side replaced, so tests can play the
// portal's replies and Response signals in any order.
class FakePortalPicker : public PortalFilePicker {
public:
    using PortalFilePicker::replied;
    using PortalFilePicker::responded;

    bool bus = true;
    QStringList listening, closed;
    QList<QDBusMessage> calls;
    QList<int> generations;

    QString requestPath(int call) const {
        return "/org/freedesktop/portal/desktop/request/1_42/"
               + calls.at(call).arguments().at(2).toMap().value("handle_token").toString();
    }

protected:
    QString senderName() const override { return bus ? QStringLiteral(":1.42") : QString(); }
    bool listen(const QString &path) override {
        listening << path;
        return true;
    }
    void stopListening(const QString &path) override { listening.removeOne(path); }
    void send(const QDBusMessage &call, int generation) override {
        calls << call;
        generations << generation;
    }
    void closeRequest(const QString &path) override { closed << path; }
};

// Drops the queued calls to the object it filters, as a window that closes
// before its worker's results arrive never runs them.
class QueuedCallDropper : public QObject {
protected:
    bool eventFilter(QObject *, QEvent *event) override { return event->type() == QEvent::MetaCall; }
};

// A keyring in memory, so no test ever reaches the real one. Each answer
// comes from the event loop, as the real one's does, and `error` makes every
// call fail that way. With `hold`, calls wait, as for a prompt the user has
// not answered, until the test releases them, and with `delays`, each
// answer comes after 0 to 2 ms drawn from it, so calls overlap.
class FakeKeyStore : public KeyStore {
public:
    QHash<QString, QByteArray> keys;
    QHash<QString, QByteArray> pairings;
    Error error = Error::None;
    QStringList calls;
    QStringList pairingCalls;
    bool hold = false;
    QRandomGenerator *delays = nullptr;

    qsizetype held() const { return m_held.size(); }
    // Answers every held call, oldest first, there and then.
    void release() {
        for (const auto &run : std::exchange(m_held, {}))
            run();
    }
    // Answers only the newest held call, as a keyring does whose earlier
    // call is still waiting on a prompt.
    void releaseNewest() {
        if (m_held.isEmpty())
            qFatal("no held call to release");
        m_held.takeLast()();
    }

    void contains(const QString &npub, Done done) override {
        calls << "contains " + npub;
        answer(done, [this, npub](Result &result) { result.found = keys.contains(npub); });
    }
    void lookup(const QString &npub, Done done) override {
        calls << "lookup " + npub;
        answer(done, [this, npub](Result &result) {
            result.found = keys.contains(npub);
            result.secret = keys.value(npub);
        });
    }
    void store(const QString &npub, const QByteArray &secret, Done done) override {
        calls << "store " + npub;
        answer(done, [this, npub, secret](Result &) { keys.insert(npub, secret); });
    }
    void remove(const QString &npub, Done done) override {
        calls << "remove " + npub;
        answer(done, [this, npub](Result &result) { result.found = keys.remove(npub) > 0; });
    }
    // Pairings with a remote signer, in a list of calls of their own, so
    // the tests of the key see only the key's.
    void containsPairing(const QString &npub, Done done) override {
        pairingCalls << "contains " + npub;
        answer(done, [this, npub](Result &result) { result.found = pairings.contains(npub); });
    }
    void lookupPairing(const QString &npub, Done done) override {
        pairingCalls << "lookup " + npub;
        answer(done, [this, npub](Result &result) {
            result.found = pairings.contains(npub);
            result.secret = pairings.value(npub);
        });
    }
    void storePairing(const QString &npub, const QByteArray &pairing, Done done) override {
        pairingCalls << "store " + npub;
        answer(done, [this, npub, pairing](Result &) { pairings.insert(npub, pairing); });
    }
    void removePairing(const QString &npub, Done done) override {
        pairingCalls << "remove " + npub;
        answer(done, [this, npub](Result &result) { result.found = pairings.remove(npub) > 0; });
    }

private:
    void answer(const Done &done, const std::function<void(Result &)> &work) {
        const auto run = [this, done, work] {
            Result result;
            result.error = error;
            if (error == Error::None)
                work(result);
            done(result);
        };
        if (hold)
            m_held << run;
        else
            QTimer::singleShot(delays ? delays->bounded(3) : 0, this, run);
    }

    QList<std::function<void()>> m_held;
};

// The Secret Service store with its D-Bus side replaced: every call waits
// until the test answers it, in order, as the service would.
class FakeSecretService : public SecretServiceKeyStore {
public:
    using SecretServiceKeyStore::completed;
    using SecretServiceKeyStore::serviceLost;

    bool bus = true;
    QList<QDBusMessage> calls;
    QStringList listening;

    // Answers the oldest call not answered yet.
    void reply(const QVariantList &arguments) { answer(next().createReply(arguments)); }
    void fail(const QString &name, const QString &message = "failed") {
        answer(next().createErrorReply(name, message));
    }
    bool waiting() const { return m_answered < calls.size(); }
    // "Interface.Method path" for each call, to compare in one go.
    QStringList methods() const {
        QStringList methods;
        for (const QDBusMessage &call : calls)
            methods << call.interface().section('.', -1) + "." + call.member() + " " + call.path();
        return methods;
    }

protected:
    bool connected() const override { return bus; }
    void call(const QDBusMessage &message, std::function<void(const QDBusMessage &)> done) override {
        calls << message;
        m_done << std::move(done);
    }
    bool listen(const QString &path) override {
        listening << path;
        return true;
    }
    void stopListening(const QString &path) override { listening.removeOne(path); }

private:
    const QDBusMessage &next() {
        if (!waiting())
            qFatal("no call to answer");
        return calls.at(m_answered);
    }
    void answer(const QDBusMessage &reply) {
        const auto done = m_done.at(m_answered++);
        done(reply);
    }

    QList<std::function<void(const QDBusMessage &)>> m_done;
    qsizetype m_answered = 0;
};


// A remote signer (NIP-46) for tests, such as Amber, listening on a
// FakeRelay: it answers each kind 24133 request tagged to it as a signer app
// would, signing with the user's key, unless told to misbehave. Its keys
// are made from fixed bytes in memory.
class FakeBunker {
public:
    enum class Sign { Honest, BadSignature, OtherKey, OtherEvent, Refuse, Twice };
    struct Held {
        QString client;
        QString id;
        QString method;
        QJsonArray params;
    };

    explicit FakeBunker(FakeRelay &relay, char signerByte = 5, char userByte = 1)
        : m_relay(relay), m_signerSecret(32, signerByte), m_signer(LocalSigner::create(m_signerSecret)),
          m_user(LocalSigner::create(QByteArray(32, userByte))) {
        relay.onEvent = [this](const QJsonObject &event) { received(event); };
    }
    ~FakeBunker() { m_relay.onEvent = {}; }

    // The methods asked for, in order, and the params of each.
    QStringList methods;
    QList<QJsonArray> params;
    // What connect must carry, when set.
    QString secret;
    // Requests are kept, unanswered, until answerHeld().
    bool hold = false;
    QList<Held> held;
    // Each request is first answered with auth_url and this URL.
    QString authUrl;
    Sign sign = Sign::Honest;

    QString pubkey() const { return m_signer->pubkey(); }
    QString userPubkey() const { return m_user->pubkey(); }
    QString uri(const QString &relay, const QString &connectSecret = {}) const {
        QString link = "bunker://" + pubkey() + "?relay=" + QUrl::toPercentEncoding(relay);
        if (!connectSecret.isEmpty())
            link += "&secret=" + connectSecret;
        return link;
    }

    void answerHeld() {
        for (const Held &request : std::exchange(held, {}))
            answer(request);
    }

    // Sends an answer to `client`, encrypted and signed by `from` (the
    // bunker's own key unless given), with `content` as the plaintext.
    void respondRaw(const QString &client, const QString &content, const QByteArray &fromSecret = {},
                    const std::optional<QString> &payloadOverride = std::nullopt) {
        const QByteArray secretKey = fromSecret.isEmpty() ? m_signerSecret : fromSecret;
        const auto from = LocalSigner::create(secretKey);
        const QByteArray key = Nip44::conversationKey(secretKey, QByteArray::fromHex(client.toLatin1()));
        NostrEvent event;
        event.kind = 24133;
        event.createdAt = QDateTime::currentSecsSinceEpoch();
        event.tags = {{"p", client}};
        event.content = payloadOverride ? *payloadOverride : *Nip44::encrypt(content.toUtf8(), key);
        m_relay.push(from->signNow(event).first.toJson());
    }
    void respond(const QString &client, const QString &id, const QString &result, const QString &error = {},
                 const QByteArray &fromSecret = {}) {
        QJsonObject answer{{"id", id}, {"result", result}};
        if (!error.isEmpty())
            answer.insert("error", error);
        respondRaw(client, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)), fromSecret);
    }

    // Answers a nostrconnect:// link as a signer app that scanned it would,
    // with the link's secret unless given another.
    void connectTo(const QString &link, const std::optional<QString> &otherSecret = std::nullopt) {
        const QString client = link.section("://", 1).section('?', 0, 0);
        const QString linkSecret = QUrlQuery(link.section('?', 1)).queryItemValue("secret", QUrl::FullyDecoded);
        respond(client, "connect-1", otherSecret ? *otherSecret : linkSecret);
    }

private:
    void received(const QJsonObject &json) {
        const auto event = NostrEvent::fromJson(json);
        if (!event || event->kind != 24133 || event->tag("p") != pubkey() || !event->verify())
            return;
        const QByteArray key = Nip44::conversationKey(m_signerSecret, QByteArray::fromHex(event->pubkey.toLatin1()));
        const auto plaintext = Nip44::decrypt(event->content, key);
        if (!plaintext)
            return;
        const QJsonObject request = QJsonDocument::fromJson(*plaintext).object();
        const Held held{event->pubkey, request.value("id").toString(), request.value("method").toString(),
                        request.value("params").toArray()};
        methods << held.method;
        params << held.params;
        if (hold)
            this->held << held;
        else
            answer(held);
    }

    void answer(const Held &request) {
        if (!authUrl.isEmpty()) {
            respond(request.client, request.id, "auth_url", authUrl);
            QTimer::singleShot(100, &m_context, [this, request] { answerNow(request); });
            return;
        }
        answerNow(request);
    }

    void answerNow(const Held &request) {
        if (request.method == "connect") {
            if (!secret.isEmpty() && request.params.at(1).toString() != secret)
                return respond(request.client, request.id, {}, "invalid secret");
            return respond(request.client, request.id, "ack");
        }
        if (request.method == "get_public_key")
            return respond(request.client, request.id, m_user->pubkey());
        if (request.method == "ping")
            return respond(request.client, request.id, "pong");
        if (request.method != "sign_event")
            return respond(request.client, request.id, {}, "unknown method");
        if (sign == Sign::Refuse)
            return respond(request.client, request.id, {}, "user rejected");
        const QJsonObject draftJson = QJsonDocument::fromJson(request.params.at(0).toString().toUtf8()).object();
        NostrEvent draft;
        draft.kind = draftJson.value("kind").toInt();
        draft.createdAt = draftJson.value("created_at").toInteger();
        for (const QJsonValue &tag : draftJson.value("tags").toArray())
            draft.tags << tag.toVariant().toStringList();
        draft.content = draftJson.value("content").toString();
        if (sign == Sign::OtherEvent)
            draft.content += "!";
        const auto other = LocalSigner::create(QByteArray(32, '\2'));
        NostrEvent signedEvent = (sign == Sign::OtherKey ? other.get() : m_user.get())->signNow(draft).first;
        if (sign == Sign::BadSignature)
            signedEvent.sig = (signedEvent.sig.startsWith('0') ? "1" : "0") + signedEvent.sig.mid(1);
        const QString result = QString::fromUtf8(QJsonDocument(signedEvent.toJson()).toJson(QJsonDocument::Compact));
        respond(request.client, request.id, result);
        if (sign == Sign::Twice)
            respond(request.client, request.id, result);
    }

    FakeRelay &m_relay;
    // Goes with the bunker, and the answers it was to send later with it.
    QObject m_context;
    QByteArray m_signerSecret;
    std::unique_ptr<LocalSigner> m_signer;
    std::unique_ptr<LocalSigner> m_user;
};

// The real window, loaded as main() loads it but with a fake picker and
// keyring and a Translator reading a language file the test writes, so
// the window's own Shortcuts and menus can be checked against what it
// shows.
struct LoadedWindow {
    FakePicker picker;
    FakeKeyStore keys;
    std::unique_ptr<Translator> translator;
    std::unique_ptr<Backend> backend;
    LocalNetworkFactory network;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;

    explicit LoadedWindow(const QString &languageFile) {
        translator = std::make_unique<Translator>(languageFile);
        QCoreApplication::installTranslator(translator.get());
        backend = std::make_unique<Backend>(&picker);
        backend->setKeyStore(&keys);
        backend->setClipboardWriter([](const QString &) {});
        engine = std::make_unique<QQmlApplicationEngine>();
        engine->setNetworkAccessManagerFactory(&network);
        QObject::connect(translator.get(), &Translator::changed, engine.get(), &QQmlEngine::retranslate);
        engine->rootContext()->setContextProperty("translator", translator.get());
        engine->rootContext()->setContextProperty(
            "theme", QVariantMap{{"radius", 0}, {"background", "#000000"}, {"foreground", "#ffffff"},
                                 {"accent", "#FFD60A"}, {"accentForeground", "black"}});
        engine->rootContext()->setContextProperty("backend", backend.get());
        engine->load(QUrl("qrc:/Main.qml"));
        if (!engine->rootObjects().isEmpty())
            window = qobject_cast<QQuickWindow *>(engine->rootObjects().first());
    }
    ~LoadedWindow() {
        engine.reset();
        backend.reset();
        QCoreApplication::removeTranslator(translator.get());
    }
};

class ReadstrTests : public QObject {
    Q_OBJECT

private slots:
    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
        QCOMPARE(theme.background(), QString("#0e0e10"));
        QCOMPARE(theme.foreground(), QString("#ffffff"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml",
              "selection_background = \"#abcdef\"\nbackground = \"#000000\"\nforeground = \"#c0c0c0\"\n"
              "accent = \"#112233\"\n");

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
        QCOMPARE(theme.background(), QString("#000000"));
        QCOMPARE(theme.foreground(), QString("#c0c0c0"));
    }

    // Omarchy switches themes by pointing current/theme at another directory.
    void themeFollowsAThemeSwitch()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("themes/one");
        QDir(dir.path()).mkpath("themes/two");
        QDir(dir.path()).mkpath("current");
        write(dir.path() + "/themes/one/colors.toml", "accent = \"#112233\"\n");
        write(dir.path() + "/themes/two/colors.toml", "accent = \"#445566\"\nbackground = \"#fafafa\"\n");
        QVERIFY(QFile::link(dir.path() + "/themes/one", dir.path() + "/current/theme"));

        Theme theme(dir.path() + "/current");
        QCOMPARE(theme.accent(), QString("#112233"));

        QVERIFY(QFile::link(dir.path() + "/themes/two", dir.path() + "/current/theme.new"));
        QVERIFY(std::rename(QFile::encodeName(dir.path() + "/current/theme.new").constData(),
                            QFile::encodeName(dir.path() + "/current/theme").constData()) == 0);
        QTRY_COMPARE(theme.accent(), QString("#445566"));
        QCOMPARE(theme.background(), QString("#fafafa"));
    }

    // A FIFO would block the open and an endless or huge file would fill
    // memory, both on the GUI thread; either way the theme falls back.
    void themeIgnoresFifosAndOversizedFiles()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("fifo/theme");
        QVERIFY(mkfifo(QFile::encodeName(dir.path() + "/fifo/theme/colors.toml").constData(), 0600) == 0);
        Theme fifo(dir.path() + "/fifo");
        QCOMPARE(fifo.accent(), QString("#FFD60A"));

        QDir(dir.path()).mkpath("large/theme");
        write(dir.path() + "/large/theme/colors.toml", "accent = \"#112233\"\n" + QByteArray(64 * 1024, '#'));
        Theme large(dir.path() + "/large");
        QCOMPARE(large.accent(), QString("#FFD60A"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":3}");

        Theme theme(dir.path(), nullptr, command);
        QTRY_COMPARE(theme.radius(), 3);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorFallsBackFromRegionToLanguageToEnglish()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"No feeds yet": "Ainda não há feeds", "Quit": "Sair"})");
        write(dir.path() + "/pt-BR.json", R"({"No feeds yet": "Nenhum feed ainda"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "No feeds yet"), QString("Nenhum feed ainda"));
        QCOMPARE(translator.translate("Main", "Quit"), QString("Sair"));
        QVERIFY(translator.translate("Main", "Keyboard shortcuts").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n feeds": {"one": "%n feed", "other": "%n feeds"}})");
        write(dir.path() + "/ru.json", R"({"%n feeds": {"one": "%n лента", "few": "%n ленты", "many": "%n лент", "other": "%n ленты"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n feeds", nullptr, 21), QString("%n лента"));
        QCOMPARE(translator.translate("Main", "%n feeds", nullptr, 22), QString("%n ленты"));
        QCOMPARE(translator.translate("Main", "%n feeds", nullptr, 25), QString("%n лент"));
    }

    void shippedCatalogsTranslateTheWindow_data()
    {
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("expected");
        QTest::newRow("da") << "da" << "Ingen feeds endnu";
        QTest::newRow("pt") << "pt" << "Ainda não há feeds";
        QTest::newRow("pt-BR") << "pt-BR" << "Nenhum feed ainda";
        QTest::newRow("ru") << "ru" << "Лент пока нет";
    }

    void shippedCatalogsTranslateTheWindow()
    {
        QFETCH(QString, language);
        QFETCH(QString, expected);
        QTemporaryDir dir;
        write(dir.path() + "/language", language.toUtf8());

        Translator translator(dir.path() + "/language");
        QCOMPARE(translator.translate("Main", "No feeds yet"), expected);
        QVERIFY(!translator.translate("Main", "Quit").isNull());
    }

    void pathsHonourReadstrHome()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());

        QCOMPARE(Paths::dataDirectory(), dir.path() + "/data");
        QCOMPARE(Paths::configDirectory(), dir.path() + "/config");
        QCOMPARE(Paths::cacheDirectory(), dir.path() + "/cache");
        QCOMPARE(Paths::databaseFile(), dir.path() + "/data/readstr.db");
        QCOMPARE(Paths::settingsFile(), dir.path() + "/config/settings.json");
    }

    // Falling back to the real directories would touch the very data
    // READSTR_HOME was set to keep away from, so nothing is usable instead.
    void pathsRefuseARelativeReadstrHome()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME", "XDG_DATA_HOME"});
        qputenv("READSTR_HOME", "relative/home");
        qputenv("XDG_DATA_HOME", (dir.path() + "/share").toUtf8());

        QVERIFY(Paths::dataDirectory().isEmpty());
        QVERIFY(Paths::databaseFile().isEmpty());
        QString out, err;
        QCOMPARE(run({"feeds"}, out, err), 1);
        QVERIFY2(err.contains("READSTR_HOME must be an absolute path"), qPrintable(err));
        QVERIFY(!QFile::exists(dir.path() + "/share"));
    }

    void pathsFollowXdgWithoutReadstrHome()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"});
        qunsetenv("READSTR_HOME");
        qputenv("XDG_DATA_HOME", (dir.path() + "/share").toUtf8());
        qputenv("XDG_CONFIG_HOME", (dir.path() + "/conf").toUtf8());
        qputenv("XDG_CACHE_HOME", (dir.path() + "/cache").toUtf8());

        QCOMPARE(Paths::dataDirectory(), dir.path() + "/share/readstr");
        QCOMPARE(Paths::configDirectory(), dir.path() + "/conf/readstr");
        QCOMPARE(Paths::cacheDirectory(), dir.path() + "/cache/readstr");
    }

    void cliDispatch()
    {
        QVERIFY(Cli::opensWindow({}));
        QVERIFY(Cli::opensWindow({"open"}));
        QVERIFY(!Cli::opensWindow({"--version"}));
        QVERIFY(!Cli::opensWindow({"open", "extra"}));
    }

    void versionPrintsPlainText()
    {
        QString out, err;
        QCOMPARE(run({"--version"}, out, err), 0);
        QCOMPARE(out, QString("readstr %1\n").arg(Cli::version));
        QVERIFY(err.isEmpty());

        out.clear();
        QCOMPARE(run({"version"}, out, err), 0);
        QCOMPARE(out, QString("readstr %1\n").arg(Cli::version));
    }

    void versionPrintsJson()
    {
        QString out, err;
        QCOMPARE(run({"version", "--json"}, out, err), 0);
        QVERIFY(err.isEmpty());
        QVERIFY(out.endsWith('\n'));
        QJsonParseError error;
        const QJsonObject object = QJsonDocument::fromJson(out.toUtf8(), &error).object();
        QCOMPARE(error.error, QJsonParseError::NoError);
        QCOMPARE(object.value("name").toString(), QString("readstr"));
        QCOMPARE(object.value("version").toString(), QString(Cli::version));
        QCOMPARE(object.value("qt").toString(), QString(qVersion()));
    }

    void badCommandsFailOnStderr()
    {
        QString out, err;
        QCOMPARE(run({"version", "--yaml"}, out, err), 1);
        QVERIFY(out.isEmpty());
        QVERIFY(err.contains("--yaml"));

        err.clear();
        QCOMPARE(run({"frobnicate"}, out, err), 1);
        QVERIFY(out.isEmpty());
        QVERIFY(err.contains("unknown command 'frobnicate'"));

        err.clear();
        QCOMPARE(run({"open", "x"}, out, err), 1);
        QVERIFY(out.isEmpty());
        QVERIFY(err.contains("open: takes no arguments yet"));
    }

    // `readstr --version > /dev/full` must fail rather than exit 0.
    void failedOutputWritesFail()
    {
        QBuffer readOnly;
        QVERIFY(readOnly.open(QIODevice::ReadOnly));
        QString err;
        QTextStream out(&readOnly), errStream(&err);
        QTest::ignoreMessage(QtWarningMsg, "QIODevice::write (QBuffer): ReadOnly device");
        QCOMPARE(Cli::finish(Cli::run({"--version"}, out, errStream), out, errStream), 1);
        QVERIFY(err.contains("could not write"));

        QString written;
        QTextStream good(&written);
        QCOMPARE(Cli::finish(Cli::run({"--version"}, good, errStream), good, errStream), 0);
        QVERIFY(!written.isEmpty());
    }

    // Store

    void storeMigratesAFreshDatabaseAndReopens()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/data/readstr.db";
        {
            Store store;
            QVERIFY2(store.open(path), qPrintable(store.lastError()));
            QCOMPARE(store.schemaVersion(), 6);
            QVERIFY(store.addFeed("https://a.example.com/feed", "A"));
        }
        Store again;
        QVERIFY2(again.open(path), qPrintable(again.lastError()));
        QCOMPARE(again.schemaVersion(), 6);
        QCOMPARE(again.feeds().size(), 1);

        QSqlDatabase raw = QSqlDatabase::addDatabase("QSQLITE", "raw");
        raw.setDatabaseName(path);
        QVERIFY(raw.open());
        QSqlQuery query(raw);
        QVERIFY(query.exec("PRAGMA journal_mode") && query.next());
        QCOMPARE(query.value(0).toString(), QString("wal"));
        QVERIFY(query.exec("PRAGMA user_version = 99"));
        query = QSqlQuery();
        raw.close();
        raw = QSqlDatabase();
        QSqlDatabase::removeDatabase("raw");

        Store newer;
        QVERIFY(!newer.open(path));
        QVERIFY(newer.lastError().contains("newer readstr"));
    }

    void storeRestrictsItsFiles()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        // Loose modes left by something else are tightened on open.
        QVERIFY(QDir(dir.path()).mkpath("data"));
        QVERIFY(QFile::setPermissions(dir.path() + "/data", QFile::Permissions(0x7755)));
        write(dir.path() + "/data/readstr.db", {});
        QVERIFY(QFile::setPermissions(dir.path() + "/data/readstr.db", QFile::Permissions(0x6644)));

        Store store;
        QVERIFY2(store.open(), qPrintable(store.lastError()));
        QCOMPARE(mode(dir.path() + "/data"), 0700);
        QCOMPARE(mode(dir.path() + "/data/readstr.db"), 0600);
        QVERIFY(store.addFeed("https://a.example.com/feed", "A"));
        QCOMPARE(mode(dir.path() + "/data/readstr.db-wal"), 0600);

        QTemporaryDir fresh;
        Store other;
        QVERIFY(other.open(fresh.path() + "/new/readstr.db"));
        QCOMPARE(mode(fresh.path() + "/new"), 0700);
        QCOMPARE(mode(fresh.path() + "/new/readstr.db"), 0600);
    }

    void storeDeduplicatesFeedsAndItems()
    {
        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.path() + "/readstr.db"));

        bool created = false;
        const qint64 id = store.addFeed("https://Example.com/feed/?b=2&a=1", "", "News", &created);
        QVERIFY(id && created);
        QCOMPARE(store.addFeed("http://example.com/feed?a=1&b=2", "Again", {}, &created), id);
        QVERIFY(!created);
        QVERIFY(store.addFeed("https://example.com:8443/feed", "Other port", {}, &created) != id);
        QVERIFY(created);

        const auto feed = store.feed(id);
        QVERIFY(feed);
        QCOMPARE(feed->title, QString("https://Example.com/feed/?b=2&a=1"));
        QCOMPARE(feed->category, QString("News"));

        ParsedFeed parsed;
        parsed.title = "Example";
        QVERIFY(store.updateFeedDetails(id, parsed));
        QCOMPARE(store.feed(id)->title, QString("Example"));
        parsed.title = "Renamed upstream";
        QVERIFY(store.updateFeedDetails(id, parsed));
        QCOMPARE(store.feed(id)->title, QString("Example"));

        ParsedItem first{"guid-1", "One", "https://example.com/1", "Ada", "Summary", "<p>One</p>",
                         QDateTime(QDate(2024, 1, 1), QTime(0, 0), QTimeZone::UTC), {}, 0, {}, {}, -1};
        ParsedItem second{"guid-2", "Two", {}, {}, {}, {}, {}, {}, 0, {}, {}, -1};
        QCOMPARE(store.saveItems(id, {first, second}), 2);
        QCOMPARE(store.saveItems(id, {first, second}), 0);

        const QList<Article> all = store.articles({});
        QCOMPARE(all.size(), 2);
        // Undated items sort by arrival, so the new one comes first.
        QCOMPARE(all.at(0).title, QString("Two"));
        QVERIFY(!all.at(0).published.isValid());
        const Article one = all.at(1);
        QCOMPARE(one.content, QString("<p>One</p>"));
        QCOMPARE(one.published, first.published);

        QVERIFY(store.setRead(one.id, true));
        QVERIFY(store.setStarred(one.id, true));
        first.title = "One, edited";
        first.published = {};
        QCOMPARE(store.saveItems(id, {first}), 0);
        const Article edited = store.articles({0, false, true, 0}).value(0);
        QCOMPARE(edited.title, QString("One, edited"));
        QVERIFY(edited.read && edited.starred);
        QCOMPARE(edited.published, QDateTime(QDate(2024, 1, 1), QTime(0, 0), QTimeZone::UTC));

        QCOMPARE(store.articles({0, true, false, 0}).size(), 1);
        QCOMPARE(store.articles({0, false, false, 1}).size(), 1);
        QCOMPARE(store.feed(id)->unread, 1);
        QCOMPARE(store.feed(id)->total, 2);

        QVERIFY(!store.setRead(9999, true));
        QCOMPARE(store.lastError(), QString("no article with id 9999"));
        QVERIFY(store.removeFeed(id));
        QVERIFY(store.articles({}).isEmpty());
        QVERIFY(!store.removeFeed(id));
    }

    void storeNeverReusesIds()
    {
        QTemporaryDir dir;
        Store store;
        QVERIFY2(store.open(dir.path() + "/readstr.db"), qPrintable(store.lastError()));
        const ParsedItem item{"guid", "Title", {}, {}, {}, {}, {}, {}, 0, {}, {}, -1};
        const qint64 first = store.addFeed("https://a.example.com/feed", "A");
        QCOMPARE(store.saveItems(first, {item}), 1);
        const qint64 firstItem = store.articles({}).value(0).id;
        QVERIFY(store.removeFeed(first));

        // Ids a user read from an earlier listing never point at a newer row.
        const qint64 second = store.addFeed("https://b.example.com/feed", "B");
        QVERIFY(second > first);
        QCOMPARE(store.saveItems(second, {item}), 1);
        QVERIFY(store.articles({}).value(0).id > firstItem);
        QVERIFY(!store.setRead(firstItem, true));
        QVERIFY(!store.removeFeed(first));
    }

    void storeKeysIpv6HostsUnambiguously()
    {
        QCOMPARE(Store::urlKey("http://[::1]:8080/feed"), QString("[::1]:8080/feed"));
        QVERIFY(Store::urlKey("http://[::1:8080]/feed") != Store::urlKey("http://[::1]:8080/feed"));
        QCOMPARE(Store::urlKey("https://[2001:DB8::1]/feed/"), QString("[2001:db8::1]/feed"));
    }

    void storeRefusesSymlinks()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("real"));
        QVERIFY(QFile::link(dir.path() + "/real", dir.path() + "/linked"));
        Store viaDirectory;
        QVERIFY(!viaDirectory.open(dir.path() + "/linked/readstr.db"));
        QVERIFY(!QFile::exists(dir.path() + "/real/readstr.db"));

        write(dir.path() + "/elsewhere.db", {});
        QVERIFY(QFile::setPermissions(dir.path() + "/elsewhere.db", QFile::Permissions(0x6644)));
        QVERIFY(QFile::link(dir.path() + "/elsewhere.db", dir.path() + "/real/readstr.db"));
        Store viaFile;
        QVERIFY(!viaFile.open(dir.path() + "/real/readstr.db"));
        QCOMPARE(mode(dir.path() + "/elsewhere.db"), 0644);

        QVERIFY(QFile::link(dir.path() + "/nowhere.db", dir.path() + "/real/dangling.db"));
        Store viaDangling;
        QVERIFY(!viaDangling.open(dir.path() + "/real/dangling.db"));
        QVERIFY(!QFile::exists(dir.path() + "/nowhere.db"));
    }

    // Several commands may open a fresh library at once: one switches it to
    // WAL and migrates it, and the others wait for that instead of failing.
    void storeOpensAFreshFileFromManyConnections()
    {
        QTemporaryDir dir;
        for (int trial = 0; trial < 5; ++trial) {
            const QString path = dir.path() + QString("/fresh%1.db").arg(trial);
            std::atomic<int> failures{0};
            QList<QThread *> threads;
            for (int i = 0; i < 8; ++i)
                threads << QThread::create([&failures, path] {
                    Store store;
                    if (!store.open(path) || store.schemaVersion() != 6) {
                        qWarning("%s", qPrintable(store.lastError()));
                        ++failures;
                    }
                });
            for (QThread *thread : threads)
                thread->start();
            for (QThread *thread : threads) {
                QVERIFY(thread->wait(30000));
                delete thread;
            }
            QCOMPARE(failures.load(), 0);
        }
    }

    // The window and the headless commands share one file, each with its own
    // connection.
    void storeSharesAFileBetweenConnections()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/readstr.db";
        constexpr int workers = 4, rounds = 20, perRound = 10;
        std::atomic<int> failures{0};
        QList<QThread *> threads;
        // All open the fresh file at once: one migrates and the others find
        // the schema in place; then all write items to the same feed.
        for (int worker = 0; worker < workers; ++worker)
            threads << QThread::create([&failures, path, worker] {
                Store store;
                const qint64 feed = store.open(path) ? store.addFeed("https://a.example.com/feed", "A") : 0;
                if (!feed) {
                    qWarning("%s", qPrintable(store.lastError()));
                    ++failures;
                    return;
                }
                for (int round = 0; round < rounds; ++round) {
                    QList<ParsedItem> items;
                    for (int i = 0; i < perRound; ++i)
                        items << ParsedItem{QString("%1-%2").arg(worker).arg(i), "Item", {}, {}, {}, {}, {}, {}, 0, {}, {}, -1};
                    if (store.saveItems(feed, items) < 0) {
                        qWarning("%s", qPrintable(store.lastError()));
                        ++failures;
                    }
                }
            });
        for (QThread *thread : threads)
            thread->start();
        for (QThread *thread : threads) {
            QVERIFY(thread->wait(30000));
            delete thread;
        }
        QCOMPARE(failures.load(), 0);

        Store writer;
        QVERIFY2(writer.open(path), qPrintable(writer.lastError()));
        QCOMPARE(writer.schemaVersion(), 6);
        QCOMPARE(writer.articles({0, false, false, 0}).size(), workers * perRound);

        // A reader in the middle of a transaction neither blocks a writer
        // nor sees its rows until it starts over.
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase("QSQLITE", "reader");
            raw.setDatabaseName(path);
            QVERIFY(raw.open());
            QSqlQuery query(raw);
            QVERIFY(query.exec("BEGIN") && query.exec("SELECT COUNT(*) FROM items") && query.next());
            QCOMPARE(query.value(0).toInt(), workers * perRound);
            query.finish();
            const qint64 feed = writer.feeds().value(0).id;
            QCOMPARE(writer.saveItems(feed, {ParsedItem{"late", "Late", {}, {}, {}, {}, {}, {}, 0, {}, {}, -1}}), 1);
            QVERIFY(query.exec("SELECT COUNT(*) FROM items") && query.next());
            QCOMPARE(query.value(0).toInt(), workers * perRound);
            QVERIFY(query.exec("COMMIT"));
            QVERIFY(query.exec("SELECT COUNT(*) FROM items") && query.next());
            QCOMPARE(query.value(0).toInt(), workers * perRound + 1);
            query = QSqlQuery();
            raw.close();
        }
        QSqlDatabase::removeDatabase("reader");
    }

    // Feed parser

    void parsesRss()
    {
        QString error;
        const auto feed = FeedParser::parse(fixture("rss2.xml"), QUrl("https://example.com/feed.xml"), &error);
        QVERIFY2(feed, qPrintable(error));
        QCOMPARE(feed->title, QString("Example & Friends"));
        QCOMPARE(feed->siteUrl, QString("https://example.com/"));
        QCOMPARE(feed->description, QString("News from the example team"));
        QCOMPARE(feed->items.size(), 4);

        const ParsedItem &first = feed->items.at(0);
        QCOMPARE(first.title, QString("First post"));
        QCOMPARE(first.guid, QString("post-1"));
        QCOMPARE(first.url, QString("https://example.com/posts/1"));
        QCOMPARE(first.author, QString("Ada"));
        QCOMPARE(first.summary, QString("Short & sweet"));
        QVERIFY(first.content.startsWith("<p>The <strong>full</strong> text.</p>"));
        QCOMPARE(first.published, QDateTime(QDate(2006, 1, 2), QTime(15, 4, 5), QTimeZone::UTC));

        const ParsedItem &relative = feed->items.at(1);
        QCOMPARE(relative.url, QString("https://example.com/posts/2"));
        QCOMPARE(relative.guid, relative.url);
        QVERIFY(!relative.published.isValid());
        QCOMPARE(relative.content, QString("Plain description"));

        const ParsedItem &guidLink = feed->items.at(2);
        QCOMPARE(guidLink.url, QString("https://example.com/posts/3"));
        QCOMPARE(guidLink.published, QDateTime(QDate(2006, 1, 3), QTime(15, 0), QTimeZone::UTC));

        const ParsedItem &script = feed->items.at(3);
        QVERIFY(script.url.isEmpty());
        QVERIFY(!script.published.isValid());
    }

    void parsesRss1()
    {
        const auto feed = FeedParser::parse(fixture("rss1.rdf"), QUrl("https://rdf.example.com/index.rdf"));
        QVERIFY(feed);
        QCOMPARE(feed->title, QString("RDF Site"));
        QCOMPARE(feed->items.size(), 1);
        QCOMPARE(feed->items.at(0).url, QString("https://rdf.example.com/one"));
        QCOMPARE(feed->items.at(0).published, QDateTime(QDate(2024, 3, 4), QTime(5, 6, 7), QTimeZone::UTC));
    }

    void parsesAtom()
    {
        QString error;
        const auto feed = FeedParser::parse(fixture("atom.xml"), QUrl("https://feeds.example.org/atom"), &error);
        QVERIFY2(feed, qPrintable(error));
        QCOMPARE(feed->title, QString("Atom & Eve"));
        QCOMPARE(feed->description, QString("Notes"));
        QCOMPARE(feed->siteUrl, QString("https://blog.example.org/"));
        QCOMPARE(feed->items.size(), 3);

        const ParsedItem &hello = feed->items.at(0);
        QCOMPARE(hello.title, QString("Hello world"));
        QCOMPARE(hello.url, QString("https://blog.example.org/2024/hello"));
        QCOMPARE(hello.guid, QString("tag:blog.example.org,2024:1"));
        QCOMPARE(hello.author, QString("Eve"));
        QCOMPARE(hello.summary, QString("A short summary"));
        QVERIFY(hello.content.contains("<p>Rich <a href=\"/x\">content</a></p>"));
        QCOMPARE(hello.published, QDateTime(QDate(2024, 1, 2), QTime(1, 4, 5, 123), QTimeZone::UTC));

        const ParsedItem &updated = feed->items.at(1);
        QCOMPARE(updated.url, QString("https://other.example.net/section/page.html"));
        QCOMPARE(updated.content, QString("<p>Escaped <em>html</em></p>"));
        QCOMPARE(updated.summary, QString("Escaped html"));
        QCOMPARE(updated.published, QDateTime(QDate(2024, 2, 1), QTime(0, 0), QTimeZone::UTC));

        const ParsedItem &text = feed->items.at(2);
        QCOMPARE(text.content, QString("1 &lt; 2 &amp; plain"));
        QVERIFY(!text.published.isValid());
    }

    void parsesJsonFeed()
    {
        QString error;
        const auto feed = FeedParser::parse(fixture("jsonfeed.json"), QUrl("https://json.example.com/feed.json"), &error);
        QVERIFY2(feed, qPrintable(error));
        // JSON Feed titles are plain text, so angle brackets are kept.
        QCOMPARE(feed->title, QString("JSON <Feed>"));
        QCOMPARE(feed->siteUrl, QString("https://json.example.com/"));
        QCOMPARE(feed->items.size(), 3);

        const ParsedItem &first = feed->items.at(0);
        QCOMPARE(first.guid, QString("42"));
        QCOMPARE(first.url, QString("https://json.example.com/items/42"));
        QCOMPARE(first.author, QString("Item Author"));
        QCOMPARE(first.summary, QString("Hello JSON"));
        QCOMPARE(first.published, QDateTime(QDate(2024, 5, 6), QTime(11, 8, 9), QTimeZone::UTC));

        const ParsedItem &text = feed->items.at(1);
        QCOMPARE(text.url, QString("https://elsewhere.example.com/b"));
        QCOMPARE(text.title, QString("Untitled"));
        QCOMPARE(text.author, QString("Feed Author"));
        QCOMPARE(text.content, QString("Text with &lt;angle&gt; brackets"));
        QCOMPARE(text.summary, QString("Text with <angle> brackets"));

        const ParsedItem &undated = feed->items.at(2);
        QVERIFY(undated.url.isEmpty());
        QVERIFY(!undated.published.isValid());
        QCOMPARE(undated.summary, QString("Given summary"));

        const QByteArray notJsonFeed = "{\"title\": \"not a feed\"}";
        QVERIFY(!FeedParser::parse(notJsonFeed, {}, &error));
        QVERIFY(error.contains("JSON Feed"));
    }

    void refusesMalformedAndHostileFeeds()
    {
        QString error;
        QVERIFY(!FeedParser::parse(fixture("malformed.xml"), {}, &error));
        QVERIFY(error.startsWith("malformed feed"));
        QVERIFY(!FeedParser::parse("<html><body>hi</body></html>", {}, &error));
        QVERIFY(error.contains("<html>"));
        QVERIFY(!FeedParser::parse("   \n", {}, &error));
        QVERIFY(!FeedParser::parse("{not json", {}, &error));

        QVERIFY(!FeedParser::parse(fixture("billion-laughs.xml"), {}, &error));
        QVERIFY(error.contains("entities"));
        QVERIFY(!FeedParser::parse(fixture("external-entity.xml"), {}, &error));
        QVERIFY(error.contains("entities"));

        // A DTD that only names an external file is fine: it is never fetched.
        const auto old = FeedParser::parse(fixture("external-dtd.xml"), {}, &error);
        QVERIFY2(old, qPrintable(error));
        QCOMPARE(old->title, QString("Old & DTD"));
        QCOMPARE(old->items.size(), 1);

        QByteArray huge = "<rss><channel><title>";
        huge += QByteArray(FeedParser::maxBytes, 'a');
        huge += "</title></channel></rss>";
        QVERIFY(!FeedParser::parse(huge, {}, &error));
        QVERIFY(error.contains("larger than"));
    }

    void resolvesRssGuidsAgainstTheirOwnBase()
    {
        const auto feed = FeedParser::parse("<rss><channel>"
                                            "<item><guid xml:base=\"https://other.example.com/x/\">p/1</guid></item>"
                                            "<item><guid>p/2</guid></item>"
                                            "</channel></rss>",
                                            QUrl("https://example.com/feed.xml"));
        QVERIFY(feed);
        QCOMPARE(feed->items.at(0).url, QString("https://other.example.com/x/p/1"));
        QCOMPARE(feed->items.at(1).url, QString("https://example.com/p/2"));
    }

    void ignoresForeignElementsInAtom()
    {
        QString error;
        const auto feed = FeedParser::parse(
            "<feed xmlns=\"http://www.w3.org/2005/Atom\" xmlns:x=\"urn:example:other\">"
            "<title>Feed</title><x:title>Not the feed</x:title><x:link href=\"https://evil.example.com/\"/>"
            "<link href=\"https://good.example.com/\"/>"
            "<entry><x:link href=\"https://evil.example.com/1\"/><link href=\"https://good.example.com/1\"/>"
            "<id>right</id><x:id>wrong</x:id><title>Right</title><x:title>Wrong</x:title>"
            "<author><name>Ann</name><x:name>Bob</x:name></author>"
            "<summary type=\"text\">1 &lt; 2 &lt;b&gt; &amp;amp; ok</summary><x:summary>Wrong</x:summary>"
            "</entry></feed>",
            {}, &error);
        QVERIFY2(feed, qPrintable(error));
        QCOMPARE(feed->title, QString("Feed"));
        QCOMPARE(feed->siteUrl, QString("https://good.example.com/"));
        const ParsedItem &item = feed->items.at(0);
        QCOMPARE(item.guid, QString("right"));
        QCOMPARE(item.title, QString("Right"));
        QCOMPARE(item.url, QString("https://good.example.com/1"));
        QCOMPARE(item.author, QString("Ann"));
        // Atom text is plain: its '<' and '&amp;' are what the author wrote.
        QCOMPARE(item.summary, QString("1 < 2 <b> &amp; ok"));
    }

    void keepsPlainTextJsonFieldsLiteral()
    {
        const auto feed = FeedParser::parse(
            "{\"version\": \"https://jsonfeed.org/version/1.1\", \"title\": \"A\", \"items\": ["
            "{\"id\": \"1\", \"title\": \"x < y\", \"summary\": \"<b> stays &amp; so does this\","
            " \"content_html\": \"<p>Hi</p>\"},"
            "{\"id\": \"2\", \"content_html\": \"<p>Only &amp; <b>html</b></p>\"}]}",
            {});
        QVERIFY(feed);
        QCOMPARE(feed->items.at(0).title, QString("x < y"));
        QCOMPARE(feed->items.at(0).summary, QString("<b> stays &amp; so does this"));
        QCOMPARE(feed->items.at(1).summary, QString("Only & html"));

        const auto mixed = FeedParser::parse(
            "{\"version\": \"https://jsonfeed.org/version/1.1\", \"items\": [null, 1, \"x\", [], {\"id\": \"real\"}]}",
            {});
        QVERIFY(mixed);
        QCOMPARE(mixed->items.size(), 1);
        QCOMPARE(mixed->items.at(0).guid, QString("real"));
    }

    void acceptsOnlyRecognizedRoots()
    {
        QString error;
        QVERIFY(!FeedParser::parse("<feed><title>No namespace</title></feed>", {}, &error));
        QVERIFY(error.contains("<feed>"));
        QVERIFY(!FeedParser::parse("<feed xmlns=\"urn:example:other\"/>", {}, &error));
        QVERIFY(!FeedParser::parse("<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
                                   "<rdf:Description/></rdf:RDF>",
                                   {}, &error));
        QVERIFY(error.contains("RSS channel"));
        QVERIFY(!FeedParser::parse("<RDF><channel/></RDF>", {}, &error));

        // Empty but well-formed feeds are still feeds.
        QVERIFY(FeedParser::parse("<feed xmlns=\"http://www.w3.org/2005/Atom\"/>", {}));
        QVERIFY(FeedParser::parse("<rss><channel/></rss>", {}));
        QVERIFY(FeedParser::parse("<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\" "
                                  "xmlns=\"http://purl.org/rss/1.0/\"><channel/></rdf:RDF>",
                                  {}));
        const auto old = FeedParser::parse("<feed xmlns=\"http://purl.org/atom/ns#\"><title>Old</title>"
                                           "<tagline>Atom 0.3</tagline></feed>",
                                           {});
        QVERIFY(old);
        QCOMPARE(old->title, QString("Old"));
        QCOMPARE(old->description, QString("Atom 0.3"));

        QVERIFY(!FeedParser::parse("<rss><channel/></rss><rss><channel/></rss>", {}, &error));
        QVERIFY(error.startsWith("malformed feed"));
        QVERIFY(!FeedParser::parse("<feed xmlns=\"http://www.w3.org/2005/Atom\"/><feed/>", {}, &error));
        QVERIFY(FeedParser::parse("<rss><channel/></rss>\n<!-- generated -->\n", {}, &error));
    }

    void capsTheItemCount()
    {
        QByteArray xml = "<rss><channel><title>Many</title>";
        for (int i = 0; i < FeedParser::maxItems + 200; ++i)
            xml += "<item><guid>" + QByteArray::number(i) + "</guid></item>";
        xml += "</channel></rss>";
        const auto feed = FeedParser::parse(xml, {});
        QVERIFY(feed);
        QCOMPARE(feed->items.size(), FeedParser::maxItems);
        QCOMPARE(feed->items.last().guid, QString::number(FeedParser::maxItems - 1));
    }

    void parsesDates_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QDateTime>("expected");
        const auto utc = [](int y, int mo, int d, int h, int mi, int s) {
            return QDateTime(QDate(y, mo, d), QTime(h, mi, s), QTimeZone::UTC);
        };
        QTest::newRow("rfc822 gmt") << "Mon, 02 Jan 2006 15:04:05 GMT" << utc(2006, 1, 2, 15, 4, 5);
        QTest::newRow("rfc822 offset") << "Mon, 02 Jan 2006 15:04:05 +0130" << utc(2006, 1, 2, 13, 34, 5);
        QTest::newRow("rfc822 16 h offset") << "Mon, 02 Jan 2006 15:04:05 +1600" << utc(2006, 1, 1, 23, 4, 5);
        QTest::newRow("rfc822 offset past 16 h") << "Mon, 02 Jan 2006 15:04:05 +1630" << QDateTime();
        QTest::newRow("rfc822 negative offset past 16 h") << "Mon, 02 Jan 2006 15:04:05 -1700" << QDateTime();
        QTest::newRow("rfc822 named zone") << "Mon, 2 Jan 2006 15:04:05 PST" << utc(2006, 1, 2, 23, 4, 5);
        QTest::newRow("rfc822 no seconds") << "02 Jan 2006 15:04 UT" << utc(2006, 1, 2, 15, 4, 0);
        QTest::newRow("rfc822 two-digit year") << "Sat, 07 Sep 02 00:00:01 Z" << utc(2002, 9, 7, 0, 0, 1);
        QTest::newRow("rfc822 long names") << "Tuesday, 03 January 2006 10:00:00 +0000" << utc(2006, 1, 3, 10, 0, 0);
        QTest::newRow("rfc822 no zone") << "Mon, 02 Jan 2006 15:04:05" << utc(2006, 1, 2, 15, 4, 5);
        QTest::newRow("rfc3339") << "2024-01-02T03:04:05Z" << utc(2024, 1, 2, 3, 4, 5);
        QTest::newRow("rfc3339 offset") << "2024-01-02T03:04:05-05:00" << utc(2024, 1, 2, 8, 4, 5);
        QTest::newRow("rfc3339 space") << "2024-01-02 03:04:05Z" << utc(2024, 1, 2, 3, 4, 5);
        QTest::newRow("date only") << "2024-01-02" << utc(2024, 1, 2, 0, 0, 0);
        QTest::newRow("empty") << "" << QDateTime();
        QTest::newRow("garbage") << "yesterday-ish" << QDateTime();
        QTest::newRow("bad day") << "Mon, 32 Jan 2006 15:04:05 GMT" << QDateTime();
    }

    void parsesDates()
    {
        QFETCH(QString, text);
        QFETCH(QDateTime, expected);
        QCOMPARE(FeedParser::parseDate(text), expected);
    }

    void convertsHtmlToPlainText()
    {
        QCOMPARE(FeedParser::plainText("<p>Hi &amp; <b>bye</b></p><!-- x --><script>evil()</script><style>p{}</style>"),
                 QString("Hi & bye"));
        QCOMPARE(FeedParser::plainText("1 < 2 &#38; &#x263A; &unknown; &mdash;"), QString("1 < 2 & ☺ &unknown; —"));
        QCOMPARE(FeedParser::plainText(QString("a\x01\x1b[31mb")), QString("a [31mb"));
        QCOMPARE(FeedParser::plainText("&#0; &#xD800; &#99999999;"), QString("&#0; &#xD800; &#99999999;"));
        QCOMPARE(FeedParser::plainText("a <SCRIPT type=x>1 < 2</script > b <scrip>c</scrip> <!--x-->d"),
                 QString("a b c d"));
        QCOMPARE(FeedParser::plainText("kept <script>never closed"), QString("kept"));
        QCOMPARE(FeedParser::plainText("1 < 2 <b and <!-- open"), QString("1 < 2 <b and <!-- open"));
        QCOMPARE(FeedParser::plainText("<scripts>x</scripts>"), QString("x"));
    }

    // Each of these took quadratic time with a regular expression.
    void plainTextStaysFastOnHostileMarkup()
    {
        const QStringList inputs{
            QString("<script a").repeated(200000) + "</scrip>",
            QString("<style ").repeated(200000) + "x>",
            QString("<!--").repeated(500000) + ">",
            QString("<a").repeated(500000),
            QString("<b>").repeated(300000),
        };
        QElapsedTimer timer;
        timer.start();
        for (const QString &input : inputs)
            FeedParser::plainText(input);
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString("took %1 ms").arg(timer.elapsed())));
    }

    // Discovery

    void findsFeedLinksInPages()
    {
        const auto links = Discovery::feedLinks(fixture("page-with-links.html"), QUrl("https://www.example.com/blog/"));
        QCOMPARE(links.size(), 3);
        QCOMPARE(links.at(0).url, QUrl("https://cdn.example.com/site/feed.xml?a=1&b=2"));
        QCOMPARE(links.at(0).title, QString("Posts & notes"));
        QCOMPARE(links.at(0).type, QString("application/rss+xml"));
        QCOMPARE(links.at(1).url, QUrl("https://cdn.example.com/atom.xml"));
        QCOMPARE(links.at(1).type, QString("application/atom+xml"));
        QCOMPARE(links.at(2).url, QUrl("https://json.example.com/feed.json"));

        QVERIFY(Discovery::feedLinks(fixture("page-without-links.html"), QUrl("https://example.com/")).isEmpty());

        const QList<QUrl> common = Discovery::commonFeedUrls(QUrl("https://user@example.com:8080/a/b?c#d"));
        QCOMPARE(common.size(), 11);
        QCOMPARE(common.first(), QUrl("https://example.com:8080/feed"));
        QCOMPARE(common.last(), QUrl("https://example.com:8080/feeds/posts/default"));
    }

    void readsLinkTagsLikeABrowser()
    {
        const auto links = Discovery::feedLinks(
            "<link title=\"a > b\" rel=alternate type='application/rss+xml' href=/q.xml>"
            "<meta content='<link rel=alternate type=application/rss+xml href=/fake1.xml>'>"
            "<script>if (a<b) x = '<link rel=alternate type=application/rss+xml href=/fake2.xml>'</script>"
            "<!-- <link rel=alternate type=application/rss+xml href=/fake3.xml> -->"
            "<LINK REL=\"alternate\" TYPE=\"application/atom+xml\" HREF=\"/a.xml\"/>",
            QUrl("https://example.com/"));
        QCOMPARE(links.size(), 2);
        QCOMPARE(links.at(0).url, QUrl("https://example.com/q.xml"));
        QCOMPARE(links.at(0).title, QString("a > b"));
        QCOMPARE(links.at(1).url, QUrl("https://example.com/a.xml"));

        const QList<QByteArray> hostile{
            QByteArray("<link ").repeated(150000) + "\">",
            QByteArray("<link a=\"").repeated(100000),
            QByteArray("<!--").repeated(250000) + ">",
            "<link " + QByteArray(900000, 'a') + " b=c>",
            QByteArray("<script>").repeated(120000),
        };
        QElapsedTimer timer;
        timer.start();
        for (const QByteArray &page : hostile)
            Discovery::feedLinks(page, QUrl("https://example.com/"));
        // The title attribute goes through plainText, so a long run of
        // openers in it must not be slow either.
        const auto stuffed = Discovery::feedLinks("<link title=\"" + QByteArray("<style").repeated(150000)
                                                      + "\" rel=alternate type=application/rss+xml href=/t.xml>",
                                                  QUrl("https://example.com/"));
        QCOMPARE(stuffed.size(), 1);
        QCOMPARE(stuffed.at(0).url, QUrl("https://example.com/t.xml"));
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString("took %1 ms").arg(timer.elapsed())));
    }

    void discoversFeedsOverHttp()
    {
        FakeHttpServer server;
        server.serve("/feed.xml", fixture("rss2.xml"));
        server.serve("/page", "<link rel=alternate type=application/rss+xml href=/feed.xml>", "text/html");
        server.serve("/bare", fixture("page-without-links.html"), "text/html");
        Fetcher fetcher;

        auto direct = Discovery::discover(fetcher, server.url("/feed.xml"));
        QVERIFY2(direct.found, qPrintable(direct.error));
        QVERIFY(!direct.discovered);
        QCOMPARE(direct.feed.title, QString("Example & Friends"));

        auto linked = Discovery::discover(fetcher, server.url("/page"));
        QVERIFY2(linked.found, qPrintable(linked.error));
        QVERIFY(linked.discovered);
        QCOMPARE(linked.feedUrl, server.url("/feed.xml"));

        // No links: the common paths are tried, and /feed.xml is one of them.
        auto common = Discovery::discover(fetcher, server.url("/bare"));
        QVERIFY2(common.found, qPrintable(common.error));
        QCOMPARE(common.feedUrl, server.url("/feed.xml"));

        FakeHttpServer empty;
        empty.serve("/", "<html></html>", "text/html");
        auto none = Discovery::discover(fetcher, empty.url("/"));
        QVERIFY(!none.found);
        QVERIFY(none.error.contains("no RSS, Atom, or JSON feed"));
    }

    // OPML, ported from src/lib/opml.test.ts

    void opmlAssignsFolderFromNestedOutline()
    {
        const auto feeds = Opml::parse("<opml version=\"2.0\"><body>\n"
            "          <outline text=\"News\" title=\"News\">\n"
            "            <outline type=\"rss\" text=\"Example\" title=\"Example\" xmlUrl=\"https://example.com/feed\"/>\n"
            "          </outline>\n"
            "        </body></opml>");
        QCOMPARE(feeds.size(), 1);
        QCOMPARE(feeds.at(0).folder, QString("News"));
        QCOMPARE(feeds.at(0).xmlUrl, QString("https://example.com/feed"));
        QCOMPARE(feeds.at(0).title, QString("Example"));
    }

    void opmlDistinguishesNostr()
    {
        auto feeds = Opml::parse("<opml><body><outline type=\"nostr\" text=\"Author\" nostr=\"npub1abc\"/></body></opml>");
        QCOMPARE(feeds.value(0).npub, QString("npub1abc"));
        QVERIFY(feeds.value(0).xmlUrl.isEmpty());
        feeds = Opml::parse("<opml><body><outline text=\"Author\" npub=\"npub1xyz\"/></body></opml>");
        QCOMPARE(feeds.value(0).npub, QString("npub1xyz"));
    }

    void opmlFallsBackFromTitleToTextAndParsesTags()
    {
        auto feeds = Opml::parse("<opml><body><outline type=\"rss\" text=\"Text Title\" xmlUrl=\"https://a.com/feed\"/></body></opml>");
        QCOMPARE(feeds.value(0).title, QString("Text Title"));
        feeds = Opml::parse("<opml><body><outline type=\"rss\" text=\"A\" xmlUrl=\"https://a.com/feed\" category=\"tech, news ,\"/></body></opml>");
        QCOMPARE(feeds.value(0).tags, QStringList({"tech", "news"}));
    }

    void opmlRefusesMalformedAndHostileInput()
    {
        QString error;
        QVERIFY(Opml::parse("", &error).isEmpty());
        QVERIFY(Opml::parse("<opml><body><outline></outline></body></opml>").isEmpty());
        QVERIFY(Opml::parse("<opml><body><outline text=", &error).isEmpty());
        QVERIFY(error.startsWith("malformed OPML"));
        QVERIFY(Opml::parse("<html/>", &error).isEmpty());
        QCOMPARE(error, QString("not an OPML file"));

        const QByteArray laughs = "<?xml version=\"1.0\"?><!DOCTYPE opml [<!ENTITY a \"aaaaaaaaaa\"><!ENTITY b \"&a;&a;&a;&a;\">]>\n"
            "            <opml><body><outline xmlUrl=\"https://a.com/&b;\"/></body></opml>";
        QVERIFY(Opml::parse(laughs, &error).isEmpty());
        QVERIFY(error.contains("entities"));

        QVERIFY(Opml::parse("<opml>" + QByteArray(Opml::maxBytes, ' ') + "</opml>", &error).isEmpty());
        QVERIFY(error.contains("larger than"));
    }

    void opmlCapsOutlinesAndDepth()
    {
        QByteArray rows;
        for (int i = 0; i < 5100; ++i)
            rows += "<outline type=\"rss\" xmlUrl=\"https://x.com/" + QByteArray::number(i) + "\"/>";
        QVERIFY(Opml::parse("<opml><body>" + rows + "</body></opml>").size() <= Opml::maxOutlines);

        QByteArray deep;
        for (int i = 0; i < 200; ++i)
            deep += "<outline text=\"f" + QByteArray::number(i) + "\">";
        deep += "<outline xmlUrl=\"https://deep.example.com/feed\"/>";
        for (int i = 0; i < 200; ++i)
            deep += "</outline>";
        QVERIFY(Opml::parse("<opml><body>" + deep + "</body></opml>").isEmpty());
    }

    void opmlBoundsNestedBodies()
    {
        const QByteArray nested = "<opml>" + QByteArray("<body>").repeated(100000)
                                  + "<outline xmlUrl=\"https://x.example.com/feed\"/>"
                                  + QByteArray("</body>").repeated(100000) + "</opml>";
        QString error;
        QVERIFY(Opml::parse(nested, &error).isEmpty());
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void opmlRoundTrips()
    {
        const QList<Opml::ExportFeed> input{
            {"RSS", "https://a.com/feed", "Feed A", "News", {"tech"}},
            {"NOSTR", "npub1abc", "Author", {}, {"nostr"}},
        };
        const auto parsed = Opml::parse(Opml::build(input));
        QCOMPARE(parsed.size(), 2);
        const auto rss = parsed.at(0).xmlUrl.isEmpty() ? parsed.at(1) : parsed.at(0);
        const auto nostr = parsed.at(0).xmlUrl.isEmpty() ? parsed.at(0) : parsed.at(1);
        QCOMPARE(rss.xmlUrl, QString("https://a.com/feed"));
        QCOMPARE(rss.title, QString("Feed A"));
        QCOMPARE(rss.folder, QString("News"));
        QCOMPARE(rss.tags, QStringList{"tech"});
        QCOMPARE(nostr.npub, QString("npub1abc"));
        QCOMPARE(nostr.title, QString("Author"));
        QCOMPARE(nostr.tags, QStringList{"nostr"});
    }

    void opmlEscapesSpecialCharacters()
    {
        const QByteArray xml = Opml::build({{"RSS", "https://a.com/feed?x=1&y=2", "Tom & Jerry <\"quote\">\x01", {}, {}}});
        QVERIFY(xml.contains("&amp;"));
        QVERIFY(xml.contains("&lt;"));
        QVERIFY(xml.contains("&quot;"));
        const auto parsed = Opml::parse(xml);
        QCOMPARE(parsed.size(), 1);
        QCOMPARE(parsed.at(0).title, QString("Tom & Jerry <\"quote\">"));
        QCOMPARE(parsed.at(0).xmlUrl, QString("https://a.com/feed?x=1&y=2"));
    }

    void opmlSkipsVideoDuplicatingNostr()
    {
        const QByteArray xml = Opml::build({{"NOSTR", "npub1dup", "A", {}, {}}, {"NOSTR_VIDEO", "npub1dup", "A video", {}, {}}});
        int count = 0;
        for (const auto &feed : Opml::parse(xml))
            count += feed.npub == "npub1dup";
        QCOMPARE(count, 1);
    }

    void opmlPlanDeduplicates()
    {
        const auto feed = [](const QString &url, const QString &npub = {}) {
            return Opml::Outline{{}, url, npub, {}, {}};
        };
        Opml::Plan plan = Opml::plan({feed("https://a.com/feed/"), feed("https://new.com/feed"), feed({}, "npub1abc"),
                                      feed({}, "npub1new")},
                                     {"https://a.com/feed"}, {"npub1abc"});
        QCOMPARE(plan.skipped, 2);
        QCOMPARE(plan.toAdd.size(), 2);
        QCOMPARE(plan.toAdd.at(0).xmlUrl, QString("https://new.com/feed"));
        QCOMPARE(plan.toAdd.at(1).npub, QString("npub1new"));

        plan = Opml::plan({feed("https://a.com/feed"), feed("https://a.com/feed/"), feed({}, "npub1abc"),
                           feed({}, "npub1abc")},
                          {});
        QCOMPARE(plan.toAdd.size(), 2);
        QCOMPARE(plan.skipped, 2);

        Opml::Outline folder = feed("https://a.com/feed");
        folder.folder = "News";
        folder.tags = {"tech", "news"};
        plan = Opml::plan({folder}, {});
        QCOMPARE(plan.toAdd.at(0).folder, QString("News"));
        QCOMPARE(plan.toAdd.at(0).tags, QStringList({"tech", "news"}));
    }

    // Fetcher, against a local server only

    void fetcherSendsConditionalGets()
    {
        FakeHttpServer server;
        server.route("/feed", [](const FakeHttpServer::Request &request) {
            if (request.headers.value("if-none-match") == "\"v1\"")
                return FakeHttpServer::Response{304, {{"ETag", "\"v1\""}}, {}};
            return FakeHttpServer::Response{
                200, {{"ETag", "\"v1\""}, {"Last-Modified", "Mon, 02 Jan 2006 15:04:05 GMT"}}, "body"};
        });
        Fetcher fetcher;
        const FetchResult first = fetcher.get({server.url("/feed"), {}, {}});
        QVERIFY2(first.ok(), qPrintable(first.error));
        QCOMPARE(first.status, 200);
        QCOMPARE(first.body, QByteArray("body"));
        QCOMPARE(first.etag, QString("\"v1\""));
        QCOMPARE(first.lastModified, QString("Mon, 02 Jan 2006 15:04:05 GMT"));
        QCOMPARE(server.requests.last().headers.value("user-agent"), QByteArray("readstr-desktop/" READSTR_VERSION));

        const FetchResult second = fetcher.get({server.url("/feed"), first.etag, first.lastModified});
        QVERIFY2(second.ok(), qPrintable(second.error));
        QVERIFY(second.notModified);
        QVERIFY(second.body.isEmpty());
        QCOMPARE(server.requests.last().headers.value("if-modified-since"), QByteArray("Mon, 02 Jan 2006 15:04:05 GMT"));

        const FetchResult missing = fetcher.get({server.url("/missing"), {}, {}});
        QCOMPARE(missing.error, QString("HTTP 404"));
    }

    void fetcherAbortsOversizedResponses()
    {
        FakeHttpServer server;
        // Declares a gigabyte but sends one byte: only the Content-Length
        // check can call it oversized.
        server.route("/declared", [](const FakeHttpServer::Request &) {
            FakeHttpServer::Response response{200, {{"Content-Length", "1073741824"}}, {}};
            response.stream = 1;
            response.hold = true;
            return response;
        });
        // Never ends: only aborting while it streams stops it before the deadline.
        server.route("/streamed", [](const FakeHttpServer::Request &) {
            FakeHttpServer::Response response;
            response.stream = std::numeric_limits<qint64>::max();
            return response;
        });
        server.route("/fits", [](const FakeHttpServer::Request &) {
            return FakeHttpServer::Response{200, {}, QByteArray(100 * 1024, 'x')};
        });
        Fetcher fetcher;
        fetcher.setLimits({200 * 1024, 10000, 5});
        const auto results = fetcher.fetchAll({{server.url("/declared"), {}, {}},
                                               {server.url("/streamed"), {}, {}},
                                               {server.url("/fits"), {}, {}}});
        QVERIFY2(results.at(0).error.contains("larger than"), qPrintable(results.at(0).error));
        QVERIFY(results.at(0).body.isEmpty());
        QVERIFY2(results.at(1).error.contains("larger than"), qPrintable(results.at(1).error));
        QVERIFY(results.at(1).body.isEmpty());
        QVERIFY2(results.at(2).ok(), qPrintable(results.at(2).error));
        QCOMPARE(results.at(2).body.size(), 100 * 1024);
    }

    void fetcherFailsCompressionBombsFast()
    {
        FakeHttpServer server;
        // 128 MiB of zeros in well under a megabyte, large enough that a
        // capped read buffer used to stall it until the deadline. A zlib
        // stream without qCompress's four-byte length is what
        // Content-Encoding: deflate carries.
        const QByteArray bomb = qCompress(QByteArray(128 * 1024 * 1024, '\0'), 1).mid(4);
        server.route("/bomb", [bomb](const FakeHttpServer::Request &) {
            return FakeHttpServer::Response{200, {{"Content-Encoding", "deflate"}}, bomb};
        });
        Fetcher fetcher;
        fetcher.setLimits({10 * 1024 * 1024, 20000, 5});
        QElapsedTimer timer;
        timer.start();
        const FetchResult result = fetcher.get({server.url("/bomb"), {}, {}});
        QVERIFY(!result.ok());
        QVERIFY(result.body.isEmpty());
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString("took %1 ms: %2").arg(timer.elapsed()).arg(result.error)));
    }

    void fetcherLimitsRedirects()
    {
        FakeHttpServer server;
        server.redirect("/loop", "/loop");
        server.redirect("/one", "/two", 301);
        server.redirect("/two", "/feed");
        server.serve("/feed", "ok");
        server.redirect("/file", "file:///etc/passwd");
        server.redirect("/ftp", "ftp://127.0.0.1/feed");
        Fetcher fetcher;
        fetcher.setLimits({1024 * 1024, 10000, 3});

        const FetchResult loop = fetcher.get({server.url("/loop"), {}, {}});
        QCOMPARE(loop.error, QString("more than 3 redirects"));
        QCOMPARE(server.requests.size(), 4);

        const FetchResult followed = fetcher.get({server.url("/one"), {}, {}});
        QVERIFY2(followed.ok(), qPrintable(followed.error));
        QCOMPARE(followed.body, QByteArray("ok"));
        QCOMPARE(followed.finalUrl, server.url("/feed"));

        QCOMPARE(fetcher.get({server.url("/file"), {}, {}}).error, QString("refused a redirect to a non-http(s) URL"));
        QCOMPARE(fetcher.get({server.url("/ftp"), {}, {}}).error, QString("refused a redirect to a non-http(s) URL"));
    }

    void fetcherKeepsValidatorsToTheirUrl()
    {
        FakeHttpServer server;
        server.redirect("/moved", "/target");
        server.route("/target", [](const FakeHttpServer::Request &) {
            return FakeHttpServer::Response{200, {{"ETag", "\"target\""}}, "moved body"};
        });
        Fetcher fetcher;
        const FetchResult result = fetcher.get({server.url("/moved"), "\"old\"", "Mon, 02 Jan 2006 15:04:05 GMT"});
        QVERIFY2(result.ok(), qPrintable(result.error));
        QCOMPARE(result.body, QByteArray("moved body"));
        QCOMPARE(result.url, server.url("/moved"));
        QCOMPARE(result.finalUrl, server.url("/target"));
        // The target's validators are its own, not the moved URL's.
        QVERIFY(result.etag.isEmpty());
        QCOMPARE(server.requests.first().headers.value("if-none-match"), QByteArray("\"old\""));
        int targets = 0;
        for (const auto &request : std::as_const(server.requests)) {
            if (request.path != "/target")
                continue;
            ++targets;
            QVERIFY(!request.headers.contains("if-none-match"));
            QVERIFY(!request.headers.contains("if-modified-since"));
        }
        QCOMPARE(targets, 1);
    }

    void fetcherBoundsRequestsInFlight()
    {
        FakeHttpServer server;
        QList<FetchRequest> requests;
        for (int i = 0; i < 20; ++i) {
            const QByteArray path = "/feed" + QByteArray::number(i);
            server.route(path, [path](const FakeHttpServer::Request &) {
                FakeHttpServer::Response response{200, {}, path};
                response.delayMs = 200;
                return response;
            });
            requests << FetchRequest{server.url(QString::fromLatin1(path)), {}, {}};
        }
        Fetcher fetcher;
        // Twenty answers two at a time take about two seconds, twice the
        // deadline, so this only passes if each deadline starts when its
        // request is sent.
        fetcher.setLimits({1024 * 1024, 1000, 5, 2});
        QList<qsizetype> order;
        fetcher.fetchEach(requests, [&](qsizetype index, const FetchResult &result) {
            order << index;
            QVERIFY2(result.ok(), qPrintable(result.error));
            QCOMPARE(result.body, "/feed" + QByteArray::number(index));
        });
        QCOMPARE(order.size(), 20);
        QCOMPARE(server.peakInFlight, 2);
        const auto all = fetcher.fetchAll(requests);
        QCOMPARE(all.size(), 20);
        QCOMPARE(all.at(19).body, QByteArray("/feed19"));
    }

    void fetcherRejectsOtherSchemes()
    {
        Fetcher fetcher;
        for (const char *url : {"file:///etc/passwd", "ftp://example.com/feed", "data:text/plain,hi", "/relative", "http://"})
            QCOMPARE(fetcher.get({QUrl(url), {}, {}}).error, QString("only http and https URLs can be fetched"));
    }

    void fetcherTimesOut()
    {
        FakeHttpServer server;
        server.route("/stall", [](const FakeHttpServer::Request &) {
            FakeHttpServer::Response response;
            response.stall = true;
            return response;
        });
        Fetcher fetcher;
        fetcher.setLimits({1024 * 1024, 300, 5});
        QElapsedTimer timer;
        timer.start();
        const FetchResult result = fetcher.get({server.url("/stall"), {}, {}});
        QCOMPARE(result.error, QString("timed out after 0.3 s"));
        QVERIFY(timer.elapsed() < 5000);
    }

    // Headless commands end to end

    void cliSubscribesRefreshesAndLists()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        server.route("/feed.xml", [](const FakeHttpServer::Request &request) {
            if (request.headers.value("if-none-match") == "\"rss-1\"")
                return FakeHttpServer::Response{304, {}, {}};
            return FakeHttpServer::Response{200, {{"Content-Type", "application/rss+xml"}, {"ETag", "\"rss-1\""}},
                                            fixture("rss2.xml")};
        });
        server.serve("/", "<head><link rel=\"alternate\" type=\"application/rss+xml\" href=\"/feed.xml\"></head>",
                     "text/html");
        server.serve("/atom.xml", fixture("atom.xml"), "application/atom+xml");

        QString out, err;
        QCOMPARE(run({"feeds", "--json"}, out, err), 0);
        QCOMPARE(out, QString("[]\n"));

        out.clear();
        QCOMPARE(run({"subscribe", server.url("/").toString(), "--json"}, out, err), 0);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QJsonObject subscribed = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(subscribed.value("url").toString(), server.url("/feed.xml").toString());
        QCOMPARE(subscribed.value("title").toString(), QString("Example & Friends"));
        QCOMPARE(subscribed.value("created").toBool(), true);
        QCOMPARE(subscribed.value("discovered").toBool(), true);
        QCOMPARE(subscribed.value("articles").toInt(), 4);
        const qint64 rssId = subscribed.value("id").toInteger();

        out.clear();
        QCOMPARE(run({"subscribe", server.url("/feed.xml").toString()}, out, err), 0);
        QVERIFY(out.startsWith("Already subscribed to Example & Friends"));
        out.clear();
        QCOMPARE(run({"subscribe", server.url("/atom.xml").toString()}, out, err), 0);
        QVERIFY(out.startsWith("Subscribed to Atom & Eve"));

        out.clear();
        QCOMPARE(run({"refresh", "--json"}, out, err), 0);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QJsonArray refreshed = QJsonDocument::fromJson(out.toUtf8()).array();
        QCOMPARE(refreshed.size(), 2);
        for (const QJsonValue &value : refreshed) {
            const QJsonObject result = value.toObject();
            const bool rss = result.value("id").toInteger() == rssId;
            QCOMPARE(result.value("status").toString(), QString(rss ? "not-modified" : "updated"));
            QCOMPARE(result.value("new").toInt(), 0);
            QVERIFY(result.value("error").isNull());
        }

        out.clear();
        QCOMPARE(run({"articles", "--feed", QString::number(rssId), "--json"}, out, err), 0);
        QJsonArray list = QJsonDocument::fromJson(out.toUtf8()).array();
        QCOMPARE(list.size(), 4);
        QJsonObject newest = list.at(0).toObject();
        const QStringList keys{"author", "feedId", "feedTitle", "guid", "id", "published", "read", "starred",
                               "summary", "title", "url"};
        QCOMPARE(newest.keys(), keys);
        const qint64 articleId = list.at(3).toObject().value("id").toInteger();
        QCOMPARE(list.at(3).toObject().value("published").toString(), QString("2006-01-02T15:04:05Z"));

        QCOMPARE(run({"read", QString::number(articleId)}, out, err), 0);
        QCOMPARE(run({"star", QString::number(articleId)}, out, err), 0);
        out.clear();
        QCOMPARE(run({"articles", "--starred", "--json"}, out, err), 0);
        list = QJsonDocument::fromJson(out.toUtf8()).array();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.at(0).toObject().value("read").toBool(), true);
        out.clear();
        QCOMPARE(run({"articles", "--unread", "--feed", QString::number(rssId), "--limit", "2", "--json"}, out, err), 0);
        list = QJsonDocument::fromJson(out.toUtf8()).array();
        QCOMPARE(list.size(), 2);
        for (const QJsonValue &value : list)
            QVERIFY(value.toObject().value("id").toInteger() != articleId);
        QCOMPARE(run({"unread", QString::number(articleId)}, out, err), 0);
        QCOMPARE(run({"unstar", QString::number(articleId)}, out, err), 0);

        out.clear();
        QCOMPARE(run({"feeds", "--json"}, out, err), 0);
        const QJsonArray feedList = QJsonDocument::fromJson(out.toUtf8()).array();
        QCOMPARE(feedList.size(), 2);
        const QStringList feedKeys{"category", "description", "id", "lastError", "lastFetchedAt", "siteUrl",
                                   "title", "total", "unread", "url"};
        QCOMPARE(feedList.at(0).toObject().keys(), feedKeys);

        QVERIFY(err.isEmpty());
        QCOMPARE(run({"read", "9999"}, out, err), 1);
        QVERIFY(err.contains("no article with id 9999"));
        err.clear();
        QCOMPARE(run({"articles", "--limit", "x"}, out, err), 1);
        QCOMPARE(run({"articles", "--feed", "0"}, out, err), 1);
        QCOMPARE(run({"subscribe", "file:///etc/passwd"}, out, err), 1);
        QCOMPARE(run({"refresh", "--feed", "9999"}, out, err), 1);
        QVERIFY(err.contains("no feed with id 9999"));
    }

    void cliReportsFailedRefreshes()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        server.serve("/feed.xml", fixture("rss2.xml"));
        QString out, err;
        QCOMPARE(run({"subscribe", server.url("/feed.xml").toString()}, out, err), 0);
        server.serve("/feed.xml", fixture("malformed.xml"));

        out.clear();
        QCOMPARE(run({"refresh", "--json"}, out, err), 1);
        const QJsonObject result = QJsonDocument::fromJson(out.toUtf8()).array().at(0).toObject();
        QCOMPARE(result.value("status").toString(), QString("error"));
        QVERIFY(result.value("error").toString().startsWith("malformed feed"));
        QVERIFY(err.contains("malformed feed"));

        out.clear();
        QCOMPARE(run({"feeds"}, out, err), 0);
        QVERIFY(out.contains("(error: malformed feed"));
        QCOMPARE(run({"unsubscribe", "1"}, out, err), 0);
        out.clear();
        QCOMPARE(run({"feeds", "--json"}, out, err), 0);
        QCOMPARE(out, QString("[]\n"));
    }

    // A subscription changed from the command line goes out when the window
    // next starts.
    void cliMarksSubscriptionChangesToPublish()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", (dir.path() + "/home").toUtf8());
        FakeHttpServer server;
        server.serve("/feed.xml", fixture("rss2.xml"));
        write(dir.path() + "/in.opml", fixture("import.opml"));
        QString out, err;
        QCOMPARE(run({"subscribe", server.url("/feed.xml").toString()}, out, err), 0);
        seedSyncAccount();
        const int bit = publishBit(Sync::subscriptionsKind);
        const auto pending = [bit] {
            Store store;
            return store.open() && (store.syncAccount().publishPending & bit);
        };
        const auto clear = [bit] {
            Store store;
            return store.open() && store.markPublishPending(syncPubkey(), bit, false);
        };
        QCOMPARE(run({"subscribe", server.url("/feed.xml").toString()}, out, err), 0);
        QVERIFY(!pending());
        QCOMPARE(run({"import", dir.path() + "/in.opml"}, out, err), 0);
        QVERIFY(pending());
        QVERIFY(clear());
        QCOMPARE(run({"import", dir.path() + "/in.opml"}, out, err), 0);
        QVERIFY(!pending());
        QCOMPARE(run({"unsubscribe", "1"}, out, err), 0);
        QVERIFY(pending());
        QVERIFY(clear());
        QCOMPARE(run({"subscribe", server.url("/feed.xml").toString()}, out, err), 0);
        QVERIFY(pending());
        QVERIFY2(err.isEmpty(), qPrintable(err));
    }

    void cliExportsAndImportsOpml()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", (dir.path() + "/one").toUtf8());
        write(dir.path() + "/in.opml", fixture("import.opml"));

        QString out, err;
        QCOMPARE(run({"import", dir.path() + "/in.opml", "--json"}, out, err), 0);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object(),
                 QJsonObject({{"added", 2}, {"skipped", 1}, {"unsupported", 2}}));
        out.clear();
        QCOMPARE(run({"import", dir.path() + "/in.opml"}, out, err), 0);
        QVERIFY(out.startsWith("Imported 0 feeds (3 already subscribed"));

        out.clear();
        QCOMPARE(run({"export"}, out, err), 0);
        QVERIFY(out.contains("<opml version=\"2.0\">"));
        QVERIFY(out.contains("text=\"News &amp; Views\""));
        QCOMPARE(run({"export", dir.path() + "/out.opml"}, out, err), 0);
        QCOMPARE(QString::fromUtf8(fixtureAt(dir.path() + "/out.opml")), out);

        qputenv("READSTR_HOME", (dir.path() + "/two").toUtf8());
        out.clear();
        QCOMPARE(run({"import", dir.path() + "/out.opml", "--json"}, out, err), 0);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value("added").toInt(), 2);
        out.clear();
        QCOMPARE(run({"feeds", "--json"}, out, err), 0);
        const QJsonArray feeds = QJsonDocument::fromJson(out.toUtf8()).array();
        QCOMPARE(feeds.size(), 2);
        QCOMPARE(feeds.at(0).toObject().value("title").toString(), QString("Daily"));
        QCOMPARE(feeds.at(0).toObject().value("category").toString(), QString("News & Views"));
        QVERIFY(feeds.at(1).toObject().value("category").isNull());

        QCOMPARE(run({"import", dir.path() + "/missing.opml"}, out, err), 1);
        QVERIFY(err.contains("cannot read"));
        QVERIFY(mkfifo(QFile::encodeName(dir.path() + "/fifo.opml").constData(), 0600) == 0);
        QCOMPARE(run({"import", dir.path() + "/fifo.opml"}, out, err), 1);
    }

    void cliImportCountsUnsupportedBeforeDeduplicating()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        write(dir.path() + "/in.opml", "<opml><body><outline xmlUrl=\"ftp://a.example.com/feed\"/>"
                                       "<outline xmlUrl=\"https://a.example.com/feed\"/></body></opml>");
        QString out, err;
        QCOMPARE(run({"import", dir.path() + "/in.opml", "--json"}, out, err), 0);
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object(),
                 QJsonObject({{"added", 1}, {"skipped", 0}, {"unsupported", 1}}));
    }

    void cliKeepsTheTerminalSafe()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        write(dir.path() + "/in.opml", "<opml><body><outline text=\"Fold&#x202e;er&#x9b;2J\">"
                                       "<outline text=\"Evil&#x9b;31m&#x2066;txt\" xmlUrl=\"https://a.example.com/feed\"/>"
                                       "</outline></body></opml>");
        QString out, err;
        QCOMPARE(run({"import", dir.path() + "/in.opml"}, out, err), 0);
        out.clear();
        QCOMPARE(run({"feeds"}, out, err), 0);
        QVERIFY2(out.contains("Evil 31mtxt") && out.contains("[Folder 2J]"), qPrintable(out));
        for (const char16_t unsafe : {u'\x9b', u'\x202e', u'\x2066'})
            QVERIFY(!out.contains(QChar(unsafe)));

        // JSON is for programs, which get the text as it is.
        out.clear();
        QCOMPARE(run({"feeds", "--json"}, out, err), 0);
        const QJsonObject feed = QJsonDocument::fromJson(out.toUtf8()).array().at(0).toObject();
        QCOMPARE(feed.value("title").toString(), QString("Evil\u009b31m\u2066txt"));
    }

    void cliExportsPrivately()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        write(dir.path() + "/in.opml", fixture("import.opml"));
        QString out, err;
        QCOMPARE(run({"import", dir.path() + "/in.opml"}, out, err), 0);
        QCOMPARE(run({"export", dir.path() + "/new.opml"}, out, err), 0);
        QCOMPARE(mode(dir.path() + "/new.opml"), 0600);
        write(dir.path() + "/old.opml", "old");
        QVERIFY(QFile::setPermissions(dir.path() + "/old.opml", QFile::Permissions(0x6644)));
        QCOMPARE(run({"export", dir.path() + "/old.opml"}, out, err), 0);
        QCOMPARE(mode(dir.path() + "/old.opml"), 0600);
        QVERIFY(fixtureAt(dir.path() + "/old.opml").contains("daily.example.com"));
    }

    void cliSubscribeLeavesNothingBehindOnFailure()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        server.serve("/feed.xml", fixture("rss2.xml"));
        QString out, err;
        QCOMPARE(run({"feeds"}, out, err), 0);

        const auto exec = [&](const QString &sql) {
            QSqlDatabase raw = QSqlDatabase::addDatabase("QSQLITE", "raw");
            raw.setDatabaseName(Paths::databaseFile());
            bool done = raw.open() && QSqlQuery(raw).exec(sql);
            raw.close();
            raw = QSqlDatabase();
            QSqlDatabase::removeDatabase("raw");
            return done;
        };
        QVERIFY(exec("CREATE TRIGGER no_items BEFORE INSERT ON items BEGIN SELECT RAISE(ABORT, 'no items'); END"));
        QCOMPARE(run({"subscribe", server.url("/feed.xml").toString()}, out, err), 1);
        QVERIFY2(err.contains("no items"), qPrintable(err));
        out.clear();
        QCOMPARE(run({"feeds", "--json"}, out, err), 0);
        QCOMPARE(out, QString("[]\n"));

        QVERIFY(exec("DROP TRIGGER no_items"));
        out.clear();
        QCOMPARE(run({"subscribe", server.url("/feed.xml").toString(), "--json"}, out, err), 0);
        const QJsonObject subscribed = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(subscribed.value("created").toBool(), true);
        QCOMPARE(subscribed.value("articles").toInt(), 4);
    }

    // Hyprland's event socket

    void themeFollowsHyprlandEventSocket()
    {
        // The fake hyprctl lives outside the watched theme directory, so
        // changing its answer cannot trigger a theme reload (which asks for
        // the rounding too) and only the event socket can.
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("hypr") && QDir(dir.path()).mkpath("current"));
        const QString response = dir.filePath("hypr/response.json"), command = dir.filePath("hypr/hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":3}");
        QLocalServer server;
        QVERIFY(server.listen(dir.filePath("hypr/socket2.sock")));

        Theme theme(dir.filePath("current"), nullptr, command, server.fullServerName());
        QTRY_COMPARE(theme.radius(), 3);
        QVERIFY(server.waitForNewConnection(5000) || server.hasPendingConnections());
        QLocalSocket *events = server.nextPendingConnection();
        QVERIFY(events);
        QTRY_VERIFY(theme.listening());

        // Nothing polls while the socket is up: a changed value waits for
        // Hyprland's word.
        write(response, "{\"int\":6}");
        QTest::qWait(1500);
        QCOMPARE(theme.radius(), 3);
        events->write("activewindow>>kitty,~\nconfigreloaded>>\n");
        events->flush();
        QTRY_COMPARE(theme.radius(), 6);

        // Without the socket it goes back to asking every second.
        events->disconnectFromServer();
        write(response, "{\"int\":9}");
        QTRY_COMPARE_WITH_TIMEOUT(theme.radius(), 9, 10000);
        QVERIFY(!theme.listening());
    }

    void themePollsWithoutAnEventSocket()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":2}");
        Theme theme(dir.path(), nullptr, command, dir.filePath("missing.sock"));
        QTRY_COMPARE(theme.radius(), 2);
        write(response, "{\"int\":5}");
        QTRY_COMPARE_WITH_TIMEOUT(theme.radius(), 5, 10000);
    }

    // The catalogs translate every string the window and its C++ show, so a
    // new string cannot ship in English only.
    void catalogsCoverEveryString_data()
    {
        QTest::addColumn<QString>("language");
        for (const char *language : {"da", "pt", "pt-BR", "ru"})
            QTest::newRow(language) << language;
    }

    void catalogsCoverEveryString()
    {
        QFETCH(QString, language);
        QTemporaryDir dir;
        write(dir.path() + "/language", language.toUtf8());
        Translator translator(dir.path() + "/language");

        static const QRegularExpression qmlString(QStringLiteral("qsTr\\(\"((?:[^\"\\\\]|\\\\.)*)\""));
        static const QRegularExpression cppString(
            QStringLiteral("(?:\\btr|translate)\\((?:\"[A-Za-z]+\",\\s*)?\"((?:[^\"\\\\]|\\\\.)*)\""));
        QStringList strings;
        QDirIterator qml(":/", {"*.qml"});
        while (qml.hasNext()) {
            QFile file(qml.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            for (const auto &match : qmlString.globalMatch(QString::fromUtf8(file.readAll())))
                strings << match.captured(1);
        }
        const QString source = QFileInfo(QFINDTESTDATA("../src/backend.cpp")).absolutePath();
        for (const char *name :
             {"backend.cpp", "keystore.cpp", "sanitizer.cpp", "portalfilepicker.cpp", "remotesigner.cpp", "signer.cpp",
              "sync.cpp"}) {
            QFile file(source + "/" + name);
            QVERIFY(file.open(QIODevice::ReadOnly));
            for (const auto &match : cppString.globalMatch(QString::fromUtf8(file.readAll())))
                strings << match.captured(1);
        }
        strings.removeDuplicates();
        QVERIFY(strings.size() > 60);
        QStringList missing;
        for (const QString &text : strings) {
            const QByteArray key = text.toUtf8();
            if (translator.translate("Main", key.constData(), nullptr, text.contains("%n") ? 2 : -1).isNull())
                missing << text;
        }
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join(" | ")));

        // English carries the plural forms of every counted string.
        QTemporaryDir englishDir;
        write(englishDir.path() + "/language", "en");
        Translator english(englishDir.path() + "/language");
        for (const QString &text : strings)
            if (text.contains("%n"))
                QVERIFY2(!english.translate("Main", text.toUtf8().constData(), nullptr, 1).isNull(), qPrintable(text));
    }

    // The ? overlay lists every shortcut of the window, so a new Shortcut
    // cannot ship without telling the user about it. The window is loaded
    // for real rather than its source parsed, so a Shortcut in any file the
    // window uses is found, however it is written. Shortcuts inside a
    // dialog, such as Enter for its default button, belong to that dialog
    // and are not listed.
    void shortcutsOverlayListsEveryShortcut()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        write(dir.path() + "/language", "en");
        QTest::failOnWarning(QRegularExpression("Binding loop|qrc:/"));
        LoadedWindow loaded(dir.path() + "/language");
        QVERIFY(loaded.window);

        const auto portable = [](const QKeySequence &sequence) {
            return sequence.toString(QKeySequence::PortableText);
        };
        QSet<QString> listed;
        const QVariantList shortcuts = loaded.window->property("shortcuts").toList();
        QVERIFY(shortcuts.size() > 10);
        for (const QVariant &shortcut : shortcuts)
            for (const QString &key : shortcut.toMap().value("keys").toString().split(", "))
                listed << portable(QKeySequence(key, QKeySequence::PortableText));

        QSet<QString> bound;
        QStringList missing;
        int found = 0;
        for (QObject *object : loaded.window->findChildren<QObject *>()) {
            if (!object->inherits("QQuickShortcut"))
                continue;
            bool inDialog = false;
            for (QObject *parent = object->parent(); parent && !inDialog; parent = parent->parent())
                inDialog = parent->inherits("QQuickPopup");
            if (inDialog)
                continue;
            ++found;
            // A Shortcut has keys in `sequence` or in `sequences`, which
            // holds a standard key once for every key the platform binds it to.
            QVariantList values = object->property("sequences").toList();
            if (values.isEmpty())
                values << object->property("sequence");
            QStringList texts;
            for (const QVariant &value : values) {
                // The overlay lists a standard key's first binding, the one
                // users know.
                texts << (value.typeId() == QMetaType::QString
                              ? portable(QKeySequence(value.toString(), QKeySequence::PortableText))
                              : portable(QKeySequence::keyBindings(QKeySequence::StandardKey(value.toInt())).value(0)));
            }
            texts.removeDuplicates();
            for (const QString &text : texts) {
                bound << text;
                if (text.isEmpty() || !listed.contains(text))
                    missing << text;
            }
        }
        QVERIFY(found > 20);
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join(" | ")));
        // Nor does the overlay list a key that no Shortcut handles.
        QStringList unbound;
        for (const QString &key : std::as_const(listed))
            if (!bound.contains(key))
                unbound << key;
        unbound.sort();
        QVERIFY2(unbound.isEmpty(), qPrintable(unbound.join(" | ")));
    }

    // The menus are as wide as their widest item, so no label is elided in
    // English or in the longer Russian.
    void menusFitTheirLongestItem()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        write(dir.path() + "/language", "en");
        QTest::failOnWarning(QRegularExpression("Binding loop|qrc:/"));
        LoadedWindow loaded(dir.path() + "/language");
        QVERIFY(loaded.window);
        QVERIFY(QTest::qWaitForWindowExposed(loaded.window));

        for (const char *language : {"en", "ru"}) {
            write(dir.path() + "/language", language);
            QTRY_COMPARE(loaded.translator->language(), QString(language));
            const QString markAll = QCoreApplication::translate("Main", "Mark everything in the view read");
            bool sawMarkAll = false;
            for (const char *name : {"appMenu", "feedMenu"}) {
                QObject *menu = loaded.window->findChild<QObject *>(name);
                QVERIFY2(menu, name);
                QVERIFY(QMetaObject::invokeMethod(menu, "open"));
                QTRY_VERIFY(menu->property("opened").toBool());
                const int count = menu->property("count").toInt();
                QVERIFY(count >= 4);
                for (int i = 0; i < count; ++i) {
                    QQuickItem *item = nullptr;
                    QVERIFY(QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, i)));
                    QVERIFY(item);
                    const QString text = item->property("text").toString();
                    sawMarkAll = sawMarkAll || text == markAll;
                    QVERIFY2(item->width() >= item->implicitWidth(), qPrintable(QString("%1 %2: %3 < %4")
                                                                                     .arg(language, text)
                                                                                     .arg(item->width())
                                                                                     .arg(item->implicitWidth())));
                }
                QVERIFY(QMetaObject::invokeMethod(menu, "close"));
                QTRY_VERIFY(!menu->property("visible").toBool());
            }
            QVERIFY2(sawMarkAll, qPrintable(markAll));
        }
    }

    // NIP-44 v2

    void nip44DerivesConversationAndMessageKeys()
    {
        const QJsonObject valid = nip44Vectors().value("valid").toObject();
        const QJsonArray conversations = valid.value("get_conversation_key").toArray();
        QCOMPARE(conversations.size(), 35);
        for (const QJsonValue &value : conversations) {
            const QJsonObject vector = value.toObject();
            QCOMPARE(Nip44::conversationKey(hex(vector.value("sec1")), hex(vector.value("pub2"))).toHex(),
                     vector.value("conversation_key").toString().toLatin1());
        }
        const QJsonObject messages = valid.value("get_message_keys").toObject();
        const QByteArray conversationKey = hex(messages.value("conversation_key"));
        const QJsonArray keys = messages.value("keys").toArray();
        QCOMPARE(keys.size(), 32);
        for (const QJsonValue &value : keys) {
            const QJsonObject vector = value.toObject();
            const auto derived = Nip44::messageKeys(conversationKey, hex(vector.value("nonce")));
            QVERIFY(derived);
            QCOMPARE(derived->chachaKey, hex(vector.value("chacha_key")));
            QCOMPARE(derived->chachaNonce, hex(vector.value("chacha_nonce")));
            QCOMPARE(derived->hmacKey, hex(vector.value("hmac_key")));
        }
        const QJsonArray lengths = valid.value("calc_padded_len").toArray();
        QCOMPARE(lengths.size(), 24);
        for (const QJsonValue &pair : lengths)
            QCOMPARE(Nip44::paddedLength(pair.toArray().at(0).toInteger()), pair.toArray().at(1).toInteger());

        const QJsonArray invalid = nip44Vectors().value("invalid").toObject().value("get_conversation_key").toArray();
        QCOMPARE(invalid.size(), 8);
        for (const QJsonValue &value : invalid) {
            const QJsonObject vector = value.toObject();
            QVERIFY2(Nip44::conversationKey(hex(vector.value("sec1")), hex(vector.value("pub2"))).isEmpty(),
                     qPrintable(vector.value("note").toString()));
        }
    }

    void nip44EncryptsAndDecryptsAsTheVectors()
    {
        const QJsonObject valid = nip44Vectors().value("valid").toObject();
        const QJsonArray vectors = valid.value("encrypt_decrypt").toArray();
        QCOMPARE(vectors.size(), 10);
        for (const QJsonValue &value : vectors) {
            const QJsonObject vector = value.toObject();
            const QByteArray sec1 = hex(vector.value("sec1")), sec2 = hex(vector.value("sec2"));
            const QByteArray key = hex(vector.value("conversation_key"));
            // The same key from either side.
            QCOMPARE(Nip44::conversationKey(sec1, QByteArray::fromHex(LocalSigner::create(sec2)->pubkey().toLatin1())),
                     key);
            QCOMPARE(Nip44::conversationKey(sec2, QByteArray::fromHex(LocalSigner::create(sec1)->pubkey().toLatin1())),
                     key);
            const QByteArray plaintext = vector.value("plaintext").toString().toUtf8();
            const QString payload = vector.value("payload").toString();
            QCOMPARE(Nip44::encrypt(plaintext, key, hex(vector.value("nonce"))), std::optional(payload));
            QCOMPARE(Nip44::decrypt(payload, key), std::optional(plaintext));
        }
        const QJsonArray longMessages = valid.value("encrypt_decrypt_long_msg").toArray();
        QCOMPARE(longMessages.size(), 3);
        for (const QJsonValue &value : longMessages) {
            const QJsonObject vector = value.toObject();
            const QByteArray plaintext = vector.value("pattern").toString().toUtf8().repeated(vector.value("repeat").toInt());
            const auto sha256 = [](const QByteArray &data) {
                return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
            };
            QCOMPARE(sha256(plaintext), vector.value("plaintext_sha256").toString().toLatin1());
            const QByteArray key = hex(vector.value("conversation_key"));
            const auto payload = Nip44::encrypt(plaintext, key, hex(vector.value("nonce")));
            QVERIFY(payload);
            QCOMPARE(sha256(payload->toLatin1()), vector.value("payload_sha256").toString().toLatin1());
            QCOMPARE(Nip44::decrypt(*payload, key), std::optional(plaintext));
        }

        // A fresh nonce each time: the same message never encrypts the same.
        const QByteArray key = hex(vectors.first().toObject().value("conversation_key"));
        const auto first = Nip44::encrypt("hello", key), second = Nip44::encrypt("hello", key);
        QVERIFY(first && second && *first != *second);
        QCOMPARE(Nip44::decrypt(*first, key), std::optional(QByteArray("hello")));
        QCOMPARE(Nip44::decrypt(*second, key), std::optional(QByteArray("hello")));
    }

    void nip44RefusesWhatTheVectorsRefuse()
    {
        const QJsonObject invalid = nip44Vectors().value("invalid").toObject();
        const QByteArray key(32, '\1');
        const QJsonArray lengths = invalid.value("encrypt_msg_lengths").toArray();
        QCOMPARE(lengths.size(), 4);
        for (const QJsonValue &length : lengths)
            QVERIFY(!Nip44::encrypt(QByteArray(length.toInteger(), 'a'), key));
        const QJsonArray payloads = invalid.value("decrypt").toArray();
        QCOMPARE(payloads.size(), 12);
        for (const QJsonValue &value : payloads) {
            const QJsonObject vector = value.toObject();
            QVERIFY2(!Nip44::decrypt(vector.value("payload").toString(), hex(vector.value("conversation_key"))),
                     qPrintable(vector.value("note").toString()));
        }
        // A key of the wrong size encrypts nothing.
        QVERIFY(!Nip44::encrypt("hello", QByteArray(31, '\1')));
        QVERIFY(!Nip44::conversationKey(QByteArray(31, '\1'), QByteArray(32, '\2')).size());
    }

    // NIP-46 remote signer

    void remoteSignerParsesBunkerUris()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        const QString pk = bunker.pubkey();
        auto pairing = RemoteSigner::parseBunkerUri("  " + bunker.uri(relay.url(), "s3cret") + " ", {relay.url()});
        QVERIFY(pairing);
        QCOMPARE(pairing->signerPubkey, pk);
        QCOMPARE(pairing->relays, QStringList{relay.url()});
        QCOMPARE(pairing->secret, QString("s3cret"));
        QCOMPARE(pairing->clientSecret.size(), 32);
        // A fresh client key every time.
        auto again = RemoteSigner::parseBunkerUri(bunker.uri(relay.url()), {relay.url()});
        QVERIFY(again && again->clientSecret != pairing->clientSecret);
        QVERIFY(again->secret.isEmpty());

        // A loopback relay is only taken when the user configured it, and
        // the URI's other relays only when public, over wss, and not onion
        // services; each once, and no more than maxRelays.
        QVERIFY(!RemoteSigner::parseBunkerUri(bunker.uri(relay.url()), {}));
        auto publicOnes = RemoteSigner::parseBunkerUri(
            "bunker://" + pk + "?relay=wss%3A%2F%2Frelay.one.invalid&relay=wss://relay.one.invalid/"
                + "&relay=wss://hidden.onion&relay=wss://192.168.1.2&relay=ws://relay.two.invalid"
                + "&relay=wss://localhost&relay=wss://user@relay.three.invalid&relay=wss://relay.four.invalid",
            {});
        QVERIFY(publicOnes);
        QCOMPARE(publicOnes->relays, QStringList({"wss://relay.one.invalid", "wss://relay.four.invalid"}));
        QString many = "bunker://" + pk + "?";
        for (int i = 0; i < 6; ++i)
            many += QString("relay=wss://r%1.invalid&").arg(i);
        QCOMPARE(RemoteSigner::parseBunkerUri(many, {})->relays.size(), RemoteSigner::maxRelays);

        for (const QString &bad :
             {QString("bunker://%1").arg(pk), QString("bunker://%1?relay=wss://192.168.1.2").arg(pk),
              QString("bunker://abc?relay=wss://r.invalid"),
              QString("bunker://%1?relay=wss://r.invalid").arg(QString(64, 'f')),
              QString("bunker://%1?relay=wss://r.invalid&secret=a%20b").arg(pk),
              QString("bunker://%1?relay=wss://r.invalid&secret=%2").arg(pk, QString(300, 'x')),
              QString("bunker://%1?relay=wss://r.invalid&x=%2").arg(pk, QString(2000, 'x')),
              QString("bunker://%1:1234?relay=wss://r.invalid").arg(pk),
              QString("bunker://%1/path?relay=wss://r.invalid").arg(pk),
              QString("nostrconnect://%1?relay=wss://r.invalid").arg(pk), syncNpub(), QString()})
            QVERIFY2(!RemoteSigner::parseBunkerUri(bad, {}), qPrintable(bad.left(120)));

        // As the keyring keeps it, and back, the client key first, written
        // and read by hand.
        QByteArray json = pairing->toJson();
        QVERIFY(json.startsWith("{\"client\":\"" + pairing->clientSecret.toHex() + "\","));
        QCOMPARE(QJsonDocument::fromJson(json).object().keys(), QStringList({"client", "relays", "secret", "signer"}));
        auto restored = RemoteSigner::Pairing::fromJson(json);
        QVERIFY(restored);
        QCOMPARE(restored->clientSecret, pairing->clientSecret);
        QCOMPARE(restored->signerPubkey, pk);
        QCOMPARE(restored->relays, QStringList{relay.url()});
        QCOMPARE(restored->secret, QString("s3cret"));
        for (const QByteArray &bad :
             {QByteArray("not json"), QByteArray("{}"), json.replace(relay.url().toUtf8(), "http://x.invalid"),
              pairing->toJson().replace(pk.toUtf8(), QByteArray(64, 'f')),
              pairing->toJson().replace(pairing->clientSecret.toHex(), QByteArray(64, '0'))})
            QVERIFY2(!RemoteSigner::Pairing::fromJson(bad), bad.constData());
        restored->wipe();
        pairing->wipe();
        QVERIFY(pairing->clientSecret.isEmpty() && pairing->secret.isEmpty());
    }

    // Connecting with the URI's secret, then the account's public key, and
    // signing: each answer checked, the event the account's.
    void remoteSignerConnectsAndSigns()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        bunker.secret = "s3cret";
        auto pairing = RemoteSigner::parseBunkerUri(bunker.uri(relay.url(), "s3cret"), {relay.url()});
        auto signer = RemoteSigner::create(*pairing, syncPubkey());
        QVERIFY(signer);
        signer->setLimits(quickRemoteLimits());
        QCOMPARE(signer->pubkey(), syncPubkey());
        std::optional<QString> paired;
        signer->pair([&](const QString &error) { paired = error; });
        QTRY_VERIFY(paired);
        QCOMPARE(*paired, QString());
        QCOMPARE(bunker.methods, QStringList({"connect", "get_public_key", "sign_event"}));
        QCOMPARE(bunker.params.first(), (QJsonArray{bunker.pubkey(), "s3cret", RemoteSigner::permissions}));
        // The pairing check: a throwaway event of its own kind, with
        // nothing in it but a fresh token, which is never published.
        const QJsonObject check = QJsonDocument::fromJson(bunker.params.last().at(0).toString().toUtf8()).object();
        QCOMPARE(check.value("kind").toInt(), RemoteSigner::pairingCheckKind);
        QVERIFY(RemoteSigner::pairingCheckKind >= 20000 && RemoteSigner::pairingCheckKind < 30000);
        QVERIFY(check.value("content").toString().startsWith("Readstr pairing check "));
        QCOMPARE(check.value("tags").toArray(), QJsonArray());
        QVERIFY(qAbs(check.value("created_at").toInteger() - QDateTime::currentSecsSinceEpoch()) < 60);
        QCOMPARE(signer->waiting(), 0);
        // Every request came from the client key, never the account's.
        for (const QJsonObject &event : std::as_const(relay.published)) {
            QCOMPARE(event.value("kind").toInt(), 24133);
            QCOMPARE(event.value("pubkey").toString(), signer->clientPubkey());
            QVERIFY(!event.value("content").toString().contains("get_public_key"));
        }

        Signed result;
        signer->sign(signingDraft(), result.done());
        QCOMPARE(result.answers, 0);
        QCOMPARE(signer->waiting(), 1);
        QTRY_COMPARE(result.answers, 1);
        QCOMPARE(result.error, QString());
        QVERIFY(result.event.verify());
        QCOMPARE(result.event.pubkey, syncPubkey());
        QCOMPARE(result.event.content, signingDraft().content);
        QCOMPARE(result.event.tags, signingDraft().tags);
        const QJsonObject draft = QJsonDocument::fromJson(bunker.params.last().at(0).toString().toUtf8()).object();
        QCOMPARE(draft.keys(), QStringList({"content", "created_at", "kind", "tags"}));

        std::optional<QString> pinged;
        signer->ping([&](const QString &error) { pinged = error; });
        QTRY_VERIFY(pinged);
        QCOMPARE(*pinged, QString());

        // A wrong secret is refused by the signer, and said.
        auto wrong = RemoteSigner::parseBunkerUri(bunker.uri(relay.url(), "guess"), {relay.url()});
        auto refused = RemoteSigner::create(*wrong, syncPubkey());
        std::optional<QString> refusal;
        refused->pair([&](const QString &error) { refusal = error; });
        QTRY_VERIFY(refusal);
        QCOMPARE(*refusal, QString("The signer refused to connect: invalid secret"));
    }

    void remoteSignerRefusesAnotherAccount()
    {
        FakeRelay relay;
        FakeBunker bunker(relay, 5, 2);
        auto signer = pairedSigner(bunker, relay);
        std::optional<QString> paired;
        signer->pair([&](const QString &error) { paired = error; });
        QTRY_VERIFY(paired);
        const QString theirs = Nip19::npub(bunker.userPubkey());
        QCOMPARE(*paired, "The signer holds the key of " + Nip19::shortNpub(theirs, theirs) + ", not the one for "
                     + Nip19::shortNpub(syncNpub(), syncNpub()));
    }

    // A signed event is only taken when it is the one asked for, signed by
    // the account, with a signature that checks out, and once.
    void remoteSignerChecksWhatComesBack()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        auto signer = pairedSigner(bunker, relay);
        const QString shortNpub = Nip19::shortNpub(syncNpub(), syncNpub());
        for (const auto &[mode, expected] :
             {std::pair(FakeBunker::Sign::BadSignature, QString("The signer's signature does not check out")),
              std::pair(FakeBunker::Sign::OtherKey, "The signer signed with another key than the one for " + shortNpub),
              std::pair(FakeBunker::Sign::OtherEvent, QString("The signer sent back another event than the one asked for")),
              std::pair(FakeBunker::Sign::Refuse, QString("The signer refused to sign: user rejected"))}) {
            bunker.sign = mode;
            Signed result;
            signer->sign(signingDraft(), result.done());
            QTRY_COMPARE(result.answers, 1);
            QCOMPARE(result.error, expected);
            QVERIFY(result.event.id.isEmpty());
        }
        bunker.sign = FakeBunker::Sign::Twice;
        Signed twice;
        signer->sign(signingDraft(), twice.done());
        QTRY_COMPARE(twice.answers, 1);
        QTest::qWait(200);
        QCOMPARE(twice.answers, 1);
        QVERIFY(twice.error.isEmpty());

        // What is out of range never reaches the signer.
        const qsizetype asked = bunker.methods.size();
        NostrEvent huge = signingDraft();
        huge.content = QString(70000, 'x');
        Signed tooLarge;
        signer->sign(huge, tooLarge.done());
        QCOMPARE(tooLarge.answers, 0);
        QTRY_COMPARE(tooLarge.answers, 1);
        QCOMPARE(tooLarge.error, QString("This event is too large for a remote signer to sign"));
        NostrEvent bad = signingDraft();
        bad.kind = 70000;
        Signed outOfRange;
        signer->sign(bad, outOfRange.done());
        QTRY_COMPARE(outOfRange.answers, 1);
        QCOMPARE(outOfRange.error, QString("Cannot sign this event: its kind or date is out of range"));
        QCOMPARE(bunker.methods.size(), asked);
    }

    // Answers with unknown ids, from other keys, that do not decrypt or
    // parse, or that are too large are passed over, and the real one still
    // counts, even after a relay dropped the connection over one.
    void remoteSignerIgnoresStrayAnswers()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        auto signer = pairedSigner(bunker, relay);
        bunker.hold = true;
        Signed result;
        signer->sign(signingDraft(), result.done());
        QTRY_COMPARE(bunker.held.size(), 1);
        const FakeBunker::Held request = bunker.held.first();
        QCOMPARE(request.client, signer->clientPubkey());
        bunker.respond(request.client, "unknown-id", "{}");
        bunker.respond(request.client, request.id, "{}", {}, QByteArray(32, '\6'));
        bunker.respondRaw(request.client, {}, {}, QString("not a payload"));
        bunker.respondRaw(request.client, "not json");
        bunker.respondRaw(request.client, QJsonDocument(QJsonObject{{"id", request.id}, {"result", 7}}).toJson());
        bunker.respondRaw(request.client, QJsonDocument(QJsonObject{{"id", QString(200, 'x')}}).toJson());
        relay.pushRaw("[\"EVENT\",\"other\",{}]");
        QTest::qWait(200);
        QCOMPARE(result.answers, 0);
        QCOMPARE(signer->waiting(), 1);

        // A message past the cap closes the connection, which is opened
        // again while an answer is awaited.
        QCOMPARE(relay.connections, 1);
        relay.pushRaw("[\"NOTICE\",\"" + QString(300 * 1024, 'x') + "\"]");
        QTRY_COMPARE(relay.connections, 2);
        QTRY_COMPARE(relay.requests.size(), 2);
        bunker.answerHeld();
        QTRY_COMPARE(result.answers, 1);
        QCOMPARE(result.error, QString());
        QVERIFY(result.event.verify());
        QCOMPARE(signer->waiting(), 0);
    }

    void remoteSignerTimesOut()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        bunker.hold = true;
        auto signer = pairedSigner(bunker, relay);
        RemoteSigner::Limits limits = quickRemoteLimits();
        limits.requestTimeoutMs = 300;
        signer->setLimits(limits);
        Signed result;
        signer->sign(signingDraft(), result.done());
        QTRY_COMPARE(result.answers, 1);
        QCOMPARE(result.error, QString("The signer did not answer in time"));
        QCOMPARE(signer->waiting(), 0);
        // An answer after the timeout is no answer.
        bunker.answerHeld();
        QTest::qWait(100);
        QCOMPARE(result.answers, 1);

        // No relay to reach: said as such.
        RemoteSigner::Pairing pairing;
        pairing.clientSecret = QByteArray(32, '\7');
        pairing.signerPubkey = bunker.pubkey();
        pairing.relays = {"ws://127.0.0.1:1"};
        auto unreachable = RemoteSigner::create(pairing, syncPubkey());
        limits.connectTimeoutMs = 200;
        unreachable->setLimits(limits);
        Signed lost;
        unreachable->sign(signingDraft(), lost.done());
        QTRY_COMPARE(lost.answers, 1);
        QCOMPARE(lost.error, QString("Cannot reach the relays of the remote signer"));

        // Gone, it answers nothing.
        Signed dropped;
        bunker.hold = true;
        signer->sign(signingDraft(), dropped.done());
        QTRY_COMPARE(bunker.held.size(), 1);
        signer.reset();
        bunker.answerHeld();
        QTest::qWait(400);
        QCOMPARE(dropped.answers, 0);
    }

    // An auth_url answer is shown, never opened, and the real answer that
    // follows it is still taken.
    void remoteSignerSurfacesAuthUrls()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        auto signer = pairedSigner(bunker, relay);
        QSignalSpy urls(signer.get(), &RemoteSigner::authUrl);
        bunker.authUrl = "https://signer.example.invalid/approve?id=1";
        Signed result;
        signer->sign(signingDraft(), result.done());
        QTRY_COMPARE(urls.size(), 1);
        QCOMPARE(urls.first().first().toString(), bunker.authUrl);
        QCOMPARE(result.answers, 0);
        QTRY_COMPARE(result.answers, 1);
        QCOMPARE(result.error, QString());
        QVERIFY(result.event.verify());

        // Not a web page, or one over plain http: passed over, and the
        // request goes on waiting.
        for (const QString &url : {QString("file:///etc/passwd"), QString("http://signer.example.invalid/approve")}) {
            bunker.authUrl = url;
            Signed other;
            signer->sign(signingDraft(), other.done());
            QTRY_COMPARE(other.answers, 1);
            QCOMPARE(urls.size(), 1);
        }
    }

    // A nostrconnect:// link names the client, the relays, and a secret; the
    // signer app's connect must carry that secret, and tells its key.
    void remoteSignerPairsThroughANostrConnectLink()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        auto pairing = RemoteSigner::newPairing({relay.url(), "http://not.a.relay.invalid"});
        QVERIFY(pairing);
        QCOMPARE(pairing->relays, QStringList{relay.url()});
        QVERIFY(pairing->signerPubkey.isEmpty());
        QCOMPARE(pairing->secret.size(), 32);
        const QString link = RemoteSigner::nostrConnectUri(*pairing);
        auto signer = RemoteSigner::create(*pairing, syncPubkey());
        QVERIFY(signer);
        signer->setLimits(quickRemoteLimits());
        QVERIFY(link.startsWith("nostrconnect://" + signer->clientPubkey() + "?relay="));
        const QUrlQuery query(link.section('?', 1));
        QCOMPARE(query.allQueryItemValues("relay", QUrl::FullyDecoded), QStringList{relay.url()});
        QCOMPARE(query.queryItemValue("secret"), pairing->secret);
        QCOMPARE(query.queryItemValue("name"), QString("Readstr"));
        QCOMPARE(query.queryItemValue("perms", QUrl::FullyDecoded),
                 QString("sign_event:30404,sign_event:30405,sign_event:30406,sign_event:%1")
                     .arg(RemoteSigner::pairingCheckKind));

        std::optional<QString> paired;
        signer->pair([&](const QString &error) { paired = error; });
        QCOMPARE(signer->waiting(), 1);
        QTRY_COMPARE(relay.requests.size(), 1);
        // Another secret, or another key with the right one but no
        // connection to it, is not the pairing.
        bunker.connectTo(link, QString("guess"));
        QTest::qWait(100);
        QVERIFY(!paired);
        QCOMPARE(signer->pairing().signerPubkey, QString());
        bunker.connectTo(link);
        QTRY_VERIFY(paired);
        QCOMPARE(*paired, QString());
        QCOMPARE(bunker.methods, QStringList({"get_public_key", "sign_event"}));
        RemoteSigner::Pairing learned = signer->pairing();
        QCOMPARE(learned.signerPubkey, bunker.pubkey());
        QCOMPARE(learned.clientSecret, pairing->clientSecret);
        QVERIFY(RemoteSigner::Pairing::fromJson(learned.toJson()));
        learned.wipe();

        Signed result;
        signer->sign(signingDraft(), result.done());
        QTRY_COMPARE(result.answers, 1);
        QVERIFY(result.event.verify());

        // A link nobody answers ends in time.
        auto unanswered = RemoteSigner::newPairing({relay.url()});
        auto waiting = RemoteSigner::create(*unanswered, syncPubkey());
        RemoteSigner::Limits limits = quickRemoteLimits();
        limits.pairingTimeoutMs = 200;
        waiting->setLimits(limits);
        std::optional<QString> timedOut;
        waiting->pair([&](const QString &error) { timedOut = error; });
        QTRY_VERIFY(timedOut);
        QCOMPARE(*timedOut, QString("No signer app answered the link in time"));
        QCOMPARE(waiting->waiting(), 0);
        pairing->wipe();
        unanswered->wipe();
    }

    // A signer that only repeats the account's public key, which anyone
    // can, but cannot sign with its key is no pairing.
    void remoteSignerPairsOnlyWhenItCanSign()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        for (const auto &[mode, expected] :
             {std::pair(FakeBunker::Sign::OtherKey,
                        "The signer did not show that it holds the account's key: The signer signed with another key "
                        "than the one for " + Nip19::shortNpub(syncNpub(), syncNpub())),
              std::pair(FakeBunker::Sign::BadSignature,
                        QString("The signer did not show that it holds the account's key: The signer's signature "
                                "does not check out")),
              std::pair(FakeBunker::Sign::Refuse,
                        QString("The signer did not show that it holds the account's key: The signer refused to "
                                "sign: user rejected"))}) {
            bunker.sign = mode;
            bunker.methods.clear();
            auto signer = pairedSigner(bunker, relay);
            std::optional<QString> paired;
            signer->pair([&](const QString &error) { paired = error; });
            QTRY_VERIFY(paired);
            QCOMPARE(*paired, expected);
            QCOMPARE(bunker.methods, QStringList({"connect", "get_public_key", "sign_event"}));
        }
    }

    // Deleted with a request out on a connected relay, or from inside one
    // of its own callbacks, the signer answers nothing more and nothing of
    // it runs after.
    void remoteSignerCanBeDeletedAnytime()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        bunker.hold = true;
        auto signer = pairedSigner(bunker, relay);
        Signed dropped;
        signer->sign(signingDraft(), dropped.done());
        QTRY_COMPARE(bunker.held.size(), 1);
        signer.reset();
        bunker.answerHeld();
        relay.dropAll();
        QTest::qWait(100);
        QCOMPARE(dropped.answers, 0);

        bunker.hold = false;
        signer = pairedSigner(bunker, relay);
        bool answered = false;
        signer->sign(signingDraft(), [&](const NostrEvent &event, const QString &error) {
            answered = event.verify() && error.isEmpty();
            signer.reset();
        });
        QTRY_VERIFY(answered);
        QTest::qWait(100);

        bunker.hold = true;
        signer = pairedSigner(bunker, relay);
        RemoteSigner::Limits limits = quickRemoteLimits();
        limits.requestTimeoutMs = 100;
        signer->setLimits(limits);
        std::optional<QString> timedOut;
        signer->sign(signingDraft(), [&](const NostrEvent &, const QString &error) {
            timedOut = error;
            signer.reset();
        });
        QTRY_VERIFY(timedOut);
        QCOMPARE(*timedOut, QString("The signer did not answer in time"));
        bunker.held.clear();

        auto unanswered = RemoteSigner::newPairing({relay.url()});
        signer = RemoteSigner::create(*unanswered, syncPubkey());
        unanswered->wipe();
        limits.pairingTimeoutMs = 100;
        signer->setLimits(limits);
        std::optional<QString> unpaired;
        signer->pair([&](const QString &error) {
            unpaired = error;
            signer.reset();
        });
        QTRY_VERIFY(unpaired);
        QTest::qWait(100);
    }

    // A second pair() while a link waits fails, and the first still pairs.
    void remoteSignerPairsOnceAtATime()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        auto pairing = RemoteSigner::newPairing({relay.url()});
        const QString link = RemoteSigner::nostrConnectUri(*pairing);
        auto signer = RemoteSigner::create(*pairing, syncPubkey());
        pairing->wipe();
        signer->setLimits(quickRemoteLimits());
        std::optional<QString> first, second;
        signer->pair([&](const QString &error) { first = error; });
        signer->pair([&](const QString &error) { second = error; });
        QTRY_VERIFY(second);
        QCOMPARE(*second, QString("Already waiting for a signer app to answer the link"));
        QVERIFY(!first);
        QTRY_COMPARE(relay.requests.size(), 1);
        bunker.connectTo(link);
        QTRY_VERIFY(first);
        QCOMPARE(*first, QString());
    }

    // Limits set after the signer is made apply to its relays' sockets.
    void remoteSignerAppliesLimitsToItsRelays()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        bunker.hold = true;
        auto pairing = RemoteSigner::parseBunkerUri(bunker.uri(relay.url()), {relay.url()});
        auto signer = RemoteSigner::create(*pairing, syncPubkey());
        pairing->wipe();
        RemoteSigner::Limits limits = quickRemoteLimits();
        limits.maxMessageBytes = 4 * 1024;
        signer->setLimits(limits);
        Signed result;
        signer->sign(signingDraft(), result.done());
        QTRY_COMPARE(bunker.held.size(), 1);
        QCOMPARE(relay.connections, 1);
        relay.pushRaw("[\"NOTICE\",\"" + QString(8 * 1024, 'x') + "\"]");
        QTRY_COMPARE(relay.connections, 2);
        QTRY_COMPARE(relay.requests.size(), 2);
        bunker.answerHeld();
        QTRY_COMPARE(result.answers, 1);
        QCOMPARE(result.error, QString());
    }

    // A request every relay refused fails at once with what they said;
    // one refused by some relays only still goes through the others.
    void remoteSignerFailsWhatEveryRelayRefuses()
    {
        FakeRelay first, second;
        first.rejectWith = "blocked: not on the list";
        second.rejectWith = "rate-limited: slow down";
        FakeBunker bunker(second);
        // The relay passes the request on even so; the signer's answer
        // must not be what ends it.
        bunker.hold = true;
        const QString uri = bunker.uri(first.url()) + "&relay=" + QUrl::toPercentEncoding(second.url());
        auto pairing = RemoteSigner::parseBunkerUri(uri, {first.url(), second.url()});
        QCOMPARE(pairing->relays.size(), 2);
        auto signer = RemoteSigner::create(*pairing, syncPubkey());
        pairing->wipe();
        RemoteSigner::Limits limits = quickRemoteLimits();
        limits.requestTimeoutMs = 30000;
        signer->setLimits(limits);
        QElapsedTimer clock;
        clock.start();
        Signed refused;
        signer->sign(signingDraft(), refused.done());
        QTRY_COMPARE(refused.answers, 1);
        QVERIFY(clock.elapsed() < 10000);
        QVERIFY2(refused.error.startsWith("The relays of the remote signer refused the request: "),
                 qPrintable(refused.error));
        QCOMPARE(signer->waiting(), 0);

        bunker.held.clear();
        bunker.hold = false;
        second.rejectWith.clear();
        Signed signedOnce;
        signer->sign(signingDraft(), signedOnce.done());
        QTRY_COMPARE(signedOnce.answers, 1);
        QCOMPARE(signedOnce.error, QString());
        QVERIFY(signedOnce.event.verify());
    }

    // A request queued for a relay still connecting is not sent there once
    // another relay has brought its answer.
    void remoteSignerSendsOnlyWhatStillWaits()
    {
        FakeRelay fast, slow;
        FakeBunker bunker(fast);
        slow.pauseAccepting();
        const QString uri = bunker.uri(fast.url()) + "&relay=" + QUrl::toPercentEncoding(slow.url());
        auto pairing = RemoteSigner::parseBunkerUri(uri, {fast.url(), slow.url()});
        QCOMPARE(pairing->relays.size(), 2);
        auto signer = RemoteSigner::create(*pairing, syncPubkey());
        pairing->wipe();
        RemoteSigner::Limits limits = quickRemoteLimits();
        limits.connectTimeoutMs = 10000;
        signer->setLimits(limits);
        Signed result;
        signer->sign(signingDraft(), result.done());
        QTRY_COMPARE(result.answers, 1);
        QCOMPARE(result.error, QString());
        QVERIFY(slow.log.isEmpty());
        slow.resumeAccepting();
        QTRY_VERIFY(slow.log.contains("REQ"));
        QTest::qWait(100);
        QCOMPARE(slow.published.size(), 0);
        QCOMPARE(fast.published.size(), 1);
    }

    // A relay that ends the subscription with CLOSED is asked again, and
    // the answer still comes through it; one that keeps closing it counts
    // as refusing the request, which a later request asks it again for.
    void remoteSignerResubscribesAfterClosed()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        bunker.hold = true;
        relay.closeWith = "error: restarting";
        RemoteSigner::Limits limits = quickRemoteLimits();
        limits.requestTimeoutMs = 30000;
        auto signer = pairedSigner(bunker, relay);
        signer->setLimits(limits);
        Signed result;
        signer->sign(signingDraft(), result.done());
        QTRY_COMPARE(bunker.held.size(), 1);
        QTRY_VERIFY(relay.requests.size() >= 2);
        relay.closeWith.clear();
        const qsizetype asked = relay.requests.size();
        QTRY_VERIFY(relay.requests.size() > asked);
        QCOMPARE(result.answers, 0);
        bunker.answerHeld();
        QTRY_COMPARE(result.answers, 1);
        QCOMPARE(result.error, QString());
        QVERIFY(result.event.verify());
        QCOMPARE(relay.connections, 1);

        relay.closeWith = "blocked: not for you";
        auto closing = pairedSigner(bunker, relay);
        closing->setLimits(limits);
        QElapsedTimer clock;
        clock.start();
        Signed refused;
        const qsizetype before = relay.requests.size();
        closing->sign(signingDraft(), refused.done());
        QTRY_COMPARE(refused.answers, 1);
        QVERIFY(clock.elapsed() < 10000);
        QCOMPARE(refused.error, QString("The relays of the remote signer refused the request: blocked: not for you"));
        QCOMPARE(relay.requests.size() - before, 4);
        QCOMPARE(closing->waiting(), 0);

        bunker.held.clear();
        bunker.hold = false;
        relay.closeWith.clear();
        Signed again;
        closing->sign(signingDraft(), again.done());
        QTRY_COMPARE(again.answers, 1);
        QCOMPARE(again.error, QString());
        QVERIFY(again.event.verify());
    }

    // A draft whose signed answer cannot come back, past NIP-44's bound or
    // the relay message cap, is refused before it reaches the signer, even
    // when the request itself would fit.
    void remoteSignerRefusesWhatCannotComeBack()
    {
        FakeRelay relay;
        FakeBunker bunker(relay);
        auto signer = pairedSigner(bunker, relay);
        NostrEvent over = signingDraft();
        over.tags = {};
        over.content = QString(Nip44::maxPlaintext - 200, 'x');
        Signed tooLarge;
        signer->sign(over, tooLarge.done());
        QTRY_COMPARE(tooLarge.answers, 1);
        QCOMPARE(tooLarge.error, QString("This event is too large for a remote signer to sign"));
        QTest::qWait(100);
        QCOMPARE(bunker.methods, QStringList());

        NostrEvent under = over;
        under.content = QString(Nip44::maxPlaintext - 600, 'x');
        Signed fits;
        signer->sign(under, fits.done());
        QTRY_COMPARE(fits.answers, 1);
        QCOMPARE(fits.error, QString());
        QVERIFY(fits.event.verify());
        QCOMPARE(fits.event.content, under.content);

        RemoteSigner::Limits limits = quickRemoteLimits();
        limits.maxMessageBytes = 4 * 1024;
        signer->setLimits(limits);
        bunker.methods.clear();
        NostrEvent capped = over;
        capped.content = QString(3000, 'x');
        Signed pastCap;
        signer->sign(capped, pastCap.done());
        QTRY_COMPARE(pastCap.answers, 1);
        QCOMPARE(pastCap.error, QString("This event is too large for a remote signer to sign"));
        capped.content = QString(1000, 'x');
        Signed withinCap;
        signer->sign(capped, withinCap.done());
        QTRY_COMPARE(withinCap.answers, 1);
        QCOMPARE(withinCap.error, QString());
        QCOMPARE(bunker.methods, QStringList{"sign_event"});
    }

    // The pairing is an item of its own, which a search for the key cannot
    // find, and only one that parses is handed back.
    void secretServiceKeepsThePairingApart()
    {
        const QString npub = syncNpub(), session = "/org/freedesktop/secrets/session/1";
        const QVariantList opened{QVariant::fromValue(QDBusVariant(QString())),
                                  QVariant::fromValue(QDBusObjectPath(session))};
        RemoteSigner::Pairing pairing;
        pairing.clientSecret = QByteArray(32, '\7');
        pairing.signerPubkey = syncPubkey();
        pairing.relays = {"wss://relay.example.invalid"};
        QByteArray json = pairing.toJson();
        using Attributes = QMap<QString, QString>;
        const Attributes attributes{{"application", "readstr-nip46"}, {"npub", npub}};
        QCOMPARE(SecretServiceKeyStore::pairingAttributes(npub), attributes);
        {
            FakeSecretService service;
            std::optional<KeyStore::Result> result;
            service.storePairing(npub, json, [&](const KeyStore::Result &answer) { result = answer; });
            service.reply(opened);
            const QVariantList create = service.calls.last().arguments();
            const QVariantMap properties = create.at(0).toMap();
            QCOMPARE(properties.value("org.freedesktop.Secret.Item.Label").toString(),
                     "Readstr remote signer pairing (" + npub + ")");
            QCOMPARE(properties.value("org.freedesktop.Secret.Item.Attributes").value<Attributes>(), attributes);
            QCOMPARE(create.at(1).value<SecretServiceKeyStore::Secret>().value, json);
            service.reply({QVariant::fromValue(QDBusObjectPath("/org/freedesktop/secrets/collection/login/7")),
                           QVariant::fromValue(QDBusObjectPath("/"))});
            QVERIFY(result && result->error == KeyStore::Error::None);
            QCOMPARE(service.calls.at(1).arguments().at(1).value<SecretServiceKeyStore::Secret>().value,
                     QByteArray(json.size(), '\0'));
        }
        {
            FakeSecretService service;
            std::optional<KeyStore::Result> result;
            service.lookupPairing(npub, [&](const KeyStore::Result &answer) { result = answer; });
            QCOMPARE(service.calls.first().arguments().at(0).value<Attributes>(), attributes);
            const auto paths = [](const QString &path) {
                return QVariant::fromValue(QList<QDBusObjectPath>{QDBusObjectPath(path)});
            };
            service.reply({paths("/org/freedesktop/secrets/collection/login/1"), QVariant::fromValue(QList<QDBusObjectPath>())});
            service.reply(opened);
            // A buffer of its own, as the store wipes the reply's in place.
            const QByteArray sent(json.constData(), json.size());
            service.reply({QVariant::fromValue(SecretServiceKeyStore::Secret{QDBusObjectPath(session), {}, sent,
                                                                             "text/plain"})});
            QVERIFY(result && result->found);
            QCOMPARE(result->secret, json);
        }
        {
            // A key search names the key's attributes only.
            FakeSecretService service;
            service.contains(npub, [](const KeyStore::Result &) {});
            QCOMPARE(service.calls.first().arguments().at(0).value<Attributes>(),
                     (Attributes{{"application", "readstr"}, {"npub", npub}}));
        }
        pairing.wipe();
        wipe(json);
    }

    // With no nsec stored, the window pairs with a remote signer through a
    // bunker:// URI, keeps the pairing in the keyring alone, publishes
    // through it, shows that the request waits for approval, and signs with
    // it again after a restart without connecting anew.
    void backendPublishesThroughARemoteSigner()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        const QString npub = syncNpub();
        qint64 id = 0;
        {
            Store store;
            QVERIFY(store.open());
            id = store.addFeed("https://a.invalid/feed", "A");
            store.addFeed("https://b.invalid/feed", "B");
        }
        FakeBunker bunker(relay);
        bunker.secret = "s3cret";
        FakeKeyStore keys;
        const auto published = [&relay](int kind) {
            QList<NostrEvent> events;
            for (const QJsonObject &json : std::as_const(relay.published))
                if (json.value("kind").toInt() == kind)
                    events << *NostrEvent::fromJson(json);
            return events;
        };
        QByteArray clientSecret;
        {
            Backend backend;
            backend.setRelays({relay.url()});
            backend.setPublishDelays(100, 100);
            backend.setRemoteSignerLimits(quickRemoteLimits());
            backend.setKeyStore(&keys);
            QTRY_VERIFY(backend.signingKeyChecked() && !backend.signingBusy());
            QVERIFY(backend.signerMissing());
            QCOMPARE(backend.signerKind(), QString("none"));
            QCOMPARE(keys.pairingCalls, QStringList{"contains " + npub});

            QVERIFY(!backend.connectBunker("bunker://nothing"));
            QCOMPARE(backend.remoteSignerError(), QString("That is not a bunker:// link with a relay Readstr can use"));
            QVERIFY(backend.connectBunker(bunker.uri(relay.url(), "s3cret")));
            QVERIFY(backend.remoteSignerError().isEmpty());
            QVERIFY(backend.remoteConnecting());
            QTRY_VERIFY(backend.remoteSignerPaired());
            QVERIFY(!backend.remoteConnecting());
            QVERIFY(!backend.signerMissing());
            QCOMPARE(backend.signerKind(), QString("remote"));
            QCOMPARE(backend.status(), QString("Connected to the remote signer"));
            QCOMPARE(bunker.methods, QStringList({"connect", "get_public_key", "sign_event"}));
            auto pairing = RemoteSigner::Pairing::fromJson(keys.pairings.value(npub));
            QVERIFY(pairing);
            QCOMPARE(pairing->signerPubkey, bunker.pubkey());
            QCOMPARE(pairing->relays, QStringList{relay.url()});
            clientSecret = pairing->clientSecret.toHex();
            pairing->wipe();
            QVERIFY(keys.keys.isEmpty());

            bunker.hold = true;
            QVERIFY(backend.unsubscribe(id));
            QTRY_VERIFY_WITH_TIMEOUT(backend.remoteSignerWaiting(), 10000);
            QCOMPARE(backend.status(), QString("Approve the request in your signer app"));
            QVERIFY(published(Sync::subscriptionsKind).isEmpty());
            bunker.hold = false;
            bunker.answerHeld();
            QTRY_COMPARE(published(Sync::subscriptionsKind).size(), 1);
            QTRY_VERIFY(!backend.publishing());
            QVERIFY(!backend.remoteSignerWaiting());
            QVERIFY(backend.status() != "Approve the request in your signer app");
            QVERIFY(backend.publishError().isEmpty());
            const NostrEvent event = published(Sync::subscriptionsKind).first();
            QVERIFY(event.verify());
            QCOMPARE(event.pubkey, syncPubkey());
            QCOMPARE(keys.pairingCalls.count("lookup " + npub), 0);
        }
        {
            bunker.methods.clear();
            Backend backend;
            backend.setRelays({relay.url()});
            backend.setRemoteSignerLimits(quickRemoteLimits());
            backend.setKeyStore(&keys);
            QTRY_VERIFY(backend.remoteSignerPaired());
            QCOMPARE(backend.signerKind(), QString("remote"));
            const qsizetype before = published(Sync::subscriptionsKind).size();
            backend.publishNow();
            QTRY_COMPARE_WITH_TIMEOUT(published(Sync::subscriptionsKind).size(), before + 1, 10000);
            QTRY_VERIFY(!backend.publishing());
            QVERIFY2(backend.publishError().isEmpty(), qPrintable(backend.publishError()));
            QVERIFY(!bunker.methods.contains("connect"));
            QVERIFY(bunker.methods.contains("sign_event"));
            QCOMPARE(keys.pairingCalls.count("lookup " + npub), 1);

            backend.disconnectSigner();
            QTRY_VERIFY(!backend.remoteSignerPaired());
            QVERIFY(keys.pairings.isEmpty());
            QVERIFY(backend.signerMissing());
            QCOMPARE(backend.signerKind(), QString("none"));
            Signed result;
            backend.sign(signingDraft(), result.done());
            QTRY_COMPARE(result.answers, 1);
            QCOMPARE(result.error, "No signing key is stored for " + Nip19::shortNpub(npub, npub));
        }
        // Nothing of the client key is left in Readstr's files.
        QDirIterator files(dir.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            QFile file(files.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY2(!file.readAll().contains(clientSecret), qPrintable(file.fileName()));
        }
    }

    // A signer holding another account's key is refused and not kept; a
    // nostrconnect:// link pairs with the app that answers it; with both a
    // key and a pairing stored, the key signs; and "Stop syncing" with
    // removal takes the pairing too.
    void backendPairsOnlyWithTheAccountsSigner()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        FakeKeyStore keys;
        Backend backend;
        QStringList clipboard;
        backend.setClipboardWriter([&](const QString &text) { clipboard << text; });
        backend.setRelays({relay.url()});
        backend.setRemoteSignerLimits(quickRemoteLimits());
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyChecked() && !backend.signingBusy());
        {
            FakeBunker other(relay, 5, 2);
            QVERIFY(backend.connectBunker(other.uri(relay.url())));
            QTRY_VERIFY(!backend.remoteConnecting());
            QVERIFY2(backend.remoteSignerError().startsWith("The signer holds the key of "),
                     qPrintable(backend.remoteSignerError()));
            QVERIFY(!backend.remoteSignerPaired());
            QVERIFY(keys.pairings.isEmpty());
        }
        {
            FakeBunker bunker(relay);
            QVERIFY(backend.nostrConnectUri().isEmpty());
            QVERIFY(backend.startNostrConnect());
            const QString link = backend.nostrConnectUri();
            QVERIFY(link.startsWith("nostrconnect://"));
            QCOMPARE(QUrlQuery(link.section('?', 1)).allQueryItemValues("relay", QUrl::FullyDecoded),
                     QStringList{relay.url()});
            QCOMPARE(backend.nostrConnectQr(), QrCode::source(link));
            QVERIFY(backend.remoteSignerError().isEmpty());
            backend.copyNostrConnectUri();
            QCOMPARE(clipboard, QStringList{link});
            // Stopped and started again, the old link pairs nothing.
            backend.cancelRemoteConnect();
            QVERIFY(!backend.remoteConnecting());
            QVERIFY(backend.nostrConnectUri().isEmpty());
            QVERIFY(backend.startNostrConnect());
            const QString fresh = backend.nostrConnectUri();
            QVERIFY(fresh != link);
            QTRY_VERIFY(relay.requests.size() >= 2);
            bunker.connectTo(link);
            QTest::qWait(200);
            QVERIFY(!backend.remoteSignerPaired());
            bunker.connectTo(fresh);
            QTRY_VERIFY(backend.remoteSignerPaired());
            QVERIFY(backend.nostrConnectUri().isEmpty());
            QVERIFY(keys.pairings.contains(npub));

            // The key wins once there is one.
            QVERIFY(backend.addSigningKey(QByteArray(32, '\1').toHex()));
            QTRY_COMPARE(backend.signerKind(), QString("key"));
            bunker.methods.clear();
            Signed result;
            backend.sign(signingDraft(), result.done());
            QTRY_COMPARE(result.answers, 1);
            QVERIFY(result.error.isEmpty());
            QVERIFY(result.event.verify());
            QTest::qWait(100);
            QVERIFY(bunker.methods.isEmpty());
        }
        backend.stopSync(true);
        QTRY_VERIFY(keys.pairings.isEmpty());
        QTRY_VERIFY(keys.keys.isEmpty());
        QCOMPARE(keys.pairingCalls.last(), "remove " + npub);
        QTRY_COMPARE(backend.status(), "Stopped syncing with " + Nip19::shortNpub(npub, npub) + " and removed its signing key");
        QVERIFY(!backend.remoteSignerPaired());
    }

    // A signer that cannot sign as the account pairs nothing, and nothing
    // is kept.
    void backendStoresNoPairingWithoutProof()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        FakeBunker bunker(relay);
        bunker.sign = FakeBunker::Sign::OtherKey;
        FakeKeyStore keys;
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setRemoteSignerLimits(quickRemoteLimits());
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyChecked() && !backend.signingBusy());
        QVERIFY(backend.connectBunker(bunker.uri(relay.url())));
        QTRY_VERIFY(!backend.remoteConnecting());
        QVERIFY2(backend.remoteSignerError().startsWith("The signer did not show that it holds the account's key: "),
                 qPrintable(backend.remoteSignerError()));
        QVERIFY(!backend.remoteSignerPaired());
        QVERIFY(keys.pairings.isEmpty());
        QVERIFY(!keys.pairingCalls.contains("store " + syncNpub()));
    }

    // A key added while the remote signer still has signs out takes over
    // once they are answered: what comes meanwhile, and after, is signed
    // with the key.
    void backendSwitchesToAKeyAddedMidRemoteSign()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        FakeBunker bunker(relay);
        FakeKeyStore keys;
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setRemoteSignerLimits(quickRemoteLimits());
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyChecked() && !backend.signingBusy());
        QVERIFY(backend.connectBunker(bunker.uri(relay.url())));
        QTRY_VERIFY(backend.remoteSignerPaired());
        bunker.hold = true;
        Signed a, b;
        backend.sign(signingDraft(), a.done());
        backend.sign(signingDraft(), b.done());
        QTRY_COMPARE(bunker.held.size(), 2);
        bunker.methods.clear();
        QVERIFY(backend.addSigningKey(QByteArray(32, '\1').toHex()));
        QTRY_COMPARE(backend.signerKind(), QString("key"));
        Signed meanwhile;
        backend.sign(signingDraft(), meanwhile.done());
        QTest::qWait(200);
        QCOMPARE(meanwhile.answers, 0);
        bunker.hold = false;
        bunker.answerHeld();
        QTRY_COMPARE(a.answers, 1);
        QTRY_COMPARE(b.answers, 1);
        QTRY_COMPARE(meanwhile.answers, 1);
        QVERIFY(a.error.isEmpty() && b.error.isEmpty() && meanwhile.error.isEmpty());
        QVERIFY(meanwhile.event.verify());
        Signed next;
        backend.sign(signingDraft(), next.done());
        QTRY_COMPARE(next.answers, 1);
        QVERIFY(next.error.isEmpty());
        QVERIFY(next.event.verify());
        QTest::qWait(100);
        QCOMPARE(bunker.methods, QStringList());
    }

    // Paired anew with another signer while the first still has a sign
    // out, the next sign goes to the new one.
    void backendSwitchesToANewPairingMidRemoteSign()
    {
        QTemporaryDir dir;
        FakeRelay relayA, relayB;
        relayA.honourFilters = relayB.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        FakeBunker bunkerA(relayA, 5, 1), bunkerB(relayB, 7, 1);
        FakeKeyStore keys;
        Backend backend;
        backend.setRelays({relayA.url(), relayB.url()});
        backend.setRemoteSignerLimits(quickRemoteLimits());
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyChecked() && !backend.signingBusy());
        QVERIFY(backend.connectBunker(bunkerA.uri(relayA.url())));
        QTRY_VERIFY(backend.remoteSignerPaired());
        bunkerA.hold = true;
        Signed a;
        backend.sign(signingDraft(), a.done());
        QTRY_COMPARE(bunkerA.held.size(), 1);
        QVERIFY(backend.connectBunker(bunkerB.uri(relayB.url())));
        QTRY_VERIFY(!backend.remoteConnecting());
        QCOMPARE(RemoteSigner::Pairing::fromJson(keys.pairings.value(syncNpub()))->signerPubkey, bunkerB.pubkey());
        bunkerA.hold = false;
        bunkerA.answerHeld();
        QTRY_COMPARE(a.answers, 1);
        QVERIFY(a.error.isEmpty());
        bunkerA.methods.clear();
        bunkerB.methods.clear();
        Signed next;
        backend.sign(signingDraft(), next.done());
        QTRY_COMPARE(next.answers, 1);
        QVERIFY2(next.error.isEmpty(), qPrintable(next.error));
        QCOMPARE(bunkerA.methods, QStringList());
        QCOMPARE(bunkerB.methods, QStringList{"sign_event"});
    }

    // "Disconnect signer" fails what the remote signer has out, and what
    // waits on the pairing being read, but not a read of the key, and a
    // removal the keyring fails is said.
    void backendDisconnectsOnlyTheRemoteSigner()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        const QString disconnected = "The remote signer was disconnected before the event was signed";
        FakeBunker bunker(relay);
        FakeKeyStore keys;
        auto backend = std::make_unique<Backend>();
        const auto open = [&] {
            backend = std::make_unique<Backend>();
            backend->setRelays({relay.url()});
            backend->setRemoteSignerLimits(quickRemoteLimits());
            backend->setKeyStore(&keys);
            QTRY_VERIFY(backend->signingKeyChecked() && !backend->signingBusy());
        };
        const auto pair = [&] {
            QVERIFY(backend->connectBunker(bunker.uri(relay.url())));
            QTRY_VERIFY(backend->remoteSignerPaired());
        };
        // Mid remote sign.
        open();
        pair();
        bunker.hold = true;
        Signed remote;
        backend->sign(signingDraft(), remote.done());
        QTRY_COMPARE(bunker.held.size(), 1);
        backend->disconnectSigner();
        QTRY_COMPARE(remote.answers, 1);
        QCOMPARE(remote.error, disconnected);
        QTRY_VERIFY(!backend->remoteSignerPaired());
        QVERIFY(keys.pairings.isEmpty());
        bunker.held.clear();
        bunker.hold = false;

        // While the key is read, the key signs.
        pair();
        keys.keys.insert(npub, QByteArray(32, '\1'));
        open();
        QCOMPARE(backend->signerKind(), QString("key"));
        keys.hold = true;
        Signed withKey;
        backend->sign(signingDraft(), withKey.done());
        QTRY_COMPARE(keys.held(), 1);
        backend->disconnectSigner();
        keys.hold = false;
        keys.release();
        QTRY_COMPARE(withKey.answers, 1);
        QVERIFY2(withKey.error.isEmpty(), qPrintable(withKey.error));
        QVERIFY(withKey.event.verify());
        QTRY_VERIFY(keys.pairings.isEmpty());

        // A key the check found but the read did not: the pairing is not
        // read after it.
        pair();
        open();
        keys.keys.clear();
        keys.hold = true;
        bunker.methods.clear();
        Signed gone;
        backend->sign(signingDraft(), gone.done());
        QTRY_COMPARE(keys.held(), 1);
        backend->disconnectSigner();
        keys.hold = false;
        const qsizetype lookups = keys.pairingCalls.count("lookup " + npub);
        keys.release();
        QTRY_COMPARE(gone.answers, 1);
        QCOMPARE(gone.error, disconnected);
        QCOMPARE(keys.pairingCalls.count("lookup " + npub), lookups);
        QTRY_VERIFY(keys.pairings.isEmpty());

        // While the pairing is read.
        pair();
        open();
        bunker.methods.clear();
        keys.hold = true;
        Signed reading;
        backend->sign(signingDraft(), reading.done());
        QTRY_COMPARE(keys.held(), 1);
        QCOMPARE(keys.pairingCalls.last(), "lookup " + npub);
        backend->disconnectSigner();
        QTRY_COMPARE(reading.answers, 1);
        QCOMPARE(reading.error, disconnected);
        keys.hold = false;
        keys.release();
        QTRY_VERIFY(keys.pairings.isEmpty());
        QTest::qWait(100);
        QVERIFY(bunker.methods.isEmpty());

        // A removal that fails.
        pair();
        keys.error = KeyStore::Error::Locked;
        backend->disconnectSigner();
        QTRY_VERIFY(!backend->signingError().isEmpty());
        QVERIFY(backend->remoteSignerPaired());
        QVERIFY(keys.pairings.contains(npub));
        keys.error = KeyStore::Error::None;
    }

    // Disconnected while giving way to a key added mid sign, the remote
    // signer fails only what it has out; what waits behind it is signed
    // with the key.
    void backendDisconnectKeepsSignsForTheKey()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        FakeBunker bunker(relay);
        FakeKeyStore keys;
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setRemoteSignerLimits(quickRemoteLimits());
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyChecked() && !backend.signingBusy());
        QVERIFY(backend.connectBunker(bunker.uri(relay.url())));
        QTRY_VERIFY(backend.remoteSignerPaired());
        bunker.hold = true;
        Signed remote;
        backend.sign(signingDraft(), remote.done());
        QTRY_COMPARE(bunker.held.size(), 1);
        QVERIFY(backend.addSigningKey(QByteArray(32, '\1').toHex()));
        QTRY_COMPARE(backend.signerKind(), QString("key"));
        Signed queued;
        backend.sign(signingDraft(), queued.done());
        QTest::qWait(100);
        QCOMPARE(queued.answers, 0);
        backend.disconnectSigner();
        QTRY_COMPARE(remote.answers, 1);
        QCOMPARE(remote.error, QString("The remote signer was disconnected before the event was signed"));
        QTRY_COMPARE(queued.answers, 1);
        QVERIFY2(queued.error.isEmpty(), qPrintable(queued.error));
        QVERIFY(queued.event.verify());
        QTRY_VERIFY(!backend.remoteSignerPaired());
        bunker.held.clear();
    }

    // The approval page shown is that of a request still waiting: once the
    // one that named it is answered, it goes, though another still waits.
    void backendShowsTheAuthUrlOfAWaitingRequest()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        FakeBunker bunker(relay);
        FakeKeyStore keys;
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setRemoteSignerLimits(quickRemoteLimits());
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyChecked() && !backend.signingBusy());
        QVERIFY(backend.connectBunker(bunker.uri(relay.url())));
        QTRY_VERIFY(backend.remoteSignerPaired());
        QTRY_VERIFY(!backend.remoteConnecting());
        bunker.hold = true;
        Signed a, b;
        backend.sign(signingDraft(), a.done());
        QTRY_COMPARE(bunker.held.size(), 1);
        backend.sign(signingDraft(), b.done());
        QTRY_COMPARE(bunker.held.size(), 2);
        const FakeBunker::Held first = bunker.held.at(0), second = bunker.held.at(1);
        const QString url = "https://signer.example.invalid/approve?id=a";
        bunker.respond(first.client, first.id, "auth_url", url);
        QTRY_COMPARE(backend.remoteAuthUrl(), url);
        bunker.held = {first};
        bunker.answerHeld();
        QTRY_COMPARE(a.answers + b.answers, 1);
        QTRY_COMPARE(backend.remoteAuthUrl(), QString());
        QVERIFY(backend.remoteSignerWaiting());
        bunker.held = {second};
        bunker.answerHeld();
        QTRY_COMPARE(a.answers + b.answers, 2);
        QVERIFY(a.error.isEmpty() && b.error.isEmpty());
        QTRY_VERIFY(!backend.remoteSignerWaiting());
        QCOMPARE(backend.remoteAuthUrl(), QString());
    }

    // "Stop connecting" while the keyring writes the pairing takes it out
    // again, and nothing shows as paired.
    void backendUndoesAPairingStoppedWhileStored()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        FakeBunker bunker(relay);
        FakeKeyStore keys;
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setRemoteSignerLimits(quickRemoteLimits());
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyChecked() && !backend.signingBusy());
        keys.hold = true;
        QVERIFY(backend.connectBunker(bunker.uri(relay.url())));
        QTRY_COMPARE(keys.held(), 1);
        QCOMPARE(keys.pairingCalls.last(), "store " + npub);
        backend.cancelRemoteConnect();
        keys.hold = false;
        keys.release();
        QTRY_COMPARE(keys.pairingCalls.last(), "remove " + npub);
        QTRY_VERIFY(keys.pairings.isEmpty());
        QTest::qWait(50);
        QVERIFY(!backend.remoteSignerPaired());
        QCOMPARE(backend.signerKind(), QString("none"));
        QVERIFY(backend.status() != "Connected to the remote signer");
    }

    // Stopped while a new pairing is written over a working one, the new
    // one stays: the old item is already overwritten, and removing the new
    // one would leave no signer at all. The old signer gives way to it.
    void backendKeepsARepairingStoppedWhileStored()
    {
        QTemporaryDir dir;
        FakeRelay relayA, relayB;
        relayA.honourFilters = relayB.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        FakeBunker bunkerA(relayA, 5, 1), bunkerB(relayB, 7, 1);
        FakeKeyStore keys;
        Backend backend;
        backend.setRelays({relayA.url(), relayB.url()});
        backend.setRemoteSignerLimits(quickRemoteLimits());
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyChecked() && !backend.signingBusy());
        QVERIFY(backend.connectBunker(bunkerA.uri(relayA.url())));
        QTRY_VERIFY(backend.remoteSignerPaired());
        QTRY_VERIFY(!backend.remoteConnecting() && !backend.signingBusy());
        keys.hold = true;
        QVERIFY(backend.connectBunker(bunkerB.uri(relayB.url())));
        QTRY_COMPARE(keys.held(), 1);
        QCOMPARE(keys.pairingCalls.last(), "store " + npub);
        backend.cancelRemoteConnect();
        keys.hold = false;
        keys.release();
        QTRY_VERIFY(!backend.signingBusy());
        QTest::qWait(50);
        QVERIFY(!keys.pairingCalls.contains("remove " + npub));
        QCOMPARE(RemoteSigner::Pairing::fromJson(keys.pairings.value(npub))->signerPubkey, bunkerB.pubkey());
        QVERIFY(backend.remoteSignerPaired());
        bunkerA.methods.clear();
        bunkerB.methods.clear();
        Signed next;
        backend.sign(signingDraft(), next.done());
        QTRY_COMPARE(next.answers, 1);
        QVERIFY2(next.error.isEmpty(), qPrintable(next.error));
        QCOMPARE(bunkerA.methods, QStringList());
        QCOMPARE(bunkerB.methods, QStringList{"sign_event"});
    }

    // A link's QR code is drawn in process, scaled by whole modules, with
    // its quiet zone, and the dialog shows it with the link to copy.
    void qrCodeDrawsANostrConnectLink()
    {
        const QString link = "nostrconnect://" + syncPubkey()
            + "?relay=wss%3A%2F%2Frelay.example.invalid&secret=0123456789abcdef&name=Readstr";
        const QImage image = QrCode::image(link, 4);
        QVERIFY(!image.isNull());
        QCOMPARE(image.width(), image.height());
        QCOMPARE(image.width() % 4, 0);
        QCOMPARE(qGray(image.pixel(0, 0)), 255);
        QCOMPARE(qGray(image.pixel(image.width() - 1, image.height() - 1)), 255);
        // The finder pattern's corner, just inside the quiet zone, is dark.
        QCOMPARE(qGray(image.pixel(16, 16)), 0);
        QVERIFY(QrCode::image(QString()).isNull());

        QrImageProvider provider;
        const QString source = QrCode::source(link);
        QVERIFY(source.startsWith("image://qr/"));
        QSize size;
        const QImage served = provider.requestImage(source.mid(11), &size, QSize(200, 200));
        QVERIFY(!served.isNull());
        QCOMPARE(size, served.size());
        QVERIFY(served.width() <= 200 && served.width() > 100);

        // However large the size asked for, no wider than maxSize, and a
        // scale past it draws nothing rather than overflowing.
        const QImage huge = provider.requestImage(source.mid(11), &size, QSize(300000000, 300000000));
        QVERIFY(!huge.isNull());
        QVERIFY(huge.width() <= QrCode::maxSize && huge.width() > QrCode::maxSize / 2);
        QVERIFY(QrCode::image(link, 100000000).isNull());
        QVERIFY(QrCode::image(link, QrCode::maxSize).isNull());
    }

    void syncDialogConnectsASignerApp()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        FakeKeyStore keys;
        Backend backend;
        QStringList clipboard;
        backend.setClipboardWriter([&](const QString &text) { clipboard << text; });
        backend.setRelays({relay.url()});
        backend.setKeyStore(&keys);
        QQmlEngine engine;
        engine.addImageProvider("qr", new QrImageProvider);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty(
            "theme", QVariantMap{{"radius", 0}, {"background", "#000000"}, {"foreground", "#ffffff"}});
        engine.rootContext()->setContextProperty("translator", QVariantMap{{"language", "en"}});
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport QtQuick.Controls\n"
                          "ApplicationWindow { width: 800; height: 760; property alias dialog: dialog\n"
                          " SyncDialog { id: dialog } }\n",
                          QUrl("qrc:/probe.qml"));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *dialog = window->property("dialog").value<QObject *>();
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(!backend.signingBusy());
        QObject *start = window->findChild<QObject *>("nostrConnectButton");
        QObject *panel = window->findChild<QObject *>("nostrConnectPanel");
        QObject *qr = window->findChild<QObject *>("nostrConnectQr");
        QObject *copy = window->findChild<QObject *>("copyNostrConnectButton");
        QObject *cancel = window->findChild<QObject *>("cancelRemoteConnectButton");
        QObject *bunkerField = window->findChild<QObject *>("bunkerField");
        QObject *connectBunker = window->findChild<QObject *>("connectBunkerButton");
        QObject *error = window->findChild<QObject *>("remoteSignerErrorLabel");
        QVERIFY(start && panel && qr && copy && cancel && bunkerField && connectBunker && error);
        QVERIFY(start->property("visible").toBool());
        QVERIFY(!panel->property("visible").toBool());

        QVERIFY(QMetaObject::invokeMethod(start, "clicked"));
        QTRY_VERIFY(panel->property("visible").toBool());
        QVERIFY(!start->property("visible").toBool());
        QTRY_COMPARE(qr->property("status").toInt(), 1);
        QVERIFY(qr->property("sourceSize").toSize().width() > 0);
        // The dialog stays within the window, its form scrolls, and the
        // footer's buttons stay in view below it.
        auto *scroll = window->findChild<QQuickItem *>("syncDialogScroll");
        auto *stop = window->findChild<QQuickItem *>("stopSyncButton");
        auto *publish = window->findChild<QQuickItem *>("publishReadsBox");
        QVERIFY(scroll && stop && publish);
        QTRY_VERIFY(scroll->property("contentHeight").toReal() > scroll->height());
        const qreal dialogTop = dialog->property("y").toReal();
        QVERIFY(dialogTop >= 0);
        QVERIFY(dialogTop + dialog->property("height").toReal() <= 760);
        QVERIFY(stop->isVisible());
        const QPointF stopBottom = stop->mapToScene(QPointF(0, stop->height()));
        QVERIFY2(stopBottom.y() <= 760 && stopBottom.y() > 0, qPrintable(QString::number(stopBottom.y())));
        QVERIFY(publish->mapToScene(QPointF(0, publish->height())).y() > 760);
        QVERIFY(QMetaObject::invokeMethod(copy, "clicked"));
        QCOMPARE(clipboard, QStringList{backend.nostrConnectUri()});
        QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
        QTRY_VERIFY(!panel->property("visible").toBool());

        // The pasted link leaves the field as it is submitted.
        QVERIFY(QMetaObject::invokeMethod(bunkerField, "insert", Q_ARG(int, 0), Q_ARG(QString, "bunker://nope")));
        QVERIFY(QMetaObject::invokeMethod(connectBunker, "clicked"));
        QCOMPARE(bunkerField->property("text").toString(), QString());
        QTRY_VERIFY(error->property("visible").toBool());
        QCOMPARE(error->property("text").toString(), QString("That is not a bunker:// link with a relay Readstr can use"));
    }

    // Sanitizer

    void sanitizerKeepsTheAllowedSubset()
    {
        const QUrl base("https://blog.example.com/posts/1");
        QCOMPARE(Sanitizer::toRichText("<p>Hi <strong>there</strong>, <em>you</em> <b>and</b> <i>them</i></p>", base),
                 QString("<p>Hi <strong>there</strong>, <em>you</em> <b>and</b> <i>them</i></p>"));
        QCOMPARE(Sanitizer::toRichText("<H2>Title</H2><UL><LI>one<li>two</UL><hr/><br>", base),
                 QString("<h2>Title</h2><ul><li>one<li>two</li></li></ul><hr><br>"));
        QCOMPARE(Sanitizer::toRichText("<blockquote>q</blockquote><pre><code>a &lt; b</code></pre>", base),
                 QString("<blockquote>q</blockquote><pre><code>a &lt; b</code></pre>"));
        QCOMPARE(Sanitizer::toRichText("<table><tr><th>a</th><td>b</td></tr></table>", base),
                 QString("<table><tr><th>a</th><td>b</td></tr></table>"));
        QCOMPARE(Sanitizer::toRichText("<a href=\"/two?x=1&amp;y=2\">next</a>", base),
                 QString("<a href=\"https://blog.example.com/two?x=1&amp;y=2\">next</a>"));
        // Other tags go but keep their text; blocks keep it on its own line.
        QCOMPARE(Sanitizer::toRichText("<span>a</span><font color=red>b</font><div>c</div>d", base),
                 QString("ab<br>c<br>d"));
    }

    void sanitizerDropsAttributes()
    {
        const QUrl base("https://blog.example.com/");
        QCOMPARE(Sanitizer::toRichText("<p style=\"color:red;background:url(https://t.example/x)\" class=\"c\" "
                                       "onclick=\"steal()\" id=x>t</p>",
                                       base),
                 QString("<p>t</p>"));
        QCOMPARE(Sanitizer::toRichText("<a href='https://a.example/x' onmouseover='x()' style='y' target=_blank "
                                       "title='t'>l</a>",
                                       base),
                 QString("<a href=\"https://a.example/x\">l</a>"));
        QCOMPARE(Sanitizer::toRichText("<table><tr><td background=\"https://t.example/bg.png\">c</td></tr></table>",
                                       base),
                 QString("<table><tr><td>c</td></tr></table>"));
    }

    void sanitizerDropsDangerousElementsWithTheirContent_data()
    {
        QTest::addColumn<QString>("html");
        QTest::newRow("script") << "a<script>secret()</script>b";
        QTest::newRow("script upper") << "a<SCRIPT type=text/javascript>secret()</SCRIPT >b";
        QTest::newRow("script quoting") << "a<script>var s = '</scrip' + 't>secret';</script>b";
        QTest::newRow("style") << "a<style>secret{background:url(https://t.example/p)}</style>b";
        QTest::newRow("iframe") << "a<iframe src=https://t.example/f>secret</iframe>b";
        QTest::newRow("object") << "a<object data=x><param name=secret value=1>secret</object>b";
        QTest::newRow("embed") << "a<embed src=https://t.example/secret.swf>b";
        QTest::newRow("form") << "a<form action=https://t.example/><input value=secret>secret<button>secret</button></form>b";
        QTest::newRow("svg") << "a<svg><image href=https://t.example/secret.png/><text>secret</text></svg>b";
        QTest::newRow("svg nested") << "a<svg><svg>secret</svg>secret</svg>b";
        QTest::newRow("math") << "a<math><mi>secret</mi></math>b";
        QTest::newRow("noscript") << "a<noscript><img src=https://t.example/secret.gif></noscript>b";
        QTest::newRow("template") << "a<template><p>secret</p></template>b";
        QTest::newRow("title") << "a<title>secret</title>b";
        QTest::newRow("comment") << "a<!-- secret <img src=x> -->b";
        QTest::newRow("cdata") << "a<![CDATA[ x ]]>b";
        QTest::newRow("processing") << "a<?php secret ?>b";
    }

    void sanitizerDropsDangerousElementsWithTheirContent()
    {
        QFETCH(QString, html);
        const QString out = Sanitizer::toRichText(html, QUrl("https://blog.example.com/"));
        QVERIFY2(!out.contains("secret", Qt::CaseInsensitive), qPrintable(out));
        QVERIFY2(out.startsWith('a') && out.endsWith('b'), qPrintable(out));
    }

    void sanitizerOnlyLinksTheWeb_data()
    {
        QTest::addColumn<QString>("href");
        QTest::addColumn<QString>("expected");
        QTest::newRow("https") << "https://a.example/x" << "https://a.example/x";
        QTest::newRow("http") << "http://a.example/x" << "http://a.example/x";
        QTest::newRow("relative") << "../y" << "https://blog.example.com/y";
        QTest::newRow("protocol relative") << "//cdn.example.org/z" << "https://cdn.example.org/z";
        QTest::newRow("credentials") << "https://user:pw@a.example/" << "https://a.example/";
        QTest::newRow("javascript") << "javascript:alert(1)" << "";
        QTest::newRow("javascript case") << "JaVaScRiPt:alert(1)" << "";
        QTest::newRow("javascript hex entity") << "jav&#x61;script:alert(1)" << "";
        QTest::newRow("javascript decimal entity") << "&#106;avascript:alert(1)" << "";
        // An entity browsers know and the sanitizer does not stays literal,
        // which makes the whole thing a relative path on the article's site.
        QTest::newRow("javascript unknown entity") << "javascript&colon;alert(1)"
                                                   << "https://blog.example.com/posts/javascript&amp;colon;alert(1)";
        QTest::newRow("javascript tab") << "java\tscript:alert(1)" << "";
        QTest::newRow("javascript newline entity") << "java&#10;script:alert(1)" << "";
        QTest::newRow("javascript leading space") << " \x01javascript:alert(1)" << "";
        QTest::newRow("data") << "data:text/html;base64,PHNjcmlwdD4=" << "";
        QTest::newRow("vbscript") << "vbscript:msgbox" << "";
        QTest::newRow("file") << "file:///etc/passwd" << "";
        QTest::newRow("ftp") << "ftp://a.example/" << "";
        QTest::newRow("mailto") << "mailto:a@example.com" << "";
        QTest::newRow("empty") << "" << "";
    }

    void sanitizerOnlyLinksTheWeb()
    {
        QFETCH(QString, href);
        QFETCH(QString, expected);
        const QString out =
            Sanitizer::toRichText("<a href=\"" + href + "\">x</a>", QUrl("https://blog.example.com/posts/1"));
        if (expected.isEmpty())
            QCOMPARE(out, QString("x"));
        else
            QCOMPARE(out, QString("<a href=\"%1\">x</a>").arg(expected));
    }

    void sanitizerNeverEmitsImagesOrRemoteResources()
    {
        const QUrl base("https://blog.example.com/");
        QCOMPARE(Sanitizer::toRichText("<img src=\"https://t.example/chart.png\" alt=\"Sales &amp; <b>chart</b>\">", base),
                 QString("<a href=\"https://t.example/chart.png\">[image: Sales &amp; &lt;b&gt;chart&lt;/b&gt;]</a>"));
        QCOMPARE(Sanitizer::toRichText("<img src=/p.gif>", base),
                 QString("<a href=\"https://blog.example.com/p.gif\">[image]</a>"));
        QCOMPARE(Sanitizer::toRichText("<img data-src=https://t.example/lazy.png alt=lazy>", base),
                 QString("<a href=\"https://t.example/lazy.png\">[image: lazy]</a>"));
        QCOMPARE(Sanitizer::toRichText("<a href=https://a.example/><img src=https://t.example/i.png alt=i></a>", base),
                 QString("<a href=\"https://a.example/\">[image: i]</a>"));
        QCOMPARE(Sanitizer::toRichText("<img src=\"data:image/png;base64,AAAA\" alt=x>", base), QString());
        QCOMPARE(Sanitizer::toRichText("<img src=\"javascript:alert(1)\" onerror=alert(1)>", base), QString());

        const QStringList hostile{
            "<IMG SRC=https://t.example/a.gif>",
            "<img/src=https://t.example/a.gif>",
            "<img\nsrc=https://t.example/a.gif>",
            "<picture><source srcset=https://t.example/a.webp><img src=https://t.example/a.png></picture>",
            "<svg><image href=https://t.example/a.png /></svg>",
            "<input type=image src=https://t.example/a.png>",
            "<body background=https://t.example/bg.png>x</body>",
            "<table background=https://t.example/bg.png><tr><td>x</td></tr></table>",
            "<div style=\"background-image:url(https://t.example/bg.png)\">x</div>",
            "<link rel=stylesheet href=https://t.example/s.css>",
            "<base href=https://t.example/>",
            "<meta http-equiv=refresh content=\"0;url=https://t.example/\">",
            "<style>@import url(https://t.example/s.css);</style>",
            "<video poster=https://t.example/p.png src=https://t.example/v.mp4></video>",
            "<audio src=https://t.example/a.mp3></audio>",
            "<object data=https://t.example/o></object>",
            "<a href=https://a.example/ style=\"background:url(https://t.example/p)\">x</a>",
            "<p onload=\"fetch('https://t.example/')\">x</p>",
            "<scr<script>ipt>alert(1)</script>",
            "<<img src=https://t.example/a.gif>>",
            "<img src=\"https://t.example/a.gif\"",
        };
        for (const QString &html : hostile) {
            const QString out = Sanitizer::toRichText(html, base);
            for (const char *token : {"<img", "style", "url(", "javascript", "onerror", "onload", "<script", "background",
                                      "<link", "<base", "<meta", "<source", "<video", "<audio", "<object", "<input",
                                      "<svg", "srcset", "poster"})
                QVERIFY2(!out.contains(QLatin1String(token), Qt::CaseInsensitive),
                         qPrintable(html + " -> " + out));
        }
    }

    void sanitizerEscapesText()
    {
        const QUrl base("https://blog.example.com/");
        QCOMPARE(Sanitizer::toRichText("5 < 6 > 4 & \"q\"", base), QString("5 &lt; 6 &gt; 4 &amp; &quot;q&quot;"));
        QCOMPARE(Sanitizer::toRichText("&lt;script&gt;alert(1)&lt;/script&gt;", base),
                 QString("&lt;script&gt;alert(1)&lt;/script&gt;"));
        QCOMPARE(Sanitizer::toRichText("caf&eacute; &unknown; &#0; &#xD800;", base),
                 QString("caf&amp;eacute; &amp;unknown; &amp;#0; &amp;#xD800;"));
        QCOMPARE(Sanitizer::toRichText(QString("a") + QChar(0) + "b", base), QString("a") + QChar(0xFFFD) + "b");
        QCOMPARE(Sanitizer::plainToRichText("1 < 2 <b>"), QString("<p>1 &lt; 2 &lt;b&gt;</p>"));
        QCOMPARE(Sanitizer::plainToRichText({}), QString());
    }

    void sanitizerBalancesTags()
    {
        const QUrl base("https://blog.example.com/");
        QCOMPARE(Sanitizer::toRichText("<p><strong>bold", base), QString("<p><strong>bold</strong></p>"));
        QCOMPARE(Sanitizer::toRichText("</p></div></strong>stray", base), QString("stray"));
        QCOMPARE(Sanitizer::toRichText("<b><i>x</b>y</i>", base), QString("<b><i>x</i></b>y"));
        QCOMPARE(Sanitizer::toRichText("<a href=https://a.example/>one<a href=https://b.example/>two</a>", base),
                 QString("<a href=\"https://a.example/\">one</a><a href=\"https://b.example/\">two</a>"));
        QCOMPARE(Sanitizer::toRichText("<a href=javascript:x>one</a>two", base), QString("onetwo"));
        QCOMPARE(Sanitizer::toRichText("text <a href='https://a.example/", base), QString("text "));
        QCOMPARE(Sanitizer::toRichText("<p>a</p><script>alert(1)", base), QString("<p>a</p>"));
        QCOMPARE(Sanitizer::toRichText("a < b and </ c>d", base), QString("a &lt; b and d"));

        QString deep;
        for (int i = 0; i < 1000; ++i)
            deep += "<blockquote>";
        deep += "deep";
        const QString out = Sanitizer::toRichText(deep, base);
        QCOMPARE(out.count("<blockquote>"), Sanitizer::maxDepth);
        QCOMPARE(out.count("</blockquote>"), Sanitizer::maxDepth);
        QVERIFY(out.contains("deep"));
    }

    void sanitizerStaysFastOnHostileInput_data()
    {
        QTest::addColumn<QString>("html");
        const int n = Sanitizer::maxInputLength;
        QTest::newRow("unclosed tags") << QString("<a ").repeated(n / 3);
        QTest::newRow("unclosed comments") << QString("<!--").repeated(n / 4);
        QTest::newRow("unclosed scripts") << QString("<script>").repeated(n / 8);
        QTest::newRow("unclosed quotes") << QString("<a href='x>").repeated(n / 11);
        QTest::newRow("bogus") << QString("<!x").repeated(n / 3);
        QTest::newRow("end tags") << QString("</b").repeated(n / 3);
        QTest::newRow("nested") << QString("<b><i>").repeated(n / 6);
        QTest::newRow("closers") << QString("<p></b>").repeated(n / 7);
        QTest::newRow("links") << QString("<a href=x>").repeated(n / 10);
        QTest::newRow("images") << QString("<img src=x>").repeated(n / 11);
        QTest::newRow("entities") << QString("&#x61;&amp;").repeated(n / 11);
        QTest::newRow("lt") << QString("<").repeated(n);
        QTest::newRow("oversized") << QString("<p>x</p>").repeated(n);
    }

    void sanitizerStaysFastOnHostileInput()
    {
        QFETCH(QString, html);
        // A long base URL makes every link and image expensive to write.
        const QUrl base("https://blog.example.com/" + QString("a").repeated(4000));
        QElapsedTimer timer;
        timer.start();
        const QString out = Sanitizer::toRichText(html, base);
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(out.size() <= Sanitizer::maxOutputLength + 8192);
        if (html.size() > Sanitizer::maxInputLength || html.startsWith("<<"))
            QVERIFY(out.endsWith(QString("<p>") + QChar(0x2026) + "</p>"));
    }

    // Models

    void sourceModelGroupsFeedsByCategory()
    {
        SourceModel model;
        Feed loose{1, "https://l.example/", "Loose", {}, {}, {}, {}, {}, {}, {}, {}, {}, 2, 3};
        Feed news{2, "https://n.example/", "Daily", {}, {}, "News", {}, {}, "HTTP 404", {}, {}, {}, 1, 1};
        Feed alpha{3, "https://a.example/", "Alpha", {}, {}, "art", {}, {}, {}, {}, {}, {}, 4, 4};
        model.setLibrary({loose, news, alpha}, LibraryCounts{8, 7, 1, 8});
        QCOMPARE(model.rowCount(), 9);
        const QStringList kinds{"all", "unread", "starred", "category", "feed", "category", "feed", "category", "feed"};
        for (int i = 0; i < kinds.size(); ++i)
            QCOMPARE(model.data(model.index(i), SourceModel::KindRole).toString(), kinds.at(i));
        QCOMPARE(model.data(model.index(0), SourceModel::CountRole).toInt(), 7);
        QCOMPARE(model.data(model.index(2), SourceModel::CountRole).toInt(), 1);
        // Categories sort case-insensitively, each followed by its feeds.
        QCOMPARE(model.data(model.index(5), SourceModel::CategoryRole).toString(), QString("art"));
        QCOMPARE(model.data(model.index(5), SourceModel::CountRole).toInt(), 4);
        QCOMPARE(model.data(model.index(6), SourceModel::TitleRole).toString(), QString("Alpha"));
        QCOMPARE(model.data(model.index(8), SourceModel::LastErrorRole).toString(), QString("HTTP 404"));
        QVERIFY(!model.data(model.index(3), SourceModel::SelectableRole).toBool());
        QCOMPARE(model.step(2, 1), 4);
        QCOMPARE(model.step(4, -1), 2);
        QCOMPARE(model.step(8, 1), 8);
        QCOMPARE(model.rowOf(SourceModel::Kind::Feed, 2), 8);

        // Same rows, new counts: updated in place, no reset.
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        news.unread = 0;
        model.setLibrary({loose, news, alpha}, LibraryCounts{8, 6, 1, 8});
        QCOMPARE(reset.count(), 0);
        // All, Unread, the News heading, and Daily.
        QCOMPARE(changed.count(), 4);
        QCOMPARE(model.data(model.index(8), SourceModel::CountRole).toInt(), 0);
        model.setLibrary({loose, alpha}, LibraryCounts{});
        QCOMPARE(reset.count(), 1);
        QCOMPARE(model.rowCount(), 7);
    }

    void articleModelTracksFlags()
    {
        ArticleModel model;
        QSignalSpy count(&model, &ArticleModel::countChanged);
        Article a, b, c;
        a.id = 10, a.read = true, a.title = "a";
        b.id = 11, b.title = "b", b.published = QDateTime(QDate(2026, 1, 2), QTime(3, 4), QTimeZone::UTC);
        c.id = 12, c.read = true, c.title = "c";
        model.setArticles({a, b, c});
        QCOMPARE(count.count(), 1);
        QCOMPARE(model.rowOf(12), 2);
        QCOMPARE(model.nextUnread(-1), 1);
        QCOMPARE(model.nextUnread(1), -1);
        QVERIFY(model.data(model.index(0), ArticleModel::PublishedRole).isNull());
        QCOMPARE(model.data(model.index(1), ArticleModel::PublishedRole).toDateTime(), b.published);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.setFlags(1, true, true);
        QCOMPARE(changed.count(), 1);
        QVERIFY(model.data(model.index(1), ArticleModel::StarredRole).toBool());
        model.setFlags(1, true, true);
        QCOMPARE(changed.count(), 1);
    }

    // The window's backend

    void backendShowsAndNavigatesTheLibrary()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const auto [first, second] = seedLibrary();

        Backend backend;
        QCOMPARE(backend.feedCount(), 2);
        QCOMPARE(backend.sourceKind(), QString("all"));
        QCOMPARE(backend.articles()->count(), 5);
        // Newest first, the undated one by arrival.
        QCOMPARE(backend.articles()->at(0).title, QString("Newest"));
        QCOMPARE(backend.sources()->data(backend.sources()->index(0), SourceModel::CountRole).toInt(), 4);
        QVERIFY(!backend.hasArticle());

        backend.moveArticle(1);
        QCOMPARE(backend.articleIndex(), 0);
        QCOMPARE(backend.articleTitle(), QString("Newest"));
        QVERIFY(backend.articleRead());
        QVERIFY2(!backend.articleHtml().contains("script"), qPrintable(backend.articleHtml()));
        QVERIFY(backend.articleHtml().contains("<strong>full</strong>"));
        // Saved on the worker; the count follows once it is.
        QTRY_COMPARE(backend.sources()->data(backend.sources()->index(0), SourceModel::CountRole).toInt(), 3);
        backend.moveArticle(1);
        QCOMPARE(backend.articleTitle(), QString("Second A"));
        backend.moveArticle(10);
        QCOMPARE(backend.articleIndex(), 4);
        backend.moveArticle(-1);
        QCOMPARE(backend.articleIndex(), 3);

        Store store;
        QVERIFY(store.open());
        QCOMPARE(store.articles({}).size(), 5);
        QTRY_VERIFY(unreadIs(store, 1));

        backend.selectSource(backend.sources()->rowOf(SourceModel::Kind::Unread));
        QCOMPARE(backend.sourceKind(), QString("unread"));
        QCOMPARE(backend.articles()->count(), 1);
        QVERIFY(!backend.hasArticle());
        backend.nextUnread();
        QVERIFY(backend.hasArticle());
        QCOMPARE(backend.articleIndex(), 0);
        // A read article stays in the Unread list until the view reloads.
        QCOMPARE(backend.articles()->count(), 1);
        backend.nextUnread();
        QVERIFY(backend.status().contains("No more unread"));

        backend.selectSource(backend.sources()->rowOf(SourceModel::Kind::Feed, second));
        QCOMPARE(backend.selectedFeedId(), second);
        QCOMPARE(backend.sourceTitle(), QString("Second"));
        QCOMPARE(backend.articles()->count(), 2);
        backend.moveSource(-1);
        QCOMPARE(backend.selectedFeedId(), first);
        backend.moveSource(-1);
        QCOMPARE(backend.sourceKind(), QString("starred"));
    }

    void backendTogglesFlagsAndMarksTheViewRead()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const auto [first, second] = seedLibrary();
        Backend backend;
        backend.selectArticle(0);
        const qint64 id = backend.articleId();

        backend.toggleStar();
        QVERIFY(backend.articleStarred());
        backend.toggleRead();
        QVERIFY(!backend.articleRead());
        Store store;
        QVERIFY(store.open());
        QTRY_VERIFY(articleIs(store, id, [](const Article &a) { return a.starred; }));
        QTRY_VERIFY(articleIs(store, id, [](const Article &a) { return !a.read; }));
        QTRY_COMPARE(backend.sources()->data(backend.sources()->index(2), SourceModel::CountRole).toInt(), 1);
        QVERIFY(backend.articles()->at(0).starred);

        backend.selectSource(backend.sources()->rowOf(SourceModel::Kind::Starred));
        QCOMPARE(backend.articles()->count(), 1);
        backend.toggleStar();
        QVERIFY(!backend.hasArticle());
        backend.selectArticle(0);
        backend.toggleStar();
        QTRY_VERIFY(articleIs(store, id, [](const Article &a) { return !a.starred; }));

        // Marking a feed read leaves the other feed, and anything newer than
        // the list, alone.
        backend.selectSource(backend.sources()->rowOf(SourceModel::Kind::Feed, first));
        ParsedItem late;
        late.guid = "late";
        late.title = "Late";
        QCOMPARE(store.saveItems(first, {late}), 1);
        backend.markViewRead();
        QTRY_VERIFY(backend.status().contains("articles read"));
        for (const Article &article : store.articles({}))
            QCOMPARE(article.read, article.feedId == first ? article.guid != "late" : article.guid == "second-b");
        QCOMPARE(backend.articles()->count(), 4);
    }

    void backendOpensOnlyWebLinks()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedLibrary();
        Backend backend;
        QList<QUrl> opened;
        backend.setUrlOpener([&opened](const QUrl &url) {
            opened << url;
            return true;
        });
        backend.openOriginal();
        QVERIFY(opened.isEmpty());
        backend.selectArticle(0);
        backend.openOriginal();
        QCOMPARE(opened, QList<QUrl>{QUrl("https://first.example.com/newest")});
        for (const char *link : {"javascript:alert(1)", "file:///etc/passwd", "data:text/html,x", "mailto:a@b.c", ""})
            QVERIFY2(!backend.openLink(link), link);
        QVERIFY(backend.statusError());
        QVERIFY(backend.openLink("http://other.example.org/page"));
        QCOMPARE(opened.size(), 2);
        backend.setUrlOpener([](const QUrl &) { return false; });
        QVERIFY(!backend.openLink("https://other.example.org/"));
        QVERIFY(backend.status().contains("other.example.org"));
    }

    void backendSubscribesThroughDiscovery()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        server.serve("/", "<head><link rel=\"alternate\" type=\"application/rss+xml\" href=\"/feed.xml\"></head>",
                     "text/html");
        server.serve("/feed.xml", fixture("rss2.xml"));

        Backend backend;
        QSignalSpy subscribed(&backend, &Backend::subscribed);
        backend.addFeed(server.url("/").toString());
        QVERIFY(backend.adding());
        QVERIFY(subscribed.wait(20000));
        QVERIFY(!backend.adding());
        QVERIFY(backend.addError().isEmpty());
        QCOMPARE(backend.feedCount(), 1);
        QCOMPARE(backend.sourceKind(), QString("feed"));
        QCOMPARE(backend.sourceTitle(), QString("Example & Friends"));
        QCOMPARE(backend.articles()->count(), 4);
        QVERIFY2(backend.status().startsWith("Subscribed to Example & Friends"), qPrintable(backend.status()));

        backend.addFeed(server.url("/feed.xml").toString());
        QVERIFY(subscribed.wait(20000));
        QVERIFY(backend.status().startsWith("Already subscribed"));
        QCOMPARE(backend.feedCount(), 1);

        backend.addFeed("not an address");
        QVERIFY(!backend.adding());
        QVERIFY(!backend.addError().isEmpty());
        backend.clearAddError();
        backend.addFeed(server.url("/missing").toString());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.adding(), 20000);
        QVERIFY2(backend.addError().contains("HTTP 404"), qPrintable(backend.addError()));
        QCOMPARE(backend.feedCount(), 1);
    }

    void backendUnsubscribes()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const auto [first, second] = seedLibrary();
        Backend backend;
        backend.selectSource(backend.sources()->rowOf(SourceModel::Kind::Feed, first));
        backend.selectArticle(0);
        QVERIFY(backend.unsubscribe(first));
        QTRY_COMPARE(backend.feedCount(), 1);
        QCOMPARE(backend.sourceKind(), QString("all"));
        QVERIFY(!backend.hasArticle());
        QCOMPARE(backend.articles()->count(), 2);
        QTRY_VERIFY(backend.status().contains("First"));
        QVERIFY(!backend.unsubscribe(first));
        QVERIFY(backend.statusError());
    }

    // The window keeps drawing while feeds load: fetches run on its event
    // loop and the parsing and saving on the worker thread.
    void backendRefreshesWithoutBlocking()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        server.route("/slow.xml", [](const FakeHttpServer::Request &) {
            FakeHttpServer::Response response{200, {{"Content-Type", "application/rss+xml"}}, fixture("rss2.xml")};
            response.delayMs = 1500;
            return response;
        });
        {
            Store store;
            QVERIFY(store.open());
            QVERIFY(store.addFeed(server.url("/slow.xml").toString(), "Slow"));
            QVERIFY(store.addFeed(server.url("/missing.xml").toString(), "Missing"));
        }

        Backend backend;
        int ticks = 0;
        QTimer ticker;
        ticker.setInterval(20);
        connect(&ticker, &QTimer::timeout, [&ticks] { ++ticks; });
        ticker.start();
        QSignalSpy finished(&backend, &Backend::refreshFinished);
        QElapsedTimer timer;
        timer.start();
        backend.refreshAll();
        QVERIFY(timer.elapsed() < 1000);
        QVERIFY(backend.refreshing());
        QCOMPARE(backend.refreshTotal(), 2);
        backend.refreshAll();
        QVERIFY(finished.wait(30000));
        QCOMPARE(finished.count(), 1);
        QVERIFY(timer.elapsed() >= 1400);
        // About 75 ticks at 20 ms; a blocked loop would get one or two.
        QVERIFY2(ticks >= 15, qPrintable(QString::number(ticks)));
        QVERIFY(!backend.refreshing());
        QCOMPARE(backend.refreshDone(), 2);
        QCOMPARE(backend.articles()->count(), 4);
        QVERIFY(backend.statusError());
        QVERIFY2(backend.status().contains("Missing") && backend.status().contains("HTTP 404"),
                 qPrintable(backend.status()));
        const int missing = backend.sources()->rowOf(SourceModel::Kind::Feed, 2);
        QCOMPARE(backend.sources()->data(backend.sources()->index(missing), SourceModel::LastErrorRole).toString(),
                 QString("HTTP 404"));

        // One feed on its own, and nothing new this time.
        backend.refreshFeed(1);
        QCOMPARE(backend.refreshTotal(), 1);
        QVERIFY(finished.wait(30000));
        QCOMPARE(backend.status(), QString("No new articles"));
        QVERIFY(!backend.statusError());
    }

    void backendRefreshesAutomaticallyWithJitter()
    {
        QCOMPARE(Backend::autoRefreshDelayMs(0), 27 * 60 * 1000);
        QCOMPARE(Backend::autoRefreshDelayMs(0.5), 30 * 60 * 1000);
        QCOMPARE(Backend::autoRefreshDelayMs(1), 33 * 60 * 1000);
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        Backend backend;
        QCOMPARE(backend.nextAutoRefreshMs(), -1);
        backend.startAutoRefresh();
        const int next = backend.nextAutoRefreshMs();
        QVERIFY2(next > 26 * 60 * 1000 && next <= 33 * 60 * 1000, qPrintable(QString::number(next)));
    }

    void backendImportsAndExportsOpml()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", (dir.path() + "/home").toUtf8());
        FakeHttpServer server;
        server.serve("/one.xml", fixture("rss2.xml"));
        server.serve("/two.xml", fixture("atom.xml"), "application/atom+xml");
        write(dir.path() + "/in.opml",
              "<opml version=\"2.0\"><body><outline text=\"Folder\">"
              "<outline type=\"rss\" text=\"One\" xmlUrl=\"" + server.url("/one.xml").toString().toUtf8() + "\"/>"
              "<outline type=\"rss\" text=\"One again\" xmlUrl=\"" + server.url("/one.xml/").toString().toUtf8() + "\"/>"
              "</outline><outline type=\"rss\" text=\"Two\" xmlUrl=\"" + server.url("/two.xml").toString().toUtf8() + "\"/>"
              "<outline type=\"nostr\" text=\"Author\" nostr=\"npub1abc\"/>"
              "<outline type=\"rss\" text=\"Bad\" xmlUrl=\"javascript:alert(1)\"/></body></opml>");

        FakePicker picker;
        Backend backend(&picker);
        QSignalSpy imported(&backend, &Backend::imported);
        QSignalSpy refreshed(&backend, &Backend::refreshFinished);
        picker.openUrl = QUrl::fromLocalFile(dir.path() + "/in.opml");
        backend.importOpml();
        QVERIFY(imported.wait(20000));
        QCOMPARE(backend.feedCount(), 2);
        QVERIFY2(backend.status().startsWith("Imported 2 feeds (1 already subscribed, 2 not supported yet)"),
                 qPrintable(backend.status()));
        // Imported feeds are fetched straight away.
        QVERIFY(refreshed.wait(20000));
        QCOMPARE(backend.articles()->count(), 4 + 3);

        picker.openUrl = QUrl::fromLocalFile(dir.path() + "/missing.opml");
        backend.importOpml();
        QTRY_VERIFY(backend.statusError());
        QVERIFY(backend.status().contains("cannot read"));
        picker.openUrl = QUrl("https://example.com/remote.opml");
        backend.importOpml();
        QVERIFY(backend.status().contains("local files"));

        picker.saveUrl = QUrl::fromLocalFile(dir.path() + "/out.opml");
        backend.exportOpml();
        QCOMPARE(picker.suggestedName, QString("readstr-subscriptions.opml"));
        QVERIFY2(!backend.statusError(), qPrintable(backend.status()));
        QCOMPARE(mode(dir.path() + "/out.opml"), 0600);
        const QByteArray exported = fixtureAt(dir.path() + "/out.opml");
        QVERIFY(exported.contains(server.url("/two.xml").toString().toUtf8()));
        QCOMPARE(Opml::parse(exported).size(), 2);
        picker.saveUrl = QUrl::fromLocalFile(dir.path() + "/no/such/dir/out.opml");
        backend.exportOpml();
        QVERIFY(backend.statusError());

        picker.failure = "The desktop portal is not running";
        backend.importOpml();
        QCOMPARE(backend.status(), picker.failure);
        Backend withoutPicker;
        withoutPicker.importOpml();
        QVERIFY(withoutPicker.statusError());
    }

    void storeMarksReadUpToAnId()
    {
        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.path() + "/library.db"));
        const qint64 feed = store.addFeed("https://a.example/feed", "A");
        ParsedItem one, two;
        one.guid = one.title = "1";
        two.guid = two.title = "2";
        QCOMPARE(store.saveItems(feed, {one, two}), 2);
        const LibraryCounts counts = *store.counts();
        QCOMPARE(counts.total, 2);
        QCOMPARE(counts.unread, 2);
        QCOMPARE(store.markRead({}, counts.lastId - 1), 1);
        QCOMPARE(store.counts()->unread, 1);
        ArticleFilter starred;
        starred.starredOnly = true;
        QCOMPARE(store.markRead(starred, counts.lastId), 0);
        QVERIFY(!store.article(9999));
        ArticleFilter light;
        light.withContent = false;
        two.content = "<p>body</p>";
        store.saveItems(feed, {two});
        QVERIFY(store.articles(light).at(0).content.isEmpty());
        QCOMPARE(store.article(counts.lastId)->content, QString("<p>body</p>"));
    }

    // Review and stress-test regressions

    // Qt lays out nested tables in time exponential in their depth: 21
    // levels from a 316-byte article froze the window for minutes.
    void sanitizerKeepsTablesFlatAndBounded()
    {
        const QUrl base("https://blog.example.com/");
        auto layoutMs = [](const QString &html) {
            QElapsedTimer timer;
            timer.start();
            QTextDocument document;
            document.setTextWidth(800);
            document.setHtml(html);
            document.documentLayout()->documentSize();
            return timer.elapsed();
        };
        for (int depth : {16, 21, 64}) {
            const QString out = Sanitizer::toRichText(QString("<table><tr><td>").repeated(depth) + "x", base);
            QCOMPARE(out, QString("<table><tr><td>x</td></tr></table>"));
            const qint64 ms = layoutMs(out);
            QVERIFY2(ms < 500, qPrintable(QString("depth %1: %2 ms").arg(depth).arg(ms)));
        }
        QCOMPARE(Sanitizer::toRichText("<table><tr><td>a<table><tr><td>b</td><td>c</td></tr></table>d</td></tr>"
                                       "<tr><td>e</td></tr></table>",
                                       base),
                 QString("<table><tr><td>a<br>b<br>c<br>d</td></tr><tr><td>e</td></tr></table>"));
        // An inner table left open ends with the cell around it.
        QCOMPARE(Sanitizer::toRichText("<table><tr><td><table>x</td></tr><tr><td>y</td></tr></table><p>after</p>"
                                       "<table><tr><td>z</td></tr></table>",
                                       base),
                 QString("<table><tr><td>x</td></tr><tr><td>y</td></tr></table><p>after</p>"
                         "<table><tr><td>z</td></tr></table>"));
        // Rows and cells outside a table are only separators.
        QCOMPARE(Sanitizer::toRichText("a<td>b<tr>c", base), QString("a<br>b<br>c"));

        const QString wide =
            Sanitizer::toRichText("<table><tr>" + QString("<td>x</td>").repeated(40) + "</tr></table>", base);
        QCOMPARE(wide.count("<td>"), 32);
        const QString tall =
            Sanitizer::toRichText("<table>" + QString("<tr><td>x</td></tr>").repeated(1100) + "</table>", base);
        QCOMPARE(tall.count("<tr>"), 1000);
        const QString largest = Sanitizer::toRichText("<table><tr>" + QString("<td>x</td>").repeated(24000) + "</tr>"
                                                           + QString("<tr><td>x</td></tr>").repeated(14000) + "</table>",
                                                       base);
        QCOMPARE(largest.count("<table>"), 1);
        const qint64 ms = layoutMs(largest);
        QVERIFY2(ms < 5000, qPrintable(QString("%1 ms").arg(ms)));
    }

    // Qt's line layout crawls on long runs without a break opportunity.
    void sanitizerBreaksLongRuns()
    {
        const QUrl base("https://blog.example.com/");
        const QString out = Sanitizer::toRichText(QString(500, 'x'), base);
        QCOMPARE(out.count(QChar(0x200B)), 6);
        QCOMPARE(out.indexOf(QChar(0x200B)), 80);
        QCOMPARE(Sanitizer::toRichText(QString(79, 'y') + " " + QString(79, 'y'), base).count(QChar(0x200B)), 0);
        // Never inside a cluster: a Thai vowel stays on its consonant, and
        // an emoji sequence stays whole, but long runs of either still break.
        const QString thai = QString(80, QChar(0x0E01)) + QChar(0x0E31) + QString(10, QChar(0x0E01));
        const QString thaiOut = Sanitizer::toRichText(thai, base);
        QVERIFY(!thaiOut.contains(QString(QChar(0x200B)) + QChar(0x0E31)));
        QCOMPARE(thaiOut.count(QChar(0x200B)), 1);
        QCOMPARE(thaiOut.indexOf(QChar(0x200B)), 81);
        const QString family = QString::fromUtf8("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xEF\xB8\x8F");
        const QString emojiOut = Sanitizer::toRichText(family.repeated(40), base);
        QVERIFY(emojiOut.count(QChar(0x200B)) > 0);
        for (const QString &part : emojiOut.split(QChar(0x200B))) {
            QVERIFY(!part.isEmpty() && !part.front().isLowSurrogate() && part.front() != QChar(0x200D)
                    && part.front() != QChar(0xFE0F));
            QVERIFY(!part.endsWith(QChar(0x200D)));
        }
        // An endless cluster (a letter under combining marks) breaks inside
        // itself after a while, but never inside a surrogate pair.
        const QString zalgo = "a" + QString(100000, QChar(0x0301));
        const QString zalgoOut = Sanitizer::toRichText(zalgo, base);
        QVERIFY2(zalgoOut.count(QChar(0x200B)) >= 100000 / 160, qPrintable(QString::number(zalgoOut.count(QChar(0x200B)))));
        {
            QElapsedTimer timer;
            timer.start();
            QTextDocument document;
            document.setTextWidth(800);
            document.setHtml(zalgoOut);
            document.documentLayout()->documentSize();
            QVERIFY2(timer.elapsed() < 2000, qPrintable(QString("%1 ms").arg(timer.elapsed())));
        }
        const QString wideMark = QString::fromUcs4(U"\U0001D167");
        const QString wideOut = Sanitizer::toRichText("a" + wideMark.repeated(500), base);
        QVERIFY(wideOut.count(QChar(0x200B)) > 0);
        for (const QString &part : wideOut.split(QChar(0x200B)))
            QVERIFY(!part.isEmpty() && !part.front().isLowSurrogate() && !part.back().isHighSurrogate());
        // The address of a link is never touched.
        const QString link = "https://a.example/" + QString(200, 'p');
        QVERIFY(Sanitizer::toRichText("<a href=\"" + link + "\">t</a>", base).contains("href=\"" + link + "\""));

        for (const QString &input : {QString(Sanitizer::maxInputLength, 'x'),
                                     ("<td>" + QString(1000, 'x') + " ").repeated(500),
                                     QString("&#99999999;").repeated(45000)}) {
            QElapsedTimer timer;
            timer.start();
            QTextDocument document;
            document.setTextWidth(800);
            document.setHtml(Sanitizer::toRichText(input, base));
            document.documentLayout()->documentSize();
            QVERIFY2(timer.elapsed() < 2000, qPrintable(QString("%1 ms").arg(timer.elapsed())));
        }
    }

    // Closing an inline tag is no block boundary, so text after it still
    // gets its separator before a block Qt does not get.
    void sanitizerSeparatesBlocksAfterInlineText()
    {
        const QUrl base("https://blog.example.com/");
        QCOMPARE(Sanitizer::toRichText("text<em>x</em><div>y</div>", base), QString("text<em>x</em><br>y<br>"));
        QCOMPARE(Sanitizer::toRichText("<p><b>a</b></p><div>b</div>", base), QString("<p><b>a</b></p>b<br>"));
        QCOMPARE(Sanitizer::toRichText("<p><b>a</b></p><p>b</p>", base), QString("<p><b>a</b></p><p>b</p>"));
        QCOMPARE(Sanitizer::toRichText("text<em>x</em><p>y</p>", base), QString("text<em>x</em><p>y</p>"));
    }

    void sanitizerNeverSplitsASurrogatePairOrKeepsHugeLinks()
    {
        const QUrl base("https://blog.example.com/");
        // Escaped, this text is past the output cap, which then keeps its
        // first maxOutputLength / 6 characters: up to the emoji's first half.
        const qsizetype cut = Sanitizer::maxOutputLength / 6;
        QString input = QString("& ").repeated((cut - 1) / 2) + "&";
        QCOMPARE(input.size(), cut - 1);
        input += QString::fromUtf8("\xF0\x9F\x98\x80") + QString("& ").repeated(100000);
        const QString out = Sanitizer::toRichText(input, base);
        QVERIFY(out.endsWith(QString("<p>") + QChar(0x2026) + "</p>"));
        QVERIFY(QStringView(out).isValidUtf16());
        QVERIFY(!out.contains(QChar(0xD83D)));

        const QString huge = "https://a.example/" + QString(9000, 'p');
        QCOMPARE(Sanitizer::toRichText("<a href=\"" + huge + "\">words</a><img src=\"" + huge + "\">", base),
                 QString("words"));
    }

    void bidiFormattingNeverReachesTheWindow()
    {
        QCOMPARE(FeedParser::cleanText(QString(u"a\u202eb\u2066c\nd\u200fe\u061cf")), QString("abc def"));
        const QByteArray rss = "<rss version=\"2.0\"><channel><title>Feed&#x202e;txt</title><item><guid>1</guid>"
                               "<title>Item&#x2067;&#x061c;one</title></item></channel></rss>";
        const auto parsed = FeedParser::parse(rss, QUrl("https://a.example/"));
        QVERIFY(parsed);
        QCOMPARE(parsed->title, QString("Feedtxt"));
        QCOMPARE(parsed->items.at(0).title, QString("Itemone"));
        // Names from OPML are stored as written but shown clean.
        SourceModel model;
        Feed feed;
        feed.id = 1;
        feed.title = QString(u"Evil\u202etxt\nmore");
        feed.category = QString(u"Fold\u2066er");
        model.setLibrary({feed}, {});
        const int row = model.rowOf(SourceModel::Kind::Feed, 1);
        QCOMPARE(model.data(model.index(row), SourceModel::TitleRole).toString(), QString("Eviltxt more"));
        QCOMPARE(model.data(model.index(row), SourceModel::CategoryRole).toString(), QString("Folder"));
        // A title of nothing but bidi controls and spaces shows the address,
        // without credentials; the stored title is not the model's business.
        Feed blank;
        blank.id = 2;
        blank.title = QString(u"\u202e \u2066\t");
        blank.url = "https://user:secret@example.com/feed";
        model.setLibrary({feed, blank}, {});
        const int blankRow = model.rowOf(SourceModel::Kind::Feed, 2);
        QCOMPARE(model.data(model.index(blankRow), SourceModel::TitleRole).toString(),
                 QString("https://example.com/feed"));
        QCOMPARE(model.data(model.index(blankRow), SourceModel::UrlRole).toString(), blank.url);
    }

    void modelsAndBackendTakeExtremeSteps()
    {
        SourceModel sources;
        Feed feed;
        feed.id = 1;
        sources.setLibrary({feed}, {});
        QCOMPARE(sources.step(1, INT_MIN), 0);
        QCOMPARE(sources.step(0, INT_MAX), sources.count() - 1);
        ArticleModel articles;
        Article article;
        article.id = 1;
        articles.setArticles({article});
        QCOMPARE(articles.nextUnread(INT_MAX), -1);
        QCOMPARE(articles.nextUnread(-1), 0);

        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedLibrary();
        Backend backend;
        backend.moveArticle(1);
        backend.moveArticle(INT_MAX);
        QCOMPARE(backend.articleIndex(), 4);
        backend.moveArticle(INT_MIN);
        QCOMPARE(backend.articleIndex(), 0);
        backend.moveSource(INT_MIN);
        QCOMPARE(backend.sourceKind(), QString("all"));
    }

    // A failure part way through an import leaves nothing behind.
    void importIsAllOrNothing()
    {
        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.path() + "/library.db"));
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "importIsAllOrNothing");
        db.setDatabaseName(dir.path() + "/library.db");
        QVERIFY(db.open());
        QVERIFY(QSqlQuery(db).exec("CREATE TRIGGER refuse BEFORE INSERT ON feeds WHEN NEW.url LIKE '%refused%'"
                                   " BEGIN SELECT RAISE(ABORT, 'refused by the test'); END"));
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase("importIsAllOrNothing");

        const QList<Opml::Outline> outlines{{"A", "https://a.example/feed", {}, {}, {}},
                                            {"B", "https://b.example/refused", {}, "Folder", {}},
                                            {"C", "https://c.example/feed", {}, {}, {}}};
        const Library::Import result = Library::importOutlines(store, outlines);
        QVERIFY2(result.error.contains("refused by the test"), qPrintable(result.error));
        QCOMPARE(result.added, 0);
        QVERIFY(store.feeds().isEmpty());
        QVERIFY(store.lastError().isEmpty());
        // The store still works after the rollback.
        QCOMPARE(Library::importOutlines(store, {outlines.at(0), outlines.at(2)}).added, 2);
    }

    void importKeepsAnNprofilesRelayHints()
    {
        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.path() + "/library.db"));
        const QString nprofile =
            "nprofile1qqsrhuxx8l9ex335q7he0f09aej04zpazpl0ne2cgukyawd24mayt8gpp4mhxue69uhhytnc9e3k7mgpz4mhxue69uhkg6nzv9e"
            "juumpv34kytnrdaksjlyr9p";
        const QString npub = Nip19::npub(Nip19::decodeProfile(nprofile)->pubkey);
        const QString other = Nip19::npub(QString(64, 'e'));
        const Library::Import result = Library::importOutlines(
            store, {{"Hinted", {}, nprofile, {}, {}}, {"Again", {}, npub, {}, {}}, {"Plain", {}, other, {}, {}}});
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.added, 2);
        QCOMPARE(result.skipped, 1);
        for (const Feed &feed : store.feeds())
            QCOMPARE(feed.relayHints, feed.url == npub ? QStringList({"wss://r.x.com", "wss://djbas.sadkb.com"})
                                                       : QStringList());
    }

    void cliRefusesABadImportBeforeCreatingTheLibrary()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", (dir.path() + "/home").toUtf8());
        QString out, err;
        QCOMPARE(run({"import", dir.path() + "/missing.opml"}, out, err), 1);
        QVERIFY2(err.contains("cannot read"), qPrintable(err));
        write(dir.path() + "/bad.opml", "<opml><body><outline");
        QCOMPARE(run({"import", dir.path() + "/bad.opml"}, out, err), 1);
        QVERIFY(!QFile::exists(dir.path() + "/home"));
    }

    // Slots are the Fetcher's, not each batch's.
    void fetcherSharesItsSlotsAcrossBatches()
    {
        FakeHttpServer server;
        QList<FetchRequest> first, second;
        for (int i = 0; i < 6; ++i) {
            const QByteArray path = "/feed" + QByteArray::number(i);
            server.route(path, [path](const FakeHttpServer::Request &) {
                FakeHttpServer::Response response{200, {}, path};
                response.delayMs = 150;
                return response;
            });
            (i < 3 ? first : second) << FetchRequest{server.url(QString::fromLatin1(path)), {}, {}};
        }
        Fetcher fetcher;
        fetcher.setLimits({1024 * 1024, 5000, 5, 2});
        int done = 0;
        fetcher.fetchEachLater(first, [](qsizetype, const FetchResult &) {}, [&done] { ++done; });
        fetcher.fetchEachLater(second, [](qsizetype, const FetchResult &) {}, [&done] { ++done; });
        QTRY_COMPARE(done, 2);
        QCOMPARE(server.requests.size(), 6);
        QCOMPARE(server.peakInFlight, 2);

        // A held slot stays taken until it is released.
        QList<Fetcher::Release> held;
        bool finished = false;
        fetcher.fetchEachHeld(
            first, [&held](qsizetype, const FetchResult &, const Fetcher::Release &release) { held << release; },
            [&finished] { finished = true; });
        QTRY_COMPARE(held.size(), 2);
        QTest::qWait(400);
        QCOMPARE(held.size(), 2);
        QCOMPARE(server.requests.size(), 8);
        held.at(0)();
        held.at(0)();
        QTRY_COMPARE(held.size(), 3);
        QVERIFY(!finished);
        held.at(1)();
        held.at(2)();
        QTRY_VERIFY(finished);
    }

    // fetchEach() calls the caller's own callable, not a copy of it.
    void fetcherCallsTheCallersCallback()
    {
        FakeHttpServer server;
        server.serve("/a", "a");
        Fetcher fetcher;
        int seen = 0;
        const std::function<void(qsizetype, const FetchResult &)> each =
            [count = 0, &seen](qsizetype, const FetchResult &) mutable { seen = ++count; };
        fetcher.fetchEach({{server.url("/a"), {}, {}}, {server.url("/a"), {}, {}}}, each);
        QCOMPARE(seen, 2);
        each(0, {});
        QCOMPARE(seen, 3);
    }

    // While the worker cannot save (another process holds the write lock),
    // the refresh stops fetching instead of piling bodies up for it.
    void backendBoundsBodiesWaitingForTheWorker()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        {
            Store store;
            QVERIFY(store.open());
            for (int i = 0; i < 12; ++i) {
                const QByteArray path = "/f" + QByteArray::number(i) + ".xml";
                server.serve(path, fixture("rss2.xml"));
                QVERIFY(store.addFeed(server.url(QString::fromLatin1(path)).toString(), QString("F%1").arg(i)));
            }
        }
        Backend backend;
        backend.setFetchLimits({1024 * 1024, 30000, 5, 2});
        QSignalSpy finished(&backend, &Backend::refreshFinished);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "locker");
            db.setDatabaseName(Paths::databaseFile());
            QVERIFY(db.open());
            QVERIFY(QSqlQuery(db).exec("BEGIN IMMEDIATE"));
            backend.refreshAll();
            QTest::qWait(1200);
            QCOMPARE(server.requests.size(), 2);
            QVERIFY(QSqlQuery(db).exec("COMMIT"));
            db.close();
        }
        QSqlDatabase::removeDatabase("locker");
        QVERIFY(finished.wait(30000));
        QCOMPARE(server.requests.size(), 12);
        QCOMPARE(backend.refreshDone(), 12);
        QVERIFY2(!backend.statusError(), qPrintable(backend.status()));
    }

    // Flags change at once even while a refresh holds the library's write
    // lock, and land once it is free.
    void backendNeverWaitsToChangeFlags()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedLibrary();
        Backend backend;
        qint64 id = 0;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "locker");
            db.setDatabaseName(Paths::databaseFile());
            QVERIFY(db.open());
            QVERIFY(QSqlQuery(db).exec("BEGIN IMMEDIATE"));
            QElapsedTimer timer;
            timer.start();
            backend.selectArticle(0);
            id = backend.articleId();
            backend.toggleStar();
            QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
            QVERIFY(backend.articleRead() && backend.articleStarred());
            // A reload meanwhile shows what is about to be saved.
            backend.selectSource(0);
            QCOMPARE(backend.articles()->at(0).id, id);
            QVERIFY(backend.articles()->at(0).read && backend.articles()->at(0).starred);
            QVERIFY(QSqlQuery(db).exec("COMMIT"));
            db.close();
        }
        QSqlDatabase::removeDatabase("locker");
        Store store;
        QVERIFY(store.open());
        QTRY_VERIFY(articleIs(store, id, [](const Article &a) { return a.starred && a.read; }));
        QTRY_COMPARE(backend.sources()->data(backend.sources()->index(2), SourceModel::CountRole).toInt(), 1);
        QVERIFY(!backend.statusError());
    }

    // The selection follows its source when another process moves rows.
    void backendKeepsTheSelectedSourceWhenRowsMove()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const auto [first, second] = seedLibrary();
        Backend backend;
        backend.selectSource(backend.sources()->rowOf(SourceModel::Kind::Feed, second));
        const int before = backend.sourceIndex();
        Store other;
        QVERIFY(other.open());
        QVERIFY(other.addFeed("https://aardvark.example.com/feed", "Aardvark"));
        backend.selectArticle(0);
        QTRY_COMPARE(backend.feedCount(), 3);
        QCOMPARE(backend.selectedFeedId(), second);
        QCOMPARE(backend.sourceIndex(), before + 1);
        QCOMPARE(backend.sourceIndex(), backend.sources()->rowOf(SourceModel::Kind::Feed, second));

        // The selected feed removed elsewhere: back to All articles.
        QVERIFY(other.removeFeed(second));
        backend.markViewRead();
        QTRY_COMPARE(backend.sourceKind(), QString("all"));
        QCOMPARE(backend.articles()->count(), 3);
        QVERIFY(!backend.hasArticle());
    }

    // A read article stays in Unread, in its place, until the view changes.
    void backendKeepsTheOpenArticleInItsView()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const auto [first, second] = seedLibrary();
        Backend backend;
        backend.selectSource(backend.sources()->rowOf(SourceModel::Kind::Unread));
        QCOMPARE(backend.articles()->count(), 4);
        backend.selectArticle(2);
        QCOMPARE(backend.articleTitle(), QString("Older"));
        Store store;
        QVERIFY(store.open());
        QTRY_VERIFY(unreadIs(store, 3));
        // A reload in the background (here, an unsubscribe).
        QVERIFY(backend.unsubscribe(second));
        QTRY_COMPARE(backend.feedCount(), 1);
        QCOMPARE(backend.articles()->count(), 3);
        QCOMPARE(backend.articleIndex(), 1);
        QCOMPARE(backend.articles()->at(1).title, QString("Older"));
        backend.moveArticle(1);
        QCOMPARE(backend.articleTitle(), QString("Oldest"));
        backend.selectSource(backend.sources()->rowOf(SourceModel::Kind::All));
        backend.selectSource(backend.sources()->rowOf(SourceModel::Kind::Unread));
        QTRY_VERIFY(unreadIs(store, 1));
        backend.selectSource(backend.sources()->rowOf(SourceModel::Kind::All));
        backend.selectSource(backend.sources()->rowOf(SourceModel::Kind::Unread));
        QCOMPARE(backend.articles()->count(), 1);
    }

    // A refresh that changes the open article shows the new text.
    void backendRefreshesTheOpenArticle()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        QByteArray text = "Old words";
        server.route("/feed.xml", [&text](const FakeHttpServer::Request &) {
            return FakeHttpServer::Response{200,
                                            {{"Content-Type", "application/rss+xml"}},
                                            "<rss version=\"2.0\"><channel><title>T</title><item><guid>g</guid>"
                                            "<title>Item</title><link>https://a.example/item</link><description>"
                                                + text + "</description></item></channel></rss>"};
        });
        {
            Store store;
            QVERIFY(store.open());
            QVERIFY(store.addFeed(server.url("/feed.xml").toString(), "Feed"));
        }
        Backend backend;
        QSignalSpy finished(&backend, &Backend::refreshFinished);
        backend.refreshAll();
        QVERIFY(finished.wait(20000));
        backend.selectArticle(0);
        const qint64 id = backend.articleId();
        QVERIFY2(backend.articleHtml().contains("Old words"), qPrintable(backend.articleHtml()));
        text = "New words";
        backend.refreshAll();
        QVERIFY(finished.wait(20000));
        QCOMPARE(backend.articleId(), id);
        QVERIFY2(backend.articleHtml().contains("New words"), qPrintable(backend.articleHtml()));
    }

    void backendRetriesARefreshThatCannotReadTheLibrary()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        Backend backend;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "breaker");
            db.setDatabaseName(Paths::databaseFile());
            QVERIFY(db.open());
            QVERIFY(QSqlQuery(db).exec("ALTER TABLE feeds RENAME TO feeds_gone"));
            db.close();
        }
        QSqlDatabase::removeDatabase("breaker");
        backend.startAutoRefresh();
        QVERIFY(backend.statusError());
        QVERIFY(backend.nextAutoRefreshMs() > 0);
    }

    // An unsubscribe that races the refresh fetching the feed reports
    // nothing and leaves no articles without a feed.
    void backendUnsubscribesDuringARefresh()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        server.route("/slow.xml", [](const FakeHttpServer::Request &) {
            FakeHttpServer::Response response{200, {{"Content-Type", "application/rss+xml"}}, fixture("rss2.xml")};
            response.delayMs = 800;
            return response;
        });
        server.serve("/fast.xml", fixture("atom.xml"), "application/atom+xml");
        qint64 slow = 0;
        {
            Store store;
            QVERIFY(store.open());
            slow = store.addFeed(server.url("/slow.xml").toString(), "Slow");
            QVERIFY(store.addFeed(server.url("/fast.xml").toString(), "Fast"));
        }
        Backend backend;
        QSignalSpy finished(&backend, &Backend::refreshFinished);
        backend.refreshAll();
        QVERIFY(backend.unsubscribe(slow));
        QVERIFY(finished.wait(20000));
        QTRY_COMPARE(backend.feedCount(), 1);
        QVERIFY2(!backend.statusError(), qPrintable(backend.status()));
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "orphans");
        db.setDatabaseName(Paths::databaseFile());
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec("SELECT COUNT(*) FROM items WHERE feed_id NOT IN (SELECT id FROM feeds)") && query.next());
        QCOMPARE(query.value(0).toInt(), 0);
        query = QSqlQuery();
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase("orphans");
    }

    // An unsubscribe hides the feed at once, takes a repeat quietly, and
    // gives the feed back, errors and all, when the delete fails.
    void backendUnsubscribesOptimistically()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        qint64 kept = 0, gone = 0;
        {
            Store store;
            QVERIFY(store.open());
            kept = store.addFeed(server.url("/kept.xml").toString(), "Kept");
            gone = store.addFeed(server.url("/gone.xml").toString(), "Gone");
        }
        Backend backend;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "locker");
            db.setDatabaseName(Paths::databaseFile());
            QVERIFY(db.open());
            QVERIFY(QSqlQuery(db).exec("BEGIN IMMEDIATE"));
            QVERIFY(backend.unsubscribe(gone));
            QCOMPARE(backend.feedCount(), 1);
            QCOMPARE(backend.sources()->rowOf(SourceModel::Kind::Feed, gone), -1);
            QVERIFY(backend.unsubscribe(gone));
            QVERIFY(!backend.statusError());
            QVERIFY(QSqlQuery(db).exec("COMMIT"));
            QTRY_VERIFY(backend.status().startsWith("Unsubscribed from Gone"));
            QVERIFY(!backend.statusError());
            QCOMPARE(backend.feedCount(), 1);
            QVERIFY(QSqlQuery(db).exec("CREATE TRIGGER keep BEFORE DELETE ON feeds"
                                       " BEGIN SELECT RAISE(ABORT, 'kept by the test'); END"));
            db.close();
        }
        QSqlDatabase::removeDatabase("locker");

        QVERIFY(backend.unsubscribe(kept));
        QCOMPARE(backend.feedCount(), 0);
        QTRY_VERIFY(backend.statusError());
        QVERIFY2(backend.status().contains("kept by the test"), qPrintable(backend.status()));
        QCOMPARE(backend.feedCount(), 1);
        // Its refresh errors show again.
        QSignalSpy finished(&backend, &Backend::refreshFinished);
        backend.refreshAll();
        QVERIFY(finished.wait(20000));
        QVERIFY2(backend.statusError() && backend.status().contains("HTTP 404"), qPrintable(backend.status()));
    }

    // Feeds imported while a refresh runs are fetched right after it.
    void backendFetchesFeedsImportedDuringARefresh()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", (dir.path() + "/home").toUtf8());
        FakeHttpServer server;
        server.route("/slow.xml", [](const FakeHttpServer::Request &) {
            FakeHttpServer::Response response{200, {{"Content-Type", "application/rss+xml"}}, fixture("rss2.xml")};
            response.delayMs = 800;
            return response;
        });
        server.serve("/new.xml", fixture("atom.xml"), "application/atom+xml");
        {
            Store store;
            QVERIFY(store.open());
            QVERIFY(store.addFeed(server.url("/slow.xml").toString(), "Slow"));
        }
        write(dir.path() + "/in.opml", "<opml><body><outline type=\"rss\" text=\"New\" xmlUrl=\""
                                           + server.url("/new.xml").toString().toUtf8() + "\"/></body></opml>");
        FakePicker picker;
        Backend backend(&picker);
        QSignalSpy finished(&backend, &Backend::refreshFinished);
        QSignalSpy imported(&backend, &Backend::imported);
        backend.refreshAll();
        picker.openUrl = QUrl::fromLocalFile(dir.path() + "/in.opml");
        backend.importOpml();
        QVERIFY(imported.wait(20000));
        QVERIFY(backend.refreshing());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 20000);
        QCOMPARE(backend.feedCount(), 2);
        QCOMPARE(backend.articles()->count(), 4 + 3);
    }

    // Closing the window with a fetch out and work queued on the worker:
    // nothing runs after, and the user's own changes are still saved.
    void backendClosesWithWorkInFlight()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        server.serve("/fast.xml", fixture("atom.xml"), "application/atom+xml");
        server.route("/slow.xml", [](const FakeHttpServer::Request &) {
            FakeHttpServer::Response response{200, {{"Content-Type", "application/rss+xml"}}, fixture("rss2.xml")};
            response.delayMs = 2000;
            return response;
        });
        {
            Store store;
            QVERIFY(store.open());
            const qint64 fast = store.addFeed(server.url("/fast.xml").toString(), "Fast");
            QVERIFY(store.addFeed(server.url("/slow.xml").toString(), "Slow"));
            ParsedItem item;
            item.guid = item.title = "kept";
            QCOMPARE(store.saveItems(fast, {item}), 1);
        }
        auto backend = std::make_unique<Backend>();
        // A writer in another thread holds the lock for a while, so the
        // fast feed's save blocks the worker and the flags queue behind it.
        // 0 while it tries, 1 once it holds the lock, -1 when it cannot.
        std::atomic<int> locked = 0;
        std::thread locker([&locked, path = Paths::databaseFile()] {
            {
                QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "threadLocker");
                db.setDatabaseName(path);
                if (db.open() && QSqlQuery(db).exec("BEGIN IMMEDIATE")) {
                    locked = 1;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                    QSqlQuery(db).exec("COMMIT");
                } else {
                    locked = -1;
                }
                db.close();
            }
            QSqlDatabase::removeDatabase("threadLocker");
        });
        // Joined however the test ends, a failed check included.
        const auto joinLocker = qScopeGuard([&locker] {
            if (locker.joinable())
                locker.join();
        });
        QTRY_VERIFY(locked.load() != 0);
        QCOMPARE(locked.load(), 1);
        backend->refreshAll();
        QTRY_VERIFY(server.requests.size() == 2);
        QTest::qWait(300);
        backend->selectArticle(0);
        const qint64 id = backend->articleId();
        backend->toggleStar();
        QVERIFY(id);
        backend.reset();
        locker.join();
        // The slow answer arrives after the window has gone.
        QTest::qWait(2500);
        Store store;
        QVERIFY(store.open());
        const std::optional<Article> saved = store.article(id);
        QVERIFY(saved);
        QVERIFY(saved->read);
        QVERIFY(saved->starred);
    }

    // The article open in the window is listed even past the limit, in
    // date order, and still only within its feed.
    void storeKeepsTheOpenArticlePastTheLimit()
    {
        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.filePath("library.sqlite")));
        const qint64 feed = store.addFeed("https://a.example.com/feed", "A");
        const qint64 other = store.addFeed("https://b.example.com/feed", "B");
        QList<ParsedItem> items;
        for (int day = 1; day <= 6; ++day) {
            ParsedItem item;
            item.guid = item.title = QString("day%1").arg(day);
            item.published = QDateTime(QDate(2026, 1, day), QTime(12, 0), QTimeZone::UTC);
            items << item;
        }
        QCOMPARE(store.saveItems(feed, items), 6);
        ParsedItem late;
        late.guid = late.title = "other";
        late.published = QDateTime(QDate(2026, 2, 1), QTime(12, 0), QTimeZone::UTC);
        QCOMPARE(store.saveItems(other, {late}), 1);
        qint64 oldest = 0;
        for (const Article &article : store.articles({}))
            if (article.guid == "day1")
                oldest = article.id;
        QVERIFY(oldest);
        QVERIFY(store.setRead(oldest, true));

        ArticleFilter filter;
        filter.limit = 3;
        filter.alsoId = oldest;
        auto titles = [&store](const ArticleFilter &filter) {
            QStringList titles;
            for (const Article &article : store.articles(filter))
                titles << article.title;
            return titles;
        };
        QCOMPARE(titles(filter), QStringList({"other", "day6", "day5", "day1"}));
        filter.unreadOnly = true;
        QCOMPARE(titles(filter), QStringList({"other", "day6", "day5", "day1"}));
        filter.feedId = feed;
        QCOMPARE(titles(filter), QStringList({"day6", "day5", "day4", "day1"}));
        filter.feedId = other;
        QCOMPARE(titles(filter), QStringList({"other"}));
        filter.feedId = 0;
        filter.unreadOnly = false;
        qint64 recent = 0;
        for (const Article &article : store.articles({}))
            if (article.guid == "day6")
                recent = article.id;
        QVERIFY(recent);
        filter.alsoId = recent;
        QCOMPARE(titles(filter), QStringList({"other", "day6", "day5", "day4"}));
        filter.unreadOnly = true;
        QCOMPARE(titles(filter), QStringList({"other", "day6", "day5", "day4"}));
        filter.unreadOnly = false;
        filter.alsoId = 0;
        QCOMPARE(titles(filter), QStringList({"other", "day6", "day5"}));
        QVERIFY(store.lastError().isEmpty());
    }

    // A transaction whose work throws is rolled back, and the exception
    // goes on to the caller.
    void storeRollsBackAThrowingTransaction()
    {
        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.filePath("library.sqlite")));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, store.transaction([&store]() -> bool {
            store.addFeed("https://a.example.com/feed", "A");
            throw std::runtime_error("boom");
        }));
        QVERIFY(store.feeds().isEmpty());
        QVERIFY(store.transaction([&store] { return store.addFeed("https://b.example.com/feed", "B") != 0; }));
        QCOMPARE(store.feeds().size(), 1);
    }

    // The sanitizer drops bidi controls from text and image labels, as the
    // feed parser does from every other field.
    void sanitizerDropsBidiControls()
    {
        const QUrl base("https://blog.example.com/");
        QCOMPARE(Sanitizer::toRichText(QString(u"<p>a\u202eb&#x2067;c&#8207;d</p>"), base), QString(u"<p>abc\u200fd</p>"));
        QCOMPARE(Sanitizer::toRichText(QString(u"<img src=\"/i.png\" alt=\"x\u202ey&#x2066;z\">"), base),
                 QString("<a href=\"https://blog.example.com/i.png\">[image: xyz]</a>"));
        QCOMPARE(Sanitizer::toRichText(QString(u"<img src=\"/i.png\" alt=\"\u202e\">"), base),
                 QString("<a href=\"https://blog.example.com/i.png\">[image]</a>"));
    }

    // A slot released on another thread is given back on the Fetcher's.
    void fetcherTakesAReleaseFromAnotherThread()
    {
        FakeHttpServer server;
        server.serve("/a", "a");
        Fetcher fetcher;
        QList<Fetcher::Release> held;
        QThread *finishedOn = nullptr;
        fetcher.fetchEachHeld(
            {FetchRequest{server.url("/a"), {}, {}}},
            [&held](qsizetype, const FetchResult &, const Fetcher::Release &release) { held << release; },
            [&finishedOn] { finishedOn = QThread::currentThread(); });
        QTRY_COMPARE(held.size(), 1);
        std::thread([release = held.at(0)] { release(); }).join();
        QVERIFY(!finishedOn);
        QTRY_VERIFY(finishedOn);
        QCOMPARE(finishedOn, QThread::currentThread());
    }

    // Copied text loses the zero-width spaces that break long words, and
    // reads as plain text with line breaks.
    void backendCopiesPlainText()
    {
        QCOMPARE(Backend::copyableText(QString(u"long\u200bword\u2029next\u2028line")),
                 QString("longword\nnext\nline"));
        QTextDocument document;
        document.setHtml(Sanitizer::toRichText(QString(500, 'x') + "<p>two</p>", QUrl("https://a.example/")));
        QTextCursor cursor(&document);
        cursor.select(QTextCursor::Document);
        QCOMPARE(Backend::copyableText(cursor.selectedText()), QString(500, 'x') + "\ntwo");

        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        Backend backend;
        backend.copyText(QString(u"a\u200bb"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("ab"));
    }

    // A flag change on an article that has gone meanwhile clears it from the
    // list and the reader rather than leaving them stale.
    void backendDropsAnArticleThatWentWhileSaving()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const auto [first, second] = seedLibrary();
        Backend backend;
        backend.selectArticle(0);
        const qint64 id = backend.articleId();
        Store store;
        QVERIFY(store.open());
        QTRY_VERIFY(articleIs(store, id, [](const Article &a) { return a.read; }));
        QVERIFY(store.removeFeed(first));
        backend.toggleStar();
        QVERIFY(backend.articleStarred());
        QTRY_VERIFY(!backend.hasArticle());
        QCOMPARE(backend.articles()->count(), 2);
        QCOMPARE(backend.articles()->rowOf(id), -1);
        QVERIFY(backend.statusError());
    }

    // The selected feed renamed in place still announces its new title.
    void backendFollowsTheSelectedSourcesTitle()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const auto [first, second] = seedLibrary();
        Backend backend;
        backend.selectSource(backend.sources()->rowOf(SourceModel::Kind::Feed, second));
        const int row = backend.sourceIndex();
        QSignalSpy changed(&backend, &Backend::sourceChanged);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "renamer");
            db.setDatabaseName(Paths::databaseFile());
            QVERIFY(db.open());
            QSqlQuery query(db);
            query.prepare("UPDATE feeds SET title = 'Second renamed' WHERE id = ?");
            query.addBindValue(second);
            QVERIFY(query.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase("renamer");
        backend.selectArticle(0);
        QTRY_VERIFY(!changed.isEmpty());
        QCOMPARE(backend.sourceIndex(), row);
        QCOMPARE(backend.sourceTitle(), QString("Second renamed"));
    }

    // At close, a write the worker saved is not done again, and one that
    // failed, with no window left to say so, is reported on stderr.
    void backendReportsFailedWritesAtClose()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const auto [first, second] = seedLibrary();
        auto backend = std::make_unique<Backend>();
        backend->selectArticle(0);
        const qint64 id = backend->articleId();
        Store store;
        QVERIFY(store.open());
        QTRY_VERIFY(articleIs(store, id, [](const Article &a) { return a.read; }));
        QVERIFY(store.removeFeed(first));
        backend->toggleStar();
        QVERIFY(backend->unsubscribe(second));
        // The worker gets to both, but the window never hears back: this
        // thread waits without taking its events.
        QDeadlineTimer deadline(5000);
        while (store.feed(second) && !deadline.hasExpired())
            QThread::msleep(20);
        QVERIFY(!store.feed(second));
        QTest::failOnWarning(QRegularExpression("Cannot unsubscribe"));
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QString("^readstr: Cannot change the article: no article with id %1$")
                                                    .arg(id)));
        backend.reset();
    }

    void backendStripsCredentialsAndOpensOnlyLocalFiles()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        {
            Store store;
            QVERIFY(store.open());
            const qint64 feed = store.addFeed("https://a.example.com/feed", "A");
            ParsedItem item;
            item.guid = item.title = "one";
            item.url = "https://me:secret@a.example.com/one";
            QCOMPARE(store.saveItems(feed, {item}), 1);
        }
        FakePicker picker;
        Backend backend(&picker);
        QList<QUrl> opened;
        backend.setUrlOpener([&opened](const QUrl &url) {
            opened << url;
            return true;
        });
        backend.selectArticle(0);
        QCOMPARE(backend.articleUrl(), QString("https://a.example.com/one"));
        backend.openOriginal();
        QVERIFY(backend.openLink("http://user@b.example.org/x"));
        QCOMPARE(opened, (QList<QUrl>{QUrl("https://a.example.com/one"), QUrl("http://b.example.org/x")}));

        backend.importFrom(QUrl("file://remotehost/tmp/in.opml"));
        QVERIFY(backend.status().contains("Only local files"));
        backend.exportTo(QUrl("file://remotehost/tmp/out.opml"));
        QVERIFY(backend.status().contains("Only local files"));
        backend.exportTo(QUrl("file://localhost" + dir.path() + "/out.opml"));
        QVERIFY2(!backend.statusError(), qPrintable(backend.status()));
        QVERIFY(QFile::exists(dir.path() + "/out.opml"));
    }

    void portalPickerFollowsItsRequest()
    {
        const QVariantMap chosen{{"uris", QStringList{"file:///home/me/feeds.opml"}}};
        FakePortalPicker picker;
        QSignalSpy opened(&picker, &FilePicker::openSelected);
        QSignalSpy saved(&picker, &FilePicker::saveSelected);
        QSignalSpy failed(&picker, &FilePicker::failed);

        // Listening at the predicted path before the call goes out.
        picker.openOpml();
        QCOMPARE(picker.calls.size(), 1);
        QCOMPARE(picker.calls.at(0).member(), QString("OpenFile"));
        QCOMPARE(picker.calls.at(0).service(), QString("org.freedesktop.portal.Desktop"));
        QCOMPARE(picker.listening, QStringList{picker.requestPath(0)});
        // The answer before the reply: taken, and the reply then ignored.
        picker.responded(picker.requestPath(0), 0, chosen);
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.at(0).at(0).toUrl(), QUrl("file:///home/me/feeds.opml"));
        QVERIFY(picker.listening.isEmpty());
        picker.replied(picker.generations.at(0), picker.requestPath(0), {});
        QVERIFY(picker.listening.isEmpty());
        QCOMPARE(failed.count(), 0);

        // A request replaced by a newer one is closed and its answer dropped.
        picker.openOpml();
        picker.saveOpml("readstr-subscriptions.opml");
        QCOMPARE(picker.calls.at(2).member(), QString("SaveFile"));
        QCOMPARE(picker.closed, QStringList{picker.requestPath(1)});
        picker.responded(picker.requestPath(1), 0, chosen);
        picker.replied(picker.generations.at(1), "/elsewhere", {});
        QCOMPARE(opened.count(), 1);
        QCOMPARE(picker.listening, QStringList{picker.requestPath(2)});
        picker.responded(picker.requestPath(2), 0, chosen);
        QCOMPARE(saved.count(), 1);

        // The portal names another path: the answer comes from there only.
        picker.openOpml();
        picker.replied(picker.generations.at(3), "/org/freedesktop/portal/desktop/request/1_42/other", {});
        QCOMPARE(picker.listening, QStringList{"/org/freedesktop/portal/desktop/request/1_42/other"});
        picker.responded(picker.requestPath(3), 0, chosen);
        QCOMPARE(opened.count(), 1);
        picker.responded("/org/freedesktop/portal/desktop/request/1_42/other", 0, chosen);
        QCOMPARE(opened.count(), 2);

        // Cancelled says nothing; ended otherwise, or a failed call, says so.
        picker.openOpml();
        picker.responded(picker.requestPath(4), 1, {});
        QCOMPARE(failed.count(), 0);
        picker.openOpml();
        picker.responded(picker.requestPath(5), 2, {});
        QCOMPARE(failed.count(), 1);
        picker.openOpml();
        picker.replied(picker.generations.at(6), {}, "boom");
        QCOMPARE(failed.count(), 2);
        QCOMPARE(failed.at(1).at(0).toString(), QString("The file chooser failed: boom"));
        QVERIFY(picker.listening.isEmpty());
        picker.bus = false;
        picker.openOpml();
        QCOMPARE(failed.count(), 3);
        QCOMPARE(picker.calls.size(), 7);
    }

    // Rounding follows the event socket without polling; a failed query is
    // tried once more, and a flood of reloads asks only now and then.
    void themeRetriesOnceAndDebouncesReloads()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("hypr") && QDir(dir.path()).mkpath("current"));
        const QString count = dir.filePath("hypr/count"), command = dir.filePath("hypr/hyprctl");
        write(command, "#!/bin/sh\nn=$(cat '" + count.toUtf8() + "' 2>/dev/null || echo 0)\nn=$((n+1))\necho $n > '"
                           + count.toUtf8() + "'\n[ $n -le 1 ] && exit 1\necho '{\"int\":4}'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QLocalServer server;
        QVERIFY(server.listen(dir.filePath("hypr/socket2.sock")));
        auto runs = [&count] { return fixtureAt(count).trimmed().toInt(); };

        Theme theme(dir.filePath("current"), nullptr, command, server.fullServerName());
        QVERIFY(server.waitForNewConnection(5000) || server.hasPendingConnections());
        QLocalSocket *events = server.nextPendingConnection();
        QVERIFY(events);
        QTRY_VERIFY(theme.listening());
        QTRY_COMPARE_WITH_TIMEOUT(theme.radius(), 4, 6000);
        QCOMPARE(runs(), 2);
        QTest::qWait(2500);
        QCOMPARE(runs(), 2);

        for (int i = 0; i < 40; ++i) {
            events->write("configreloaded>>\n");
            events->flush();
            QTest::qWait(10);
        }
        QTest::qWait(600);
        QVERIFY2(runs() - 2 >= 1 && runs() - 2 <= 5, qPrintable(QString::number(runs())));
    }

    // QML reaches no network: a remote image or rich text image is never
    // requested.
    void qmlReachesNoNetwork()
    {
        QVERIFY(LocalNetworkAccessManager::allowed(QUrl("qrc:/Main.qml")));
        QVERIFY(LocalNetworkAccessManager::allowed(QUrl("file:///usr/share/icons/x.png")));
        for (const char *url : {"http://a.example/x.png", "https://a.example/", "file://host/x.png", "data:image/png,x",
                                "ftp://a.example/x"})
            QVERIFY2(!LocalNetworkAccessManager::allowed(QUrl(url)), url);

        FakeHttpServer server;
        server.serve("/pixel.png", "not really a png", "image/png");
        const QByteArray url = server.url("/pixel.png").toString().toUtf8();
        const QByteArray qml = "import QtQuick\nItem {\n property alias status: image.status\n"
                               " Image { id: image; source: \"" + url + "\" }\n"
                               " Text { textFormat: Text.RichText; text: \"<img src='" + url + "?text'>\" }\n}\n";
        auto load = [&qml](QQmlEngine &engine) {
            QQmlComponent component(&engine);
            component.setData(qml, QUrl("qrc:/probe.qml"));
            std::unique_ptr<QObject> item(component.create());
            if (!item)
                qWarning() << component.errors();
            return item;
        };
        {
            LocalNetworkFactory factory;
            QQmlEngine engine;
            engine.setNetworkAccessManagerFactory(&factory);
            const auto item = load(engine);
            QVERIFY(item);
            QTRY_COMPARE(item->property("status").toInt(), 3);
            QTest::qWait(300);
            QVERIFY(server.requests.isEmpty());
        }
        // The probe does reach the server without the factory.
        QQmlEngine engine;
        const auto item = load(engine);
        QVERIFY(item);
        QTRY_VERIFY(!server.requests.isEmpty());
    }

    void nip19DecodesNpubAndNprofile()
    {
        const QString hex = "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d";
        const QString npub = "npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6";
        QCOMPARE(Nip19::npub(hex), npub);
        QCOMPARE(Nip19::decodeProfile(npub)->pubkey, hex);
        QCOMPARE(Nip19::decodeProfile("nostr:" + npub.toUpper())->pubkey, hex);
        QVERIFY(Nip19::decodeProfile(" " + npub + "\n"));

        // The example from NIP-19.
        const auto profile = Nip19::decodeProfile(
            "nprofile1qqsrhuxx8l9ex335q7he0f09aej04zpazpl0ne2cgukyawd24mayt8gpp4mhxue69uhhytnc9e3k7mgpz4mhxue69uhkg6nzv9e"
            "juumpv34kytnrdaksjlyr9p");
        QVERIFY(profile);
        QCOMPARE(profile->pubkey, hex);
        QCOMPARE(profile->relays, QStringList({"wss://r.x.com", "wss://djbas.sadkb.com"}));

        QVERIFY(!Nip19::decodeProfile(npub.left(npub.size() - 1) + 'q'));
        QVERIFY(!Nip19::decodeProfile("npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6W6"));
        QVERIFY(!Nip19::decodeProfile("note1" + npub.mid(5)));
        QVERIFY(!Nip19::decodeProfile("npub1"));
        QVERIFY(!Nip19::decodeProfile(QString(5000, 'q')));
        QVERIFY(!Nip19::decodeProfile("https://example.com/feed"));
        QVERIFY(Nip19::npub("not hex").isEmpty());

        const QString naddr = Nip19::naddr(30023, hex, "my-post");
        QVERIFY(naddr.startsWith("naddr1"));
        QCOMPARE(naddr, Nip19::naddr(30023, hex, "my-post"));
        QVERIFY(naddr != Nip19::naddr(30023, hex, "other"));
        QVERIFY(Nip19::naddr(30023, hex, QString(300, 'x')).isEmpty());
    }

    void nostrEventVerifiesItsSignature()
    {
        const QJsonObject json = signedEvent(30023, "Line one\n\"quoted\" \\ \u0001 é 😀",
                                             {{"d", "post"}, {"title", "Hello"}}, 1700000000);
        auto event = NostrEvent::fromJson(json);
        QVERIFY(event);
        QVERIFY(event->verify());
        QCOMPARE(event->tag("title"), QString("Hello"));
        QCOMPARE(event->tag("missing"), QString());
        // JSON.stringify's escaping, which the id is the hash of.
        QVERIFY(event->serialized().endsWith(
            QByteArray("[[\"d\",\"post\"],[\"title\",\"Hello\"]],\"Line one\\n\\\"quoted\\\" \\\\ \\u0001 ")
            + QString("é 😀").toUtf8() + "\"]"));

        NostrEvent changed = *event;
        changed.content += '!';
        QVERIFY(!changed.verify());
        changed = *event;
        changed.sig[0] = changed.sig[0] == 'a' ? 'b' : 'a';
        QVERIFY(!changed.verify());
        changed = *event;
        changed.pubkey = signedEvent(1, "", {}, 1, 2).value("pubkey").toString();
        QVERIFY(!changed.verify());
        changed = *event;
        ++changed.createdAt;
        QVERIFY(!changed.verify());
        changed = *event;
        changed.tags[0][1] = "other";
        QVERIFY(!changed.verify());

        QJsonObject broken = json;
        broken.insert("created_at", 1.5);
        QVERIFY(!NostrEvent::fromJson(broken));
        broken = json;
        broken.insert("id", json.value("id").toString().toUpper());
        QVERIFY(!NostrEvent::fromJson(broken));
        broken = json;
        broken.insert("tags", QJsonArray{QJsonArray{"d", 1}});
        QVERIFY(!NostrEvent::fromJson(broken));
        broken = json;
        broken.remove("sig");
        QVERIFY(!NostrEvent::fromJson(broken));
        QVERIFY(!NostrEvent::fromJson(QJsonValue("event")));
    }

    // NIP-19's own nsec example, and the same key as hex in either case.
    void nip19DecodesSecretKeys()
    {
        const QString nsec = "nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5";
        const QByteArray key = QByteArray::fromHex("67dea2ed018072d675f5415ecfaed7d2597555e202d85b3d65ea4e58d2d92ffa");
        const auto decoded = [](const QString &text) { return Nip19::secretKey(text).value_or(QByteArray("none")); };
        QCOMPARE(decoded(nsec), key);
        QCOMPARE(decoded(" " + nsec.toUpper() + "\n"), key);
        QCOMPARE(decoded(key.toHex()), key);
        QCOMPARE(decoded(key.toHex().toUpper()), key);
        // Hex in mixed case, read digit by digit, and digits outside ASCII,
        // which a case fold or a toLatin1() copy might have let through.
        QString mixed = key.toHex();
        for (qsizetype i = 0; i < mixed.size(); i += 2)
            mixed[i] = mixed[i].toUpper();
        QCOMPARE(decoded("\t" + mixed + " "), key);
        QVERIFY(!Nip19::secretKey(QString(key.toHex()).replace(0, 1, QChar(0xff10))));
        QVERIFY(!Nip19::secretKey(nsec.left(nsec.size() - 1) + QChar(0x212a)));

        QVERIFY(!Nip19::secretKey(nsec.left(nsec.size() - 1) + 'q'));
        QVERIFY(!Nip19::secretKey(nsec.left(10) + nsec.mid(10).toUpper()));
        QVERIFY(!Nip19::secretKey("nostr:" + nsec));
        // The same bytes as an npub are a public key, not a secret one.
        QVERIFY(!Nip19::secretKey(Nip19::npub(key.toHex())));
        QVERIFY(!Nip19::secretKey(key.toHex().left(63)));
        QVERIFY(!Nip19::secretKey(key.toHex() + "0"));
        QVERIFY(!Nip19::secretKey(QString(64, 'g')));
        QVERIFY(!Nip19::secretKey("nsec1"));
        QVERIFY(!Nip19::secretKey(""));
    }

    // The signing vectors of BIP-340 (test-vectors.csv, indices 0 to 3),
    // signed through LocalSigner and checked again by libsecp256k1.
    void localSignerMatchesTheBip340Vectors_data()
    {
        QTest::addColumn<QByteArray>("secret");
        QTest::addColumn<QByteArray>("pubkey");
        QTest::addColumn<QByteArray>("aux");
        QTest::addColumn<QByteArray>("message");
        QTest::addColumn<QByteArray>("signature");
        QTest::newRow("0") << QByteArray("0000000000000000000000000000000000000000000000000000000000000003")
                           << QByteArray("F9308A019258C31049344F85F89D5229B531C845836F99B08601F113BCE036F9")
                           << QByteArray("0000000000000000000000000000000000000000000000000000000000000000")
                           << QByteArray("0000000000000000000000000000000000000000000000000000000000000000")
                           << QByteArray("E907831F80848D1069A5371B402410364BDF1C5F8307B0084C55F1CE2DCA8215"
                                         "25F66A4A85EA8B71E482A74F382D2CE5EBEEE8FDB2172F477DF4900D310536C0");
        QTest::newRow("1") << QByteArray("B7E151628AED2A6ABF7158809CF4F3C762E7160F38B4DA56A784D9045190CFEF")
                           << QByteArray("DFF1D77F2A671C5F36183726DB2341BE58FEAE1DA2DECED843240F7B502BA659")
                           << QByteArray("0000000000000000000000000000000000000000000000000000000000000001")
                           << QByteArray("243F6A8885A308D313198A2E03707344A4093822299F31D0082EFA98EC4E6C89")
                           << QByteArray("6896BD60EEAE296DB48A229FF71DFE071BDE413E6D43F917DC8DCF8C78DE3341"
                                         "8906D11AC976ABCCB20B091292BFF4EA897EFCB639EA871CFA95F6DE339E4B0A");
        QTest::newRow("2") << QByteArray("C90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B14E5C9")
                           << QByteArray("DD308AFEC5777E13121FA72B9CC1B7CC0139715309B086C960E18FD969774EB8")
                           << QByteArray("C87AA53824B4D7AE2EB035A2B5BBBCCC080E76CDC6D1692C4B0B62D798E6D906")
                           << QByteArray("7E2D58D8B3BCDF1ABADEC7829054F90DDA9805AAB56C77333024B9D0A508B75C")
                           << QByteArray("5831AAEED7B44BB74E5EAB94BA9D4294C49BCF2A60728D8B4C200F50DD313C1B"
                                         "AB745879A5AD954A72C45A91C3A51D3C7ADEA98D82F8481E0E1E03674A6F3FB7");
        QTest::newRow("3") << QByteArray("0B432B2677937381AEF05BB02A66ECD012773062CF3FA2549E44F58ED2401710")
                           << QByteArray("25D1DFF95105F5253C4022F628A996AD3A0D95FBF21D468A1B33F8C160D8F517")
                           << QByteArray("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF")
                           << QByteArray("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF")
                           << QByteArray("7EB0509757E246F19449885651611CB965ECC1A187DD51B64FDA1EDC9637D5EC"
                                         "97582B9CB13DB3933705B32BA982AF5AF25FD78881EBB32771FC5922EFC66EA3");
    }

    void localSignerMatchesTheBip340Vectors()
    {
        QFETCH(QByteArray, secret);
        QFETCH(QByteArray, pubkey);
        QFETCH(QByteArray, aux);
        QFETCH(QByteArray, message);
        QFETCH(QByteArray, signature);
        const auto signer = LocalSigner::create(QByteArray::fromHex(secret));
        QVERIFY(signer);
        QCOMPARE(signer->pubkey(), QString::fromLatin1(pubkey.toLower()));
        QByteArray digest = QByteArray::fromHex(message);
        const QByteArray made = signer->signDigest(digest, QByteArray::fromHex(aux));
        QCOMPARE(made.toHex().toUpper(), signature);

        const auto bytes = [](const QByteArray &data) { return reinterpret_cast<const unsigned char *>(data.constData()); };
        secp256k1_xonly_pubkey xonly;
        QVERIFY(secp256k1_xonly_pubkey_parse(secp256k1_context_static, &xonly, bytes(QByteArray::fromHex(pubkey))));
        QVERIFY(secp256k1_schnorrsig_verify(secp256k1_context_static, bytes(made), bytes(digest), 32, &xonly));
        digest[31] = char(digest.at(31) ^ 1);
        QVERIFY(!secp256k1_schnorrsig_verify(secp256k1_context_static, bytes(made), bytes(digest), 32, &xonly));
        QVERIFY(signer->signDigest(digest.left(31), QByteArray::fromHex(aux)).isEmpty());
    }

    // A secret key is a number from 1 to the group order less one.
    void localSignerRefusesKeysOutOfRange()
    {
        const QByteArray order = QByteArray::fromHex("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141");
        QVERIFY(!LocalSigner::create(QByteArray(32, '\0')));
        QVERIFY(!LocalSigner::create(order));
        QVERIFY(!LocalSigner::create(QByteArray(32, '\xff')));
        QVERIFY(!LocalSigner::create(QByteArray(31, '\1')));
        QVERIFY(!LocalSigner::create(QByteArray(33, '\1')));
        QVERIFY(!LocalSigner::create({}));
        QByteArray largest = order;
        largest[31] = '\x40';
        QVERIFY(LocalSigner::create(largest));
        QVERIFY(LocalSigner::create(QByteArray(31, '\0') + '\1'));
    }

    // What LocalSigner signs is what the app, and so every relay and
    // client, takes from a relay: the same canonical JSON for the id, and a
    // signature that verifies.
    void localSignerSignsEventsTheAppVerifies()
    {
        const auto signer = LocalSigner::create(QByteArray(32, '\1'));
        QVERIFY(signer);
        QCOMPARE(signer->pubkey(), syncPubkey());
        NostrEvent draft;
        draft.kind = Sync::subscriptionsKind;
        draft.createdAt = 1700000000;
        draft.tags = {{"d", Sync::subscriptionsTag}, {"client", "readstr"}};
        draft.content = "Line one\n\"quoted\" \\ \u0001 é 😀";
        // Whatever these held is replaced.
        draft.pubkey = QString(64, 'f');
        draft.id = draft.sig = "stale";

        std::optional<std::pair<NostrEvent, QString>> result;
        signer->sign(draft, [&](const NostrEvent &event, const QString &error) { result.emplace(event, error); });
        QVERIFY(!result);
        QTRY_VERIFY(result);
        QVERIFY2(result->second.isEmpty(), qPrintable(result->second));
        const NostrEvent event = result->first;
        QCOMPARE(event.pubkey, syncPubkey());
        QCOMPARE(event.id, QString::fromLatin1(
                               QCryptographicHash::hash(event.serialized(), QCryptographicHash::Sha256).toHex()));
        QCOMPARE(event.id, signedEvent(draft.kind, draft.content, draft.tags, draft.createdAt).value("id").toString());
        QVERIFY(event.verify());
        QJsonArray tags;
        for (const QStringList &tag : event.tags)
            tags << QJsonArray::fromStringList(tag);
        const auto parsed = NostrEvent::fromJson(QJsonObject{{"id", event.id},
                                                             {"pubkey", event.pubkey},
                                                             {"created_at", event.createdAt},
                                                             {"kind", event.kind},
                                                             {"tags", tags},
                                                             {"content", event.content},
                                                             {"sig", event.sig}});
        QVERIFY(parsed);
        QVERIFY(parsed->verify());

        // Fresh randomness for each signature: same id, another signature.
        const auto again = signer->signNow(draft);
        QVERIFY(again.second.isEmpty());
        QCOMPARE(again.first.id, event.id);
        QVERIFY(again.first.sig != event.sig);
        QVERIFY(again.first.verify());

        NostrEvent outOfRange = draft;
        outOfRange.kind = 70000;
        QVERIFY(signer->signNow(outOfRange).second.startsWith("Cannot sign this event"));
        outOfRange = draft;
        outOfRange.createdAt = -1;
        QVERIFY(signer->signNow(outOfRange).second.startsWith("Cannot sign this event"));
        QVERIFY(signer->signNow(outOfRange).first.sig.isEmpty());

        // Tags past what fromJson() takes are refused; at the limits, signed.
        const QString tooManyTags = "Cannot sign this event: it has too many tags or too much in them";
        NostrEvent tagged = draft;
        tagged.tags = QList<QStringList>(NostrEvent::maxTags, {"t", "x"});
        QVERIFY(signer->signNow(tagged).second.isEmpty());
        tagged.tags << QStringList{"t", "x"};
        QCOMPARE(signer->signNow(tagged).second, tooManyTags);
        tagged.tags = {QStringList(NostrEvent::maxTagValues, "x")};
        QVERIFY(signer->signNow(tagged).second.isEmpty());
        tagged.tags.first() << "x";
        QCOMPARE(signer->signNow(tagged).second, tooManyTags);
        tagged.tags = {{"t", QString(NostrEvent::maxTagCharacters - 1, 'x')}};
        QVERIFY(signer->signNow(tagged).second.isEmpty());
        tagged.tags.first().last() += 'x';
        QCOMPARE(signer->signNow(tagged).second, tooManyTags);
        QVERIFY(signer->signNow(tagged).first.sig.isEmpty());

        // A signer gone before it answers never answers.
        bool answered = false;
        auto gone = LocalSigner::create(QByteArray(32, '\2'));
        gone->sign(draft, [&](const NostrEvent &, const QString &) { answered = true; });
        gone.reset();
        QTest::qWait(50);
        QVERIFY(!answered);
    }

    // What goes over the bus to keep a key: a "plain" session, then an item
    // in the default collection with Readstr's attributes, replacing any.
    void secretServiceStoresTheKeyInTheDefaultCollection()
    {
        FakeSecretService service;
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<SecretServiceKeyStore::Secret>())),
                 QByteArray("(oayays)"));
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<QMap<QString, QString>>())),
                 QByteArray("a{ss}"));
        const QString npub = syncNpub(), session = "/org/freedesktop/secrets/session/1";
        std::optional<KeyStore::Result> result;
        service.store(npub, QByteArray(32, '\1'), [&](const KeyStore::Result &answer) { result = answer; });
        QCOMPARE(service.methods(), QStringList{"Service.OpenSession /org/freedesktop/secrets"});
        QCOMPARE(service.calls.first().service(), QString("org.freedesktop.secrets"));
        QCOMPARE(service.calls.first().arguments().at(0).toString(), QString("plain"));
        QCOMPARE(service.calls.first().arguments().at(1).value<QDBusVariant>().variant(), QVariant(QString()));
        service.reply({QVariant::fromValue(QDBusVariant(QString())), QVariant::fromValue(QDBusObjectPath(session))});

        QCOMPARE(service.methods().last(), QString("Collection.CreateItem /org/freedesktop/secrets/aliases/default"));
        const QVariantList create = service.calls.last().arguments();
        const QVariantMap properties = create.at(0).toMap();
        QCOMPARE(properties.value("org.freedesktop.Secret.Item.Label").toString(), "Readstr Nostr key (" + npub + ")");
        using Attributes = QMap<QString, QString>;
        const Attributes attributes{{"application", "readstr"}, {"npub", npub}};
        QCOMPARE(properties.value("org.freedesktop.Secret.Item.Attributes").value<Attributes>(), attributes);
        const auto secret = create.at(1).value<SecretServiceKeyStore::Secret>();
        QCOMPARE(secret.session.path(), session);
        QVERIFY(secret.parameters.isEmpty());
        QCOMPARE(secret.value, QByteArray(32, '\1').toHex());
        QCOMPARE(secret.contentType, QString("text/plain"));
        QCOMPARE(create.at(2).toBool(), true);
        QVERIFY(!result);
        service.reply({QVariant::fromValue(QDBusObjectPath("/org/freedesktop/secrets/collection/login/7")),
                       QVariant::fromValue(QDBusObjectPath("/"))});
        QCOMPARE(service.methods().last(), "Session.Close " + session);
        QVERIFY(result);
        QVERIFY(result->error == KeyStore::Error::None);
        // The hex the sent message held is wiped where it is, not in a copy.
        QCOMPARE(service.calls.at(1).arguments().at(1).value<SecretServiceKeyStore::Secret>().value,
                 QByteArray(64, '\0'));
    }

    // A locked collection is unlocked through the service's prompt, once;
    // a dismissed prompt and a collection still locked are told apart.
    void secretServiceUnlocksThroughThePrompt()
    {
        const QString npub = syncNpub(), prompt = "/org/freedesktop/secrets/prompt/u1";
        const QVariantList session{QVariant::fromValue(QDBusVariant(QString())),
                                   QVariant::fromValue(QDBusObjectPath("/org/freedesktop/secrets/session/1"))};
        const auto path = [](const QString &path) { return QVariant::fromValue(QDBusObjectPath(path)); };
        const auto paths = [](const QStringList &list) {
            QList<QDBusObjectPath> paths;
            for (const QString &path : list)
                paths << QDBusObjectPath(path);
            return QVariant::fromValue(paths);
        };
        std::optional<KeyStore::Result> result;
        const auto keep = [&](const KeyStore::Result &answer) { result = answer; };
        {
            FakeSecretService service;
            service.store(npub, QByteArray(32, '\1'), keep);
            service.reply(session);
            service.fail("org.freedesktop.Secret.Error.IsLocked", "locked");
            QCOMPARE(service.methods().last(), QString("Service.Unlock /org/freedesktop/secrets"));
            const QList<QDBusObjectPath> collection{QDBusObjectPath("/org/freedesktop/secrets/aliases/default")};
            QCOMPARE(qdbus_cast<QList<QDBusObjectPath>>(service.calls.last().arguments().at(0)), collection);
            service.reply({paths({}), path(prompt)});
            QCOMPARE(service.listening, QStringList{prompt});
            QCOMPARE(service.methods().last(), "Prompt.Prompt " + prompt);
            QCOMPARE(service.calls.last().arguments(), QVariantList{QString()});
            service.reply({});
            service.completed("/org/freedesktop/secrets/prompt/other", false, {});
            QCOMPARE(service.listening, QStringList{prompt});
            service.completed(prompt, false, {});
            QVERIFY(service.listening.isEmpty());
            QCOMPARE(service.methods().last(), QString("Collection.CreateItem /org/freedesktop/secrets/aliases/default"));
            // Still locked after the prompt: no second round.
            service.fail("org.freedesktop.Secret.Error.IsLocked");
            QVERIFY(result);
            QVERIFY(result->error == KeyStore::Error::Locked);
            QCOMPARE(service.methods().last(), QString("Session.Close /org/freedesktop/secrets/session/1"));
        }
        {
            // A service that prompts for the item itself, dismissed.
            result.reset();
            FakeSecretService service;
            service.store(npub, QByteArray(32, '\1'), keep);
            service.reply(session);
            service.reply({path("/"), path(prompt)});
            service.reply({});
            QVERIFY(!result);
            service.completed(prompt, true, {});
            QVERIFY(result);
            QVERIFY(result->error == KeyStore::Error::Dismissed);
        }
        {
            // Reading a locked item unlocks it first.
            result.reset();
            FakeSecretService service;
            service.lookup(npub, keep);
            QCOMPARE(service.methods(), QStringList{"Service.SearchItems /org/freedesktop/secrets"});
            using Attributes = QMap<QString, QString>;
            QCOMPARE(qdbus_cast<Attributes>(service.calls.first().arguments().at(0)),
                     SecretServiceKeyStore::attributes(npub));
            const QString item = "/org/freedesktop/secrets/collection/login/7";
            service.reply({paths({}), paths({item})});
            QCOMPARE(service.methods().last(), QString("Service.Unlock /org/freedesktop/secrets"));
            service.reply({paths({item}), path("/")});
            QCOMPARE(service.methods().last(), QString("Service.OpenSession /org/freedesktop/secrets"));
            service.reply(session);
            QCOMPARE(service.methods().last(), "Item.GetSecret " + item);
            QCOMPARE(service.calls.last().arguments().at(0).value<QDBusObjectPath>().path(),
                     QString("/org/freedesktop/secrets/session/1"));
            SecretServiceKeyStore::Secret secret{QDBusObjectPath("/org/freedesktop/secrets/session/1"), {},
                                                 QByteArray(32, '\1').toHex(), "text/plain"};
            service.reply({QVariant::fromValue(secret)});
            QCOMPARE(service.methods().last(), QString("Session.Close /org/freedesktop/secrets/session/1"));
            QVERIFY(result);
            QVERIFY(result->error == KeyStore::Error::None);
            QVERIFY(result->found);
            QCOMPARE(result->secret, QByteArray(32, '\1'));
        }
    }

    // Whether a key is there costs one search, which unlocks and reads
    // nothing; no Secret Service is told apart from any other failure.
    void secretServiceFindsAndDeletesWithoutReading()
    {
        const QString npub = syncNpub(), item = "/org/freedesktop/secrets/collection/login/7";
        const auto paths = [](const QStringList &list) {
            QList<QDBusObjectPath> paths;
            for (const QString &path : list)
                paths << QDBusObjectPath(path);
            return QVariant::fromValue(paths);
        };
        std::optional<KeyStore::Result> result;
        const auto keep = [&](const KeyStore::Result &answer) { result = answer; };
        FakeSecretService service;
        service.contains(npub, keep);
        service.reply({paths({}), paths({item})});
        QVERIFY(result && result->error == KeyStore::Error::None && result->found);
        QVERIFY(result->secret.isEmpty());
        service.contains(npub, keep);
        service.reply({paths({}), paths({})});
        QVERIFY(result && !result->found);
        QCOMPARE(service.methods(), QStringList({"Service.SearchItems /org/freedesktop/secrets",
                                                 "Service.SearchItems /org/freedesktop/secrets"}));

        service.remove(npub, keep);
        service.reply({paths({item}), paths({})});
        QCOMPARE(service.methods().last(), "Item.Delete " + item);
        result.reset();
        service.reply({QVariant::fromValue(QDBusObjectPath("/"))});
        QVERIFY(result && result->error == KeyStore::Error::None && result->found);

        // A reply of the wrong shape is a failure, not a key.
        service.lookup(npub, keep);
        service.reply({QString("not paths"), paths({})});
        QVERIFY(result && result->error == KeyStore::Error::Failed);
        QVERIFY(!result->found);

        result.reset();
        service.contains(npub, keep);
        service.fail("org.freedesktop.DBus.Error.ServiceUnknown", "The name is not activatable");
        QVERIFY(result && result->error == KeyStore::Error::Unavailable);
        result.reset();
        service.contains(npub, keep);
        service.fail("org.freedesktop.DBus.Error.NoReply", "Timed out");
        QVERIFY(result && result->error == KeyStore::Error::Failed);
        QCOMPARE(result->detail, QString("Timed out"));

        const qsizetype calls = service.calls.size();
        service.bus = false;
        result.reset();
        service.store(npub, QByteArray(32, '\1'), keep);
        QVERIFY(!result);
        QTRY_VERIFY(result);
        QVERIFY(result->error == KeyStore::Error::Unavailable);
        QCOMPARE(service.calls.size(), calls);
    }

    // A prompt that never completes fails once its time is up, and closes,
    // and one whose service leaves the bus fails at once; neither answers
    // twice when a late Completed comes.
    void secretServiceGivesUpOnAPromptThatNeverCompletes()
    {
        const QString npub = syncNpub(), item = "/org/freedesktop/secrets/collection/login/7";
        const QString prompt = "/org/freedesktop/secrets/prompt/u1";
        const QVariant noItems = QVariant::fromValue(QList<QDBusObjectPath>());
        const QVariant lockedItem = QVariant::fromValue(QList<QDBusObjectPath>{QDBusObjectPath(item)});
        QList<KeyStore::Result> results;
        const auto keep = [&](const KeyStore::Result &answer) { results << answer; };
        FakeSecretService service;
        service.setPromptTimeout(std::chrono::milliseconds(30));
        service.lookup(npub, keep);
        service.reply({noItems, lockedItem});
        service.reply({noItems, QVariant::fromValue(QDBusObjectPath(prompt))});
        service.reply({});
        QCOMPARE(service.listening, QStringList{prompt});
        QTRY_COMPARE(results.size(), 1);
        QVERIFY(results.first().error == KeyStore::Error::Failed);
        QCOMPARE(results.first().detail, QString("The keyring's prompt was not answered in time."));
        QVERIFY(service.listening.isEmpty());
        QCOMPARE(service.methods().last(), "Prompt.Dismiss " + prompt);
        service.completed(prompt, false, {});
        QTest::qWait(50);
        QCOMPARE(results.size(), 1);

        FakeSecretService lost;
        lost.lookup(npub, keep);
        lost.reply({noItems, lockedItem});
        lost.reply({noItems, QVariant::fromValue(QDBusObjectPath(prompt))});
        lost.serviceLost();
        QCOMPARE(results.size(), 2);
        QVERIFY(results.last().error == KeyStore::Error::Failed);
        QCOMPARE(results.last().detail, QString("The keyring went away before its prompt was answered."));
        QVERIFY(lost.listening.isEmpty());
        // The Prompt call's own error, after that, answers no one again.
        lost.fail("org.freedesktop.DBus.Error.NoReply");
        lost.completed(prompt, true, {});
        QCOMPARE(results.size(), 2);
    }

    // Two requests given the same prompt path cannot be told apart by its
    // one Completed signal: the second fails at once, and the first is still
    // answered when the prompt completes.
    void secretServiceRefusesASecondWaitOnTheSamePrompt()
    {
        const QString npub = syncNpub(), item = "/org/freedesktop/secrets/collection/login/7";
        const QString prompt = "/org/freedesktop/secrets/prompt/u1";
        const QVariant noItems = QVariant::fromValue(QList<QDBusObjectPath>());
        const QVariant lockedItem = QVariant::fromValue(QList<QDBusObjectPath>{QDBusObjectPath(item)});
        QList<KeyStore::Result> first, second;
        FakeSecretService service;
        service.lookup(npub, [&](const KeyStore::Result &answer) { first << answer; });
        service.lookup(npub, [&](const KeyStore::Result &answer) { second << answer; });
        service.reply({noItems, lockedItem});
        service.reply({noItems, lockedItem});
        service.reply({noItems, QVariant::fromValue(QDBusObjectPath(prompt))});
        service.reply({noItems, QVariant::fromValue(QDBusObjectPath(prompt))});
        QVERIFY(first.isEmpty());
        QCOMPARE(second.size(), 1);
        QVERIFY(second.first().error == KeyStore::Error::Failed);
        QCOMPARE(second.first().detail, QString("The keyring gave the same prompt to two requests."));
        QCOMPARE(service.listening, QStringList{prompt});
        service.completed(prompt, true, {});
        QCOMPARE(first.size(), 1);
        QVERIFY(first.first().error == KeyStore::Error::Dismissed);
        QCOMPARE(second.size(), 1);
        QVERIFY(service.listening.isEmpty());
    }

    // Any program of this user's can add items with Readstr's attributes, so
    // the read passes over one that is not the account's key, whether junk
    // or another key, and takes the one that is.
    void secretServicePassesOverItemsThatAreNotTheKey()
    {
        const QString npub = syncNpub(), session = "/org/freedesktop/secrets/session/1";
        const QString junk = "/org/freedesktop/secrets/collection/login/1",
                      wrong = "/org/freedesktop/secrets/collection/login/2",
                      real = "/org/freedesktop/secrets/collection/login/3";
        const auto paths = [](const QStringList &list) {
            QList<QDBusObjectPath> paths;
            for (const QString &path : list)
                paths << QDBusObjectPath(path);
            return QVariant::fromValue(paths);
        };
        const QVariantList opened{QVariant::fromValue(QDBusVariant(QString())),
                                  QVariant::fromValue(QDBusObjectPath(session))};
        const auto secret = [&](const QByteArray &value) {
            return QVariantList{QVariant::fromValue(
                SecretServiceKeyStore::Secret{QDBusObjectPath(session), {}, value, "text/plain"})};
        };
        std::optional<KeyStore::Result> result;
        const auto keep = [&](const KeyStore::Result &answer) { result = answer; };
        {
            FakeSecretService service;
            service.lookup(npub, keep);
            service.reply({paths({junk, wrong}), paths({real})});
            service.reply(opened);
            QCOMPARE(service.methods().last(), "Item.GetSecret " + junk);
            service.reply(secret("not a key"));
            QCOMPARE(service.methods().mid(3),
                     QStringList({"Session.Close " + session, "Service.OpenSession /org/freedesktop/secrets"}));
            service.reply({});
            service.reply(opened);
            QCOMPARE(service.methods().last(), "Item.GetSecret " + wrong);
            service.reply(secret(QByteArray(32, '\2').toHex()));
            service.reply({});
            QVERIFY(!result);
            // The locked one is unlocked only once the others are passed over.
            QCOMPARE(service.methods().last(), QString("Service.Unlock /org/freedesktop/secrets"));
            service.reply({paths({real}), QVariant::fromValue(QDBusObjectPath("/"))});
            service.reply(opened);
            QCOMPARE(service.methods().last(), "Item.GetSecret " + real);
            service.reply(secret(QByteArray(32, '\1').toHex()));
            QVERIFY(result);
            QVERIFY(result->error == KeyStore::Error::None);
            QVERIFY(result->found);
            QCOMPARE(result->secret, QByteArray(32, '\1'));
            QCOMPARE(service.methods().count("Session.Close " + session), 3);
        }
        {
            result.reset();
            FakeSecretService service;
            service.lookup(npub, keep);
            service.reply({paths({junk}), paths({})});
            service.reply(opened);
            service.reply(secret(QByteArray(32, '\2').toHex()));
            QVERIFY(result);
            QVERIFY(result->error == KeyStore::Error::Failed);
            QCOMPARE(result->detail, QString("No item in the keyring holds this account's key."));
            QVERIFY(result->secret.isEmpty());
        }
        // An item deleted since the search is passed over too; any other
        // failure to read one still fails the read.
        {
            result.reset();
            FakeSecretService service;
            service.lookup(npub, keep);
            service.reply({paths({junk, real}), paths({})});
            service.reply(opened);
            QCOMPARE(service.methods().last(), "Item.GetSecret " + junk);
            service.fail("org.freedesktop.Secret.Error.NoSuchObject");
            QVERIFY(!result);
            service.reply({});
            service.reply(opened);
            QCOMPARE(service.methods().last(), "Item.GetSecret " + real);
            service.reply(secret(QByteArray(32, '\1').toHex()));
            QVERIFY(result);
            QVERIFY(result->error == KeyStore::Error::None);
            QVERIFY(result->found);
        }
        {
            result.reset();
            FakeSecretService service;
            service.lookup(npub, keep);
            service.reply({paths({junk, real}), paths({})});
            service.reply(opened);
            service.fail("org.freedesktop.DBus.Error.Failed", "broken");
            QVERIFY(result);
            QVERIFY(result->error == KeyStore::Error::Failed);
            QCOMPARE(result->detail, QString("broken"));
        }
    }

    void nostrSourceKeepsTheLatestVersionOfEachArticle()
    {
        QList<NostrEvent> events;
        for (const QJsonObject &json :
             {signedEvent(30023, "# Old", {{"d", "a"}, {"title", "Old"}, {"published_at", "1600000000"}}, 1700000000),
              signedEvent(30023, "# New", {{"d", "a"}, {"title", "New\u202e"}, {"published_at", "1600000000"}},
                          1700000500),
              signedEvent(30023, "Body text", {{"d", "b"}}, 1700000100),
              signedEvent(1, "a note", {}, 1700000200),
              signedEvent(0, R"({"name":"ada","display_name":"Ada L.","about":"Writes.","website":"javascript:x"})",
                          {}, 1700000300)})
            events << *NostrEvent::fromJson(json);
        const QString pubkey = events.first().pubkey;

        const QList<ParsedItem> items = NostrSource::longForm(events, "Ada L.");
        QCOMPARE(items.size(), 2);
        QCOMPARE(items.at(0).title, QString("Untitled"));
        QCOMPARE(items.at(0).summary, QString("Body text"));
        QCOMPARE(items.at(0).published.toSecsSinceEpoch(), 1700000100);
        QCOMPARE(items.at(1).title, QString("New"));
        QCOMPARE(items.at(1).content, QString("# New"));
        QCOMPARE(items.at(1).guid, events.at(1).id);
        QCOMPARE(items.at(1).published.toSecsSinceEpoch(), 1600000000);
        QCOMPARE(items.at(1).address, "30023:" + pubkey + ":a");
        QCOMPARE(items.at(1).revision, 1700000500);
        QCOMPARE(items.at(1).url, "https://njump.me/" + Nip19::naddr(30023, pubkey, "a"));
        QCOMPARE(items.at(1).author, QString("Ada L."));

        const auto profile = NostrSource::profile(events, pubkey);
        QVERIFY(profile);
        QCOMPARE(profile->feed.title, QString("Ada L."));
        QCOMPARE(profile->feed.description, QString("Writes."));
        QCOMPARE(profile->feed.siteUrl, "https://njump.me/" + Nip19::npub(pubkey));
        QCOMPARE(profile->eventId, events.last().id);
        QVERIFY(!NostrSource::profile(events, QString(64, 'a')));

        // An empty object clears the details; what is not an object is no profile.
        events << *NostrEvent::fromJson(signedEvent(0, "{}", {}, 1700000400));
        const auto cleared = NostrSource::profile(events, pubkey);
        QVERIFY(cleared);
        QVERIFY(cleared->feed.title.isEmpty());
        QVERIFY(cleared->feed.description.isEmpty());
        events << *NostrEvent::fromJson(signedEvent(0, "[]", {}, 1700000500));
        QVERIFY(!NostrSource::profile(events, pubkey));
    }

    void storeKeepsTheNewestVersionOfAReplaceableArticle()
    {
        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.path() + "/readstr.db"));
        const qint64 feed = store.addFeed("npub1example", "npub1example");
        ParsedItem item;
        item.guid = "event-1";
        item.title = "First";
        item.address = "30023:abc:post";
        item.revision = 100;
        QCOMPARE(store.saveItems(feed, {item}), 1);
        const qint64 id = store.articles({})[0].id;
        QVERIFY(store.setRead(id, true));
        QVERIFY(store.setStarred(id, true));

        item.guid = "event-2";
        item.title = "Edited";
        item.revision = 200;
        QCOMPARE(store.saveItems(feed, {item}), 0);
        ParsedItem stale = item;
        stale.guid = "event-0";
        stale.title = "Stale";
        stale.revision = 50;
        QCOMPARE(store.saveItems(feed, {stale}), 0);

        const QList<Article> articles = store.articles({});
        QCOMPARE(articles.size(), 1);
        QCOMPARE(articles[0].id, id);
        QCOMPARE(articles[0].guid, QString("event-2"));
        QCOMPARE(articles[0].title, QString("Edited"));
        QCOMPARE(articles[0].address, QString("30023:abc:post"));
        QVERIFY(articles[0].read);
        QVERIFY(articles[0].starred);
    }

    void storeKeepsTheNewestProfileAndAsksFromTheLastCompleteRefresh()
    {
        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.path() + "/readstr.db"));
        const QString npub = Nip19::npub(QString(64, 'c'));
        const qint64 feed = store.addFeed(npub, npub);
        ParsedFeed profile;
        profile.title = "Ada";
        profile.siteUrl = "https://ada.example/";
        QVERIFY(store.updateProfile(feed, profile, 200, "b-profile"));
        ParsedFeed old = profile;
        old.title = "Old Ada";
        old.siteUrl = "https://expired.example/";
        QVERIFY(store.updateProfile(feed, old, 100, "a-profile"));
        QCOMPARE(store.feed(feed)->siteUrl, QString("https://ada.example/"));
        QCOMPARE(store.feed(feed)->title, QString("Ada"));
        // Same second: the lower id wins, whichever comes first.
        ParsedFeed tied = profile;
        tied.title = "Tied Ada";
        QVERIFY(store.updateProfile(feed, tied, 200, "c-profile"));
        QCOMPARE(store.feed(feed)->title, QString("Ada"));
        QVERIFY(store.updateProfile(feed, tied, 200, "a-profile"));
        QCOMPARE(store.feed(feed)->title, QString("Tied Ada"));
        // A newer profile renames what an earlier one named, but not a title
        // from OPML.
        ParsedFeed renamed = profile;
        renamed.title = "Ada L.";
        QVERIFY(store.updateProfile(feed, renamed, 300, "d-profile"));
        QCOMPARE(store.feed(feed)->title, QString("Ada L."));
        const QString otherNpub = Nip19::npub(QString(64, 'd'));
        const qint64 named = store.addFeed(otherNpub, "My Ada");
        QVERIFY(store.updateProfile(named, renamed, 300, "d-profile"));
        QCOMPARE(store.feed(named)->title, QString("My Ada"));

        // Same second: the lower id wins, whichever comes first.
        ParsedItem item;
        item.guid = "b-event";
        item.title = "B";
        item.address = "30023:c:post";
        item.revision = 300;
        QCOMPARE(store.saveItems(feed, {item}), 1);
        item.guid = "a-event";
        item.title = "A";
        QCOMPARE(store.saveItems(feed, {item}), 0);
        item.guid = "c-event";
        item.title = "C";
        QCOMPARE(store.saveItems(feed, {item}), 0);
        QCOMPARE(store.articles({})[0].title, QString("A"));

        QVERIFY(store.setRelayHints(feed, {"wss://hint.example.com"}));
        Feed stored = *store.feed(feed);
        QVERIFY(Library::isNostrFeed(stored));
        QCOMPARE(stored.relayHints, QStringList({"wss://hint.example.com"}));
        const QList<NostrQuery> queries = Library::nostrQueries(stored);
        QCOMPARE(queries.size(), 2);
        QCOMPARE(queries.at(0).kinds, QList<int>({0, 30023}));
        QCOMPARE(queries.at(1).kinds, QList<int>({21, 22}));
        for (const NostrQuery &query : queries) {
            QCOMPARE(query.author, QString(64, 'c'));
            QCOMPARE(query.extraRelays, QStringList({"wss://hint.example.com"}));
            QCOMPARE(query.since, 0);
        }

        // Complete only when both queries are.
        NostrResult partial;
        partial.relays = 2;
        partial.answered = 1;
        partial.startedAt = QDateTime::currentDateTimeUtc();
        NostrResult complete = partial;
        complete.answered = 2;
        QCOMPARE(Library::applyNostr(store, stored, partial, complete).status, QString("updated"));
        QCOMPARE(Library::nostrQueries(*store.feed(feed)).at(0).since, 0);
        QCOMPARE(Library::applyNostr(store, stored, complete, partial).status, QString("updated"));
        QCOMPARE(Library::nostrQueries(*store.feed(feed)).at(1).since, 0);
        NostrResult earlier = complete;
        earlier.startedAt = complete.startedAt.addSecs(-60);
        Library::applyNostr(store, stored, complete, earlier);
        for (const NostrQuery &query : Library::nostrQueries(*store.feed(feed)))
            QCOMPARE(query.since, earlier.startedAt.toSecsSinceEpoch() - 24 * 3600);

        // A failed videos query still saves the articles, as an incomplete
        // refresh; a failed articles query fails the feed.
        NostrResult failed;
        failed.relays = 2;
        failed.error = "no relay answered";
        QCOMPARE(Library::applyNostr(store, stored, complete, failed).status, QString("updated"));
        QCOMPARE(Library::applyNostr(store, stored, failed, complete).status, QString("error"));
    }

    void sanitizerKeepsTheLinesOfPlainText()
    {
        QCOMPARE(Sanitizer::linesToRichText("# Title\n\nOne <b>\nTwo\r\n\r\n\n  Three\u202e "),
                 QString("<p># Title</p><p>One &lt;b&gt;<br>Two</p><p>Three</p>"));
        QCOMPARE(Sanitizer::plainToRichText("a\u202eb"), QString("<p>ab</p>"));
    }

    void relayPoolKeepsOnlyVerifiedEventsOfTheQuery()
    {
        FakeRelay first, second;
        const QJsonObject post = signedEvent(30023, "body", {{"d", "a"}}, 1700000000);
        const QJsonObject other = signedEvent(30023, "someone else", {{"d", "b"}}, 1700000000, 2);
        const QJsonObject note = signedEvent(1, "a note", {}, 1700000002);
        first.events = {post, other, note, QJsonValue("junk"), QJsonValue(QJsonArray{1, 2})};
        second.events = {post};

        RelayPool pool;
        pool.setRelays({first.url(), second.url(), "ws://example.com", "wss://user:pw@relay.example.com",
                        "https://relay.example.com"});
        NostrQuery query;
        query.author = post.value("pubkey").toString();
        query.kinds = {30023};
        const NostrResult result = pool.query(query);
        QVERIFY2(result.ok(), qPrintable(result.error));
        QCOMPARE(result.answered, 2);
        QCOMPARE(result.events.size(), 1);
        QCOMPARE(result.events.first().id, post.value("id").toString());
        QCOMPARE(first.filters.size(), 1);
        QCOMPARE(first.filters.first().value("authors").toArray(), QJsonArray{query.author});
        QTRY_COMPARE(first.closed.size(), 1);
    }

    void relayPoolDropsARelayThatForgesEvents()
    {
        FakeRelay forger, honest;
        const QJsonObject post = signedEvent(30023, "body", {{"d", "a"}}, 1700000000);
        QJsonObject forged = signedEvent(30023, "forged", {{"d", "c"}}, 1700000001);
        forged.insert("content", "changed");
        forger.events = {forged, signedEvent(30023, "later", {{"d", "d"}}, 1700000002)};
        honest.events = {post};
        RelayPool::Limits limits;
        limits.maxOpenQueries = 1;
        RelayPool pool;
        pool.setLimits(limits);
        pool.setRelays({forger.url(), honest.url()});
        NostrQuery query;
        query.author = post.value("pubkey").toString();
        query.kinds = {30023};
        NostrResult result;
        pool.queryEach({query, query}, [&](qsizetype, const NostrResult &each) { result = each; });
        QVERIFY(result.ok());
        QVERIFY(!result.complete());
        QCOMPARE(result.relays, 2);
        QCOMPARE(result.answered, 1);
        QCOMPARE(result.events.size(), 1);
        QCOMPARE(result.events.first().id, post.value("id").toString());
        // Given up on after the first forgery: the second query never reaches it.
        QCOMPARE(forger.filters.size(), 1);
        QCOMPARE(honest.filters.size(), 2);
    }

    void relayPoolKeepsTheNewestVersionAndRefusesTheFuture()
    {
        FakeRelay relay;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const QString bom = QString(QChar(0xFEFF)) + "starts with a byte order mark";
        const QJsonObject newest = signedEvent(30023, "new", {{"d", "a"}}, now - 100);
        relay.events = {newest, signedEvent(30023, "old", {{"d", "a"}}, now - 200),
                        signedEvent(30023, "ancient", {{"d", "b"}}, now - 100000),
                        signedEvent(30023, "future", {{"d", "c"}}, now + 3 * 24 * 3600),
                        signedEvent(30023, bom, {{"d", "e"}}, now - 50)};
        RelayPool pool;
        pool.setRelays({relay.url()});
        NostrQuery query;
        query.author = newest.value("pubkey").toString();
        query.kinds = {30023};
        query.since = now - 1000;
        const NostrResult result = pool.query(query);
        QVERIFY(result.complete());
        QStringList contents;
        for (const NostrEvent &event : result.events)
            contents << event.content;
        contents.sort();
        QCOMPARE(contents, QStringList({"new", bom}));
    }

    void relayPoolGivesSlowRelaysAShortGrace()
    {
        FakeRelay silent, good;
        silent.silent = true;
        good.events = {signedEvent(30023, "body", {{"d", "a"}}, 1700000000)};
        RelayPool::Limits limits;
        limits.graceMs = 200;
        limits.subscriptionTimeoutMs = 10000;
        RelayPool pool;
        pool.setLimits(limits);
        pool.setRelays({silent.url(), good.url()});
        NostrQuery query;
        query.author = good.events.first().toObject().value("pubkey").toString();
        query.kinds = {30023};
        QElapsedTimer timer;
        timer.start();
        int delivered = 0, ok = 0;
        pool.queryEach(QList<NostrQuery>(12, query), [&](qsizetype, const NostrResult &result) {
            ok += result.ok();
            ++delivered;
        });
        QCOMPARE(delivered, 12);
        QCOMPARE(ok, 12);
        // One grace period, after which the silent relay is given up on,
        // rather than a timeout for every few authors.
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(silent.filters.size() <= 4);
    }

    void relayPoolReconnectsAfterARelayDrops()
    {
        FakeRelay relay;
        relay.events = {signedEvent(30023, "body", {{"d", "a"}}, 1700000000)};
        relay.dropAfter = 1;
        RelayPool::Limits limits;
        limits.maxOpenQueries = 1;
        RelayPool pool;
        pool.setLimits(limits);
        pool.setRelays({relay.url()});
        NostrQuery query;
        query.author = relay.events.first().toObject().value("pubkey").toString();
        query.kinds = {30023};
        QList<bool> ok;
        pool.queryEach(QList<NostrQuery>(3, query), [&](qsizetype, const NostrResult &result) { ok << result.ok(); });
        QCOMPARE(ok, QList<bool>({true, true, true}));
        QCOMPARE(relay.connections, 2);
    }

    void relayPoolKeepsRelayHintsOffTheLocalNetwork()
    {
        QVERIFY(RelayPool::isPublicRelay(QUrl("wss://relay.example.com")));
        for (const char *url : {"ws://relay.example.com", "wss://localhost", "wss://relay.local", "wss://router",
                                "wss://127.0.0.1", "wss://10.0.0.1", "wss://192.168.1.1", "wss://169.254.1.1",
                                "wss://[::1]", "wss://[fd00::1]", "wss://[::ffff:127.0.0.1]",
                                "wss://u:p@relay.example.com"})
            QVERIFY2(!RelayPool::isPublicRelay(QUrl(url)), url);
        for (const char *url : {"wss://localhost.", "wss://relay.local.", "wss://router."})
            QVERIFY2(!RelayPool::isPublicRelay(QUrl(url)), url);
        QVERIFY(RelayPool::isPublicRelay(QUrl("wss://[2606:4700:4700::1111]")));
        QVERIFY(RelayPool::isPublicRelay(QUrl("wss://1.1.1.1")));
        QVERIFY(RelayPool::isUsableRelay(QUrl("ws://127.0.0.1:7000")));
        QVERIFY(!RelayPool::isUsableRelay(QUrl("ws://relay.example.com")));

        const EnvironmentGuard guard({"READSTR_RELAYS"});
        qputenv("READSTR_RELAYS", "ws://relay.example.com, nonsense");
        QVERIFY(RelayPool::configuredRelays().isEmpty());
        RelayPool pool;
        NostrQuery query;
        query.author = QString(64, 'a');
        query.kinds = {30023};
        query.extraRelays = {"ws://127.0.0.1:1"};
        QCOMPARE(pool.query(query).error, QString("no usable relays"));
        // Set but empty is still a list, of none.
        qputenv("READSTR_RELAYS", " ");
        QVERIFY(RelayPool::configuredRelays().isEmpty());
        QCOMPARE(pool.query(query).error, QString("no usable relays"));
        qunsetenv("READSTR_RELAYS");
        QCOMPARE(RelayPool::configuredRelays(), RelayPool::defaultRelays());
    }

    void fetcherTellsLocalAddressesFromPublicOnes()
    {
        // The unspecified address, IPv6 addresses that carry a local IPv4
        // one (mapped, compatible, NAT64, 6to4), and the reserved IPv4 ranges.
        for (const char *host : {"::", "0:0:0:0:0:0:0:0", "::ffff:10.0.0.1", "::10.0.0.1", "::127.0.0.1",
                                 "::192.168.1.1", "64:ff9b::192.168.1.1", "64:ff9b::7f00:1", "64:ff9b::0.0.0.0",
                                 "2002:c0a8:0101::1", "2002:7f00:1::", "2002:0a00:0001:1234::5", "198.18.0.1",
                                 "198.19.255.255", "240.0.0.1", "250.1.2.3", "255.255.255.255",
                                 "::ffff:198.18.0.1", "64:ff9b::100.64.0.1"})
            QVERIFY2(Fetcher::isLocalHost(host), host);
        // The same forms around public IPv4 addresses, and the edges of the
        // ranges, stay public.
        for (const char *host : {"1.1.1.1", "198.17.255.255", "198.20.0.0", "223.255.255.255", "::ffff:1.1.1.1",
                                 "::1.1.1.1", "64:ff9b::1.1.1.1", "64:ff9b::808:808", "2002:0101:0101::1",
                                 "64:ff9c::a00:1", "2606:4700:4700::1111", "2001:db9::1", "example.com"})
            QVERIFY2(!Fetcher::isLocalHost(host), host);
        // As URLs give them, which is how they arrive.
        QVERIFY(Fetcher::isLocalHost(QUrl("http://[::]:8080/").host()));
        QVERIFY(Fetcher::isLocalHost(QUrl("http://[64:ff9b::192.168.0.1]/").host()));
        QVERIFY(Fetcher::isLocalHost(QUrl("http://255.255.255.255/").host()));
        QVERIFY(!RelayPool::isPublicRelay(QUrl("wss://[2002:c0a8:1::]")));
        QVERIFY(RelayPool::isPublicRelay(QUrl("wss://[2002:101:101::]")));
    }

    void relayPoolTellsRelayPathsApartAndDefersHintsPastItsCap()
    {
        FakeRelay relay;
        relay.events = {signedEvent(30023, "body", {{"d", "a"}}, 1700000000)};
        RelayPool pool;
        NostrQuery query;
        query.author = relay.events.first().toObject().value("pubkey").toString();
        query.kinds = {30023};
        pool.setRelays({relay.url() + "/", relay.url()});
        NostrResult result = pool.query(query);
        QCOMPARE(result.relays, 1);
        QVERIFY(result.complete());
        pool.setRelays({relay.url() + "/nostr/", relay.url() + "/nostr"});
        QCOMPARE(pool.query(query).relays, 2);

        RelayPool::Limits limits;
        limits.maxConnections = 1;
        pool.setLimits(limits);
        pool.setRelays({relay.url()});
        query.extraRelays = {"wss://relay.example.com"};
        result = pool.query(query);
        QVERIFY(result.ok());
        QCOMPARE(result.relays, 1);
        QVERIFY(result.deferred);
        QVERIFY(!result.complete());
    }

    void relayPoolSurvivesBeingDeletedWhenItFinishes()
    {
        FakeRelay relay;
        relay.events = {signedEvent(30023, "body", {{"d", "a"}}, 1700000000)};
        relay.dropBeforeEose = true;
        auto *pool = new RelayPool;
        pool->setRelays({relay.url()});
        NostrQuery query;
        query.author = relay.events.first().toObject().value("pubkey").toString();
        query.kinds = {30023};
        bool finished = false;
        pool->queryEachHeld(
            {query}, [](qsizetype, const NostrResult &, const RelayPool::Release &release) { release(); },
            [&] {
                finished = true;
                delete pool;
            });
        QTRY_VERIFY(finished);
        QTest::qWait(100);
        QCOMPARE(relay.connections, 1);
    }

    void nostrEventSerializesLikeJsonStringify()
    {
        // What JSON.stringify gives for the same array in Node: quote and
        // backslash escaped, the short escapes, other controls as \u00XX,
        // DEL and U+2028 raw, a lone surrogate escaped.
        NostrEvent event;
        event.pubkey = QString(64, 'a');
        event.createdAt = 1;
        event.kind = 1;
        event.tags = {{"t", QString(QChar(7))}};
        event.content = QString::fromUtf16(u"q\"b\\ \b\f\n\r\t\u001f\u007f\u2028 \u00e9 \U0001F600 ") + QChar(0xD800) + "x";
        QCOMPARE(event.serialized(),
                 "[0,\"" + QByteArray(64, 'a') + "\",1,1,[[\"t\",\"\\u0007\"]],\"q\\\"b\\\\ \\b\\f\\n\\r\\t\\u001f\x7f"
                 "\xe2\x80\xa8 \xc3\xa9 \xf0\x9f\x98\x80 \\ud800x\"]");
    }

    void relayPoolReportsRelaysThatFail()
    {
        FakeRelay silent, refusing, good;
        silent.silent = true;
        refusing.closeWith = "auth-required: sign in";
        good.events = {signedEvent(30023, "body", {{"d", "a"}}, 1700000000)};
        RelayPool::Limits limits;
        limits.connectTimeoutMs = 2000;
        limits.subscriptionTimeoutMs = 300;
        RelayPool pool;
        pool.setLimits(limits);
        NostrQuery query;
        query.author = good.events.first().toObject().value("pubkey").toString();
        query.kinds = {30023};

        pool.setRelays({silent.url(), refusing.url()});
        QElapsedTimer timer;
        timer.start();
        NostrResult result = pool.query(query);
        QVERIFY(timer.elapsed() < 5000);
        QVERIFY(!result.ok());
        QVERIFY(result.error.contains("timed out"));
        QVERIFY(result.error.contains("auth-required: sign in"));

        pool.setRelays({silent.url(), good.url()});
        result = pool.query(query);
        QVERIFY(result.ok());
        QCOMPARE(result.answered, 1);
        QCOMPARE(result.events.size(), 1);

        // A port nothing listens on.
        QTcpServer closed;
        QVERIFY(closed.listen(QHostAddress::LocalHost));
        const QString nowhere = QStringLiteral("ws://127.0.0.1:%1").arg(closed.serverPort());
        closed.close();
        pool.setRelays({nowhere});
        QVERIFY(!pool.query(query).ok());
        pool.setRelays({"wss://user:pw@relay.example.com"});
        QCOMPARE(pool.query(query).error, QString("no usable relays"));
    }

    void relayPoolCapsEventsAndOpenQueries()
    {
        FakeRelay relay;
        for (int i = 0; i < 5; ++i)
            relay.events << signedEvent(30023, QString(1000, 'x'), {{"d", QString::number(i)}}, 1700000000 + i);
        RelayPool::Limits limits;
        limits.maxEventsPerQuery = 3;
        limits.maxOpenQueries = 2;
        RelayPool pool;
        pool.setLimits(limits);
        pool.setRelays({relay.url()});
        NostrQuery query;
        query.author = relay.events.first().toObject().value("pubkey").toString();
        query.kinds = {30023};

        NostrResult result = pool.query(query);
        QCOMPARE(result.events.size(), 3);
        QVERIFY(result.truncated);
        limits.maxEventsPerQuery = 100;
        limits.maxBytesPerQuery = 6000;
        pool.setLimits(limits);
        result = pool.query(query);
        QCOMPARE(result.events.size(), 2);
        QVERIFY(result.truncated);

        // Held results keep their slots, so no more than two are open.
        QList<RelayPool::Release> held;
        int delivered = 0, finished = 0;
        pool.queryEachHeld(QList<NostrQuery>(5, query),
                           [&](qsizetype, const NostrResult &, const RelayPool::Release &release) {
                               held << release;
                               ++delivered;
                           },
                           [&] { ++finished; });
        while (!finished) {
            QTRY_VERIFY(!held.isEmpty() || finished);
            QTest::qWait(50);
            QVERIFY(held.size() <= 2);
            for (const auto &release : std::exchange(held, {})) {
                release();
                release();
            }
        }
        QCOMPARE(delivered, 5);
        QCOMPARE(finished, 1);
        // One connection per batch, however many queries it carries.
        QCOMPARE(relay.connections, 3);
    }

    void cliSubscribesToANostrAuthor()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME", "READSTR_RELAYS"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        qputenv("READSTR_RELAYS", relay.url().toUtf8());
        relay.events = {signedEvent(0, R"({"name":"ada"})", {}, now + 0),
                        signedEvent(30023, "# Hi", {{"d", "a"}, {"title", "Hello"}}, now + 100)};
        const QString npub = Nip19::npub(relay.events.first().toObject().value("pubkey").toString());

        QString out, err;
        QCOMPARE(run({"subscribe", npub, "--json"}, out, err), 0);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        const QJsonObject subscribed = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(subscribed.value("title").toString(), QString("ada"));
        QCOMPARE(subscribed.value("url").toString(), npub);
        QCOMPARE(subscribed.value("articles").toInt(), 1);
        QVERIFY(!relay.filters.first().contains("since"));

        relay.events << signedEvent(30023, "# Edited", {{"d", "a"}, {"title", "Hello again"}}, now + 200)
                     << signedEvent(30023, "More", {{"d", "b"}, {"title", "Second"}}, now + 300);
        out.clear();
        QCOMPARE(run({"refresh", "--json"}, out, err), 0);
        const QJsonObject refreshed = QJsonDocument::fromJson(out.toUtf8()).array().at(0).toObject();
        QCOMPARE(refreshed.value("status").toString(), QString("updated"));
        QCOMPARE(refreshed.value("new").toInt(), 1);
        QVERIFY(relay.filters.last().contains("since"));

        out.clear();
        QCOMPARE(run({"articles", "--json"}, out, err), 0);
        const QJsonArray articles = QJsonDocument::fromJson(out.toUtf8()).array();
        QCOMPARE(articles.size(), 2);
        QCOMPARE(articles.at(1).toObject().value("title").toString(), QString("Hello again"));

        out.clear();
        QCOMPARE(run({"subscribe", "nostr:" + npub}, out, err), 0);
        QVERIFY(out.startsWith("Already subscribed to ada"));
        out.clear();
        QCOMPARE(run({"export"}, out, err), 0);
        QVERIFY(out.contains("type=\"nostr\""));
        QVERIFY(out.contains("nostr=\"" + npub + "\""));

        relay.closeWith = "blocked: no";
        QCOMPARE(run({"subscribe", Nip19::npub(QString(64, 'b'))}, out, err), 1);
        QVERIFY(err.contains("blocked: no"));
        QCOMPARE(run({"subscribe", "npub1notreal"}, out, err), 1);
        QVERIFY(err.contains("or an npub"));
    }

    void backendRefreshesAndAddsNostrAuthors()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        relay.events = {signedEvent(0, R"({"display_name":"Ada"})", {}, now + 0),
                        signedEvent(30023, "Some *Markdown*", {{"d", "a"}, {"title", "Hello"}}, now + 100)};
        const QString npub = Nip19::npub(relay.events.first().toObject().value("pubkey").toString());

        Backend backend;
        backend.setRelays({relay.url()});
        QSignalSpy subscribed(&backend, &Backend::subscribed);
        backend.addFeed(npub);
        QVERIFY(subscribed.wait(5000));
        QTRY_VERIFY(backend.status().contains("Ada"));

        relay.events << signedEvent(30023, "Two", {{"d", "b"}, {"title", "Second"}}, now + 200);
        QSignalSpy finished(&backend, &Backend::refreshFinished);
        backend.refreshAll();
        QVERIFY(finished.wait(5000));
        QVERIFY2(backend.status().startsWith("1 new article"), qPrintable(backend.status()));
    }

    // Sync

    void syncLogsInAndOutWithAnNpubOnly()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const QString npub = syncNpub();
        QString out, err;
        QCOMPARE(run({"sync"}, out, err), 1);
        QVERIFY2(err.contains("sync login NPUB"), qPrintable(err));
        err.clear();
        QCOMPARE(run({"sync", "login", "npub1notreal"}, out, err), 1);
        QVERIFY(err.contains("npub or nprofile"));
        QCOMPARE(run({"sync", "login"}, out, err), 1);
        QCOMPARE(run({"sync", "pull"}, out, err), 1);
        QVERIFY(err.contains("unknown action"));

        // An nprofile is kept as its npub, with its public relay hints.
        const QByteArray key = QByteArray::fromHex(Nip19::decodeProfile(npub)->pubkey.toLatin1());
        const QByteArray relay = "wss://relay.example.com";
        const QString nprofile =
            bech32("nprofile", QByteArray(1, 0) + char(32) + key + char(1) + char(relay.size()) + relay);
        out.clear();
        QCOMPARE(run({"sync", "login", "nostr:" + nprofile}, out, err), 0);
        QVERIFY2(out.startsWith("Syncing with " + npub), qPrintable(out));
        {
            Store store;
            QVERIFY(store.open());
            const SyncAccount account = store.syncAccount();
            QCOMPARE(account.npub, npub);
            QCOMPARE(account.pubkey, Nip19::decodeProfile(npub)->pubkey);
            QCOMPARE(account.relays, QStringList{QString::fromLatin1(relay)});
            QVERIFY(!account.syncedAt.isValid());
            QCOMPARE(Sync::query(account).extraRelays, account.relays);

            // Another npub starts afresh; the same one keeps its watermarks.
            QVERIFY(store.setSyncApplied(Sync::readStatusTag, 100, QString(64, '5')));
            QVERIFY(store.setSyncApplied(Sync::readStatusTag, 50, QString(64, '1')));
            // A tie in created_at goes to the lower id, never the higher.
            QVERIFY(store.setSyncApplied(Sync::readStatusTag, 100, QString(64, '9')));
            QCOMPARE(store.syncAccount().appliedIds.value(Sync::readStatusTag), QString(64, '5'));
            QVERIFY(store.setSyncApplied(Sync::readStatusTag, 100, QString(64, '3')));
            QCOMPARE(store.syncAccount().appliedIds.value(Sync::readStatusTag), QString(64, '3'));
            QVERIFY(store.setSyncAccount(npub, account.pubkey, {}));
            QCOMPARE(store.syncAccount().applied.value(Sync::readStatusTag), 100);
            const QString other = Nip19::npub(QString(64, 'b'));
            QVERIFY(store.setSyncAccount(other, QString(64, 'b'), {}));
            QVERIFY(store.syncAccount().applied.isEmpty());
            // A pull for the account before is not merged into this one's.
            QCOMPARE(Sync::apply(store, account, NostrResult()).error, QString("the npub changed during the pull"));
        }
        out.clear();
        QCOMPARE(run({"sync", "logout"}, out, err), 0);
        QCOMPARE(out, "Stopped syncing with " + Nip19::npub(QString(64, 'b')) + "\n");
        out.clear();
        QCOMPARE(run({"sync", "logout"}, out, err), 0);
        QCOMPARE(out, QString("Not syncing with any npub\n"));
        Store store;
        QVERIFY(store.open());
        QVERIFY(store.syncAccount().npub.isEmpty());
    }

    // The web app's three events, pulled and merged into a library that
    // already has some of it: subscriptions only added (its `deleted` list
    // ignored), articles only marked read, views kept with local mute words.
    void syncPullsAndMergesEachKind()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME", "READSTR_RELAYS"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        qputenv("READSTR_RELAYS", relay.url().toUtf8());
        const QString author = Nip19::npub(QString(64, 'c'));
        qint64 existing = 0;
        {
            Store store;
            QVERIFY(store.open());
            existing = store.addFeed("https://a.example.com/feed", "A");
            ParsedItem one, two, three;
            one.guid = one.title = "g1";
            two.guid = two.title = "g2";
            three.guid = three.title = "g3";
            QCOMPARE(store.saveItems(existing, {one, two, three}), 3);
            for (const Article &article : store.articles({}))
                if (article.guid == "g3")
                    QVERIFY(store.setRead(article.id, true));
        }
        relay.events = {
            syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag,
                      {{"rss", QJsonArray{"https://A.example.com/feed/", "https://b.example.com/rss",
                                          "javascript:alert(1)", "file:///etc/passwd"}},
                       {"nostr", QJsonArray{author, "npub1bad"}},
                       {"categories", QJsonObject{{"https://b.example.com/rss", QJsonObject{{"name", "News"}}}}},
                       {"tags", QJsonObject{{author, QJsonArray{"nostr"}}}},
                       {"deleted", QJsonArray{"https://a.example.com/feed"}},
                       {"lastUpdated", now}},
                      now),
            syncEvent(Sync::readStatusKind, Sync::readStatusTag, {{"itemGuids", QJsonArray{"g1", "later"}}}, now),
            syncEvent(Sync::viewsKind, Sync::viewsTag,
                      {{"views", QJsonArray{QJsonObject{{"id", "v1"},
                                                        {"name", "Morning"},
                                                        {"order", 3},
                                                        {"source", QJsonObject{{"kind", "feed"}, {"feedId", "x"}}},
                                                        {"keywords", QJsonObject{{"include", QJsonArray{"ai"}},
                                                                                 {"exclude", QJsonArray{"leak"}}}}},
                                            QJsonObject{{"name", "  "}}}}},
                      now)};

        QString out, err;
        QCOMPARE(run({"sync", "login", syncNpub()}, out, err), 0);
        out.clear();
        QCOMPARE(run({"sync", "--json"}, out, err), 0);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QVERIFY(out.endsWith("}\n") && out.count('\n') == 1);
        const QJsonObject json = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(json.keys(), (QStringList{"added", "errors", "markedRead", "npub", "pendingRead", "skipped", "syncedAt",
                                           "unsupported", "views"}));
        QCOMPARE(json.value("npub").toString(), syncNpub());
        QCOMPARE(json.value("added").toInt(), 2);
        QCOMPARE(json.value("skipped").toInt(), 1);
        QCOMPARE(json.value("unsupported").toInt(), 3);
        QCOMPARE(json.value("markedRead").toInt(), 1);
        QCOMPARE(json.value("pendingRead").toInt(), 1);
        QCOMPARE(json.value("views").toInt(), 1);
        QVERIFY(json.value("syncedAt").toString().endsWith('Z'));
        QCOMPARE(json.value("errors").toArray(), QJsonArray());
        // One request for the three events, a filter for each kind and d tag.
        QCOMPARE(relay.requests.size(), 1);
        QCOMPARE(relay.requests.first().size(), 3);
        const QList<std::pair<int, const char *>> pairs{
            {30404, "readstr-subscriptions"}, {30405, "readstr-read-status"}, {30406, "readstr-views"}};
        for (qsizetype i = 0; i < pairs.size(); ++i) {
            const QJsonObject filter = relay.requests.first().at(i).toObject();
            QCOMPARE(filter.value("kinds").toArray(), QJsonArray{pairs.at(i).first});
            QCOMPARE(filter.value("#d").toArray(), QJsonArray{pairs.at(i).second});
            QCOMPARE(filter.value("limit").toInt(), 100);
        }

        Store store;
        QVERIFY(store.open());
        QHash<QString, Feed> feeds;
        for (const Feed &feed : store.feeds())
            feeds.insert(feed.url, feed);
        QCOMPARE(feeds.size(), 3);
        QVERIFY(feeds.contains("https://a.example.com/feed"));
        QCOMPARE(feeds.value("https://b.example.com/rss").category, QString("News"));
        QVERIFY(feeds.contains(author));
        QHash<QString, bool> read;
        for (const Article &article : store.articles({}))
            read.insert(article.guid, article.read);
        QCOMPARE(read, (QHash<QString, bool>{{"g1", true}, {"g2", false}, {"g3", true}}));
        // A read article that arrives later comes in read.
        ParsedItem later, other;
        later.guid = later.title = "later";
        other.guid = other.title = "other";
        QCOMPARE(store.saveItems(feeds.value("https://b.example.com/rss").id, {later, other}), 2);
        for (const Article &article : store.articles({}))
            if (article.guid == "later" || article.guid == "other")
                QCOMPARE(article.read, article.guid == "later");
        QCOMPARE(store.pendingReadCount(), 0);

        QList<SavedView> views = store.savedViews();
        QCOMPARE(views.size(), 1);
        QCOMPARE(views.first().id, QString("v1"));
        QCOMPARE(views.first().view.value("name").toString(), QString("Morning"));
        QCOMPARE(views.first().view.value("order").toInt(), 0);
        QCOMPARE(views.first().view.value("keywords").toObject(), (QJsonObject{{"include", QJsonArray{"ai"}}}));
        QVERIFY(views.first().exclude.isEmpty());

        // The same events again change nothing, so a feed unsubscribed here
        // stays gone and an article marked unread here stays unread.
        QVERIFY(store.removeFeed(feeds.value("https://b.example.com/rss").id));
        for (const Article &article : store.articles({}))
            if (article.guid == "g1")
                QVERIFY(store.setRead(article.id, false));
        views.first().exclude = QStringList{"mute"};
        QVERIFY(store.replaceSavedViews(views));
        out.clear();
        QCOMPARE(run({"sync", "--json"}, out, err), 0);
        const QJsonObject again = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(again.value("added").toInt(), 0);
        QCOMPARE(again.value("markedRead").toInt(), 0);
        QCOMPARE(store.feeds().size(), 2);

        // Newer ones apply: a view renamed keeps its local mute words, and a
        // local view the remote lacks stays.
        views << SavedView{"local", QJsonObject{{"id", "local"}, {"name", "Mine"}, {"order", 1}}, {}};
        QVERIFY(store.replaceSavedViews(views));
        relay.events = {
            syncEvent(Sync::readStatusKind, Sync::readStatusTag, {{"itemGuids", QJsonArray{"g2"}}}, now + 10),
            syncEvent(Sync::viewsKind, Sync::viewsTag,
                      {{"views", QJsonArray{QJsonObject{{"id", "v1"}, {"name", "Evening"},
                                                        {"keywords", QJsonObject{{"exclude", QJsonArray{"remote"}}}}}}}},
                      now + 10)};
        out.clear();
        QCOMPARE(run({"sync"}, out, err), 0);
        QVERIFY2(out.startsWith("Synced with " + syncNpub() + ": 0 feeds added"), qPrintable(out));
        read.clear();
        for (const Article &article : store.articles({}))
            read.insert(article.guid, article.read);
        QCOMPARE(read, (QHash<QString, bool>{{"g1", false}, {"g2", true}, {"g3", true}}));
        views = store.savedViews();
        QCOMPARE(views.size(), 2);
        QCOMPARE(views.first().view.value("name").toString(), QString("Evening"));
        QVERIFY(!views.first().view.contains("keywords"));
        QCOMPARE(views.first().exclude, QStringList{"mute"});
        QCOMPARE(views.last().id, QString("local"));
    }

    // RelayPool's checks hold for sync events: another author's, another
    // kind's, another d tag's, and a forged one are never applied.
    void syncRejectsEventsItDidNotAskFor()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        FakeRelay relay, forger;
        const EnvironmentGuard guard({"READSTR_HOME", "READSTR_RELAYS"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        qputenv("READSTR_RELAYS", (relay.url() + "," + forger.url()).toUtf8());
        const QJsonObject list{{"rss", QJsonArray{"https://evil.example.com/feed"}}, {"nostr", QJsonArray()}};
        const QString content = QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact));
        relay.events = {signedEvent(Sync::subscriptionsKind, content, {{"d", Sync::subscriptionsTag}}, now, 2),
                        signedEvent(30023, content, {{"d", Sync::subscriptionsTag}}, now),
                        signedEvent(Sync::subscriptionsKind, content, {{"d", "other"}}, now),
                        signedEvent(Sync::readStatusKind, content, {{"d", Sync::subscriptionsTag}}, now),
                        syncEvent(Sync::readStatusKind, Sync::readStatusTag, {{"itemGuids", QJsonArray{"g1"}}}, now)};
        QJsonObject forged = syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag,
                                       {{"rss", QJsonArray()}, {"nostr", QJsonArray()}}, now + 5);
        forged.insert("content", content);
        forger.events = {forged};

        QString out, err;
        QCOMPARE(run({"sync", "login", syncNpub()}, out, err), 0);
        out.clear();
        QCOMPARE(run({"sync", "--json"}, out, err), 0);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        const QJsonObject json = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(json.value("added").toInt(), 0);
        QCOMPARE(json.value("pendingRead").toInt(), 1);
        Store store;
        QVERIFY(store.open());
        QVERIFY(store.feeds().isEmpty());
        QVERIFY(!store.syncAccount().applied.contains(Sync::subscriptionsTag));

        // No relay answering is a failure, said on stderr.
        relay.closeWith = "blocked: no";
        forger.closeWith = "blocked: no";
        out.clear();
        err.clear();
        QCOMPARE(run({"sync", "--json"}, out, err), 1);
        QVERIFY2(err.contains("blocked: no"), qPrintable(err));
        QVERIFY(QJsonDocument::fromJson(out.toUtf8()).object().value("errors").toArray().first().toString().contains(
            "blocked: no"));
    }

    // Payloads past the web app's caps are refused whole and said so; an
    // event dated days ahead, or too large, is passed over for the newest
    // one that fits; and a list with nothing applied is tried again.
    void syncEnforcesCapsAndDates()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME", "READSTR_RELAYS"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        qputenv("READSTR_RELAYS", relay.url().toUtf8());
        QJsonArray tooMany;
        for (int i = 0; i <= Sync::maxFeeds; ++i)
            tooMany << QStringLiteral("https://f%1.example.com/feed").arg(i);
        QJsonArray views;
        for (int i = 0; i <= Sync::maxRemoteViews; ++i)
            views << QJsonObject{{"name", QString::number(i)}};
        relay.events = {
            syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag, {{"rss", tooMany}, {"nostr", QJsonArray()}}, now),
            syncEvent(Sync::readStatusKind, Sync::readStatusTag,
                      {{"itemGuids", QJsonArray{QString(Sync::maxGuidLength + 1, 'g')}}}, now),
            syncEvent(Sync::viewsKind, Sync::viewsTag, {{"views", views}}, now)};

        QString out, err;
        QCOMPARE(run({"sync", "login", syncNpub()}, out, err), 0);
        out.clear();
        QCOMPARE(run({"sync", "--json"}, out, err), 1);
        for (const char *problem : {"subscription list", "read status", "saved views"})
            QVERIFY2(err.contains(problem), qPrintable(err));
        QCOMPARE(QJsonDocument::fromJson(out.toUtf8()).object().value("errors").toArray().size(), 3);
        {
            Store store;
            QVERIFY(store.open());
            QVERIFY(store.feeds().isEmpty());
            QVERIFY(store.syncAccount().applied.isEmpty());
        }

        // Older events that fit, beside a newer one too large and one dated
        // three days ahead.
        relay.events = {
            syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag,
                      {{"rss", QJsonArray{"https://ok.example.com/feed", "ftp://x.example.com/feed"}},
                       {"nostr", QJsonArray()}},
                      now),
            syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag,
                      {{"rss", QJsonArray{"https://future.example.com/feed"}}, {"nostr", QJsonArray()}},
                      QDateTime::currentSecsSinceEpoch() + 3 * 24 * 3600),
            syncEvent(Sync::readStatusKind, Sync::readStatusTag, {{"itemGuids", QJsonArray{"small"}}}, now),
            syncEvent(Sync::readStatusKind, Sync::readStatusTag,
                      {{"itemGuids", QJsonArray{"big"}}, {"padding", QString(Sync::maxContentLength, 'x')}}, now + 5)};
        out.clear();
        err.clear();
        QCOMPARE(run({"sync", "--json"}, out, err), 0);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        const QJsonObject json = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(json.value("added").toInt(), 1);
        QCOMPARE(json.value("unsupported").toInt(), 1);
        Store store;
        QVERIFY(store.open());
        QCOMPARE(store.feeds().size(), 1);
        QCOMPARE(store.feeds().first().url, QString("https://ok.example.com/feed"));
        QCOMPARE(store.syncAccount().applied.value(Sync::readStatusTag), now);
    }

    // Another writer on the same library (the window and `readstr sync`)
    // may apply a newer event while a pull is out, or the npub be stopped
    // and set again; the pull's events are judged against the library as it
    // is when the pull writes, not as it was when the pull began.
    void syncJudgesFreshnessWhenItWrites()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        const QString path = dir.path() + "/readstr.db";
        Store store, other;
        QVERIFY2(store.open(path), qPrintable(store.lastError()));
        QVERIFY2(other.open(path), qPrintable(other.lastError()));
        const SyncAccount login = *Sync::account(syncNpub());
        QVERIFY(store.setSyncAccount(login.npub, login.pubkey, {}));
        // A library with feeds, so the subscription list's watermark counts.
        QVERIFY(store.addFeed("https://a.invalid/feed", "A"));
        const auto subscriptions = [](const char *url, qint64 at) {
            return syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag,
                             {{"rss", QJsonArray{url}}, {"nostr", QJsonArray()}}, at);
        };
        const auto read = [](const char *guid, qint64 at) {
            return syncEvent(Sync::readStatusKind, Sync::readStatusTag, {{"itemGuids", QJsonArray{guid}}}, at);
        };
        const auto views = [](const char *name, qint64 at) {
            return syncEvent(Sync::viewsKind, Sync::viewsTag,
                             {{"views", QJsonArray{QJsonObject{{"id", "v"}, {"name", name}}}}}, at);
        };

        const SyncAccount stale = store.syncAccount();
        Sync::Outcome outcome = Sync::apply(
            other, other.syncAccount(),
            syncResult({subscriptions("https://new.invalid/feed", now + 20), read("new", now + 20), views("New", now + 20)}));
        QVERIFY2(outcome.error.isEmpty() && outcome.problems.isEmpty(), qPrintable(outcome.error));
        QCOMPARE(outcome.added, 1);
        // The pull that began before holds older versions and applies none.
        outcome = Sync::apply(
            store, stale,
            syncResult({subscriptions("https://old.invalid/feed", now + 15), read("old", now + 15), views("Old", now + 15)}));
        QVERIFY2(outcome.error.isEmpty() && outcome.problems.isEmpty(), qPrintable(outcome.error));
        QCOMPARE(outcome.added, 0);
        QCOMPARE(outcome.markedRead, 0);
        QCOMPARE(store.feeds().size(), 2);
        QCOMPARE(store.savedViews().first().view.value("name").toString(), QString("New"));
        QCOMPARE(store.syncAccount().applied.value(Sync::readStatusTag), now + 20);
        ParsedItem oldItem, newItem;
        oldItem.guid = oldItem.title = "old";
        newItem.guid = newItem.title = "new";
        QCOMPARE(store.saveItems(store.feeds().first().id, {oldItem, newItem}), 2);
        for (const Article &article : store.articles({}))
            QCOMPARE(article.read, article.guid == "new");

        // Stopped and set again while a pull was out: what was applied for
        // the npub before is forgotten, so the pull's events apply rather
        // than being held to the old watermarks.
        const SyncAccount before = store.syncAccount();
        QVERIFY(other.clearSyncAccount());
        QVERIFY(other.setSyncAccount(login.npub, login.pubkey, {}));
        outcome = Sync::apply(store, before, syncResult({read("old", now + 10), views("Again", now + 10)}));
        QVERIFY2(outcome.error.isEmpty() && outcome.problems.isEmpty(), qPrintable(outcome.error));
        QCOMPARE(outcome.markedRead, 1);
        QCOMPARE(store.syncAccount().applied.value(Sync::readStatusTag), now + 10);
        QCOMPARE(store.savedViews().first().view.value("name").toString(), QString("Again"));
        QVERIFY(store.syncAccount().syncedAt.isValid());

        // Stopped while a pull was out: nothing is applied or dated.
        const SyncAccount gone = store.syncAccount();
        QVERIFY(other.clearSyncAccount());
        outcome = Sync::apply(store, gone, syncResult({views("Late", now + 30)}));
        QCOMPARE(outcome.error, QString("the npub changed during the pull"));
        QCOMPARE(store.savedViews().first().view.value("name").toString(), QString("Again"));
        QVERIFY(store.syncAccount().npub.isEmpty());

        // A newer event committed by the other writer after this pull's first
        // check, but before it writes: the check inside each write refuses
        // the pull's older events.
        QVERIFY(store.setSyncAccount(login.npub, login.pubkey, {}));
        QVERIFY(store.addFeed("https://b.invalid/feed", "B"));
        const SyncAccount racing = store.syncAccount();
        const qsizetype feedsBefore = store.feeds().size();
        outcome = Sync::apply(
            store, racing,
            syncResult({subscriptions("https://race.invalid/feed", now + 50), read("race", now + 50),
                        views("Race", now + 50)}),
            [&] {
                const Sync::Outcome won = Sync::apply(
                    other, other.syncAccount(),
                    syncResult({subscriptions("https://won.invalid/feed", now + 60), read("won", now + 60),
                                views("Won", now + 60)}));
                QVERIFY2(won.error.isEmpty() && won.problems.isEmpty(), qPrintable(won.error));
            });
        QVERIFY2(outcome.error.isEmpty() && outcome.problems.isEmpty(), qPrintable(outcome.error));
        QCOMPARE(outcome.added, 0);
        QCOMPARE(store.feeds().size(), feedsBefore + 1);
        for (const Feed &feed : store.feeds())
            QVERIFY(feed.url != "https://race.invalid/feed");
        QCOMPARE(store.savedViews().first().view.value("name").toString(), QString("Won"));
        QCOMPARE(store.syncAccount().applied.value(Sync::readStatusTag), now + 60);
        QCOMPARE(store.syncAccount().applied.value(Sync::subscriptionsTag), now + 60);

        // Stopped by the other writer between the first check and the writes:
        // the import's gate sees it and nothing is added.
        const SyncAccount stopping = store.syncAccount();
        outcome = Sync::apply(store, stopping, syncResult({subscriptions("https://late.invalid/feed", now + 70)}),
                              [&] { QVERIFY(other.clearSyncAccount()); });
        QCOMPARE(outcome.error, QString("the npub changed during the pull"));
        for (const Feed &feed : store.feeds())
            QVERIFY(feed.url != "https://late.invalid/feed");

        // Each kind is judged against the watermarks as they are when it is
        // reached: while one newer is there, an older event and an older
        // payload that does not fit are passed over unread, and once the
        // account is stopped and set again after the pull's first check,
        // both count, the payload being said not to fit.
        QVERIFY(store.setSyncAccount(login.npub, login.pubkey, {}));
        QVERIFY(store.setSyncApplied(Sync::readStatusTag, now + 20, QString(64, '0')));
        QVERIFY(store.setSyncApplied(Sync::viewsTag, now + 20, QString(64, '0')));
        const QJsonObject badViews = syncEvent(Sync::viewsKind, Sync::viewsTag, {{"views", "none"}}, now + 10);
        outcome = Sync::apply(store, store.syncAccount(), syncResult({read("reset", now + 10), badViews}));
        QVERIFY2(outcome.error.isEmpty() && outcome.problems.isEmpty(), qPrintable(outcome.problems.join("; ")));
        QCOMPARE(store.syncAccount().applied.value(Sync::readStatusTag), now + 20);
        outcome = Sync::apply(store, store.syncAccount(), syncResult({read("reset", now + 10), badViews}), [&] {
            QVERIFY(other.clearSyncAccount());
            QVERIFY(other.setSyncAccount(login.npub, login.pubkey, {}));
        });
        QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
        QCOMPARE(outcome.problems, QStringList{"the saved views do not fit the web app's format"});
        QCOMPARE(store.syncAccount().applied.value(Sync::readStatusTag), now + 10);
    }

    // Two versions in the same second are ordered by id, as the pool and
    // NIP-01 order them: the lower id applies after the higher, and neither
    // the higher nor the same event again applies after it.
    void syncBreaksATieInCreatedAtById()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        Store store;
        QVERIFY2(store.open(dir.path() + "/readstr.db"), qPrintable(store.lastError()));
        const SyncAccount account = *Sync::account(syncNpub());
        QVERIFY(store.setSyncAccount(account.npub, account.pubkey, {}));
        QJsonObject low = syncEvent(Sync::readStatusKind, Sync::readStatusTag, {{"itemGuids", QJsonArray{"a"}}}, now);
        QJsonObject high = syncEvent(Sync::readStatusKind, Sync::readStatusTag, {{"itemGuids", QJsonArray{"b"}}}, now);
        if (high.value("id").toString() < low.value("id").toString())
            std::swap(low, high);
        const auto applies = [&](const QJsonObject &event) {
            const QString before = store.syncAccount().appliedIds.value(Sync::readStatusTag);
            const Sync::Outcome outcome = Sync::apply(store, store.syncAccount(), syncResult({event}));
            return outcome.error.isEmpty() && outcome.problems.isEmpty()
                && store.syncAccount().appliedIds.value(Sync::readStatusTag) != before;
        };
        QVERIFY(applies(high));
        QVERIFY(applies(low));
        QVERIFY(!applies(high));
        QVERIFY(!applies(low));
        QCOMPARE(store.syncAccount().appliedIds.value(Sync::readStatusTag), low.value("id").toString());
        QCOMPARE(store.syncAccount().applied.value(Sync::readStatusTag), now);
    }

    // A payload refused for not fitting keeps the last sync's date, though
    // the kinds beside it that fit apply; a pull with nothing refused, even
    // one that finds no events, is dated as before.
    void syncDoesNotDateAPullWithARefusal()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        Store store;
        QVERIFY2(store.open(dir.path() + "/readstr.db"), qPrintable(store.lastError()));
        const SyncAccount account = *Sync::account(syncNpub());
        QVERIFY(store.setSyncAccount(account.npub, account.pubkey, {}));
        Sync::Outcome outcome = Sync::apply(
            store, store.syncAccount(),
            syncResult({syncEvent(Sync::readStatusKind, Sync::readStatusTag, {{"itemGuids", QJsonArray{"g"}}}, now),
                        syncEvent(Sync::viewsKind, Sync::viewsTag, {{"views", "none"}}, now)}));
        QVERIFY(outcome.error.isEmpty());
        QCOMPARE(outcome.problems, QStringList{"the saved views do not fit the web app's format"});
        QCOMPARE(outcome.pendingRead, 1);
        QCOMPARE(store.syncAccount().applied.value(Sync::readStatusTag), now);
        QVERIFY(!store.syncAccount().syncedAt.isValid());

        outcome = Sync::apply(store, store.syncAccount(), syncResult({}));
        QVERIFY(outcome.error.isEmpty() && outcome.problems.isEmpty());
        QVERIFY(store.syncAccount().syncedAt.isValid());
    }

    // An event past the size cap is passed over for an older one that fits,
    // and refused, and said so, only when no version that fits came; author
    // queries, which set no cap, keep events of any size.
    void syncRefusesAnEventWithNoVersionThatFits()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        FakeRelay relay;
        const SyncAccount account = *Sync::account(syncNpub());
        const QString padding(200, 'x');
        relay.events = {
            syncEvent(Sync::readStatusKind, Sync::readStatusTag, {{"itemGuids", QJsonArray{"small"}}}, now),
            syncEvent(Sync::readStatusKind, Sync::readStatusTag, {{"itemGuids", QJsonArray{"big"}}, {"padding", padding}},
                      now + 5),
            syncEvent(Sync::viewsKind, Sync::viewsTag, {{"views", QJsonArray()}, {"padding", padding}}, now),
            syncEvent(Sync::viewsKind, Sync::viewsTag, {{"views", QJsonArray()}, {"padding", padding}}, now + 5)};
        RelayPool pool;
        pool.setLimits(Sync::limits());
        pool.setRelays({relay.url()});
        NostrQuery query = Sync::query(account);
        query.maxContentLength = 150;
        const NostrResult result = pool.query(query);
        QVERIFY2(result.ok(), qPrintable(result.error));
        QCOMPARE(result.events.size(), 1);
        QCOMPARE(result.events.first().createdAt, now);
        QCOMPARE(result.oversized.size(), 1);
        QCOMPARE(result.oversized.first().kind, Sync::viewsKind);
        QCOMPARE(result.oversized.first().tag("d"), QString(Sync::viewsTag));
        QCOMPARE(result.oversized.first().id, relay.events.last().toObject().value("id").toString());
        QVERIFY(result.oversized.first().content.isEmpty());

        query.maxContentLength = 0;
        const NostrResult uncapped = pool.query(query);
        QCOMPARE(uncapped.events.size(), 2);
        QVERIFY(uncapped.oversized.isEmpty());

        QTemporaryDir dir;
        Store store;
        QVERIFY2(store.open(dir.path() + "/readstr.db"), qPrintable(store.lastError()));
        QVERIFY(store.setSyncAccount(account.npub, account.pubkey, {}));
        const Sync::Outcome outcome = Sync::apply(store, store.syncAccount(), result);
        QVERIFY(outcome.error.isEmpty());
        QCOMPARE(outcome.problems, QStringList{"the saved views do not fit the web app's format"});
        QCOMPARE(outcome.pendingRead, 1);
        QVERIFY(!store.syncAccount().syncedAt.isValid());
    }

    // An oversized event no newer than what was already applied for its
    // kind is obsolete, as one that fits would be, so it fails no pull; one
    // newer than that, or any after the account starts afresh, is refused.
    void syncRefusesOnlyAFreshOversizedEvent()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        FakeRelay relay;
        const SyncAccount account = *Sync::account(syncNpub());
        relay.events = {syncEvent(Sync::viewsKind, Sync::viewsTag,
                                  {{"views", QJsonArray()}, {"padding", QString(200, 'x')}}, now)};
        RelayPool pool;
        pool.setLimits(Sync::limits());
        pool.setRelays({relay.url()});
        NostrQuery query = Sync::query(account);
        query.maxContentLength = 150;
        const NostrResult result = pool.query(query);
        QVERIFY2(result.ok(), qPrintable(result.error));
        QVERIFY(result.events.isEmpty());
        QCOMPARE(result.oversized.size(), 1);
        QCOMPARE(result.oversized.first().createdAt, now);

        QTemporaryDir dir;
        Store store;
        QVERIFY2(store.open(dir.path() + "/readstr.db"), qPrintable(store.lastError()));
        QVERIFY(store.setSyncAccount(account.npub, account.pubkey, {}));
        QVERIFY(store.setSyncApplied(Sync::viewsTag, now + 100, QString(64, 'f')));
        Sync::Outcome outcome = Sync::apply(store, store.syncAccount(), result);
        QVERIFY(outcome.error.isEmpty());
        QVERIFY2(outcome.problems.isEmpty(), qPrintable(outcome.problems.join("; ")));
        QVERIFY(store.syncAccount().syncedAt.isValid());

        QVERIFY(store.clearSyncAccount());
        QVERIFY(store.setSyncAccount(account.npub, account.pubkey, {}));
        QVERIFY(store.setSyncApplied(Sync::viewsTag, now - 100, QString(64, 'f')));
        outcome = Sync::apply(store, store.syncAccount(), result);
        QVERIFY(outcome.error.isEmpty());
        QCOMPARE(outcome.problems, QStringList{"the saved views do not fit the web app's format"});
        QVERIFY(!store.syncAccount().syncedAt.isValid());

        QVERIFY(store.clearSyncAccount());
        QVERIFY(store.setSyncAccount(account.npub, account.pubkey, {}));
        outcome = Sync::apply(store, store.syncAccount(), result);
        QCOMPARE(outcome.problems, QStringList{"the saved views do not fit the web app's format"});
        QVERIFY(!store.syncAccount().syncedAt.isValid());
    }

    // Each kind and d tag pair is asked for with its own limit, so the old
    // versions a relay keeps of one cannot crowd another out.
    void syncGivesEachPairItsOwnLimit()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        FakeRelay relay;
        relay.honourFilters = true;
        const SyncAccount account = *Sync::account(syncNpub());
        relay.events = {
            syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag, {{"rss", QJsonArray()}, {"nostr", QJsonArray()}}, now)};
        for (int i = 1; i <= 3; ++i)
            relay.events << syncEvent(Sync::readStatusKind, Sync::readStatusTag,
                                      {{"itemGuids", QJsonArray{QString::number(i)}}}, now + i);
        RelayPool pool;
        pool.setLimits(Sync::limits());
        pool.setRelays({relay.url()});
        NostrQuery query = Sync::query(account);
        query.limit = 2;
        const NostrResult result = pool.query(query);
        QVERIFY2(result.ok(), qPrintable(result.error));
        QHash<int, qint64> kinds;
        for (const NostrEvent &event : result.events)
            kinds.insert(event.kind, event.createdAt);
        QCOMPARE(kinds, (QHash<int, qint64>{{Sync::subscriptionsKind, now}, {Sync::readStatusKind, now + 3}}));
    }

    // Whoever holds the npub's key picks the synced feeds, which the window
    // fetches at once, so none on this machine or the local network is
    // added; OPML import, from a file the user picked, still takes them.
    void syncLeavesOutLocalFeeds()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        Store store;
        QVERIFY2(store.open(dir.path() + "/readstr.db"), qPrintable(store.lastError()));
        const SyncAccount account = *Sync::account(syncNpub());
        QVERIFY(store.setSyncAccount(account.npub, account.pubkey, {}));
        const Sync::Outcome outcome = Sync::apply(
            store, store.syncAccount(),
            syncResult({syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag,
                                  {{"rss", QJsonArray{"http://127.0.0.1:8080/feed", "http://192.168.1.2/rss",
                                                      "http://localhost/feed", "http://[::1]/feed",
                                                      "http://router/feed", "https://ok.invalid/feed"}},
                                   {"nostr", QJsonArray()}},
                                  now)}));
        QVERIFY2(outcome.error.isEmpty() && outcome.problems.isEmpty(), qPrintable(outcome.error));
        QCOMPARE(outcome.added, 1);
        QCOMPARE(outcome.unsupported, 5);
        QCOMPARE(store.feeds().size(), 1);
        QCOMPARE(store.feeds().first().url, QString("https://ok.invalid/feed"));

        const Library::Import imported =
            Library::importOutlines(store, {Opml::Outline{{}, "http://127.0.0.1:8080/feed", {}, {}, {}}});
        QCOMPARE(imported.added, 1);
    }

    // A lastUpdated past the safe integers, or with a fraction, is refused
    // as zod refuses it, rather than cast (undefined for 1e300).
    void syncRefusesAnUnsafeLastUpdated()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        Store store;
        QVERIFY2(store.open(dir.path() + "/readstr.db"), qPrintable(store.lastError()));
        const SyncAccount account = *Sync::account(syncNpub());
        QVERIFY(store.setSyncAccount(account.npub, account.pubkey, {}));
        for (const double lastUpdated : {1e300, 9007199254740992.0, 1.5, -1.0}) {
            const Sync::Outcome outcome = Sync::apply(
                store, store.syncAccount(),
                syncResult({syncEvent(Sync::readStatusKind, Sync::readStatusTag,
                                      {{"itemGuids", QJsonArray{"g"}}, {"lastUpdated", lastUpdated}}, now)}));
            QVERIFY(outcome.error.isEmpty());
            QCOMPARE(outcome.problems, QStringList{"the read status does not fit the web app's format"});
        }
        const Sync::Outcome outcome = Sync::apply(
            store, store.syncAccount(),
            syncResult({syncEvent(Sync::readStatusKind, Sync::readStatusTag,
                                  {{"itemGuids", QJsonArray{"g"}}, {"lastUpdated", 9007199254740991.0}}, now)}));
        QVERIFY(outcome.problems.isEmpty());
        QCOMPARE(outcome.pendingRead, 1);
    }

    // A view without an id gets the same one from every newer event, so it
    // is updated in place rather than piling up as copies.
    void syncKeepsOneCopyOfAViewWithoutAnId()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        Store store;
        QVERIFY2(store.open(dir.path() + "/readstr.db"), qPrintable(store.lastError()));
        const SyncAccount account = *Sync::account(syncNpub());
        QVERIFY(store.setSyncAccount(account.npub, account.pubkey, {}));
        for (qint64 at : {now, now + 1, now + 2}) {
            const Sync::Outcome outcome = Sync::apply(
                store, store.syncAccount(),
                syncResult({syncEvent(Sync::viewsKind, Sync::viewsTag,
                                      {{"views", QJsonArray{QJsonObject{{"name", "Morning"}}}}}, at)}));
            QVERIFY2(outcome.error.isEmpty() && outcome.problems.isEmpty(), qPrintable(outcome.error));
            QCOMPARE(outcome.views, 1);
        }
        QVERIFY(store.savedViews().first().id.startsWith("synced-"));
    }

    // Only the kind and d tag pairs asked for are kept, so an event of one
    // kind carrying another's d tag cannot fill the byte cap and push the
    // real one out; a pull cut short anyway says so and is not dated.
    void syncKeepsOnlyThePairsItAsksFor()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        FakeRelay relay;
        const SyncAccount account = *Sync::account(syncNpub());
        const QJsonObject real =
            syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag, {{"rss", QJsonArray()}, {"nostr", QJsonArray()}}, now);
        relay.events = {signedEvent(Sync::readStatusKind, QString(100000, 'x'), {{"d", Sync::subscriptionsTag}}, now + 5),
                        signedEvent(Sync::viewsKind, QString(100000, 'x'), {{"d", Sync::readStatusTag}}, now + 5),
                        real};
        RelayPool pool;
        RelayPool::Limits limits = Sync::limits();
        limits.maxBytesPerQuery = 50000;
        pool.setLimits(limits);
        pool.setRelays({relay.url()});
        const NostrResult result = pool.query(Sync::query(account));
        QVERIFY2(result.ok(), qPrintable(result.error));
        QVERIFY(!result.truncated);
        QCOMPARE(result.events.size(), 1);
        QCOMPARE(result.events.first().id, real.value("id").toString());

        QTemporaryDir dir;
        Store store;
        QVERIFY2(store.open(dir.path() + "/readstr.db"), qPrintable(store.lastError()));
        QVERIFY(store.setSyncAccount(account.npub, account.pubkey, {}));
        NostrResult truncated = syncResult({syncEvent(Sync::readStatusKind, Sync::readStatusTag,
                                                      {{"itemGuids", QJsonArray{"g"}}}, now)});
        truncated.truncated = true;
        const Sync::Outcome outcome = Sync::apply(store, store.syncAccount(), truncated);
        QVERIFY(outcome.error.isEmpty());
        QCOMPARE(outcome.problems, QStringList{"the relays sent more than fits, so some sync events may be missing"});
        QCOMPARE(outcome.pendingRead, 1);
        QVERIFY(!store.syncAccount().syncedAt.isValid());
    }

    void syncMigratesALibraryFromVersionFour()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/readstr.db";
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase("QSQLITE", "v4");
            raw.setDatabaseName(path);
            QVERIFY(raw.open());
            QSqlQuery query(raw);
            QStringList statements = versionOneSchema();
            statements << "ALTER TABLE items ADD COLUMN address TEXT"
                       << "ALTER TABLE items ADD COLUMN revision INTEGER"
                       << "CREATE INDEX items_by_address ON items(feed_id, address) WHERE address IS NOT NULL"
                       << "ALTER TABLE feeds ADD COLUMN relay_hints TEXT"
                       << "ALTER TABLE feeds ADD COLUMN profile_revision INTEGER"
                       << "ALTER TABLE feeds ADD COLUMN profile_event_id TEXT"
                       << "ALTER TABLE feeds ADD COLUMN profile_title TEXT"
                       << "ALTER TABLE feeds ADD COLUMN complete_at INTEGER"
                       << "ALTER TABLE items ADD COLUMN media_url TEXT"
                       << "ALTER TABLE items ADD COLUMN thumbnail_url TEXT"
                       << "ALTER TABLE items ADD COLUMN duration REAL"
                       << "ALTER TABLE feeds ADD COLUMN incomplete INTEGER NOT NULL DEFAULT 0"
                       << "INSERT INTO feeds (url, url_key, title, created_at) VALUES ('https://a.example.com/feed',"
                          " 'a.example.com/feed', 'A', 1)"
                       << "INSERT INTO items (feed_id, guid, title, created_at, is_starred) VALUES (1, 'old', 'Old', 1, 1)"
                       << "PRAGMA user_version = 4";
            for (const QString &statement : statements)
                QVERIFY2(query.exec(statement), qPrintable(query.lastError().text()));
            query = QSqlQuery();
            raw.close();
            raw = QSqlDatabase();
            QSqlDatabase::removeDatabase("v4");
        }
        Store store;
        QVERIFY2(store.open(path), qPrintable(store.lastError()));
        QCOMPARE(store.schemaVersion(), 6);
        QCOMPARE(store.articles({}).first().title, QString("Old"));
        QVERIFY(store.articles({}).first().starred);
        const SyncAccount none = store.syncAccount();
        QVERIFY(store.lastError().isEmpty());
        QVERIFY(none.npub.isEmpty());
        QVERIFY(store.savedViews().isEmpty());
        QCOMPARE(store.applyReadGuids({"old", "new"}), 1);
        QCOMPARE(store.pendingReadCount(), 1);
        QVERIFY(store.articles({}).first().read);
        QVERIFY(store.setSyncAccount(Nip19::npub(QString(64, 'a')), QString(64, 'a'), {}));
        QVERIFY(store.recordSynced(QDateTime::currentDateTimeUtc()));
        QVERIFY(store.syncAccount().syncedAt.isValid());
    }

    // The window pulls once the dialog sets an npub, and after a refresh of
    // every feed, and says in the status bar what came of it.
    void backendSyncsAfterLoginAndRefresh()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        // The account follows its own author, whose profile names the feed.
        const QString author = syncNpub();
        relay.events = {syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag,
                                  {{"rss", QJsonArray()}, {"nostr", QJsonArray{author}}}, now),
                        signedEvent(0, R"({"name":"me"})", {}, now)};

        Backend backend;
        backend.setRelays({relay.url()});
        QSignalSpy synced(&backend, &Backend::syncFinished);
        QVERIFY(!backend.setSyncNpub("not an npub"));
        QVERIFY(backend.syncNpub().isEmpty());
        QVERIFY(backend.setSyncNpub(syncNpub()));
        QVERIFY(synced.wait(10000));
        QVERIFY2(backend.status().startsWith("Synced with Nostr: 1 feed"), qPrintable(backend.status()));
        QVERIFY(!backend.statusError());
        QCOMPARE(backend.syncNpub(), syncNpub());
        QVERIFY(backend.syncedAt().isValid());
        // The feed it added is fetched at once.
        QSignalSpy finished(&backend, &Backend::refreshFinished);
        QVERIFY(finished.wait(10000));
        QCOMPARE(backend.feedCount(), 1);

        relay.closeWith = "blocked: no";
        backend.refreshAll();
        QVERIFY(synced.wait(10000));
        QVERIFY2(backend.status().startsWith("Cannot sync with Nostr: "), qPrintable(backend.status()));
        QVERIFY(backend.statusError());

        backend.stopSync();
        QTRY_VERIFY(backend.syncNpub().isEmpty());
        QVERIFY(backend.status().startsWith("Stopped syncing with npub1"));
    }

    // A pull asked for while one is out runs once that one is done, rather
    // than being dropped, and a refresh that ends during the first pull,
    // with a feed that failed, keeps saying so when neither pull changes
    // anything.
    void backendPullsAgainForARequestDuringAPull()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        FakeHttpServer server;
        server.route("/feed", [](const FakeHttpServer::Request &) { return FakeHttpServer::Response{404, {}, {}}; });
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        {
            Store store;
            QVERIFY(store.open());
            QVERIFY(store.addFeed(server.url("/feed").toString(), "Broken"));
        }
        // Slow enough that the refresh ends while the first pull is out.
        relay.delayMs = 500;
        Backend backend;
        backend.setRelays({relay.url()});
        QSignalSpy synced(&backend, &Backend::syncFinished);
        bool asked = false;
        connect(&backend, &Backend::syncChanged, &backend, [&] {
            if (backend.syncing() && !std::exchange(asked, true))
                backend.refreshAll();
        });
        QVERIFY(backend.setSyncNpub(syncNpub()));
        QVERIFY(synced.wait(10000));
        QCOMPARE(relay.requests.size(), 2);
        QVERIFY2(backend.status().startsWith("Cannot refresh Broken"), qPrintable(backend.status()));
        QVERIFY(backend.statusError());
        QVERIFY(!backend.syncing());
        QTest::qWait(800);
        QCOMPARE(synced.count(), 1);
        QCOMPARE(relay.requests.size(), 2);
    }

    // A feed one account's pull added, whose fetch waited for the follow-up
    // pull, is still fetched when the account changes during that follow-up
    // and the new account's pull adds nothing.
    void backendFetchesAFeedAddedBeforeTheAccountChanged()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        relay.events = {syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag,
                                  {{"rss", QJsonArray()}, {"nostr", QJsonArray{syncNpub()}}}, now),
                        signedEvent(0, R"({"name":"me"})", {}, now)};
        relay.delayMs = 300;
        const QString other = Nip19::npub(signedEvent(1, "", {}, 1, 2).value("pubkey").toString());
        Backend backend;
        backend.setRelays({relay.url()});
        QSignalSpy synced(&backend, &Backend::syncFinished);
        QSignalSpy finished(&backend, &Backend::refreshFinished);
        // Counted as each pull starts, since syncChanged also says the
        // account changed.
        int pulls = 0;
        bool wasSyncing = false;
        connect(&backend, &Backend::syncChanged, &backend, [&] {
            if (!backend.syncing() || std::exchange(wasSyncing, true)) {
                wasSyncing = backend.syncing();
                return;
            }
            if (++pulls == 1)
                QVERIFY(backend.setSyncNpub(syncNpub()));
            else if (pulls == 2)
                QVERIFY(backend.setSyncNpub(other));
        });
        QVERIFY(backend.setSyncNpub(syncNpub()));
        QVERIFY(synced.wait(10000));
        QCOMPARE(pulls, 3);
        QCOMPARE(backend.syncNpub(), other);
        QVERIFY2(backend.status().startsWith("Synced with Nostr: 0 feeds"), qPrintable(backend.status()));
        QCOMPARE(backend.feedCount(), 1);
        QVERIFY(finished.wait(10000));
        Store store;
        QVERIFY(store.open());
        QVERIFY(store.feeds().first().lastFetchedAt.isValid());
    }

    // The account's key goes to the keyring only when it is the account's,
    // is read back from it only to sign, once, and goes with "Forget key".
    void backendKeepsTheSigningKeyInTheKeyring()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub(), shortNpub = Nip19::shortNpub(npub, npub);
        FakeKeyStore keys;
        Backend backend;
        backend.setKeyStore(&keys);
        QVERIFY(backend.signingBusy());
        QTRY_VERIFY(!backend.signingBusy());
        QVERIFY(!backend.signingKeyStored());
        QVERIFY(backend.signingError().isEmpty());

        // Another account's key, or no key at all, is refused, and nothing
        // reaches the keyring.
        QVERIFY(!backend.addSigningKey(QByteArray(32, '\2').toHex()));
        QCOMPARE(backend.signingError(), "That key is not the one for " + shortNpub);
        QVERIFY(!backend.addSigningKey("nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5"));
        QCOMPARE(backend.signingError(), "That key is not the one for " + shortNpub);
        QVERIFY(!backend.addSigningKey("nsec1notakey"));
        QCOMPARE(backend.signingError(), QString("That is not an nsec or a 64-character hex secret key"));
        QVERIFY(!backend.addSigningKey(QString(64, '0')));
        QCOMPARE(backend.signingError(), QString("That is not an nsec or a 64-character hex secret key"));
        QVERIFY(!backend.addSigningKey(npub));
        QTest::qWait(20);
        QCOMPARE(keys.calls, QStringList{"contains " + npub});
        QVERIFY(keys.keys.isEmpty());
        QVERIFY(!backend.signingKeyStored());

        QVERIFY(backend.addSigningKey(" " + QByteArray(32, '\1').toHex().toUpper() + " "));
        QVERIFY(backend.signingError().isEmpty());
        QTRY_VERIFY(backend.signingKeyStored());
        QVERIFY(!backend.signingBusy());
        QCOMPARE(keys.keys.value(npub), QByteArray(32, '\1'));
        QCOMPARE(keys.calls, QStringList({"contains " + npub, "store " + npub}));

        // Read once, for the first event signed, and kept for the next.
        NostrEvent draft;
        draft.kind = Sync::subscriptionsKind;
        draft.createdAt = 1700000000;
        draft.tags = {{"d", Sync::subscriptionsTag}};
        draft.content = "{}";
        QList<NostrEvent> signedEvents;
        QStringList errors;
        const auto keep = [&](const NostrEvent &event, const QString &error) {
            signedEvents << event;
            errors << error;
        };
        backend.sign(draft, keep);
        backend.sign(draft, keep);
        QTRY_COMPARE(signedEvents.size(), 2);
        backend.sign(draft, keep);
        QTRY_COMPARE(signedEvents.size(), 3);
        QCOMPARE(errors, QStringList({"", "", ""}));
        for (const NostrEvent &event : std::as_const(signedEvents)) {
            QCOMPARE(event.pubkey, syncPubkey());
            QVERIFY(event.verify());
        }
        QCOMPARE(keys.calls.count("lookup " + npub), 1);

        backend.forgetSigningKey();
        QTRY_VERIFY(!backend.signingKeyStored());
        QVERIFY(keys.keys.isEmpty());
        QCOMPARE(keys.calls.last(), "remove " + npub);
        backend.sign(draft, keep);
        QTRY_COMPARE(errors.size(), 4);
        QCOMPARE(errors.last(), "No signing key is stored for " + shortNpub);
        QCOMPARE(keys.calls.count("lookup " + npub), 1);
    }

    // On launch the keyring is only asked whether it holds a key, which
    // reads nothing, and a key put there by another program is checked
    // against the account before anything is signed with it.
    void backendOnlyAsksWhetherAKeyIsStored()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        FakeKeyStore keys;
        keys.keys.insert(npub, QByteArray(32, '\2'));
        Backend backend;
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QTest::qWait(20);
        QCOMPARE(keys.calls, QStringList{"contains " + npub});

        QString error;
        backend.sign({}, [&](const NostrEvent &, const QString &answer) { error = answer; });
        QTRY_VERIFY(!error.isEmpty());
        QCOMPARE(error, "The key in the keyring is not the one for " + Nip19::shortNpub(npub, npub));

        // Without an account there is no key to keep.
        backend.stopSync();
        QTRY_VERIFY(backend.syncNpub().isEmpty());
        QVERIFY(!backend.signingKeyStored());
        QVERIFY(!backend.addSigningKey(QByteArray(32, '\1').toHex()));
        QCOMPARE(backend.signingError(), QString("Sync with an npub before adding its signing key"));
        QCOMPARE(keys.calls, QStringList({"contains " + npub, "lookup " + npub}));
    }

    // Without a Secret Service the dialog says so, and nothing weaker keeps
    // the key: no trace of it is left in Readstr's files.
    void backendSaysWhenNoSecretServiceRuns()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QByteArray secret = QByteArray(32, '\1').toHex();
        FakeKeyStore keys;
        keys.error = KeyStore::Error::Unavailable;
        {
            Backend backend;
            backend.setKeyStore(&keys);
            QTRY_VERIFY(!backend.signingBusy());
            QVERIFY(backend.signingError().startsWith("No Secret Service is running"));
            QVERIFY(backend.addSigningKey(secret));
            QTRY_VERIFY(!backend.signingBusy());
            QVERIFY(backend.signingError().startsWith("No Secret Service is running"));
            QVERIFY(!backend.signingKeyStored());

            keys.error = KeyStore::Error::Dismissed;
            QVERIFY(backend.addSigningKey(secret));
            QTRY_COMPARE(backend.signingError(), QString("The keyring prompt was dismissed."));
            keys.error = KeyStore::Error::Locked;
            QVERIFY(backend.addSigningKey(secret));
            QTRY_COMPARE(backend.signingError(), QString("The keyring is locked. Unlock it and try again."));
            QVERIFY(!backend.signingKeyStored());
            QVERIFY(keys.keys.isEmpty());

            // No store at all, as before one is set, is no Secret Service.
            Backend bare;
            QVERIFY(!bare.addSigningKey(secret));
            QVERIFY(bare.signingError().startsWith("No Secret Service is running"));
        }
        QDirIterator files(dir.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        int checked = 0;
        while (files.hasNext()) {
            QFile file(files.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray contents = file.readAll();
            QVERIFY2(!contents.contains(secret) && !contents.contains(secret.toUpper())
                         && !contents.contains(QByteArray(32, '\1')),
                     qPrintable(file.fileName()));
            ++checked;
        }
        QVERIFY(checked > 0);
    }

    // The dialog's key field: masked, cleared as soon as it is submitted or
    // the dialog closes, and the key kept only when it is the account's.
    void syncDialogAddsAndForgetsTheSigningKey()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        FakeKeyStore keys;
        Backend backend;
        backend.setKeyStore(&keys);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty(
            "theme", QVariantMap{{"radius", 0}, {"background", "#000000"}, {"foreground", "#ffffff"}});
        engine.rootContext()->setContextProperty("translator", QVariantMap{{"language", "en"}});
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport QtQuick.Controls\n"
                          "ApplicationWindow { width: 800; height: 600; property alias dialog: dialog\n"
                          " SyncDialog { id: dialog } }\n",
                          QUrl("qrc:/probe.qml"));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *dialog = window->property("dialog").value<QObject *>();
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QObject *field = window->findChild<QObject *>("signingKeyField");
        QObject *add = window->findChild<QObject *>("addSigningKeyButton");
        QObject *forget = window->findChild<QObject *>("forgetSigningKeyButton");
        QObject *error = window->findChild<QObject *>("signingErrorLabel");
        QVERIFY(field && add && forget && error);
        QTRY_VERIFY(!backend.signingBusy());
        QCOMPARE(field->property("echoMode").toInt(), 2);
        QVERIFY(field->property("visible").toBool());
        QVERIFY(!forget->property("visible").toBool());

        // Typed, so that it is in the field's undo history, as a key the
        // user pastes would be; once submitted, undo cannot bring it back.
        const auto type = [field](const QString &text) {
            QVERIFY(QMetaObject::invokeMethod(field, "insert", Q_ARG(int, 0), Q_ARG(QString, text)));
            QCOMPARE(field->property("text").toString(), text);
            QVERIFY(field->property("canUndo").toBool());
        };
        const auto undoneToNothing = [field] {
            QVERIFY(!field->property("canUndo").toBool());
            QVERIFY(QMetaObject::invokeMethod(field, "undo"));
            QCOMPARE(field->property("text").toString(), QString());
        };
        type(QString(QByteArray(32, '\2').toHex()));
        QVERIFY(add->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(add, "clicked"));
        QCOMPARE(field->property("text").toString(), QString());
        undoneToNothing();
        QVERIFY(error->property("visible").toBool());
        QVERIFY(error->property("text").toString().startsWith("That key is not the one for npub1"));
        QVERIFY(keys.keys.isEmpty());

        type(QString(QByteArray(32, '\1').toHex()));
        QVERIFY(QMetaObject::invokeMethod(field, "accepted"));
        QCOMPARE(field->property("text").toString(), QString());
        undoneToNothing();
        QTRY_VERIFY(forget->property("visible").toBool());
        QVERIFY(!error->property("visible").toBool());
        QCOMPARE(keys.keys.value(syncNpub()), QByteArray(32, '\1'));

        QVERIFY(QMetaObject::invokeMethod(forget, "clicked"));
        QTRY_VERIFY(!backend.signingKeyStored());
        QVERIFY(keys.keys.isEmpty());
        QTRY_VERIFY(field->property("visible").toBool());

        // Typed and never submitted, it goes with the dialog.
        type("nsec1typed");
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_COMPARE(field->property("text").toString(), QString());
        undoneToNothing();
        QVERIFY(keys.keys.isEmpty());
    }

    // "Stop syncing" clears the account either way, and removes its key
    // from the keyring only when asked to, saying so in the status bar.
    void backendStopsSyncingWithOrWithoutTheKey()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const QString npub = syncNpub(), shortNpub = Nip19::shortNpub(npub, npub);
        FakeKeyStore keys;
        // A fresh window over a library syncing with the test key, whose
        // key the keyring holds.
        const auto loggedIn = [&] {
            seedSyncAccount();
            keys.error = KeyStore::Error::None;
            keys.keys.insert(npub, QByteArray(32, '\1'));
            auto backend = std::make_unique<Backend>();
            backend->setKeyStore(&keys);
            return backend;
        };

        {
            auto backend = loggedIn();
            QTRY_VERIFY(backend->signingKeyStored());
            backend->stopSync(false);
            QTRY_VERIFY(backend->syncNpub().isEmpty());
            QCOMPARE(backend->status(), "Stopped syncing with " + shortNpub);
            QVERIFY(!backend->statusError());
            QVERIFY(!backend->signingKeyStored());
            QTest::qWait(20);
            QCOMPARE(keys.keys.value(npub), QByteArray(32, '\1'));
            QVERIFY(!keys.calls.contains("remove " + npub));
        }
        {
            auto backend = loggedIn();
            QTRY_VERIFY(backend->signingKeyStored());
            backend->stopSync(true);
            QTRY_VERIFY(keys.keys.isEmpty());
            QVERIFY(backend->syncNpub().isEmpty());
            QTRY_COMPARE(backend->status(), "Stopped syncing with " + shortNpub + " and removed its signing key");
            QVERIFY(!backend->statusError());
            QCOMPARE(keys.calls.last(), "remove " + npub);
        }
        // A removal that fails still clears the account, and says why the
        // key is still there.
        for (const auto &[error, reason] :
             {std::pair(KeyStore::Error::Dismissed, QString("The keyring prompt was dismissed.")),
              std::pair(KeyStore::Error::Unavailable, QString("No Secret Service is running"))}) {
            auto backend = loggedIn();
            QTRY_VERIFY(backend->signingKeyStored());
            keys.error = error;
            backend->stopSync(true);
            QTRY_VERIFY(backend->syncNpub().isEmpty());
            const QString expected =
                "Stopped syncing with " + shortNpub + ", but its signing key could not be removed: " + reason;
            QTRY_VERIFY2(backend->status().startsWith(expected), qPrintable(backend->status()));
            QVERIFY(backend->statusError());
            QCOMPARE(keys.keys.value(npub), QByteArray(32, '\1'));
            Store store;
            QVERIFY(store.open());
            QVERIFY(store.syncAccount().npub.isEmpty());
        }
    }

    // With a key stored, "Stop syncing" asks in the dialog whether to remove
    // it too, "Remove key" being the default that Enter takes; without one
    // it just stops.
    void syncDialogAsksWhetherStopRemovesTheKey()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const QString npub = syncNpub();
        FakeKeyStore keys;
        // How the user answers, given the window and the dialog's parts.
        using Answer = std::function<void(QQuickWindow *, QObject *keep, QObject *remove)>;
        // With `checkFails`, the keyring cannot say whether it holds a key.
        const auto stopWith = [&](bool keyStored, const Answer &answer, bool checkFails = false) {
            seedSyncAccount();
            keys.keys.clear();
            if (keyStored)
                keys.keys.insert(npub, QByteArray(32, '\1'));
            keys.error = checkFails ? KeyStore::Error::Failed : KeyStore::Error::None;
            const bool asks = keyStored || checkFails;
            Backend backend;
            backend.setKeyStore(&keys);
            QQmlEngine engine;
            engine.rootContext()->setContextProperty("backend", &backend);
            engine.rootContext()->setContextProperty(
                "theme", QVariantMap{{"radius", 0}, {"background", "#000000"}, {"foreground", "#ffffff"}});
            engine.rootContext()->setContextProperty("translator", QVariantMap{{"language", "en"}});
            QQmlComponent component(&engine);
            component.setData("import QtQuick\nimport QtQuick.Controls\n"
                              "ApplicationWindow { width: 800; height: 600; visible: true\n"
                              " property alias dialog: dialog\n SyncDialog { id: dialog } }\n",
                              QUrl("qrc:/probe.qml"));
            std::unique_ptr<QObject> root(component.create());
            QVERIFY2(root, qPrintable(component.errorString()));
            auto *window = qobject_cast<QQuickWindow *>(root.get());
            window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
            QObject *dialog = root->property("dialog").value<QObject *>();
            QObject *stop = root->findChild<QObject *>("stopSyncButton");
            QObject *keep = root->findChild<QObject *>("keepKeyButton");
            QObject *remove = root->findChild<QObject *>("removeKeyButton");
            QObject *confirmation = root->findChild<QObject *>("stopSyncConfirmation");
            QVERIFY(stop && keep && remove && confirmation);
            QTRY_VERIFY(!backend.signingBusy());
            QCOMPARE(backend.signingKeyStored(), keyStored && !checkFails);
            QCOMPARE(backend.signingKeyChecked(), !checkFails);
            QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
            QTRY_VERIFY(dialog->property("opened").toBool());
            QTRY_VERIFY(!backend.signingBusy());
            QVERIFY(stop->property("visible").toBool());
            QVERIFY(!keep->property("visible").toBool());
            QVERIFY(QMetaObject::invokeMethod(stop, "clicked"));
            if (asks) {
                QVERIFY(confirmation->property("visible").toBool());
                QVERIFY(!stop->property("visible").toBool());
                QVERIFY(keep->property("visible").toBool());
                QVERIFY(remove->property("visible").toBool());
                QVERIFY(remove->property("highlighted").toBool());
                QTRY_VERIFY(remove->property("activeFocus").toBool());
                // Nothing changes until the user answers.
                const qsizetype removals = keys.calls.count("remove " + npub);
                QTest::qWait(20);
                QCOMPARE(backend.syncNpub(), npub);
                QCOMPARE(keys.calls.count("remove " + npub), removals);
                answer(window, keep, remove);
            } else {
                QVERIFY(!confirmation->property("visible").toBool());
            }
            QTRY_VERIFY(!dialog->property("visible").toBool());
            QTRY_VERIFY(backend.syncNpub().isEmpty());
            QTest::qWait(20);
            keys.error = KeyStore::Error::None;
        };

        stopWith(true, [](QQuickWindow *, QObject *keep, QObject *) {
            QVERIFY(QMetaObject::invokeMethod(keep, "clicked"));
        });
        QCOMPARE(keys.keys.value(npub), QByteArray(32, '\1'));
        QVERIFY(!keys.calls.contains("remove " + npub));

        stopWith(true, [](QQuickWindow *window, QObject *, QObject *) { QTest::keyClick(window, Qt::Key_Return); });
        QVERIFY(keys.keys.isEmpty());
        QCOMPARE(keys.calls.count("remove " + npub), 1);

        stopWith(true, [](QQuickWindow *, QObject *, QObject *remove) {
            QVERIFY(QMetaObject::invokeMethod(remove, "clicked"));
        });
        QVERIFY(keys.keys.isEmpty());
        QCOMPARE(keys.calls.count("remove " + npub), 2);

        // Tab reaches "Keep key", and Enter then takes it.
        stopWith(true, [](QQuickWindow *window, QObject *keep, QObject *) {
            QTest::keyClick(window, Qt::Key_Backtab);
            QTRY_VERIFY(keep->property("activeFocus").toBool());
            QTest::keyClick(window, Qt::Key_Return);
        });
        QCOMPARE(keys.keys.value(npub), QByteArray(32, '\1'));
        QCOMPARE(keys.calls.count("remove " + npub), 2);

        stopWith(false, {});
        QCOMPARE(keys.calls.count("remove " + npub), 2);

        // A keyring that could not be checked may hold the key, so it asks.
        stopWith(false, [](QQuickWindow *, QObject *keep, QObject *) {
            QVERIFY(QMetaObject::invokeMethod(keep, "clicked"));
        }, true);
        QCOMPARE(keys.calls.count("remove " + npub), 2);
    }

    // A key forgotten while a read of it waits on an unlock prompt stays
    // forgotten: the read's answer, when it comes, brings no signer back.
    void backendForgetDuringALookupKeepsTheKeyForgotten()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub(), shortNpub = Nip19::shortNpub(npub, npub);
        FakeKeyStore keys;
        keys.keys.insert(npub, QByteArray(32, '\1'));
        Backend backend;
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QTRY_VERIFY(!backend.signingBusy());
        QStringList errors;
        const auto keep = [&](const NostrEvent &, const QString &error) { errors << error; };

        keys.hold = true;
        backend.sign(signingDraft(), keep);
        QCOMPARE(keys.calls.last(), "lookup " + npub);
        backend.forgetSigningKey();
        // The removal waits its turn behind the read.
        QCOMPARE(keys.held(), 1);
        keys.hold = false;
        keys.release();
        QTRY_VERIFY(!backend.signingBusy());
        QVERIFY(keys.keys.isEmpty());
        QVERIFY(!backend.signingKeyStored());
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(errors.first(), QString("The signing key was forgotten before the event was signed"));

        backend.sign(signingDraft(), keep);
        QTRY_COMPARE(errors.size(), 2);
        QCOMPARE(errors.last(), "No signing key is stored for " + shortNpub);
        QCOMPARE(keys.calls.count("lookup " + npub), 1);
    }

    // A check that failed says nothing about the key, so a sign still reads
    // it: failing with the keyring's error while that fails, and signing
    // once the keyring answers.
    void backendSignsAfterTheLaunchCheckFailed()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        FakeKeyStore keys;
        keys.keys.insert(npub, QByteArray(32, '\1'));
        keys.error = KeyStore::Error::Locked;
        Backend backend;
        backend.setKeyStore(&keys);
        QTRY_VERIFY(!backend.signingBusy());
        QVERIFY(!backend.signingKeyStored());
        QVERIFY(!backend.signingKeyChecked());
        const QString locked = backend.signingError();
        QCOMPARE(locked, QString("The keyring is locked. Unlock it and try again."));
        std::optional<std::pair<NostrEvent, QString>> answer;
        const auto keep = [&](const NostrEvent &event, const QString &error) { answer.emplace(event, error); };

        backend.sign(signingDraft(), keep);
        QTRY_VERIFY(answer);
        QCOMPARE(answer->second, locked);
        QCOMPARE(keys.calls.count("lookup " + npub), 1);
        QVERIFY(!backend.signingKeyChecked());

        keys.error = KeyStore::Error::None;
        answer.reset();
        backend.sign(signingDraft(), keep);
        QTRY_VERIFY(answer);
        QVERIFY2(answer->second.isEmpty(), qPrintable(answer->second));
        QCOMPARE(answer->first.pubkey, syncPubkey());
        QVERIFY(answer->first.verify());
        QCOMPARE(keys.calls.count("lookup " + npub), 2);
        QVERIFY(backend.signingKeyStored());
        QVERIFY(backend.signingKeyChecked());
        QVERIFY(backend.signingError().isEmpty());
    }

    // A sign handed to the signer is answered even when the signer goes
    // before it answers, whatever drops it, and answered only once.
    void backendAnswersASignTheSignerDropped()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        FakeKeyStore keys;
        keys.keys.insert(npub, QByteArray(32, '\1'));
        Backend backend;
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QList<QStringList> answers(4);
        const auto keepAs = [&](int sign) {
            return [&answers, sign](const NostrEvent &event, const QString &error) {
                answers[sign] << (error.isEmpty() && event.verify() ? QString("signed") : error);
            };
        };
        backend.sign(signingDraft(), keepAs(0));
        QTRY_COMPARE(answers[0], QStringList{"signed"});

        backend.sign(signingDraft(), keepAs(1));
        backend.forgetSigningKey();
        QTRY_VERIFY(!backend.signingKeyStored());
        QVERIFY(backend.addSigningKey(QByteArray(32, '\1').toHex()));
        QTRY_VERIFY(backend.signingKeyStored());
        backend.sign(signingDraft(), keepAs(2));
        QTRY_COMPARE(answers[2], QStringList{"signed"});
        backend.sign(signingDraft(), keepAs(3));
        backend.setKeyStore(&keys);
        QTest::qWait(50);
        QCOMPARE(answers[0], QStringList{"signed"});
        QCOMPARE(answers[1], QStringList{"The signing key was forgotten before the event was signed"});
        QCOMPARE(answers[2], QStringList{"signed"});
        QCOMPARE(answers[3], QStringList{"The sync account changed before the event was signed"});
    }

    // Every sign gets exactly one answer however keyring calls, account
    // switches, and signs interleave, and none is signed by another key.
    // The seed fixes the order of the calls; the keyring answers in 0 to 2
    // ms drawn from it, so they overlap.
    void backendAnswersEverySignOnceWhateverTheInterleaving()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME", "READSTR_RELAYS"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        qputenv("READSTR_RELAYS", relay.url().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        const auto other = LocalSigner::create(QByteArray(32, '\3'));
        const QString otherNpub = Nip19::npub(other->pubkey());
        const quint32 seed = 20261122;
        QRandomGenerator random(seed);
        FakeKeyStore keys;
        keys.delays = &random;
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setKeyStore(&keys);
        QHash<int, int> answers;
        int issued = 0, answered = 0, foreign = 0, signedCount = 0;
        for (int step = 0; step < 3000; ++step) {
            // The key of whichever account is synced, so that it is stored
            // and read often enough for signers to come and go.
            const QByteArray own(32, backend.syncNpub() == otherNpub ? '\3' : '\1');
            switch (random.bounded(14)) {
            case 0:
            case 1:
                backend.addSigningKey(own.toHex());
                break;
            case 2:
                backend.addSigningKey(QByteArray(32, '\4').toHex());
                break;
            case 3:
                backend.forgetSigningKey();
                break;
            case 4:
                backend.checkSigningKey();
                break;
            case 5:
                if (random.bounded(8) == 0)
                    backend.stopSync(random.bounded(2));
                else if (random.bounded(4) == 0)
                    backend.setSyncNpub(random.bounded(2) ? npub : otherNpub);
                break;
            default: {
                NostrEvent draft = signingDraft();
                // Some out of range, which the signer refuses.
                if (random.bounded(5) == 0)
                    draft.kind = -5;
                const int sign = issued++;
                // The account synced when it is asked for, which is the only
                // one that may sign it.
                const QString expected = backend.syncNpub() == otherNpub ? other->pubkey() : syncPubkey();
                backend.sign(draft, [&, sign, expected](const NostrEvent &event, const QString &error) {
                    ++answered;
                    ++answers[sign];
                    if (error.isEmpty())
                        ++signedCount;
                    if (error.isEmpty() && (!event.verify() || event.pubkey != expected))
                        ++foreign;
                });
            }
            }
            // Now and then long enough for the keyring's delayed answers to
            // come, so calls finish between steps as well as pile up.
            if (random.bounded(3) == 0)
                QTest::qWait(random.bounded(3));
        }
        QTRY_COMPARE_WITH_TIMEOUT(answered, issued, 20000);
        QTest::qWait(50);
        QCOMPARE(answered, issued);
        QCOMPARE(answers.size(), issued);
        for (auto count = answers.cbegin(); count != answers.cend(); ++count)
            QVERIFY2(count.value() == 1, qPrintable(QString("sign %1 answered %2 times, seed %3")
                                                        .arg(count.key())
                                                        .arg(count.value())
                                                        .arg(seed)));
        QCOMPARE(foreign, 0);
        // Some were signed, so signers did come and go along the way.
        QVERIFY(signedCount > 0);
        // And what the dialog shows agrees with the keyring.
        backend.checkSigningKey();
        QTRY_VERIFY(!backend.signingBusy());
        QCOMPARE(backend.signingKeyStored(), keys.keys.contains(backend.syncNpub()));
    }

    // Reopening the dialog asks the keyring again; asked while a store or a
    // removal is out, the answer is about the keyring after it, so what the
    // dialog shows is the write's outcome, not the state before it.
    void backendChecksTheKeyAfterAWriteStillOut()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        FakeKeyStore keys;
        Backend backend;
        backend.setKeyStore(&keys);
        QTRY_VERIFY(!backend.signingBusy());
        QVERIFY(!backend.signingKeyStored());

        keys.hold = true;
        QVERIFY(backend.addSigningKey(QByteArray(32, '\1').toHex()));
        backend.checkSigningKey();
        QVERIFY(backend.signingBusy());
        // Not even asked until the store has answered.
        QCOMPARE(keys.held(), 1);
        // The newest call answers first, as with a store waiting on a prompt.
        while (keys.held() > 0)
            keys.releaseNewest();
        QVERIFY(!backend.signingBusy());
        QVERIFY(backend.signingKeyStored());
        QVERIFY(keys.keys.contains(npub));
        QCOMPARE(keys.calls.mid(1), QStringList({"store " + npub, "contains " + npub}));

        backend.forgetSigningKey();
        backend.checkSigningKey();
        while (keys.held() > 0)
            keys.releaseNewest();
        QVERIFY(!backend.signingBusy());
        QVERIFY(!backend.signingKeyStored());
        QVERIFY(keys.keys.isEmpty());
    }

    // A key added again while "Stop syncing" is still removing the old one
    // is stored after that removal, never deleted by it, however the
    // keyring orders its answers.
    void backendStoresAKeyAddedWhileTheStopRemovalIsOut()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME", "READSTR_RELAYS"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        qputenv("READSTR_RELAYS", relay.url().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        FakeKeyStore keys;
        keys.keys.insert(npub, QByteArray(32, '\1'));
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QTRY_VERIFY(!backend.signingBusy());

        keys.hold = true;
        backend.stopSync(true);
        QTRY_VERIFY(backend.syncNpub().isEmpty());
        QVERIFY(backend.setSyncNpub(npub));
        QTRY_COMPARE(backend.syncNpub(), npub);
        QVERIFY(backend.addSigningKey(QByteArray(32, '\1').toHex()));
        QCOMPARE(keys.held(), 1);
        while (keys.held() > 0)
            keys.releaseNewest();
        QVERIFY(!backend.signingBusy());
        QCOMPARE(keys.keys.value(npub), QByteArray(32, '\1'));
        QVERIFY(backend.signingKeyStored());
        QCOMPARE(keys.calls.mid(1), QStringList({"remove " + npub, "contains " + npub, "store " + npub}));
    }

    // Signing right after launch, before the keyring has said whether it
    // holds the key, waits for that answer rather than failing.
    void backendSignsWhileTheLaunchCheckIsOut()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        FakeKeyStore keys;
        keys.keys.insert(npub, QByteArray(32, '\1'));
        keys.hold = true;
        Backend backend;
        backend.setKeyStore(&keys);
        QVERIFY(!backend.signingKeyStored());
        std::optional<std::pair<NostrEvent, QString>> result;
        backend.sign(signingDraft(), [&](const NostrEvent &event, const QString &error) { result.emplace(event, error); });
        QTest::qWait(20);
        QVERIFY(!result);
        QCOMPARE(keys.calls, QStringList{"contains " + npub});
        keys.hold = false;
        keys.release();
        QTRY_VERIFY(result);
        QVERIFY2(result->second.isEmpty(), qPrintable(result->second));
        QCOMPARE(result->first.pubkey, syncPubkey());
        QVERIFY(result->first.verify());
        QCOMPARE(keys.calls, QStringList({"contains " + npub, "lookup " + npub}));
        QVERIFY(!backend.signingBusy());

        // Without a key, the same wait ends in saying so.
        keys.keys.clear();
        keys.hold = true;
        backend.checkSigningKey();
        QString error;
        backend.sign(signingDraft(), [&](const NostrEvent &, const QString &answer) { error = answer; });
        keys.hold = false;
        keys.release();
        QTRY_VERIFY(!error.isEmpty());
        QCOMPARE(error, "No signing key is stored for " + Nip19::shortNpub(npub, npub));
    }

    // The launch check answering "no key" must not fail a sign whose load
    // waits behind a key added after the check started.
    void backendSignsWithAKeyAddedWhileTheLaunchCheckIsOut()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        FakeKeyStore keys;
        keys.hold = true;
        Backend backend;
        backend.setKeyStore(&keys);
        QVERIFY(backend.addSigningKey(QStringLiteral("01").repeated(32)));
        std::optional<std::pair<NostrEvent, QString>> result;
        backend.sign(signingDraft(), [&](const NostrEvent &event, const QString &error) { result.emplace(event, error); });
        keys.hold = false;
        keys.release();
        QTRY_VERIFY(result);
        QVERIFY2(result->second.isEmpty(), qPrintable(result->second));
        QCOMPARE(result->first.pubkey, syncPubkey());
        QVERIFY(backend.signingKeyStored());
    }

    // A keyring that cannot answer says nothing about the key, so a signer
    // already loaded keeps signing; a read that fails shows in the dialog.
    void backendKeepsItsSignerWhenACheckFails()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        FakeKeyStore keys;
        keys.keys.insert(npub, QByteArray(32, '\1'));
        Backend backend;
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QString error = "unanswered";
        backend.sign(signingDraft(), [&](const NostrEvent &, const QString &answer) { error = answer; });
        QTRY_VERIFY(error != "unanswered");
        QVERIFY2(error.isEmpty(), qPrintable(error));

        keys.error = KeyStore::Error::Locked;
        backend.checkSigningKey();
        QTRY_VERIFY(!backend.signingBusy());
        error = "unanswered";
        backend.sign(signingDraft(), [&](const NostrEvent &, const QString &answer) { error = answer; });
        QTRY_VERIFY(error != "unanswered");
        QVERIFY2(error.isEmpty(), qPrintable(error));

        // A fresh account load whose read fails says why in the dialog.
        Backend other;
        keys.error = KeyStore::Error::None;
        other.setKeyStore(&keys);
        QTRY_VERIFY(other.signingKeyStored());
        keys.error = KeyStore::Error::Failed;
        error = "unanswered";
        other.sign(signingDraft(), [&](const NostrEvent &, const QString &answer) { error = answer; });
        QTRY_VERIFY(error != "unanswered");
        QVERIFY(!error.isEmpty());
        QVERIFY(!other.signingError().isEmpty());
    }

    // A read that finds no key says so without clearing the busy flag of a
    // call still queued behind it.
    void backendStaysBusyWhileACallIsQueued()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub();
        FakeKeyStore keys;
        keys.keys.insert(npub, QByteArray(32, '\1'));
        Backend backend;
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QTRY_VERIFY(!backend.signingBusy());
        // Gone from the keyring behind Readstr's back.
        keys.keys.clear();
        keys.hold = true;
        QString error;
        backend.sign(signingDraft(), [&](const NostrEvent &, const QString &answer) { error = answer; });
        backend.checkSigningKey();
        keys.release();
        QVERIFY(!backend.signingKeyStored());
        QVERIFY(backend.signingBusy());
        QCOMPARE(keys.held(), 1);
        keys.release();
        // The check of the pairing comes after the key's.
        QVERIFY(backend.signingBusy());
        QCOMPARE(keys.held(), 1);
        QCOMPARE(keys.pairingCalls.last(), "contains " + npub);
        keys.release();
        QVERIFY(!backend.signingBusy());
        QTRY_COMPARE(error, "No signing key is stored for " + Nip19::shortNpub(npub, npub));
    }

    // A keyring that loses its prompt fails the read, and the signs waiting
    // on it are answered with why, rather than waiting forever.
    void backendFailsSignsWhenTheKeyringGoesAway()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        const QString npub = syncNpub(), item = "/org/freedesktop/secrets/collection/login/7";
        const QVariant noItems = QVariant::fromValue(QList<QDBusObjectPath>());
        const QVariant lockedItem = QVariant::fromValue(QList<QDBusObjectPath>{QDBusObjectPath(item)});
        FakeSecretService service;
        Backend backend;
        backend.setKeyStore(&service);
        service.reply({noItems, lockedItem});
        QVERIFY(backend.signingKeyStored());
        // No pairing with a remote signer.
        service.reply({noItems, noItems});
        QVERIFY(!backend.remoteSignerPaired());
        QStringList errors;
        const auto keep = [&](const NostrEvent &, const QString &error) { errors << error; };
        backend.sign(signingDraft(), keep);
        backend.sign(signingDraft(), keep);
        service.reply({noItems, lockedItem});
        service.reply({noItems, QVariant::fromValue(QDBusObjectPath("/org/freedesktop/secrets/prompt/u1"))});
        service.reply({});
        QVERIFY(backend.signingBusy());
        service.serviceLost();
        QVERIFY(!backend.signingBusy());
        QTRY_COMPARE(errors.size(), 2);
        const QString lost = "The keyring failed: The keyring went away before its prompt was answered.";
        QCOMPARE(errors, QStringList({lost, lost}));
        QVERIFY(service.listening.isEmpty());
    }

    // Markdown

    void markdownRendersTheAllowedSubset()
    {
        const QString html = Markdown::toRichText(
            "# Title\n\n*em* **strong** `code` ~~gone~~ [link](https://example.com/a)\n\n> quoted\n\n- one\n- two\n\n"
            "| a | b |\n|---|---|\n| 1 | 2 |\n\n```\nblock\n```\n\n<script>alert(1)</script> <b>raw</b>\n\n"
            "[js](javascript:alert(1)) [rel](/relative) [mail](mailto:a@b.c)");
        for (const char *expected : {"<h1>", "<em>em</em>", "<strong>strong</strong>", "<code>code</code>", "<s>gone</s>",
                                     "<a href=\"https://example.com/a\">link</a>", "<blockquote>quoted</blockquote>",
                                     "<ul>", "<li>one</li>", "<li>two</li>", "<table>", "<td>", "<pre><code>block",
                                     "&lt;script&gt;alert(1)&lt;/script&gt; &lt;b&gt;raw&lt;/b&gt;"})
            QVERIFY2(html.contains(expected), qPrintable(expected + QString(" in ") + html));
        for (const char *absent : {"<script", "<b>", "javascript", "/relative", "mailto", "style", "<span", "<img"})
            QVERIFY2(!html.contains(absent), qPrintable(absent + QString(" in ") + html));
        QVERIFY(html.contains("js ") && html.contains("rel ") && html.contains("mail"));
        QVERIFY(Markdown::toRichText("").isEmpty());
        QVERIFY(Markdown::toRichText(" \n\n ").isEmpty());
    }

    void markdownLinksNostrEntitiesToNjump()
    {
        const QString hex = "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d";
        const QString npub = Nip19::npub(hex);
        const QString note = "note14w46h2at4w46h2at4w46h2at4w46h2at4w46h2at4w46h2at4w4sfreljc";
        const QString nevent = Nip19::nevent(QString(64, 'a'), hex, 1);
        const QString naddr = Nip19::naddr(30023, hex, "post");
        const QString nprofile =
            "nprofile1qqsrhuxx8l9ex335q7he0f09aej04zpazpl0ne2cgukyawd24mayt8gpp4mhxue69uhhytnc9e3k7mgpz4mhxue69uhkg6nzv9e"
            "juumpv34kytnrdaksjlyr9p";
        for (const QString &entity : {npub, note, nevent, naddr, nprofile}) {
            const QString html = Markdown::toRichText("[who](nostr:" + entity + ")");
            QCOMPARE(html, "<p><a href=\"https://njump.me/" + entity + "\">who</a></p>");
            QCOMPARE(Markdown::nostrLinkTarget("NOSTR:" + entity.toUpper()), "https://njump.me/" + entity);
        }
        QCOMPARE(Markdown::toRichText("<nostr:" + npub + ">"),
                 "<p><a href=\"https://njump.me/" + npub + "\">nostr:" + npub + "</a></p>");
        // A broken checksum, a secret key, an unknown prefix, and a bare
        // mention in prose stay text.
        const QString broken = npub.left(npub.size() - 1) + (npub.endsWith('q') ? 'p' : 'q');
        const QString nsec = "nsec1qurswpc8qurswpc8qurswpc8qurswpc8qurswpc8qurswpc8qursl6edet";
        for (const QString &href : QStringList{"nostr:" + broken, "nostr:" + nsec, "nostr:nfoo1qqqqqqqqqqqq", "nostr:",
                                               "nostr:" + npub + "?x=1", "nostr:" + QString(3000, 'q')}) {
            const QString html = Markdown::toRichText("[who](" + href + ")");
            QVERIFY2(!html.contains("<a"), qPrintable(html));
            QVERIFY(html.contains("who"));
            QVERIFY(Markdown::nostrLinkTarget(href).isEmpty());
        }
        const QString prose = Markdown::toRichText("Ask nostr:" + npub + " about it");
        QVERIFY2(!prose.contains("<a") && prose.contains("nostr:" + npub.left(40)), qPrintable(prose));
    }

    void markdownNeverLoadsImages()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int connections = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                ++connections;
                socket->deleteLater();
            }
        });
        const QString base = QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort());
        const QString markdown = "![a cat](" + base + "cat.png \"Title\")\n\n![](" + base + "bare.png)\n\n[![inner]("
                                 + base + "inner.png)](https://example.com/)\n\n<img src=\"" + base + "raw.png\">";
        const QString html = Markdown::toRichText(markdown);
        QVERIFY2(!html.contains("<img"), qPrintable(html));
        QVERIFY2(html.contains("<a href=\"" + base + "cat.png\">[image: a cat]</a>"), qPrintable(html));
        // Qt leaves out an image without a description, and an image inside a
        // link loses the link's address for its own.
        QVERIFY2(!html.contains("bare.png"), qPrintable(html));
        QVERIFY(html.contains("<a href=\"" + base + "inner.png\">[image: inner]</a>"));
        // Laid out as the reader would, and given time to ask: nothing does.
        QTextDocument view;
        view.setHtml(html);
        view.setTextWidth(600);
        QVERIFY(view.size().height() > 0);
        QTest::qWait(300);
        QCOMPARE(connections, 0);
    }

    void markdownStaysFastOnHostileInput_data()
    {
        QTest::addColumn<QString>("markdown");
        QTest::addColumn<bool>("plain");
        const int n = Markdown::maxInputLength;
        QTest::newRow("nested quotes") << QString(100000, '>') + " deep" << false;
        QTest::newRow("nested lists") << QString("- ").repeated(50000) + "x" << true;
        QTest::newRow("nested quote lists") << QString("> - ").repeated(30000) + "x" << true;
        QString indented;
        for (int i = 0; indented.size() < n; ++i)
            indented += QString(2 * i, ' ') + "- x\n";
        QTest::newRow("indented lists") << indented << false;
        QTest::newRow("bullets") << QString("- x\n").repeated(30000) << true;
        QTest::newRow("loose bullets") << QString("1. x\n\n").repeated(20000) << true;
        QTest::newRow("brackets") << QString(100000, '[') << false;
        QTest::newRow("link openers") << QString("[a](").repeated(30000) << false;
        QString table = "| a | b |\n|---|---|\n";
        while (table.size() < n)
            table += "| 1 | 2 |\n";
        QTest::newRow("huge table") << table << true;
        QTest::newRow("single-cell rows") << "a|b\n-|-\n" + QString("1\n").repeated(n / 2) << true;
        QTest::newRow("wide table") << "|" + QString("a|").repeated(8000) + "\n|" + QString("-|").repeated(8000) << true;
        QString capped = "|" + QString("a|").repeated(10) + "\n|" + QString("-|").repeated(10) + "\n";
        for (int i = 0; i < Markdown::maxTableCells / 10 - 2; ++i)
            capped += "|" + QString("1|").repeated(10) + "\n";
        QTest::newRow("table at the caps") << capped << false;
        // md4c takes form feeds and vertical tabs between delimiter cells.
        QTest::newRow("form-feed delimiter") << "|a|b|\n|---\f|---|\n" + QString("|x|y|\n").repeated(8000) << true;
        QTest::newRow("vertical-tab delimiter") << "|a|b|\n|---|\v---|\n" + QString("|x|y|\n").repeated(8000) << true;
        // A quote ends a table without a blank line, and starts a wider one.
        const QString narrow = "|a|\n|-|\n|1|\n";
        QTest::newRow("quoted wide table") << narrow + "> |" + QString("b|").repeated(65) + "\n> |"
                + QString("-|").repeated(65) + "\n> |" + QString("x|").repeated(65) + "\n" << true;
        QTest::newRow("quoted long table") << narrow + "> |" + QString("b|").repeated(64) + "\n> |"
                + QString("-|").repeated(64) + "\n" + ("> |" + QString("x|").repeated(64) + "\n").repeated(400) << true;
        QTest::newRow("quoted small table") << narrow + "> |b|c|\n> |-|-|\n> |x|y|\n" << false;
        QTest::newRow("bullets at the cap") << QString("- x\n").repeated(Markdown::maxListItems) << false;
        QTest::newRow("long line") << QString(n, 'x') << false;
        QTest::newRow("long lines") << (QString(20000, 'y') + "\n").repeated(n / 20001) << false;
        QTest::newRow("headings") << QString("# h\n").repeated(n / 4) << false;
        QTest::newRow("hard breaks") << QString("a\\\n").repeated(n / 3) << false;
        QTest::newRow("emphasis") << QString("*_").repeated(n / 2) << false;
        QTest::newRow("backticks") << QString(n, '`') << false;
        // md4c ends a line at a lone carriage return too.
        QTest::newRow("CR bullets") << QString("- a\r").repeated(8000) << true;
        QTest::newRow("CR bullets in full") << QString("- a\r").repeated(32767) << true;
        QString crTable = "| a | b |\r|---|---|\r";
        while (crTable.size() < n)
            crTable += "| 1 | 2 |\r";
        QTest::newRow("CR table") << crTable << true;
        QTest::newRow("CRLF table") << QString(crTable).replace("\r", "\r\n") << true;
        QTest::newRow("oversized") << QString("word word word\n\n").repeated(4 * n / 16) << false;
    }

    void markdownStaysFastOnHostileInput()
    {
        QFETCH(QString, markdown);
        QFETCH(bool, plain);
        QCOMPARE(Markdown::withinCaps(markdown.left(Markdown::maxInputLength)), !plain);
        QElapsedTimer timer;
        timer.start();
        const QString html = Markdown::toRichText(markdown);
        const qint64 converted = timer.elapsed();
        // Laid out as the reader lays it out, on this (the GUI) thread.
        QTextDocument view;
        view.setHtml(html);
        view.setTextWidth(700);
        view.size();
        QVERIFY2(timer.elapsed() < 2000,
                 qPrintable(QString("%1 ms to convert, %2 ms in all").arg(converted).arg(timer.elapsed())));
        QVERIFY(html.size() <= Sanitizer::maxOutputLength + 8192);
        QVERIFY(!html.contains("<img"));
        if (markdown.size() > Markdown::maxInputLength)
            QVERIFY(html.endsWith(QString("<p>") + QChar(0x2026) + "</p>"));
    }

    void markdownReadsEveryLineEnding()
    {
        // A table and a list read the same whichever line endings they have,
        // and a CRLF is one line ending, not a blank line between two.
        const QString lf = "| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\n- one\n- two\n";
        const QString html = Markdown::toRichText(lf);
        QVERIFY2(html.contains("<table>") && html.contains("<li>two</li>"), qPrintable(html));
        QCOMPARE(Markdown::toRichText(QString(lf).replace("\n", "\r\n")), html);
        QCOMPARE(Markdown::toRichText(QString(lf).replace("\n", "\r")), html);
        QCOMPARE(Markdown::normalizedLineEndings("a\r\nb\rc\n\r\nd\r\r"), QString("a\nb\nc\n\nd\n\n"));
        // Counted as md4c counts them: a CRLF table's rows count once each.
        QString rows = "a|b\r\n-|-\r\n";
        for (int i = 0; i < Markdown::maxTableRows - 2; ++i)
            rows += "1|2\r\n";
        QVERIFY(Markdown::withinCaps(rows));
        QVERIFY(!Markdown::withinCaps(rows + "1|2\r\n"));
        QVERIFY(!Markdown::withinCaps(QString(rows).replace("\r\n", "\r") + "1|2\r"));
    }

    void markdownDocumentsLoadNoResources()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("cat.png");
        QVERIFY(QImage(37, 23, QImage::Format_RGB32).save(path));
        const QUrl url = QUrl::fromLocalFile(path);
        const auto sizeOf = [](const QVariant &resource) {
            return resource.canConvert<QImage>() ? resource.value<QImage>().size() : QSize();
        };
        // A plain QTextDocument reads a local image it is asked for; the one
        // Markdown converts with never reads anything.
        QTextDocument plain;
        QCOMPARE(sizeOf(plain.resource(QTextDocument::ImageResource, url)), QSize(37, 23));
        Markdown::InertDocument inert;
        QVERIFY(inert.resource(QTextDocument::ImageResource, url).isNull());
        QVERIFY(inert.resource(QTextDocument::StyleSheetResource, url).isNull());
        // Asked to lay out, which has Qt's own image handler load a file by
        // its name when the document gives it nothing, it lays nothing out
        // and so reads nothing.
        inert.setMarkdown("![a cat](" + url.toString() + ")\n\n<img src=\"" + path + "\">");
        inert.setTextWidth(600);
        QVERIFY(inert.size().isEmpty());
        QVERIFY(dynamic_cast<Markdown::InertLayout *>(inert.documentLayout()));
        QVERIFY(inert.resource(QTextDocument::ImageResource, url).isNull());
        QVERIFY(inert.resource(QTextDocument::ImageResource, QUrl(path)).isNull());
        QTextDocument shown;
        shown.setMarkdown("![a cat](" + url.toString() + ")");
        shown.setTextWidth(600);
        QVERIFY(shown.size().height() > inert.size().height());
    }

    void sanitizerReadsQtStylesOnlyWhenAsked()
    {
        const QString html = "<p style=\" margin-left:40px; margin-right:40px;\"><span style=\" font-weight:700;"
                             " font-style:italic;\">x</span> <span style=\"font-family:'monospace'\">y</span>"
                             " <span style=\"text-decoration: line-through\">z</span>"
                             " <span style=\"color:#0000ff\">w</span></p><p style=\"margin-left:40px\">v</p>";
        Sanitizer::Options qt;
        qt.qtStyles = true;
        QCOMPARE(Sanitizer::toRichText(html, QUrl(), qt),
                 QString("<blockquote><strong><em>x</em></strong> <code>y</code> <s>z</s> w</blockquote><p>v</p>"));
        QCOMPARE(Sanitizer::toRichText(html, QUrl()), QString("<p>x y z w</p><p>v</p>"));
        // Spans past the nesting cap, and stray ends, stay balanced.
        const QString deep = QString("<span style=\"font-weight:700\">").repeated(200) + "t"
                             + QString("</span>").repeated(300);
        const QString out = Sanitizer::toRichText(deep, QUrl(), qt);
        QCOMPARE(out.count("<strong>"), out.count("</strong>"));
        QVERIFY(out.contains('t'));
        // A smaller input cap cuts there.
        Sanitizer::Options small;
        small.maxInputLength = 10;
        QCOMPARE(Sanitizer::toRichText("<p>0123456789abc</p>", QUrl(), small),
                 QString("<p>0123456</p><p>") + QChar(0x2026) + "</p>");
    }

    // NIP-71 video

    void nip19EncodesNeventAndChecksEntities()
    {
        const QString hex = "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d";
        // What nostr-tools' neventEncode({id, author, kind}) gives.
        QCOMPARE(Nip19::nevent(QString(32, 'a').replace('a', "ab"), hex, 21),
                 QString("nevent1qvzqqqqqz5pzqwlsccluhy6xxsr6l9a9uhhxf75g85g8a709tprjcn4e42h053vaqqs2h2at4w46h2at4w46h"
                         "2at4w46h2at4w46h2at4w46h2at4w46h2cgm8zk2"));
        QVERIFY(Nip19::nevent("not hex", hex, 21).isEmpty());
        QVERIFY(Nip19::nevent(QString(64, 'a'), "short", 21).isEmpty());
        QVERIFY(Nip19::nevent(QString(64, 'a'), hex, -1).isEmpty());

        const QString npub = Nip19::npub(hex);
        const QString note = "note14w46h2at4w46h2at4w46h2at4w46h2at4w46h2at4w46h2at4w4sfreljc";
        const QString nevent = Nip19::nevent(QString(64, 'b'), hex, 22);
        for (const QString &entity : {npub, note, nevent, Nip19::naddr(30023, hex, "x")}) {
            QCOMPARE(Nip19::entity(entity), entity);
            QCOMPARE(Nip19::entity(entity.toUpper()), entity);
        }
        QVERIFY(Nip19::entity(npub.left(5) + npub.mid(5).toUpper()).isEmpty());
        QVERIFY(Nip19::entity(npub.left(npub.size() - 1) + (npub.endsWith('q') ? 'p' : 'q')).isEmpty());
        // A secret key never becomes a link, nor does a key of the wrong size.
        QVERIFY(Nip19::entity("nsec1qurswpc8qurswpc8qurswpc8qurswpc8qurswpc8qurswpc8qursl6edet").isEmpty());
        QVERIFY(Nip19::entity("note1pyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpysrhujx").isEmpty());
        QVERIFY(Nip19::entity("npub1pyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysjzgfpyysj6qhpr4").isEmpty());
        QVERIFY(Nip19::entity("npub1" + QString(Nip19::maxLength, 'q')).isEmpty());
        QVERIFY(Nip19::entity("npub1bbbbbbb").isEmpty());
        QVERIFY(Nip19::entity("").isEmpty());
    }

    void nip19RefusesNonAsciiAndIncompleteEntities()
    {
        const QString hex = "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d";
        const QByteArray key = QByteArray::fromHex(hex.toLatin1());
        // The Kelvin sign lowercases to "k", so an uppercase entity with one
        // for its K would pass for the entity; nothing outside printable
        // ASCII does.
        QString withK;
        for (int kind = 1; withK.isEmpty() && kind < 200; ++kind)
            if (const QString nevent = Nip19::nevent(QString(64, 'a'), hex, kind); nevent.contains('k'))
                withK = nevent;
        QVERIFY(!withK.isEmpty());
        QCOMPARE(Nip19::entity(withK.toUpper()), withK);
        QVERIFY(Nip19::entity(withK.toUpper().replace('K', QChar(0x212A))).isEmpty());
        QVERIFY(Nip19::entity(withK + QChar(0x00A0)).isEmpty());
        QString npubWithK;
        for (char c = 'a'; npubWithK.isEmpty() && c <= 'f'; ++c)
            if (const QString npub = Nip19::npub(QString(64, c)); npub.contains('k'))
                npubWithK = npub;
        QVERIFY(!npubWithK.isEmpty());
        QVERIFY(Nip19::decodeProfile(npubWithK.toUpper()).has_value());
        QVERIFY(!Nip19::decodeProfile(npubWithK.toUpper().replace('K', QChar(0x212A))).has_value());

        // The encoder below makes what Nip19 does.
        const auto field = [](int type, const QByteArray &value) {
            return QByteArray(1, char(type)) + QByteArray(1, char(value.size())) + value;
        };
        const QByteArray kind = QByteArray::fromHex("00007597");
        QCOMPARE(bech32("naddr", field(0, "x") + field(2, key) + field(3, kind)), Nip19::naddr(30103, hex, "x"));

        // What nostr-tools reads, unknown types skipped, is accepted.
        const QString id = QString(64, 'b');
        const QByteArray idBytes = QByteArray::fromHex(id.toLatin1());
        for (const QString &entity :
             {Nip19::nevent(id, hex, 22), bech32("nevent", field(0, idBytes)), Nip19::naddr(30023, hex, ""),
              bech32("nevent", field(9, "?") + field(0, idBytes) + field(1, "wss://relay.example.com")),
              bech32("nprofile", field(0, key) + field(1, "wss://relay.example.com")),
              bech32("naddr", field(0, "post") + field(2, key) + field(3, kind) + field(7, "extra"))})
            QVERIFY2(Nip19::entity(entity) == entity, qPrintable(entity));
        // Without the fields it needs, of the wrong size, or cut short, it
        // is not.
        QVERIFY(Nip19::entity("nevent1c3esv7").isEmpty());
        for (const QString &entity :
             {bech32("nevent", {}), bech32("nevent", field(0, idBytes.left(31))),
              bech32("nevent", field(0, idBytes) + field(2, key.left(31))),
              bech32("nevent", field(0, idBytes) + field(3, kind.left(3))),
              bech32("nprofile", field(1, "wss://r.example")),
              bech32("nprofile", field(0, key + 'x')), bech32("nprofile", field(0, key) + field(0, key)),
              bech32("naddr", field(0, "post")),
              bech32("naddr", field(2, key) + field(3, kind)), bech32("naddr", field(0, "post") + field(2, key)),
              bech32("naddr", field(0, "post") + field(2, key) + field(3, kind + 'x')),
              bech32("naddr", QByteArray::fromHex("deadbeefcafe")), bech32("nevent", field(0, idBytes).chopped(1)),
              bech32("naddr", field(0, "post") + field(2, key) + field(3, kind) + QByteArray(1, '\x05'))})
            QVERIFY2(Nip19::entity(entity).isEmpty(), qPrintable(entity));
        // Subscribing refuses the same two-key nprofile a link does.
        QVERIFY(!Nip19::decodeProfile(bech32("nprofile", field(0, key) + field(0, key))));
        QVERIFY(Nip19::decodeProfile(bech32("nprofile", field(0, key) + field(1, "wss://relay.example.com"))));
    }

    void nostrSourceReadsVideos()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const QString cdn = "https://cdn.example.com/";
        QList<NostrEvent> events;
        for (const QJsonObject &json : {
                 // Chosen with its own image and duration; a future published_at.
                 signedEvent(21, "Line one\nline two\n\nNext <b>", {{"title", "Clip\u202e"},
                             {"published_at", QString::number(now + 7 * 86400)},
                             {"imeta", "url " + cdn + "a.mp4", "m video/mp4", "image " + cdn + "a.jpg",
                              "duration 61.5", "dim 1920x1080"}}, now - 100),
                 // A javascript: url and an ftp: url are skipped; an image
                 // from an imeta that plays nothing is the thumbnail; NaN is
                 // no duration.
                 signedEvent(22, "short", {{"published_at", QString::number(now - 5000)},
                             {"imeta", "url javascript:alert(1)", "m video/mp4", "image " + cdn + "first.jpg"},
                             {"imeta", "url ftp://cdn.example.com/b.mp4"},
                             {"imeta", "url " + cdn + "b.webm", "duration NaN", "image javascript:x"}}, now - 200),
                 // Not a video type, and an imeta without a url.
                 signedEvent(21, "", {{"imeta", "url " + cdn + "c.jpg", "m image/jpeg"},
                             {"imeta", "m video/mp4", "duration 3"}}, now - 300),
                 // No imeta; a later one wins over an earlier usable one only
                 // when the earlier does not play.
                 signedEvent(21, "", {{"title", "Bare"}}, now - 400),
                 signedEvent(21, "", {{"imeta", "url " + cdn + QString(9000, 'x') + ".mp4"},
                             {"imeta", "url " + cdn + "e1.mp4", "M VIDEO/MP4", "duration -3"},
                             {"imeta", "url " + cdn + "e2.mp4", "duration 9"}}, now - 500),
                 signedEvent(21, "", {{"imeta", "url " + cdn + "f.mp4", "duration inf", "m VIDEO/WebM"}}, now - 600),
                 signedEvent(30023, "# Long", {{"d", "a"}}, now - 50),
                 signedEvent(1, "a note", {}, now - 60)})
            events << *NostrEvent::fromJson(json);
        const QString pubkey = events.first().pubkey;

        const QList<ParsedItem> items = NostrSource::videos(events, "Ada");
        QCOMPARE(items.size(), 6);
        const ParsedItem &a = items.at(0);
        QCOMPARE(a.title, QString("Clip"));
        QCOMPARE(a.guid, events.at(0).id);
        QCOMPARE(a.published.toSecsSinceEpoch(), now - 100);
        QCOMPARE(a.mediaUrl, cdn + "a.mp4");
        QCOMPARE(a.thumbnailUrl, cdn + "a.jpg");
        QCOMPARE(a.duration, 61.5);
        QCOMPARE(a.author, QString("Ada"));
        QCOMPARE(a.content, QString("<p>Line one<br>line two</p><p>Next &lt;b&gt;</p>"));
        QCOMPARE(a.summary, QString("Line one line two Next <b>"));
        QCOMPARE(a.url, "https://njump.me/" + Nip19::nevent(events.at(0).id, pubkey, 21));
        QVERIFY(a.address.isEmpty());

        const ParsedItem &e = items.at(3);
        QCOMPARE(e.mediaUrl, cdn + "e1.mp4");
        QCOMPARE(e.duration, -1.0);
        QCOMPARE(items.at(4).mediaUrl, cdn + "f.mp4");
        QCOMPARE(items.at(4).duration, -1.0);

        const ParsedItem &b = items.at(5);
        QCOMPARE(b.title, QString("Untitled Video"));
        QCOMPARE(b.published.toSecsSinceEpoch(), now - 5000);
        QCOMPARE(b.mediaUrl, cdn + "b.webm");
        QCOMPARE(b.thumbnailUrl, cdn + "first.jpg");
        QCOMPARE(b.duration, -1.0);
        QCOMPARE(b.url, "https://njump.me/" + Nip19::nevent(events.at(1).id, pubkey, 22));

        const ParsedItem &c = items.at(1);
        QVERIFY(c.mediaUrl.isEmpty());
        QVERIFY(c.thumbnailUrl.isEmpty());
        QCOMPARE(c.duration, -1.0);
        QVERIFY(c.content.isEmpty());
        QCOMPARE(items.at(2).title, QString("Bare"));
        QVERIFY(items.at(2).mediaUrl.isEmpty());
        // The long-form reader leaves videos alone.
        QCOMPARE(NostrSource::longForm(events, "Ada").size(), 1);
    }

    void storeMigratesAVersionOneLibraryAndKeepsMedia()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/readstr.db";
        {
            // A library as phase 1 wrote it, schema version 1.
            QSqlDatabase raw = QSqlDatabase::addDatabase("QSQLITE", "v1");
            raw.setDatabaseName(path);
            QVERIFY(raw.open());
            QSqlQuery query(raw);
            const QStringList statements =
                versionOneSchema()
                << "INSERT INTO feeds (url, url_key, title, created_at) VALUES ('https://a.example.com/feed',"
                   " 'a.example.com/feed', 'A', 1)"
                << "INSERT INTO items (feed_id, guid, title, created_at, is_starred) VALUES (1, 'old', 'Old', 1, 1)"
                << "PRAGMA user_version = 1";
            for (const QString &statement : statements)
                QVERIFY2(query.exec(statement), qPrintable(query.lastError().text()));
            query = QSqlQuery();
            raw.close();
            raw = QSqlDatabase();
            QSqlDatabase::removeDatabase("v1");
        }
        Store store;
        QVERIFY2(store.open(path), qPrintable(store.lastError()));
        QCOMPARE(store.schemaVersion(), 6);
        QList<Article> articles = store.articles({});
        QCOMPARE(articles.size(), 1);
        QCOMPARE(articles.first().title, QString("Old"));
        QVERIFY(articles.first().starred);
        QVERIFY(articles.first().mediaUrl.isEmpty());
        QCOMPARE(articles.first().duration, -1.0);

        ParsedItem video;
        video.guid = "video";
        video.title = "Clip";
        video.mediaUrl = "https://cdn.example.com/a.mp4";
        video.thumbnailUrl = "https://cdn.example.com/a.jpg";
        video.duration = 12.5;
        QCOMPARE(store.saveItems(1, {video}), 1);
        Article saved = store.articles({}).first();
        QCOMPARE(saved.mediaUrl, video.mediaUrl);
        QCOMPARE(saved.thumbnailUrl, video.thumbnailUrl);
        QCOMPARE(saved.duration, 12.5);
        // A later save of the same event updates it in place.
        video.thumbnailUrl.clear();
        video.duration = -1;
        QCOMPARE(store.saveItems(1, {video}), 0);
        saved = *store.article(saved.id);
        QVERIFY(saved.thumbnailUrl.isEmpty());
        QCOMPARE(saved.duration, -1.0);
        QCOMPARE(saved.mediaUrl, video.mediaUrl);
    }

    void storeMigrationAsksNostrFeedsForEverythingAgain()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/readstr.db";
        {
            // A library as the long-form release wrote it, schema version 2,
            // with a Nostr feed that every relay answered.
            QSqlDatabase raw = QSqlDatabase::addDatabase("QSQLITE", "v2");
            raw.setDatabaseName(path);
            QVERIFY(raw.open());
            QSqlQuery query(raw);
            QStringList statements = versionOneSchema();
            statements << "ALTER TABLE items ADD COLUMN address TEXT"
                       << "ALTER TABLE items ADD COLUMN revision INTEGER"
                       << "CREATE INDEX items_by_address ON items(feed_id, address) WHERE address IS NOT NULL"
                       << "ALTER TABLE feeds ADD COLUMN relay_hints TEXT"
                       << "ALTER TABLE feeds ADD COLUMN profile_revision INTEGER"
                       << "ALTER TABLE feeds ADD COLUMN profile_event_id TEXT"
                       << "ALTER TABLE feeds ADD COLUMN profile_title TEXT"
                       << "ALTER TABLE feeds ADD COLUMN complete_at INTEGER"
                       << "INSERT INTO feeds (url, url_key, title, created_at, complete_at) VALUES ('"
                              + Nip19::npub(QString(64, 'a')) + "', 'nostr-a', 'Ada', 1, 1700000000)"
                       << "INSERT INTO feeds (url, url_key, title, created_at, complete_at) VALUES"
                          " ('https://a.example.com/feed', 'a.example.com/feed', 'A', 1, 1700000000)"
                       << "PRAGMA user_version = 2";
            for (const QString &statement : statements)
                QVERIFY2(query.exec(statement), qPrintable(query.lastError().text()));
            query = QSqlQuery();
            raw.close();
            raw = QSqlDatabase();
            QSqlDatabase::removeDatabase("v2");
        }
        Store store;
        QVERIFY2(store.open(path), qPrintable(store.lastError()));
        QCOMPARE(store.schemaVersion(), 6);
        const QList<Feed> feeds = store.feeds();
        QCOMPARE(feeds.size(), 2);
        for (const Feed &feed : feeds) {
            // The Nostr feed asks for everything again, its videos among
            // them; nothing else changes.
            QCOMPARE(feed.completeAt.isValid(), !Library::isNostrFeed(feed));
            if (Library::isNostrFeed(feed))
                for (const NostrQuery &query : Library::nostrQueries(feed))
                    QCOMPARE(query.since, 0);
        }
    }

    void nostrAuthorsBringVideosAndLongForm()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeRelay relay;
        // The thumbnail is on this machine, which the window never asks.
        const QJsonObject video = signedEvent(21, "Watch this", {{"title", "Clip"},
            {"imeta", "url https://cdn.example.com/a.mp4", "m video/mp4", "image http://127.0.0.1:1/a.jpg",
             "duration 3725"}}, now + 300);
        relay.events = {signedEvent(0, R"({"name":"ada"})", {}, now),
                        signedEvent(30023, "# Hi", {{"d", "a"}, {"title", "Hello"}, {"published_at", QString::number(now)}},
                                    now + 100),
                        signedEvent(22, "Short one", {{"imeta", "url https://cdn.example.com/b.mp4"}}, now + 200),
                        video, video};
        const QString pubkey = relay.events.first().toObject().value("pubkey").toString();
        const auto author = Nip19::decodeProfile(Nip19::npub(pubkey));

        RelayPool pool;
        pool.setRelays({relay.url()});
        const QList<NostrResult> found = queryAll(pool, Library::nostrQueries(*author));
        QVERIFY2(found.at(0).complete() && found.at(1).complete(), qPrintable(found.at(0).error + found.at(1).error));
        QCOMPARE(relay.filters.size(), 2);
        const QList<QJsonArray> kinds{relay.filters.at(0).value("kinds").toArray(),
                                      relay.filters.at(1).value("kinds").toArray()};
        QVERIFY(kinds.contains(QJsonArray({0, 30023})) && kinds.contains(QJsonArray({21, 22})));
        // Videos are regular events: the repeated one is kept once, by id.
        QCOMPARE(found.at(0).events.size(), 2);
        QCOMPARE(found.at(1).events.size(), 2);
        Store store;
        QVERIFY(store.open());
        const Library::Subscription subscription = Library::subscribeNostr(store, *author, found.at(0), found.at(1));
        QVERIFY2(subscription.error.isEmpty(), qPrintable(subscription.error));
        QCOMPARE(subscription.added, 3);

        // A second refresh with an edited article adds nothing twice.
        relay.events << signedEvent(30023, "# Hi again",
                                    {{"d", "a"}, {"title", "Hello again"}, {"published_at", QString::number(now)}},
                                    now + 400);
        const QList<NostrResult> refreshed = queryAll(pool, Library::nostrQueries(*store.feed(subscription.id)));
        const Library::Refresh refresh =
            Library::applyNostr(store, *store.feed(subscription.id), refreshed.at(0), refreshed.at(1));
        QVERIFY2(refresh.error.isEmpty(), qPrintable(refresh.error));
        QCOMPARE(refresh.added, 0);
        const QList<Article> articles = store.articles({});
        QCOMPARE(articles.size(), 3);
        QCOMPARE(articles.at(0).title, QString("Clip"));
        QCOMPARE(articles.at(0).guid, video.value("id").toString());
        QCOMPARE(articles.at(0).duration, 3725.0);
        QCOMPARE(articles.at(1).title, QString("Untitled Video"));
        QCOMPARE(articles.at(2).title, QString("Hello again"));
        QVERIFY(articles.at(2).mediaUrl.isEmpty());

        // The window shows the video, its length, and the long-form as Markdown.
        Backend backend;
        QList<QPair<QString, QStringList>> launched;
        backend.setLauncher([&](const QString &program, const QStringList &arguments) {
            launched << qMakePair(program, arguments);
            return Backend::Launch::Started;
        });
        backend.selectArticle(2);
        QVERIFY(!backend.articleIsVideo());
        QVERIFY2(backend.articleHtml().contains("<h1>") && backend.articleHtml().contains("Hi again"),
                 qPrintable(backend.articleHtml()));
        backend.playVideo();
        QVERIFY(launched.isEmpty());
        QVERIFY(!backend.statusError());
        backend.selectArticle(0);
        QVERIFY(backend.articleIsVideo());
        QCOMPARE(backend.articleDuration(), 3725.0);
        QCOMPARE(backend.articleHtml(), QString("<p>Watch this</p>"));
        QVERIFY(backend.articleUrl().startsWith("https://njump.me/nevent1"));
    }

    // A Nostr author goes by a shortened npub wherever a title shows, until
    // their profile names them; the stored title stays the whole npub, so
    // the profile's name still replaces it, and a name from OPML stays.
    void nostrTitlesShowShortenedUntilTheProfileNamesThem()
    {
        const QString npub = Nip19::npub(QString(64, 'a'));
        const QString shortened = npub.left(9) + QChar(0x2026) + npub.right(4);
        QCOMPARE(Nip19::shortNpub(npub, npub), shortened);
        const QString badChecksum = npub.chopped(1) + (npub.endsWith('q') ? "p" : "q");
        // Anything but the feed's own whole, valid npub stays as it is.
        for (const QString &text : {QString("Ada"), QString(), badChecksum, "nostr:" + npub, npub + " ", npub.toUpper(),
                                    npub.left(20)})
            QCOMPARE(Nip19::shortNpub(text, text), text);
        // A profile that names itself after another author's npub shows that
        // npub whole, not as the other author's placeholder.
        const QString someoneElse = Nip19::npub(QString(64, 'c'));
        QCOMPARE(Nip19::shortNpub(someoneElse, npub), someoneElse);

        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const QString other = Nip19::npub(QString(64, 'b'));
        qint64 placeholder = 0, named = 0, web = 0;
        {
            Store store;
            QVERIFY(store.open());
            placeholder = store.addFeed(npub, npub);
            named = store.addFeed(other, "From OPML");
            web = store.addFeed("https://w.example.com/feed", "Web");
            ParsedItem item;
            item.guid = "a";
            item.title = "Hello";
            QCOMPARE(store.saveItems(placeholder, {item}), 1);
        }
        {
            Backend backend;
            SourceModel *sources = backend.sources();
            auto data = [sources](qint64 id, int role) {
                return sources->data(sources->index(sources->rowOf(SourceModel::Kind::Feed, id)), role);
            };
            QCOMPARE(data(placeholder, SourceModel::TitleRole).toString(), shortened);
            QCOMPARE(data(placeholder, SourceModel::UrlRole).toString(), npub);
            QCOMPARE(data(named, SourceModel::TitleRole).toString(), QString("From OPML"));
            QVERIFY(data(placeholder, SourceModel::NostrRole).toBool());
            QVERIFY(data(named, SourceModel::NostrRole).toBool());
            QVERIFY(!data(web, SourceModel::NostrRole).toBool());
            QVERIFY(!data(placeholder, SourceModel::IncompleteRole).toBool());
            QVERIFY(!sources->data(sources->index(0), SourceModel::NostrRole).toBool());

            // The article list's source label, the reader's header, and the
            // unsubscribe dialog (given the selected source's title).
            backend.selectSource(sources->rowOf(SourceModel::Kind::Feed, placeholder));
            QCOMPARE(backend.sourceTitle(), shortened);
            QCOMPARE(backend.articles()->count(), 1);
            QCOMPARE(backend.articles()->data(backend.articles()->index(0), ArticleModel::FeedTitleRole).toString(),
                     shortened);
            backend.selectArticle(0);
            QCOMPARE(backend.articleFeed(), shortened);
        }

        Store store;
        QVERIFY(store.open());
        QCOMPARE(store.feed(placeholder)->title, npub);
        ParsedFeed ada;
        ada.title = "Ada";
        QVERIFY(store.updateProfile(placeholder, ada, 1, QString(64, '1')));
        QVERIFY(store.updateProfile(named, ada, 1, QString(64, '1')));
        SourceModel model;
        model.setLibrary(store.feeds(), {});
        auto title = [&model](qint64 id) {
            return model.data(model.index(model.rowOf(SourceModel::Kind::Feed, id)), SourceModel::TitleRole).toString();
        };
        QCOMPARE(title(placeholder), QString("Ada"));
        QCOMPARE(title(named), QString("From OPML"));
    }

    // A refresh that a relay was silent on, or that a cap cut short, leaves
    // a note on the author's row; a complete one clears it, and an error
    // shows in its place.
    void nostrRefreshNotesWhatItMissed()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 3600;
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeRelay good, silent;
        good.honourFilters = true;
        silent.silent = true;
        good.events = {signedEvent(0, R"({"name":"ada"})", {}, now),
                       signedEvent(30023, "# Hi", {{"d", "a"}, {"title", "Hello"}}, now + 100)};
        const QString npub = Nip19::npub(good.events.first().toObject().value("pubkey").toString());

        Backend backend;
        RelayPool::Limits limits;
        limits.connectTimeoutMs = 2000;
        limits.subscriptionTimeoutMs = 300;
        limits.graceMs = 200;
        backend.setRelayLimits(limits);
        backend.setRelays({good.url()});
        QSignalSpy subscribed(&backend, &Backend::subscribed);
        backend.addFeed(npub);
        QVERIFY(subscribed.wait(5000));
        const qint64 id = subscribed.first().first().toLongLong();
        SourceModel *sources = backend.sources();
        auto data = [sources, id](int role) {
            return sources->data(sources->index(sources->rowOf(SourceModel::Kind::Feed, id)), role);
        };
        Store store;
        QVERIFY(store.open());
        QVERIFY(data(SourceModel::NostrRole).toBool());
        QVERIFY(!data(SourceModel::IncompleteRole).toBool());
        QVERIFY(!store.feed(id)->incomplete);

        QSignalSpy finished(&backend, &Backend::refreshFinished);
        auto refresh = [&](const QStringList &relays) {
            backend.setRelays(relays);
            finished.clear();
            backend.refreshAll();
            return finished.wait(10000);
        };

        QVERIFY(refresh({good.url(), silent.url()}));
        QVERIFY(data(SourceModel::IncompleteRole).toBool());
        QVERIFY(data(SourceModel::LastErrorRole).toString().isEmpty());
        QVERIFY(!backend.statusError());
        QVERIFY(store.feed(id)->incomplete);

        // No relay answers: the error shows, not the note.
        QVERIFY(refresh({silent.url()}));
        QVERIFY(!data(SourceModel::LastErrorRole).toString().isEmpty());
        QVERIFY(!data(SourceModel::IncompleteRole).toBool());

        QVERIFY(refresh({good.url()}));
        QVERIFY(data(SourceModel::LastErrorRole).toString().isEmpty());
        QVERIFY(!data(SourceModel::IncompleteRole).toBool());
        QVERIFY(!store.feed(id)->incomplete);

        // Every relay answers, but the profile and the article are more
        // than the cap keeps.
        limits.maxEventsPerQuery = 1;
        backend.setRelayLimits(limits);
        QVERIFY(refresh({good.url()}));
        QVERIFY(data(SourceModel::IncompleteRole).toBool());
        QVERIFY(store.feed(id)->incomplete);
    }

    void backendOpensAndCopiesANostrAuthorsProfile()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const QString npub = Nip19::npub(QString(64, 'a'));
        qint64 author = 0, web = 0;
        {
            Store store;
            QVERIFY(store.open());
            author = store.addFeed(npub, "Ada");
            web = store.addFeed("https://w.example.com/feed", "Web");
        }
        const QString clipboardBefore = QGuiApplication::clipboard()->text();
        Backend backend;
        QList<QUrl> opened;
        QStringList copied;
        backend.setUrlOpener([&opened](const QUrl &url) {
            opened << url;
            return true;
        });
        backend.setClipboardWriter([&copied](const QString &text) { copied << text; });

        backend.openProfile(author);
        QCOMPARE(opened, QList<QUrl>{QUrl("https://njump.me/" + npub)});
        backend.copyNpub(author);
        QCOMPARE(copied, QStringList{npub});
        QVERIFY(!backend.statusError());
        QVERIFY2(backend.status().contains(Nip19::shortNpub(npub, npub)), qPrintable(backend.status()));

        // Nothing for a web feed or a feed that is not there.
        for (const qint64 id : {web, qint64(9999)}) {
            backend.openProfile(id);
            backend.copyNpub(id);
        }
        QCOMPARE(opened.size(), 1);
        QCOMPARE(copied.size(), 1);
        QCOMPARE(QGuiApplication::clipboard()->text(), clipboardBefore);
    }

    void storeMigrationFourStartsNostrFeedsComplete()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/readstr.db";
        const QString npub = Nip19::npub(QString(64, 'a'));
        {
            // A library as the video release wrote it, schema version 3.
            QSqlDatabase raw = QSqlDatabase::addDatabase("QSQLITE", "v3");
            raw.setDatabaseName(path);
            QVERIFY(raw.open());
            QSqlQuery query(raw);
            QStringList statements = versionOneSchema();
            statements << "ALTER TABLE items ADD COLUMN address TEXT"
                       << "ALTER TABLE items ADD COLUMN revision INTEGER"
                       << "CREATE INDEX items_by_address ON items(feed_id, address) WHERE address IS NOT NULL"
                       << "ALTER TABLE feeds ADD COLUMN relay_hints TEXT"
                       << "ALTER TABLE feeds ADD COLUMN profile_revision INTEGER"
                       << "ALTER TABLE feeds ADD COLUMN profile_event_id TEXT"
                       << "ALTER TABLE feeds ADD COLUMN profile_title TEXT"
                       << "ALTER TABLE feeds ADD COLUMN complete_at INTEGER"
                       << "ALTER TABLE items ADD COLUMN media_url TEXT"
                       << "ALTER TABLE items ADD COLUMN thumbnail_url TEXT"
                       << "ALTER TABLE items ADD COLUMN duration REAL"
                       << "INSERT INTO feeds (url, url_key, title, created_at) VALUES ('" + npub
                              + "', 'nostr-a', 'Ada', 1)"
                       << "PRAGMA user_version = 3";
            for (const QString &statement : statements)
                QVERIFY2(query.exec(statement), qPrintable(query.lastError().text()));
            query = QSqlQuery();
            raw.close();
            raw = QSqlDatabase();
            QSqlDatabase::removeDatabase("v3");
        }
        {
            Store store;
            QVERIFY2(store.open(path), qPrintable(store.lastError()));
            QCOMPARE(store.schemaVersion(), 6);
            const QList<Feed> feeds = store.feeds();
            QCOMPARE(feeds.size(), 1);
            QCOMPARE(feeds.first().title, QString("Ada"));
            QVERIFY(!feeds.first().incomplete);
            QVERIFY(store.setIncomplete(feeds.first().id, true));
        }
        Store again;
        QVERIFY2(again.open(path), qPrintable(again.lastError()));
        QVERIFY(again.feeds().first().incomplete);
        QVERIFY(again.setIncomplete(again.feeds().first().id, false));
        QVERIFY(!again.feeds().first().incomplete);
    }

    void nostrShortsLeaveRoomForArticlesAndTheProfile()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch() - 7200;
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME", "READSTR_RELAYS"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        qputenv("READSTR_RELAYS", relay.url().toUtf8());
        // A profile and an article older than 150 short videos, which one
        // query with one limit would have crowded out.
        relay.events = {signedEvent(0, R"({"name":"ada"})", {}, now),
                        signedEvent(30023, "# Hi", {{"d", "a"}, {"title", "Hello"}}, now + 1)};
        for (int i = 0; i < 150; ++i)
            relay.events << signedEvent(
                22, "Short", {{"imeta", "url https://cdn.example.com/" + QString::number(i) + ".mp4"}}, now + 10 + i);
        const QString npub = Nip19::npub(relay.events.first().toObject().value("pubkey").toString());

        // Through the headless commands...
        QString out, err;
        QCOMPARE(run({"subscribe", npub, "--json"}, out, err), 0);
        const QJsonObject subscribed = QJsonDocument::fromJson(out.toUtf8()).object();
        QCOMPARE(subscribed.value("title").toString(), QString("ada"));
        QCOMPARE(subscribed.value("articles").toInt(), 101);
        Store store;
        QVERIFY(store.open());
        ArticleFilter all;
        all.limit = 1000;
        QList<Article> articles = store.articles(all);
        QCOMPARE(articles.size(), 101);
        QCOMPARE(articles.last().title, QString("Hello"));
        QVERIFY(store.feeds().first().completeAt.isValid());
        relay.events << signedEvent(30023, "More", {{"d", "b"}, {"title", "Second"}}, now + 2);
        out.clear();
        QCOMPARE(run({"refresh", "--json"}, out, err), 0);
        const QJsonObject refreshed = QJsonDocument::fromJson(out.toUtf8()).array().at(0).toObject();
        QCOMPARE(refreshed.value("status").toString(), QString("updated"));
        QCOMPARE(refreshed.value("new").toInt(), 1);

        // ...and through the window.
        relay.events << signedEvent(30023, "Third", {{"d", "c"}, {"title", "Third"}}, now + 3);
        Backend backend;
        backend.setRelays({relay.url()});
        QSignalSpy finished(&backend, &Backend::refreshFinished);
        backend.refreshAll();
        QVERIFY(finished.wait(5000));
        QVERIFY2(backend.status().startsWith("1 new article"), qPrintable(backend.status()));
        articles = store.articles(all);
        QCOMPARE(articles.size(), 103);
        QCOMPARE(articles.last().title, QString("Hello"));
    }

    void nostrSourceKeepsVideosOffTheLocalNetwork()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const auto video = [now](const QList<QStringList> &tags) {
            return NostrEvent::fromJson(signedEvent(21, "", tags, now));
        };
        // A local media URL is passed over for the next imeta's, and a local
        // image is no thumbnail.
        auto event = video({{"imeta", "url http://192.168.1.5/a.mp4", "image https://cdn.example.com/a.jpg"},
                            {"imeta", "url http://[::ffff:127.0.0.1]/b.mp4"},
                            {"imeta", "url https://cdn.example.com/c.mp4", "image http://10.0.0.1/c.jpg"}});
        QVERIFY(event);
        QList<ParsedItem> items = NostrSource::videos({*event}, "Ada");
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().mediaUrl, QString("https://cdn.example.com/c.mp4"));
        QCOMPARE(items.first().thumbnailUrl, QString("https://cdn.example.com/a.jpg"));
        event = video({{"imeta", "url http://localhost/a.mp4", "image http://router/a.jpg"}});
        items = NostrSource::videos({*event}, "Ada");
        QVERIFY(items.first().mediaUrl.isEmpty());
        QVERIFY(items.first().thumbnailUrl.isEmpty());
    }

    void nostrSourceBelievesOnlySaneDurations()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        // Zero is unknown, as is anything not above it, past 1e7 seconds, or
        // not finite.
        const QList<QPair<QString, double>> cases = {{"0", -1},       {"0.0", -1},     {"-0", -1},   {"0.5", 0.5},
                                                      {"3725", 3725}, {"1e7", 1e7},    {"1e7.5", -1}, {"10000001", -1},
                                                      {"1e308", -1},  {"1e309", -1},   {"nan", -1},  {"x", -1}};
        for (const auto &[text, expected] : cases) {
            const auto event = NostrEvent::fromJson(
                signedEvent(21, "", {{"imeta", "url https://cdn.example.com/a.mp4", "duration " + text}}, now));
            QVERIFY(event);
            QCOMPARE(NostrSource::videos({*event}, "Ada").first().duration, expected);
        }
    }

    void backendRefusesToPlayLocalVideos()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        {
            // As a library saved before NostrSource dropped such URLs holds it.
            Store store;
            QVERIFY(store.open());
            const qint64 feed = store.addFeed(Nip19::npub(QString(64, 'a')), "Ada");
            ParsedItem item;
            item.guid = "local";
            item.title = "Local";
            item.mediaUrl = "http://192.168.1.5/a.mp4";
            item.published = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
            QCOMPARE(store.saveItems(feed, {item}), 1);
        }
        Backend backend;
        QStringList launched;
        backend.setLauncher([&](const QString &program, const QStringList &) {
            launched << program;
            return Backend::Launch::Started;
        });
        backend.selectArticle(0);
        QVERIFY(backend.articleIsVideo());
        backend.playVideo();
        QVERIFY(launched.isEmpty());
        QVERIFY(backend.statusError());
        QVERIFY2(backend.status().contains("local network"), qPrintable(backend.status()));
    }

    void backendPlaysVideosInMpv()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME", "PATH"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        // Thumbnails on this machine, which the window never asks.
        seedVideos(QUrl("http://127.0.0.1:1/a.jpg"), QUrl("http://127.0.0.1:1/b.jpg"));
        Backend backend;
        QList<QPair<QString, QStringList>> launched;
        Backend::Launch answer = Backend::Launch::Started;
        backend.setLauncher([&](const QString &program, const QStringList &arguments) {
            launched << qMakePair(program, arguments);
            return answer;
        });
        backend.playVideo();
        QVERIFY(launched.isEmpty());
        backend.selectArticle(0);
        backend.playVideo();
        QCOMPARE(launched.size(), 1);
        QCOMPARE(launched.first().first, QString("mpv"));
        QCOMPARE(launched.first().second, QStringList({"--", "https://cdn.example.com/a.mp4"}));
        QVERIFY(!backend.statusError());
        QVERIFY(backend.status().contains("Slow"));

        answer = Backend::Launch::Missing;
        backend.playVideo();
        QVERIFY(backend.statusError());
        QVERIFY2(backend.status().contains("mpv is not installed"), qPrintable(backend.status()));
        answer = Backend::Launch::Failed;
        backend.playVideo();
        QVERIFY(backend.statusError());
        QVERIFY(backend.status().contains("mpv did not start"));
        backend.selectArticle(2);
        backend.playVideo();
        QCOMPARE(launched.size(), 3);
        QCOMPARE(backend.status(), QString("This article has no video to play"));

        // Without mpv on PATH the real launcher says so, and starts nothing.
        QTemporaryDir empty;
        qputenv("PATH", empty.path().toUtf8());
        QCOMPARE(Backend::launchDetached("mpv", {"--", "https://cdn.example.com/a.mp4"}), Backend::Launch::Missing);
    }

    void thumbnailsFetchDecodeAndCache()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        server.serve("/wide.png", png(3000, 300), "image/png");
        Thumbnails thumbnails;
        Thumbnails::Limits limits;
        limits.publicHostsOnly = false;
        thumbnails.setLimits(limits);

        const QUrl url = server.url("/wide.png");
        QVERIFY(!Thumbnails::cached(url).isValid());
        const QUrl file = fetchThumbnail(thumbnails, url);
        QVERIFY(file.isLocalFile());
        QCOMPARE(file, Thumbnails::cached(url));
        QCOMPARE(file.toLocalFile(), Thumbnails::cachePath(url));
        QVERIFY(file.toLocalFile().startsWith(dir.path() + "/cache/thumbnails/"));
        QVERIFY(file.toLocalFile().endsWith(
            QCryptographicHash::hash(url.toString(QUrl::FullyEncoded).toUtf8(), QCryptographicHash::Sha256).toHex()
            + ".jpg"));
        // Kept as JPEG, which is far smaller than PNG for a photograph.
        QFile saved(file.toLocalFile());
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(saved.read(3) == QByteArray("\xff\xd8\xff"));
        const QImage image(file.toLocalFile());
        QCOMPARE(image.size(), QSize(1280, 128));
        QCOMPARE(mode(file.toLocalFile()), 0600);
        QCOMPARE(mode(dir.path() + "/cache/thumbnails"), 0700);
        QCOMPARE(mode(dir.path() + "/cache"), 0700);
        QCOMPARE(server.requests.size(), 1);
        QVERIFY(server.requests.first().headers.value("cookie").isEmpty());
        // From the cache the second time, without asking.
        QCOMPARE(fetchThumbnail(thumbnails, url), file);
        QCOMPARE(server.requests.size(), 1);
        // Nothing but the final file is left in the directory.
        QCOMPARE(QDir(dir.path() + "/cache/thumbnails").entryList(QDir::Files).size(), 1);
    }

    void thumbnailsRefuseWhatTheyShouldNot()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        server.serve("/big.png", png(10, 10) + QByteArray(3 * 1024 * 1024, 'x'), "image/png");
        server.serve("/page.png", png(10, 10), "text/html");
        server.serve("/bomb.png", pngClaiming(100000, 100000), "image/png");
        server.serve("/svg.png", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"/>",
                     "image/svg+xml");
        server.serve("/junk.png", QByteArray(1000, 'j'), "image/png");
        server.route("/gone.png", [](const FakeHttpServer::Request &) { return FakeHttpServer::Response{404, {}, {}}; });
        Thumbnails thumbnails;
        Thumbnails::Limits limits;
        limits.publicHostsOnly = false;
        thumbnails.setLimits(limits);
        QSignalSpy failed(&thumbnails, &Thumbnails::failed);
        const QList<QPair<const char *, const char *>> cases = {{"/big.png", "larger than 2 MiB"},
                                                                 {"/page.png", "not an image (text/html)"},
                                                                 {"/bomb.png", "too large to decode"},
                                                                 {"/svg.png", "not a PNG"},
                                                                 {"/junk.png", "not a PNG"},
                                                                 {"/gone.png", "HTTP 404"}};
        for (const auto &[path, reason] : cases) {
            failed.clear();
            QVERIFY2(!fetchThumbnail(thumbnails, server.url(path)).isValid(), path);
            QCOMPARE(failed.size(), 1);
            QVERIFY2(failed.first().at(1).toString().contains(reason), qPrintable(failed.first().at(1).toString()));
            QVERIFY(!Thumbnails::cached(server.url(path)).isValid());
        }

        // Within the dimensions but past the allocation limit, and past the
        // dimensions however few pixels.
        QString error;
        Thumbnails::Limits tight;
        tight.allocationLimitMb = 1;
        QVERIFY(Thumbnails::decode(png(1000, 1000), tight, &error).isNull());
        QVERIFY2(error.contains("too large"), qPrintable(error));
        QVERIFY(Thumbnails::decode(png(9000, 2), Thumbnails::Limits{}, &error).isNull());
        QVERIFY(error.contains("9000 by 2"));
        QVERIFY(!Thumbnails::decode(png(500, 2000), Thumbnails::Limits{}, &error).isNull());

        // By default this machine is never asked.
        const int asked = int(server.requests.size());
        Thumbnails strict;
        failed.clear();
        QSignalSpy strictFailed(&strict, &Thumbnails::failed);
        QVERIFY(!fetchThumbnail(strict, server.url("/page.png")).isValid());
        QVERIFY(strictFailed.first().at(1).toString().contains("local network"));
        QCOMPARE(server.requests.size(), asked);
        Fetcher fetcher;
        Fetcher::Limits publicOnly;
        publicOnly.publicHostsOnly = true;
        fetcher.setLimits(publicOnly);
        QVERIFY(fetcher.get({QUrl("http://localhost:1/x"), {}, {}}).error.contains("local network"));
        QVERIFY(fetcher.get({QUrl("http://[::1]:1/x"), {}, {}}).error.contains("local network"));
        QCOMPARE(server.requests.size(), asked);
    }

    void thumbnailsSniffTheFormatBeforeAnyReader()
    {
        QCOMPARE(Thumbnails::sniff(png(2, 2)), QByteArray("png"));
        QCOMPARE(Thumbnails::sniff(jpeg(2, 2)), QByteArray("jpeg"));
        QCOMPARE(Thumbnails::sniff(QByteArray("GIF89a\x02\x00\x02\x00", 10)), QByteArray("gif"));
        QCOMPARE(Thumbnails::sniff(QByteArray("GIF87a\x02\x00\x02\x00", 10)), QByteArray("gif"));
        QCOMPARE(Thumbnails::sniff(QByteArray("RIFF\x10\x00\x00\x00WEBPVP8 ", 16)), QByteArray("webp"));
        // Documents and the formats left out, whatever type they came as.
        const QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"/>";
        const QByteArray pdf = "%PDF-1.4\n1 0 obj << /Type /Catalog >> endobj\ntrailer << /Root 1 0 R >>\n%%EOF\n";
        for (const QByteArray &data :
             {svg, pdf, QByteArray("<?xml version=\"1.0\"?>") + svg, QByteArray("II*\x00\x08\x00\x00\x00", 8),
              QByteArray("MM\x00*", 4), QByteArray("BM\x36\x00"), QByteArray("RIFF\x10\x00\x00\x00WAVE", 12),
              QByteArray("\x89PNG", 4), QByteArray("\xff\xd8"), QByteArray("GIF8"), QByteArray()}) {
            QVERIFY2(Thumbnails::sniff(data).isEmpty(), data.toHex().constData());
            QString error;
            QVERIFY(Thumbnails::decode(data, Thumbnails::Limits{}, &error).isNull());
            QVERIFY2(error.contains("not a PNG"), qPrintable(error));
        }
        // The bytes decide, not the name: a JPEG is read as a JPEG.
        QString error;
        QCOMPARE(Thumbnails::decode(jpeg(30, 20), Thumbnails::Limits{}, &error).size(), QSize(30, 20));
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void thumbnailsRefuseJpegsThatTakeLongToDecode()
    {
        // A progressive JPEG decodes in a few scans.
        const QByteArray progressive = jpeg(64, 48, true);
        QVERIFY(progressive.count(QByteArrayView("\xff\xda", 2)) > 1);
        QString error;
        QCOMPARE(Thumbnails::decode(progressive, Thumbnails::Limits{}, &error).size(), QSize(64, 48));
        // A thousand more, empty, are refused before any reader is made, at once.
        QByteArray bomb = progressive.chopped(2);
        for (int i = 0; i < 1000; ++i)
            bomb += QByteArray::fromHex("ffda000801010000003f00");
        bomb += QByteArray::fromHex("ffd9");
        QElapsedTimer timer;
        timer.start();
        QVERIFY(Thumbnails::decode(bomb, Thumbnails::Limits{}, &error).isNull());
        QVERIFY2(error.contains("scans, too many"), qPrintable(error));
        QVERIFY(timer.elapsed() < 500);
        // A progressive one also has a smaller pixel cap than the rest.
        Thumbnails::Limits limits;
        limits.maxProgressivePixels = 64 * 47;
        QVERIFY(Thumbnails::decode(progressive, limits, &error).isNull());
        QVERIFY2(error.contains("progressive JPEG is 64 by 48"), qPrintable(error));
        QCOMPARE(Thumbnails::decode(jpeg(64, 48), limits, &error).size(), QSize(64, 48));
        // So does a baseline one of several scans.
        const QByteArray baseline = jpeg(64, 48);
        const QByteArray twoScans = baseline.chopped(2) + QByteArray::fromHex("ffda000801010000003f00ffd9");
        QVERIFY(Thumbnails::decode(twoScans, limits, &error).isNull());
        QVERIFY2(error.contains("JPEG of several scans is 64 by 48"), qPrintable(error));
        // Scan markers inside a marker segment (an Exif thumbnail's) are no
        // scans of this image, though the quick count errs toward them.
        QByteArray exif = QByteArray::fromHex("ffe1") + QByteArray(1, 0) + QByteArray(1, char(2 + 5 * 2))
                          + QByteArray("\xff\xda").repeated(5);
        const QByteArray tagged = baseline.left(2) + exif + baseline.mid(2);
        QCOMPARE(Thumbnails::decode(tagged, limits, &error).size(), QSize(64, 48));
        limits.maxJpegScans = 5;
        QVERIFY(Thumbnails::decode(tagged, limits, &error).isNull());
        QVERIFY2(error.contains("6 scans"), qPrintable(error));
    }

    void thumbnailsBudgetWhatDecodingHolds()
    {
        QString error;
        // Past half the allocation limit at 4 bytes a pixel, since scaling
        // makes a second copy, a PNG is refused before it is decoded.
        QVERIFY(Thumbnails::decode(pngClaiming(5000, 5000), Thumbnails::Limits{}, &error).isNull());
        QVERIFY2(error.contains("5000 by 5000 pixels, too large"), qPrintable(error));
        Thumbnails::Limits small;
        small.allocationLimitMb = 1;
        // 362 by 362 is just within 1 MiB at 8 bytes a pixel.
        QVERIFY(Thumbnails::decode(png(363, 363), small, &error).isNull());
        QVERIFY2(!Thumbnails::decode(png(362, 362), small, &error).isNull(), qPrintable(error));
        // A baseline JPEG decodes straight to a fraction of its size, so
        // more of one fits; a progressive one holds its coefficients too.
        small.maxWidth = 100;
        QVERIFY2(!Thumbnails::decode(jpeg(800, 400), small, &error).isNull(), qPrintable(error));
        QCOMPARE(Thumbnails::decode(jpeg(800, 400), small, &error).width(), 100);
        QVERIFY(Thumbnails::decode(jpeg(800, 400, true), small, &error).isNull());
        QVERIFY2(error.contains("800 by 400"), qPrintable(error));
    }

    void thumbnailsFetchOnlyTheLatest()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        for (const char *path : {"/a.png", "/c.png"})
            server.route(path, [](const FakeHttpServer::Request &) {
                FakeHttpServer::Response response{200, {{"Content-Type", "image/png"}}, png(40, 20)};
                response.delayMs = 300;
                return response;
            });
        server.serve("/b.png", png(20, 40), "image/png");
        Thumbnails thumbnails;
        Thumbnails::Limits limits;
        limits.publicHostsOnly = false;
        thumbnails.setLimits(limits);
        QSignalSpy failed(&thumbnails, &Thumbnails::failed);

        // A, then B at once: A is dropped before it is asked, or aborted, and
        // only B arrives and is cached.
        int calledA = 0;
        thumbnails.fetch(server.url("/a.png"), [&calledA](const QUrl &) { ++calledA; });
        const QUrl b = fetchThumbnail(thumbnails, server.url("/b.png"));
        QCOMPARE(b, QUrl::fromLocalFile(Thumbnails::cachePath(server.url("/b.png"))));
        QTest::qWait(600);
        QCOMPARE(calledA, 0);
        QVERIFY(!Thumbnails::cached(server.url("/a.png")).isValid());
        QCOMPARE(QDir(dir.path() + "/cache/thumbnails").entryList(QDir::Files).size(), 1);

        // Once asked, leaving it (cancel) aborts it the same way.
        int calledC = 0;
        thumbnails.fetch(server.url("/c.png"), [&calledC](const QUrl &) { ++calledC; });
        QTRY_VERIFY(std::any_of(server.requests.cbegin(), server.requests.cend(),
                                [](const FakeHttpServer::Request &request) { return request.path == "/c.png"; }));
        thumbnails.cancel();
        QTest::qWait(600);
        QCOMPARE(calledC, 0);
        QVERIFY(!Thumbnails::cached(server.url("/c.png")).isValid());
        QVERIFY(failed.isEmpty());
        // And the next fetch works as ever.
        QVERIFY(fetchThumbnail(thumbnails, server.url("/c.png")).isValid());
    }

    void thumbnailsSkipDoneOnceFailedDropsTheFetch()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        server.serve("/b.png", png(20, 40), "image/png");
        Thumbnails thumbnails;
        Thumbnails::Limits limits;
        limits.publicHostsOnly = false;
        thumbnails.setLimits(limits);

        // A slot on failed that cancels: the failed fetch's done never runs.
        int calledGone = 0;
        auto connection = connect(&thumbnails, &Thumbnails::failed, &thumbnails, [&thumbnails] { thumbnails.cancel(); });
        thumbnails.fetch(server.url("/gone.png"), [&calledGone](const QUrl &) { ++calledGone; });
        QTRY_COMPARE(server.requests.size(), 1);
        QTest::qWait(200);
        QCOMPARE(calledGone, 0);
        disconnect(connection);

        // One that starts another fetch: only the new one's done runs.
        std::optional<QUrl> b;
        connection = connect(&thumbnails, &Thumbnails::failed, &thumbnails, [&] {
            thumbnails.fetch(server.url("/b.png"), [&b](const QUrl &file) { b = file; });
        });
        thumbnails.fetch(server.url("/gone.png"), [&calledGone](const QUrl &) { ++calledGone; });
        QTRY_VERIFY(b.has_value());
        QCOMPARE(*b, QUrl::fromLocalFile(Thumbnails::cachePath(server.url("/b.png"))));
        QCOMPARE(calledGone, 0);
        disconnect(connection);
    }

    void thumbnailsPruneTheCache()
    {
        QTemporaryDir dir;
        const QString cache = dir.filePath("thumbnails");
        QVERIFY(QDir().mkpath(cache));
        const QDateTime now = QDateTime::currentDateTimeUtc();
        // Ten files of 100 bytes, an hour apart, and one from 40 days ago.
        const auto make = [&cache](const QString &name, const QDateTime &time) {
            QFile file(cache + "/" + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray(100, 'x'));
            // Written out first, or closing it would date it now.
            QVERIFY(file.flush());
            QVERIFY(file.setFileTime(time, QFileDevice::FileModificationTime));
        };
        for (int i = 0; i < 10; ++i)
            make(QString("%1.jpg").arg(i), now.addSecs(-3600 * i));
        make("old.jpg", now.addDays(-40));
        // Past the size, the oldest go first; past the age, so does that one.
        QCOMPARE(Thumbnails::prune(cache, 450, 30), 7);
        QCOMPARE(QDir(cache).entryList(QDir::Files, QDir::Name), QStringList({"0.jpg", "1.jpg", "2.jpg", "3.jpg"}));
        QCOMPARE(Thumbnails::prune(cache, 10000, 30), 0);
        QCOMPARE(Thumbnails::prune(QString(), 0, 0), 0);

        // A new Thumbnails prunes the cache, off its own thread.
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        const QString own = dir.filePath("cache/thumbnails");
        QVERIFY(QDir().mkpath(own));
        QFile stale(own + "/stale.jpg");
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.write("x");
        QVERIFY(stale.flush());
        QVERIFY(stale.setFileTime(now.addDays(-31), QFileDevice::FileModificationTime));
        stale.close();
        Thumbnails thumbnails;
        QTRY_VERIFY(!QFileInfo::exists(own + "/stale.jpg"));
    }

    void backendShowsOnlyTheOpenVideosThumbnail()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeHttpServer server;
        server.route("/slow.png", [](const FakeHttpServer::Request &) {
            FakeHttpServer::Response response{200, {{"Content-Type", "image/png"}}, png(40, 20)};
            response.delayMs = 600;
            return response;
        });
        server.serve("/fast.png", png(20, 40), "image/png");
        seedVideos(server.url("/slow.png"), server.url("/fast.png"));
        Backend backend;
        Thumbnails::Limits limits;
        limits.publicHostsOnly = false;
        backend.setThumbnailLimits(limits);
        QVERIFY(server.requests.isEmpty());
        QVERIFY(!backend.articleThumbnail().isValid());

        // The slow one is left before it arrives: it is dropped, so it never
        // shows on the next, and is not cached either.
        backend.selectArticle(0);
        QTRY_COMPARE(server.requests.size(), 1);
        backend.selectArticle(1);
        const QUrl fast = QUrl::fromLocalFile(Thumbnails::cachePath(server.url("/fast.png")));
        QTRY_COMPARE(backend.articleThumbnail(), fast);
        QTest::qWait(900);
        QCOMPARE(backend.articleThumbnail(), fast);
        QVERIFY(!Thumbnails::cached(server.url("/slow.png")).isValid());

        // Nor on an article without a video.
        backend.selectArticle(2);
        QVERIFY(!backend.articleThumbnail().isValid());
        QVERIFY(!backend.articleIsVideo());
        QCOMPARE(backend.articleDuration(), -1.0);
        // Back on the first, it is asked for again; after that it comes from
        // the cache at once.
        const QUrl slow = QUrl::fromLocalFile(Thumbnails::cachePath(server.url("/slow.png")));
        backend.selectArticle(0);
        QTRY_COMPARE(backend.articleThumbnail(), slow);
        QCOMPARE(backend.articleDuration(), 75.0);
        backend.selectArticle(2);
        const int asked = int(server.requests.size());
        backend.selectArticle(0);
        QCOMPARE(backend.articleThumbnail(), slow);
        QCOMPARE(server.requests.size(), asked);
        // Listing never fetched one: the slow one twice, as the first was
        // dropped, and the fast one once.
        QCOMPARE(asked, 3);

        // A thumbnail that fails shows none, and says nothing.
        Store store;
        QVERIFY(store.open());
        ParsedItem broken;
        broken.guid = "broken";
        broken.title = "Broken";
        broken.mediaUrl = "https://cdn.example.com/c.mp4";
        broken.thumbnailUrl = server.url("/missing.png").toString();
        broken.published = QDateTime(QDate(2026, 1, 10), QTime(12, 0), QTimeZone::UTC);
        QCOMPARE(store.saveItems(store.feeds().first().id, {broken}), 1);
        backend.selectSource(0);
        backend.selectArticle(0);
        QCOMPARE(backend.articleTitle(), QString("Broken"));
        QTRY_COMPARE(server.requests.size(), asked + 1);
        QTest::qWait(100);
        QVERIFY(!backend.articleThumbnail().isValid());
        QVERIFY(!backend.statusError());
        // Opened again once it is there, it is asked for again and shows.
        server.serve("/missing.png", png(30, 30), "image/png");
        backend.selectArticle(0);
        QTRY_COMPARE(backend.articleThumbnail(),
                     QUrl::fromLocalFile(Thumbnails::cachePath(server.url("/missing.png"))));
        QCOMPARE(server.requests.size(), asked + 2);
    }

    // Publishing

    // The events the desktop publishes carry the web app's d tags and
    // format, its own client tag, and what the last list held that it does
    // not keep itself, and they pass the pull's own checks.
    void syncBuildsEventsTheWebAppTakes()
    {
        const qint64 now = 1800000000;
        const NostrEvent draft = Sync::draft(Sync::readStatusKind, "{}", now);
        QCOMPARE(draft.tags, (QList<QStringList>{{"d", Sync::readStatusTag}, {"client", "readstr-omarchy"}}));
        QCOMPARE(Sync::nextCreatedAt(now, now - 10), now);
        QCOMPARE(Sync::nextCreatedAt(now, now), now + 1);
        QCOMPARE(Sync::nextCreatedAt(now, now + 99), now + 100);

        Feed a, b, author;
        a.url = "https://a.invalid/feed";
        a.category = "News";
        b.url = "https://b.invalid/feed";
        author.url = syncNpub();
        author.category = "People";
        const QJsonObject remote{
            {"rss", QJsonArray{"https://A.invalid/feed/", "https://gone.invalid/rss", "http://localhost:8080/feed",
                               "ftp://x.invalid/f"}},
            {"nostr", QJsonArray{"nostr:" + syncNpub(), "npub1bad"}},
            {"tags", QJsonObject{{"https://A.invalid/feed/", QJsonArray{"t1", "t2"}},
                                 {"http://localhost:8080/feed", QJsonArray{"lan"}}}},
            {"categories", QJsonObject{{"https://A.invalid/feed/",
                                        QJsonObject{{"name", "News"}, {"color", "#f00"}, {"icon", "N"}}},
                                       {"https://other.invalid/f", QJsonObject{{"name", "People"}, {"color", "#0f0"}}}}},
            {"deleted", QJsonArray{"https://old.invalid/x", "https://b.invalid/feed"}},
            {"lastUpdated", now - 100}};
        const QString remoteContent = QString::fromUtf8(QJsonDocument(remote).toJson());
        const qint64 storedAt = now - 50;
        const auto listOf = [&](const QList<Feed> &feeds) {
            return QJsonDocument::fromJson(Sync::subscriptionsContent(feeds, remoteContent, storedAt, now)->toUtf8())
                .object();
        };
        // The list says b was unsubscribed elsewhere, and b was here before
        // that list came: it stays off the list, and in `deleted`, though
        // this library keeps it.
        b.createdAt = QDateTime::fromSecsSinceEpoch(storedAt - 1, QTimeZone::UTC);
        QJsonObject list = listOf({a, b, author});
        QCOMPARE(list.value("rss").toArray(), (QJsonArray{a.url, "http://localhost:8080/feed", "ftp://x.invalid/f"}));
        QCOMPARE(list.value("deleted").toArray(),
                 (QJsonArray{"https://gone.invalid/rss", "https://old.invalid/x", "https://b.invalid/feed"}));
        // Subscribed here once that list came, even in the same second, it
        // is listed again.
        b.createdAt = QDateTime::fromSecsSinceEpoch(storedAt, QTimeZone::UTC);
        list = listOf({a, b, author});
        QCOMPARE(list.value("rss").toArray(),
                 (QJsonArray{a.url, b.url, "http://localhost:8080/feed", "ftp://x.invalid/f"}));
        QCOMPARE(list.value("nostr").toArray(), (QJsonArray{syncNpub(), "npub1bad"}));
        QCOMPARE(list.value("tags").toObject(), (QJsonObject{{a.url, QJsonArray{"t1", "t2"}},
                                                             {"http://localhost:8080/feed", QJsonArray{"lan"}}}));
        QCOMPARE(list.value("categories").toObject(),
                 (QJsonObject{{a.url, QJsonObject{{"name", "News"}, {"color", "#f00"}, {"icon", "N"}}},
                              {syncNpub(), QJsonObject{{"name", "People"}, {"color", "#0f0"}}}}));
        QCOMPARE(list.value("deleted").toArray(), (QJsonArray{"https://gone.invalid/rss", "https://old.invalid/x"}));
        QCOMPARE(list.value("lastUpdated").toInteger(), now);
        // With no list known, only what is here.
        const QJsonObject alone =
            QJsonDocument::fromJson(Sync::subscriptionsContent({b}, {}, 0, now)->toUtf8()).object();
        QCOMPARE(alone, (QJsonObject{{"rss", QJsonArray{b.url}}, {"nostr", QJsonArray()}, {"lastUpdated", now}}));

        // The read status: local reads first, each once, then the remote
        // ones, under 60,000 bytes counted as the web app counts them.
        QCOMPARE(Sync::publishedReadGuids({"l1", "l2", "", QString(257, 'x'), "l1"}, {"r1", "l2", "r2"}),
                 (QStringList{"l1", "l2", "r1", "r2"}));
        QStringList many;
        for (int i = 0; i < 2000; ++i)
            many << QString::number(i).rightJustified(50, '0');
        const QStringList capped = Sync::publishedReadGuids(many, {"remote"});
        // Each takes 55 bytes ("…" escaped twice, less a quote) of the
        // 60,000 less the 1,024 kept for the rest of the event.
        QCOMPARE(capped.size(), (60000 - 1024) / 55);
        QCOMPARE(capped, many.mid(0, capped.size()));
        // A quote costs four bytes more, escaped twice.
        QStringList quoted;
        for (int i = 0; i < 2000; ++i)
            quoted << "\"" + QString::number(i).rightJustified(49, '0');
        QCOMPARE(Sync::publishedReadGuids(quoted, {}).size(), (60000 - 1024) / 58);
        const QString content = Sync::readStatusContent(capped, now);
        QVERIFY(content.toUtf8().size() <= Sync::maxPublishedReadContent);
        // Escaped once more inside the event, with its other fields, it
        // still fits what common relays take.
        NostrEvent sized = Sync::draft(Sync::readStatusKind, content, now);
        sized.id = QString(64, 'a');
        sized.pubkey = QString(64, 'b');
        sized.sig = QString(128, 'c');
        QVERIFY(QJsonDocument(QJsonArray{"EVENT", sized.toJson()}).toJson(QJsonDocument::Compact).size() < 64 * 1024);

        // Saved views go without their mute words.
        QJsonObject view{{"id", "v1"}, {"name", "Tech"}, {"order", 0}, {"source", QJsonObject{{"kind", "all"}}},
                         {"readState", "all"}, {"sort", "newest"}};
        QJsonObject leaky = view;
        leaky.insert("id", "v2");
        leaky.insert("keywords", QJsonObject{{"exclude", QJsonArray{"secret"}}});
        const QString views = Sync::viewsContent({{"v1", view, {"mute"}}, {"v2", leaky, {}}}, now);
        QVERIFY(!views.contains("secret") && !views.contains("mute") && !views.contains("exclude"));
        QCOMPARE(QJsonDocument::fromJson(views.toUtf8()).object().value("views").toArray().size(), 2);

        // Signed with a key made here, each round-trips through the pull.
        QByteArray secret(32, '\0');
        for (char &byte : secret)
            byte = char(QRandomGenerator::system()->bounded(256));
        const std::unique_ptr<LocalSigner> signer = LocalSigner::create(secret);
        secret.fill('\0');
        QVERIFY(signer);
        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.path() + "/readstr.db"));
        QVERIFY(store.setSyncAccount(Nip19::npub(signer->pubkey()), signer->pubkey(), {}));
        QList<QJsonObject> sent;
        for (const NostrEvent &unsigned_ :
             {Sync::draft(Sync::subscriptionsKind, QString::fromUtf8(QJsonDocument(list).toJson()), now),
              Sync::draft(Sync::readStatusKind, Sync::readStatusContent({"g1", "g2"}, now), now),
              Sync::draft(Sync::viewsKind, views, now)}) {
            const auto [event, error] = signer->signNow(unsigned_);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            const auto parsed = NostrEvent::fromJson(event.toJson());
            QVERIFY(parsed);
            QVERIFY(parsed->verify());
            QCOMPARE(parsed->id, event.id);
            QCOMPARE(parsed->tag("client"), QString("readstr-omarchy"));
            sent << parsed->toJson();
        }
        NostrResult result = syncResult(sent);
        const Sync::Outcome outcome = Sync::apply(store, store.syncAccount(), result);
        QVERIFY2(outcome.error.isEmpty() && outcome.problems.isEmpty(),
                 qPrintable(outcome.error + outcome.problems.join("; ")));
        QCOMPARE(outcome.added, 3);
        QCOMPARE(outcome.unsupported, 3);
        QCOMPARE(outcome.pendingRead, 2);
        QCOMPARE(outcome.views, 2);
        QCOMPARE(outcome.newest.value(Sync::viewsKind), now);
        QVERIFY(outcome.settled);
        // What was applied is kept for the next publish to build on.
        QCOMPARE(store.syncRemote(Sync::subscriptionsTag)->eventId, sent.first().value("id").toString());
    }

    // A new event is dated past the newest version seen, whether pulled or
    // published here, and a list the relays already hold is not sent again.
    void syncDraftsGoPastTheNewestSeen()
    {
        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.path() + "/readstr.db"));
        QVERIFY(store.setSyncAccount(syncNpub(), syncPubkey(), {}));
        const qint64 feed = store.addFeed("https://a.invalid/feed", "A");
        ParsedItem item;
        item.guid = "g1";
        item.title = "One";
        QCOMPARE(store.saveItems(feed, {item}), 1);
        const qint64 now = 1800000000;

        Sync::Drafts drafts = Sync::drafts(store, syncPubkey(), {Sync::subscriptionsKind},
                                           {{Sync::subscriptionsKind, now + 500}}, now);
        QVERIFY(drafts.error.isEmpty());
        QCOMPARE(drafts.events.size(), 1);
        QCOMPARE(drafts.events.first().createdAt, now + 501);
        // A list applied, as in a library from before publishing, whose
        // content is not known: not replaced until a pull brings it.
        QVERIFY(store.setSyncApplied(Sync::subscriptionsTag, now + 900, QString(64, 'f')));
        drafts = Sync::drafts(store, syncPubkey(), {Sync::subscriptionsKind}, {}, now);
        QVERIFY(drafts.error.isEmpty());
        QVERIFY(drafts.events.isEmpty());
        QCOMPARE(drafts.waiting, QList<int>{Sync::subscriptionsKind});
        QCOMPARE(drafts.notes.size(), 1);
        QVERIFY(drafts.failed.isEmpty() && drafts.errors.isEmpty());
        QVERIFY(store.setSyncRemote(Sync::subscriptionsTag, QString(64, 'f'), R"({"rss":[],"nostr":[]})"));
        drafts = Sync::drafts(store, syncPubkey(), {Sync::subscriptionsKind}, {}, now);
        QCOMPARE(drafts.events.first().createdAt, now + 901);
        QCOMPARE(Sync::drafts(store, syncPubkey(), {Sync::subscriptionsKind}, {}, now + 2000).events.first().createdAt,
                 now + 2000);
        QVERIFY(!Sync::drafts(store, signedEvent(1, "", {}, 1, 2).value("pubkey").toString(),
                              {Sync::subscriptionsKind}, {}, now)
                     .error.isEmpty());

        // Nothing read and no views: nothing to say.
        QVERIFY(store.setPublishReads(syncPubkey(), true));
        drafts = Sync::drafts(store, syncPubkey(), {Sync::readStatusKind, Sync::viewsKind}, {}, now);
        QVERIFY(drafts.events.isEmpty());
        QCOMPARE(drafts.unchanged, (QList<int>{Sync::readStatusKind, Sync::viewsKind}));

        QVERIFY(store.setRead(store.articles({}).first().id, true));
        QVERIFY(store.transaction([&] {
            return store.replaceSavedViews({{"v1", QJsonObject{{"id", "v1"}, {"name", "Mine"}, {"order", 0}}, {"x"}}});
        }));
        drafts = Sync::drafts(store, syncPubkey(), {Sync::readStatusKind, Sync::viewsKind}, {}, now);
        QCOMPARE(drafts.events.size(), 2);
        QCOMPARE(QJsonDocument::fromJson(drafts.events.first().content.toUtf8()).object().value("itemGuids").toArray(),
                 QJsonArray{"g1"});
        // Recorded as published, each is the newest seen and what the
        // relays hold, so neither goes out again unchanged.
        const std::unique_ptr<LocalSigner> signer = LocalSigner::create(QByteArray(32, '\1'));
        for (const NostrEvent &draft : std::as_const(drafts.events)) {
            const NostrEvent event = signer->signNow(draft).first;
            // Stamped with when the drafts were built, not when recorded.
            bool superseded = true;
            QCOMPARE(Sync::recordPublished(store, syncPubkey(), event, now - 30, &superseded), QString());
            QVERIFY(!superseded);
            QCOMPARE(store.syncAccount().applied.value(Sync::dTag(event.kind)), now);
            QCOMPARE(store.syncRemote(QString::fromLatin1(Sync::dTag(event.kind)))->storedAt, now - 30);
        }
        QCOMPARE(*store.rememberedReadGuids(), QStringList{"g1"});
        QCOMPARE(store.pendingReadCount(), 0);
        drafts = Sync::drafts(store, syncPubkey(), {Sync::readStatusKind, Sync::viewsKind}, {}, now);
        QVERIFY(drafts.events.isEmpty());
        QCOMPARE(drafts.unchanged.size(), 2);
        // A list from another device that came in meanwhile: not replaced
        // with an older date, and kept in the union.
        QVERIFY(store.setSyncApplied(Sync::readStatusTag, now + 5, QString(64, 'e')));
        QVERIFY(store.setSyncRemote(Sync::readStatusTag, QString(64, 'e'),
                                    Sync::readStatusContent({"web1"}, now + 5)));
        drafts = Sync::drafts(store, syncPubkey(), {Sync::readStatusKind}, {{Sync::readStatusKind, now + 5}}, now);
        QCOMPARE(drafts.events.size(), 1);
        QCOMPARE(drafts.events.first().createdAt, now + 6);
        QCOMPARE(QJsonDocument::fromJson(drafts.events.first().content.toUtf8()).object().value("itemGuids").toArray(),
                 (QJsonArray{"g1", "web1"}));
    }

    // An event goes to every relay; one accepting it is enough, and each
    // relay's refusal, silence, or failure to connect is reported.
    void relayPoolPublishesAndReportsEachRelay()
    {
        FakeRelay good, refusing, silent, gone;
        refusing.rejectWith = "blocked: not here";
        silent.ignoreEvents = true;
        const QString goneUrl = gone.url();
        gone.close();
        QByteArray secret(32, '\0');
        for (char &byte : secret)
            byte = char(QRandomGenerator::system()->bounded(256));
        const std::unique_ptr<LocalSigner> signer = LocalSigner::create(secret);
        secret.fill('\0');
        QVERIFY(signer);
        const NostrEvent event =
            signer->signNow(Sync::draft(Sync::subscriptionsKind, R"({"rss":[],"nostr":[]})", 1800000000)).first;

        RelayPool pool;
        RelayPool::Limits limits;
        limits.publishTimeoutMs = 300;
        limits.connectTimeoutMs = 2000;
        pool.setLimits(limits);
        pool.setRelays({good.url(), refusing.url(), silent.url(), goneUrl});
        std::optional<PublishResult> result;
        // A hint that is not a public relay, or an onion service, is never
        // used.
        pool.publish(event, {"ws://127.0.0.1:1", good.url() + "/", "wss://relay.onion", "wss://Hidden.Onion./"},
                     [&](const PublishResult &done) { result = done; });
        QVERIFY(!result);
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 10000);
        QCOMPARE(result->relays.size(), 4);
        QVERIFY(result->ok());
        QCOMPARE(result->accepted(), 1);
        QVERIFY(result->error().isEmpty());
        QCOMPARE(result->relays.at(0).relay, good.url());
        QVERIFY(result->relays.at(0).accepted);
        QCOMPARE(result->relays.at(1).message, QString("blocked: not here"));
        QCOMPARE(result->relays.at(2).message, QString("timed out"));
        QVERIFY(!result->relays.at(3).accepted && !result->relays.at(3).message.isEmpty());
        QCOMPARE(good.published.size(), 1);
        const auto received = NostrEvent::fromJson(good.published.first());
        QVERIFY(received && received->verify());
        QCOMPARE(received->id, event.id);

        // None accepting fails it, saying why for each.
        result.reset();
        pool.setRelays({refusing.url(), silent.url()});
        pool.publish(event, {}, [&](const PublishResult &done) { result = done; });
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 10000);
        QVERIFY(!result->ok());
        QCOMPARE(result->error(), refusing.url() + ": blocked: not here; " + silent.url() + ": timed out");

        result.reset();
        pool.setRelays({"https://not-a-relay.invalid"});
        pool.publish(event, {}, [&](const PublishResult &done) { result = done; });
        QTRY_VERIFY(result.has_value());
        QCOMPARE(result->error(), QString("no usable relays"));

        // Never answered once the pool is gone.
        bool answered = false;
        {
            RelayPool brief;
            brief.setRelays({good.url()});
            brief.publish(event, {}, [&](const PublishResult &) { answered = true; });
        }
        QTest::qWait(200);
        QVERIFY(!answered);
    }

    // A change to the subscriptions waits for more, pulls so a newer list
    // from the web app is merged first, and publishes one event dated past
    // the newest seen, which the web app takes, with what was unsubscribed
    // here marked deleted.
    void backendPublishesSubscriptionsAfterPulling()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QTemporaryDir dir, other;
        FakeRelay relay;
        relay.honourFilters = true;
        relay.keepPublished = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        const QString author = Nip19::npub(signedEvent(1, "", {}, 1, 2).value("pubkey").toString());
        const QString author2 = Nip19::npub(signedEvent(1, "", {}, 1, 3).value("pubkey").toString());
        QStringList urls;
        QList<qint64> ids;
        // The web app's list, pulled and applied before, dated ahead of
        // this clock.
        const QJsonObject first = syncEvent(
            Sync::subscriptionsKind, Sync::subscriptionsTag,
            {{"rss", QJsonArray{"https://a.invalid/feed", "https://c.invalid/feed", "https://d.invalid/feed",
                                "http://192.168.1.5/feed"}},
             {"nostr", QJsonArray{author}},
             {"tags", QJsonObject{{"https://a.invalid/feed", QJsonArray{"t1"}}}},
             {"categories", QJsonObject{{"https://a.invalid/feed", QJsonObject{{"name", "News"}, {"color", "#f00"}}}}},
             {"deleted", QJsonArray{"https://old.invalid/x"}}},
            now + 100);
        {
            Store store;
            QVERIFY(store.open());
            for (const char *name : {"a", "b", "c", "d"}) {
                ids << store.addFeed(QStringLiteral("https://%1.invalid/feed").arg(name), name,
                                     QString::fromLatin1(qstrcmp(name, "a") == 0 ? "News" : ""));
                urls << store.feed(ids.last())->url;
            }
            ids << store.addFeed(author, "Author");
            for (qint64 id : std::as_const(ids))
                QVERIFY(store.recordFetch(id, {}, {}, {}));
            QVERIFY(store.setSyncApplied(Sync::subscriptionsTag, now + 100, first.value("id").toString()));
            QVERIFY(store.setSyncRemote(Sync::subscriptionsTag, first.value("id").toString(),
                                        first.value("content").toString()));
        }
        QCOMPARE(urls.first(), QString("https://a.invalid/feed"));
        relay.events = {first};

        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setPublishDelays(300, 300);
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QVERIFY(relay.log.isEmpty());

        // Two changes in a row go out as one event, after a pull.
        QVERIFY(backend.unsubscribe(ids[2]));
        QVERIFY(backend.unsubscribe(ids[3]));
        QTRY_COMPARE_WITH_TIMEOUT(relay.published.size(), 1, 10000);
        QTRY_VERIFY(!backend.publishing());
        QTest::qWait(600);
        QCOMPARE(relay.published.size(), 1);
        QVERIFY(relay.log.indexOf("REQ") >= 0 && relay.log.indexOf("REQ") < relay.log.indexOf("EVENT"));
        QVERIFY(backend.publishError().isEmpty());
        QVERIFY(backend.publishedAt().isValid());

        auto event = NostrEvent::fromJson(relay.published.first());
        QVERIFY(event && event->verify());
        QCOMPARE(event->pubkey, syncPubkey());
        QCOMPARE(event->kind, Sync::subscriptionsKind);
        QCOMPARE(event->tags, (QList<QStringList>{{"d", Sync::subscriptionsTag}, {"client", "readstr-omarchy"}}));
        QVERIFY(event->createdAt > now + 100);
        QJsonObject list = QJsonDocument::fromJson(event->content.toUtf8()).object();
        QCOMPARE(list.value("rss").toArray(), (QJsonArray{urls[0], urls[1], "http://192.168.1.5/feed"}));
        QCOMPARE(list.value("nostr").toArray(), QJsonArray{author});
        QCOMPARE(list.value("tags").toObject(), (QJsonObject{{urls[0], QJsonArray{"t1"}}}));
        QCOMPARE(list.value("categories").toObject(),
                 (QJsonObject{{urls[0], QJsonObject{{"name", "News"}, {"color", "#f00"}}}}));
        QCOMPARE(list.value("deleted").toArray(), (QJsonArray{urls[2], urls[3], "https://old.invalid/x"}));
        {
            Store store;
            QVERIFY(store.open());
            QCOMPARE(store.syncAccount().appliedIds.value(Sync::subscriptionsTag), event->id);
        }

        // Another library takes it as the web app's.
        Store fresh;
        QVERIFY(fresh.open(other.path() + "/readstr.db"));
        QVERIFY(fresh.setSyncAccount(syncNpub(), syncPubkey(), {}));
        const Sync::Outcome outcome = Sync::apply(fresh, fresh.syncAccount(), syncResult({relay.published.first()}));
        QVERIFY(outcome.problems.isEmpty() && outcome.error.isEmpty());
        QCOMPARE(outcome.added, 3);
        QCOMPARE(outcome.unsupported, 1);

        // A newer list from the web app is merged before the next publish,
        // which goes after it.
        relay.events << syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag,
                                  {{"rss", QJsonArray{urls[0], "http://192.168.1.5/feed"}},
                                   {"nostr", QJsonArray{author, author2}}},
                                  now + 200);
        QVERIFY(backend.unsubscribe(ids[1]));
        QTRY_COMPARE_WITH_TIMEOUT(relay.published.size(), 2, 10000);
        QTRY_VERIFY(!backend.publishing());
        event = NostrEvent::fromJson(relay.published.last());
        QVERIFY(event && event->verify());
        QVERIFY(event->createdAt > now + 200);
        list = QJsonDocument::fromJson(event->content.toUtf8()).object();
        QCOMPARE(list.value("rss").toArray(), (QJsonArray{urls[0], "http://192.168.1.5/feed"}));
        QStringList authors = list.value("nostr").toVariant().toStringList();
        authors.sort();
        QStringList expected{author, author2};
        expected.sort();
        QCOMPARE(authors, expected);
        QVERIFY(!list.contains("deleted"));
        QVERIFY(!list.contains("tags"));
        QCOMPARE(list.value("categories").toObject(), (QJsonObject{{urls[0], QJsonObject{{"name", "News"}}}}));
    }

    // Read articles are published only once allowed, as the union of the
    // reads here and the last list, and not again while that holds them.
    void backendPublishesReadsOnlyWhenAllowed()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        relay.keepPublished = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        {
            Store store;
            QVERIFY(store.open());
            const qint64 feed = store.addFeed("https://a.invalid/feed", "A");
            QVERIFY(store.recordFetch(feed, {}, {}, {}));
            QList<ParsedItem> items;
            for (const char *guid : {"g1", "g2", "g3"}) {
                ParsedItem item;
                item.guid = guid;
                item.title = guid;
                items << item;
            }
            QCOMPARE(store.saveItems(feed, items), 3);
        }
        relay.events = {syncEvent(Sync::readStatusKind, Sync::readStatusTag, {{"itemGuids", QJsonArray{"r1", "g3"}}},
                                  now + 50)};
        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setPublishDelays(100, 100);
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QVERIFY(!backend.publishReads());

        backend.selectSource(0);
        backend.markViewRead();
        QTRY_VERIFY(backend.status().startsWith("Marked 3 articles read"));
        QTest::qWait(400);
        QVERIFY(relay.log.isEmpty());

        backend.setPublishReads(true);
        QVERIFY(backend.publishReads());
        QTRY_COMPARE_WITH_TIMEOUT(relay.published.size(), 1, 10000);
        QTRY_VERIFY(!backend.publishing());
        const auto event = NostrEvent::fromJson(relay.published.first());
        QVERIFY(event && event->verify());
        QCOMPARE(event->kind, Sync::readStatusKind);
        QCOMPARE(event->tag("client"), QString("readstr-omarchy"));
        QVERIFY(event->createdAt > now + 50);
        QCOMPARE(QJsonDocument::fromJson(event->content.toUtf8()).object().value("itemGuids").toArray(),
                 (QJsonArray{"g3", "g2", "g1", "r1"}));
        {
            Store store;
            QVERIFY(store.open());
            QVERIFY(store.syncAccount().publishReads);
        }

        // Nothing new read: only the subscription list goes out.
        backend.publishNow();
        QVERIFY(backend.publishing());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.publishing(), 10000);
        QVERIFY(backend.publishError().isEmpty());
        QCOMPARE(relay.published.size(), 2);
        QCOMPARE(relay.published.last().value("kind").toInt(), Sync::subscriptionsKind);

        // Turned off, a read is not published.
        backend.setPublishReads(false);
        QTRY_VERIFY(!backend.publishReads());
        backend.toggleRead();
        backend.selectArticle(0);
        backend.toggleRead();
        QTest::qWait(500);
        QCOMPARE(relay.published.size(), 2);
    }

    // Without a stored key nothing is pulled for or published, and asking
    // says why.
    void backendPublishesNothingWithoutAKey()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount();
        qint64 id = 0;
        {
            Store store;
            QVERIFY(store.open());
            id = store.addFeed("https://a.invalid/feed", "A");
        }
        FakeKeyStore keys;
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setPublishDelays(50, 50);
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyChecked() && !backend.signingBusy());
        QVERIFY(backend.unsubscribe(id));
        QTest::qWait(400);
        QVERIFY(relay.log.isEmpty());
        QVERIFY(!backend.publishing());
        QVERIFY(backend.publishError().isEmpty());
        backend.publishNow();
        QCOMPARE(backend.publishError(), "No signing key is stored for " + Nip19::shortNpub(syncNpub(), syncNpub()));
        QTest::qWait(100);
        QVERIFY(relay.log.isEmpty());
        QCOMPARE(keys.calls, QStringList{"contains " + syncNpub()});
    }

    // A key that cannot be read fails the publish where the dialog shows
    // it, and is not asked for again at every pull, only on the next
    // change or "Publish now".
    void backendSaysWhenItCannotSign()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        qint64 id = 0;
        {
            Store store;
            QVERIFY(store.open());
            id = store.addFeed("https://a.invalid/feed", "A");
        }
        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setPublishDelays(50, 50);
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        keys.error = KeyStore::Error::Locked;
        QVERIFY(backend.unsubscribe(id));
        QTRY_VERIFY_WITH_TIMEOUT(!backend.publishError().isEmpty(), 10000);
        QCOMPARE(backend.publishError(),
                 QString("Cannot publish to Nostr: cannot sign: The keyring is locked. Unlock it and try again."));
        QVERIFY(backend.statusError());
        QVERIFY(!backend.publishing());
        QVERIFY(relay.published.isEmpty());
        QCOMPARE(keys.calls.count("lookup " + syncNpub()), 1);

        QSignalSpy synced(&backend, &Backend::syncFinished);
        backend.refreshAll();
        QVERIFY(synced.wait(10000));
        QTest::qWait(200);
        QCOMPARE(keys.calls.count("lookup " + syncNpub()), 1);
        QVERIFY(relay.published.isEmpty());

        keys.error = KeyStore::Error::None;
        backend.publishNow();
        QTRY_VERIFY_WITH_TIMEOUT(!backend.publishing(), 10000);
        QVERIFY2(backend.publishError().isEmpty(), qPrintable(backend.publishError()));
        QCOMPARE(relay.published.size(), 1);
    }

    // A publish no relay accepts says why, and goes out again with the
    // next pull.
    void backendPublishesAgainAfterARefusal()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        relay.rejectWith = "blocked: no";
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        qint64 id = 0;
        {
            Store store;
            QVERIFY(store.open());
            id = store.addFeed("https://a.invalid/feed", "A");
        }
        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setPublishDelays(50, 50);
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QVERIFY(backend.unsubscribe(id));
        QTRY_VERIFY_WITH_TIMEOUT(!backend.publishError().isEmpty(), 10000);
        QCOMPARE(backend.publishError(), "Cannot publish to Nostr: no relay took the subscription list: " + relay.url()
                                             + ": blocked: no");
        QVERIFY(!backend.publishedAt().isValid());
        QCOMPARE(relay.published.size(), 1);

        relay.rejectWith.clear();
        QSignalSpy synced(&backend, &Backend::syncFinished);
        backend.refreshAll();
        QVERIFY(synced.wait(10000));
        QTRY_VERIFY_WITH_TIMEOUT(!backend.publishing(), 10000);
        QCOMPARE(relay.published.size(), 2);
        QVERIFY(backend.publishError().isEmpty());
        QVERIFY(backend.publishedAt().isValid());
        QVERIFY(relay.published.last().value("created_at").toInteger()
                >= relay.published.first().value("created_at").toInteger());
    }

    // The subscription list is public: a feed URL with a user name or
    // password, or on the local network, never goes into it, nor into
    // `deleted`; and it fits what common relays take, cutting what the last
    // list carried, or not publishing when this library alone is too large.
    void syncKeepsPrivateFeedsOffTheListAndFitsTheRelays()
    {
        const qint64 now = 1800000000;
        Feed open, secret, nas, lan, single;
        open.url = "https://open.invalid/feed";
        secret.url = "https://me:hunter2@only-here.invalid/feed";
        nas.url = "http://nas.local/feed";
        lan.url = "http://192.168.1.9/rss";
        single.url = "http://intranet/feed";
        const QJsonObject remote{{"rss", QJsonArray{"http://nas.local/feed", "http://192.168.1.9/rss"}},
                                 {"nostr", QJsonArray()}};
        const QString remoteContent = QString::fromUtf8(QJsonDocument(remote).toJson());
        const auto built = Sync::subscriptionsContent({open, secret, nas, lan, single}, remoteContent, 0, now);
        QVERIFY(built);
        QVERIFY(!built->contains("hunter2"));
        QVERIFY(!built->contains("intranet"));
        const QJsonObject list = QJsonDocument::fromJson(built->toUtf8()).object();
        // What the last list held stays as it was, and nothing is deleted.
        QCOMPARE(list.value("rss").toArray(), (QJsonArray{open.url, nas.url, lan.url}));
        QVERIFY(!list.contains("deleted"));
        const auto alone = Sync::subscriptionsContent({open, secret, nas}, {}, 0, now);
        QCOMPARE(QJsonDocument::fromJson(alone->toUtf8()).object().value("rss").toArray(), QJsonArray{open.url});

        const auto fits = [](const QString &content) {
            NostrEvent event = Sync::draft(Sync::subscriptionsKind, content, 1800000000);
            event.id = QString(64, 'a');
            event.pubkey = QString(64, 'b');
            event.sig = QString(128, 'c');
            return QJsonDocument(QJsonArray{"EVENT", event.toJson()}).toJson(QJsonDocument::Compact).size()
                < 64 * 1024;
        };
        // Libraries of a few hundred feeds fit whole, and the web app takes
        // them; one too large for any relay is not published.
        for (const int count : {100, 600}) {
            QList<Feed> feeds;
            for (int i = 0; i < count; ++i) {
                Feed feed;
                feed.url = QStringLiteral("https://f%1.invalid/feed").arg(i);
                feed.category = "C";
                feeds << feed;
            }
            const auto content = Sync::subscriptionsContent(feeds, {}, 0, now);
            QVERIFY(content);
            QVERIFY(fits(*content));
            QCOMPARE(QJsonDocument::fromJson(content->toUtf8()).object().value("rss").toArray().size(), count);
        }
        QList<Feed> many;
        for (int i = 0; i < 3000; ++i) {
            Feed feed;
            feed.url = QStringLiteral("https://f%1.invalid/feed").arg(i);
            feed.category = "C";
            many << feed;
        }
        QVERIFY(!Sync::subscriptionsContent(many, {}, 0, now));

        // A last list carrying more than fits: `deleted` goes first, from
        // the end, then the feeds this machine cannot follow, and this
        // library's own feeds stay.
        QJsonArray deleted, unfollowable;
        QJsonObject tags;
        for (int i = 0; i < 2000; ++i) {
            deleted << QStringLiteral("https://gone%1.invalid/feed").arg(i);
            const QString entry = QStringLiteral("ftp://far%1.invalid/feed").arg(i);
            unfollowable << entry;
            if (i < 1000)
                tags.insert(entry, QJsonArray{"t"});
        }
        const QJsonObject full{{"rss", unfollowable}, {"nostr", QJsonArray()}, {"tags", tags}, {"deleted", deleted}};
        const auto trimmed =
            Sync::subscriptionsContent({open}, QString::fromUtf8(QJsonDocument(full).toJson()), 0, now);
        QVERIFY(trimmed);
        QVERIFY(fits(*trimmed));
        const QJsonObject cut = QJsonDocument::fromJson(trimmed->toUtf8()).object();
        QVERIFY(!cut.contains("deleted"));
        const QJsonArray rss = cut.value("rss").toArray();
        QVERIFY(rss.size() > 100 && rss.size() < 2001);
        QCOMPARE(rss.first().toString(), open.url);
        QCOMPARE(rss.at(1).toString(), unfollowable.first().toString());
        // Only `deleted` cut: the feeds carried stay whole.
        const QJsonObject some{{"rss", QJsonArray{"ftp://far.invalid/feed"}}, {"nostr", QJsonArray()},
                               {"deleted", deleted}};
        const QJsonObject kept = QJsonDocument::fromJson(
                                     Sync::subscriptionsContent({open}, QString::fromUtf8(QJsonDocument(some).toJson()),
                                                                0, now)
                                         ->toUtf8())
                                     .object();
        QCOMPARE(kept.value("rss").toArray(), (QJsonArray{open.url, "ftp://far.invalid/feed"}));
        const QJsonArray left = kept.value("deleted").toArray();
        QVERIFY(left.size() > 100 && left.size() < 2000);
        for (qsizetype i = 0; i < left.size(); ++i)
            QCOMPARE(left.at(i), deleted.at(i));
    }

    // A library from before publishing has watermarks but no content, and a
    // published event a newer one overtook is not what the relays hold.
    void syncPublishesNothingItCannotBuildOn()
    {
        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.path() + "/readstr.db"));
        QVERIFY(store.setSyncAccount(syncNpub(), syncPubkey(), {}));
        const qint64 now = 1800000000;
        QVERIFY(store.transaction([&] {
            return store.replaceSavedViews({{"v1", QJsonObject{{"id", "v1"}, {"name", "Mine"}, {"order", 0}}, {}}});
        }));
        // Views a list was applied for, though its content is not kept, are
        // not published over it.
        QVERIFY(store.setSyncApplied(Sync::viewsTag, now - 10, QString(64, 'd')));
        Sync::Drafts drafts = Sync::drafts(store, syncPubkey(), {Sync::viewsKind}, {}, now);
        QVERIFY(drafts.events.isEmpty());
        QCOMPARE(drafts.unchanged, QList<int>{Sync::viewsKind});

        // Not allowed to publish reads, the read status is left out.
        const qint64 feed = store.addFeed("https://a.invalid/feed", "A");
        ParsedItem item;
        item.guid = "mine";
        item.title = "Mine";
        QCOMPARE(store.saveItems(feed, {item}), 1);
        QVERIFY(store.setRead(store.articles({}).first().id, true));
        drafts = Sync::drafts(store, syncPubkey(), {Sync::readStatusKind}, {}, now);
        QVERIFY(drafts.events.isEmpty());
        QCOMPARE(drafts.unchanged, QList<int>{Sync::readStatusKind});
        // Only the account set may change that.
        QVERIFY(!store.setPublishReads(QString(64, '0'), true));
        QVERIFY(!store.syncAccount().publishReads);
        QVERIFY(store.setPublishReads(syncPubkey(), true));
        drafts = Sync::drafts(store, syncPubkey(), {Sync::readStatusKind}, {}, now);
        QCOMPARE(drafts.events.size(), 1);

        // A newer read status applied while this one was out keeps its
        // GUIDs and content.
        const std::unique_ptr<LocalSigner> signer = LocalSigner::create(QByteArray(32, '\1'));
        const NostrEvent mine = signer->signNow(drafts.events.first()).first;
        const QString web = Sync::readStatusContent({"web1", "web2"}, now + 10);
        QVERIFY(store.setSyncApplied(Sync::readStatusTag, now + 10, QString(64, 'e')));
        QVERIFY(store.setSyncRemote(Sync::readStatusTag, QString(64, 'e'), web));
        QVERIFY(store.rememberReadGuids({"web1", "web2"}));
        bool superseded = false;
        QCOMPARE(Sync::recordPublished(store, syncPubkey(), mine, mine.createdAt, &superseded), QString());
        QVERIFY(superseded);
        QCOMPARE(store.syncAccount().appliedIds.value(Sync::readStatusTag), QString(64, 'e'));
        QCOMPARE(store.syncRemote(Sync::readStatusTag)->content, web);
        QStringList remembered = *store.rememberedReadGuids();
        remembered.sort();
        QCOMPARE(remembered, (QStringList{"web1", "web2"}));
    }

    // Turning read articles off while a read status waits to be signed
    // keeps it off the relays.
    void backendDropsReadsTurnedOffMidPublish()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        {
            Store store;
            QVERIFY(store.open());
            for (int i = 0; i < 3; ++i) {
                const qint64 feed = store.addFeed(QStringLiteral("https://s%1.invalid/feed").arg(i), "S");
                QVERIFY(store.recordFetch(feed, {}, {}, {}));
                ParsedItem item;
                item.guid = QStringLiteral("g%1").arg(i);
                item.title = "t";
                QCOMPARE(store.saveItems(feed, {item}), 1);
            }
        }
        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setPublishDelays(30, 30);
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        backend.setPublishReads(true);
        QTRY_VERIFY(backend.publishReads());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.publishing(), 10000);
        QTest::qWait(200);
        keys.hold = true;
        backend.selectSource(0);
        backend.markViewRead();
        QTRY_VERIFY_WITH_TIMEOUT(keys.held() > 0, 10000);
        backend.setPublishReads(false);
        QVERIFY(!backend.publishReads());
        keys.hold = false;
        keys.release();
        QTRY_VERIFY_WITH_TIMEOUT(!backend.publishing(), 10000);
        QTest::qWait(300);
        for (const QJsonObject &event : std::as_const(relay.published))
            QVERIFY(event.value("kind").toInt() != Sync::readStatusKind);
        QVERIFY(backend.publishError().isEmpty());
    }

    // Nothing is published on its own for an npub until "Publish now" was
    // pressed once for it, so a library never goes out under an account the
    // user did not confirm, as after switching npubs; from then on changes
    // go out by themselves.
    void backendPublishesOnItsOwnOnlyOnceConfirmed()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        QList<qint64> ids;
        {
            Store store;
            QVERIFY(store.open());
            for (const char *name : {"a", "b", "c"})
                ids << store.addFeed(QStringLiteral("https://%1.invalid/feed").arg(name), name);
        }
        const QString other = signedEvent(1, "", {}, 1, 2).value("pubkey").toString();
        const QString otherNpub = Nip19::npub(other);
        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        keys.keys.insert(otherNpub, QByteArray(32, '\2'));
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setPublishDelays(50, 50);
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QVERIFY(backend.publishConfirmed());

        // Another npub: what changes is kept, not published.
        QVERIFY(backend.setSyncNpub(otherNpub));
        QTRY_COMPARE(backend.syncNpub(), otherNpub);
        QVERIFY(!backend.publishConfirmed());
        QTRY_VERIFY(backend.signingKeyStored());
        QVERIFY(backend.unsubscribe(ids[0]));
        QTest::qWait(500);
        QVERIFY(relay.published.isEmpty());
        QVERIFY(!backend.publishing());
        {
            Store store;
            QVERIFY(store.open());
            QVERIFY(!store.syncAccount().publishConfirmed);
            QVERIFY(store.syncAccount().publishPending & publishBit(Sync::subscriptionsKind));
        }

        // "Publish now" confirms it and publishes; then a change goes out by
        // itself.
        backend.publishNow();
        QVERIFY(backend.publishConfirmed());
        QTRY_VERIFY_WITH_TIMEOUT(!relay.published.isEmpty() && !backend.publishing(), 10000);
        QCOMPARE(relay.published.first().value("pubkey").toString(), other);
        const qsizetype before = relay.published.size();
        QVERIFY(backend.unsubscribe(ids[1]));
        QTRY_VERIFY_WITH_TIMEOUT(relay.published.size() > before, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.publishing(), 10000);
        {
            Store store;
            QVERIFY(store.open());
            QVERIFY(store.syncAccount().publishConfirmed);
            QTRY_COMPARE(store.syncAccount().publishPending, 0);
        }
    }

    // A change whose publish failed goes out after a restart, with the
    // first pull.
    void backendPublishesWhatAFailedRunLeftAfterARestart()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        relay.keepPublished = true;
        relay.rejectWith = "blocked: offline";
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        QList<qint64> ids;
        {
            Store store;
            QVERIFY(store.open());
            for (const char *name : {"a", "b"})
                ids << store.addFeed(QStringLiteral("https://%1.invalid/feed").arg(name), name);
        }
        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        {
            Backend backend;
            backend.setRelays({relay.url()});
            backend.setPublishDelays(30, 30);
            backend.setKeyStore(&keys);
            QTRY_VERIFY(backend.signingKeyStored());
            QVERIFY(backend.unsubscribe(ids[0]));
            QTRY_VERIFY_WITH_TIMEOUT(!backend.publishError().isEmpty(), 10000);
            QTRY_VERIFY(!backend.publishing());
        }
        relay.rejectWith.clear();
        relay.published.clear();
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setPublishDelays(30, 30);
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QVERIFY(backend.setSyncNpub(syncNpub()));
        QTRY_COMPARE_WITH_TIMEOUT(relay.published.size(), 1, 10000);
        QTRY_VERIFY(!backend.publishing());
        const QJsonObject list =
            QJsonDocument::fromJson(relay.published.first().value("content").toString().toUtf8()).object();
        QCOMPARE(list.value("rss").toArray(), QJsonArray{"https://b.invalid/feed"});
        Store store;
        QVERIFY(store.open());
        QTRY_COMPARE(store.syncAccount().publishPending, 0);
    }

    // A change made before the launch check of the keyring has answered
    // goes out once the key turns out to be there.
    void backendPublishesAChangeMadeBeforeTheKeyCheck()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        qint64 id = 0;
        {
            Store store;
            QVERIFY(store.open());
            id = store.addFeed("https://a.invalid/feed", "A");
            store.addFeed("https://b.invalid/feed", "B");
        }
        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        keys.hold = true;
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setPublishDelays(30, 30);
        backend.setKeyStore(&keys);
        QVERIFY(backend.unsubscribe(id));
        QTest::qWait(300);
        QVERIFY(relay.published.isEmpty());
        keys.hold = false;
        keys.release();
        QTRY_VERIFY(backend.signingKeyStored());
        QTRY_COMPARE_WITH_TIMEOUT(relay.published.size(), 1, 10000);
        QTRY_VERIFY(!backend.publishing());
        QVERIFY(backend.publishError().isEmpty());
    }

    // Reading on and on still publishes, once the longest wait is up.
    void backendPublishesReadsWhileReadingGoesOn()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        {
            Store store;
            QVERIFY(store.open());
            const qint64 feed = store.addFeed("https://a.invalid/feed", "A");
            QVERIFY(store.recordFetch(feed, {}, {}, {}));
            QList<ParsedItem> items;
            for (const char *guid : {"g1", "g2"}) {
                ParsedItem item;
                item.guid = guid;
                item.title = guid;
                items << item;
            }
            QCOMPARE(store.saveItems(feed, items), 2);
        }
        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setPublishDelays(50, 400, 700);
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        backend.setPublishReads(true);
        QTRY_VERIFY(backend.publishReads());
        QTest::qWait(600);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.publishing(), 10000);
        relay.published.clear();
        backend.selectSource(0);
        backend.selectArticle(0);
        backend.selectArticle(1);
        // A read or unread every 150 ms, each well inside the 400 ms wait.
        const auto reads = [&] {
            int count = 0;
            for (const QJsonObject &event : std::as_const(relay.published))
                count += event.value("kind").toInt() == Sync::readStatusKind;
            return count;
        };
        for (int i = 0; i < 20 && reads() == 0; ++i) {
            backend.toggleRead();
            QTest::qWait(150);
        }
        QVERIFY(reads() > 0);
    }

    // A draft is built only on the content of the event its date goes past:
    // a cached event that is not the one applied, as when a pull applied a
    // newer one between the two reads or has not cached it yet, waits for
    // the next pull.
    void syncDraftsWaitWhenTheCacheIsNotTheEventApplied()
    {
        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.path() + "/readstr.db"));
        QVERIFY(store.setSyncAccount(syncNpub(), syncPubkey(), {}));
        QVERIFY(store.setPublishReads(syncPubkey(), true));
        const qint64 feed = store.addFeed("https://a.invalid/feed", "A");
        ParsedItem item;
        item.guid = "g1";
        item.title = "One";
        QCOMPARE(store.saveItems(feed, {item}), 1);
        QVERIFY(store.setRead(store.articles({}).first().id, true));
        const qint64 now = 1800000000;
        for (const char *tag : {Sync::subscriptionsTag, Sync::readStatusTag}) {
            QVERIFY(store.setSyncApplied(tag, now + 10, QString(64, 'b')));
            QVERIFY(store.setSyncRemote(tag, QString(64, 'a'), R"({"rss":[],"nostr":[],"itemGuids":[]})"));
        }
        Sync::Drafts drafts =
            Sync::drafts(store, syncPubkey(), {Sync::subscriptionsKind, Sync::readStatusKind}, {}, now);
        QVERIFY(drafts.error.isEmpty());
        QVERIFY(drafts.events.isEmpty());
        QCOMPARE(drafts.waiting, (QList<int>{Sync::subscriptionsKind, Sync::readStatusKind}));
        QVERIFY(drafts.failed.isEmpty() && drafts.errors.isEmpty());

        // Once the pull caches the event applied, both are built on it.
        for (const char *tag : {Sync::subscriptionsTag, Sync::readStatusTag})
            QVERIFY(store.setSyncRemote(tag, QString(64, 'b'), R"({"rss":[],"nostr":[],"itemGuids":[]})"));
        drafts = Sync::drafts(store, syncPubkey(), {Sync::subscriptionsKind, Sync::readStatusKind}, {}, now);
        QVERIFY(drafts.waiting.isEmpty());
        QCOMPARE(drafts.events.size(), 2);
        QCOMPARE(drafts.events.first().createdAt, now + 11);
    }

    // What the last list carried with a user name or password is never
    // published again, deleted or carried; plain tombstones and feeds this
    // machine cannot follow still are.
    void syncDropsCarriedEntriesWithCredentials()
    {
        const qint64 now = 1800000000;
        Feed open;
        open.url = "https://open.invalid/feed";
        const QJsonObject remote{
            {"rss", QJsonArray{"https://me:pw1@gone.invalid/feed", "http://me:pw2@192.168.1.5/feed",
                               "ftp://me:pw3@far.invalid/f", "http://192.168.1.6/feed", "ftp://far.invalid/g"}},
            {"nostr", QJsonArray()},
            {"tags", QJsonObject{{"http://me:pw2@192.168.1.5/feed", QJsonArray{"t"}}}},
            {"deleted", QJsonArray{"https://me:pw4@old.invalid/x", "https://old.invalid/y"}}};
        const auto built =
            Sync::subscriptionsContent({open}, QString::fromUtf8(QJsonDocument(remote).toJson()), 0, now);
        QVERIFY(built);
        for (const char *secret : {"pw1", "pw2", "pw3", "pw4", "me:"})
            QVERIFY2(!built->contains(secret), secret);
        const QJsonObject list = QJsonDocument::fromJson(built->toUtf8()).object();
        QCOMPARE(list.value("rss").toArray(),
                 (QJsonArray{open.url, "http://192.168.1.6/feed", "ftp://far.invalid/g"}));
        QCOMPARE(list.value("deleted").toArray(), QJsonArray{"https://old.invalid/y"});
        QVERIFY(!list.contains("tags"));
    }

    // Tags and categories may name the same feed in different spellings,
    // and each is found by its own.
    void syncFindsTagsAndCategoriesSpelledApart()
    {
        const qint64 now = 1800000000;
        Feed a;
        a.url = "https://a.invalid/feed";
        a.category = "News";
        const QJsonObject remote{
            {"rss", QJsonArray{"https://A.invalid/feed/"}},
            {"nostr", QJsonArray()},
            {"tags", QJsonObject{{"https://A.invalid/feed/", QJsonArray{"t1"}}}},
            {"categories", QJsonObject{{"https://a.invalid/feed", QJsonObject{{"name", "News"}, {"icon", "N"}}}}}};
        const QJsonObject list = QJsonDocument::fromJson(
                                     Sync::subscriptionsContent({a}, QString::fromUtf8(QJsonDocument(remote).toJson()),
                                                                0, now)
                                         ->toUtf8())
                                     .object();
        QCOMPARE(list.value("tags").toObject(), (QJsonObject{{a.url, QJsonArray{"t1"}}}));
        QCOMPARE(list.value("categories").toObject(),
                 (QJsonObject{{a.url, QJsonObject{{"name", "News"}, {"icon", "N"}}}}));
    }

    // A feed whose address is past what the web app takes is left out of
    // the list, the rest still published, and the dialog says so.
    void syncSaysWhichFeedsAreTooLongToPublish()
    {
        const qint64 now = 1800000000;
        Feed open, longer, longest;
        open.url = "https://open.invalid/feed";
        longer.url = "https://long.invalid/" + QString(Sync::maxUrlLength, 'x');
        longest.url = "https://long.invalid/" + QString(Sync::maxUrlLength, 'y');
        int tooLong = -1;
        const auto built = Sync::subscriptionsContent({open, longer, longest}, {}, 0, now, &tooLong);
        QVERIFY(built);
        QCOMPARE(tooLong, 2);
        QCOMPARE(QJsonDocument::fromJson(built->toUtf8()).object().value("rss").toArray(), QJsonArray{open.url});

        QTemporaryDir dir;
        Store store;
        QVERIFY(store.open(dir.path() + "/readstr.db"));
        QVERIFY(store.setSyncAccount(syncNpub(), syncPubkey(), {}));
        QVERIFY(store.addFeed(open.url, "Open") > 0);
        QVERIFY(store.addFeed(longer.url, "Long") > 0);
        QVERIFY(store.addFeed(longest.url, "Longest") > 0);
        const Sync::Drafts drafts = Sync::drafts(store, syncPubkey(), {Sync::subscriptionsKind}, {}, now);
        QVERIFY(drafts.error.isEmpty() && drafts.errors.isEmpty() && drafts.failed.isEmpty());
        QCOMPARE(drafts.events.size(), 1);
        QCOMPARE(drafts.notes,
                 QStringList{"2 feeds are left out of the subscription list, as their addresses are too long"});
    }

    // An event no longer wanted when its relay connects is not sent there,
    // and the publish says it was called off rather than refused.
    void relayPoolSendsNothingNoLongerWanted()
    {
        FakeRelay relay;
        const std::unique_ptr<LocalSigner> signer = LocalSigner::create(QByteArray(32, '\1'));
        const NostrEvent event =
            signer->signNow(Sync::draft(Sync::readStatusKind, R"({"itemGuids":[]})", 1800000000)).first;
        RelayPool pool;
        pool.setRelays({relay.url()});
        bool wanted = true;
        std::optional<PublishResult> result;
        pool.publish(event, {}, [&](const PublishResult &done) { result = done; }, [&] { return wanted; });
        // Turned off after the publish began, while the socket connects.
        wanted = false;
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 10000);
        QVERIFY(!result->ok());
        QVERIFY(result->cancelled);
        QTest::qWait(100);
        QVERIFY(relay.published.isEmpty());

        result.reset();
        wanted = true;
        pool.publish(event, {}, [&](const PublishResult &done) { result = done; }, [&] { return wanted; });
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 10000);
        QVERIFY(result->ok());
        QVERIFY(!result->cancelled);
        QCOMPARE(relay.published.size(), 1);
    }

    // A read saved as the window closes, before its write has reported
    // back, is still marked to be published after a restart.
    void backendMarksAReadToPublishInItsOwnWrite()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        {
            Store store;
            QVERIFY(store.open());
            QVERIFY(store.setPublishReads(syncPubkey(), true));
            const qint64 feed = store.addFeed("https://a.invalid/feed", "A");
            QVERIFY(store.recordFetch(feed, {}, {}, {}));
            ParsedItem item;
            item.guid = "g1";
            item.title = "One";
            QCOMPARE(store.saveItems(feed, {item}), 1);
        }
        {
            FakeKeyStore keys;
            Backend backend;
            backend.setKeyStore(&keys);
            QVERIFY(backend.publishReads());
            backend.selectSource(0);
            backend.selectArticle(0);
        }
        Store store;
        QVERIFY(store.open());
        QVERIFY(store.articles({}).first().read);
        QVERIFY(store.syncAccount().publishPending & publishBit(Sync::readStatusKind));
    }

    // Each subscription change marks its kind in its own write, so one
    // saved as the window closes, before the window heard back, still goes
    // out after a restart.
    void backendPublishesAnUnsubscribeSavedAsTheWindowCloses()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        QList<qint64> ids;
        {
            Store store;
            QVERIFY(store.open());
            for (const char *name : {"a", "b"})
                ids << store.addFeed(QStringLiteral("https://%1.invalid/feed").arg(name), name);
        }
        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        {
            Backend backend;
            backend.setRelays({relay.url()});
            backend.setKeyStore(&keys);
            QVERIFY(backend.unsubscribe(ids[0]));
        }
        {
            Store store;
            QVERIFY(store.open());
            QCOMPARE(store.feeds().size(), 1);
            QVERIFY(store.syncAccount().publishPending & publishBit(Sync::subscriptionsKind));
        }
        publishesSubscriptionsAfterARestart(relay, keys, {"https://b.invalid/feed"});
    }

    void backendPublishesASubscribeSavedAsTheWindowCloses()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        FakeHttpServer server;
        server.serve("/feed.xml", fixture("rss2.xml"));
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        {
            Backend backend;
            backend.setRelays({relay.url()});
            backend.setKeyStore(&keys);
            QTRY_VERIFY(backend.signingKeyStored());
            QueuedCallDropper dropper;
            backend.installEventFilter(&dropper);
            backend.addFeed(server.url("/feed.xml").toString());
            Store store;
            QVERIFY(store.open());
            QTRY_COMPARE_WITH_TIMEOUT(store.feeds().size(), 1, 20000);
            QVERIFY(backend.adding());
        }
        {
            Store store;
            QVERIFY(store.open());
            QVERIFY(store.syncAccount().publishPending & publishBit(Sync::subscriptionsKind));
        }
        // The list leaves out a feed on this machine, but still goes out.
        publishesSubscriptionsAfterARestart(relay, keys, QJsonArray());
    }

    void backendPublishesAnImportSavedAsTheWindowCloses()
    {
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", (dir.path() + "/home").toUtf8());
        seedSyncAccount(true);
        write(dir.path() + "/in.opml",
              "<opml version=\"2.0\"><body>"
              "<outline type=\"rss\" text=\"A\" xmlUrl=\"https://a.invalid/feed\"/>"
              "</body></opml>");
        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        {
            Backend backend;
            backend.setRelays({relay.url()});
            backend.setKeyStore(&keys);
            QTRY_VERIFY(backend.signingKeyStored());
            QueuedCallDropper dropper;
            backend.installEventFilter(&dropper);
            QSignalSpy imported(&backend, &Backend::imported);
            backend.importFrom(QUrl::fromLocalFile(dir.path() + "/in.opml"));
            Store store;
            QVERIFY(store.open());
            QTRY_COMPARE_WITH_TIMEOUT(store.feeds().size(), 1, 20000);
            QVERIFY(imported.isEmpty());
        }
        {
            Store store;
            QVERIFY(store.open());
            QVERIFY(store.syncAccount().publishPending & publishBit(Sync::subscriptionsKind));
        }
        publishesSubscriptionsAfterARestart(relay, keys, {"https://a.invalid/feed"});
    }

    // A published list a newer one overtook before it was recorded may lack
    // feeds only this machine has, so the kind stays marked and is built
    // again on the winning list after the next pull.
    void backendPublishesAgainOverANewerListThatWon()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QTemporaryDir dir;
        FakeRelay relay;
        relay.honourFilters = true;
        relay.keepPublished = true;
        relay.okDelayMs = 600;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        seedSyncAccount(true);
        QList<qint64> ids;
        {
            Store store;
            QVERIFY(store.open());
            for (const char *name : {"a", "b"}) {
                ids << store.addFeed(QStringLiteral("https://%1.invalid/feed").arg(name), name);
                QVERIFY(store.recordFetch(ids.last(), {}, {}, {}));
            }
        }
        FakeKeyStore keys;
        keys.keys.insert(syncNpub(), QByteArray(32, '\1'));
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setPublishDelays(30, 30);
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QVERIFY(backend.unsubscribe(ids[1]));
        QTRY_COMPARE_WITH_TIMEOUT(relay.published.size(), 1, 10000);
        // While the relay has not said OK, `readstr sync` applies the web
        // app's newer list, which lacks a.
        const QJsonObject web = syncEvent(Sync::subscriptionsKind, Sync::subscriptionsTag,
                                          {{"rss", QJsonArray{"https://c.invalid/feed"}}, {"nostr", QJsonArray()}},
                                          now + 100);
        {
            Store other;
            QVERIFY(other.open());
            const Sync::Outcome outcome = Sync::apply(other, other.syncAccount(), syncResult({web}));
            QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
        }
        relay.events << web;
        QTRY_VERIFY_WITH_TIMEOUT(!backend.publishing(), 10000);
        {
            Store store;
            QVERIFY(store.open());
            QVERIFY(store.syncAccount().publishPending & publishBit(Sync::subscriptionsKind));
        }

        relay.okDelayMs = 0;
        QSignalSpy synced(&backend, &Backend::syncFinished);
        backend.refreshAll();
        QVERIFY(synced.wait(10000));
        QTRY_COMPARE_WITH_TIMEOUT(relay.published.size(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.publishing(), 10000);
        const auto event = NostrEvent::fromJson(relay.published.last());
        QVERIFY(event && event->verify());
        QVERIFY(event->createdAt > now + 100);
        QStringList rss = QJsonDocument::fromJson(event->content.toUtf8()).object().value("rss").toVariant().toStringList();
        rss.sort();
        QCOMPARE(rss, (QStringList{"https://a.invalid/feed", "https://c.invalid/feed"}));
        Store store;
        QVERIFY(store.open());
        QTRY_COMPARE(store.syncAccount().publishPending & publishBit(Sync::subscriptionsKind), 0);
    }

    // The box shows what is stored, even when a toggle did not change it.
    void syncDialogShowsWhetherReadsArePublished()
    {
        QTemporaryDir dir;
        const EnvironmentGuard guard({"READSTR_HOME"});
        qputenv("READSTR_HOME", dir.path().toUtf8());
        FakeRelay relay;
        FakeKeyStore keys;
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setKeyStore(&keys);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty(
            "theme", QVariantMap{{"radius", 0}, {"background", "#000000"}, {"foreground", "#ffffff"}});
        engine.rootContext()->setContextProperty("translator", QVariantMap{{"language", "en"}});
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport QtQuick.Controls\n"
                          "ApplicationWindow { width: 800; height: 600; property alias dialog: dialog\n"
                          " SyncDialog { id: dialog } }\n",
                          QUrl("qrc:/probe.qml"));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *box = window->findChild<QObject *>("publishReadsBox");
        QVERIFY(box);
        const auto click = [box] {
            QVERIFY(QMetaObject::invokeMethod(box, "toggle"));
            QVERIFY(QMetaObject::invokeMethod(box, "toggled"));
        };
        // No account: nothing is stored, and the box says so.
        click();
        QVERIFY(!backend.publishReads());
        QVERIFY(!box->property("checked").toBool());

        QVERIFY(backend.setSyncNpub(syncNpub()));
        QTRY_COMPARE(backend.syncNpub(), syncNpub());
        click();
        QVERIFY(backend.publishReads());
        QVERIFY(box->property("checked").toBool());
        backend.setPublishReads(false);
        QVERIFY(!box->property("checked").toBool());
    }

private:

    // A solid PNG of this size.
    static QByteArray png(int width, int height)
    {
        QImage image(width, height, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        return data;
    }

    // A solid JPEG of this size, progressive when asked.
    static QByteArray jpeg(int width, int height, bool progressive = false)
    {
        QImage image(width, height, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "jpeg");
        writer.setProgressiveScanWrite(progressive);
        if (!writer.write(image))
            qFatal("cannot write the test JPEG: %s", qPrintable(writer.errorString()));
        return data;
    }

    // `data` as bech32 with this prefix, as NIP-19 encodes it.
    static QString bech32(const QByteArray &prefix, const QByteArray &data)
    {
        static const char charset[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
        const auto polymod = [](const QList<int> &values) {
            static const quint32 generator[] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
            quint32 checksum = 1;
            for (int value : values) {
                const quint32 top = checksum >> 25;
                checksum = ((checksum & 0x1ffffff) << 5) ^ quint32(value);
                for (int i = 0; i < 5; ++i)
                    if ((top >> i) & 1)
                        checksum ^= generator[i];
            }
            return checksum;
        };
        QList<int> words;
        quint32 accumulator = 0;
        int bits = 0;
        for (char c : data) {
            accumulator = (accumulator << 8) | quint8(c);
            bits += 8;
            while (bits >= 5) {
                bits -= 5;
                words << int((accumulator >> bits) & 31);
            }
        }
        if (bits)
            words << int((accumulator << (5 - bits)) & 31);
        QList<int> values;
        for (char c : prefix)
            values << (quint8(c) >> 5);
        values << 0;
        for (char c : prefix)
            values << (quint8(c) & 31);
        values << words << QList<int>(6, 0);
        const quint32 checksum = polymod(values) ^ 1;
        QString out = QString::fromLatin1(prefix) + '1';
        for (int word : words)
            out += QLatin1Char(charset[word]);
        for (int i = 0; i < 6; ++i)
            out += QLatin1Char(charset[(checksum >> (5 * (5 - i))) & 31]);
        return out;
    }

    // Runs the queries and returns their results in order, as the headless
    // commands do for an author's two.
    static QList<NostrResult> queryAll(RelayPool &pool, const QList<NostrQuery> &queries)
    {
        QList<NostrResult> found(queries.size());
        pool.queryEach(queries, [&found](qsizetype i, const NostrResult &result) { found[i] = result; });
        return found;
    }

    // The tables and indexes of schema version 1, as phase 1 created them.
    static QStringList versionOneSchema()
    {
        return {"CREATE TABLE categories (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)",
                "CREATE TABLE feeds (id INTEGER PRIMARY KEY AUTOINCREMENT, url TEXT NOT NULL, url_key TEXT NOT NULL"
                " UNIQUE, title TEXT NOT NULL DEFAULT '', site_url TEXT, description TEXT, category_id INTEGER"
                " REFERENCES categories(id) ON DELETE SET NULL, etag TEXT, last_modified TEXT, last_fetched_at"
                " INTEGER, last_error TEXT, created_at INTEGER NOT NULL)",
                "CREATE TABLE items (id INTEGER PRIMARY KEY AUTOINCREMENT, feed_id INTEGER NOT NULL REFERENCES"
                " feeds(id) ON DELETE CASCADE, guid TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', url TEXT, author"
                " TEXT, summary TEXT, content TEXT, published_at INTEGER, created_at INTEGER NOT NULL, is_read"
                " INTEGER NOT NULL DEFAULT 0, is_starred INTEGER NOT NULL DEFAULT 0, UNIQUE(feed_id, guid))",
                "CREATE INDEX items_by_date ON items(COALESCE(published_at, created_at) DESC, id DESC)",
                "CREATE INDEX items_by_feed_unread ON items(feed_id, is_read)",
                "CREATE INDEX items_starred ON items(is_starred) WHERE is_starred = 1",
                "CREATE INDEX feeds_by_category ON feeds(category_id)"};
    }

    // A one-pixel PNG whose header claims this size, with a valid checksum,
    // as a decompression bomb would.
    static QByteArray pngClaiming(quint32 width, quint32 height)
    {
        QByteArray data = png(1, 1);
        for (int i = 0; i < 4; ++i) {
            data[16 + i] = char((width >> (24 - 8 * i)) & 0xff);
            data[20 + i] = char((height >> (24 - 8 * i)) & 0xff);
        }
        quint32 crc = 0xffffffff;
        for (int at = 12; at < 29; ++at) {
            crc ^= quint8(data.at(at));
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xedb88320 & (0 - (crc & 1)));
        }
        crc ^= 0xffffffff;
        for (int i = 0; i < 4; ++i)
            data[29 + i] = char((crc >> (24 - 8 * i)) & 0xff);
        return data;
    }

    // Waits for one fetch's file: URL, empty when it failed. A fetch that
    // has not finished in time is cancelled, so its callback never writes to
    // `result` once this has returned, and fails the test as a timeout
    // rather than passing for a failed fetch.
    static QUrl fetchThumbnail(Thumbnails &thumbnails, const QUrl &url)
    {
        std::optional<QUrl> result;
        thumbnails.fetch(url, [&result](const QUrl &file) { result = file; });
        const QDeadlineTimer deadline(10000);
        while (!result && !deadline.hasExpired())
            QTest::qWait(10);
        if (!result) {
            thumbnails.cancel();
            QTest::qFail(qPrintable("the thumbnail fetch of " + url.toString() + " timed out"), __FILE__, __LINE__);
        }
        return result.value_or(QUrl());
    }

    // An author with three articles, newest first: the video "Slow" (75
    // seconds, its thumbnail at `first`), the video "Fast" (at `second`),
    // and the text "Text".
    static void seedVideos(const QUrl &first, const QUrl &second)
    {
        Store store;
        if (!store.open())
            qFatal("cannot open the library: %s", qPrintable(store.lastError()));
        const qint64 feed = store.addFeed(Nip19::npub(QString(64, 'a')), "Ada");
        auto item = [](const char *guid, const char *title, int day) {
            ParsedItem item;
            item.guid = guid;
            item.title = title;
            item.published = QDateTime(QDate(2026, 1, day), QTime(12, 0), QTimeZone::UTC);
            return item;
        };
        ParsedItem slow = item("slow", "Slow", 3);
        slow.mediaUrl = "https://cdn.example.com/a.mp4";
        slow.thumbnailUrl = first.toString();
        slow.duration = 75;
        ParsedItem fast = item("fast", "Fast", 2);
        fast.mediaUrl = "https://cdn.example.com/b.mp4";
        fast.thumbnailUrl = second.toString();
        ParsedItem text = item("text", "Text", 1);
        text.content = "<p>Words.</p>";
        if (store.saveItems(feed, {slow, fast, text}) != 3)
            qFatal("cannot seed the videos: %s", qPrintable(store.lastError()));
    }
    // A NIP-01 event signed with the secret key whose 32 bytes are all
    // `secretByte`, as a relay would send it.
    static QJsonObject signedEvent(int kind, const QString &content, const QList<QStringList> &tags,
                                   qint64 createdAt, char secretByte = 1)
    {
        secp256k1_context *context = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
        const auto cleanup = qScopeGuard([context] { secp256k1_context_destroy(context); });
        const QByteArray secret(32, secretByte);
        secp256k1_keypair keypair;
        secp256k1_xonly_pubkey xonly;
        unsigned char pubkey[32], signature[64];
        if (!secp256k1_keypair_create(context, &keypair, reinterpret_cast<const unsigned char *>(secret.constData()))
            || !secp256k1_keypair_xonly_pub(context, &xonly, nullptr, &keypair)
            || !secp256k1_xonly_pubkey_serialize(context, pubkey, &xonly))
            qFatal("cannot make the test key");
        NostrEvent event;
        event.pubkey = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(pubkey), 32).toHex());
        event.createdAt = createdAt;
        event.kind = kind;
        event.tags = tags;
        event.content = content;
        const QByteArray hash = QCryptographicHash::hash(event.serialized(), QCryptographicHash::Sha256);
        if (!secp256k1_schnorrsig_sign32(context, signature, reinterpret_cast<const unsigned char *>(hash.constData()),
                                         &keypair, nullptr))
            qFatal("cannot sign the test event");
        QJsonArray tagArray;
        for (const QStringList &tag : tags)
            tagArray << QJsonArray::fromStringList(tag);
        return {{"id", QString::fromLatin1(hash.toHex())},
                {"pubkey", event.pubkey},
                {"created_at", createdAt},
                {"kind", kind},
                {"tags", tagArray},
                {"content", content},
                {"sig", QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(signature), 64).toHex())}};
    }

    // An event for the signing tests to sign.
    // The official vectors (nip44.vectors.json, from the NIP's repository),
    // checked against the checksum NIP-44 publishes before any is used.
    static QJsonObject nip44Vectors()
    {
        QFile file(QFINDTESTDATA("nip44.vectors.json"));
        if (!file.open(QIODevice::ReadOnly))
            qFatal("cannot read the NIP-44 vectors");
        const QByteArray contents = file.readAll();
        if (QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex()
            != "269ed0f69e4c192512cc779e78c555090cebc7c785b609e338a62afc3ce25040")
            qFatal("the NIP-44 vectors are not the published ones");
        return QJsonDocument::fromJson(contents).object().value("v2").toObject();
    }

    static QByteArray hex(const QJsonValue &value) { return QByteArray::fromHex(value.toString().toLatin1()); }

    static RemoteSigner::Limits quickRemoteLimits()
    {
        RemoteSigner::Limits limits;
        limits.requestTimeoutMs = 5000;
        limits.pairingTimeoutMs = 5000;
        limits.connectTimeoutMs = 2000;
        limits.reconnectMs = 100;
        return limits;
    }

    // A pairing with `bunker` over `relay`, as a bunker:// URI it gave would
    // make, ready to sign.
    static std::unique_ptr<RemoteSigner> pairedSigner(const FakeBunker &bunker, const FakeRelay &relay,
                                                     const QString &userPubkey = syncPubkey())
    {
        auto pairing = RemoteSigner::parseBunkerUri(bunker.uri(relay.url()), {relay.url()});
        if (!pairing)
            qFatal("cannot parse the test bunker URI");
        auto signer = RemoteSigner::create(*pairing, userPubkey);
        pairing->wipe();
        signer->setLimits(quickRemoteLimits());
        return signer;
    }

    // What a sign gave, once it has answered.
    struct Signed {
        int answers = 0;
        NostrEvent event;
        QString error;
        Signer::Done done()
        {
            return [this](const NostrEvent &signedEvent, const QString &why) {
                ++answers;
                event = signedEvent;
                error = why;
            };
        }
    };

    static NostrEvent signingDraft()
    {
        NostrEvent draft;
        draft.kind = Sync::subscriptionsKind;
        draft.createdAt = 1700000000;
        draft.tags = {{"d", Sync::subscriptionsTag}};
        draft.content = "{}";
        return draft;
    }

    // Syncs the library under READSTR_HOME with the test key's npub, with
    // "Publish now" pressed for it once when `confirmed`.
    static void seedSyncAccount(bool confirmed = false)
    {
        Store store;
        if (!store.open() || !store.setSyncAccount(syncNpub(), syncPubkey(), {})
            || (confirmed && !store.setPublishConfirmed(syncPubkey())))
            qFatal("cannot seed the sync account: %s", qPrintable(store.lastError()));
    }

    // The test key's public key and npub, which the sync tests pull for.
    static QString syncPubkey() { return signedEvent(1, "", {}, 1).value("pubkey").toString(); }
    static QString syncNpub() { return Nip19::npub(syncPubkey()); }

    // A sync event as the web app publishes it: `content` as JSON, with its
    // d tag and client tag.
    static QJsonObject syncEvent(int kind, const char *dTag, const QJsonObject &content, qint64 createdAt)
    {
        return signedEvent(kind, QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Compact)),
                           {{"d", dTag}, {"client", "readstr"}}, createdAt);
    }

    // What a relay that answered would hand Sync::apply for these events.
    static NostrResult syncResult(const QList<QJsonObject> &events)
    {
        NostrResult result;
        result.relays = result.answered = 1;
        for (const QJsonObject &event : events)
            result.events << *NostrEvent::fromJson(event);
        return result;
    }

    // Restores the variables a test changes, so later tests see the original
    // environment even when a comparison fails part way.
    class EnvironmentGuard {
    public:
        explicit EnvironmentGuard(const QList<const char *> &names) : m_names(names)
        {
            for (const char *name : names)
                m_values << (qEnvironmentVariableIsSet(name) ? std::optional(qgetenv(name)) : std::nullopt);
        }
        ~EnvironmentGuard()
        {
            for (qsizetype i = 0; i < m_names.size(); ++i)
                m_values[i] ? qputenv(m_names[i], *m_values[i]) : qunsetenv(m_names[i]);
        }

    private:
        QList<const char *> m_names;
        QList<std::optional<QByteArray>> m_values;
    };

    // A new window publishes the subscription list left marked, as `rss`,
    // after its first pull.
    static void publishesSubscriptionsAfterARestart(FakeRelay &relay, FakeKeyStore &keys, const QJsonArray &rss)
    {
        QVERIFY(relay.published.isEmpty());
        Backend backend;
        backend.setRelays({relay.url()});
        backend.setPublishDelays(30, 30);
        backend.setKeyStore(&keys);
        QTRY_VERIFY(backend.signingKeyStored());
        QVERIFY(backend.setSyncNpub(syncNpub()));
        QTRY_COMPARE_WITH_TIMEOUT(relay.published.size(), 1, 10000);
        QTRY_VERIFY(!backend.publishing());
        QVERIFY2(backend.publishError().isEmpty(), qPrintable(backend.publishError()));
        QCOMPARE(relay.published.first().value("kind").toInt(), Sync::subscriptionsKind);
        const QJsonObject list =
            QJsonDocument::fromJson(relay.published.first().value("content").toString().toUtf8()).object();
        QCOMPARE(list.value("rss").toArray(), rss);
        Store store;
        QVERIFY(store.open());
        QTRY_COMPARE(store.syncAccount().publishPending, 0);
    }

    static int run(const QStringList &arguments, QString &out, QString &err)
    {
        QTextStream outStream(&out), errStream(&err);
        const int code = Cli::run(arguments, outStream, errStream);
        outStream.flush();
        errStream.flush();
        return code;
    }

    static QByteArray fixtureAt(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            qFatal("cannot read %s", qPrintable(path));
        return file.readAll();
    }

    static QByteArray fixture(const char *name)
    {
        return fixtureAt(QFINDTESTDATA(QString("fixtures/") + name));
    }

    // Two feeds, five articles: "Newest" (with markup to sanitize), "Older",
    // and "Oldest" in First, "Second A" and "Second B" (read) in Second.
    static std::pair<qint64, qint64> seedLibrary()
    {
        Store store;
        if (!store.open())
            qFatal("cannot open the library: %s", qPrintable(store.lastError()));
        const qint64 first = store.addFeed("https://first.example.com/feed", "First");
        const qint64 second = store.addFeed("https://second.example.com/feed", "Second");
        auto item = [](const char *guid, const char *title, int day) {
            ParsedItem item;
            item.guid = guid;
            item.title = title;
            item.url = QStringLiteral("https://first.example.com/") + guid;
            if (day)
                item.published = QDateTime(QDate(2026, 1, day), QTime(12, 0), QTimeZone::UTC);
            return item;
        };
        ParsedItem newest = item("newest", "Newest", 20);
        newest.content = "<p>The <strong>full</strong> text.</p><script>alert(1)</script>";
        store.saveItems(first, {newest, item("older", "Older", 10), item("oldest", "Oldest", 1)});
        store.saveItems(second, {item("second-a", "Second A", 15), item("second-b", "Second B", 5)});
        for (const Article &article : store.articles({}))
            if (article.guid == "second-b")
                store.setRead(article.id, true);
        return {first, second};
    }

    // For polling: each looks the library up once per try, and a missing
    // row or failed query is a miss rather than a crash.
    static bool unreadIs(Store &store, int expected)
    {
        const std::optional<LibraryCounts> counts = store.counts();
        return counts && counts->unread == expected;
    }

    static bool articleIs(Store &store, qint64 id, const std::function<bool(const Article &)> &check)
    {
        const std::optional<Article> article = store.article(id);
        return article && check(*article);
    }

    static int mode(const QString &path)
    {
        struct stat st;
        return ::stat(QFile::encodeName(path).constData(), &st) == 0 ? int(st.st_mode & 07777) : -1;
    }

    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(ReadstrTests)
#include "readstr_tests.moc"
