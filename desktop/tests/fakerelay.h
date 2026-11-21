#pragma once

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QWebSocket>
#include <QWebSocketServer>

// A Nostr relay on 127.0.0.1 for tests. Every REQ gets the same stored
// events (whatever the filter says, as a careless relay would), then EOSE,
// unless the relay is told to stay silent or to close subscriptions.
class FakeRelay : public QWebSocketServer {
public:
    FakeRelay() : QWebSocketServer(QStringLiteral("fake-relay"), QWebSocketServer::NonSecureMode) {
        listen(QHostAddress::LocalHost);
        connect(this, &QWebSocketServer::newConnection, this, [this] {
            while (QWebSocket *socket = nextPendingConnection()) {
                socket->setParent(this);
                ++connections;
                connect(socket, &QWebSocket::textMessageReceived, socket,
                        [this, socket](const QString &text) { received(socket, text); });
            }
        });
    }

    QString url() const { return QStringLiteral("ws://127.0.0.1:%1").arg(serverPort()); }

    // Sent as given, so tests can send events that are malformed or forged.
    QList<QJsonValue> events;
    bool silent = false;
    QString closeWith;
    // Closes the connection after answering this many REQs on it, once.
    int dropAfter = 0;
    QList<QJsonObject> filters;
    QStringList closed;
    int connections = 0;

private:
    int m_answered = 0;
    void received(QWebSocket *socket, const QString &text) {
        const QJsonArray message = QJsonDocument::fromJson(text.toUtf8()).array();
        const QString type = message.at(0).toString(), id = message.at(1).toString();
        if (type == QLatin1String("CLOSE")) {
            closed << id;
            return;
        }
        if (type != QLatin1String("REQ"))
            return;
        filters << message.at(2).toObject();
        if (silent)
            return;
        if (!closeWith.isEmpty()) {
            send(socket, {"CLOSED", id, closeWith});
            return;
        }
        for (const QJsonValue &event : std::as_const(events))
            send(socket, {"EVENT", id, event});
        send(socket, {"EOSE", id});
        if (dropAfter && ++m_answered == dropAfter) {
            dropAfter = 0;
            socket->close();
        }
    }

    static void send(QWebSocket *socket, const QJsonArray &message) {
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }
};
