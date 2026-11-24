#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
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
    // The body's bytes received, whatever the outcome, which an error drops
    // from `body`: what the response cost to fetch.
    qint64 received = 0;
    // Empty on success: a 2xx with a body, or a 304.
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// HTTP(S) GETs for feeds and pages. Only http and https URLs are fetched, on
// the first request and on every redirect, no redirect hop goes from https to
// http, validators reach only the URL asked for, and each response is cut off
// past a size cap while it streams in and past an overall deadline. It sends
// a plain "readstr-desktop/<version>" User-Agent and nothing that identifies
// the user; an image fetch (Limits::images) sends less still.
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
        // Refuses this machine and the local network, on the first request
        // and on every redirect, for URLs that come from someone else, such
        // as a video's thumbnail or an article's image. Feeds, relays and
        // sync may live there, so it is off for them, and they are fetched
        // as before.
        //
        // A host is refused by its name or literal address (isLocalHost and
        // isHomeNetworkName), and a name also by what it resolves to: every
        // address the lookup returns must be public, or the request is
        // refused before anything is sent. The request then goes to one of
        // those checked addresses itself, as an IP-literal URL carrying the
        // name in its Host header, and https verifies the certificate (and
        // sends SNI) for the name, so a second answer to the same name
        // (DNS rebinding) cannot move it elsewhere. On a direct connection
        // Qt does no lookup of its own. Through a proxy the user configured
        // (the system's, which Qt follows), the proxy is asked for the
        // checked address, not the name, but connects on our behalf from its
        // own network, which decides what that address reaches; the proxy is
        // the user's choice and is not bypassed. A pinned request goes over
        // HTTP/1.1, since Qt's HTTP/2 takes its :authority from the URL
        // rather than the Host header, and its redirects are followed here,
        // each hop looked up, checked and pinned the same way. It goes to
        // that one address (IPv4 when there is one): when it cannot be
        // reached the others are not tried.
        bool publicHostsOnly = false;
        // Refuses a port other than the scheme's own (80 for http, 443 for
        // https), on the first request and on every redirect, so a URL from
        // someone else cannot reach a service that is not a web server.
        bool defaultPortsOnly = false;
        // For an image that someone else's article names: it asks for the
        // four formats Thumbnails decodes, with a User-Agent that leaves out
        // the version and a neutral Accept-Language in place of the one Qt
        // would make from the system's locale, so the host learns as little
        // about the reader as it can. It stops a response as soon as its
        // headers give an error status or a type other than image/*, rather
        // than downloading a body nobody will look at.
        bool images = false;
    };

    explicit Fetcher(QObject *parent = nullptr);
    ~Fetcher() override;

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
    // What an image fetch sends instead: no version, which would narrow
    // down who is reading.
    static QByteArray imageUserAgent();
    // This machine and the local network, by name or literal address:
    // localhost, .local, .internal, a name without a dot, loopback,
    // unspecified, private, link-local, unique-local, CGNAT, IETF protocol
    // assignment (192.0.0.0/24), benchmarking, documentation, reserved,
    // broadcast, and multicast addresses, and an IPv6 address that carries
    // one of these IPv4 addresses (mapped, compatible, SIIT, NAT64, 6to4),
    // as well as Teredo (2001::/32) and local-use NAT64 (64:ff9b:1::/48),
    // whose IPv4 address may be anything the network chose. The
    // documentation ranges name no real host, so refusing them costs no
    // feed, relay, or image anyone can actually reach.
    static bool isLocalHost(const QString &host);
    // The names home routers hand out (.lan, .home, home.arpa). They are
    // refused only under Limits::publicHostsOnly, for URLs someone else
    // names, and not by isLocalHost: Sync with Nostr and a pasted bunker
    // relay keep treating them as public, since a user may run a feed or a
    // relay on one.
    static bool isHomeNetworkName(const QString &host);

#ifdef READSTR_TESTS
    // Only in the test build (tests/readstr_tests.pro defines
    // READSTR_TESTS), so the app always looks names up with QHostInfo and
    // takes no local address for a public one.
    //
    // Looks a name up for publicHostsOnly: calls `answer` once with the
    // name's addresses or an error. It must call it on the thread of the
    // Fetcher that asked, the thread `resolver` itself is called on, since
    // the answer checks there whether that Fetcher still exists; it may
    // call it before returning or later.
    using Answer = std::function<void(const QList<QHostAddress> &addresses, const QString &error)>;
    using Resolver = std::function<void(const QString &host, const Answer &answer)>;
    // Looks names up with `resolver` instead of the system's resolver
    // (QHostInfo), and takes `treatedAsPublic` for public addresses, so a
    // test can pin a name to its server on 127.0.0.1. An empty resolver
    // restores the system's. Set on the GUI thread while no fetch is in
    // flight.
    static void setResolverForTests(Resolver resolver, const QList<QHostAddress> &treatedAsPublic = {});
#endif

private:
    struct Batch;
    // Issues requests while slots are free, a batch at a time in turn, so a
    // long refresh does not hold up the few requests of a subscription.
    void pump();
    void issue(const std::shared_ptr<Batch> &batch);
    // `hops` is how many redirects publicHostsOnly has followed so far, and
    // `budgetMs` what is left of limits.timeoutMs (all of it when negative).
    void fetchWithin(const FetchRequest &request, std::function<void(const FetchResult &)> done,
                     const Limits &limits, int hops = 0, qint64 budgetMs = -1);
    // Sends the request; under publicHostsOnly, to `pinned`, an address
    // that `request`'s host was checked to resolve to.
    void send(const FetchRequest &request, std::function<void(const FetchResult &)> done, const Limits &limits,
              int hops, qint64 budgetMs, const QHostAddress &pinned);

    QNetworkAccessManager m_network;
    Limits m_limits;
    QList<std::shared_ptr<Batch>> m_waiting;
    int m_inFlight = 0;
    // The ids of system lookups still running, given up when the Fetcher
    // goes.
    QSet<int> m_lookups;
};
