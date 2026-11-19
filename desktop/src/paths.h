#pragma once

#include <QString>

// Where Readstr keeps its files: the XDG data, config, and cache directories,
// each with a readstr subdirectory. READSTR_HOME overrides all three (as
// data/, config/, and cache/ under it), so tests and the offscreen harness
// never touch the user's real files.
namespace Paths {
QString dataDirectory();
QString configDirectory();
QString cacheDirectory();
QString databaseFile();
QString settingsFile();
}
