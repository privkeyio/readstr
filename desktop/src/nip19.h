#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <optional>

// NIP-19 bech32 entities: npub and nprofile in (what people paste to follow
// an author), nsec in (a signing key), npub, naddr, and nevent out, and any
// entity checked for links.
// Everything here is pure and bounded, since what comes in is untrusted text.
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
// A feed's title as shown: when it is the feed's own npub (`feedUrl`), its
// first 9 and last 4 characters around an ellipsis ("npub1abcd…wxyz"), and
// otherwise as it is. A Nostr feed is titled with its npub until the
// author's profile names it, and 63 characters of bech32 crowd out
// everything else where titles show. Only the feed's own npub is shortened,
// so a profile naming itself after someone else's npub shows in full rather
// than as that other author's placeholder.
QString shortNpub(const QString &title, const QString &feedUrl);
// The naddr for a parameterized replaceable event (kind, author, d tag).
QString naddr(int kind, const QString &pubkeyHex, const QString &identifier);
// The nevent for an event id, with its author and kind, in the order
// nostr-tools writes them, so the link is the one the web app makes.
QString nevent(const QString &eventIdHex, const QString &authorHex, int kind);
// The entity, lowercase, when `text` is a well-formed npub, nprofile, note,
// nevent, or naddr (printable ASCII, its charset, checksum, and length within
// maxLength, a key or id of 32 bytes for npub and note, and the TLV fields
// nostr-tools requires for the others); empty for anything else.
QString entity(const QString &text);

// The 32 bytes of a secret key given as an nsec or as 64 hex characters
// (either case), with surrounding whitespace, or null for anything else.
// Whether secp256k1 accepts the bytes as a key is the signer's to check.
// NIP-21 forbids "nostr:nsec", so a "nostr:" prefix is refused here.
std::optional<QByteArray> secretKey(const QString &text);

// Lowercase hex of exactly `length` characters.
bool isHex(const QString &text, int length);
bool isHexKey(const QString &text);
}
