// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

#include "smallfile.h"

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl, const QString &eventSocket)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    // Without Hyprland the corners stay sharp, like Omarchy's default.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        if (m_roundingAgain) {
            m_roundingAgain = false;
            QTimer::singleShot(0, this, &Theme::refreshRounding);
        }
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &Theme::refreshRounding);

    // Hyprland announces every config reload on its event socket, so the
    // rounding is asked for again only then. Without the socket (an older
    // Hyprland, or it went away) it falls back to asking every second.
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const QString instance = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
    const QString socketPath = !eventSocket.isEmpty() ? eventSocket
                               : directory.isEmpty() && !runtime.isEmpty() && !instance.isEmpty()
                                   ? runtime + "/hypr/" + instance + "/.socket2.sock"
                                   : QString();
    connect(&m_events, &QLocalSocket::readyRead, this, [this] {
        bool reloaded = false;
        while (m_events.canReadLine())
            reloaded |= m_events.readLine().startsWith("configreloaded>>");
        // A line that never ends must not pile up.
        if (m_events.bytesAvailable() > 64 * 1024)
            m_events.readAll();
        if (reloaded)
            refreshRounding();
    });
    connect(&m_events, &QLocalSocket::errorOccurred, this, &Theme::pollRounding);
    connect(&m_events, &QLocalSocket::disconnected, this, &Theme::pollRounding);
    if (!m_hyprctl.isEmpty()) {
        if (!socketPath.isEmpty() && QFileInfo::exists(socketPath))
            m_events.connectToServer(socketPath);
        else
            m_roundingPoll.start();
    }
    reload();
}

Theme::~Theme() {
    // Closing the socket emits disconnected, which must not start polling now.
    disconnect(&m_events, nullptr, this, nullptr);
    m_events.abort();
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::pollRounding() {
    if (m_hyprctl.isEmpty() || m_roundingPoll.isActive())
        return;
    m_roundingPoll.start();
    refreshRounding();
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty())
        return;
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingAgain = true;
        return;
    }
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

QString Theme::readColor(const QByteArray &colors, const char *key, const QString &fallback) {
    const QRegularExpression expression(QString(R"re(^\s*%1\s*=\s*["'](#[0-9a-fA-F]{6})["'])re").arg(key),
                                        QRegularExpression::MultilineOption);
    const auto match = expression.match(QString::fromUtf8(colors));
    return match.hasMatch() ? match.captured(1) : fallback;
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QString Theme::accentForeground() const {
    const QColor c(m_accent);
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    return luminance > .179 ? "black" : "white";
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change. Watching the nearest existing ancestor means
    // installing Omarchy later works without restarting the app.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml"};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    const QByteArray colors = readSmallFile(m_directory + "/theme/colors.toml");
    const auto accent = readColor(colors, "accent", "#FFD60A");
    const auto background = readColor(colors, "background", "#0e0e10");
    const auto foreground = readColor(colors, "foreground", "#ffffff");
    if (accent != m_accent || background != m_background || foreground != m_foreground) {
        m_accent = accent;
        m_background = background;
        m_foreground = foreground;
        emit changed();
    }
}
