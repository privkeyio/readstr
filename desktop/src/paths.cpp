#include "paths.h"

#include <QDir>
#include <QStandardPaths>

namespace {
QString resolve(QStandardPaths::StandardLocation location, const char *overrideName) {
    const QString home = qEnvironmentVariable("READSTR_HOME");
    if (!home.isEmpty())
        return QDir::cleanPath(home + "/" + overrideName);
    return QDir::cleanPath(QStandardPaths::writableLocation(location) + "/readstr");
}
}

namespace Paths {
QString dataDirectory() { return resolve(QStandardPaths::GenericDataLocation, "data"); }
QString configDirectory() { return resolve(QStandardPaths::GenericConfigLocation, "config"); }
QString cacheDirectory() { return resolve(QStandardPaths::GenericCacheLocation, "cache"); }
QString databaseFile() { return dataDirectory() + "/readstr.db"; }
QString settingsFile() { return configDirectory() + "/settings.json"; }
}
