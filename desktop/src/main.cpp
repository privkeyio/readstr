// readstr: read RSS, Nostr long-form, and video subscriptions in an Omarchy window.

#include <QCoreApplication>
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
#include "keystore.h"
#include "localnetwork.h"
#include "portalfilepicker.h"
#include "qrcode.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    QStringList arguments;
    for (int i = 1; i < argc; ++i)
        arguments << QString::fromLocal8Bit(argv[i]);

    // Headless commands must run without a display. They get a
    // QCoreApplication, which has the event loop the network needs but loads
    // no platform plugin (Omarchy's gtk3 platform theme exits the process when
    // no display is reachable), and the environment is left untouched, so
    // any helper process they start sees the user's own QT_QPA_* settings.
    if (!Cli::opensWindow(arguments)) {
        QCoreApplication app(argc, argv);
        app.setApplicationName("readstr");
        app.setApplicationVersion(Cli::version);
        QTextStream out(stdout), err(stderr);
        const int code = Cli::finish(Cli::run(arguments, out, err), out, err);
        return std::fflush(stdout) != 0 || std::ferror(stdout) ? 1 : code;
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("readstr");
    app.setApplicationVersion(Cli::version);

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
    PortalFilePicker picker;
    // Before the backend, so it outlives the backend that uses it.
    SecretServiceKeyStore keyStore;
    Backend backend(&picker);
    backend.setKeyStore(&keyStore);

    // Feeds are fetched in C++; QML reaches no network at all, so nothing in
    // an article can load a remote resource.
    LocalNetworkFactory localNetwork;
    QQmlApplicationEngine engine;
    engine.setNetworkAccessManagerFactory(&localNetwork);
    // The engine owns the provider. image://qr/ draws a nostrconnect://
    // link's QR code in process, so the link never leaves for a service.
    engine.addImageProvider(QStringLiteral("qr"), new QrImageProvider);
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    // Feeds refresh only while the window is open, as in omafeed.
    backend.startAutoRefresh();

    return app.exec();
}
