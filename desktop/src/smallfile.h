#pragma once

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QString>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Reads a small settings file on the GUI thread. Only regular files of at most
// `cap` bytes are read, so a symlink to /dev/zero or a FIFO can neither hang
// the window nor run it out of memory. Anything else reads as null.
inline QByteArray readSmallFile(const QString &path, qint64 cap = 64 * 1024) {
#ifdef Q_OS_UNIX
    // Open once without blocking and check that descriptor, not the path:
    // checking the path and opening it again would leave a window in which a
    // FIFO swapped in blocks open() until a writer appears.
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return {};
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > cap) {
        ::close(fd);
        return {};
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return {};
    }
    return file.read(cap);
#else
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > cap)
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.read(cap);
#endif
}
