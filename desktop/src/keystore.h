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
        // contains() and lookup(): whether a key is stored for the npub.
        bool found = false;
        // lookup() only: the 32-byte secret key. It is wiped once `done`
        // returns, so a caller keeps what it needs, not the buffer.
        QByteArray secret;
    };
    using Done = std::function<void(const Result &result)>;

    using QObject::QObject;

    // Whether a key is stored for `npub`, without reading it or asking to
    // unlock anything.
    virtual void contains(const QString &npub, Done done) = 0;
    // Reads the key for `npub`, unlocking the keyring first if need be. Only
    // a key whose public key is the npub's is handed back.
    virtual void lookup(const QString &npub, Done done) = 0;
    // Stores the 32-byte `secret` for `npub`, replacing any key it had.
    virtual void store(const QString &npub, const QByteArray &secret, Done done) = 0;
    // Deletes every key stored for `npub`; `found` says whether there was one.
    virtual void remove(const QString &npub, Done done) = 0;
};

// The freedesktop Secret Service (GNOME Keyring, KeePassXC, KWallet's
// bridge) over the session bus: one item per npub in the default
// collection, with the attributes application=readstr and npub=<npub>,
// holding the key as 64 hex characters. A locked collection or item is
// unlocked through the service's own prompt. Nothing here waits on the bus.
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

    // The attributes that find an npub's item.
    static QMap<QString, QString> attributes(const QString &npub);

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
    void later(const Done &done, const Result &result);
    void openSession(std::function<void(const QString &session, const Result &status)> next);
    void closeSession(const QString &session);
    void search(const QString &npub, std::function<void(const Paths &unlocked, const Paths &locked,
                                                        const Result &status)> next);
    void unlock(const Paths &objects, std::function<void(const Result &status)> next);
    void prompt(const QString &path, std::function<void(const Result &status)> next);
    // Stops waiting on the prompt at `path`, handing back what was to run
    // when it completed, or nothing if no prompt waits there.
    std::function<void(const Result &status)> takePrompt(const QString &path);
    // Reads `items` in turn, the first `unlocked` of them already unlocked,
    // until one holds the key of `pubkey`.
    void readItems(const Paths &items, qsizetype unlocked, qsizetype next, const QString &pubkey, Done done);
    void createItem(const QString &session, const QString &npub, const std::shared_ptr<QByteArray> &secret,
                    bool unlocked, Done done);
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
