#include "cli.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace {
using Handler = int (*)(const QStringList &, QTextStream &, QTextStream &);

struct Command {
    const char *name;
    Handler handler;
};

void usage(QTextStream &stream) {
    stream << "Usage: readstr [open]\n"
              "       readstr version [--json]\n"
              "       readstr --version\n";
}

int version(const QStringList &options, QTextStream &out, QTextStream &err) {
    bool json = false;
    for (const auto &option : options) {
        if (option != "--json") {
            err << "readstr version: unknown option '" << option << "'\n";
            return 1;
        }
        json = true;
    }
    if (json)
        out << QJsonDocument(QJsonObject{{"name", "readstr"}, {"version", Cli::version}, {"qt", qVersion()}})
                   .toJson(QJsonDocument::Compact)
            << "\n";
    else
        out << "readstr " << Cli::version << "\n";
    return 0;
}

int help(const QStringList &, QTextStream &out, QTextStream &) {
    usage(out);
    return 0;
}

// Every headless command is one row here; phase 1 adds feeds, articles,
// refresh, subscribe, import, and export (docs/OMARCHY_APP_PLAN.md, section 5).
constexpr Command commands[] = {
    {"version", version},
    {"--version", version},
    {"help", help},
    {"--help", help},
};
}

namespace Cli {
bool opensWindow(const QStringList &arguments) {
    return arguments.isEmpty() || (arguments.size() == 1 && arguments.first() == "open");
}

int run(const QStringList &arguments, QTextStream &out, QTextStream &err) {
    if (arguments.isEmpty()) {
        usage(err);
        return 1;
    }
    const QString name = arguments.first();
    for (const auto &command : commands)
        if (name == QLatin1String(command.name))
            return command.handler(arguments.mid(1), out, err);
    err << "readstr: unknown command '" << name << "'\n";
    usage(err);
    return 1;
}
}
