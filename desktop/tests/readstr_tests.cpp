#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <optional>

#include "cli.h"
#include "paths.h"
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
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "background = \"#000000\"\naccent = \"#112233\"\n");

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
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
        QCOMPARE(out, QString("readstr 0.1.0\n"));
        QVERIFY(err.isEmpty());

        out.clear();
        QCOMPARE(run({"version"}, out, err), 0);
        QCOMPARE(out, QString("readstr 0.1.0\n"));
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

    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(ReadstrTests)
#include "readstr_tests.moc"
