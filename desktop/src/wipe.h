#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <string.h>
#include <type_traits>

// Overwrites a buffer that held a secret key before it is freed. Only this
// copy is wiped: writing to a buffer another QByteArray shares detaches it
// first, so the caller must hold the last reference for the bytes to go.
// explicit_bzero, unlike memset, is not optimized away.
inline void wipe(QByteArray &bytes) {
    if (!bytes.isEmpty())
        explicit_bzero(bytes.data(), size_t(bytes.size()));
    bytes.clear();
}

inline void wipe(QString &text) {
    if (!text.isEmpty())
        explicit_bzero(text.data(), size_t(text.size()) * sizeof(QChar));
    text.clear();
}

// Overwrites the bytes of `bytes` where they are, without detaching, so
// every QByteArray sharing the buffer, such as the copy inside a sent
// QDBusMessage, sees zeros from now on. Only for a buffer Qt allocated (never
// one from fromRawData(), which may be read-only), and only once nothing
// needs the bytes any more.
inline void wipeShared(const QByteArray &bytes) {
    if (!bytes.isEmpty())
        explicit_bzero(const_cast<char *>(bytes.constData()), size_t(bytes.size()));
}

template <typename T>
inline void wipe(QList<T> &values) {
    static_assert(std::is_trivially_copyable_v<T>, "only plain values can be overwritten in place");
    if (!values.isEmpty())
        explicit_bzero(values.data(), size_t(values.size()) * sizeof(T));
    values.clear();
}
