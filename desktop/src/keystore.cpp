#include "keystore.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QTimer>
#include <optional>

#include "nip19.h"
#include "signer.h"
#include "wipe.h"

QDBusArgument &operator<<(QDBusArgument &argument, const SecretServiceKeyStore::Secret &secret) {
    argument.beginStructure();
    argument << secret.session << secret.parameters << secret.value << secret.contentType;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, SecretServiceKeyStore::Secret &secret) {
    argument.beginStructure();
    argument >> secret.session >> secret.parameters >> secret.value >> secret.contentType;
    argument.endStructure();
    return argument;
}

namespace {
const QString service = QStringLiteral("org.freedesktop.secrets");
const QString servicePath = QStringLiteral("/org/freedesktop/secrets");
const QString serviceInterface = QStringLiteral("org.freedesktop.Secret.Service");
const QString defaultCollection = QStringLiteral("/org/freedesktop/secrets/aliases/default");
const QString collectionInterface = QStringLiteral("org.freedesktop.Secret.Collection");
const QString itemInterface = QStringLiteral("org.freedesktop.Secret.Item");
const QString sessionInterface = QStringLiteral("org.freedesktop.Secret.Session");
const QString promptInterface = QStringLiteral("org.freedesktop.Secret.Prompt");
const QString noPrompt = QStringLiteral("/");

void registerTypes() {
    static const bool registered = [] {
        qDBusRegisterMetaType<SecretServiceKeyStore::Secret>();
        qDBusRegisterMetaType<QMap<QString, QString>>();
        return true;
    }();
    Q_UNUSED(registered);
}

QDBusMessage serviceCall(const QString &method) {
    return QDBusMessage::createMethodCall(service, servicePath, serviceInterface, method);
}

// Argument `index` of `reply` as a T. The bus hands containers and structs
// over as a QDBusArgument, which is read only when its signature is the one
// expected; a test's reply holds the T itself.
template <typename T>
std::optional<T> argument(const QDBusMessage &reply, int index, const char *signature) {
    const QVariant value = reply.arguments().value(index);
    if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
        const QDBusArgument wire = value.value<QDBusArgument>();
        if (wire.currentSignature() != QLatin1String(signature))
            return std::nullopt;
        T result;
        wire >> result;
        return result;
    }
    if (value.metaType() != QMetaType::fromType<T>())
        return std::nullopt;
    return value.value<T>();
}

KeyStore::Result failure(KeyStore::Error error, const QString &detail = {}) {
    KeyStore::Result result;
    result.error = error;
    result.detail = detail;
    return result;
}

KeyStore::Result unexpected() {
    return failure(KeyStore::Error::Failed,
                   SecretServiceKeyStore::tr("The keyring sent a reply Readstr does not understand."));
}

// What an error reply means for the user.
KeyStore::Result failure(const QDBusMessage &reply) {
    const QString name = reply.errorName();
    // No service owns the name and none can be started for it, or the bus
    // itself is gone.
    if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
        || name == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")
        || name == QLatin1String("org.freedesktop.DBus.Error.Disconnected")
        || name == QLatin1String("org.freedesktop.DBus.Error.NoServer")
        || name.startsWith(QLatin1String("org.freedesktop.DBus.Error.Spawn.")))
        return failure(KeyStore::Error::Unavailable);
    if (name == QLatin1String("org.freedesktop.Secret.Error.IsLocked"))
        return failure(KeyStore::Error::Locked);
    return failure(KeyStore::Error::Failed, reply.errorMessage().isEmpty() ? name : reply.errorMessage());
}

bool isError(const QDBusMessage &reply) {
    return reply.type() != QDBusMessage::ReplyMessage;
}
}

SecretServiceKeyStore::SecretServiceKeyStore(QObject *parent) : KeyStore(parent) {
    registerTypes();
}

QMap<QString, QString> SecretServiceKeyStore::attributes(const QString &npub) {
    return {{QStringLiteral("application"), QStringLiteral("readstr")}, {QStringLiteral("npub"), npub}};
}

bool SecretServiceKeyStore::connected() const {
    return QDBusConnection::sessionBus().isConnected();
}

void SecretServiceKeyStore::call(const QDBusMessage &message, std::function<void(const QDBusMessage &reply)> done) {
    // Owned by this store, so a reply after it is gone goes nowhere.
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, done = std::move(done)] {
        watcher->deleteLater();
        done(watcher->reply());
    });
}

bool SecretServiceKeyStore::listen(const QString &promptPath) {
    // Watched from the first prompt on, as only a prompt waits on the
    // service for longer than a method call's own timeout.
    if (!std::exchange(m_watching, true)) {
        auto *watcher = new QDBusServiceWatcher(service, QDBusConnection::sessionBus(),
                                                QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
                [this](const QString &, const QString &oldOwner, const QString &) {
                    if (!oldOwner.isEmpty())
                        serviceLost();
                });
    }
    return QDBusConnection::sessionBus().connect(service, promptPath, promptInterface, QStringLiteral("Completed"),
                                                 this, SLOT(handleCompleted(bool, QDBusVariant, QDBusMessage)));
}

void SecretServiceKeyStore::stopListening(const QString &promptPath) {
    QDBusConnection::sessionBus().disconnect(service, promptPath, promptInterface, QStringLiteral("Completed"), this,
                                             SLOT(handleCompleted(bool, QDBusVariant, QDBusMessage)));
}

void SecretServiceKeyStore::handleCompleted(bool dismissed, const QDBusVariant &result, const QDBusMessage &message) {
    completed(message.path(), dismissed, result.variant());
}

void SecretServiceKeyStore::completed(const QString &promptPath, bool dismissed, const QVariant &) {
    const auto next = takePrompt(promptPath);
    if (next)
        next(dismissed ? failure(Error::Dismissed) : Result());
}

std::function<void(const KeyStore::Result &)> SecretServiceKeyStore::takePrompt(const QString &path) {
    const auto waiting = m_prompts.find(path);
    if (waiting == m_prompts.end())
        return {};
    auto next = std::move(waiting->next);
    m_prompts.erase(waiting);
    stopListening(path);
    return next;
}

void SecretServiceKeyStore::serviceLost() {
    // Taken out first, as what runs next may start prompts of its own.
    const auto prompts = std::exchange(m_prompts, {});
    for (auto waiting = prompts.begin(); waiting != prompts.end(); ++waiting)
        stopListening(waiting.key());
    for (const Prompt &waiting : prompts)
        waiting.next(failure(Error::Failed, tr("The keyring went away before its prompt was answered.")));
}

void SecretServiceKeyStore::later(const Done &done, const Result &result) {
    QTimer::singleShot(0, this, [done, result] { done(result); });
}

void SecretServiceKeyStore::openSession(std::function<void(const QString &, const Result &)> next) {
    // The "plain" algorithm sends the secret unencrypted over the session
    // bus. That bus is this user's alone, and anything that can listen on
    // it runs as this user and could ask the service for the secret just as
    // Readstr does, so the Diffie-Hellman session would add a cipher to get
    // right without keeping the key from anyone it does not already reach.
    QDBusMessage message = serviceCall(QStringLiteral("OpenSession"));
    message << QStringLiteral("plain") << QVariant::fromValue(QDBusVariant(QString()));
    call(message, [next = std::move(next)](const QDBusMessage &reply) {
        if (isError(reply))
            return next({}, failure(reply));
        const auto session = argument<QDBusObjectPath>(reply, 1, "o");
        if (!session || session->path().isEmpty())
            return next({}, unexpected());
        next(session->path(), {});
    });
}

void SecretServiceKeyStore::closeSession(const QString &session) {
    if (!session.isEmpty())
        call(QDBusMessage::createMethodCall(service, session, sessionInterface, QStringLiteral("Close")),
             [](const QDBusMessage &) {});
}

void SecretServiceKeyStore::search(const QString &npub,
                                   std::function<void(const Paths &, const Paths &, const Result &)> next) {
    QDBusMessage message = serviceCall(QStringLiteral("SearchItems"));
    message << QVariant::fromValue(attributes(npub));
    call(message, [next = std::move(next)](const QDBusMessage &reply) {
        if (isError(reply))
            return next({}, {}, failure(reply));
        const auto unlocked = argument<Paths>(reply, 0, "ao"), locked = argument<Paths>(reply, 1, "ao");
        if (!unlocked || !locked)
            return next({}, {}, unexpected());
        next(*unlocked, *locked, {});
    });
}

void SecretServiceKeyStore::unlock(const Paths &objects, std::function<void(const Result &)> next) {
    QDBusMessage message = serviceCall(QStringLiteral("Unlock"));
    message << QVariant::fromValue(objects);
    call(message, [this, objects, next = std::move(next)](const QDBusMessage &reply) {
        if (isError(reply))
            return next(failure(reply));
        const auto unlocked = argument<Paths>(reply, 0, "ao");
        const auto promptPath = argument<QDBusObjectPath>(reply, 1, "o");
        if (!unlocked || !promptPath)
            return next(unexpected());
        if (promptPath->path() != noPrompt)
            return prompt(promptPath->path(), next);
        // Without a prompt, whatever the service did not unlock stays locked.
        next(unlocked->size() >= objects.size() ? Result() : failure(Error::Locked));
    });
}

void SecretServiceKeyStore::prompt(const QString &path, std::function<void(const Result &)> next) {
    // One Completed signal could not tell two operations waiting on the same
    // path apart, so the second is refused rather than left waiting forever.
    if (m_prompts.contains(path))
        return next(failure(Error::Failed, tr("The keyring gave the same prompt to two requests.")));
    // Listened for before the prompt shows, so a quick answer is not missed.
    if (!listen(path))
        return next(failure(Error::Failed, tr("Could not listen for the keyring's prompt.")));
    const quint64 serial = ++m_promptSerial;
    m_prompts.insert(path, {std::move(next), serial});
    QTimer::singleShot(m_promptTimeout, this, [this, path, serial] {
        const auto waiting = m_prompts.constFind(path);
        if (waiting == m_prompts.cend() || waiting->serial != serial)
            return;
        const auto next = takePrompt(path);
        // Closed too, so a dialog left open cannot unlock anything later on
        // behalf of an operation that has already failed.
        call(QDBusMessage::createMethodCall(service, path, promptInterface, QStringLiteral("Dismiss")),
             [](const QDBusMessage &) {});
        next(failure(Error::Failed, tr("The keyring's prompt was not answered in time.")));
    });
    QDBusMessage message = QDBusMessage::createMethodCall(service, path, promptInterface, QStringLiteral("Prompt"));
    // No parent window: Wayland has no window id to give the service.
    message << QString();
    call(message, [this, path, serial](const QDBusMessage &reply) {
        if (!isError(reply))
            return;
        const auto waiting = m_prompts.constFind(path);
        if (waiting == m_prompts.cend() || waiting->serial != serial)
            return;
        takePrompt(path)(failure(reply));
    });
}

void SecretServiceKeyStore::contains(const QString &npub, Done done) {
    if (!connected())
        return later(done, failure(Error::Unavailable));
    search(npub, [done](const Paths &unlocked, const Paths &locked, const Result &status) {
        Result result = status;
        result.found = status.error == Error::None && (!unlocked.isEmpty() || !locked.isEmpty());
        done(result);
    });
}

void SecretServiceKeyStore::lookup(const QString &npub, Done done) {
    if (!connected())
        return later(done, failure(Error::Unavailable));
    const auto profile = Nip19::decodeProfile(npub);
    const QString pubkey = profile ? profile->pubkey : QString();
    search(npub, [this, done, pubkey](const Paths &unlocked, const Paths &locked, const Result &status) {
        if (status.error != Error::None)
            return done(status);
        if (unlocked.isEmpty() && locked.isEmpty())
            return done({});
        // Any program of this user's can add an item with these attributes,
        // so one that is not the account's key is passed over rather than
        // left to hide the real one. The unlocked ones come first, as they
        // cost no prompt.
        readItems(unlocked + locked, unlocked.size(), 0, pubkey, done);
    });
}

void SecretServiceKeyStore::readItems(const Paths &items, qsizetype unlocked, qsizetype next, const QString &pubkey,
                                      Done done) {
    if (next == items.size())
        return done(failure(Error::Failed, tr("No item in the keyring holds this account's key.")));
    const QDBusObjectPath item = items.at(next);
    const auto skip = [this, items, unlocked, next, pubkey, done] {
        readItems(items, unlocked, next + 1, pubkey, done);
    };
    const auto read = [this, item, pubkey, done, skip](const Result &status) {
        if (status.error != Error::None)
            return done(status);
        openSession([this, item, pubkey, done, skip](const QString &session, const Result &status) {
            if (status.error != Error::None)
                return done(status);
            QDBusMessage message =
                QDBusMessage::createMethodCall(service, item.path(), itemInterface, QStringLiteral("GetSecret"));
            message << QVariant::fromValue(QDBusObjectPath(session));
            call(message, [this, session, pubkey, done, skip](const QDBusMessage &reply) {
                closeSession(session);
                if (isError(reply))
                    return done(failure(reply));
                auto secret = argument<Secret>(reply, 0, "(oayays)");
                if (!secret)
                    return skip();
                // store() writes hex, and an nsec someone put there by hand
                // is taken too; anything else is not a key. The value is
                // wiped where it is, which also reaches a reply that shares
                // it; the copy libdbus read it from is freed unwiped, out of
                // Readstr's reach.
                QString text = QString::fromLatin1(secret->value);
                auto key = Nip19::secretKey(text);
                wipe(text);
                wipeShared(secret->value);
                secret->value.clear();
                const auto signer = key ? LocalSigner::create(*key) : nullptr;
                if (!signer || signer->pubkey() != pubkey) {
                    if (key)
                        wipe(*key);
                    return skip();
                }
                Result result;
                result.found = true;
                result.secret = std::move(*key);
                done(result);
                wipe(result.secret);
            });
        });
    };
    if (next < unlocked)
        read({});
    else
        unlock({item}, read);
}

void SecretServiceKeyStore::store(const QString &npub, const QByteArray &secret, Done done) {
    if (!connected())
        return later(done, failure(Error::Unavailable));
    // A copy of its own rather than one sharing the caller's buffer, so the
    // wipes here and the caller's each reach the bytes they mean to.
    auto key = std::make_shared<QByteArray>(secret.constData(), secret.size());
    openSession([this, npub, key, done](const QString &session, const Result &status) {
        if (status.error != Error::None) {
            wipe(*key);
            return done(status);
        }
        createItem(session, npub, key, false, done);
    });
}

void SecretServiceKeyStore::createItem(const QString &session, const QString &npub,
                                       const std::shared_ptr<QByteArray> &key, bool unlocked, Done done) {
    QVariantMap properties;
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"), tr("Readstr Nostr key (%1)").arg(npub));
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Attributes"), QVariant::fromValue(attributes(npub)));
    // The hex is shared with the message, which QtDBus keeps until the reply
    // is in, so it is wiped in place once the reply comes, or once the call
    // is dropped with this store; wiping a copy would leave the message's.
    // libdbus serializes the message into a buffer of its own, which it
    // frees unwiped: that copy is out of Readstr's reach.
    const std::shared_ptr<const QByteArray> hex(new QByteArray(key->toHex()), [](const QByteArray *bytes) {
        wipeShared(*bytes);
        delete bytes;
    });
    QDBusMessage message =
        QDBusMessage::createMethodCall(service, defaultCollection, collectionInterface, QStringLiteral("CreateItem"));
    // Replacing the item with the same attributes, so a key stored again is
    // one item, not two.
    message << properties
            << QVariant::fromValue(Secret{QDBusObjectPath(session), {}, *hex, QStringLiteral("text/plain")}) << true;
    call(message, [this, session, npub, key, unlocked, done, hex](const QDBusMessage &reply) {
        wipeShared(*hex);
        const auto finish = [this, session, key, done](const Result &result) {
            closeSession(session);
            wipe(*key);
            done(result);
        };
        if (isError(reply)) {
            Result result = failure(reply);
            // A locked collection is unlocked once and the item made
            // again; still locked after that, it stays a failure.
            if (result.error == Error::Locked && !unlocked)
                return unlock({QDBusObjectPath(defaultCollection)},
                              [this, session, npub, key, done, finish](const Result &status) {
                                  if (status.error != Error::None)
                                      return finish(status);
                                  createItem(session, npub, key, true, done);
                              });
            return finish(result);
        }
        const auto item = argument<QDBusObjectPath>(reply, 0, "o");
        const auto promptPath = argument<QDBusObjectPath>(reply, 1, "o");
        if (!item || !promptPath)
            return finish(unexpected());
        if (promptPath->path() == noPrompt)
            return finish({});
        prompt(promptPath->path(), finish);
    });
}

void SecretServiceKeyStore::remove(const QString &npub, Done done) {
    if (!connected())
        return later(done, failure(Error::Unavailable));
    search(npub, [this, done](const Paths &unlocked, const Paths &locked, const Result &status) {
        if (status.error != Error::None)
            return done(status);
        const Paths items = unlocked + locked;
        if (items.isEmpty())
            return done({});
        if (locked.isEmpty())
            return deleteItems(items, 0, done);
        unlock(locked, [this, items, done](const Result &status) {
            if (status.error != Error::None)
                return done(status);
            deleteItems(items, 0, done);
        });
    });
}

void SecretServiceKeyStore::deleteItems(const Paths &items, qsizetype next, Done done) {
    if (next == items.size()) {
        Result result;
        result.found = true;
        return done(result);
    }
    call(QDBusMessage::createMethodCall(service, items.at(next).path(), itemInterface, QStringLiteral("Delete")),
         [this, items, next, done](const QDBusMessage &reply) {
             if (isError(reply))
                 return done(failure(reply));
             const auto promptPath = argument<QDBusObjectPath>(reply, 0, "o");
             if (!promptPath)
                 return done(unexpected());
             const auto deleted = [this, items, next, done](const Result &status) {
                 if (status.error != Error::None)
                     return done(status);
                 deleteItems(items, next + 1, done);
             };
             if (promptPath->path() == noPrompt)
                 return deleted({});
             prompt(promptPath->path(), deleted);
         });
}
