#pragma once

#include <QStringList>
#include <QTextStream>

// Headless commands for agents and the bar widget. Each prints to `out`,
// reports errors on `err`, and returns the process exit code (0 or 1).
namespace Cli {
// READSTR_VERSION comes from VERSION in sources.pri.
inline constexpr char version[] = READSTR_VERSION;

// No arguments, or `open`, shows the window; anything else runs headless.
bool opensWindow(const QStringList &arguments);
int run(const QStringList &arguments, QTextStream &out, QTextStream &err);
// Flushes `out` and turns a failed write (a full disk, a closed pipe) into
// exit code 1, so scripts never mistake truncated output for success.
int finish(int code, QTextStream &out, QTextStream &err);
}
