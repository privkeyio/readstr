#pragma once

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QWebSocket>
#include <QWebSocketServer>
#include <algorithm>

// A Nostr relay on 127.0.0.1 for tests. Every REQ gets the same stored
// events (whatever the filter says, as a careless relay would), then EOSE,
// unless the relay is told to stay silent, to close subscriptions, or to
// honour filters.
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
    // Answers as an honest relay does instead: only the events of the
    // filter's kinds, newest first, and no more than its limit.
    bool honourFilters = false;
    // Closes the connection after answering this many REQs on it, once.
    int dropAfter = 0;
    // Closes the connection after the events of a REQ, before its EOSE.
    bool dropBeforeEose = false;
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
        for (const QJsonValue &event : answer(message.at(2).toObject()))
            send(socket, {"EVENT", id, event});
        if (dropBeforeEose) {
            socket->close();
            return;
        }
        send(socket, {"EOSE", id});
        if (dropAfter && ++m_answered == dropAfter) {
            dropAfter = 0;
            socket->close();
        }
    }

    QList<QJsonValue> answer(const QJsonObject &filter) const {
        if (!honourFilters)
            return events;
        QList<QJsonValue> matching;
        const QJsonArray kinds = filter.value(QStringLiteral("kinds")).toArray();
        // Authors and since as NIP-01 has them, before the limit, so an event
        // a relay would never send cannot take a place in the answer.
        const bool anyAuthor = !filter.contains(QStringLiteral("authors"));
        const QJsonArray authors = filter.value(QStringLiteral("authors")).toArray();
        const qint64 since = filter.value(QStringLiteral("since")).toInteger(0);
        for (const QJsonValue &event : events) {
            const QJsonObject object = event.toObject();
            if (!anyAuthor && !authors.contains(object.value(QStringLiteral("pubkey"))))
                continue;
            if (object.value(QStringLiteral("created_at")).toInteger() < since)
                continue;
            for (const QJsonValue &kind : kinds)
                if (kind.toInt() == object.value(QStringLiteral("kind")).toInt(-1)) {
                    matching << event;
                    break;
                }
        }
        std::stable_sort(matching.begin(), matching.end(), [](const QJsonValue &a, const QJsonValue &b) {
            return a.toObject().value(QStringLiteral("created_at")).toInteger()
                > b.toObject().value(QStringLiteral("created_at")).toInteger();
        });
        const int limit = filter.value(QStringLiteral("limit")).toInt(-1);
        if (limit >= 0 && matching.size() > limit)
            matching.resize(limit);
        return matching;
    }

    static void send(QWebSocket *socket, const QJsonArray &message) {
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }
};
