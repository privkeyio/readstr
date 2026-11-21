#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

// A NIP-01 event. Relays are untrusted, so an event is only built from JSON
// that has every field with the right type and size, and it is only kept
// once verify() has checked that its id is the hash of its contents and its
// signature is the author's.
struct NostrEvent {
    inline static constexpr int maxTags = 2000;
    inline static constexpr int maxTagValues = 16;
    // Across all of an event's tags; long-form articles use a few short ones.
    inline static constexpr int maxTagCharacters = 64 * 1024;

    QString id;
    QString pubkey;
    qint64 createdAt = 0;
    int kind = 0;
    QList<QStringList> tags;
    QString content;
    QString sig;

    // Null unless `value` is an object with the NIP-01 fields, ids and keys
    // as lowercase hex of the right length, and at most maxTags tags of at
    // most maxTagValues strings each, maxTagCharacters in all.
    static std::optional<NostrEvent> fromJson(const QJsonValue &value);
    // [0, pubkey, created_at, kind, tags, content] as NIP-01 serializes it,
    // the bytes whose SHA-256 is the id.
    QByteArray serialized() const;
    // The id matches the contents and the BIP-340 signature is valid.
    bool verify() const;
    // Of two versions of one replaceable event, whether this one wins: the
    // later, and for the same second the lower id, as NIP-01 says.
    bool isNewerThan(const NostrEvent &other) const {
        return createdAt != other.createdAt ? createdAt > other.createdAt : id < other.id;
    }
    // Roughly the bytes it takes in memory, for caps.
    qint64 size() const;
    // The first value of the first tag with this name, or empty.
    QString tag(const QString &name) const;
};
