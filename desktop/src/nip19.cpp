#include "nip19.h"

#include <QList>
#include <QPair>
#include <QUrl>
#include <algorithm>
#include <cstring>

namespace {
const char charset[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

quint32 polymod(const QList<quint8> &values) {
    static const quint32 generator[] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
    quint32 checksum = 1;
    for (quint8 value : values) {
        const quint32 top = checksum >> 25;
        checksum = ((checksum & 0x1ffffff) << 5) ^ value;
        for (int i = 0; i < 5; ++i)
            if ((top >> i) & 1)
                checksum ^= generator[i];
    }
    return checksum;
}

QList<quint8> expandPrefix(const QByteArray &prefix) {
    QList<quint8> values;
    for (char c : prefix)
        values << quint8(quint8(c) >> 5);
    values << 0;
    for (char c : prefix)
        values << quint8(quint8(c) & 31);
    return values;
}

// Regroups bits; `pad` is true going to 5-bit words and false coming back,
// where leftover bits must be zero padding.
std::optional<QByteArray> convertBits(const QByteArray &data, int from, int to, bool pad) {
    QByteArray out;
    quint32 accumulator = 0;
    int bits = 0;
    const quint32 mask = (1u << to) - 1;
    for (char c : data) {
        const quint32 value = quint8(c);
        if (value >> from)
            return std::nullopt;
        accumulator = (accumulator << from) | value;
        bits += from;
        while (bits >= to) {
            bits -= to;
            out.append(char((accumulator >> bits) & mask));
        }
    }
    if (pad) {
        if (bits)
            out.append(char((accumulator << (to - bits)) & mask));
    } else if (bits >= from || ((accumulator << (to - bits)) & mask)) {
        return std::nullopt;
    }
    return out;
}

struct Decoded {
    QByteArray prefix;
    QByteArray data;
};

std::optional<Decoded> decode(const QString &input) {
    if (input.size() < 8 || input.size() > Nip19::maxLength)
        return std::nullopt;
    // Bech32 is printable ASCII. Anything else is refused before the case
    // is folded, since some characters outside it fold into it: the Kelvin
    // sign U+212A lowercases to "k".
    for (const QChar c : input)
        if (c.unicode() < 0x21 || c.unicode() > 0x7e)
            return std::nullopt;
    const bool lower = input == input.toLower();
    if (!lower && input != input.toUpper())
        return std::nullopt;
    const QByteArray text = input.toLower().toLatin1();
    const qsizetype separator = text.lastIndexOf('1');
    if (separator < 1 || separator + 7 > text.size())
        return std::nullopt;
    Decoded decoded;
    decoded.prefix = text.left(separator);
    for (char c : decoded.prefix)
        if (c < 33 || c > 126)
            return std::nullopt;
    QList<quint8> values = expandPrefix(decoded.prefix);
    QByteArray words;
    for (qsizetype i = separator + 1; i < text.size(); ++i) {
        const char *found = strchr(charset, text.at(i));
        if (!found || !*found)
            return std::nullopt;
        values << quint8(found - charset);
        words.append(char(found - charset));
    }
    if (polymod(values) != 1)
        return std::nullopt;
    words.chop(6);
    const auto data = convertBits(words, 5, 8, false);
    if (!data)
        return std::nullopt;
    decoded.data = *data;
    return decoded;
}

QString encode(const QByteArray &prefix, const QByteArray &data) {
    const auto words = convertBits(data, 8, 5, true);
    QList<quint8> values = expandPrefix(prefix);
    for (char w : *words)
        values << quint8(w);
    for (int i = 0; i < 6; ++i)
        values << 0;
    const quint32 checksum = polymod(values) ^ 1;
    QByteArray out = prefix + '1';
    for (char w : *words)
        out.append(charset[quint8(w)]);
    for (int i = 0; i < 6; ++i)
        out.append(charset[(checksum >> (5 * (5 - i))) & 31]);
    return QString::fromLatin1(out);
}

QByteArray keyBytes(const QString &hex) {
    return Nip19::isHexKey(hex) ? QByteArray::fromHex(hex.toLatin1()) : QByteArray();
}

// A NIP-19 TLV list as (type, value) pairs in order, or null when one runs
// past the end.
std::optional<QList<QPair<int, QByteArray>>> tlvEntries(const QByteArray &data) {
    QList<QPair<int, QByteArray>> entries;
    qsizetype at = 0;
    while (at < data.size()) {
        if (at + 2 > data.size())
            return std::nullopt;
        const int type = quint8(data.at(at));
        const int length = quint8(data.at(at + 1));
        at += 2;
        if (at + length > data.size())
            return std::nullopt;
        entries.append({type, data.mid(at, length)});
        at += length;
    }
    return entries;
}

// The first value of `type`, or null when there is none.
std::optional<QByteArray> firstOf(const QList<QPair<int, QByteArray>> &entries, int type) {
    for (const auto &[entryType, value] : entries)
        if (entryType == type)
            return value;
    return std::nullopt;
}

// Whether a TLV entity has the fields nostr-tools requires of it, the first
// of each type being the one it reads: for nprofile and nevent a 32-byte
// key or id (type 0), for nprofile only one, and for nevent an author (type
// 2) of 32 bytes and a kind (type 3) of 4 when there is one; for naddr a d
// tag (type 0, which may be empty), an author of 32 bytes, and a kind of 4.
// Unknown types are skipped, as NIP-19 asks.
bool hasRequiredFields(const QByteArray &prefix, const QByteArray &data) {
    const auto entries = tlvEntries(data);
    if (!entries)
        return false;
    const auto sized = [&](int type, int length, bool required) {
        const auto value = firstOf(*entries, type);
        return value ? value->size() == length : !required;
    };
    // An nprofile names one key: with two, which one it means is a guess, so
    // it is refused here as it is when subscribing.
    if (prefix == "nprofile") {
        const auto keys =
            std::count_if(entries->begin(), entries->end(), [](const auto &entry) { return entry.first == 0; });
        return sized(0, 32, true) && keys == 1;
    }
    if (prefix == "nevent")
        return sized(0, 32, true) && sized(2, 32, false) && sized(3, 4, false);
    if (prefix == "naddr")
        return firstOf(*entries, 0).has_value() && sized(2, 32, true) && sized(3, 4, true);
    return false;
}

bool isRelayUrl(const QString &text) {
    const QUrl url(text, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == "wss" || scheme == "ws") && !url.host().isEmpty() && url.userInfo().isEmpty();
}
}

namespace Nip19 {
bool isHex(const QString &text, int length) {
    if (text.size() != length)
        return false;
    for (QChar c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

bool isHexKey(const QString &text) {
    return isHex(text, 64);
}

std::optional<Profile> decodeProfile(const QString &input) {
    QString text = input.trimmed();
    if (text.startsWith(QLatin1String("nostr:"), Qt::CaseInsensitive))
        text = text.mid(6);
    const auto decoded = decode(text);
    if (!decoded)
        return std::nullopt;
    Profile profile;
    if (decoded->prefix == "npub") {
        if (decoded->data.size() != 32)
            return std::nullopt;
        profile.pubkey = QString::fromLatin1(decoded->data.toHex());
        return profile;
    }
    if (decoded->prefix != "nprofile")
        return std::nullopt;
    // TLV: type 0 is the public key (exactly once), type 1 a relay; other
    // types are skipped, as NIP-19 asks.
    const auto entries = tlvEntries(decoded->data);
    if (!entries || !hasRequiredFields(decoded->prefix, decoded->data))
        return std::nullopt;
    for (const auto &[type, value] : *entries) {
        if (type == 0) {
            profile.pubkey = QString::fromLatin1(value.toHex());
        } else if (type == 1 && profile.relays.size() < maxRelayHints) {
            const QString relay = QString::fromUtf8(value);
            if (isRelayUrl(relay) && !profile.relays.contains(relay))
                profile.relays << relay;
        }
    }
    if (profile.pubkey.isEmpty())
        return std::nullopt;
    return profile;
}

QString npub(const QString &pubkeyHex) {
    const QByteArray key = keyBytes(pubkeyHex);
    return key.isEmpty() ? QString() : encode("npub", key);
}

QString entity(const QString &text) {
    const auto decoded = decode(text);
    if (!decoded)
        return {};
    const QByteArray &prefix = decoded->prefix;
    if (prefix == "npub" || prefix == "note") {
        if (decoded->data.size() != 32)
            return {};
    } else if (!hasRequiredFields(prefix, decoded->data)) {
        return {};
    }
    return text.toLower();
}

QString nevent(const QString &eventIdHex, const QString &authorHex, int kind) {
    const QByteArray id = keyBytes(eventIdHex);
    const QByteArray author = keyBytes(authorHex);
    if (id.isEmpty() || author.isEmpty() || kind < 0)
        return {};
    QByteArray tlv;
    tlv.append(char(3)).append(char(4));
    for (int shift = 24; shift >= 0; shift -= 8)
        tlv.append(char((quint32(kind) >> shift) & 0xff));
    tlv.append(char(2)).append(char(32)).append(author);
    tlv.append(char(0)).append(char(32)).append(id);
    return encode("nevent", tlv);
}

QString naddr(int kind, const QString &pubkeyHex, const QString &identifier) {
    const QByteArray key = keyBytes(pubkeyHex);
    const QByteArray d = identifier.toUtf8();
    if (key.isEmpty() || d.size() > 255 || kind < 0)
        return {};
    QByteArray tlv;
    tlv.append(char(0)).append(char(d.size())).append(d);
    tlv.append(char(2)).append(char(32)).append(key);
    tlv.append(char(3)).append(char(4));
    for (int shift = 24; shift >= 0; shift -= 8)
        tlv.append(char((quint32(kind) >> shift) & 0xff));
    return encode("naddr", tlv);
}
}
