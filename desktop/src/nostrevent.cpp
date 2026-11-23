#include "nostrevent.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

#include "nip19.h"

namespace {
using Nip19::isHex;

// JSON.stringify's escaping, which is what NIP-01 and every client hash:
// quote, backslash, and control characters escaped, everything else as
// UTF-8. Lone surrogates come out as \uXXXX, as JSON.stringify writes them
// since ES2019. Runs that need no escaping are converted in one go, since a
// relay can send megabytes of text to hash.
void appendString(QByteArray &out, const QString &text) {
    static const char hex[] = "0123456789abcdef";
    // \uXXXX, which for a control character is \u00XX.
    auto escape = [&out](char16_t c) {
        out.append("\\u").append(hex[c >> 12]).append(hex[(c >> 8) & 0xf]).append(hex[(c >> 4) & 0xf])
            .append(hex[c & 0xf]);
    };
    out.append('"');
    const char16_t *data = reinterpret_cast<const char16_t *>(text.constData());
    const qsizetype size = text.size();
    qsizetype run = 0;
    auto flush = [&](qsizetype end) {
        if (end > run)
            out.append(QStringView(data + run, end - run).toUtf8());
    };
    for (qsizetype i = 0; i < size; ++i) {
        const char16_t c = data[i];
        const bool pair = QChar::isHighSurrogate(c) && i + 1 < size && QChar::isLowSurrogate(data[i + 1]);
        if (pair) {
            ++i;
            continue;
        }
        if (c >= 0x20 && c != '"' && c != '\\' && !QChar::isSurrogate(c))
            continue;
        flush(i);
        run = i + 1;
        switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\b': out.append("\\b"); break;
        case '\f': out.append("\\f"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
            // Another control character, or a lone surrogate.
            escape(c);
        }
    }
    flush(size);
    out.append('"');
}
}

std::optional<NostrEvent> NostrEvent::fromJson(const QJsonValue &value) {
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject object = value.toObject();
    const QJsonValue id = object.value("id"), pubkey = object.value("pubkey"), createdAt = object.value("created_at"),
                     kind = object.value("kind"), tags = object.value("tags"), content = object.value("content"),
                     sig = object.value("sig");
    if (!id.isString() || !pubkey.isString() || !createdAt.isDouble() || !kind.isDouble() || !tags.isArray()
        || !content.isString() || !sig.isString())
        return std::nullopt;
    NostrEvent event;
    event.id = id.toString();
    event.pubkey = pubkey.toString();
    event.sig = sig.toString();
    if (!isHex(event.id, 64) || !isHex(event.pubkey, 64) || !isHex(event.sig, 128))
        return std::nullopt;
    // Whole numbers that serialize the same way they parsed: a fraction or
    // an exponent would hash differently than the author's client did.
    const double created = createdAt.toDouble(), kindNumber = kind.toDouble();
    if (created < 0 || created > 1e12 || created != qint64(created) || kindNumber < 0 || kindNumber > 65535
        || kindNumber != int(kindNumber))
        return std::nullopt;
    event.createdAt = qint64(created);
    event.kind = int(kindNumber);
    const QJsonArray tagArray = tags.toArray();
    if (tagArray.size() > maxTags)
        return std::nullopt;
    qint64 tagCharacters = 0;
    for (const QJsonValue &tag : tagArray) {
        if (!tag.isArray() || tag.toArray().size() > maxTagValues)
            return std::nullopt;
        QStringList values;
        for (const QJsonValue &part : tag.toArray()) {
            if (!part.isString())
                return std::nullopt;
            values << part.toString();
            tagCharacters += values.last().size();
            if (tagCharacters > maxTagCharacters)
                return std::nullopt;
        }
        event.tags << values;
    }
    event.content = content.toString();
    return event;
}

QJsonObject NostrEvent::toJson() const {
    QJsonArray tagArray;
    for (const QStringList &tag : tags)
        tagArray << QJsonArray::fromStringList(tag);
    return {{"id", id}, {"pubkey", pubkey},   {"created_at", createdAt}, {"kind", kind},
            {"tags", tagArray}, {"content", content}, {"sig", sig}};
}

QByteArray NostrEvent::jsonString(const QString &text) {
    QByteArray out;
    appendString(out, text);
    return out;
}

bool NostrEvent::tagsWithinLimits() const {
    if (tags.size() > maxTags)
        return false;
    qint64 tagCharacters = 0;
    for (const QStringList &tag : tags) {
        if (tag.size() > maxTagValues)
            return false;
        for (const QString &value : tag)
            tagCharacters += value.size();
    }
    return tagCharacters <= maxTagCharacters;
}

QByteArray NostrEvent::serialized() const {
    QByteArray out = "[0,";
    appendString(out, pubkey);
    out.append(',').append(QByteArray::number(createdAt)).append(',').append(QByteArray::number(kind)).append(",[");
    for (qsizetype t = 0; t < tags.size(); ++t) {
        if (t)
            out.append(',');
        out.append('[');
        for (qsizetype v = 0; v < tags.at(t).size(); ++v) {
            if (v)
                out.append(',');
            appendString(out, tags.at(t).at(v));
        }
        out.append(']');
    }
    out.append("],");
    appendString(out, content);
    out.append(']');
    return out;
}

bool NostrEvent::verify() const {
    const QByteArray hash = QCryptographicHash::hash(serialized(), QCryptographicHash::Sha256);
    if (hash.toHex() != id.toLatin1())
        return false;
    const QByteArray key = QByteArray::fromHex(pubkey.toLatin1());
    const QByteArray signature = QByteArray::fromHex(sig.toLatin1());
    // fromJson() checked the lengths, but an event can be built by hand.
    if (key.size() != 32 || signature.size() != 64)
        return false;
    secp256k1_xonly_pubkey xonly;
    if (!secp256k1_xonly_pubkey_parse(secp256k1_context_static, &xonly,
                                      reinterpret_cast<const unsigned char *>(key.constData())))
        return false;
    return secp256k1_schnorrsig_verify(secp256k1_context_static,
                                       reinterpret_cast<const unsigned char *>(signature.constData()),
                                       reinterpret_cast<const unsigned char *>(hash.constData()), 32, &xonly);
}

qint64 NostrEvent::size() const {
    qint64 characters = content.size() + 300;
    for (const QStringList &values : tags)
        for (const QString &value : values)
            characters += value.size() + 8;
    return characters * 2;
}

QString NostrEvent::tag(const QString &name) const {
    for (const QStringList &values : tags)
        if (values.size() >= 2 && values.first() == name)
            return values.at(1);
    return {};
}
