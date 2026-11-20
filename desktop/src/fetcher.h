#pragma once

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

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
    QString etag;
    QString lastModified;
    QString contentType;
    // Empty on success: a 2xx with a body, or a 304.
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// HTTP(S) GETs for feeds and pages. Only http and https URLs are fetched, on
// the first request and on every redirect, redirects never go from https to
// http, and each response is cut off past a size cap while it streams in and
// past an overall deadline. It sends a plain "readstr-desktop/<version>"
// User-Agent and nothing that identifies the user.
class Fetcher : public QObject {
    Q_OBJECT

public:
    struct Limits {
        qint64 maxBytes = 10 * 1024 * 1024;
        int timeoutMs = 30 * 1000;
        int maxRedirects = 5;
    };

    explicit Fetcher(QObject *parent = nullptr);

    void setLimits(const Limits &limits) { m_limits = limits; }
    Limits limits() const { return m_limits; }

    // `done` runs later on this object's thread, exactly once per request.
    void fetch(const FetchRequest &request, std::function<void(const FetchResult &)> done);
    // Runs the requests concurrently and waits for all of them in a local
    // event loop, for the headless commands. The window uses fetch().
    QList<FetchResult> fetchAll(const QList<FetchRequest> &requests);
    FetchResult get(const FetchRequest &request) { return fetchAll({request}).constFirst(); }

    static QByteArray userAgent();

private:
    QNetworkAccessManager m_network;
    Limits m_limits;
};
