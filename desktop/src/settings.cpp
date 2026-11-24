#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>

#include "library.h"
#include "paths.h"
#include "smallfile.h"

Settings::Settings(QObject *parent) : Settings(Paths::settingsFile(), parent) {}

Settings::Settings(const QString &path, QObject *parent) : QObject(parent), m_path(path) {
    QString error;
    if (!read(&m_values, &error))
        setError(error, true);
    if (m_path.isEmpty())
        return;
    m_reload.setSingleShot(true);
    m_reload.setInterval(50);
    connect(&m_reload, &QTimer::timeout, this, &Settings::reload);
    const auto changed = [this] { m_reload.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    watch();
}

QJsonValue Settings::defaultValue(const QString &key) {
    if (key == QLatin1String(loadRemoteImagesKey))
        return false;
    return QJsonValue(QJsonValue::Undefined);
}

QJsonValue Settings::value(const QString &key) const {
    const QJsonValue fallback = defaultValue(key);
    const QJsonValue stored = m_values.value(key);
    return stored.type() == fallback.type() ? stored : fallback;
}

QJsonObject Settings::all() const {
    QJsonObject known;
    for (const char *key : {loadRemoteImagesKey})
        known.insert(QLatin1String(key), value(QLatin1String(key)));
    return known;
}

bool Settings::read(QJsonObject *values, QString *error) const {
    if (m_path.isEmpty() || !QFileInfo::exists(m_path)) {
        *values = {};
        return true;
    }
    const QByteArray data = readSmallFile(m_path);
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parse);
    if (data.isEmpty())
        *error = QStringLiteral("cannot read %1: empty, unreadable, or not a regular file of at most 64 KiB").arg(m_path);
    else if (parse.error != QJsonParseError::NoError)
        *error = QStringLiteral("cannot read %1: %2").arg(m_path, parse.errorString());
    else if (!document.isObject())
        *error = QStringLiteral("cannot read %1: not a JSON object").arg(m_path);
    else {
        *values = document.object();
        return true;
    }
    return false;
}

void Settings::watch() {
    const QString directory = QFileInfo(m_path).absolutePath();
    const QString above = QFileInfo(directory).absolutePath();
    // A write renames a new file over the old one, whose watch goes with
    // it, so the file is watched afresh each time.
    m_watcher.removePath(m_path);
    if (QFileInfo(m_path).isFile())
        m_watcher.addPath(m_path);
    // Until the directory is made, the one above it says when it is.
    const bool hasDirectory = QFileInfo(directory).isDir();
    if (hasDirectory && !m_watcher.directories().contains(directory))
        m_watcher.addPath(directory);
    if (hasDirectory && m_watcher.directories().contains(above))
        m_watcher.removePath(above);
    else if (!hasDirectory && QFileInfo(above).isDir() && !m_watcher.directories().contains(above))
        m_watcher.addPath(above);
}

void Settings::reload() {
    watch();
    QJsonObject values;
    QString error;
    // As at the start, a file that cannot be read is the defaults.
    const bool readable = read(&values, &error);
    apply(readable ? values : QJsonObject());
    // A failed write's error stays until a write succeeds, and comes back
    // once a file that could not be read in the meantime reads again.
    if (!readable)
        setError(error, true);
    else
        setError(m_unsaved.isEmpty() ? QString() : m_saveError);
}

void Settings::apply(QJsonObject values) {
    for (auto it = m_unsaved.constBegin(); it != m_unsaved.constEnd(); ++it)
        values.insert(it.key(), it.value());
    const bool loadedRemoteImages = loadRemoteImages();
    m_values = values;
    if (loadRemoteImages() != loadedRemoteImages)
        emit loadRemoteImagesChanged();
}

void Settings::setError(const QString &error, bool unreadable) {
    m_unreadable = unreadable && !error.isEmpty();
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}

bool Settings::change(const QString &key, const QJsonValue &value) {
    // From the file as it is now, so what was changed elsewhere since it
    // was last read is neither lost nor written back over. A file that
    // cannot be read is replaced with what this session holds.
    QJsonObject values = m_values;
    QString error;
    read(&values, &error);
    error.clear();
    // A value whose write failed is written again even when it is asked
    // for again unchanged, so a retry once the problem is mended saves it
    // and clears the error; only one the file already holds is skipped.
    const bool unchanged = values.value(key) == value && !m_unsaved.contains(key);
    for (auto it = m_unsaved.constBegin(); it != m_unsaved.constEnd(); ++it)
        values.insert(it.key(), it.value());
    if (unchanged) {
        apply(values);
        return false;
    }
    values.insert(key, value);
    // Compact, so that a file of large unknown keys which reads now is not
    // pushed past the read limit by indentation; one that would still not
    // read again is left as it is.
    const QByteArray data = QJsonDocument(values).toJson(QJsonDocument::Compact);
    const QString directory = QFileInfo(m_path).absolutePath();
    // The directory holds the library's settings and, beside them, the
    // user's other private files, so only they may list it.
    if (m_path.isEmpty())
        error = QStringLiteral("no config directory: READSTR_HOME must be an absolute path, or unset it and set "
                               "XDG_CONFIG_HOME");
    else if (data.size() > maxSmallFileSize)
        error = QStringLiteral("cannot write %1: the settings would come to %2 bytes, more than the %3 that can be "
                               "read back")
                    .arg(m_path)
                    .arg(data.size())
                    .arg(maxSmallFileSize);
    else if (!QDir().mkpath(directory)
             || !QFile::setPermissions(directory,
                                       QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        error = QStringLiteral("cannot create %1").arg(directory);
    else
        Library::writePrivately(m_path, data, &error);
    if (error.isEmpty())
        m_unsaved = {};
    else
        m_unsaved.insert(key, value);
    m_saveError = error;
    apply(values);
    // Saved, a file that could not be read before is replaced and so no
    // longer an error.
    setError(error);
    if (!m_path.isEmpty())
        watch();
    return true;
}

void Settings::setLoadRemoteImages(bool on) { change(QLatin1String(loadRemoteImagesKey), on); }

bool Settings::set(const QString &key, const QJsonValue &value, QString *error) {
    const QJsonValue fallback = defaultValue(key);
    if (fallback.isUndefined()) {
        *error = QStringLiteral("unknown setting %1").arg(key);
        return false;
    }
    if (value.type() != fallback.type()) {
        *error = QStringLiteral("%1 takes %2").arg(key, fallback.isBool() ? QStringLiteral("true or false")
                                                                         : QStringLiteral("another value"));
        return false;
    }
    if (key == QLatin1String(loadRemoteImagesKey))
        setLoadRemoteImages(value.toBool());
    *error = m_error;
    return m_error.isEmpty();
}
