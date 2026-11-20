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

int fail(const QString &command, const QString &message, QTextStream &err) {
    err << "readstr " << command << ": " << message << "\n";
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
        out << feed.id << "  " << feed.unread << "/" << feed.total << "  " << feed.title << "  " << feed.url;
        if (!feed.category.isEmpty())
            out << "  [" << feed.category << "]";
        if (!feed.lastError.isEmpty())
            out << "  (error: " << feed.lastError << ")";
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
            << "  " << (article.read ? ' ' : 'N') << (article.starred ? '*' : ' ') << "  " << article.title << "  ("
            << article.feedTitle << ")\n";
        if (!article.url.isEmpty())
            out << "    " << article.url << "\n";
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
            || !store.recordFetch(id, found.fetch.etag, found.fetch.lastModified, {}))
            return fail(command, store.lastError(), err);
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
        out << "Subscribed to " << feed->title << " (id " << feed->id << ", " << added << " articles)\n";
    } else {
        out << "Already subscribed to " << feed->title << " (id " << feed->id << ")\n";
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
    out << "Unsubscribed from " << feed->title << "\n";
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
    Fetcher fetcher;
    const QList<FetchResult> results = fetcher.fetchAll(requests);

    int code = 0;
    QJsonArray array;
    for (qsizetype i = 0; i < targets.size(); ++i) {
        const Feed &feed = targets.at(i);
        const FetchResult &fetched = results.at(i);
        QString status, error, title = feed.title;
        int added = 0;
        if (!fetched.ok()) {
            error = fetched.error;
        } else if (fetched.notModified) {
            status = QStringLiteral("not-modified");
            // A 304 may leave out the validators; the stored ones still hold.
            if (!store.recordFetch(feed.id, fetched.etag.isEmpty() ? feed.etag : fetched.etag,
                                   fetched.lastModified.isEmpty() ? feed.lastModified : fetched.lastModified, {}))
                error = store.lastError();
        } else if (const auto parsed = FeedParser::parse(fetched.body, fetched.finalUrl, &error)) {
            status = QStringLiteral("updated");
            added = store.saveItems(feed.id, parsed->items);
            if (added < 0 || !store.updateFeedDetails(feed.id, *parsed)
                || !store.recordFetch(feed.id, fetched.etag, fetched.lastModified, {}))
                error = store.lastError();
            if (const auto updated = store.feed(feed.id))
                title = updated->title;
        }
        if (!error.isEmpty()) {
            status = QStringLiteral("error");
            added = 0;
            store.recordFetch(feed.id, {}, {}, error);
            err << "readstr " << command << ": " << title << ": " << error << "\n";
            code = 1;
        }
        if (options.json) {
            array << QJsonObject{{"id", feed.id},       {"url", feed.url},  {"title", title},
                                 {"status", status},   {"new", added},     {"error", nullable(error)}};
        } else if (status == QLatin1String("updated")) {
            out << feed.id << "  " << title << ": " << added << " new\n";
        } else if (status == QLatin1String("not-modified")) {
            out << feed.id << "  " << title << ": not modified\n";
        } else {
            out << feed.id << "  " << title << ": failed\n";
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

    Store store;
    if (!openStore(command, store, err))
        return 1;
    QStringList existing;
    for (const Feed &feed : store.feeds())
        existing << feed.url;
    const Opml::Plan plan = Opml::plan(outlines, existing);

    int added = 0, unsupported = 0;
    for (const Opml::Outline &outline : plan.toAdd) {
        // Nostr sources arrive in phase 2; until then they are counted, not dropped silently.
        if (!outline.npub.isEmpty() || !isWebUrl(QUrl(outline.xmlUrl, QUrl::StrictMode))) {
            ++unsupported;
            continue;
        }
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
    // leaves half a file where the old one was.
    QSaveFile file(options.positional.first());
    if (!file.open(QIODevice::WriteOnly) || file.write(opml) != opml.size() || !file.commit())
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
