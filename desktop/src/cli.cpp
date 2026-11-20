#include "cli.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include "discovery.h"
#include "fetcher.h"
#include "feedparser.h"
#include "opml.h"
#include "smallfile.h"
#include "store.h"

namespace {
using Handler = int (*)(const QString &, const QStringList &, QTextStream &, QTextStream &);

struct Command {
    const char *name;
    Handler handler;
};

void usage(QTextStream &stream) {
    stream << "Usage: readstr [open]\n"
              "       readstr feeds [--json]\n"
              "       readstr articles [--feed ID] [--unread] [--starred] [--limit N] [--json]\n"
              "       readstr subscribe URL [--json]\n"
              "       readstr unsubscribe ID\n"
              "       readstr refresh [--feed ID] [--json]\n"
              "       readstr read|unread|star|unstar ID...\n"
              "       readstr import FILE.opml [--json]\n"
              "       readstr export [FILE]\n"
              "       readstr version [--json]\n"
              "       readstr --version\n";
}

struct Options {
    bool json = false;
    QSet<QString> flags;
    QHash<QString, QString> values;
    QStringList positional;
};

// Parses `--flag`, `--name VALUE`, and positional arguments, rejecting any
// option the command does not take.
bool parseOptions(const QString &command, const QStringList &arguments, const QStringList &flags,
                  const QStringList &valued, Options &options, QTextStream &err) {
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (!argument.startsWith("--") || argument == "--") {
            options.positional << argument;
        } else if (argument == "--json" && flags.contains("--json")) {
            options.json = true;
        } else if (flags.contains(argument)) {
            options.flags.insert(argument);
        } else if (valued.contains(argument)) {
            if (i + 1 >= arguments.size()) {
                err << "readstr " << command << ": " << argument << " needs a value\n";
                return false;
            }
            options.values.insert(argument, arguments.at(++i));
        } else {
            err << "readstr " << command << ": unknown option '" << argument << "'\n";
            return false;
        }
    }
    return true;
}

bool parseId(const QString &command, const QString &text, qint64 &id, QTextStream &err) {
    bool ok = false;
    id = text.toLongLong(&ok);
    if (ok && id > 0)
        return true;
    err << "readstr " << command << ": '" << text << "' is not an id\n";
    return false;
}

bool openStore(const QString &command, Store &store, QTextStream &err) {
    if (store.open())
        return true;
    err << "readstr " << command << ": " << store.lastError() << "\n";
    return false;
}

// Feed and OPML text reaches the terminal in text mode, so control characters
// (escape sequences) become spaces and bidirectional formatting characters,
// which can make a line read differently than it is, are dropped.
QString printable(const QString &text) {
    QString safe;
    safe.reserve(text.size());
    for (const QChar character : text) {
        const char16_t code = character.unicode();
        if (code < 0x20 || (code >= 0x7F && code <= 0x9F))
            safe += ' ';
        else if (code == 0x061C || code == 0x200E || code == 0x200F || (code >= 0x202A && code <= 0x202E)
                 || (code >= 0x2066 && code <= 0x2069))
            continue;
        else
            safe += character;
    }
    return safe;
}

int fail(const QString &command, const QString &message, QTextStream &err) {
    err << "readstr " << command << ": " << printable(message) << "\n";
    return 1;
}

void printJson(const QJsonValue &value, QTextStream &out) {
    const QJsonDocument document =
        value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
    out << document.toJson(QJsonDocument::Compact) << "\n";
}

QJsonValue nullable(const QString &text) { return text.isEmpty() ? QJsonValue() : QJsonValue(text); }
QJsonValue nullable(const QDateTime &time) {
    return time.isValid() ? QJsonValue(time.toUTC().toString(Qt::ISODate)) : QJsonValue();
}

QJsonObject feedJson(const Feed &feed) {
    return {{"id", feed.id},
            {"url", feed.url},
            {"title", feed.title},
            {"siteUrl", nullable(feed.siteUrl)},
            {"description", nullable(feed.description)},
            {"category", nullable(feed.category)},
            {"unread", feed.unread},
            {"total", feed.total},
            {"lastFetchedAt", nullable(feed.lastFetchedAt)},
            {"lastError", nullable(feed.lastError)}};
}

QJsonObject articleJson(const Article &article) {
    return {{"id", article.id},
            {"feedId", article.feedId},
            {"feedTitle", article.feedTitle},
            {"guid", article.guid},
            {"title", article.title},
            {"url", nullable(article.url)},
            {"author", nullable(article.author)},
            {"summary", nullable(article.summary)},
            {"published", nullable(article.published)},
            {"read", article.read},
            {"starred", article.starred}};
}

bool isWebUrl(const QUrl &url) {
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == "http" || scheme == "https") && !url.host().isEmpty();
}

int feeds(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {"--json"}, {}, options, err))
        return 1;
    if (!options.positional.isEmpty())
        return fail(command, "takes no arguments", err);
    Store store;
    if (!openStore(command, store, err))
        return 1;
    const QList<Feed> list = store.feeds();
    if (!store.lastError().isEmpty())
        return fail(command, store.lastError(), err);

    if (options.json) {
        QJsonArray array;
        for (const Feed &feed : list)
            array << feedJson(feed);
        printJson(array, out);
        return 0;
    }
    for (const Feed &feed : list) {
        out << feed.id << "  " << feed.unread << "/" << feed.total << "  " << printable(feed.title) << "  "
            << printable(feed.url);
        if (!feed.category.isEmpty())
            out << "  [" << printable(feed.category) << "]";
        if (!feed.lastError.isEmpty())
            out << "  (error: " << printable(feed.lastError) << ")";
        out << "\n";
    }
    return 0;
}

int articles(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {"--json", "--unread", "--starred"}, {"--feed", "--limit"}, options, err))
        return 1;
    if (!options.positional.isEmpty())
        return fail(command, "takes no arguments", err);
    ArticleFilter filter;
    filter.unreadOnly = options.flags.contains("--unread");
    filter.starredOnly = options.flags.contains("--starred");
    if (options.values.contains("--feed") && !parseId(command, options.values.value("--feed"), filter.feedId, err))
        return 1;
    if (options.values.contains("--limit")) {
        bool ok = false;
        filter.limit = options.values.value("--limit").toInt(&ok);
        if (!ok || filter.limit < 0)
            return fail(command, "--limit needs a number (0 for no limit)", err);
    }

    Store store;
    if (!openStore(command, store, err))
        return 1;
    if (filter.feedId && !store.feed(filter.feedId))
        return fail(command, store.lastError(), err);
    const QList<Article> list = store.articles(filter);
    if (!store.lastError().isEmpty())
        return fail(command, store.lastError(), err);

    if (options.json) {
        QJsonArray array;
        for (const Article &article : list)
            array << articleJson(article);
        printJson(array, out);
        return 0;
    }
    for (const Article &article : list) {
        out << article.id << "  " << (article.published.isValid() ? article.published.toString("yyyy-MM-dd") : "----------")
            << "  " << (article.read ? ' ' : 'N') << (article.starred ? '*' : ' ') << "  " << printable(article.title)
            << "  (" << printable(article.feedTitle) << ")\n";
        if (!article.url.isEmpty())
            out << "    " << printable(article.url) << "\n";
    }
    return 0;
}

int subscribe(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {"--json"}, {}, options, err))
        return 1;
    if (options.positional.size() != 1)
        return fail(command, "needs one URL", err);
    const QUrl url(options.positional.first().trimmed(), QUrl::StrictMode);
    if (!isWebUrl(url))
        return fail(command, "the URL must start with http:// or https://", err);

    Store store;
    if (!openStore(command, store, err))
        return 1;
    Fetcher fetcher;
    const Discovery::Result found = Discovery::discover(fetcher, url);
    if (!found.found)
        return fail(command, found.error, err);

    const QString feedUrl = found.feedUrl.toString(QUrl::FullyEncoded);
    bool created = false;
    const qint64 id = store.addFeed(feedUrl, found.feed.title, {}, &created);
    if (!id)
        return fail(command, store.lastError(), err);
    int added = 0;
    if (created) {
        added = store.saveItems(id, found.feed.items);
        if (added < 0 || !store.updateFeedDetails(id, found.feed)
            || !store.recordFetch(id, found.fetch.etag, found.fetch.lastModified, {})) {
            // A feed left half saved would read as already subscribed next
            // time and never get its articles, so it goes and a retry starts
            // clean.
            const QString error = store.lastError();
            store.removeFeed(id);
            return fail(command, error, err);
        }
    }
    const std::optional<Feed> feed = store.feed(id);
    if (!feed)
        return fail(command, store.lastError(), err);

    if (options.json) {
        printJson(QJsonObject{{"id", feed->id},
                              {"url", feed->url},
                              {"title", feed->title},
                              {"created", created},
                              {"discovered", found.discovered},
                              {"articles", added}},
                  out);
    } else if (created) {
        out << "Subscribed to " << printable(feed->title) << " (id " << feed->id << ", " << added << " articles)\n";
    } else {
        out << "Already subscribed to " << printable(feed->title) << " (id " << feed->id << ")\n";
    }
    return 0;
}

int unsubscribe(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {}, {}, options, err))
        return 1;
    qint64 id = 0;
    if (options.positional.size() != 1)
        return fail(command, "needs one feed id", err);
    if (!parseId(command, options.positional.first(), id, err))
        return 1;
    Store store;
    if (!openStore(command, store, err))
        return 1;
    const std::optional<Feed> feed = store.feed(id);
    if (!feed || !store.removeFeed(id))
        return fail(command, store.lastError(), err);
    out << "Unsubscribed from " << printable(feed->title) << "\n";
    return 0;
}

int refresh(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {"--json"}, {"--feed"}, options, err))
        return 1;
    if (!options.positional.isEmpty())
        return fail(command, "takes no arguments", err);
    qint64 only = 0;
    if (options.values.contains("--feed") && !parseId(command, options.values.value("--feed"), only, err))
        return 1;

    Store store;
    if (!openStore(command, store, err))
        return 1;
    QList<Feed> targets;
    if (only) {
        const std::optional<Feed> feed = store.feed(only);
        if (!feed)
            return fail(command, store.lastError(), err);
        targets << *feed;
    } else {
        targets = store.feeds();
        if (!store.lastError().isEmpty())
            return fail(command, store.lastError(), err);
    }

    QList<FetchRequest> requests;
    for (const Feed &feed : targets)
        requests << FetchRequest{QUrl(feed.url), feed.etag, feed.lastModified};

    // Each response is parsed and saved as it arrives, so only the few in
    // flight hold a body; the rows are printed in feed order at the end.
    struct Row {
        QString status, error, title;
        int added = 0;
    };
    QList<Row> rows(targets.size());
    Fetcher fetcher;
    fetcher.fetchEach(requests, [&](qsizetype i, const FetchResult &fetched) {
        const Feed &feed = targets.at(i);
        Row &row = rows[i];
        row.title = feed.title;
        if (!fetched.ok()) {
            row.error = fetched.error;
        } else if (fetched.notModified) {
            row.status = QStringLiteral("not-modified");
            // A 304 may leave out the validators; the stored ones still hold.
            if (!store.recordFetch(feed.id, fetched.etag.isEmpty() ? feed.etag : fetched.etag,
                                   fetched.lastModified.isEmpty() ? feed.lastModified : fetched.lastModified, {}))
                row.error = store.lastError();
        } else if (const auto parsed = FeedParser::parse(fetched.body, fetched.finalUrl, &row.error)) {
            row.status = QStringLiteral("updated");
            row.added = store.saveItems(feed.id, parsed->items);
            if (row.added < 0 || !store.updateFeedDetails(feed.id, *parsed)
                || !store.recordFetch(feed.id, fetched.etag, fetched.lastModified, {}))
                row.error = store.lastError();
            if (const auto updated = store.feed(feed.id))
                row.title = updated->title;
        }
        if (!row.error.isEmpty()) {
            row.status = QStringLiteral("error");
            row.added = 0;
            store.recordFetch(feed.id, {}, {}, row.error);
        }
    });

    int code = 0;
    QJsonArray array;
    for (qsizetype i = 0; i < targets.size(); ++i) {
        const Feed &feed = targets.at(i);
        const Row &row = rows.at(i);
        if (!row.error.isEmpty()) {
            err << "readstr " << command << ": " << printable(row.title) << ": " << printable(row.error) << "\n";
            code = 1;
        }
        if (options.json) {
            array << QJsonObject{{"id", feed.id},         {"url", feed.url},      {"title", row.title},
                                 {"status", row.status},  {"new", row.added},     {"error", nullable(row.error)}};
        } else if (row.status == QLatin1String("updated")) {
            out << feed.id << "  " << printable(row.title) << ": " << row.added << " new\n";
        } else if (row.status == QLatin1String("not-modified")) {
            out << feed.id << "  " << printable(row.title) << ": not modified\n";
        } else {
            out << feed.id << "  " << printable(row.title) << ": failed\n";
        }
    }
    if (options.json)
        printJson(array, out);
    return code;
}

int setFlag(const QString &command, const QStringList &arguments, QTextStream &, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {}, {}, options, err))
        return 1;
    if (options.positional.isEmpty())
        return fail(command, "needs at least one article id", err);
    QList<qint64> ids;
    for (const QString &text : options.positional) {
        qint64 id = 0;
        if (!parseId(command, text, id, err))
            return 1;
        ids << id;
    }
    Store store;
    if (!openStore(command, store, err))
        return 1;
    int code = 0;
    for (const qint64 id : ids) {
        const bool done = command == "read"     ? store.setRead(id, true)
                          : command == "unread" ? store.setRead(id, false)
                          : command == "star"   ? store.setStarred(id, true)
                                                : store.setStarred(id, false);
        if (!done)
            code = fail(command, store.lastError(), err);
    }
    return code;
}

int importOpml(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {"--json"}, {}, options, err))
        return 1;
    if (options.positional.size() != 1)
        return fail(command, "needs one OPML file", err);
    const QString path = options.positional.first();
    const QByteArray data = readSmallFile(path, Opml::maxBytes);
    if (data.isNull())
        return fail(command,
                    QStringLiteral("cannot read %1 (missing, not a regular file, or larger than %2 MiB)")
                        .arg(path)
                        .arg(Opml::maxBytes / (1024 * 1024)),
                    err);
    QString error;
    const QList<Opml::Outline> outlines = Opml::parse(data, &error);
    if (!error.isEmpty())
        return fail(command, error, err);

    // Nostr sources arrive in phase 2; until then they are counted, not
    // dropped silently. They are set aside before de-duplication, so an
    // unsupported outline cannot shadow a supported one with the same key.
    QList<Opml::Outline> supported;
    int unsupported = 0;
    for (const Opml::Outline &outline : outlines) {
        if (!outline.npub.isEmpty() || !isWebUrl(QUrl(outline.xmlUrl, QUrl::StrictMode)))
            ++unsupported;
        else
            supported << outline;
    }

    Store store;
    if (!openStore(command, store, err))
        return 1;
    const QList<Feed> list = store.feeds();
    if (!store.lastError().isEmpty())
        return fail(command, store.lastError(), err);
    QStringList existing;
    for (const Feed &feed : list)
        existing << feed.url;
    const Opml::Plan plan = Opml::plan(supported, existing);

    int added = 0;
    for (const Opml::Outline &outline : plan.toAdd) {
        bool created = false;
        if (!store.addFeed(outline.xmlUrl, outline.title, outline.folder, &created))
            return fail(command, store.lastError(), err);
        added += created;
    }

    if (options.json)
        printJson(QJsonObject{{"added", added}, {"skipped", plan.skipped}, {"unsupported", unsupported}}, out);
    else
        out << "Imported " << added << " feeds (" << plan.skipped << " already subscribed, " << unsupported
            << " unsupported). Run 'readstr refresh' to fetch them.\n";
    return 0;
}

int exportOpml(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {}, {}, options, err))
        return 1;
    if (options.positional.size() > 1)
        return fail(command, "takes at most one file", err);
    Store store;
    if (!openStore(command, store, err))
        return 1;
    const QList<Feed> list = store.feeds();
    if (!store.lastError().isEmpty())
        return fail(command, store.lastError(), err);
    QList<Opml::ExportFeed> exported;
    for (const Feed &feed : list)
        exported << Opml::ExportFeed{QStringLiteral("RSS"), feed.url, feed.title, feed.category, {}};
    const QByteArray opml = Opml::build(exported);

    if (options.positional.isEmpty()) {
        out << QString::fromUtf8(opml);
        return 0;
    }
    // Written beside the target and renamed over it, so a failure never
    // leaves half a file where the old one was. The subscription list is
    // private, so the file is 0600 before anything is written to it.
    QSaveFile file(options.positional.first());
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(opml) != opml.size() || !file.commit())
        return fail(command, QStringLiteral("cannot write %1: %2").arg(file.fileName(), file.errorString()), err);
    return 0;
}

int version(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {"--json"}, {}, options, err))
        return 1;
    if (!options.positional.isEmpty())
        return fail(command, "takes no arguments", err);
    if (options.json) {
        const QJsonObject object{{"name", "readstr"}, {"version", Cli::version}, {"qt", qVersion()}};
        out << QJsonDocument(object).toJson(QJsonDocument::Compact) << "\n";
    } else {
        out << "readstr " << Cli::version << "\n";
    }
    return 0;
}

int help(const QString &, const QStringList &, QTextStream &out, QTextStream &) {
    usage(out);
    return 0;
}

// Every headless command is one row here (docs/OMARCHY_APP_PLAN.md, section 5).
constexpr Command commands[] = {
    {"feeds", feeds},
    {"articles", articles},
    {"subscribe", subscribe},
    {"unsubscribe", unsubscribe},
    {"refresh", refresh},
    {"read", setFlag},
    {"unread", setFlag},
    {"star", setFlag},
    {"unstar", setFlag},
    {"import", importOpml},
    {"export", exportOpml},
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
    if (name == "open") {
        err << "readstr open: takes no arguments yet\n";
        return 1;
    }
    for (const auto &command : commands)
        if (name == QLatin1String(command.name))
            return command.handler(name, arguments.mid(1), out, err);
    err << "readstr: unknown command '" << name << "'\n";
    usage(err);
    return 1;
}

int finish(int code, QTextStream &out, QTextStream &err) {
    out.flush();
    if (out.status() == QTextStream::Ok)
        return code;
    err << "readstr: could not write the output\n";
    err.flush();
    return 1;
}
}
