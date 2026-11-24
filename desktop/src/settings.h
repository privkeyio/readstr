#pragma once

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QTimer>

// The user's preferences, one JSON object in Paths::settingsFile()
// (settings.json), shown to QML as properties. A missing file is the
// defaults. So is one that cannot be read or is not a JSON object, with the
// reason in error(); it is left as it is until a setting changes. A value of
// the wrong type reads as its default. Keys this version does not know are
// kept, so an older Readstr never drops a newer one's.
//
// Every change is written at once: beside the file and renamed over it, so a
// crash never leaves half of it, mode 0600 in a 0700 directory. The file is
// read again first and only the changed key is set in it, so a change made
// meanwhile elsewhere (`readstr settings set`, another window) is kept. A
// failed write keeps the change for this session and says why in error().
//
// The file, and the directory it is renamed into, are watched, so a change
// made elsewhere is read as it lands and each setting it changes is
// signalled: turning remote images off from the CLI takes effect in a
// running window.
//
// A new setting is a property whose getter reads value() and whose setter
// calls change(), which writes the file and says whether it changed.
class Settings : public QObject {
    Q_OBJECT
    // Off by default: no article image is fetched unless the user asks.
    Q_PROPERTY(bool loadRemoteImages READ loadRemoteImages WRITE setLoadRemoteImages NOTIFY loadRemoteImagesChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    // Reads Paths::settingsFile().
    explicit Settings(QObject *parent = nullptr);
    // Reads `path`; an empty one is no file, which nothing can be saved to.
    explicit Settings(const QString &path, QObject *parent = nullptr);

    QString path() const { return m_path; }
    QString error() const { return m_error; }
    // Whether error() is that the file could not be read, rather than that
    // a change could not be saved.
    bool unreadable() const { return m_unreadable; }

    bool loadRemoteImages() const { return value(loadRemoteImagesKey).toBool(false); }
    void setLoadRemoteImages(bool on);

    static constexpr char loadRemoteImagesKey[] = "loadRemoteImages";

    // The settings as the file holds them, every known one with its value
    // or its default, for the CLI.
    QJsonObject all() const;
    // Sets a known setting from the CLI; false, with the reason in `error`,
    // for an unknown key or a value of the wrong type, or when it cannot be
    // saved.
    bool set(const QString &key, const QJsonValue &value, QString *error);

signals:
    void loadRemoteImagesChanged();
    void errorChanged();

private:
    // A known setting's default, or undefined for an unknown key.
    static QJsonValue defaultValue(const QString &key);
    // The stored value when it has its default's type, else the default.
    QJsonValue value(const QString &key) const;
    // Stores and saves `value`; false when it was already the value.
    bool change(const QString &key, const QJsonValue &value);
    // The file's settings into `values`, an empty object when there is no
    // file; false, with the reason in `error`, when it cannot be read.
    bool read(QJsonObject *values, QString *error) const;
    // Reads the file again, after a change made elsewhere.
    void reload();
    // Takes `values`, with the changes this session could not save on top,
    // and signals each known setting whose value that changes.
    void apply(QJsonObject values);
    // Watches the file, its directory, or while there is none the
    // directory above it; a file replaced by a rename is a new one to
    // watch.
    void watch();
    void setError(const QString &error, bool unreadable = false);

    QString m_path;
    QJsonObject m_values;
    // The changes whose write failed, kept for the session over what the
    // file says.
    QJsonObject m_unsaved;
    // Why m_unsaved could not be written, kept so that it is the error again
    // once a file that was unreadable meanwhile reads again.
    QString m_saveError;
    QString m_error;
    bool m_unreadable = false;
    QFileSystemWatcher m_watcher;
    // Gathers the several events one write makes into one reading.
    QTimer m_reload;
};
