#include "relaypool.h"

#include <QDateTime>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QWebSocket>

#include "feedparser.h"
#include "fetcher.h"

QList<QJsonObject> NostrQuery::filters() const {
    QJsonArray kindArray;
    for (int kind : kinds)
        kindArray << kind;
    QJsonObject object{{"authors", QJsonArray{author}}, {"kinds", kindArray}, {"limit", limit}};
    if (since > 0)
        object.insert("since", since);
    if (dTags.isEmpty())
        return {object};
    // One filter per pair, each with its own limit, so the old versions a
    // relay keeps of one pair cannot use up the limit of another.
    QList<QJsonObject> perPair;
    for (const auto &[kind, dTag] : dTags) {
        QJsonObject pair = object;
        pair.insert("kinds", QJsonArray{kind});
        pair.insert("#d", QJsonArray{dTag});
        perPair << pair;
    }
    return perPair;
}

namespace {
// NIP-01: kinds 0, 3, and 10000-19999 keep one event per author and kind,
// and 30000-39999 one per author, kind, and d tag.
QString replaceableKey(const NostrEvent &event) {
    const int kind = event.kind;
    if (kind == 0 || kind == 3 || (kind >= 10000 && kind < 20000))
        return QStringLiteral("%1:%2").arg(kind).arg(event.pubkey);
    if (kind >= 30000 && kind < 40000)
        return QStringLiteral("%1:%2:%3").arg(kind).arg(event.pubkey, event.tag(QStringLiteral("d")));
    return {};
}
}

struct RelayPool::Batch {
    struct Query {
        NostrQuery query;
        QStringList relays;
        int done = 0;
        int answered = 0;
        int unreachable = 0;
        QStringList errors;
        // By id, or for a replaceable event by its address, holding only
        // the newest version.
        QHash<QString, NostrEvent> events;
        // By address, the newest verified event passed over for being past
        // the query's maxContentLength, without its content.
        QHash<QString, NostrEvent> oversized;
        qint64 bytes = 0;
        bool truncated = false;
        // A relay hint was left out at the connection cap.
        bool deferred = false;
        bool delivered = false;
        QPointer<QTimer> grace;
        QDateTime startedAt;
    };
    QList<Query> queries;
    std::function<void(qsizetype, const NostrResult &, const Release &)> each;
    std::function<void()> finished;
    QHash<QString, QPointer<Connection>> connections;
    int next = 0;
    int open = 0;
    int released = 0;
    bool over = false;
};

// One relay's WebSocket for one batch: subscriptions go out once it is
// connected, and each ends at EOSE, CLOSED, its deadline, the socket's end,
// or its query finishing elsewhere, reporting to the pool at most once. A
// connection that drops is opened again a couple of times, for the queries
// still waiting; one that sends a forged event, or never answers at all, is
// given up on for the rest of the batch.
class RelayPool::Connection : public QObject {
public:
    Connection(const QString &relay, RelayPool *pool, const std::shared_ptr<Batch> &batch)
        : QObject(pool), m_relay(relay), m_pool(pool), m_batch(batch), m_limits(pool->limits()) {
        m_socket.setParent(this);
        m_socket.setMaxAllowedIncomingMessageSize(quint64(m_limits.maxMessageBytes));
        m_socket.setMaxAllowedIncomingFrameSize(quint64(m_limits.maxMessageBytes));
        m_connectTimer.setSingleShot(true);
        connect(&m_connectTimer, &QTimer::timeout, this, [this] { fail(QStringLiteral("timed out connecting")); });
        connect(&m_socket, &QWebSocket::connected, this, [this] {
            m_connectTimer.stop();
            m_connected = true;
            m_reached = true;
            const QList<int> queued = std::exchange(m_queued, {});
            for (int query : queued)
                subscribe(query);
        });
        connect(&m_socket, &QWebSocket::disconnected, this, &Connection::dropped);
        connect(&m_socket, &QWebSocket::errorOccurred, this, [this] {
            if (!m_connected)
                fail(m_socket.errorString());
        });
        connect(&m_socket, &QWebSocket::textMessageReceived, this, &Connection::received);
        open();
    }

    ~Connection() override { detach(); }

    // Lets go of the pool for good. Deleted from the event loop rather than
    // with the pool, since a callback may delete the pool from inside one of
    // this connection's own signals.
    void retire() {
        detach();
        setParent(nullptr);
        deleteLater();
    }

    // Stops every callback into the pool, which may be going away, and
    // closes the socket after what is queued on it (the CLOSEs) has gone.
    void detach() {
        m_pool = nullptr;
        m_socket.disconnect(this);
        m_connectTimer.stop();
        if (m_connected) {
            m_socket.flush();
            m_socket.close();
        } else {
            m_socket.abort();
        }
    }

    void add(int query) {
        if (!m_failure.isEmpty()) {
            // Reported from the event loop, so a pump that adds to a failed
            // relay does not recurse into the next delivery.
            QMetaObject::invokeMethod(
                this, [this, query] { report(query, m_failure); }, Qt::QueuedConnection);
        } else if (m_connected) {
            subscribe(query);
        } else {
            m_queued << query;
        }
    }

    // The query finished without this relay: its subscription closes and
    // reports nothing. A relay that has answered nothing in this batch by
    // then is not going to, so it is given up on.
    void cancel(int query) {
        if (m_queued.removeAll(query))
            return;
        for (auto it = m_subscriptions.begin(); it != m_subscriptions.end(); ++it) {
            if (it->query != query)
                continue;
            send(QJsonArray{"CLOSE", it.key()});
            it->timer->stop();
            it->timer->deleteLater();
            m_subscriptions.erase(it);
            if (!m_answered)
                QMetaObject::invokeMethod(
                    this,
                    [this] {
                        if (!m_answered)
                            fail(QStringLiteral("too slow to answer"));
                    },
                    Qt::QueuedConnection);
            return;
        }
    }

private:
    struct Subscription {
        int query = 0;
        QTimer *timer = nullptr;
        // An event or answer came for it.
        bool heard = false;
    };

    void open() {
        m_connected = false;
        m_connectTimer.start(m_limits.connectTimeoutMs);
        QNetworkRequest request{QUrl(m_relay)};
        request.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("readstr-desktop/" READSTR_VERSION));
        m_socket.open(request);
    }

    void subscribe(int query) {
        // Random, so the ids say nothing about which app asks or how often.
        const QString id = QString::number(QRandomGenerator::global()->generate64(), 36);
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, id] {
            if (!m_subscriptions.contains(id))
                return;
            send(QJsonArray{"CLOSE", id});
            finish(id, QStringLiteral("timed out"));
            // A relay that sits on a request will sit on the next one too.
            fail(QStringLiteral("timed out"));
        });
        timer->start(m_limits.subscriptionTimeoutMs);
        m_subscriptions.insert(id, {query, timer});
        QJsonArray request{"REQ", id};
        for (const QJsonObject &filter : m_batch->queries.at(query).query.filters())
            request << filter;
        send(request);
    }

    void send(const QJsonArray &message) {
        if (m_connected)
            m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }

    void received(const QString &text) {
        QByteArray bytes = text.toUtf8();
        // QJsonDocument drops a byte order mark that starts a string, which
        // would change the event's hash. Valid JSON has one only inside a
        // string, where the escape means the same.
        bytes.replace("\xEF\xBB\xBF", "\\ufeff");
        const QJsonArray message = QJsonDocument::fromJson(bytes).array();
        bytes.clear();
        if (message.size() < 2 || !message.at(0).isString() || !message.at(1).isString())
            return;
        const QString type = message.at(0).toString(), id = message.at(1).toString();
        const auto subscription = m_subscriptions.find(id);
        if (subscription == m_subscriptions.end())
            return;
        subscription->heard = true;
        if (type == QLatin1String("EVENT") && message.size() >= 3) {
            const auto event = NostrEvent::fromJson(message.at(2));
            if (event && m_pool && !m_pool->accept(m_batch, subscription->query, *event)) {
                m_hostile = true;
                fail(QStringLiteral("sent an event with a bad id or signature"));
            }
        } else if (type == QLatin1String("EOSE")) {
            m_answered = true;
            send(QJsonArray{"CLOSE", id});
            finish(id, {});
        } else if (type == QLatin1String("CLOSED")) {
            m_answered = true;
            const QString reason =
                message.size() >= 3 ? FeedParser::cleanText(message.at(2).toString().left(400)).left(200) : QString();
            finish(id, reason.isEmpty() ? QStringLiteral("closed the subscription") : reason);
        }
    }

    void finish(const QString &id, const QString &error) {
        const auto subscription = m_subscriptions.take(id);
        if (!subscription.timer)
            return;
        subscription.timer->stop();
        subscription.timer->deleteLater();
        report(subscription.query, error);
    }

    // The relay closed the connection or it broke. Subscriptions it had
    // already answered in part fail, since asking again might bring the same
    // message that broke it; the rest, and the queries still waiting, go out
    // again on a new connection, a couple of times.
    void dropped() {
        if (!m_failure.isEmpty())
            return;
        if (!m_connected || m_hostile || m_reconnects >= maxReconnects) {
            fail(m_socket.closeReason().isEmpty() ? QStringLiteral("connection closed") : m_socket.closeReason());
            return;
        }
        ++m_reconnects;
        m_connected = false;
        const auto subscriptions = std::exchange(m_subscriptions, {});
        for (const Subscription &subscription : subscriptions) {
            subscription.timer->stop();
            subscription.timer->deleteLater();
            if (subscription.heard)
                report(subscription.query, QStringLiteral("connection closed"));
            else
                m_queued << subscription.query;
        }
        // Not from inside the socket's own signal, and not once the pool
        // (perhaps deleted by a report above) has let go of this connection.
        QTimer::singleShot(0, this, [this] {
            if (m_pool && m_failure.isEmpty() && !m_connected)
                open();
        });
    }

    void fail(const QString &error) {
        if (!m_failure.isEmpty())
            return;
        m_failure = FeedParser::cleanText(error.left(400)).left(200);
        if (m_failure.isEmpty())
            m_failure = QStringLiteral("connection failed");
        m_connectTimer.stop();
        // Nothing more is wanted from this relay in this batch.
        m_socket.disconnect(this);
        m_socket.abort();
        m_connected = false;
        const QList<int> queued = std::exchange(m_queued, {});
        const auto subscriptions = std::exchange(m_subscriptions, {});
        for (const Subscription &subscription : subscriptions) {
            subscription.timer->stop();
            subscription.timer->deleteLater();
        }
        // The pool may end the batch while these run; the connection itself
        // is only ever deleted later, so its members stay valid.
        for (int query : queued)
            report(query, m_failure);
        for (const Subscription &subscription : subscriptions)
            report(subscription.query, m_failure);
    }

    void report(int query, const QString &error) {
        if (m_pool)
            m_pool->relayDone(m_batch, query, error.isEmpty() ? QString() : m_relay + ": " + error, m_reached);
    }

    static constexpr int maxReconnects = 2;
    QString m_relay;
    RelayPool *m_pool;
    std::shared_ptr<Batch> m_batch;
    const Limits m_limits;
    QWebSocket m_socket;
    QTimer m_connectTimer;
    bool m_connected = false;
    // Connected at least once in this batch.
    bool m_reached = false;
    // Sent an EOSE or CLOSED in this batch.
    bool m_answered = false;
    bool m_hostile = false;
    int m_reconnects = 0;
    QString m_failure;
    QList<int> m_queued;
    QHash<QString, Subscription> m_subscriptions;
};

RelayPool::RelayPool(QObject *parent) : QObject(parent) {}

RelayPool::~RelayPool() {
    // A callback may be deleting the pool from inside a connection's own
    // signal, so the connections outlive it a little, detached.
    for (const auto &batch : std::as_const(m_batches)) {
        batch->over = true;
        for (const QPointer<Connection> &connection : std::as_const(batch->connections))
            if (connection)
                connection->retire();
    }
}

QStringList RelayPool::defaultRelays() {
    // The web app's fallback relays.
    return {QStringLiteral("wss://relay.damus.io"), QStringLiteral("wss://relay.primal.net"),
            QStringLiteral("wss://nos.lol"), QStringLiteral("wss://relay.snort.social")};
}

QStringList RelayPool::configuredRelays() {
    if (!qEnvironmentVariableIsSet("READSTR_RELAYS"))
        return defaultRelays();
    // A list that is empty or names no usable relay is a mistake, and
    // falling back to the public relays would send every followed author to
    // them; it is used as it is, so queries fail and say why.
    const QString configured = qEnvironmentVariable("READSTR_RELAYS");
    QStringList relays;
    static const QRegularExpression separator(QStringLiteral("[\\s,]+"));
    for (const QString &entry : configured.split(separator, Qt::SkipEmptyParts))
        if (isUsableRelay(QUrl(entry, QUrl::StrictMode)) && !relays.contains(entry))
            relays << entry;
    return relays;
}

bool RelayPool::isUsableRelay(const QUrl &url) {
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty())
        return false;
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("wss"))
        return true;
    // Cleartext only where nothing on the network can see or change it.
    return scheme == QLatin1String("ws")
        && (url.host() == QLatin1String("localhost") || QHostAddress(url.host()).isLoopback());
}

bool RelayPool::isPublicRelay(const QUrl &url) {
    return url.isValid() && url.scheme().toLower() == QLatin1String("wss") && url.userInfo().isEmpty()
        && !url.host().isEmpty() && !Fetcher::isLocalHost(url.host());
}

void RelayPool::queryEachHeld(const QList<NostrQuery> &queries,
                              std::function<void(qsizetype, const NostrResult &, const Release &)> each,
                              std::function<void()> finished) {
    auto batch = std::make_shared<Batch>();
    for (const NostrQuery &query : queries) {
        Batch::Query state;
        state.query = query;
        batch->queries << state;
    }
    batch->each = std::move(each);
    batch->finished = std::move(finished);
    m_batches << batch;
    if (queries.isEmpty()) {
        QMetaObject::invokeMethod(
            this,
            [this, batch] {
                m_batches.removeOne(batch);
                if (batch->finished && !batch->over)
                    batch->finished();
            },
            Qt::QueuedConnection);
        return;
    }
    pump(batch);
}

void RelayPool::queryEach(const QList<NostrQuery> &queries,
                          const std::function<void(qsizetype, const NostrResult &)> &each) {
    QEventLoop loop;
    queryEachHeld(
        queries,
        [&each](qsizetype index, const NostrResult &result, const Release &release) {
            each(index, result);
            release();
        },
        [&loop] { loop.quit(); });
    loop.exec();
}

NostrResult RelayPool::query(const NostrQuery &query) {
    NostrResult out;
    queryEach({query}, [&out](qsizetype, const NostrResult &result) { out = result; });
    return out;
}

void RelayPool::pump(const std::shared_ptr<Batch> &batch) {
    while (!batch->over && batch->open < qMax(1, m_limits.maxOpenQueries) && batch->next < batch->queries.size()) {
        const int index = batch->next++;
        ++batch->open;
        Batch::Query &state = batch->queries[index];
        state.startedAt = QDateTime::currentDateTimeUtc();
        // Connections this query would add, which exist only after the loop.
        int opening = 0;
        auto add = [&](const QString &candidate, bool trusted) {
            QUrl url(candidate.trimmed(), QUrl::StrictMode);
            // "wss://host/" and "wss://host" are the same endpoint, but
            // "/nostr/" and "/nostr" need not be.
            if (url.path() == QLatin1String("/"))
                url.setPath({});
            const QString key = url.toString(QUrl::FullyEncoded);
            if (!(trusted ? isUsableRelay(url) : isPublicRelay(url)) || state.relays.contains(key)
                || state.relays.size() >= m_limits.maxRelays)
                return;
            const bool known = batch->connections.contains(key);
            if (!trusted && !known && batch->connections.size() + opening >= m_limits.maxConnections) {
                // It may hold what the others lack, so the result cannot
                // count as complete.
                state.deferred = true;
                return;
            }
            opening += !known;
            state.relays << key;
        };
        for (const QString &relay : relays())
            add(relay, true);
        for (const QString &relay : std::as_const(state.query.extraRelays))
            add(relay, false);
        if (state.relays.isEmpty()) {
            QMetaObject::invokeMethod(
                this, [this, batch, index] { deliver(batch, index, QStringLiteral("no usable relays")); },
                Qt::QueuedConnection);
            continue;
        }
        for (const QString &relay : std::as_const(state.relays)) {
            QPointer<Connection> &connection = batch->connections[relay];
            if (!connection)
                connection = new Connection(relay, this, batch);
            connection->add(index);
        }
    }
}

bool RelayPool::accept(const std::shared_ptr<Batch> &batch, int queryIndex, const NostrEvent &event) {
    Batch::Query &state = batch->queries[queryIndex];
    // Not what was asked for: a careless relay, or one ignoring the filter.
    // Dropped before anything costly.
    if (state.delivered || event.pubkey != state.query.author || !state.query.kinds.contains(event.kind)
        || event.createdAt < state.query.since
        || event.createdAt > QDateTime::currentSecsSinceEpoch() + maxClockSkew
        || (!state.query.dTags.isEmpty()
            && !state.query.dTags.contains(std::pair(event.kind, event.tag(QStringLiteral("d"))))))
        return true;
    const QString address = replaceableKey(event);
    if (state.query.maxContentLength > 0 && event.content.size() > state.query.maxContentLength) {
        // Noted so the result can say it was refused, should no version of
        // its address that fits come. Only the newest one per address is
        // kept, since the sync judges it against its watermark, and only a
        // version newer than the one held is verified, which keeps a relay
        // from claiming a refusal with a forged event without hashing every
        // oversized copy.
        if (address.isEmpty() || state.events.contains(address))
            return true;
        const auto held = state.oversized.constFind(address);
        if (held != state.oversized.cend() && (held->id == event.id || !event.isNewerThan(*held)))
            return true;
        if (!event.verify())
            return false;
        NostrEvent stripped;
        stripped.id = event.id;
        stripped.pubkey = event.pubkey;
        stripped.createdAt = event.createdAt;
        stripped.kind = event.kind;
        stripped.tags = {{QStringLiteral("d"), event.tag(QStringLiteral("d"))}};
        state.oversized.insert(address, stripped);
        return true;
    }
    const QString key = address.isEmpty() ? event.id : address;
    const auto existing = state.events.constFind(key);
    if (existing != state.events.cend() && (existing->id == event.id || !event.isNewerThan(*existing)))
        return true;
    if (!event.verify())
        return false;
    const qint64 size = event.size();
    const qint64 freed = existing != state.events.cend() ? existing->size() : 0;
    if ((existing == state.events.cend() && state.events.size() >= m_limits.maxEventsPerQuery)
        || state.bytes - freed + size > m_limits.maxBytesPerQuery) {
        state.truncated = true;
        return true;
    }
    state.bytes += size - freed;
    state.events.insert(key, event);
    return true;
}

void RelayPool::relayDone(const std::shared_ptr<Batch> &batch, int queryIndex, const QString &error, bool reached) {
    if (batch->over)
        return;
    Batch::Query &state = batch->queries[queryIndex];
    if (state.delivered)
        return;
    ++state.done;
    if (error.isEmpty())
        ++state.answered;
    else
        state.errors << error;
    if (!reached)
        ++state.unreachable;
    if (state.done >= state.relays.size()) {
        deliver(batch, queryIndex, {});
        return;
    }
    // Once one relay has answered, the rest get a little longer rather than
    // the whole timeout, so one slow relay does not hold up every author.
    if (state.answered == 1 && error.isEmpty() && !state.grace) {
        state.grace = new QTimer(this);
        state.grace->setSingleShot(true);
        connect(state.grace, &QTimer::timeout, this, [this, batch, queryIndex] { deliver(batch, queryIndex, {}); });
        state.grace->start(m_limits.graceMs);
    }
}

void RelayPool::deliver(const std::shared_ptr<Batch> &batch, int queryIndex, const QString &error) {
    if (batch->over)
        return;
    Batch::Query &state = batch->queries[queryIndex];
    if (state.delivered)
        return;
    state.delivered = true;
    if (state.grace)
        state.grace->deleteLater();
    for (const QString &relay : std::as_const(state.relays))
        if (const QPointer<Connection> connection = batch->connections.value(relay))
            connection->cancel(queryIndex);

    NostrResult result;
    result.startedAt = state.startedAt;
    result.events = state.events.values();
    result.relays = int(state.relays.size());
    result.answered = state.answered;
    result.unreachable = state.unreachable;
    result.truncated = state.truncated;
    result.deferred = state.deferred;
    for (auto it = state.oversized.cbegin(); it != state.oversized.cend(); ++it)
        if (!state.events.contains(it.key()))
            result.oversized << it.value();
    if (!error.isEmpty())
        result.error = error;
    else if (!state.answered)
        result.error = state.errors.isEmpty() ? QStringLiteral("no relay answered") : state.errors.join("; ");
    state.events.clear();
    auto released = std::make_shared<bool>(false);
    QPointer<RelayPool> self(this);
    const Release release = [self, batch, released] {
        if (*released || !self || batch->over)
            return;
        *released = true;
        --batch->open;
        if (++batch->released < batch->queries.size()) {
            self->pump(batch);
            return;
        }
        batch->over = true;
        self->m_batches.removeOne(batch);
        // Detached before `finished`, which may delete the pool and would
        // otherwise take the connections, perhaps one still running, with it.
        for (const QPointer<Connection> &connection : std::as_const(batch->connections))
            if (connection)
                connection->retire();
        if (batch->finished)
            batch->finished();
    };
    batch->each(queryIndex, result, release);
}

int PublishResult::accepted() const {
    int count = 0;
    for (const Relay &relay : relays)
        count += relay.accepted;
    return count;
}

QString PublishResult::error() const {
    if (ok())
        return {};
    if (relays.isEmpty())
        return QStringLiteral("no usable relays");
    QStringList reasons;
    for (const Relay &relay : relays)
        reasons << relay.relay + ": " + relay.message;
    return reasons.join(QStringLiteral("; "));
}

// One event sent to a few relays, each on its own WebSocket, which ends at
// the relay's OK for it, a failure to connect, the socket's end, or the
// timeout, whichever comes first. Once every relay has ended, `done` runs
// from the event loop, and the publication lets go of the pool and goes.
class RelayPool::Publication : public QObject {
public:
    Publication(RelayPool *pool, const NostrEvent &event, const QStringList &relays,
                std::function<void(const PublishResult &)> done)
        : QObject(pool), m_pool(pool), m_id(event.id), m_done(std::move(done)) {
        const Limits limits = pool->limits();
        const QString message = QString::fromUtf8(
            QJsonDocument(QJsonArray{"EVENT", event.toJson()}).toJson(QJsonDocument::Compact));
        // Every relay is listed before any socket opens, as one may fail
        // there and then, and the publication must not end before the rest.
        for (const QString &relay : relays)
            m_result.relays << PublishResult::Relay{relay, false, {}};
        for (qsizetype i = 0; i < relays.size(); ++i) {
            auto *socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
            // Only OK and NOTICE are wanted, both short.
            socket->setMaxAllowedIncomingMessageSize(64 * 1024);
            socket->setMaxAllowedIncomingFrameSize(64 * 1024);
            auto *timer = new QTimer(this);
            timer->setSingleShot(true);
            m_sockets << socket;
            m_timers << timer;
            connect(timer, &QTimer::timeout, this, [this, i] {
                end(i, false,
                    m_sockets.at(i)->state() == QAbstractSocket::ConnectedState ? QStringLiteral("timed out")
                                                                                : QStringLiteral("timed out connecting"));
            });
            connect(socket, &QWebSocket::connected, this, [this, i, message, limits] {
                m_timers.at(i)->start(limits.publishTimeoutMs);
                m_sockets.at(i)->sendTextMessage(message);
            });
            connect(socket, &QWebSocket::disconnected, this, [this, i] {
                const QString reason = m_sockets.at(i)->closeReason();
                end(i, false, reason.isEmpty() ? QStringLiteral("connection closed") : reason);
            });
            connect(socket, &QWebSocket::errorOccurred, this,
                    [this, i] { end(i, false, m_sockets.at(i)->errorString()); });
            connect(socket, &QWebSocket::textMessageReceived, this,
                    [this, i](const QString &text) { received(i, text); });
            timer->start(limits.connectTimeoutMs);
            QNetworkRequest request{QUrl(relays.at(i))};
            request.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("readstr-desktop/" READSTR_VERSION));
            socket->open(request);
        }
        if (relays.isEmpty())
            finish();
    }

private:
    void received(qsizetype index, const QString &text) {
        const QJsonArray message = QJsonDocument::fromJson(text.toUtf8()).array();
        // ["OK", <event id>, <accepted>, <message>], NIP-01; anything else,
        // a NOTICE or another event's OK, is not the answer.
        if (message.size() < 3 || message.at(0).toString() != QLatin1String("OK")
            || message.at(1).toString() != m_id || !message.at(2).isBool())
            return;
        const QString reason = FeedParser::cleanText(message.at(3).toString().left(400)).left(200);
        const bool accepted = message.at(2).toBool();
        end(index, accepted, reason.isEmpty() && !accepted ? QStringLiteral("refused the event") : reason);
    }

    void end(qsizetype index, bool accepted, const QString &message) {
        if (m_ended.contains(index))
            return;
        m_ended.insert(index);
        PublishResult::Relay &relay = m_result.relays[index];
        relay.accepted = accepted;
        relay.message = FeedParser::cleanText(message.left(400)).left(200);
        if (relay.message.isEmpty() && !accepted)
            relay.message = QStringLiteral("connection failed");
        m_timers.at(index)->stop();
        QWebSocket *socket = m_sockets.at(index);
        socket->disconnect(this);
        if (socket->state() == QAbstractSocket::ConnectedState)
            socket->close();
        else
            socket->abort();
        if (m_ended.size() == m_result.relays.size())
            finish();
    }

    void finish() {
        // Let go of the pool first, so `done` may delete it.
        setParent(nullptr);
        QTimer::singleShot(0, this, [this] {
            const QPointer<RelayPool> pool = m_pool;
            const auto done = std::move(m_done);
            const PublishResult result = m_result;
            deleteLater();
            if (pool && done)
                done(result);
        });
    }

    QPointer<RelayPool> m_pool;
    QString m_id;
    std::function<void(const PublishResult &)> m_done;
    PublishResult m_result;
    QList<QWebSocket *> m_sockets;
    QList<QTimer *> m_timers;
    QSet<qsizetype> m_ended;
};

void RelayPool::publish(const NostrEvent &event, const QStringList &extraRelays,
                        std::function<void(const PublishResult &)> done) {
    // As a query picks them: the configured relays, then the public ones
    // an nprofile named, each once, less onion services among those, which
    // cannot be reached without Tor and whose names would leak to DNS.
    QStringList chosen;
    const auto add = [&](const QString &candidate, bool trusted) {
        QUrl url(candidate.trimmed(), QUrl::StrictMode);
        if (url.path() == QLatin1String("/"))
            url.setPath({});
        const QString key = url.toString(QUrl::FullyEncoded);
        QString host = url.host().toLower();
        if (host.endsWith('.'))
            host.chop(1);
        const bool onion = host == QLatin1String("onion") || host.endsWith(QLatin1String(".onion"));
        if ((trusted ? isUsableRelay(url) : isPublicRelay(url) && !onion) && !chosen.contains(key)
            && chosen.size() < m_limits.maxRelays)
            chosen << key;
    };
    for (const QString &relay : relays())
        add(relay, true);
    for (const QString &relay : extraRelays)
        add(relay, false);
    new Publication(this, event, chosen, std::move(done));
}
