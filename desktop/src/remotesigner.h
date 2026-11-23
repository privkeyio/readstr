#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <functional>
#include <memory>
#include <optional>

#include "signer.h"

class QTimer;

// Signs as an account whose key stays in another app, such as Amber on a
// phone, over NIP-46: requests and answers are kind 24133 events between a
// throwaway client key and the remote signer's key, their content NIP-44
// encrypted JSON-RPC, carried by the relays the two agreed on. Each answer
// is checked like everything a relay sends: it must come from the signer's
// key, decrypt and parse within bounds, and answer a request still waiting;
// a signed event must be the one asked for, signed by the account. Nothing
// decrypted is logged. A request a person has to approve can take a while,
// so each waits up to Limits::requestTimeoutMs and the signer says how many
// are waiting. Every answer, and every callback, runs from the event loop
// rather than inside the signer's own code, so a callback may delete the
// signer.
class RemoteSigner : public Signer {
    Q_OBJECT

public:
    // What it takes to reach the signer again after a restart: kept in the
    // keyring, never on disk, as the client key lets anyone holding it ask
    // the signer to sign.
    struct Pairing {
        // The client's own secret key, 32 bytes.
        QByteArray clientSecret;
        // The remote signer's public key, 64 hex characters; empty while a
        // nostrconnect:// link waits for the signer to answer.
        QString signerPubkey;
        // The relays requests and answers go through.
        QStringList relays;
        // The bunker URI's secret, or the one the nostrconnect:// link
        // carries, which the signer's answer to it must repeat.
        QString secret;

        // As the keyring item holds it, a JSON object. The caller wipes it.
        QByteArray toJson() const;
        // Null unless `json` is an object with a usable client key, a
        // signer key, and one to maxRelays usable relays.
        static std::optional<Pairing> fromJson(const QByteArray &json);
        void wipe();
    };

    struct Limits {
        // From a request to its answer, which may wait on a person.
        int requestTimeoutMs = 60 * 1000;
        // From a nostrconnect:// link shown to the signer's connect.
        int pairingTimeoutMs = 5 * 60 * 1000;
        int connectTimeoutMs = 6 * 1000;
        // Before a relay that dropped is opened again.
        int reconnectMs = 5 * 1000;
        // A NIP-44 payload is at most 87472 characters, so an answer in an
        // event is well within this.
        qint64 maxMessageBytes = 256 * 1024;
    };

    static constexpr int maxRelays = 4;
    static constexpr int maxUriLength = 2000;
    static constexpr int maxSecretLength = 256;
    // The kind of the throwaway event the signer signs while pairing, to
    // show it holds the account's key and not only knows its public key.
    // Ephemeral (20000-29999), so a relay would not keep it, and one no NIP
    // or service uses, for authentication (22242, 27235) or anything else
    // (24133), so its signature is good for nothing else. It is never
    // published: it travels only inside the encrypted request and answer.
    static constexpr int pairingCheckKind = 21842;
    // The kinds Readstr asks to sign, which the nostrconnect:// link names.
    static const QString permissions;

    // The pairing a bunker:// URI names, with a fresh client key: the
    // signer's key, its relays (wss:// ones on the public network, or one
    // of `trustedRelays`, the user's own), and its secret, if any. Null for
    // anything else.
    static std::optional<Pairing> parseBunkerUri(const QString &text, const QStringList &trustedRelays);
    // A fresh client key and secret, for a nostrconnect:// link over
    // `relays`, of which at most maxRelays are kept. Null when no relay is
    // left or the system's random number generator fails.
    static std::optional<Pairing> newPairing(const QStringList &relays);
    // The nostrconnect:// link a signer app scans for `pairing`.
    static QString nostrConnectUri(const Pairing &pairing);
    // Whether a relay from a URI may be used: wss:// to a public host that
    // is not an onion service, or one of `trustedRelays`.
    static bool isAllowedRelay(const QString &relay, const QStringList &trustedRelays);

    // Null when the pairing's client key is not a key. `userPubkey` is the
    // account it signs as, which the signer must hold.
    static std::unique_ptr<RemoteSigner> create(const Pairing &pairing, const QString &userPubkey);
    ~RemoteSigner() override;

    // Applies to the relays' sockets at once.
    void setLimits(const Limits &limits);
    QString pubkey() const override { return m_userPubkey; }
    void sign(const NostrEvent &event, Done done) override;

    using Finished = std::function<void(const QString &error)>;
    // Pairs with the signer, then asks it for the account's public key,
    // which must be userPubkey, and to sign a throwaway event of
    // pairingCheckKind with it, which must check out. For a bunker:// URI
    // that is a connect request with its secret; for a nostrconnect:// link
    // it is waiting for the signer's connect, which must carry the link's
    // secret and tells the signer's key. `done` runs later, once; a second
    // pair() while a link is still waiting fails at once.
    void pair(Finished done);
    // Whether the signer answers at all.
    void ping(Finished done);
    // The pairing as it stands, with the signer's key once known. The
    // caller wipes it.
    Pairing pairing() const;
    QString clientPubkey() const { return m_client->pubkey(); }
    // Requests waiting on the signer, a person's approval among them.
    int waiting() const { return int(m_requests.size()) + (m_awaitingConnect ? 1 : 0); }
    // The approval page of a request still waiting, if any; empty once
    // every request that named one has ended.
    QString pendingAuthUrl() const;

signals:
    // The signer asked the user to approve the request at `url` (an
    // auth_url answer). Shown as text, never opened.
    void authUrl(const QString &url);
    void waitingChanged();

private:
    friend class ReadstrTests;
    class Relay;
    // The signer's result, or its error; or, when `local`, why Readstr
    // itself gave up on the request (a timeout, an event too large).
    using Answer = std::function<void(const QString &result, const QString &error, bool local)>;
    struct Request {
        Answer answer;
        QTimer *timer = nullptr;
        // The page the signer asked the user to approve it at, once shown.
        QString authUrl;
        // The kind 24133 event carrying it, and the relays that refused it.
        QString eventId;
        QList<const Relay *> refusedBy;
    };

    RemoteSigner(const Pairing &pairing, const QString &userPubkey, std::unique_ptr<LocalSigner> client);
    // Sends `method` with `params`; `answer` runs later, once, with the
    // result or why there is none.
    void request(const QString &method, const QJsonArray &params, Answer answer);
    // Sends `message`, which carries request `id`, through every relay.
    void send(const QString &id, const QString &message);
    void openRelays();
    void received(Relay *relay, const QString &text);
    // A relay's OK for the event `eventId`: once every relay refused a
    // request, it fails with what the last one said.
    void accepted(const Relay *relay, const QString &eventId, bool ok, const QString &message);
    // A relay that kept closing the subscription counts as refusing every
    // request still waiting, as for accepted().
    void unsubscribed(const Relay *relay, const QString &message);
    void refused(const QString &id, const Relay *relay, const QString &message);
    // Runs `call` from the event loop, unless the signer is gone by then.
    void later(std::function<void()> call);
    void handleEvent(const NostrEvent &event);
    void finish(const QString &id, const QString &result, const QString &error);
    // Ends request `id`, if still waiting, and answers it from the event loop.
    void conclude(const QString &id, const QString &result, const QString &error, bool local);
    void timedOut(const QString &id);
    // Whether the signer's answer with `event` signed can come back within
    // the bounds of NIP-44 and of what a relay message may be.
    bool answerFits(const NostrEvent &event) const;
    void askPublicKey(Finished done);
    // Has the signer sign a throwaway event as the account.
    void provePossession(Finished done);

    std::unique_ptr<LocalSigner> m_client;
    QByteArray m_clientSecret;
    QString m_userPubkey;
    QString m_signerPubkey;
    QStringList m_relays;
    QString m_secret;
    // With the signer, once its key is known.
    QByteArray m_conversationKey;
    Limits m_limits;
    QList<Relay *> m_relaySockets;
    QHash<QString, Request> m_requests;
    // A nostrconnect:// link is out, waiting for the signer's connect.
    bool m_awaitingConnect = false;
    Finished m_connectDone;
    QTimer *m_connectTimer = nullptr;
    QString m_subscription;
    // Some relay connected, so a request that times out reached the signer.
    bool m_reached = false;
};
