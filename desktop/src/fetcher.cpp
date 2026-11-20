#include "fetcher.h"

#include <QEventLoop>
#include <QNetworkReply>
#include <QNetworkRequest>
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

void Fetcher::fetch(const FetchRequest &request, std::function<void(const FetchResult &)> done) {
    FetchResult result;
    result.url = request.url;
    if (!isWebUrl(request.url)) {
        result.error = QStringLiteral("only http and https URLs can be fetched");
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
    networkRequest.setMaximumRedirectsAllowed(m_limits.maxRedirects);

    QNetworkReply *reply = m_network.get(networkRequest);
    // Bounds what Qt buffers between our reads, so the size cap below holds
    // memory to about the cap.
    reply->setReadBufferSize(256 * 1024);

    struct State {
        QByteArray body;
        QString error;
    };
    auto state = std::make_shared<State>();
    const Limits limits = m_limits;
    auto stop = [reply, state](const QString &error) {
        if (state->error.isEmpty())
            state->error = error;
        reply->abort();
    };

    connect(reply, &QNetworkReply::redirected, reply, [reply, stop, request](const QUrl &target) {
        if (!isWebUrl(target))
            return stop(QStringLiteral("refused a redirect to a non-http(s) URL"));
        if (request.url.scheme().compare("https", Qt::CaseInsensitive) == 0
            && target.scheme().compare("http", Qt::CaseInsensitive) == 0)
            return stop(QStringLiteral("refused a redirect from https to http"));
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

    connect(reply, &QNetworkReply::finished, this, [reply, state, deadline, limits, result, done]() mutable {
        deadline->stop();
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
        reply->deleteLater();
        done(result);
    });
}

QList<FetchResult> Fetcher::fetchAll(const QList<FetchRequest> &requests) {
    QList<FetchResult> results(requests.size());
    qsizetype pending = requests.size();
    QEventLoop loop;
    for (qsizetype i = 0; i < requests.size(); ++i)
        fetch(requests.at(i), [&results, &pending, &loop, i](const FetchResult &result) {
            results[i] = result;
            if (--pending == 0)
                loop.quit();
        });
    // Every callback arrives through the event loop, never during fetch(), so
    // the loop is always running when the last one quits it.
    if (pending > 0)
        loop.exec();
    return results;
}
