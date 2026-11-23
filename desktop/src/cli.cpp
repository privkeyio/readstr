#include "cli.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include "discovery.h"
#include "feedparser.h"
#include "fetcher.h"
#include "library.h"
#include "store.h"
#include "sync.h"

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
              "       readstr subscribe URL|NPUB [--json]\n"
              "       readstr unsubscribe ID\n"
              "       readstr refresh [--feed ID] [--json]\n"
              "       readstr read|unread|star|unstar ID...\n"
              "       readstr import FILE.opml [--json]\n"
              "       readstr export [FILE]\n"
              "       readstr sync [--json]\n"
              "       readstr sync login NPUB\n"
              "       readstr sync logout\n"
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
        else if (FeedParser::isBidiFormat(character))
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

// A subscription changed here is marked for the sync account, so the window
// publishes it when it next starts. Best effort while a window is open: a
// publish it has under way, built before this change, clears the mark, and
// the change then goes out with the next subscription change.
void markSubscriptionsPending(const QString &command, Store &store, QTextStream &err) {
    const QString pubkey = store.syncAccount().pubkey;
    if (!pubkey.isEmpty() && !store.markPublishPending(pubkey, publishBit(Sync::subscriptionsKind), true))
        err << "readstr " << command << ": cannot remember the change to publish: " << printable(store.lastError())
            << "\n";
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
        return fail(command, "needs one URL or npub", err);
    const QString text = options.positional.first().trimmed();
    const auto author = Library::nostrAuthor(text);
    const QUrl url(text, QUrl::StrictMode);
    if (!author && !Library::isWebUrl(url))
        return fail(command, "give a URL starting with http:// or https://, or an npub", err);

    Store store;
    if (!openStore(command, store, err))
        return 1;
    Library::Subscription subscription;
    bool discovered = false;
    if (author) {
        RelayPool relays;
        const QList<NostrQuery> queries = Library::nostrQueries(*author);
        QList<NostrResult> found(queries.size());
        relays.queryEach(queries, [&found](qsizetype i, const NostrResult &result) { found[i] = result; });
        subscription = Library::subscribeNostr(store, *author, found.at(0), found.at(1));
    } else {
        Fetcher fetcher;
        const Discovery::Result found = Discovery::discover(fetcher, url);
        if (!found.found)
            return fail(command, found.error, err);
        discovered = found.discovered;
        subscription = Library::subscribe(store, found);
    }
    if (!subscription.error.isEmpty())
        return fail(command, subscription.error, err);
    const bool created = subscription.created;
    const int added = subscription.added;
    if (created)
        markSubscriptionsPending(command, store, err);
    const std::optional<Feed> feed = store.feed(subscription.id);
    if (!feed)
        return fail(command, store.lastError(), err);

    if (options.json) {
        printJson(QJsonObject{{"id", feed->id},
                              {"url", feed->url},
                              {"title", feed->title},
                              {"created", created},
                              {"discovered", discovered},
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
    markSubscriptionsPending(command, store, err);
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
    QList<NostrQuery> queries;
    QList<qsizetype> web, nostr;
    for (qsizetype i = 0; i < targets.size(); ++i) {
        const Feed &feed = targets.at(i);
        if (Library::isNostrFeed(feed)) {
            nostr << i;
            queries << Library::nostrQueries(feed);
        } else {
            web << i;
            requests << FetchRequest{QUrl(feed.url), feed.etag, feed.lastModified};
        }
    }

    // Each response is parsed and saved as it arrives, so only the few in
    // flight hold a body; the rows are printed in feed order at the end.
    QList<Library::Refresh> rows(targets.size());
    Fetcher fetcher;
    fetcher.fetchEach(requests, [&](qsizetype i, const FetchResult &fetched) {
        rows[web.at(i)] = Library::apply(store, targets.at(web.at(i)), fetched);
    });
    // A Nostr feed is saved once both its queries have answered.
    RelayPool relays;
    Library::NostrPairs pairs;
    relays.queryEach(queries, [&](qsizetype i, const NostrResult &found) {
        if (const auto both = pairs.take(i, found)) {
            const qsizetype row = nostr.at(i / 2);
            rows[row] = Library::applyNostr(store, targets.at(row), both->first, both->second);
        }
    });

    int code = 0;
    QJsonArray array;
    for (qsizetype i = 0; i < targets.size(); ++i) {
        const Feed &feed = targets.at(i);
        const Library::Refresh &row = rows.at(i);
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
    // A file that cannot be read or parsed is refused before the library is
    // opened, which would create it.
    QString error;
    const auto outlines = Library::readOpml(options.positional.first(), &error);
    if (!outlines)
        return fail(command, error, err);
    Store store;
    if (!openStore(command, store, err))
        return 1;
    const Library::Import imported = Library::importOutlines(store, *outlines);
    if (!imported.error.isEmpty())
        return fail(command, imported.error, err);
    if (imported.added > 0)
        markSubscriptionsPending(command, store, err);

    if (options.json)
        printJson(QJsonObject{{"added", imported.added}, {"skipped", imported.skipped}, {"unsupported", imported.unsupported}},
                  out);
    else
        out << "Imported " << imported.added << " feeds (" << imported.skipped << " already subscribed, "
            << imported.unsupported
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
    QString error;
    const QByteArray opml = Library::exportOpml(store, &error);
    if (!error.isEmpty())
        return fail(command, error, err);

    if (options.positional.isEmpty()) {
        out << QString::fromUtf8(opml);
        return 0;
    }
    if (!Library::writePrivately(options.positional.first(), opml, &error))
        return fail(command, error, err);
    return 0;
}

int sync(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {"--json"}, {}, options, err))
        return 1;
    const QString action = options.positional.value(0);
    if (action == QLatin1String("login")) {
        if (options.json || options.positional.size() != 2)
            return fail(command, "login needs one npub or nprofile", err);
        const auto account = Sync::account(options.positional.at(1));
        if (!account)
            return fail(command, "give an npub or nprofile", err);
        Store store;
        if (!openStore(command, store, err))
            return 1;
        if (!store.setSyncAccount(account->npub, account->pubkey, account->relays))
            return fail(command, store.lastError(), err);
        out << "Syncing with " << account->npub << ". Run 'readstr sync' to pull now.\n";
        return 0;
    }
    if (action == QLatin1String("logout")) {
        if (options.json || options.positional.size() != 1)
            return fail(command, "logout takes no arguments", err);
        Store store;
        if (!openStore(command, store, err))
            return 1;
        const SyncAccount account = store.syncAccount();
        if (!store.lastError().isEmpty() || !store.clearSyncAccount())
            return fail(command, store.lastError(), err);
        if (account.npub.isEmpty())
            out << "Not syncing with any npub\n";
        else
            out << "Stopped syncing with " << account.npub << "\n";
        return 0;
    }
    if (!options.positional.isEmpty())
        return fail(command, "unknown action '" + action + "' (login, logout, or none to pull)", err);

    Store store;
    if (!openStore(command, store, err))
        return 1;
    const SyncAccount account = store.syncAccount();
    if (!store.lastError().isEmpty())
        return fail(command, store.lastError(), err);
    if (account.pubkey.isEmpty())
        return fail(command, "not syncing with any npub; run 'readstr sync login NPUB' first", err);
    RelayPool relays;
    relays.setLimits(Sync::limits());
    const Sync::Outcome outcome = Sync::apply(store, account, relays.query(Sync::query(account)));
    QStringList errors = outcome.problems;
    if (!outcome.error.isEmpty())
        errors.prepend(outcome.error);
    for (const QString &error : std::as_const(errors))
        err << "readstr " << command << ": " << printable(error) << "\n";

    if (options.json) {
        const SyncAccount after = store.syncAccount();
        printJson(QJsonObject{{"npub", account.npub},
                              {"added", outcome.added},
                              {"skipped", outcome.skipped},
                              {"unsupported", outcome.unsupported},
                              {"markedRead", outcome.markedRead},
                              {"pendingRead", outcome.pendingRead},
                              {"views", outcome.views},
                              {"syncedAt", nullable(after.syncedAt)},
                              {"errors", QJsonArray::fromStringList(errors)}},
                  out);
    } else if (outcome.error.isEmpty()) {
        out << "Synced with " << account.npub << ": " << outcome.added << " feeds added (" << outcome.skipped
            << " already subscribed, " << outcome.unsupported << " unsupported), " << outcome.markedRead
            << " articles marked read, " << outcome.pendingRead << " read GUIDs waiting for their articles, "
            << outcome.views << " saved views\n";
        if (outcome.added > 0)
            out << "Run 'readstr refresh' to fetch the new feeds.\n";
    }
    return errors.isEmpty() ? 0 : 1;
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
    {"sync", sync},
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
