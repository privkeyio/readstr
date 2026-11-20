#include <QBuffer>
#include <QDir>
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
#include <optional>
#include <sys/stat.h>

#include "cli.h"
#include "discovery.h"
#include "fakehttpserver.h"
#include "feedparser.h"
#include "fetcher.h"
#include "opml.h"
#include "paths.h"
#include "store.h"
#include "theme.h"
#include "translator.h"

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
