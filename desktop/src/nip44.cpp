#include "nip44.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <secp256k1.h>
#include <secp256k1_ecdh.h>

#include "wipe.h"

namespace {
constexpr char version = 2;
constexpr int keySize = 32;
constexpr int nonceSize = 32;
constexpr int macSize = 32;

const unsigned char *bytes(const QByteArray &data) {
    return reinterpret_cast<const unsigned char *>(data.constData());
}

unsigned char *bytes(QByteArray &data) {
    return reinterpret_cast<unsigned char *>(data.data());
}

// The x coordinate of the shared point as it is, where libsecp256k1's
// default would hash it: NIP-44 feeds the bare x to HKDF instead.
int copyX(unsigned char *output, const unsigned char *x32, const unsigned char *, void *) {
    memcpy(output, x32, 32);
    return 1;
}

// HKDF-SHA256 in one of OpenSSL's single-step modes: extract, where `key`
// is the input keying material, or expand, where it is the pseudorandom key
// and `saltOrInfo` the info.
QByteArray hkdf(int mode, const QByteArray &key, const QByteArray &saltOrInfo, qsizetype length) {
    EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    EVP_KDF_CTX *context = kdf ? EVP_KDF_CTX_new(kdf) : nullptr;
    EVP_KDF_free(kdf);
    if (!context)
        return {};
    char digest[] = "SHA256";
    const bool extract = mode == EVP_KDF_HKDF_MODE_EXTRACT_ONLY;
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE, &mode),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, const_cast<char *>(key.constData()),
                                          size_t(key.size())),
        OSSL_PARAM_construct_octet_string(extract ? OSSL_KDF_PARAM_SALT : OSSL_KDF_PARAM_INFO,
                                          const_cast<char *>(saltOrInfo.constData()), size_t(saltOrInfo.size())),
        OSSL_PARAM_construct_end(),
    };
    QByteArray out(length, '\0');
    const bool derived = EVP_KDF_derive(context, bytes(out), size_t(length), params) == 1;
    EVP_KDF_CTX_free(context);
    if (!derived)
        wipe(out);
    return out;
}

// ChaCha20 as NIP-44 has it, with the block counter starting at 0: OpenSSL
// takes a 16-byte IV that is the counter, little-endian, then the nonce.
// Encrypting and decrypting are the same keystream XOR.
QByteArray chacha20(const QByteArray &key, const QByteArray &nonce, const QByteArray &input) {
    QByteArray iv(4, '\0');
    iv.append(nonce);
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (!context)
        return {};
    QByteArray out(input.size(), '\0');
    int written = 0, finished = 0;
    const bool ok = EVP_EncryptInit_ex(context, EVP_chacha20(), nullptr, bytes(key), bytes(iv)) == 1
        && EVP_EncryptUpdate(context, bytes(out), &written, bytes(input), int(input.size())) == 1
        && EVP_EncryptFinal_ex(context, bytes(out) + written, &finished) == 1 && written + finished == input.size();
    EVP_CIPHER_CTX_free(context);
    if (!ok)
        wipe(out);
    return out;
}

// HMAC-SHA256 of the nonce and then the ciphertext, the order NIP-44 fixes.
QByteArray mac(const QByteArray &key, const QByteArray &nonce, const QByteArray &ciphertext) {
    const QByteArray message = nonce + ciphertext;
    QByteArray out(macSize, '\0');
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.constData(), int(key.size()), bytes(message), size_t(message.size()), bytes(out),
              &length)
        || length != macSize)
        return {};
    return out;
}
}

void Nip44::MessageKeys::wipe() {
    ::wipe(chachaKey);
    ::wipe(chachaNonce);
    ::wipe(hmacKey);
}

QByteArray Nip44::conversationKey(const QByteArray &secret, const QByteArray &pubkey) {
    if (secret.size() != keySize || pubkey.size() != keySize
        || !secp256k1_ec_seckey_verify(secp256k1_context_static, bytes(secret)))
        return {};
    // An x-only key stands for the point with the even y, which is the
    // compressed encoding with the 02 prefix. A parse fails for an x with no
    // point on the curve, which also covers the twist's points.
    QByteArray compressed(1, '\x02');
    compressed.append(pubkey);
    secp256k1_pubkey point;
    if (!secp256k1_ec_pubkey_parse(secp256k1_context_static, &point, bytes(compressed), size_t(compressed.size())))
        return {};
    QByteArray shared(keySize, '\0');
    if (!secp256k1_ecdh(secp256k1_context_static, bytes(shared), &point, bytes(secret), copyX, nullptr)) {
        wipe(shared);
        return {};
    }
    QByteArray key = hkdf(EVP_KDF_HKDF_MODE_EXTRACT_ONLY, shared, QByteArrayLiteral("nip44-v2"), keySize);
    wipe(shared);
    return key;
}

std::optional<Nip44::MessageKeys> Nip44::messageKeys(const QByteArray &conversationKey, const QByteArray &nonce) {
    if (conversationKey.size() != keySize || nonce.size() != nonceSize)
        return std::nullopt;
    QByteArray expanded = hkdf(EVP_KDF_HKDF_MODE_EXPAND_ONLY, conversationKey, nonce, 76);
    if (expanded.size() != 76)
        return std::nullopt;
    MessageKeys keys{expanded.left(32), expanded.mid(32, 12), expanded.mid(44, 32)};
    wipe(expanded);
    return keys;
}

qsizetype Nip44::paddedLength(qsizetype length) {
    if (length <= 32)
        return 32;
    qsizetype nextPower = 1;
    while (nextPower < length)
        nextPower <<= 1;
    const qsizetype chunk = nextPower <= 256 ? 32 : nextPower / 8;
    return chunk * ((length - 1) / chunk + 1);
}

std::optional<QString> Nip44::encrypt(const QByteArray &plaintext, const QByteArray &conversationKey) {
    QByteArray nonce(nonceSize, '\0');
    if (RAND_bytes(bytes(nonce), nonceSize) != 1)
        return std::nullopt;
    return encrypt(plaintext, conversationKey, nonce);
}

std::optional<QString> Nip44::encrypt(const QByteArray &plaintext, const QByteArray &conversationKey,
                                      const QByteArray &nonce) {
    if (plaintext.size() < minPlaintext || plaintext.size() > maxPlaintext)
        return std::nullopt;
    auto keys = messageKeys(conversationKey, nonce);
    if (!keys)
        return std::nullopt;
    QByteArray padded(2 + paddedLength(plaintext.size()), '\0');
    padded[0] = char(plaintext.size() >> 8);
    padded[1] = char(plaintext.size() & 0xff);
    memcpy(padded.data() + 2, plaintext.constData(), size_t(plaintext.size()));
    const QByteArray ciphertext = chacha20(keys->chachaKey, keys->chachaNonce, padded);
    wipe(padded);
    const QByteArray tag = ciphertext.isEmpty() ? QByteArray() : mac(keys->hmacKey, nonce, ciphertext);
    keys->wipe();
    if (tag.isEmpty())
        return std::nullopt;
    return QString::fromLatin1((QByteArray(1, version) + nonce + ciphertext + tag).toBase64());
}

std::optional<QByteArray> Nip44::decrypt(const QString &payload, const QByteArray &conversationKey) {
    // A leading '#' marks a payload of a version that is not base64 at all.
    if (payload.isEmpty() || payload.startsWith('#') || payload.size() < minPayload || payload.size() > maxPayload)
        return std::nullopt;
    const auto decoded = QByteArray::fromBase64Encoding(payload.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.toBase64() != payload.toLatin1())
        return std::nullopt;
    const QByteArray data = decoded.decoded;
    if (data.size() < 99 || data.size() > 65603 || data.at(0) != version)
        return std::nullopt;
    const QByteArray nonce = data.mid(1, nonceSize);
    const QByteArray ciphertext = data.mid(1 + nonceSize, data.size() - 1 - nonceSize - macSize);
    const QByteArray tag = data.right(macSize);
    auto keys = messageKeys(conversationKey, nonce);
    if (!keys)
        return std::nullopt;
    const QByteArray expected = mac(keys->hmacKey, nonce, ciphertext);
    if (expected.size() != macSize || CRYPTO_memcmp(expected.constData(), tag.constData(), macSize) != 0) {
        keys->wipe();
        return std::nullopt;
    }
    QByteArray padded = chacha20(keys->chachaKey, keys->chachaNonce, ciphertext);
    keys->wipe();
    if (padded.size() < 2) {
        wipe(padded);
        return std::nullopt;
    }
    const qsizetype length = (qsizetype(quint8(padded.at(0))) << 8) | quint8(padded.at(1));
    if (length < minPlaintext || padded.size() != 2 + paddedLength(length)) {
        wipe(padded);
        return std::nullopt;
    }
    QByteArray plaintext = padded.mid(2, length);
    wipe(padded);
    return plaintext;
}
