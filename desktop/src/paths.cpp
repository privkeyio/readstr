#include "paths.h"

#include <QDir>
#include <QStandardPaths>

// Phase 1 must create these directories 0700 and the files in them 0600: they
// hold the subscriptions and, from phase 2, Nostr keys.
namespace {
QString resolve(QStandardPaths::StandardLocation location, const char *overrideName) {
    // A relative READSTR_HOME would follow the working directory, and falling
    // back to the real XDG directories would touch the data it was set to
    // keep away from, so it means no usable location at all.
    const QString home = qEnvironmentVariable("READSTR_HOME");
    if (!home.isEmpty())
        return QDir::isAbsolutePath(home) ? QDir::cleanPath(home + "/" + overrideName) : QString();
    // An empty location would otherwise become /readstr at the filesystem root.
    const QString base = QStandardPaths::writableLocation(location);
    return base.isEmpty() ? QString() : QDir::cleanPath(base + "/readstr");
}

QString fileIn(const QString &directory, const char *name) {
    return directory.isEmpty() ? QString() : directory + "/" + name;
}
}

namespace Paths {
QString dataDirectory() { return resolve(QStandardPaths::GenericDataLocation, "data"); }
QString configDirectory() { return resolve(QStandardPaths::GenericConfigLocation, "config"); }
QString cacheDirectory() { return resolve(QStandardPaths::GenericCacheLocation, "cache"); }
QString databaseFile() { return fileIn(dataDirectory(), "readstr.db"); }
QString settingsFile() { return fileIn(configDirectory(), "settings.json"); }
}
