#pragma once

#include <QStringList>
#include <QTextStream>

// Headless commands for agents and the bar widget. Each prints to `out`,
// reports errors on `err`, and returns the process exit code (0 or 1).
namespace Cli {
inline constexpr char version[] = "0.1.0";

// No arguments, or `open`, shows the window; anything else runs headless.
bool opensWindow(const QStringList &arguments);
int run(const QStringList &arguments, QTextStream &out, QTextStream &err);
}
