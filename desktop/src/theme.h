#pragma once

#include <QFileSystemWatcher>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QTimer>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QString background READ background NOTIFY changed)
    Q_PROPERTY(QString foreground READ foreground NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)

public:
    // Tests pass a theme directory, a fake hyprctl, and a fake Hyprland
    // event socket; the defaults are Omarchy's and the running Hyprland's.
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {},
                   const QString &eventSocket = {});
    ~Theme() override;

    QString accent() const { return m_accent; }
    QString accentForeground() const;
    QString background() const { return m_background; }
    QString foreground() const { return m_foreground; }
    int radius() const { return m_radius; }
    // True while rounding follows Hyprland's event socket rather than polling.
    bool listening() const { return m_events.state() == QLocalSocket::ConnectedState; }

    static QString readColor(const QByteArray &colors, const char *key, const QString &fallback);

signals:
    void changed();

private:
    void reload();
    void refreshRounding();
    void pollRounding();
    void roundingFailed();

    QString m_directory;
    QString m_accent = "#FFD60A";
    QString m_background = "#0e0e10";
    QString m_foreground = "#ffffff";
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
    // Coalesces a burst of config reload events into one query.
    QTimer m_reloadDebounce;
    // One more try after a failed query, which nothing else would repeat
    // until the next config reload while the event socket is up.
    QTimer m_roundingRetry;
    bool m_roundingRetried = false;
    // Asked again once the query running now finishes.
    bool m_roundingAgain = false;
    QLocalSocket m_events;
};
