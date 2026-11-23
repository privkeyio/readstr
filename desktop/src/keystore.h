#pragma once

#include <QByteArray>
#include <QDBusObjectPath>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVariant>
#include <chrono>
#include <functional>
#include <memory>

class QDBusMessage;
class QDBusVariant;

// Where an account's secret key is kept between runs: the desktop's
// keyring, behind an interface so tests use a fake and never touch the real
// one. Every call is asynchronous, since the keyring may have to ask the
// user to unlock it, and `done` never runs inside the call. It runs exactly
// once, unless the store is destroyed first, so a caller may wait on it.
// Nothing weaker stands in when the keyring is missing: the key is not kept
// at all.
class KeyStore : public QObject {
    Q_OBJECT

public:
    enum class Error {
        None,
        // No Secret Service on the session bus, or no session bus.
        Unavailable,
        // The user closed the unlock prompt.
        Dismissed,
        // The keyring or the item stayed locked.
        Locked,
        // Anything else, with what the keyring said in `detail`.
        Failed,
    };
    struct Result {
        Error error = Error::None;
        QString detail;
        // contains(): whether an item is stored under the npub's attributes;
        // lookup(): whether one of them holds the npub's key.
        bool found = false;
        // lookup() only: the 32-byte secret key; lookupPairing() only: the
        // pairing as RemoteSigner::Pairing::toJson() writes it. It is wiped
        // once `done` returns, so a caller keeps what it needs, not the
        // buffer.
        QByteArray secret;
    };
    using Done = std::function<void(const Result &result)>;

    using QObject::QObject;

    // Whether an item is stored under `npub`'s attributes, without reading
    // it or asking to unlock anything. A match says only that much: the item
    // may hold no key, or another account's; only lookup() checks the key.
    virtual void contains(const QString &npub, Done done) = 0;
    // Reads the key for `npub`, unlocking the keyring first if need be. Only
    // a key whose public key is the npub's is handed back.
    virtual void lookup(const QString &npub, Done done) = 0;
    // Stores the 32-byte `secret` for `npub`, replacing any key it had.
    virtual void store(const QString &npub, const QByteArray &secret, Done done) = 0;
    // Deletes every key stored for `npub`; `found` says whether there was one.
    virtual void remove(const QString &npub, Done done) = 0;

    // The same for the account's pairing with a remote signer (NIP-46),
    // kept as an item of its own, as the client key it holds can ask the
    // signer to sign. lookupPairing() hands back only a pairing that parses:
    // the first one, unlocked items first. Its signer is not asked again to
    // prove it holds the key, which would take an approval on every launch,
    // so another program of the user's can plant a pairing; such a program
    // can read the unlocked keyring, the key included, anyway.
    virtual void containsPairing(const QString &npub, Done done) = 0;
    virtual void lookupPairing(const QString &npub, Done done) = 0;
    virtual void storePairing(const QString &npub, const QByteArray &pairing, Done done) = 0;
    virtual void removePairing(const QString &npub, Done done) = 0;
};

// The freedesktop Secret Service (GNOME Keyring, KeePassXC, KWallet's
// bridge) over the session bus: one item per npub in the default
// collection, with the attributes application=readstr and npub=<npub>,
// holding the key as 64 hex characters, and one per npub paired with a
// remote signer, with application=readstr-nip46 and npub=<npub>, holding the
// pairing as JSON. The pairing's application differs rather than adding an
// attribute, since a search matches every item that has the attributes it
// names, and one for the key must not find the pairing. A locked collection or
// item is unlocked through the service's own prompt. Nothing here waits on the
// bus.
class SecretServiceKeyStore : public KeyStore {
    Q_OBJECT

public:
    // A secret as the service sends and takes it, (oayays).
    struct Secret {
        QDBusObjectPath session;
        QByteArray parameters;
        QByteArray value;
        QString contentType;
    };

    // Prompts still waiting when it goes need no cleanup: QtDBus drops a
    // destroyed receiver's signal connections itself.
    explicit SecretServiceKeyStore(QObject *parent = nullptr);

    void contains(const QString &npub, Done done) override;
    void lookup(const QString &npub, Done done) override;
    void store(const QString &npub, const QByteArray &secret, Done done) override;
    void remove(const QString &npub, Done done) override;
    void containsPairing(const QString &npub, Done done) override;
    void lookupPairing(const QString &npub, Done done) override;
    void storePairing(const QString &npub, const QByteArray &pairing, Done done) override;
    void removePairing(const QString &npub, Done done) override;

    // The attributes that find an npub's key, and its pairing.
    static QMap<QString, QString> attributes(const QString &npub);
    static QMap<QString, QString> pairingAttributes(const QString &npub);

    // How long a prompt may stay unanswered before its operation fails.
    // Generous, as a user may well take a while to type a keyring password,
    // but finite, so a service that never sends Completed does not leave
    // the operation waiting forever.
    void setPromptTimeout(std::chrono::milliseconds timeout) { m_promptTimeout = timeout; }

protected:
    // The D-Bus side, which tests replace. Whether there is a session bus.
    virtual bool connected() const;
    // Sends `message` and hands its reply, or error reply, to `done`.
    virtual void call(const QDBusMessage &message, std::function<void(const QDBusMessage &reply)> done);
    virtual bool listen(const QString &promptPath);
    virtual void stopListening(const QString &promptPath);
    // A Completed signal from the prompt at `promptPath`.
    void completed(const QString &promptPath, bool dismissed, const QVariant &result);
    // The service left the bus, or another process took its name, so no
    // prompt still waiting will ever complete: each one fails.
    void serviceLost();

private slots:
    void handleCompleted(bool dismissed, const QDBusVariant &result, const QDBusMessage &message);

private:
    // Each step hands on a Result whose error is None when it succeeded.
    using Paths = QList<QDBusObjectPath>;
    // Takes an item's value, which it may wipe, and fills in `result` when
    // the value is what was looked for.
    using Accept = std::function<bool(QByteArray &value, Result &result)>;
    using Attributes = QMap<QString, QString>;
    void later(const Done &done, const Result &result);
    void openSession(std::function<void(const QString &session, const Result &status)> next);
    void closeSession(const QString &session);
    void search(const Attributes &attributes,
                std::function<void(const Paths &unlocked, const Paths &locked, const Result &status)> next);
    void unlock(const Paths &objects, std::function<void(const Result &status)> next);
    void prompt(const QString &path, std::function<void(const Result &status)> next);
    // Stops waiting on the prompt at `path`, handing back what was to run
    // when it completed, or nothing if no prompt waits there.
    std::function<void(const Result &status)> takePrompt(const QString &path);
    void containsItem(const Attributes &attributes, Done done);
    void lookupItem(const Attributes &attributes, const Accept &accept, Done done);
    // Reads `items` in turn, the first `unlocked` of them already unlocked,
    // until `accept` takes one.
    void readItems(const Paths &items, qsizetype unlocked, qsizetype next, const Accept &accept, Done done);
    // Stores `value`, which the store wipes once done with it.
    void storeItem(const Attributes &attributes, const QString &label, const std::shared_ptr<QByteArray> &value,
                   Done done);
    void createItem(const QString &session, const Attributes &attributes, const QString &label,
                    const std::shared_ptr<QByteArray> &value, bool unlocked, Done done);
    void removeItems(const Attributes &attributes, Done done);
    void deleteItems(const Paths &items, qsizetype next, Done done);

    struct Prompt {
        std::function<void(const Result &status)> next;
        // Tells this prompt's timeout from that of an earlier one at the
        // same path.
        quint64 serial = 0;
    };
    // Waiting prompts, by path.
    QHash<QString, Prompt> m_prompts;
    quint64 m_promptSerial = 0;
    std::chrono::milliseconds m_promptTimeout = std::chrono::minutes(5);
    // Whether the real bus is watched for the service going away.
    bool m_watching = false;
};
