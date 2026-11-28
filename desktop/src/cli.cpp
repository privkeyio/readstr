#include "cli.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <algorithm>

#include "discovery.h"
#include "feedparser.h"
#include "fetcher.h"
#include "library.h"
#include "savedviews.h"
#include "settings.h"
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
              "       readstr feeds category ID NAME|--none [--json]\n"
              "       readstr feeds tags ID LIST [--json]\n"
              "       readstr categories [--json]\n"
              "       readstr categories rename NAME NEW [--json]\n"
              "       readstr categories delete NAME [--json]\n"
              "       readstr tags [--json]\n"
              "       readstr tags rename TAG NEW [--json]\n"
              "       readstr tags delete TAG [--json]\n"
              "       readstr articles [--feed ID] [--view ID] [--unread] [--starred] [--limit N] [--json]\n"
              "       readstr search QUERY [--feed ID] [--view ID] [--unread] [--starred] [--limit N] [--json]\n"
              "       readstr views [--json]\n"
              "       readstr views add NAME [--icon ICON] [--source all|starred|tags] [--tags LIST]\n"
              "                         [--read all|unread|read] [--sort newest|oldest] [--include LIST] [--mute LIST]\n"
              "       readstr views edit ID [--name NAME] [the options of add]\n"
              "       readstr views delete ID\n"
              "       readstr views move ID up|down\n"
              "       readstr subscribe URL|NPUB [--json]\n"
              "       readstr unsubscribe ID\n"
              "       readstr refresh [--feed ID] [--json]\n"
              "       readstr read|unread|star|unstar ID...\n"
              "       readstr import FILE.opml [--json]\n"
              "       readstr export [FILE]\n"
              "       readstr sync [--json]\n"
              "       readstr sync login NPUB [--drop-views]\n"
              "       readstr sync logout\n"
              "       readstr filters [--json]\n"
              "       readstr settings [--json]\n"
              "       readstr settings set KEY VALUE\n"
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
            {"categoryColor", nullable(feed.categoryColor)},
            {"categoryIcon", nullable(feed.categoryIcon)},
            {"tags", QJsonArray::fromStringList(feed.tags)},
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

int organizeFeed(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err);

int feeds(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    const QString action = arguments.value(0);
    if (action == QLatin1String("category") || action == QLatin1String("tags"))
        return organizeFeed(command, arguments, out, err);
    Options options;
    if (!parseOptions(command, arguments, {"--json"}, {}, options, err))
        return 1;
    if (!options.positional.isEmpty())
        return fail(command, "takes no arguments, or category or tags", err);
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
        for (const QString &tag : feed.tags)
            out << "  #" << printable(tag);
        if (!feed.lastError.isEmpty())
            out << "  (error: " << printable(feed.lastError) << ")";
        out << "\n";
    }
    return 0;
}

// `articles`, and `search`, which takes the words to search for and lists
// the best matches first.
int articles(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {"--json", "--unread", "--starred"}, {"--feed", "--view", "--limit"}, options,
                      err))
        return 1;
    ArticleFilter filter;
    if (command == QLatin1String("search")) {
        if (options.positional.isEmpty())
            return fail(command, "needs words to search for", err);
        filter.query = options.positional.join(' ');
        if (filter.query.size() > Store::maxQueryLength)
            return fail(command, QStringLiteral("the query is longer than %1 characters").arg(Store::maxQueryLength),
                        err);
        if (Store::matchExpression(filter.query).isEmpty())
            return fail(command, "the query has no letters or digits to search for", err);
    } else if (!options.positional.isEmpty()) {
        return fail(command, "takes no arguments", err);
    }
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
    // A saved view's rules narrow the list further, as --unread and
    // --starred do; any it holds that cannot be applied are said on stderr.
    if (options.values.contains("--view")) {
        const QString id = options.values.value("--view");
        const QList<SavedView> views = store.savedViews();
        if (!store.lastError().isEmpty())
            return fail(command, store.lastError(), err);
        const auto view =
            std::find_if(views.begin(), views.end(), [&id](const SavedView &view) { return view.id == id; });
        if (view == views.end())
            return fail(command, QStringLiteral("no saved view has the id '%1'").arg(printable(id)), err);
        const SavedViews::Evaluation evaluation = SavedViews::evaluate(*view);
        if (evaluation.unknownSource)
            return fail(command,
                        QStringLiteral("the saved view '%1' targets a feed or category that cannot be "
                                       "identified here yet")
                            .arg(printable(id)),
                        err);
        // Unread articles of a view of read ones would be none at all.
        if (filter.unreadOnly && evaluation.filter.readOnly)
            return fail(command, QStringLiteral("--unread contradicts the saved view, which lists read articles"),
                        err);
        if (!evaluation.unsupported.isEmpty())
            err << "readstr " << command << ": not supported here, so left out: "
                << printable(evaluation.unsupported.join(", ")) << "\n";
        filter.unreadOnly = filter.unreadOnly || evaluation.filter.unreadOnly;
        filter.starredOnly = filter.starredOnly || evaluation.filter.starredOnly;
        filter.readOnly = evaluation.filter.readOnly;
        filter.oldestFirst = evaluation.filter.oldestFirst;
        filter.includeWords = evaluation.filter.includeWords;
        filter.excludeWords = evaluation.filter.excludeWords;
        filter.tags = evaluation.filter.tags;
    }
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
    if (!parseOptions(command, arguments, {"--json", "--drop-views"}, {}, options, err))
        return 1;
    const QString action = options.positional.value(0);
    const bool dropViews = options.flags.contains("--drop-views");
    if (dropViews && action != QLatin1String("login"))
        return fail(command, "--drop-views goes with login", err);
    if (action == QLatin1String("login")) {
        if (options.json || options.positional.size() != 2)
            return fail(command, "login needs one npub or nprofile", err);
        const auto account = Sync::account(options.positional.at(1));
        if (!account)
            return fail(command, "give an npub or nprofile", err);
        Store store;
        if (!openStore(command, store, err))
            return 1;
        // As the window asks first: what goes cannot be brought back.
        const std::optional<bool> others = store.savedViewsOfAnother(account->pubkey);
        if (!others)
            return fail(command, store.lastError(), err);
        const std::optional<bool> otherEdits = store.subscriptionEditsOfAnother(account->pubkey);
        if (!otherEdits)
            return fail(command, store.lastError(), err);
        if (*others && !dropViews)
            return fail(command,
                        "the saved views here belong to another npub, and their mute words and edits not "
                        "published yet would be lost; add --drop-views to remove them and sync with this one",
                        err);
        if (*otherEdits && !dropViews)
            return fail(command,
                        "the categories and tags here were changed under another npub, and those changes not "
                        "published yet would be lost; add --drop-views to remove them and sync with this one",
                        err);
        if (!store.setSyncAccount(account->npub, account->pubkey, account->relays, dropViews))
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

// Lists the settings, or sets one, which a running window picks up as it lands.
int settings(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {"--json"}, {}, options, err))
        return 1;
    Settings settings;
    if (!options.positional.isEmpty()) {
        if (options.positional.size() != 3 || options.positional.first() != "set" || options.json)
            return fail(command, "takes 'set KEY VALUE', or no arguments to list the settings", err);
        // A JSON value (true, false, a number, a quoted string), or else
        // the text as a string.
        const QString text = options.positional.at(2);
        const QJsonDocument parsed = QJsonDocument::fromJson("[" + text.toUtf8() + "]");
        const QJsonValue value =
            parsed.isArray() && parsed.array().size() == 1 ? parsed.array().first() : QJsonValue(text);
        QString error;
        if (!settings.set(options.positional.at(1), value, &error))
            return fail(command, error, err);
        return 0;
    }
    if (!settings.error().isEmpty())
        err << "readstr " << command << ": " << settings.error() << "; these are the defaults\n";
    const QJsonObject all = settings.all();
    if (options.json) {
        out << QJsonDocument(all).toJson(QJsonDocument::Compact) << "\n";
    } else {
        // Each value as JSON, as `set` takes it.
        for (auto it = all.constBegin(); it != all.constEnd(); ++it) {
            const QByteArray json = QJsonDocument(QJsonArray{*it}).toJson(QJsonDocument::Compact);
            out << it.key() << " " << QString::fromUtf8(json.mid(1).chopped(1)) << "\n";
        }
    }
    return settings.error().isEmpty() ? 0 : 1;
}

// The keyword filter rules, in the order they apply, as the window edits
// them; any the library holds that cannot be used are said on stderr.
int filters(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    Options options;
    if (!parseOptions(command, arguments, {"--json"}, {}, options, err))
        return 1;
    if (!options.positional.isEmpty())
        return fail(command, "takes no arguments", err);
    Store store;
    if (!openStore(command, store, err))
        return 1;
    QStringList dropped;
    const auto rules = store.filterRules(&dropped);
    if (!rules)
        return fail(command, store.lastError(), err);
    for (const QString &problem : std::as_const(dropped))
        err << "readstr " << command << ": left out " << printable(problem) << "\n";
    if (options.json) {
        QJsonArray array;
        for (const KeywordFilter::Rule &rule : *rules)
            array << KeywordFilter::toJson(rule, int(array.size()));
        printJson(array, out);
        return 0;
    }
    int position = 0;
    for (const KeywordFilter::Rule &rule : *rules) {
        out << ++position << "  " << (rule.enabled ? "on " : "off") << "  " << KeywordFilter::actionName(rule.action)
            << "  " << KeywordFilter::targetName(rule.target) << " " << KeywordFilter::typeName(rule.type)
            << (rule.caseSensitive ? " (case sensitive)" : "") << "  " << printable(rule.pattern) << "\n";
    }
    return 0;
}

// A saved view changed here is marked to publish in the same transaction
// (SavedViews::save()), so the window publishes it when it next starts, as
// a subscription changed here is.
int changeViews(const QString &command, const std::function<QString(QList<SavedView> &)> &change,
                QTextStream &err) {
    Store store;
    if (!openStore(command, store, err))
        return 1;
    QString problem;
    const int saved = SavedViews::save(store, change, &problem);
    if (saved < 0)
        return fail(command, store.lastError(), err);
    return saved == 0 ? fail(command, problem, err) : 0;
}

// `views add NAME` and `views edit ID`, whose options change only what
// they name. Lists are comma-separated, as in the window's dialog. The view
// is worked out inside the write, from the views as they are then, so an
// edit never puts back what a pull or the window changed since.
int saveView(const QString &command, const QString &action, const Options &options, QTextStream &out,
             QTextStream &err) {
    const bool edit = action == QLatin1String("edit");
    if (options.positional.size() != 2)
        return fail(command, edit ? "edit needs a view id" : "add needs a name", err);
    const QString target = options.positional.at(1);
    const QHash<QString, QString> &values = options.values;
    QVariantMap fields;
    if (!edit)
        fields.insert("name", target);
    using Field = std::pair<const char *, const char *>;
    for (const auto &[option, field] : std::initializer_list<Field>{{"--name", "name"},     {"--icon", "icon"},
                                                                    {"--read", "readState"}, {"--sort", "sort"},
                                                                    {"--include", "include"}, {"--mute", "exclude"}})
        if (values.contains(option))
            fields.insert(field, values.value(option));
    if (values.contains("--source")) {
        const QString source = values.value("--source");
        fields.insert("source", source == QLatin1String("starred") ? QStringLiteral("favorites") : source);
    }
    if (values.contains("--tags"))
        fields.insert("tags", SavedViews::words(values.value("--tags")));
    std::optional<SavedView> saved;
    const int code = changeViews(command, [&](QList<SavedView> &list) -> QString {
        const qsizetype at = edit ? SavedViews::indexOf(list, target) : -1;
        if (edit && at < 0)
            return QStringLiteral("no saved view has the id '%1'").arg(target);
        const std::optional<SavedView> base = edit ? std::optional(list.at(at)) : std::nullopt;
        const SavedViews::Draft draft = SavedViews::edited(base ? &*base : nullptr, fields);
        // Tags would be dropped unsaid on a view that lists something else.
        const QString kind = draft.source.isEmpty() && base ? SavedViews::draftOf(*base).source : draft.source;
        if (fields.contains("tags") && kind != QLatin1String("tags"))
            return QStringLiteral("--tags needs a view of tags; add --source tags");
        QString error;
        saved = SavedViews::build(draft, base ? &*base : nullptr, &error);
        if (!saved)
            return error;
        return base ? SavedViews::replace(list, *saved) : SavedViews::add(list, *saved);
    }, err);
    if (code == 0 && saved)
        out << (edit ? "Saved view " : "Created view ") << printable(saved->id) << "  "
            << printable(SavedViews::name(*saved)) << "\n";
    return code;
}

// The saved views, in the web app's order, each with the rules it holds
// that cannot be applied here; `articles --view ID` lists one's articles.
// With `add`, `edit`, `delete`, or `move`, changes them as the window does.
int views(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    const QString action = arguments.value(0);
    if (action == QLatin1String("add") || action == QLatin1String("edit")) {
        Options options;
        QStringList valued{"--icon", "--source", "--tags", "--read", "--sort", "--include", "--mute"};
        if (action == QLatin1String("edit"))
            valued << "--name";
        if (!parseOptions(command, arguments, {}, valued, options, err))
            return 1;
        return saveView(command, action, options, out, err);
    }
    if (action == QLatin1String("delete")) {
        if (arguments.size() != 2)
            return fail(command, "delete needs a view id", err);
        const QString id = arguments.at(1);
        return changeViews(command, [&id](QList<SavedView> &list) { return SavedViews::remove(list, id); }, err);
    }
    if (action == QLatin1String("move")) {
        const QString id = arguments.value(1), direction = arguments.value(2);
        if (arguments.size() != 3 || (direction != QLatin1String("up") && direction != QLatin1String("down")))
            return fail(command, "move needs a view id and up or down", err);
        const int step = direction == QLatin1String("up") ? -1 : 1;
        return changeViews(command, [&id, step](QList<SavedView> &list) {
            return SavedViews::move(list, id, SavedViews::indexOf(list, id) + step);
        }, err);
    }
    Options options;
    if (!parseOptions(command, arguments, {"--json"}, {}, options, err))
        return 1;
    if (!options.positional.isEmpty())
        return fail(command, "takes no arguments, or add, edit, delete, or move", err);
    Store store;
    if (!openStore(command, store, err))
        return 1;
    const QList<SavedView> list = store.savedViews();
    if (!store.lastError().isEmpty())
        return fail(command, store.lastError(), err);
    if (options.json) {
        QJsonArray array;
        for (const SavedView &view : list) {
            QJsonObject object = view.view;
            object.insert("id", view.id);
            object.insert("exclude", QJsonArray::fromStringList(view.exclude));
            const SavedViews::Evaluation evaluation = SavedViews::evaluate(view);
            object.insert("unsupported", QJsonArray::fromStringList(evaluation.unsupported));
            object.insert("listable", !evaluation.unknownSource);
            array << object;
        }
        printJson(array, out);
        return 0;
    }
    for (const SavedView &view : list) {
        const QJsonObject source = view.view.value("source").toObject();
        out << printable(view.id) << "  " << printable(SavedViews::name(view)) << "  "
            << printable(source.value("kind").toString("all"));
        const SavedViews::Evaluation evaluation = SavedViews::evaluate(view);
        if (!evaluation.filter.tags.isEmpty())
            out << " (" << printable(evaluation.filter.tags.join(", ")) << ")";
        out << ", "
            << printable(view.view.value("readState").toString("all")) << ", "
            << printable(view.view.value("sort").toString("newest"));
        if (evaluation.unknownSource)
            out << "  (lists nothing here: its feed or category cannot be identified yet)";
        else if (!evaluation.unsupported.isEmpty())
            out << "  (not supported here: " << printable(evaluation.unsupported.join(", ")) << ")";
        out << "\n";
    }
    return 0;
}

// `feeds category ID NAME|--none` and `feeds tags ID LIST`, which change
// one feed as the window's "Move to category…" and "Tags…" do. Each change
// is saved with the mark to publish the subscription list in one
// transaction (Store::setFeedCategory()), so the window publishes it when it
// next starts, as a subscription changed here is.
int organizeFeed(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    const QString action = arguments.value(0);
    const bool category = action == QLatin1String("category");
    Options options;
    if (!parseOptions(command, arguments.mid(1), category ? QStringList{"--json", "--none"} : QStringList{"--json"}, {},
                      options, err))
        return 1;
    const bool none = options.flags.contains("--none");
    if (category && options.positional.size() != (none ? 1 : 2))
        return fail(command, "category needs a feed id and a category name, or --none", err);
    if (!category && options.positional.size() != 2)
        return fail(command, "tags needs a feed id and a comma-separated list of tags (\"\" for none)", err);
    qint64 id = 0;
    if (!parseId(command, options.positional.first(), id, err))
        return 1;
    Store store;
    if (!openStore(command, store, err))
        return 1;
    const QString value = options.positional.value(1);
    if (!(category ? store.setFeedCategory(id, none ? QString() : value) : store.setFeedTagList(id, value)))
        return fail(command, store.lastError(), err);
    const std::optional<Feed> feed = store.feed(id);
    if (!feed)
        return fail(command, store.lastError(), err);
    if (options.json)
        printJson(feedJson(*feed), out);
    else if (category && feed->category.isEmpty())
        out << printable(feed->title) << " has no category\n";
    else if (category)
        out << printable(feed->title) << " is in " << printable(feed->category) << "\n";
    else
        out << "Tags of " << printable(feed->title) << ": " << printable(Sync::formatTagList(feed->tags)) << "\n";
    return 0;
}

// `categories rename NAME NEW` and `delete NAME`, and `tags rename TAG NEW`
// and `delete TAG`, as the window's menus have them, with what changed.
int organize(const QString &command, const QString &action, const QStringList &arguments, QTextStream &out,
             QTextStream &err) {
    const bool tags = command == QLatin1String("tags"), rename = action == QLatin1String("rename");
    Options options;
    if (!parseOptions(command, arguments.mid(1), {"--json"}, {}, options, err))
        return 1;
    if (options.positional.size() != (rename ? 2 : 1))
        return fail(command, rename ? QStringLiteral("rename needs a name and the new one")
                                    : QStringLiteral("delete needs a name"),
                    err);
    const QString name = options.positional.at(0), to = options.positional.value(1).trimmed();
    Store store;
    if (!openStore(command, store, err))
        return 1;
    OrganizeChange change;
    bool done = false;
    if (tags && rename)
        done = store.renameTag(name, to, &change);
    else if (tags)
        done = store.deleteTag(name, &change);
    else if (rename)
        done = store.renameCategory(name, to, &change);
    else
        done = store.deleteCategory(name, &change);
    if (!done)
        return fail(command, store.lastError(), err);
    if (options.json) {
        QJsonObject object = rename ? QJsonObject{{"from", name}, {"to", to}} : QJsonObject{{"name", name}};
        object.insert("feeds", change.feeds);
        if (!tags && rename)
            object.insert("merged", change.merged);
        printJson(object, out);
    } else if (tags) {
        out << (rename ? "Renamed the tag " + printable(name) + " to " + printable(to) : "Deleted the tag " + printable(name))
            << " on " << change.feeds << " feeds\n";
    } else if (rename) {
        out << (change.merged ? "Merged the category " : "Renamed the category ") << printable(name)
            << (change.merged ? " into " : " to ") << printable(to) << " (" << change.feeds << " feeds)\n";
    } else {
        out << "Deleted the category " << printable(name) << " (" << change.feeds << " feeds left without one)\n";
    }
    return 0;
}

// The categories the feeds are in, with their color, icon, and counts, by
// name, or with `rename` or `delete`, changes one.
int categories(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    const QString action = arguments.value(0);
    if (action == QLatin1String("rename") || action == QLatin1String("delete"))
        return organize(command, action, arguments, out, err);
    Options options;
    if (!parseOptions(command, arguments, {"--json"}, {}, options, err))
        return 1;
    if (!options.positional.isEmpty())
        return fail(command, "takes no arguments, or rename or delete", err);
    Store store;
    if (!openStore(command, store, err))
        return 1;
    const QList<Feed> list = store.feeds();
    if (!store.lastError().isEmpty())
        return fail(command, store.lastError(), err);
    struct Category {
        QString color, icon;
        int feeds = 0, unread = 0;
    };
    QMap<QString, Category> byName;
    for (const Feed &feed : list) {
        if (feed.category.isEmpty())
            continue;
        Category &category = byName[feed.category];
        category.color = feed.categoryColor;
        category.icon = feed.categoryIcon;
        ++category.feeds;
        category.unread += feed.unread;
    }
    QStringList names = byName.keys();
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        const int order = a.compare(b, Qt::CaseInsensitive);
        return order != 0 ? order < 0 : a < b;
    });
    QJsonArray array;
    for (const QString &name : std::as_const(names)) {
        const Category &category = byName[name];
        if (options.json) {
            array << QJsonObject{{"name", name},
                                 {"color", nullable(category.color)},
                                 {"icon", nullable(category.icon)},
                                 {"feeds", category.feeds},
                                 {"unread", category.unread}};
            continue;
        }
        out << printable(name) << "  " << category.feeds << " feeds, " << category.unread << " unread";
        if (!category.color.isEmpty())
            out << "  " << category.color;
        if (!category.icon.isEmpty())
            out << "  " << printable(category.icon);
        out << "\n";
    }
    if (options.json)
        printJson(array, out);
    return 0;
}

// The tags the feeds carry, each by the name it shows as (Sync::tagName())
// with its spellings and counts, or with `rename` or `delete`, changes one
// on every feed.
int tags(const QString &command, const QStringList &arguments, QTextStream &out, QTextStream &err) {
    const QString action = arguments.value(0);
    if (action == QLatin1String("rename") || action == QLatin1String("delete"))
        return organize(command, action, arguments, out, err);
    Options options;
    if (!parseOptions(command, arguments, {"--json"}, {}, options, err))
        return 1;
    if (!options.positional.isEmpty())
        return fail(command, "takes no arguments, or rename or delete", err);
    Store store;
    if (!openStore(command, store, err))
        return 1;
    const QList<Feed> list = store.feeds();
    if (!store.lastError().isEmpty())
        return fail(command, store.lastError(), err);
    struct Tag {
        QStringList spellings;
        // The spellings again, so a library of many feeds with many tags
        // each is not searched once per tag.
        QSet<QString> spelled;
        int feeds = 0, unread = 0;
    };
    QMap<QString, Tag> byName;
    for (const Feed &feed : list) {
        QSet<QString> counted;
        for (const QString &spelling : feed.tags) {
            const QString name = Sync::tagName(spelling);
            if (name.isEmpty())
                continue;
            Tag &tag = byName[name];
            if (!tag.spelled.contains(spelling)) {
                tag.spelled.insert(spelling);
                tag.spellings << spelling;
            }
            if (!counted.contains(name)) {
                counted.insert(name);
                ++tag.feeds;
                tag.unread += feed.unread;
            }
        }
    }
    QJsonArray array;
    for (auto it = byName.begin(); it != byName.end(); ++it) {
        std::sort(it->spellings.begin(), it->spellings.end());
        if (options.json) {
            array << QJsonObject{{"name", it.key()},
                                 {"spellings", QJsonArray::fromStringList(it->spellings)},
                                 {"feeds", it->feeds},
                                 {"unread", it->unread}};
            continue;
        }
        out << "#" << printable(it.key()) << "  " << it->feeds << " feeds, " << it->unread << " unread\n";
    }
    if (options.json)
        printJson(array, out);
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
    {"search", articles},
    {"views", views},
    {"categories", categories},
    {"tags", tags},
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
    {"filters", filters},
    {"settings", settings},
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
