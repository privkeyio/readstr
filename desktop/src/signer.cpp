#include "signer.h"

#include <QCryptographicHash>
#include <QTimer>
#include <cerrno>
#include <secp256k1_schnorrsig.h>
#include <sys/random.h>

#include "wipe.h"

namespace {
// Fills `out` from the kernel's CSPRNG, which getrandom() without flags
// waits on until it is seeded; false only when that call fails.
bool systemRandom(unsigned char *out, size_t size) {
    size_t filled = 0;
    while (filled < size) {
        const ssize_t got = getrandom(out + filled, size - filled, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        filled += size_t(got);
    }
    return true;
}

const unsigned char *bytes(const QByteArray &data) {
    return reinterpret_cast<const unsigned char *>(data.constData());
}
}

LocalSigner::LocalSigner() {
    explicit_bzero(&m_keypair, sizeof m_keypair);
}

LocalSigner::~LocalSigner() {
    explicit_bzero(&m_keypair, sizeof m_keypair);
    if (m_context)
        secp256k1_context_destroy(m_context);
}

std::unique_ptr<LocalSigner> LocalSigner::create(const QByteArray &secret) {
    if (secret.size() != 32 || !secp256k1_ec_seckey_verify(secp256k1_context_static, bytes(secret)))
        return nullptr;
    std::unique_ptr<LocalSigner> signer(new LocalSigner);
    signer->m_context = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!signer->m_context)
        return nullptr;
    // A randomized context blinds the computations that use the secret key
    // against timing and power side channels, as libsecp256k1 recommends
    // for any context that signs.
    unsigned char seed[32];
    const bool randomized = systemRandom(seed, sizeof seed) && secp256k1_context_randomize(signer->m_context, seed);
    explicit_bzero(seed, sizeof seed);
    secp256k1_xonly_pubkey xonly;
    unsigned char pubkey[32];
    if (!randomized || !secp256k1_keypair_create(signer->m_context, &signer->m_keypair, bytes(secret))
        || !secp256k1_keypair_xonly_pub(signer->m_context, &xonly, nullptr, &signer->m_keypair)
        || !secp256k1_xonly_pubkey_serialize(signer->m_context, pubkey, &xonly))
        return nullptr;
    signer->m_pubkey = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(pubkey), 32).toHex());
    return signer;
}

QByteArray LocalSigner::signDigest(const QByteArray &digest, const QByteArray &aux) const {
    if (digest.size() != 32 || aux.size() != 32)
        return {};
    unsigned char signature[64];
    if (!secp256k1_schnorrsig_sign32(m_context, signature, bytes(digest), &m_keypair, bytes(aux)))
        return {};
    return QByteArray(reinterpret_cast<const char *>(signature), 64);
}

std::pair<NostrEvent, QString> LocalSigner::signNow(const NostrEvent &event) const {
    // The ranges NostrEvent::fromJson() accepts, so what is signed here is
    // an event the app itself, and so a relay, would take.
    if (event.kind < 0 || event.kind > 65535 || event.createdAt < 0 || event.createdAt > 1000000000000LL)
        return {{}, tr("Cannot sign this event: its kind or date is out of range")};
    NostrEvent result = event;
    result.pubkey = m_pubkey;
    const QByteArray digest = QCryptographicHash::hash(result.serialized(), QCryptographicHash::Sha256);
    QByteArray aux(32, '\0');
    if (!systemRandom(reinterpret_cast<unsigned char *>(aux.data()), size_t(aux.size())))
        return {{}, tr("Cannot sign: the system's random number generator failed")};
    const QByteArray signature = signDigest(digest, aux);
    wipe(aux);
    result.id = QString::fromLatin1(digest.toHex());
    result.sig = QString::fromLatin1(signature.toHex());
    // Checked the way every reader will check it, which also catches a
    // fault while signing before anything leaves.
    if (signature.isEmpty() || !result.verify())
        return {{}, tr("Cannot sign: libsecp256k1 refused the event")};
    return {result, {}};
}

void LocalSigner::sign(const NostrEvent &event, Done done) {
    auto result = signNow(event);
    QTimer::singleShot(0, this, [done = std::move(done), result = std::move(result)] {
        done(result.first, result.second);
    });
}
