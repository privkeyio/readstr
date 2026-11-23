#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include "nostrevent.h"

// Signs Nostr events as one account. Signing is asynchronous, whatever the
// signer: a remote one (NIP-46) waits on a relay and a person, so callers
// are written for that from the start, and `done` never runs inside sign().
class Signer : public QObject {
    Q_OBJECT

public:
    // The signed event, or an empty one and why it could not be signed.
    using Done = std::function<void(const NostrEvent &event, const QString &error)>;

    using QObject::QObject;

    // The account's public key, as 64 lowercase hex characters.
    virtual QString pubkey() const = 0;
    // Signs `event`'s kind, created_at, tags, and content: its pubkey, id,
    // and sig are filled in, and whatever they held is ignored. `done` runs
    // later on this thread, and never once the signer is gone.
    virtual void sign(const NostrEvent &event, Done done) = 0;
};

// Signs with a secret key held in this process: BIP-340 Schnorr through
// libsecp256k1, with fresh auxiliary randomness from the kernel for every
// signature. The key lives only as long as the signer, which wipes it.
class LocalSigner : public Signer {
    Q_OBJECT

public:
    // Null unless `secret` is 32 bytes that secp256k1 accepts as a secret
    // key (neither zero nor past the group order).
    static std::unique_ptr<LocalSigner> create(const QByteArray &secret);
    ~LocalSigner() override;

    QString pubkey() const override { return m_pubkey; }
    void sign(const NostrEvent &event, Done done) override;

    // What sign() hands `done`, at once.
    std::pair<NostrEvent, QString> signNow(const NostrEvent &event) const;

private:
    // The tests check signDigest() against the BIP-340 vectors.
    friend class ReadstrTests;

    LocalSigner();
    // The 64-byte BIP-340 signature of a 32-byte `digest` with `aux` (32
    // bytes) as its auxiliary randomness, or empty. sign() draws `aux`
    // fresh; a fixed one is only for the BIP-340 test vectors. Private, so
    // that nothing can come to sign an arbitrary digest that is not a Nostr
    // event's id.
    QByteArray signDigest(const QByteArray &digest, const QByteArray &aux) const;

    secp256k1_context *m_context = nullptr;
    secp256k1_keypair m_keypair;
    QString m_pubkey;
};
