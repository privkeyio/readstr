#pragma once

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QString>

// Reads a small settings file on the GUI thread. Only regular files of at most
// `cap` bytes are read, so a symlink to /dev/zero or a FIFO can neither hang
// the window nor run it out of memory. Anything else reads as null.
inline QByteArray readSmallFile(const QString &path, qint64 cap = 64 * 1024) {
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > cap)
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.read(cap);
}
