#include "fetcher.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QHash>
#include <QHostInfo>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <memory>

namespace {
bool isWebUrl(const QUrl &url) {
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == "http" || scheme == "https") && !url.host().isEmpty();
}

QString megabytes(qint64 bytes) { return QString::number(double(bytes) / (1024 * 1024), 'g', 3); }

// The port `url` is refused for, described, or empty when its port is the
// scheme's own or none is given.
QString refusedPort(const QUrl &url) {
    const int standard = url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 ? 443 : 80;
    if (url.port() == -1 || url.port() == standard)
        return {};
    return QStringLiteral("port %1, which is not the default for %2").arg(url.port()).arg(url.scheme());
}

// Whether publicHostsOnly turns `host` away by its name or literal address.
bool isRefusedPublicHost(const QString &host, const Fetcher::Limits &limits) {
    return limits.publicHostsOnly && (Fetcher::isLocalHost(host) || Fetcher::isHomeNetworkName(host));
}

// Why the redirect from `from` to `target` is refused, or empty when it is
// not. publicHostsOnly also looks the target's name up, after this.
QString refusedRedirect(const QUrl &from, const QUrl &target, const Fetcher::Limits &limits) {
    if (!isWebUrl(target))
        return QStringLiteral("refused a redirect to a non-http(s) URL");
    if (isRefusedPublicHost(target.host(), limits))
        return QStringLiteral("refused a redirect to this machine or the local network");
    if (limits.defaultPortsOnly && !refusedPort(target).isEmpty())
        return QStringLiteral("refused a redirect to ") + refusedPort(target);
    if (from.scheme().compare("https", Qt::CaseInsensitive) == 0
        && target.scheme().compare("http", Qt::CaseInsensitive) == 0)
        return QStringLiteral("refused a redirect from https to http");
    return {};
}

bool isRedirectStatus(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// What the Host header names for `url`: its host, in brackets when it is an
// IPv6 address, and its port unless that is the scheme's own.
QByteArray hostHeader(const QUrl &url) {
    QByteArray host = url.host(QUrl::FullyEncoded).toLatin1();
    if (host.contains(':'))
        host = '[' + host + ']';
    if (!refusedPort(url).isEmpty())
        host += ':' + QByteArray::number(url.port());
    return host;
}

// Takes a lookup's addresses, or its error, on the Fetcher's thread.
using Answer = std::function<void(const QList<QHostAddress> &addresses, const QString &error)>;

#ifdef READSTR_TESTS
struct TestResolver {
    Fetcher::Resolver resolve;
    QList<QHostAddress> treatedAsPublic;
};

TestResolver &testResolver() {
    static TestResolver resolver;
    return resolver;
}
#endif

// Whether `address` counts as public. Only the test build can take a local
// address for one, so its servers on 127.0.0.1 can stand in for the web.
bool isPublicAddress(const QHostAddress &address) {
#ifdef READSTR_TESTS
    if (testResolver().treatedAsPublic.contains(address))
        return true;
#endif
    return !Fetcher::isLocalHost(address.toString());
}

// Whether every address is public. One local address among public ones is
// enough to refuse the name: which of them a connection would use is not up
// to us, and a name that names the local network at all is not one to trust.
bool allPublic(const QList<QHostAddress> &addresses) {
    for (const QHostAddress &address : addresses)
        if (!isPublicAddress(address))
            return false;
    return true;
}

// The most of a name's checked addresses one request tries, so a long
// answer cannot stretch the work of one fetch.
constexpr int maxCandidates = 4;

// The checked addresses in the order they are tried: IPv4 first, as the
// addresses most networks can reach, then the rest, each once.
QList<QHostAddress> orderedCandidates(const QList<QHostAddress> &addresses) {
    QList<QHostAddress> ordered;
    for (const bool ipv4 : {true, false})
        for (const QHostAddress &address : addresses)
            if ((address.protocol() == QAbstractSocket::IPv4Protocol) == ipv4 && !ordered.contains(address)
                && ordered.size() < maxCandidates)
                ordered << address;
    return ordered;
}

// Whether a reply failed for want of a connection to its address, so
// another of the name's addresses may still be reached. TLS failures and
// HTTP errors are not among these: the certificate checked is the name's,
// and an answer came, so the next address would most likely fare the same.
bool notConnected(QNetworkReply::NetworkError error) {
    switch (error) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
        return true;
    default:
        return false;
    }
}

QString timedOut(const Fetcher::Limits &limits) {
    return QStringLiteral("timed out after %1 s").arg(limits.timeoutMs / 1000.0);
}
}

// Qt picks a request's proxy by the URL it sends to, which for a pinned
// request names the checked address. A rule the user wrote for the name
// (no_proxy, a PAC file) would then not apply, and the request could go
// direct where the name would have gone through a proxy. So the proxies are
// picked for the URL as asked when it is sent, and handed to Qt here when
// it asks for the pinned URL. Anything else gets what Qt would have picked.
struct Fetcher::Routes : QNetworkProxyFactory {
    struct Route {
        QList<QNetworkProxy> proxies;
        int requests = 0;
    };
    QHash<QUrl, Route> routes;

    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query) override {
        const auto route = routes.constFind(query.url());
        return route != routes.cend() ? route->proxies : QNetworkProxyFactory::proxyForQuery(query);
    }
};

#ifdef READSTR_TESTS
void Fetcher::setResolverForTests(Resolver resolver, const QList<QHostAddress> &treatedAsPublic) {
    testResolver() = {std::move(resolver), treatedAsPublic};
}
#endif

Fetcher::Fetcher(QObject *parent) : QObject(parent) {
    // Nothing is cached or kept between requests besides what the caller
    // stores itself (the validators).
    m_network.setCookieJar(nullptr);
}

Fetcher::~Fetcher() {
    // A lookup nobody will take still holds one of the few threads Qt looks
    // names up on until the system resolver answers, so it is given up.
    for (const int id : std::as_const(m_lookups))
        QHostInfo::abortHostLookup(id);
}

QByteArray Fetcher::userAgent() { return QByteArrayLiteral("readstr-desktop/" READSTR_VERSION); }

QByteArray Fetcher::imageUserAgent() { return QByteArrayLiteral("readstr"); }

bool Fetcher::isLocalHost(const QString &host) {
    QString name = host.toLower();
    // A trailing dot names the same host, so "localhost." is localhost.
    if (name.endsWith('.'))
        name.chop(1);
    // A literal address first, since an IPv6 one has no dot; the name rules
    // are for names.
    QHostAddress address;
    if (!address.setAddress(name))
        return name == QLatin1String("localhost") || name.endsWith(QLatin1String(".localhost"))
            || name.endsWith(QLatin1String(".local")) || name.endsWith(QLatin1String(".internal"))
            || !name.contains('.');
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        // The unspecified address reaches this machine, as 0.0.0.0 does.
        if (address.isEqual(QHostAddress(QHostAddress::AnyIPv6)))
            return true;
        // Teredo (2001::/32) tunnels to an IPv4 address that is stored
        // obfuscated, and local-use NAT64 (64:ff9b:1::/48) translates to
        // whatever IPv4 address the local network's translator picks, so
        // neither can be judged by an address it carries.
        if (address.isInSubnet(QHostAddress(QStringLiteral("2001::")), 32)
            || address.isInSubnet(QHostAddress(QStringLiteral("64:ff9b:1::")), 48))
            return true;
        // The documentation range (2001:db8::/32) names no real host, so
        // a name that resolves into it is up to something.
        if (address.isInSubnet(QHostAddress(QStringLiteral("2001:db8::")), 32))
            return true;
        // An IPv6 address that carries an IPv4 one is judged by the IPv4
        // one, since that is where it leads: IPv4-mapped (::ffff:a.b.c.d),
        // IPv4-compatible (::a.b.c.d), SIIT's IPv4-translated
        // (::ffff:0:a.b.c.d), NAT64 (64:ff9b::a.b.c.d), and 6to4
        // (2002:aabb:ccdd::), whose address follows the prefix.
        const Q_IPV6ADDR bytes = address.toIPv6Address();
        const auto v4At = [&bytes](int at) {
            return quint32(bytes[at]) << 24 | quint32(bytes[at + 1]) << 16 | quint32(bytes[at + 2]) << 8
                | quint32(bytes[at + 3]);
        };
        bool mapped = false;
        const quint32 v4 = address.toIPv4Address(&mapped);
        if (mapped)
            address = QHostAddress(v4);
        else if (address.isInSubnet(QHostAddress(QStringLiteral("::")), 96)
                 || address.isInSubnet(QHostAddress(QStringLiteral("::ffff:0:0:0")), 96)
                 || address.isInSubnet(QHostAddress(QStringLiteral("64:ff9b::")), 96))
            address = QHostAddress(v4At(12));
        else if (address.isInSubnet(QHostAddress(QStringLiteral("2002::")), 16))
            address = QHostAddress(v4At(2));
    }
    // Besides the private and special ranges Qt names: "this network"
    // (0.0.0.0/8), carrier-grade NAT (100.64.0.0/10), the IETF protocol
    // assignments (192.0.0.0/24, where DS-Lite puts the home router at
    // 192.0.0.2 and its tunnel's far end at 192.0.0.1), benchmarking
    // (198.18.0.0/15), the reserved 240.0.0.0/4, which holds the broadcast
    // address 255.255.255.255, and the three documentation ranges
    // (192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24), which name no real
    // host and so are no destination for one.
    return address.isLoopback() || address.isPrivateUse() || address.isLinkLocal() || address.isSiteLocal()
        || address.isUniqueLocalUnicast() || address.isMulticast() || address.isBroadcast() || address.isNull()
        || address.isInSubnet(QHostAddress(QStringLiteral("0.0.0.0")), 8)
        || address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10)
        || address.isInSubnet(QHostAddress(QStringLiteral("192.0.0.0")), 24)
        || address.isInSubnet(QHostAddress(QStringLiteral("192.0.2.0")), 24)
        || address.isInSubnet(QHostAddress(QStringLiteral("198.18.0.0")), 15)
        || address.isInSubnet(QHostAddress(QStringLiteral("198.51.100.0")), 24)
        || address.isInSubnet(QHostAddress(QStringLiteral("203.0.113.0")), 24)
        || address.isInSubnet(QHostAddress(QStringLiteral("240.0.0.0")), 4);
}

bool Fetcher::isHomeNetworkName(const QString &host) {
    QString name = host.toLower();
    if (name.endsWith('.'))
        name.chop(1);
    return name.endsWith(QLatin1String(".lan")) || name.endsWith(QLatin1String(".home"))
        || name == QLatin1String("home.arpa") || name.endsWith(QLatin1String(".home.arpa"));
}

void Fetcher::fetch(const FetchRequest &request, std::function<void(const FetchResult &)> done) {
    fetchWithin(request, std::move(done), m_limits);
}

void Fetcher::fetchWithin(const FetchRequest &request, std::function<void(const FetchResult &)> done,
                          const Limits &limits, int hops, qint64 budgetMs) {
    FetchResult result;
    result.url = request.url;
    if (!isWebUrl(request.url))
        result.error = QStringLiteral("only http and https URLs can be fetched");
    else if (isRefusedPublicHost(request.url.host(), limits))
        result.error = QStringLiteral("refused a host on this machine or the local network");
    else if (limits.defaultPortsOnly && !refusedPort(request.url).isEmpty())
        result.error = QStringLiteral("refused ") + refusedPort(request.url);
    if (!result.error.isEmpty()) {
        QTimer::singleShot(0, this, [done, result] { done(result); });
        return;
    }
    const qint64 budget = budgetMs < 0 ? limits.timeoutMs : budgetMs;
    if (!limits.publicHostsOnly) {
        send(request, std::move(done), limits, hops, budget, {});
        return;
    }
    // A literal address was judged above, and is its own pin.
    const QHostAddress literal(request.url.host());
    if (!literal.isNull()) {
        send(request, std::move(done), limits, hops, budget, {literal});
        return;
    }

    // The lookup counts toward the deadline. Its answer is taken on this
    // thread, after fetch() has returned, and only once: whichever of it
    // and the deadline comes first wins.
    auto answered = std::make_shared<bool>(false);
    // The system lookup's id, or -1 while there is none, so a lookup the
    // deadline overtakes is given up rather than left to run.
    auto lookup = std::make_shared<int>(-1);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto *deadline = new QTimer(this);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, this, [this, deadline, answered, lookup, done, result, limits]() mutable {
        deadline->deleteLater();
        if (*answered)
            return;
        *answered = true;
        if (*lookup >= 0) {
            QHostInfo::abortHostLookup(*lookup);
            m_lookups.remove(*lookup);
        }
        result.error = timedOut(limits);
        done(result);
    });
    deadline->start(int(budget));

    const QPointer<QTimer> timer(deadline);
    auto take = [this, timer, answered, lookup, clock, request, done, result, limits, hops,
                 budget](const QList<QHostAddress> &addresses, const QString &error) mutable {
        if (*answered)
            return;
        *answered = true;
        if (*lookup >= 0)
            m_lookups.remove(*lookup);
        if (timer) {
            timer->stop();
            timer->deleteLater();
        }
        if (!error.isEmpty() || addresses.isEmpty()) {
            result.error = QStringLiteral("could not look up %1%2: %3")
                               .arg(hops > 0 ? QStringLiteral("the redirect to ") : QString(), request.url.host(),
                                    error.isEmpty() ? QStringLiteral("no address") : error);
            return done(result);
        }
        if (!allPublic(addresses)) {
            result.error = QStringLiteral("refused %1 name that resolves to this machine or the local network")
                               .arg(hops > 0 ? QStringLiteral("a redirect to a") : QStringLiteral("a"));
            return done(result);
        }
        // The answer can be taken after the deadline has passed but before
        // its timer has run, and then nothing more is sent.
        const qint64 left = budget - clock->elapsed();
        if (left <= 0) {
            result.error = timedOut(limits);
            return done(result);
        }
        send(request, std::move(done), limits, hops, left, orderedCandidates(addresses));
    };
    // The answer comes on this thread, so the pointer is read where the
    // Fetcher lives. It is queued even so, which keeps `done` from running
    // inside fetch() when a lookup answers at once.
    const QPointer<Fetcher> self(this);
    QThread *const home = thread();
    const Answer answer = [self, home, take](const QList<QHostAddress> &addresses, const QString &error) {
        Q_ASSERT(QThread::currentThread() == home);
        Q_UNUSED(home);
        if (Fetcher *fetcher = self.data())
            QMetaObject::invokeMethod(
                fetcher, [take, addresses, error]() mutable { take(addresses, error); }, Qt::QueuedConnection);
    };
    const QString name = request.url.host(QUrl::FullyEncoded);
#ifdef READSTR_TESTS
    if (testResolver().resolve) {
        testResolver().resolve(name, answer);
        return;
    }
#endif
    // With `this` as its context, the lookup answers on this thread, and
    // never once the Fetcher is gone.
    *lookup = QHostInfo::lookupHost(name, this, [answer](const QHostInfo &info) {
        answer(info.addresses(), info.error() == QHostInfo::NoError ? QString() : info.errorString());
    });
    m_lookups.insert(*lookup);
}

void Fetcher::send(const FetchRequest &request, std::function<void(const FetchResult &)> done, const Limits &limits,
                   int hops, qint64 budgetMs, const QList<QHostAddress> &candidates) {
    FetchResult result;
    result.url = request.url;
    const bool pinning = !candidates.isEmpty();
    QUrl target = request.url;
    if (pinning) {
        target.setHost(candidates.constFirst().toString());
        // Qt asks for the proxy by `target` alone, so two requests to the
        // same pinned URL whose names are routed differently cannot both
        // be told theirs; the later one fails rather than take the other's
        // route, which might be no proxy at all.
        // Picking the proxy can block (a PAC file, a slow system service), so
        // the time it takes comes out of the request's budget, and a request
        // whose budget it used up is not sent.
        QElapsedTimer picking;
        picking.start();
        const QList<QNetworkProxy> proxies = QNetworkProxyFactory::proxyForQuery(QNetworkProxyQuery(request.url));
        budgetMs -= picking.elapsed();
        if (budgetMs <= 0) {
            result.error = timedOut(limits);
            QTimer::singleShot(0, this, [done, result] { done(result); });
            return;
        }
        if (!m_routes) {
            m_routes = new Routes;
            m_network.setProxyFactory(m_routes);
        }
        Routes::Route &route = m_routes->routes[target];
        if (route.requests > 0 && route.proxies != proxies) {
            result.error = QStringLiteral("could not pick the proxy for %1: a request for another name at the same "
                                          "address is going another way")
                               .arg(request.url.host());
            QTimer::singleShot(0, this, [done, result] { done(result); });
            return;
        }
        route.proxies = proxies;
        ++route.requests;
    }
    QNetworkRequest networkRequest(target);
    if (limits.images) {
        networkRequest.setRawHeader("User-Agent", imageUserAgent());
        networkRequest.setRawHeader("Accept", "image/png,image/jpeg,image/gif,image/webp");
        // Qt sends the system locale's languages unless a request names its
        // own, which would tell an article's image hosts the reader's
        // language; any language will do for an image.
        networkRequest.setRawHeader("Accept-Language", "*");
    } else {
        networkRequest.setRawHeader("User-Agent", userAgent());
        networkRequest.setRawHeader("Accept",
                                    "application/rss+xml, application/atom+xml, application/feed+json, "
                                    "application/xml;q=0.9, text/xml;q=0.9, application/json;q=0.8, "
                                    "text/html;q=0.7, */*;q=0.5");
    }
    if (!request.etag.isEmpty())
        networkRequest.setRawHeader("If-None-Match", request.etag.toUtf8());
    if (!request.lastModified.isEmpty())
        networkRequest.setRawHeader("If-Modified-Since", request.lastModified.toUtf8());
    if (pinning) {
        // Qt connects to the address in the URL, so it looks nothing up and
        // a rebinding name has no second answer to give. The server still
        // learns the name from the Host header, and TLS sends it as SNI and
        // verifies the certificate against it, not against the address.
        // Qt keys its pooled connections by this name too, so a connection
        // verified for one name is never reused for another at the same
        // address. HTTP/2 is off: it would name the address as :authority.
        networkRequest.setRawHeader("Host", hostHeader(request.url));
        if (request.url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0)
            networkRequest.setPeerVerifyName(request.url.host(QUrl::FullyEncoded));
        networkRequest.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        // Each hop is looked up and pinned in turn, below, so Qt follows
        // none: it would go to the redirect's name without asking us.
        networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    } else {
        networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                    QNetworkRequest::UserVerifiedRedirectPolicy);
        networkRequest.setMaximumRedirectsAllowed(limits.maxRedirects);
    }

    // The read buffer stays unlimited: with a limit, Qt stalls a compressed
    // response instead of letting its decompression safety check fail it, so
    // a gzip bomb would sit until the deadline. readyRead below drains and
    // counts every chunk, which keeps the size cap.
    QNetworkReply *reply = m_network.get(networkRequest);

    struct State {
        QByteArray body;
        QString error;
        // The URL of the hop being fetched, so each redirect is checked
        // against the one before it rather than the first.
        QUrl current;
        // Set when a request carrying validators is redirected: it starts
        // over at this URL without them.
        QUrl restartAt;
        int hops = 0;
    };
    auto state = std::make_shared<State>();
    state->current = request.url;
    auto stop = [reply, state](const QString &error) {
        if (state->error.isEmpty())
            state->error = error;
        reply->abort();
    };

    connect(reply, &QNetworkReply::redirected, reply, [reply, stop, state, request, limits](const QUrl &target) {
        const QString refused = refusedRedirect(state->current, target, limits);
        if (!refused.isEmpty())
            return stop(refused);
        ++state->hops;
        // Qt copies every header of the first request onto the redirected
        // one, so the validators the first URL issued would reach the target.
        if (!request.etag.isEmpty() || !request.lastModified.isEmpty()) {
            state->restartAt = target;
            reply->abort();
            return;
        }
        state->current = target;
        emit reply->redirectAllowed();
    });
    connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, stop, limits] {
        const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
        if (length.isValid() && length.toLongLong() > limits.maxBytes)
            return stop(QStringLiteral("response is larger than %1 MiB").arg(megabytes(limits.maxBytes)));
        if (!limits.images)
            return;
        // A redirect's own headers say nothing of the image, so only the
        // final response's are judged.
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString type = QString::fromLatin1(reply->rawHeader("Content-Type")).trimmed();
        if (status >= 400)
            stop(QStringLiteral("HTTP %1").arg(status));
        else if (status >= 200 && status < 300 && !type.startsWith(QLatin1String("image/"), Qt::CaseInsensitive))
            stop(QStringLiteral("not an image (%1)").arg(type.isEmpty() ? QStringLiteral("no type") : type));
    });
    connect(reply, &QNetworkReply::readyRead, reply, [reply, state, stop, limits] {
        if (!state->error.isEmpty())
            return;
        state->body += reply->readAll();
        if (state->body.size() > limits.maxBytes)
            stop(QStringLiteral("response is larger than %1 MiB").arg(megabytes(limits.maxBytes)));
    });

    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, [stop, limits] { stop(timedOut(limits)); });
    deadline->start(int(budgetMs));

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, state, deadline, request, limits, hops, pinning, target, candidates, result,
             done]() mutable {
        // Qt asked for the proxy before connecting, so the route is done with.
        if (pinning) {
            const auto route = m_routes->routes.find(target);
            if (route != m_routes->routes.end() && --route->requests == 0)
                m_routes->routes.erase(route);
        }
        // Past the deadline when it is not positive, even if the deadline's
        // timer has not run yet; then no further request is started.
        const qint64 remainingMs = deadline->remainingTime();
        deadline->stop();
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray location = reply->rawHeader("Location");
        // Nothing came back from this address, so the request is sent to the
        // next one, as it is. A timeout can come after the request was
        // written, so a server may have seen it once already; for a GET of
        // an image that costs nothing.
        if (candidates.size() > 1 && state->error.isEmpty() && status == 0 && state->body.isEmpty()
            && notConnected(reply->error())) {
            if (remainingMs <= 0) {
                result.error = timedOut(limits);
                return done(result);
            }
            send(request, std::move(done), limits, hops, remainingMs, candidates.mid(1));
            return;
        }
        if (pinning && state->error.isEmpty() && reply->error() == QNetworkReply::NoError
            && isRedirectStatus(status) && !location.isEmpty()) {
            // A redirect's body costs what any other does.
            state->body += reply->readAll();
            result.received = state->body.size();
            // Resolved against the name, not the address the request went to.
            const QUrl next = state->current.resolved(QUrl::fromEncoded(location));
            result.finalUrl = state->current;
            result.status = status;
            result.error = refusedRedirect(state->current, next, limits);
            if (result.error.isEmpty() && hops >= limits.maxRedirects)
                result.error = QStringLiteral("more than %1 redirects").arg(limits.maxRedirects);
            if (result.error.isEmpty() && remainingMs <= 0)
                result.error = timedOut(limits);
            if (!result.error.isEmpty())
                return done(result);
            // The next hop is a new request, without the validators, which
            // belong to the first URL, and with what is left of the deadline.
            fetchWithin({next, {}, {}}, [url = result.url, received = result.received, done](FetchResult hop) {
                hop.url = url;
                hop.etag.clear();
                hop.lastModified.clear();
                hop.received += received;
                done(hop);
            }, limits, hops + 1, remainingMs);
            return;
        }
        if (state->restartAt.isValid() && state->error.isEmpty()) {
            result.received = state->body.size();
            if (remainingMs <= 0) {
                result.error = timedOut(limits);
                return done(result);
            }
            // Starts over once at most: the new request has no validators.
            // It gets only what is left of the deadline and the redirects,
            // so the restart doesn't stretch the limits of the fetch.
            Limits remaining = limits;
            remaining.timeoutMs = int(remainingMs);
            remaining.maxRedirects = qMax(0, limits.maxRedirects - state->hops);
            fetchWithin({state->restartAt, {}, {}},
                        [url = result.url, received = result.received, done](FetchResult restarted) {
                restarted.url = url;
                restarted.etag.clear();
                restarted.lastModified.clear();
                restarted.received += received;
                done(restarted);
            }, remaining);
            return;
        }
        if (state->error.isEmpty() && reply->error() == QNetworkReply::NoError) {
            state->body += reply->readAll();
            if (state->body.size() > limits.maxBytes)
                state->error = QStringLiteral("response is larger than %1 MiB").arg(megabytes(limits.maxBytes));
        }
        result.received = state->body.size();
        result.finalUrl = pinning ? state->current : reply->url();
        result.status = status;
        result.etag = QString::fromLatin1(reply->rawHeader("ETag"));
        result.lastModified = QString::fromLatin1(reply->rawHeader("Last-Modified"));
        result.contentType = QString::fromLatin1(reply->rawHeader("Content-Type"));
        if (!state->error.isEmpty())
            result.error = state->error;
        else if (reply->error() == QNetworkReply::TooManyRedirectsError)
            result.error = QStringLiteral("more than %1 redirects").arg(limits.maxRedirects);
        // The first URL was checked to be http(s), so an unknown scheme can
        // only come from a redirect Qt refused before asking us (file:, data:).
        else if (reply->error() == QNetworkReply::ProtocolUnknownError)
            result.error = QStringLiteral("refused a redirect to a non-http(s) URL");
        else if (result.status == 304)
            result.notModified = true;
        else if (result.status >= 400)
            result.error = QStringLiteral("HTTP %1").arg(result.status);
        else if (reply->error() != QNetworkReply::NoError)
            result.error = reply->errorString();
        else if (result.status < 200 || result.status >= 300)
            result.error = QStringLiteral("unexpected HTTP %1").arg(result.status);
        else
            result.body = std::move(state->body);
        if (result.finalUrl != result.url) {
            result.etag.clear();
            result.lastModified.clear();
        }
        done(result);
    });
}

struct Fetcher::Batch {
    QList<FetchRequest> requests;
    std::function<void(qsizetype, const FetchResult &, const Release &)> each;
    std::function<void()> finished;
    qsizetype next = 0;
    qsizetype pending = 0;
};

void Fetcher::fetchEachLater(const QList<FetchRequest> &requests,
                             std::function<void(qsizetype, const FetchResult &)> each,
                             std::function<void()> finished) {
    fetchEachHeld(
        requests,
        [each = std::move(each)](qsizetype index, const FetchResult &result, const Release &release) {
            each(index, result);
            release();
        },
        std::move(finished));
}

void Fetcher::fetchEachHeld(const QList<FetchRequest> &requests,
                            std::function<void(qsizetype, const FetchResult &, const Release &)> each,
                            std::function<void()> finished) {
    auto batch = std::make_shared<Batch>();
    batch->requests = requests;
    batch->each = std::move(each);
    batch->finished = std::move(finished);
    batch->pending = requests.size();
    if (requests.isEmpty()) {
        if (batch->finished)
            QTimer::singleShot(0, this, batch->finished);
        return;
    }
    m_waiting << batch;
    pump();
}

void Fetcher::pump() {
    // Each request is issued only when a slot frees up, so its deadline
    // starts when it is sent rather than while it waits.
    while (m_inFlight < qMax(1, m_limits.maxConcurrent) && !m_waiting.isEmpty()) {
        const std::shared_ptr<Batch> batch = m_waiting.takeFirst();
        issue(batch);
        if (batch->next < batch->requests.size())
            m_waiting << batch;
    }
}

void Fetcher::issue(const std::shared_ptr<Batch> &batch) {
    const qsizetype index = batch->next++;
    ++m_inFlight;
    fetch(batch->requests.at(index), [this, batch, index](const FetchResult &result) {
        auto released = std::make_shared<bool>(false);
        const QPointer<Fetcher> self(this);
        const std::function<void()> release = [self, batch, released] {
            if (*released || !self)
                return;
            *released = true;
            --self->m_inFlight;
            self->pump();
            if (--batch->pending == 0 && batch->finished)
                batch->finished();
        };
        // The slot count and the batch belong to this thread, so a release
        // from any other is carried out here.
        QThread *const home = thread();
        batch->each(index, result, [self, home, release] {
            if (QThread::currentThread() == home)
                release();
            else if (Fetcher *fetcher = self.data())
                QMetaObject::invokeMethod(fetcher, release, Qt::QueuedConnection);
        });
    });
}

void Fetcher::fetchEach(const QList<FetchRequest> &requests,
                        const std::function<void(qsizetype, const FetchResult &)> &each) {
    if (requests.isEmpty())
        return;
    QEventLoop loop;
    bool done = false;
    // Forwarded rather than copied, so a callable with state of its own is
    // the caller's object throughout; it outlives the loop below.
    fetchEachLater(
        requests, [&each](qsizetype index, const FetchResult &result) { each(index, result); },
        [&] {
            done = true;
            loop.quit();
        });
    // Every callback arrives through the event loop, never during
    // fetchEachLater(), so the loop is always running when the last one
    // quits it.
    if (!done)
        loop.exec();
}

QList<FetchResult> Fetcher::fetchAll(const QList<FetchRequest> &requests) {
    QList<FetchResult> results(requests.size());
    fetchEach(requests, [&results](qsizetype index, const FetchResult &result) { results[index] = result; });
    return results;
}
