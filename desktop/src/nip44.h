#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

// NIP-44 version 2: the encryption NIP-46 wraps its requests in. A
// conversation key is derived once per pair of keys with secp256k1 ECDH and
// HKDF; each message gets a fresh 32-byte nonce, from which HKDF derives its
// ChaCha20 key and nonce and its HMAC-SHA256 key. The plaintext is padded so
// its length says little, and the MAC covers the nonce and the ciphertext.
// Everything a peer sends is untrusted: a payload is checked in full, its MAC
// in constant time, before any of it is decrypted or believed.
class Nip44 {
public:
    // The plaintext's bounds in bytes, which the padding's two-byte length
    // prefix sets.
    static constexpr qsizetype minPlaintext = 1;
    static constexpr qsizetype maxPlaintext = 65535;
    // A payload's bounds in base64 characters, past which it is refused
    // before it is decoded.
    static constexpr qsizetype minPayload = 132;
    static constexpr qsizetype maxPayload = 87472;

    // The 32-byte conversation key of the secret key `secret` (32 bytes) and
    // the x-only public key `pubkey` (32 bytes), the same from either side;
    // empty when the secret key is out of range or the public key is not on
    // the curve. The caller wipes it once done.
    static QByteArray conversationKey(const QByteArray &secret, const QByteArray &pubkey);
    // The base64 payload of `plaintext` (UTF-8 bytes), or null when its
    // length is out of bounds, the key is not 32 bytes, or the system's
    // random number generator fails.
    static std::optional<QString> encrypt(const QByteArray &plaintext, const QByteArray &conversationKey);
    // The plaintext of `payload`, or null for anything NIP-44 v2 refuses: an
    // unknown version, a length out of bounds, base64 that is not strict, a
    // MAC that does not match, or padding that is not as encrypt() writes it.
    static std::optional<QByteArray> decrypt(const QString &payload, const QByteArray &conversationKey);
    // How long the padded plaintext of `length` bytes is, past the two-byte
    // length prefix.
    static qsizetype paddedLength(qsizetype length);

private:
    // The tests check these against the NIP-44 vectors, which fix the nonce.
    // Private, so that nothing can come to encrypt twice under one nonce.
    friend class ReadstrTests;

    struct MessageKeys {
        QByteArray chachaKey;
        QByteArray chachaNonce;
        QByteArray hmacKey;
        void wipe();
    };
    static std::optional<MessageKeys> messageKeys(const QByteArray &conversationKey, const QByteArray &nonce);
    static std::optional<QString> encrypt(const QByteArray &plaintext, const QByteArray &conversationKey,
                                          const QByteArray &nonce);
};
