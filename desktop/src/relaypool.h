#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <functional>
#include <memory>

#include "nostrevent.h"

// One author's events of some kinds, as a NIP-01 filter.
struct NostrQuery {
    // 64 lowercase hex characters.
    QString author;
    QList<int> kinds;
    int limit = 50;
    // Unix seconds; 0 asks for everything.
    qint64 since = 0;
    // Relays to ask besides the pool's own, such as an nprofile's hints.
    QStringList extraRelays;

    QJsonObject filter() const;
};

struct NostrResult {
    // Verified, by the query's author, of the query's kinds, no older than
    // its `since`, and of a replaceable event only the newest version, in no
    // particular order.
    QList<NostrEvent> events;
    // How many relays were asked, and how many answered (sent EOSE) in
    // time. A relay that closes the subscription with CLOSED counts as
    // failing, with its reason in `error` when none answers.
    int relays = 0;
    int answered = 0;
    // Set when no relay answered; the reasons, joined.
    QString error;
    // The events were cut off at the query's count or size cap.
    bool truncated = false;
    // A relay hint was not asked, past the batch's connection cap.
    bool deferred = false;
    // When the query went out.
    QDateTime startedAt;
    bool ok() const { return error.isEmpty(); }
    // Every relay asked answered, nothing was cut off at a cap, and no hint
    // was left out, so nothing they hold was missed.
    bool complete() const { return ok() && answered == relays && !truncated && !deferred; }
};

// Asks Nostr relays over WebSockets for the events of a list of queries.
// Each relay gets one connection per batch, carrying a few subscriptions at
// a time, and only a few queries are open across the batch at once, so the
// events held in memory stay bounded however many authors there are.
// Relays are untrusted: only wss:// relays are used (ws:// only on loopback,
// for tests, and only from the configured list, never a query's hints), each
// message and each query's events are capped, every event is checked against
// the query and its signature before it is kept, and a relay that sends a
// forged event, or is slow to connect or to answer, is given up on for the
// rest of the batch.
class RelayPool : public QObject {
    Q_OBJECT

public:
    struct Limits {
        int connectTimeoutMs = 6 * 1000;
        // From REQ to EOSE or CLOSED for one subscription.
        int subscriptionTimeoutMs = 15 * 1000;
        qint64 maxMessageBytes = 4 * 1024 * 1024;
        // Kept per query, across relays: events and bytes in memory.
        int maxEventsPerQuery = 200;
        qint64 maxBytesPerQuery = 16 * 1024 * 1024;
        // Queries open at once across a batch, and subscriptions open at
        // once on one relay (relays commonly refuse more than ten or so).
        int maxOpenQueries = 4;
        int maxRelays = 12;
        // Connections across a batch: relay hints beyond this are skipped.
        int maxConnections = 24;
        // How much longer the other relays get once one has answered.
        int graceMs = 4 * 1000;
    };
    // Events dated further ahead than this are refused, so one cannot stay
    // the newest version of an article forever.
    static constexpr qint64 maxClockSkew = 24 * 3600;

    explicit RelayPool(QObject *parent = nullptr);
    ~RelayPool() override;

    void setLimits(const Limits &limits) { m_limits = limits; }
    Limits limits() const { return m_limits; }
    // The relays every query goes to. Empty means configuredRelays().
    void setRelays(const QStringList &relays) { m_relays = relays; }
    QStringList relays() const { return m_relays.isEmpty() ? configuredRelays() : m_relays; }

    // The web app's default relays that still answer.
    static QStringList defaultRelays();
    // The usable relays READSTR_RELAYS names (URLs separated by commas or
    // spaces), which may be none, or defaultRelays() when it is unset.
    static QStringList configuredRelays();
    // wss:// with a host and no credentials, or ws:// to a loopback host.
    static bool isUsableRelay(const QUrl &url);
    // wss:// to a host that is not this machine or the local network: what
    // a relay hint from someone else's nprofile may name.
    static bool isPublicRelay(const QUrl &url);

    using Release = std::function<void()>;
    // Runs the queries and returns at once. `each` runs on this object's
    // thread once per query with its result, and the query keeps its open
    // slot until `each` calls the Release it gets. `finished` runs after the
    // last release.
    void queryEachHeld(const QList<NostrQuery> &queries,
                       std::function<void(qsizetype, const NostrResult &, const Release &)> each,
                       std::function<void()> finished = {});
    // Runs the queries and waits for them in a local event loop, for the
    // headless commands.
    void queryEach(const QList<NostrQuery> &queries, const std::function<void(qsizetype, const NostrResult &)> &each);
    NostrResult query(const NostrQuery &query);

private:
    struct Batch;
    class Connection;
    friend class Connection;
    void pump(const std::shared_ptr<Batch> &batch);
    void relayDone(const std::shared_ptr<Batch> &batch, int queryIndex, const QString &error);
    void deliver(const std::shared_ptr<Batch> &batch, int queryIndex, const QString &error);
    // False when the event is forged; true when it was kept or is simply
    // not wanted.
    bool accept(const std::shared_ptr<Batch> &batch, int queryIndex, const NostrEvent &event);

    Limits m_limits;
    QStringList m_relays;
    QList<std::shared_ptr<Batch>> m_batches;
};
