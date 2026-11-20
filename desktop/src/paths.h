#pragma once

#include <QString>

// Where Readstr keeps its files: the XDG data, config, and cache directories,
// each with a readstr subdirectory. READSTR_HOME overrides all three (as
// data/, config/, and cache/ under it), so tests and the offscreen harness
// never touch the user's real files. An empty result means no usable
// location, which callers must not write to; a relative READSTR_HOME gives
// that rather than the real directories.
namespace Paths {
QString dataDirectory();
QString configDirectory();
QString cacheDirectory();
QString databaseFile();
QString settingsFile();
}
