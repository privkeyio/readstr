#pragma once

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>
#include <memory>

struct FetchRequest {
    QUrl url;
    // Validators from the last successful fetch, sent as a conditional GET.
    QString etag;
    QString lastModified;
};

struct FetchResult {
    QUrl url;
    // Where the body came from after redirects.
    QUrl finalUrl;
    int status = 0;
    bool notModified = false;
    QByteArray body;
    // Validators for `url`; empty when the response came from another URL
    // after a redirect, since they belong to that URL instead.
    QString etag;
    QString lastModified;
    QString contentType;
    // Empty on success: a 2xx with a body, or a 304.
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// HTTP(S) GETs for feeds and pages. Only http and https URLs are fetched, on
// the first request and on every redirect, no redirect hop goes from https to
// http, validators reach only the URL asked for, and each response is cut off
// past a size cap while it streams in and past an overall deadline. It sends
// a plain "readstr-desktop/<version>" User-Agent and nothing that identifies
// the user.
class Fetcher : public QObject {
    Q_OBJECT

public:
    struct Limits {
        qint64 maxBytes = 10 * 1024 * 1024;
        int timeoutMs = 30 * 1000;
        int maxRedirects = 5;
        // Requests in flight at once across every batch (fetchEach() and the
        // like) on one Fetcher; fetch() on its own is not counted. Qt's
        // network access manager runs at most six connections per host and
        // queues the rest, with their deadlines already running, so this
        // stays at six.
        int maxConcurrent = 6;
        // Refuses this machine and the local network (isLocalHost), on the
        // first request and on every redirect, for URLs that come from
        // someone else, such as a video's thumbnail. Feeds the user picked
        // may live there, so it is off for them. A public name that
        // resolves to a local address is not caught.
        bool publicHostsOnly = false;
    };

    explicit Fetcher(QObject *parent = nullptr);

    void setLimits(const Limits &limits) { m_limits = limits; }
    Limits limits() const { return m_limits; }

    // `done` runs later on this object's thread, exactly once per request.
    void fetch(const FetchRequest &request, std::function<void(const FetchResult &)> done);
    // Runs the requests, at most limits().maxConcurrent at a time, and waits
    // for all of them in a local event loop, for the headless commands. Each
    // result goes to `each` (with the index of its request) as soon as it
    // arrives, so a caller that consumes it keeps only a few bodies in memory.
    void fetchEach(const QList<FetchRequest> &requests, const std::function<void(qsizetype, const FetchResult &)> &each);
    QList<FetchResult> fetchAll(const QList<FetchRequest> &requests);
    // fetchEach() without the local event loop: it returns at once, and
    // `finished` runs on this object's thread after the last `each`. The
    // window uses this, so nothing waits in a nested loop.
    void fetchEachLater(const QList<FetchRequest> &requests,
                        std::function<void(qsizetype, const FetchResult &)> each,
                        std::function<void()> finished = {});
    // Gives a request's slot back. Called at most once; later calls, and
    // calls after the Fetcher is gone, do nothing. Meant for the Fetcher's
    // thread, where it takes effect at once; called on another thread it is
    // queued to the Fetcher's, which must not be destroyed during that call.
    using Release = std::function<void()>;
    // fetchEachLater() whose requests keep their slots until `each` calls the
    // Release it gets, so a caller that hands each body on to be processed
    // elsewhere bounds how many wait there too. `finished` runs after the
    // last release.
    void fetchEachHeld(const QList<FetchRequest> &requests,
                       std::function<void(qsizetype, const FetchResult &, const Release &)> each,
                       std::function<void()> finished = {});
    FetchResult get(const FetchRequest &request) { return fetchAll({request}).constFirst(); }

    static QByteArray userAgent();
    // This machine and the local network, by name or literal address:
    // localhost, .local, .internal, a name without a dot, loopback,
    // unspecified, private, link-local, unique-local, CGNAT, benchmarking,
    // reserved, broadcast, and multicast addresses, and an IPv6 address that
    // carries one of these IPv4 addresses (mapped, compatible, NAT64, 6to4).
    static bool isLocalHost(const QString &host);

private:
    struct Batch;
    // Issues requests while slots are free, a batch at a time in turn, so a
    // long refresh does not hold up the few requests of a subscription.
    void pump();
    void issue(const std::shared_ptr<Batch> &batch);
    void fetchWithin(const FetchRequest &request, std::function<void(const FetchResult &)> done,
                     const Limits &limits);

    QNetworkAccessManager m_network;
    Limits m_limits;
    QList<std::shared_ptr<Batch>> m_waiting;
    int m_inFlight = 0;
};
