#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <functional>

// A minimal HTTP/1.1 server on 127.0.0.1 for tests, so nothing touches the
// real network. Each route answers once per connection and closes it.
class FakeHttpServer : public QTcpServer {
public:
    struct Request {
        QByteArray method;
        QByteArray path;
        QHash<QByteArray, QByteArray> headers;
    };

    struct Response {
        int status = 200;
        QList<QPair<QByteArray, QByteArray>> headers;
        QByteArray body;
        // Never answers, to exercise timeouts.
        bool stall = false;
        // Streams this many bytes without a Content-Length, to exercise the
        // size cap.
        qint64 stream = 0;
        // Leaves the connection open after the streamed bytes, as a server
        // still sending a longer body would. Closing it instead would cut the
        // body short, and Qt then fails a request it had queued for the same
        // host with "Connection closed".
        bool hold = false;
        // Answers after this long, to keep requests in flight.
        int delayMs = 0;
    };

    using Handler = std::function<Response(const Request &)>;

    FakeHttpServer() { listen(QHostAddress::LocalHost); }

    void route(const QByteArray &path, Handler handler) { m_routes.insert(path, std::move(handler)); }
    void serve(const QByteArray &path, const QByteArray &body, const QByteArray &type = "application/rss+xml") {
        route(path, [body, type](const Request &) { return Response{200, {{"Content-Type", type}}, body}; });
    }
    void redirect(const QByteArray &path, const QByteArray &location, int status = 302) {
        route(path, [location, status](const Request &) { return Response{status, {{"Location", location}}, {}}; });
    }

    QUrl url(const QString &path) const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path));
    }
    QList<Request> requests;
    // Requests received but not yet answered, and the most there have been.
    int inFlight = 0;
    int peakInFlight = 0;

protected:
    void incomingConnection(qintptr descriptor) override {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(descriptor);
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
            QByteArray &buffer = m_buffers[socket];
            buffer += socket->readAll();
            const qsizetype end = buffer.indexOf("\r\n\r\n");
            if (end < 0)
                return;
            const QList<QByteArray> lines = buffer.left(end).split('\n');
            m_buffers.remove(socket);
            Request request;
            const QList<QByteArray> first = lines.first().trimmed().split(' ');
            request.method = first.value(0);
            request.path = first.value(1);
            for (qsizetype i = 1; i < lines.size(); ++i) {
                const qsizetype colon = lines.at(i).indexOf(':');
                if (colon > 0)
                    request.headers.insert(lines.at(i).left(colon).trimmed().toLower(),
                                           lines.at(i).mid(colon + 1).trimmed());
            }
            requests << request;
            respond(socket, request);
        });
    }

private:
    static QByteArray reason(int status) {
        switch (status) {
        case 200: return "OK";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 404: return "Not Found";
        default: return "Status";
        }
    }

    void respond(QTcpSocket *socket, const Request &request) {
        const auto handler = m_routes.constFind(request.path);
        const Response response =
            handler == m_routes.cend() ? Response{404, {{"Content-Type", "text/plain"}}, "not found"} : (*handler)(request);
        if (response.stall)
            return;
        peakInFlight = std::max(peakInFlight, ++inFlight);
        if (response.delayMs > 0) {
            QTimer::singleShot(response.delayMs, socket, [this, socket, response] { send(socket, response); });
            return;
        }
        send(socket, response);
    }

    void send(QTcpSocket *socket, const Response &response) {
        --inFlight;
        QByteArray head = "HTTP/1.1 " + QByteArray::number(response.status) + ' ' + reason(response.status) + "\r\n";
        for (const auto &[name, value] : response.headers)
            head += name + ": " + value + "\r\n";
        head += "Connection: close\r\n";
        if (response.stream > 0) {
            socket->write(head + "\r\n");
            auto remaining = std::make_shared<qint64>(response.stream);
            auto more = [socket, remaining, hold = response.hold] {
                if (*remaining <= 0 && hold && socket->state() == QAbstractSocket::ConnectedState)
                    return;
                if (*remaining <= 0 || socket->state() != QAbstractSocket::ConnectedState) {
                    socket->disconnectFromHost();
                    return;
                }
                const qint64 chunk = std::min<qint64>(*remaining, 64 * 1024);
                *remaining -= chunk;
                socket->write(QByteArray(chunk, 'x'));
            };
            connect(socket, &QTcpSocket::bytesWritten, socket, more);
            more();
            return;
        }
        if (response.status != 304)
            head += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
        socket->write(head + "\r\n" + (response.status == 304 ? QByteArray() : response.body));
        socket->disconnectFromHost();
    }

    QHash<QByteArray, Handler> m_routes;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};
