// readstr: read RSS, Nostr long-form, and video subscriptions in an Omarchy window.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>
#include <QUrl>
#include <cstdio>

#include "backend.h"
#include "cli.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    QStringList arguments;
    for (int i = 1; i < argc; ++i)
        arguments << QString::fromLocal8Bit(argv[i]);

    // Headless commands must run without a display, so the platform is chosen
    // before the application exists. Omarchy also sets QT_QPA_PLATFORMTHEME to
    // gtk3, whose plugin exits the process when no display is reachable.
    const bool window = Cli::opensWindow(arguments);
    if (!window) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qunsetenv("QT_QPA_PLATFORMTHEME");
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("readstr");
    app.setApplicationVersion(Cli::version);

    if (!window) {
        QTextStream out(stdout), err(stderr);
        return Cli::run(arguments, out, err);
    }

    // Matches the window to readstr.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("readstr");
    app.setWindowIcon(QIcon::fromTheme("readstr"));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
