#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <optional>

// NIP-19 bech32 entities: npub and nprofile in (what people paste to follow
// an author), npub and naddr out. Everything here is pure and bounded, since
// what comes in is untrusted text.
namespace Nip19 {
// Longer input is refused before decoding. An nprofile with a handful of
// relay hints is a few hundred characters.
inline constexpr int maxLength = 2000;
inline constexpr int maxRelayHints = 8;

struct Profile {
    // 32 bytes as 64 lowercase hex characters.
    QString pubkey;
    // ws:// or wss:// URLs the entity named, at most maxRelayHints.
    QStringList relays;
};

// Accepts an npub or nprofile, with or without a leading "nostr:", and
// returns null for anything else, including a checksum that does not match.
std::optional<Profile> decodeProfile(const QString &text);
// The npub for a 64-character hex public key, or empty when it is not one.
QString npub(const QString &pubkeyHex);
// The naddr for a parameterized replaceable event (kind, author, d tag).
QString naddr(int kind, const QString &pubkeyHex, const QString &identifier);

// Lowercase hex of exactly `length` characters.
bool isHex(const QString &text, int length);
bool isHexKey(const QString &text);
}
