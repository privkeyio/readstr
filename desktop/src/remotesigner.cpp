#include "remotesigner.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include <QWebSocket>
#include <cctype>
#include <cstring>
#include <openssl/rand.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include "feedparser.h"
#include "nip19.h"
#include "nip44.h"
#include "relaypool.h"
#include "sync.h"
#include "wipe.h"

const QString RemoteSigner::permissions = QStringLiteral("sign_event:%1,sign_event:%2,sign_event:%3,sign_event:%4")
                                              .arg(Sync::subscriptionsKind)
                                              .arg(Sync::readStatusKind)
                                              .arg(Sync::viewsKind)
                                              .arg(RemoteSigner::pairingCheckKind);

namespace {
constexpr int requestKind = 24133;
// Queued for a relay that is still connecting; past this the oldest go, and
// their requests time out.
constexpr int maxQueued = 16;
// How far back the subscription asks, so an answer sent while a relay was
// reconnecting, or by a phone whose clock is a little behind, is not missed.
constexpr qint64 lookBackSeconds = 5 * 60;
constexpr int maxIdLength = 128;
constexpr int maxErrorLength = 400;

// The relay as a URL is compared: strict, and without a bare trailing slash.
QString normalizedRelay(const QString &relay) {
    QUrl url(relay.trimmed(), QUrl::StrictMode);
    if (url.path() == QLatin1String("/"))
        url.setPath({});
    return url.isValid() ? url.toString(QUrl::FullyEncoded) : QString();
}

bool isOnion(const QUrl &url) {
    QString host = url.host().toLower();
    if (host.endsWith('.'))
        host.chop(1);
    return host == QLatin1String("onion") || host.endsWith(QLatin1String(".onion"));
}

bool randomBytes(QByteArray &out, int size) {
    out.resize(size);
    return RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), size) == 1;
}

// A fresh secret key; secp256k1 refuses a value only with negligible
// probability, so a few tries are plenty.
QByteArray randomSecretKey() {
    QByteArray key;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!randomBytes(key, 32))
            break;
        if (secp256k1_ec_seckey_verify(secp256k1_context_static,
                                       reinterpret_cast<const unsigned char *>(key.constData())))
            return key;
    }
    wipe(key);
    return {};
}

QString randomToken() {
    QByteArray bytes;
    if (!randomBytes(bytes, 16))
        return {};
    return QString::fromLatin1(bytes.toHex());
}

bool isXOnlyKey(const QString &hex) {
    if (!Nip19::isHexKey(hex))
        return false;
    const QByteArray key = QByteArray::fromHex(hex.toLatin1());
    secp256k1_xonly_pubkey parsed;
    return secp256k1_xonly_pubkey_parse(secp256k1_context_static, &parsed,
                                        reinterpret_cast<const unsigned char *>(key.constData()));
}

// What the signer said, as one line of text that fits in a dialog.
QString cleanError(const QString &error) {
    return FeedParser::cleanText(error.left(maxErrorLength)).left(200);
}

QString shortKey(const QString &pubkey) {
    const QString npub = Nip19::npub(pubkey);
    return Nip19::shortNpub(npub, npub);
}

// Adds an already normalized relay, once, while fewer than maxRelays are in.
void addRelay(QStringList &relays, const QString &normalized) {
    if (!relays.contains(normalized) && relays.size() < RemoteSigner::maxRelays)
        relays << normalized;
}

// The client key goes in and out of the JSON by hand, so it never reaches
// Qt's JSON storage, whose copies cannot be wiped. The bunker secret still
// does; it is only good for a first connect, which the signer has answered.
const QByteArray clientField = QByteArrayLiteral("{\"client\":\"");
}

QByteArray RemoteSigner::Pairing::toJson() const {
    QByteArray rest = QJsonDocument(QJsonObject{{"signer", signerPubkey},
                                                {"relays", QJsonArray::fromStringList(relays)},
                                                {"secret", secret}})
                          .toJson(QJsonDocument::Compact);
    // Reserved up front, so no appending leaves a copy behind.
    QByteArray json;
    json.reserve(clientField.size() + 2 * clientSecret.size() + rest.size() + 1);
    json.append(clientField);
    const char *digits = "0123456789abcdef";
    for (const char byte : clientSecret) {
        json.append(digits[uchar(byte) >> 4]);
        json.append(digits[uchar(byte) & 15]);
    }
    json.append("\",");
    json.append(rest.constData() + 1, rest.size() - 1);
    ::wipe(rest);
    return json;
}

std::optional<RemoteSigner::Pairing> RemoteSigner::Pairing::fromJson(const QByteArray &json) {
    // As toJson() writes it: the client key first, as 64 hex characters,
    // read here and blanked in a copy before the rest is parsed.
    const qsizetype start = clientField.size();
    if (json.size() > 8 * 1024 || !json.startsWith(clientField) || json.size() < start + 65
        || json.at(start + 64) != '"')
        return std::nullopt;
    for (qsizetype i = start; i < start + 64; ++i)
        if (!isxdigit(uchar(json.at(i))))
            return std::nullopt;
    QByteArray blanked(json.constData(), json.size());
    memset(blanked.data() + start, '0', 64);
    const QJsonObject object = QJsonDocument::fromJson(blanked).object();
    ::wipe(blanked);
    Pairing pairing;
    if (object.value("client").toString() == QString(64, '0'))
        pairing.clientSecret = QByteArray::fromHex(QByteArray::fromRawData(json.constData() + start, 64));
    pairing.signerPubkey = object.value("signer").toString();
    pairing.secret = object.value("secret").toString().left(maxSecretLength);
    for (const QJsonValue &relay : object.value("relays").toArray()) {
        const QString normalized = normalizedRelay(relay.toString());
        if (RelayPool::isUsableRelay(QUrl(normalized)))
            addRelay(pairing.relays, normalized);
    }
    if (pairing.clientSecret.size() != 32
        || !secp256k1_ec_seckey_verify(secp256k1_context_static,
                                       reinterpret_cast<const unsigned char *>(pairing.clientSecret.constData()))
        || !isXOnlyKey(pairing.signerPubkey) || pairing.relays.isEmpty()) {
        pairing.wipe();
        return std::nullopt;
    }
    return pairing;
}

void RemoteSigner::Pairing::wipe() {
    ::wipe(clientSecret);
    ::wipe(secret);
}

bool RemoteSigner::isAllowedRelay(const QString &relay, const QStringList &trustedRelays) {
    const QString normalized = normalizedRelay(relay);
    if (normalized.isEmpty())
        return false;
    const QUrl url(normalized);
    for (const QString &trusted : trustedRelays)
        if (normalizedRelay(trusted) == normalized)
            return RelayPool::isUsableRelay(url);
    // As a relay hint in someone else's nprofile: whoever made the URI
    // picks where Readstr connects, so not this machine or its network,
    // and no onion service, whose name would leak to DNS.
    return RelayPool::isPublicRelay(url) && !isOnion(url);
}

std::optional<RemoteSigner::Pairing> RemoteSigner::parseBunkerUri(const QString &text,
                                                                  const QStringList &trustedRelays) {
    const QString trimmed = text.trimmed();
    const QLatin1String scheme("bunker://");
    if (trimmed.size() > maxUriLength || !trimmed.startsWith(scheme, Qt::CaseInsensitive))
        return std::nullopt;
    // Split by hand rather than by QUrl, which refuses a host label longer
    // than DNS allows, as the 64 hex characters of the key are. Nothing but
    // the key, and at most a slash, comes before the query.
    const QString rest = trimmed.mid(scheme.size());
    QString authority = rest.section('?', 0, 0);
    if (authority.endsWith('/'))
        authority.chop(1);
    Pairing pairing;
    pairing.signerPubkey = authority.toLower();
    if (!isXOnlyKey(pairing.signerPubkey))
        return std::nullopt;
    const QString queryText = rest.section('?', 1);
    if (queryText.contains('#') || !QUrl(QStringLiteral("x:?") + queryText, QUrl::StrictMode).isValid())
        return std::nullopt;
    const QUrlQuery query(queryText);
    for (const QString &relay : query.allQueryItemValues(QStringLiteral("relay"), QUrl::FullyDecoded)) {
        const QString normalized = normalizedRelay(relay);
        if (isAllowedRelay(normalized, trustedRelays))
            addRelay(pairing.relays, normalized);
    }
    const QString secret = query.queryItemValue(QStringLiteral("secret"), QUrl::FullyDecoded);
    for (const QChar c : secret)
        if (c.unicode() <= 0x20 || c.unicode() >= 0x7f)
            return std::nullopt;
    if (pairing.relays.isEmpty() || secret.size() > maxSecretLength)
        return std::nullopt;
    pairing.secret = secret;
    pairing.clientSecret = randomSecretKey();
    if (pairing.clientSecret.isEmpty())
        return std::nullopt;
    return pairing;
}

std::optional<RemoteSigner::Pairing> RemoteSigner::newPairing(const QStringList &relays) {
    Pairing pairing;
    for (const QString &relay : relays) {
        const QString normalized = normalizedRelay(relay);
        if (RelayPool::isUsableRelay(QUrl(normalized)))
            addRelay(pairing.relays, normalized);
    }
    pairing.secret = randomToken();
    pairing.clientSecret = randomSecretKey();
    if (pairing.relays.isEmpty() || pairing.secret.isEmpty() || pairing.clientSecret.isEmpty()) {
        pairing.wipe();
        return std::nullopt;
    }
    return pairing;
}

QString RemoteSigner::nostrConnectUri(const Pairing &pairing) {
    const auto client = LocalSigner::create(pairing.clientSecret);
    if (!client)
        return {};
    const auto encode = [](const QString &value) { return QString::fromLatin1(QUrl::toPercentEncoding(value)); };
    QString uri = QStringLiteral("nostrconnect://") + client->pubkey();
    QChar separator = '?';
    for (const QString &relay : pairing.relays) {
        uri += separator + QStringLiteral("relay=") + encode(relay);
        separator = '&';
    }
    uri += QStringLiteral("&secret=") + encode(pairing.secret);
    uri += QStringLiteral("&name=Readstr");
    uri += QStringLiteral("&perms=") + encode(permissions);
    return uri;
}

// One relay the signer is reached through: a socket subscribed to the kind
// 24133 events tagged to the client, opened when there is something to send
// and opened again a little after it drops while an answer is awaited.
class RemoteSigner::Relay : public QObject {
public:
    Relay(const QString &url, RemoteSigner *owner) : QObject(owner), m_url(url), m_owner(owner) {
        m_socket.setParent(this);
        applyLimits();
        m_connectTimer.setSingleShot(true);
        m_retry.setSingleShot(true);
        connect(&m_connectTimer, &QTimer::timeout, this, [this] {
            m_socket.abort();
            dropped();
        });
        connect(&m_retry, &QTimer::timeout, this, [this] {
            if (m_owner->waiting() > 0)
                open();
        });
        connect(&m_socket, &QWebSocket::connected, this, [this] {
            m_connectTimer.stop();
            m_connected = true;
            m_owner->m_reached = true;
            const QJsonObject filter{{"kinds", QJsonArray{requestKind}},
                                     {"#p", QJsonArray{m_owner->clientPubkey()}},
                                     {"since", QDateTime::currentSecsSinceEpoch() - lookBackSeconds}};
            m_socket.sendTextMessage(QString::fromUtf8(
                QJsonDocument(QJsonArray{"REQ", m_owner->m_subscription, filter}).toJson(QJsonDocument::Compact)));
            for (const QString &message : std::exchange(m_queue, {}))
                m_socket.sendTextMessage(message);
        });
        connect(&m_socket, &QWebSocket::disconnected, this, &Relay::dropped);
        connect(&m_socket, &QWebSocket::errorOccurred, this, [this] {
            if (!m_connected)
                dropped();
        });
        connect(&m_socket, &QWebSocket::textMessageReceived, this,
                [this](const QString &text) { m_owner->received(this, text); });
    }

    ~Relay() override { detach(); }

    // Lets go of the signer for good, from its destructor: the relay goes
    // from the event loop, as the socket may be inside one of its signals.
    void retire() {
        detach();
        setParent(nullptr);
        deleteLater();
    }

    void applyLimits() {
        const qint64 cap = m_owner->m_limits.maxMessageBytes;
        m_socket.setMaxAllowedIncomingMessageSize(quint64(cap));
        m_socket.setMaxAllowedIncomingFrameSize(quint64(cap));
    }

    void open() {
        if (m_connected || m_opening)
            return;
        m_opening = true;
        m_retry.stop();
        m_connectTimer.start(m_owner->m_limits.connectTimeoutMs);
        QNetworkRequest request{QUrl(m_url)};
        request.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("readstr-desktop/" READSTR_VERSION));
        m_socket.open(request);
    }

    void send(const QString &message) {
        if (m_connected) {
            m_socket.sendTextMessage(message);
            return;
        }
        m_queue << message;
        while (m_queue.size() > maxQueued)
            m_queue.removeFirst();
        open();
    }

private:
    // As RelayPool's connections do: nothing the socket says while it goes
    // reaches the signer, which may be gone.
    void detach() {
        m_socket.disconnect(this);
        m_connectTimer.stop();
        m_retry.stop();
        if (m_connected) {
            m_socket.flush();
            m_socket.close();
        } else {
            m_socket.abort();
        }
        m_connected = m_opening = false;
    }

    void dropped() {
        const bool wasOpen = m_connected || m_opening;
        m_connected = m_opening = false;
        m_connectTimer.stop();
        if (wasOpen && m_owner->waiting() > 0)
            m_retry.start(m_owner->m_limits.reconnectMs);
    }

    QString m_url;
    RemoteSigner *m_owner;
    QWebSocket m_socket;
    QTimer m_connectTimer;
    QTimer m_retry;
    QStringList m_queue;
    bool m_connected = false;
    bool m_opening = false;
};

RemoteSigner::RemoteSigner(const Pairing &pairing, const QString &userPubkey, std::unique_ptr<LocalSigner> client)
    : m_client(std::move(client)), m_clientSecret(pairing.clientSecret.constData(), pairing.clientSecret.size()),
      m_userPubkey(userPubkey), m_signerPubkey(pairing.signerPubkey), m_relays(pairing.relays),
      m_secret(pairing.secret), m_subscription(randomToken()) {}

RemoteSigner::~RemoteSigner() {
    for (Relay *relay : std::exchange(m_relaySockets, {}))
        relay->retire();
    wipe(m_clientSecret);
    wipe(m_conversationKey);
    wipe(m_secret);
}

std::unique_ptr<RemoteSigner> RemoteSigner::create(const Pairing &pairing, const QString &userPubkey) {
    auto client = LocalSigner::create(pairing.clientSecret);
    if (!client || !Nip19::isHexKey(userPubkey) || pairing.relays.isEmpty())
        return nullptr;
    std::unique_ptr<RemoteSigner> signer(new RemoteSigner(pairing, userPubkey, std::move(client)));
    if (signer->m_subscription.isEmpty())
        return nullptr;
    if (!pairing.signerPubkey.isEmpty()) {
        signer->m_conversationKey =
            Nip44::conversationKey(pairing.clientSecret, QByteArray::fromHex(pairing.signerPubkey.toLatin1()));
        if (signer->m_conversationKey.isEmpty())
            return nullptr;
    }
    for (const QString &relay : pairing.relays)
        signer->m_relaySockets << new Relay(relay, signer.get());
    return signer;
}

void RemoteSigner::setLimits(const Limits &limits) {
    m_limits = limits;
    for (Relay *relay : std::as_const(m_relaySockets))
        relay->applyLimits();
}

void RemoteSigner::later(std::function<void()> call) {
    QMetaObject::invokeMethod(this, std::move(call), Qt::QueuedConnection);
}

RemoteSigner::Pairing RemoteSigner::pairing() const {
    Pairing pairing;
    pairing.clientSecret = QByteArray(m_clientSecret.constData(), m_clientSecret.size());
    pairing.signerPubkey = m_signerPubkey;
    pairing.relays = m_relays;
    pairing.secret = m_secret;
    return pairing;
}

void RemoteSigner::openRelays() {
    for (Relay *relay : std::as_const(m_relaySockets))
        relay->open();
}

void RemoteSigner::send(const QString &message) {
    for (Relay *relay : std::as_const(m_relaySockets))
        relay->send(message);
}

void RemoteSigner::request(const QString &method, const QJsonArray &params, Answer answer) {
    const auto fail = [this, &answer](const QString &error) {
        later([answer = std::move(answer), error] { answer({}, error, true); });
    };
    if (m_signerPubkey.isEmpty())
        return fail(tr("The remote signer is not paired yet"));
    const QString id = randomToken();
    const QByteArray plaintext =
        QJsonDocument(QJsonObject{{"id", id}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact);
    if (plaintext.size() > Nip44::maxPlaintext)
        return fail(tr("This event is too large for a remote signer to sign"));
    const auto payload = id.isEmpty() ? std::nullopt : Nip44::encrypt(plaintext, m_conversationKey);
    if (!payload)
        return fail(tr("Cannot sign: the system's random number generator failed"));
    NostrEvent event;
    event.kind = requestKind;
    event.createdAt = QDateTime::currentSecsSinceEpoch();
    event.tags = {{"p", m_signerPubkey}};
    event.content = *payload;
    const auto [signedEvent, error] = m_client->signNow(event);
    if (!error.isEmpty())
        return fail(error);
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, id] { timedOut(id); });
    timer->start(m_limits.requestTimeoutMs);
    m_requests.insert(id, {std::move(answer), timer, false, signedEvent.id, {}});
    emit waitingChanged();
    send(QString::fromUtf8(QJsonDocument(QJsonArray{"EVENT", signedEvent.toJson()}).toJson(QJsonDocument::Compact)));
}

void RemoteSigner::conclude(const QString &id, const QString &result, const QString &error, bool local) {
    const auto found = m_requests.find(id);
    if (found == m_requests.end())
        return;
    const Request request = found.value();
    m_requests.erase(found);
    request.timer->stop();
    request.timer->deleteLater();
    emit waitingChanged();
    later([answer = request.answer, result, error, local] { answer(result, error, local); });
}

void RemoteSigner::timedOut(const QString &id) {
    conclude(id, {},
             m_reached ? tr("The signer did not answer in time") : tr("Cannot reach the relays of the remote signer"),
             true);
}

void RemoteSigner::accepted(const Relay *relay, const QString &eventId, bool ok, const QString &message) {
    if (ok || eventId.isEmpty())
        return;
    for (auto request = m_requests.begin(); request != m_requests.end(); ++request) {
        if (request->eventId != eventId)
            continue;
        if (!request->refusedBy.contains(relay))
            request->refusedBy << relay;
        if (request->refusedBy.size() < m_relaySockets.size())
            return;
        return conclude(request.key(), {},
                        tr("The relays of the remote signer refused the request: %1").arg(cleanError(message)), true);
    }
}

void RemoteSigner::received(const Relay *relay, const QString &text) {
    const QJsonArray message = QJsonDocument::fromJson(text.toUtf8()).array();
    if (message.size() >= 3 && message.at(0).toString() == QLatin1String("OK") && message.at(2).isBool())
        return accepted(relay, message.at(1).toString(), message.at(2).toBool(), message.at(3).toString());
    if (message.size() < 3 || message.at(0).toString() != QLatin1String("EVENT")
        || message.at(1).toString() != m_subscription)
        return;
    if (const auto event = NostrEvent::fromJson(message.at(2)))
        handleEvent(*event);
}

void RemoteSigner::handleEvent(const NostrEvent &event) {
    if (event.kind != requestKind || event.content.size() > Nip44::maxPayload)
        return;
    bool tagged = false;
    for (const QStringList &tag : event.tags)
        tagged = tagged || (tag.size() >= 2 && tag.at(0) == QLatin1String("p") && tag.at(1) == clientPubkey());
    if (!tagged)
        return;
    // Only the signer's own answers count, and while a nostrconnect:// link
    // waits, only an answer from whoever holds the link's secret.
    const bool known = !m_signerPubkey.isEmpty();
    if (known ? event.pubkey != m_signerPubkey : !m_awaitingConnect)
        return;
    if (!event.verify())
        return;
    QByteArray key = known ? m_conversationKey
                           : Nip44::conversationKey(m_clientSecret, QByteArray::fromHex(event.pubkey.toLatin1()));
    const auto plaintext = Nip44::decrypt(event.content, key);
    if (!plaintext) {
        if (!known)
            wipe(key);
        return;
    }
    const QJsonObject answer = QJsonDocument::fromJson(*plaintext).object();
    const QString id = answer.value("id").toString();
    const QJsonValue resultValue = answer.value("result"), errorValue = answer.value("error");
    if (id.isEmpty() || id.size() > maxIdLength || !(resultValue.isString() || resultValue.isUndefined())
        || !(errorValue.isString() || errorValue.isUndefined() || errorValue.isNull())) {
        if (!known)
            wipe(key);
        return;
    }
    const QString result = resultValue.toString(), error = errorValue.toString();
    if (!known) {
        if (error.isEmpty() && !m_secret.isEmpty() && result == m_secret) {
            m_signerPubkey = event.pubkey;
            m_conversationKey = key;
            m_awaitingConnect = false;
            if (m_connectTimer)
                m_connectTimer->stop();
            emit waitingChanged();
            askPublicKey(std::exchange(m_connectDone, {}));
        }
        wipe(key);
        return;
    }
    finish(id, result, error);
}

void RemoteSigner::finish(const QString &id, const QString &result, const QString &error) {
    const auto found = m_requests.find(id);
    if (found == m_requests.end())
        return;
    // The signer wants the user to approve at a page of its own first; the
    // real answer follows with the same id, so the request goes on waiting,
    // with its full time again.
    if (result == QLatin1String("auth_url")) {
        const QUrl url(error, QUrl::StrictMode);
        const QString scheme = url.scheme().toLower();
        if (!found->authShown && error.size() <= maxUriLength && url.isValid() && !url.host().isEmpty()
            && scheme == QLatin1String("https")) {
            found->authShown = true;
            found->timer->start(m_limits.requestTimeoutMs);
            emit authUrl(url.toString(QUrl::FullyEncoded));
        }
        return;
    }
    conclude(id, result, cleanError(error), false);
}

void RemoteSigner::pair(Finished done) {
    if (m_awaitingConnect) {
        const QString error = tr("Already waiting for a signer app to answer the link");
        return later([done = std::move(done), error] { done(error); });
    }
    if (!m_signerPubkey.isEmpty()) {
        request(QStringLiteral("connect"), QJsonArray{m_signerPubkey, m_secret, permissions},
                [this, done](const QString &result, const QString &error, bool local) {
                    if (!error.isEmpty())
                        return done(local ? error : tr("The signer refused to connect: %1").arg(error));
                    // "ack", or the secret itself, as some signers answer.
                    if (result != QLatin1String("ack") && (m_secret.isEmpty() || result != m_secret))
                        return done(tr("The signer's answer to connect is not one Readstr understands"));
                    askPublicKey(done);
                });
        return;
    }
    m_awaitingConnect = true;
    m_connectDone = std::move(done);
    if (!m_connectTimer) {
        m_connectTimer = new QTimer(this);
        m_connectTimer->setSingleShot(true);
        connect(m_connectTimer, &QTimer::timeout, this, [this] {
            if (!std::exchange(m_awaitingConnect, false))
                return;
            emit waitingChanged();
            if (const Finished done = std::exchange(m_connectDone, {})) {
                const QString error = tr("No signer app answered the link in time");
                later([done, error] { done(error); });
            }
        });
    }
    m_connectTimer->start(m_limits.pairingTimeoutMs);
    emit waitingChanged();
    openRelays();
}

void RemoteSigner::askPublicKey(Finished done) {
    request(QStringLiteral("get_public_key"), {},
            [this, done](const QString &result, const QString &error, bool local) {
        if (!error.isEmpty())
            return done(local ? error : tr("The signer did not tell the account's public key: %1").arg(error));
        if (!Nip19::isHexKey(result))
            return done(tr("The signer's answer is not a public key"));
        if (result != m_userPubkey)
            return done(tr("The signer holds the key of %1, not the one for %2")
                            .arg(shortKey(result), shortKey(m_userPubkey)));
        provePossession(done);
    });
}

void RemoteSigner::provePossession(Finished done) {
    // Knowing the account's public key proves nothing, as anyone does:
    // only a signature over a fresh event by that key shows the signer
    // holds it. sign() checks the event is the one asked for, by the
    // account, with a signature that checks out.
    const QString token = randomToken();
    if (token.isEmpty()) {
        const QString error = tr("Cannot sign: the system's random number generator failed");
        return later([done = std::move(done), error] { done(error); });
    }
    NostrEvent check;
    check.kind = pairingCheckKind;
    check.createdAt = QDateTime::currentSecsSinceEpoch();
    check.content = QStringLiteral("Readstr pairing check ") + token;
    sign(check, [done](const NostrEvent &, const QString &error) {
        if (!error.isEmpty())
            return done(tr("The signer did not show that it holds the account's key: %1").arg(error));
        done({});
    });
}

void RemoteSigner::ping(Finished done) {
    request(QStringLiteral("ping"), {}, [done](const QString &result, const QString &error, bool) {
        if (!error.isEmpty())
            return done(error);
        done(result == QLatin1String("pong") ? QString() : tr("The signer's answer to ping is not pong"));
    });
}

void RemoteSigner::sign(const NostrEvent &event, Done done) {
    const auto fail = [this, &done](const QString &error) {
        later([done = std::move(done), error] { done({}, error); });
    };
    if (event.kind < 0 || event.kind > 65535 || event.createdAt < 0 || event.createdAt > 1000000000000LL)
        return fail(tr("Cannot sign this event: its kind or date is out of range"));
    if (!event.tagsWithinLimits())
        return fail(tr("Cannot sign this event: it has too many tags or too much in them"));
    QJsonArray tags;
    for (const QStringList &tag : event.tags)
        tags << QJsonArray::fromStringList(tag);
    const QJsonObject draft{
        {"kind", event.kind}, {"created_at", event.createdAt}, {"tags", tags}, {"content", event.content}};
    const QString draftJson = QString::fromUtf8(QJsonDocument(draft).toJson(QJsonDocument::Compact));
    request(QStringLiteral("sign_event"), QJsonArray{draftJson},
            [this, event, done](const QString &result, const QString &error, bool local) {
                if (!error.isEmpty())
                    return done({}, local ? error : tr("The signer refused to sign: %1").arg(error));
                const auto signedEvent =
                    NostrEvent::fromJson(QJsonDocument::fromJson(result.toUtf8()).object());
                if (!signedEvent)
                    return done({}, tr("The signer's answer is not a signed event"));
                if (signedEvent->pubkey != m_userPubkey)
                    return done({}, tr("The signer signed with another key than the one for %1")
                                        .arg(shortKey(m_userPubkey)));
                if (signedEvent->kind != event.kind || signedEvent->createdAt != event.createdAt
                    || signedEvent->tags != event.tags || signedEvent->content != event.content)
                    return done({}, tr("The signer sent back another event than the one asked for"));
                if (!signedEvent->verify())
                    return done({}, tr("The signer's signature does not check out"));
                done(*signedEvent, {});
            });
}
