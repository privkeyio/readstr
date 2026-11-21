#include "fetcher.h"

#include <QEventLoop>
#include <QHostAddress>
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
}

Fetcher::Fetcher(QObject *parent) : QObject(parent) {
    // Nothing is cached or kept between requests besides what the caller
    // stores itself (the validators).
    m_network.setCookieJar(nullptr);
}

QByteArray Fetcher::userAgent() { return QByteArrayLiteral("readstr-desktop/" READSTR_VERSION); }

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
        // An IPv6 address that carries an IPv4 one is judged by the IPv4
        // one, since that is where it leads: IPv4-mapped (::ffff:a.b.c.d),
        // IPv4-compatible (::a.b.c.d), NAT64 (64:ff9b::a.b.c.d), and 6to4
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
                 || address.isInSubnet(QHostAddress(QStringLiteral("64:ff9b::")), 96))
            address = QHostAddress(v4At(12));
        else if (address.isInSubnet(QHostAddress(QStringLiteral("2002::")), 16))
            address = QHostAddress(v4At(2));
    }
    // Besides the private and special ranges Qt names: "this network"
    // (0.0.0.0/8), carrier-grade NAT (100.64.0.0/10), benchmarking
    // (198.18.0.0/15), and the reserved 240.0.0.0/4, which holds the
    // broadcast address 255.255.255.255.
    return address.isLoopback() || address.isPrivateUse() || address.isLinkLocal() || address.isSiteLocal()
        || address.isUniqueLocalUnicast() || address.isMulticast() || address.isBroadcast() || address.isNull()
        || address.isInSubnet(QHostAddress(QStringLiteral("0.0.0.0")), 8)
        || address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10)
        || address.isInSubnet(QHostAddress(QStringLiteral("198.18.0.0")), 15)
        || address.isInSubnet(QHostAddress(QStringLiteral("240.0.0.0")), 4);
}

void Fetcher::fetch(const FetchRequest &request, std::function<void(const FetchResult &)> done) {
    fetchWithin(request, std::move(done), m_limits);
}

void Fetcher::fetchWithin(const FetchRequest &request, std::function<void(const FetchResult &)> done,
                          const Limits &limits) {
    FetchResult result;
    result.url = request.url;
    if (!isWebUrl(request.url))
        result.error = QStringLiteral("only http and https URLs can be fetched");
    else if (limits.publicHostsOnly && isLocalHost(request.url.host()))
        result.error = QStringLiteral("refused a host on this machine or the local network");
    if (!result.error.isEmpty()) {
        QTimer::singleShot(0, this, [done, result] { done(result); });
        return;
    }

    QNetworkRequest networkRequest(request.url);
    networkRequest.setRawHeader("User-Agent", userAgent());
    networkRequest.setRawHeader("Accept",
                                "application/rss+xml, application/atom+xml, application/feed+json, "
                                "application/xml;q=0.9, text/xml;q=0.9, application/json;q=0.8, "
                                "text/html;q=0.7, */*;q=0.5");
    if (!request.etag.isEmpty())
        networkRequest.setRawHeader("If-None-Match", request.etag.toUtf8());
    if (!request.lastModified.isEmpty())
        networkRequest.setRawHeader("If-Modified-Since", request.lastModified.toUtf8());
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    networkRequest.setMaximumRedirectsAllowed(limits.maxRedirects);

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
        if (!isWebUrl(target))
            return stop(QStringLiteral("refused a redirect to a non-http(s) URL"));
        if (limits.publicHostsOnly && isLocalHost(target.host()))
            return stop(QStringLiteral("refused a redirect to this machine or the local network"));
        if (state->current.scheme().compare("https", Qt::CaseInsensitive) == 0
            && target.scheme().compare("http", Qt::CaseInsensitive) == 0)
            return stop(QStringLiteral("refused a redirect from https to http"));
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
            stop(QStringLiteral("response is larger than %1 MiB").arg(megabytes(limits.maxBytes)));
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
    connect(deadline, &QTimer::timeout, reply, [stop, limits] {
        stop(QStringLiteral("timed out after %1 s").arg(limits.timeoutMs / 1000.0));
    });
    deadline->start(limits.timeoutMs);

    connect(reply, &QNetworkReply::finished, this, [this, reply, state, deadline, limits, result, done]() mutable {
        const qint64 remainingMs = deadline->remainingTime();
        deadline->stop();
        reply->deleteLater();
        if (state->restartAt.isValid() && state->error.isEmpty()) {
            // Starts over once at most: the new request has no validators.
            // It gets only what is left of the deadline and the redirects,
            // so the restart doesn't stretch the limits of the fetch.
            Limits remaining = limits;
            remaining.timeoutMs = int(qMax<qint64>(1, remainingMs));
            remaining.maxRedirects = qMax(0, limits.maxRedirects - state->hops);
            fetchWithin({state->restartAt, {}, {}}, [url = result.url, done](FetchResult restarted) {
                restarted.url = url;
                restarted.etag.clear();
                restarted.lastModified.clear();
                done(restarted);
            }, remaining);
            return;
        }
        if (state->error.isEmpty() && reply->error() == QNetworkReply::NoError) {
            state->body += reply->readAll();
            if (state->body.size() > limits.maxBytes)
                state->error = QStringLiteral("response is larger than %1 MiB").arg(megabytes(limits.maxBytes));
        }
        result.finalUrl = reply->url();
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
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
