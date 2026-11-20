#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDeadlineTimer>
#include <QTextCursor>
#include <QGuiApplication>
#include <QClipboard>
#include <QDBusMessage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTextDocument>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <atomic>
#include <cstdio>
#include <climits>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>

#include "articlemodel.h"
#include "backend.h"
#include "cli.h"
#include "discovery.h"
#include "fakehttpserver.h"
#include "feedparser.h"
#include "fetcher.h"
#include "filepicker.h"
#include "localnetwork.h"
#include "opml.h"
#include "paths.h"
#include "portalfilepicker.h"
#include "sanitizer.h"
#include "sourcemodel.h"
#include "store.h"
#include "theme.h"
#include "translator.h"

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
            QCOMPARE(store.schemaVersion(), 1);
            QVERIFY(store.addFeed("https://a.example.com/feed", "A"));
        }
        Store again;
        QVERIFY2(again.open(path), qPrintable(again.lastError()));
        QCOMPARE(again.schemaVersion(), 1);
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
                         QDateTime(QDate(2024, 1, 1), QTime(0, 0), QTimeZone::UTC)};
        ParsedItem second{"guid-2", "Two", {}, {}, {}, {}, {}};
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
        const ParsedItem item{"guid", "Title", {}, {}, {}, {}, {}};
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
                    if (!store.open(path) || store.schemaVersion() != 1) {
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
                        items << ParsedItem{QString("%1-%2").arg(worker).arg(i), "Item", {}, {}, {}, {}, {}};
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
        QCOMPARE(writer.schemaVersion(), 1);
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
            QCOMPARE(writer.saveItems(feed, {ParsedItem{"late", "Late", {}, {}, {}, {}, {}}}), 1);
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
        for (const char *name : {"backend.cpp", "sanitizer.cpp", "portalfilepicker.cpp"}) {
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
        Feed loose{1, "https://l.example/", "Loose", {}, {}, {}, {}, {}, {}, {}, 2, 3};
        Feed news{2, "https://n.example/", "Daily", {}, {}, "News", {}, {}, "HTTP 404", {}, 1, 1};
        Feed alpha{3, "https://a.example/", "Alpha", {}, {}, "art", {}, {}, {}, {}, 4, 4};
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

private:
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
