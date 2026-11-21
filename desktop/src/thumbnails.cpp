#include "thumbnails.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QTimer>

#include "library.h"
#include "paths.h"

namespace {
// Where the thumbnails are cached, or empty when there is no cache directory.
QString thumbnailDirectory() {
    const QString cache = Paths::cacheDirectory();
    return cache.isEmpty() ? QString() : cache + QStringLiteral("/thumbnails");
}

// The cache holds what the user watched, so only they may read it: the
// directories 0700, the file 0600 (Library::writePrivately).
bool savePrivately(const QImage &image, const QString &path, QString *error) {
    const QString directory = QFileInfo(path).absolutePath();
    const QFileDevice::Permissions owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (!QDir().mkpath(directory) || !QFile::setPermissions(directory, owner)
        || !QFile::setPermissions(QFileInfo(directory).absolutePath(), owner)) {
        *error = QStringLiteral("cannot create %1").arg(directory);
        return false;
    }
    // JPEG has no alpha channel, so a transparent image is laid on black,
    // as a frame of video would be.
    QImage opaque = image;
    if (image.hasAlphaChannel()) {
        opaque = QImage(image.size(), QImage::Format_RGB32);
        opaque.fill(Qt::black);
        QPainter painter(&opaque);
        painter.drawImage(0, 0, image);
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    if (!buffer.open(QIODevice::WriteOnly) || !opaque.save(&buffer, "JPEG", 85)) {
        *error = QStringLiteral("cannot encode the thumbnail");
        return false;
    }
    return Library::writePrivately(path, jpeg, error);
}

// What a JPEG's markers say about the cost of decoding it.
struct JpegLayout {
    // From the frame header. A JPEG whose frame header cannot be found
    // counts as the costliest kind, a progressive one of four components.
    bool progressive = true;
    int components = 4;
    // The scans found by walking the markers as libjpeg does.
    int scans = 0;
    // Every FF DA byte pair in the file, which is never fewer than the scans
    // libjpeg reads, since entropy-coded data stuffs every 0xFF with a 0x00
    // and so never holds the pair; a marker segment's payload (an Exif
    // thumbnail) may add some, which only errs toward refusing.
    qsizetype markedScans = 0;
};

JpegLayout jpegLayout(const QByteArray &data) {
    JpegLayout layout;
    layout.markedScans = data.count(QByteArrayView("\xff\xda", 2));
    const auto at = [&data](qsizetype i) { return quint8(data.at(i)); };
    bool frame = false;
    qsizetype i = 2;
    while (i + 1 < data.size()) {
        // libjpeg skips anything that is not a marker, and fill bytes before
        // one, so this does too.
        if (at(i) != 0xff || at(i + 1) == 0xff) {
            ++i;
            continue;
        }
        const quint8 marker = at(i + 1);
        i += 2;
        // A stuffed zero, TEM, and the restarts stand alone.
        if (marker == 0x00 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
            continue;
        if (marker == 0xd9 || i + 2 > data.size())
            break;
        const int length = at(i) << 8 | at(i + 1);
        if (length < 2)
            break;
        // The start-of-frame markers, C0 to CF but for DHT, JPG, and DAC.
        if (!frame && marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc
            && i + 8 <= data.size()) {
            frame = true;
            layout.progressive = marker == 0xc2 || marker == 0xc6 || marker == 0xca || marker == 0xce;
            layout.components = qMax(1, int(at(i + 7)));
        }
        i += length;
        if (marker == 0xda) {
            ++layout.scans;
            // Its entropy-coded data runs to the next marker but a restart.
            while (i + 1 < data.size()
                   && !(at(i) == 0xff && at(i + 1) != 0x00 && (at(i + 1) < 0xd0 || at(i + 1) > 0xd7)))
                ++i;
        }
    }
    return layout;
}

// The most decoding an image of `size` holds at once, in bytes. Qt decodes
// every format but JPEG at full size, 4 bytes a pixel, and scaling the result
// down converts it first, a second copy as large. libjpeg decodes a JPEG
// straight to a size smaller by a power of two of up to 8, the smallest that
// stays at least twice as wide as the target or the target itself, so only
// that much is held; QImageReader's allocation limit holds the image to its
// size whatever libjpeg does. A JPEG of several scans, though, which every
// progressive one is, keeps all of the image's coefficients until its last
// scan: 2 bytes for each sample of each component, counted as if none were
// subsampled.
qint64 decodeBytes(const QSize &size, bool isJpeg, const JpegLayout &jpeg, int maxWidth) {
    const qint64 pixels = qint64(size.width()) * size.height();
    if (!isJpeg)
        return 2 * 4 * pixels;
    qint64 denominator = 1;
    while (denominator < 8 && size.width() / (denominator * 2) >= maxWidth)
        denominator *= 2;
    const qint64 coefficients = jpeg.progressive || jpeg.scans > 1 ? 2 * qint64(jpeg.components) * pixels : 0;
    return 2 * 4 * pixels / (denominator * denominator) + coefficients;
}
}

Thumbnails::Thumbnails(QObject *parent) : QObject(parent), m_fetcher(std::make_unique<Fetcher>()) {
    setLimits(m_limits);
    m_worker = new QObject;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.setObjectName(QStringLiteral("readstr-thumbnails"));
    m_thread.start();
    // Pruned once at the start, off this thread, as a directory of many
    // files takes a while to list.
    QMetaObject::invokeMethod(
        m_worker,
        [directory = thumbnailDirectory(), limits = m_limits] {
            prune(directory, limits.cacheMaxBytes, limits.cacheMaxAgeDays);
        },
        Qt::QueuedConnection);
}

Thumbnails::~Thumbnails() {
    // The fetch in flight goes with the fetcher, and a body waiting to be
    // decoded is skipped; a decode already running finishes, which the
    // limits keep short, and its result, posted to this object, goes with it.
    m_closing = true;
    ++m_wanted;
    m_fetcher.reset();
    m_thread.quit();
    m_thread.wait();
}

Fetcher::Limits Thumbnails::fetcherLimits() const {
    Fetcher::Limits fetch;
    fetch.maxBytes = m_limits.maxBytes;
    fetch.timeoutMs = m_limits.timeoutMs;
    fetch.publicHostsOnly = m_limits.publicHostsOnly;
    // Only one is ever in flight, but a slot more costs nothing.
    fetch.maxConcurrent = 2;
    return fetch;
}

void Thumbnails::setLimits(const Limits &limits) {
    m_limits = limits;
    m_fetcher->setLimits(fetcherLimits());
}

QString Thumbnails::cachePath(const QUrl &url) {
    const QString directory = thumbnailDirectory();
    if (directory.isEmpty())
        return {};
    const QByteArray key =
        QCryptographicHash::hash(url.toString(QUrl::FullyEncoded).toUtf8(), QCryptographicHash::Sha256).toHex();
    return directory + '/' + QString::fromLatin1(key) + QStringLiteral(".jpg");
}

QUrl Thumbnails::cached(const QUrl &url) {
    const QString path = cachePath(url);
    return !path.isEmpty() && QFileInfo(path).isFile() ? QUrl::fromLocalFile(path) : QUrl();
}

void Thumbnails::cancel() {
    ++m_wanted;
    if (!m_inFlight)
        return;
    m_inFlight = false;
    // A Fetcher aborts what it has in flight when it goes, and has no other
    // way to. Nothing of the caller's ever runs inside the fetcher's own
    // callbacks (see fetch()), so it is never gone from under itself.
    m_fetcher = std::make_unique<Fetcher>();
    m_fetcher->setLimits(fetcherLimits());
}

void Thumbnails::fetch(const QUrl &url, std::function<void(const QUrl &)> done) {
    cancel();
    const quint64 wanted = m_wanted;
    const QString path = cachePath(url);
    const QUrl hit = cached(url);
    if (path.isEmpty() || hit.isValid()) {
        QTimer::singleShot(0, this, [this, wanted, url, hit, done] {
            if (wanted != m_wanted)
                return;
            if (!hit.isValid())
                emit failed(url, QStringLiteral("no cache directory"));
            done(hit);
        });
        return;
    }
    const Limits limits = m_limits;
    // Run on this object's thread with the outcome, unless it has been
    // dropped meanwhile.
    const auto finish = [this, wanted, url, done](const QUrl &file, const QString &error) {
        QMetaObject::invokeMethod(
            this,
            [this, wanted, url, done, file, error] {
                if (wanted != m_wanted)
                    return;
                if (!file.isValid())
                    emit failed(url, error);
                done(file);
            },
            Qt::QueuedConnection);
    };
    m_inFlight = true;
    m_fetcher->fetchEachLater({FetchRequest{url, {}, {}}}, [this, wanted, path, limits,
                                                           finish](qsizetype, const FetchResult &result) {
        // The outcome is posted rather than handed over here, so no slot of
        // the caller's runs inside the fetcher, and cancel() can destroy it.
        if (m_closing || wanted != m_wanted)
            return;
        m_inFlight = false;
        QString error = result.error;
        const QString type = result.contentType.trimmed();
        if (error.isEmpty() && !type.startsWith(QLatin1String("image/"), Qt::CaseInsensitive))
            error = QStringLiteral("not an image (%1)").arg(type.isEmpty() ? QStringLiteral("no type") : type);
        if (!error.isEmpty()) {
            finish({}, error);
            return;
        }
        QMetaObject::invokeMethod(
            m_worker,
            [this, wanted, path, limits, finish, body = result.body] {
                // This object waits for the worker before it goes, so it is
                // still there to ask, and to take the result.
                if (wanted != m_wanted)
                    return;
                QString error;
                const QImage image = decode(body, limits, &error);
                // Asked again, as a decode takes a while: a thumbnail that is
                // no longer wanted is not cached.
                if (wanted != m_wanted)
                    return;
                const bool saved = !image.isNull() && savePrivately(image, path, &error);
                if (saved) {
                    // Pruned now and then rather than after every write, which
                    // would list the directory each time.
                    m_writtenSincePrune += QFileInfo(path).size();
                    if (m_writtenSincePrune > limits.cacheMaxBytes / 16) {
                        m_writtenSincePrune = 0;
                        prune(QFileInfo(path).absolutePath(), limits.cacheMaxBytes, limits.cacheMaxAgeDays);
                    }
                }
                finish(saved ? QUrl::fromLocalFile(path) : QUrl(), error);
            },
            Qt::QueuedConnection);
    });
}

QByteArray Thumbnails::sniff(const QByteArray &data) {
    if (data.startsWith("\x89PNG\r\n\x1a\n"))
        return "png";
    if (data.startsWith("\xff\xd8\xff"))
        return "jpeg";
    if (data.startsWith("GIF87a") || data.startsWith("GIF89a"))
        return "gif";
    if (data.size() >= 12 && data.startsWith("RIFF") && data.mid(8, 4) == "WEBP")
        return "webp";
    return {};
}

QImage Thumbnails::decode(const QByteArray &data, const Limits &limits, QString *error) {
    const auto refuse = [error](const QString &reason) {
        if (error)
            *error = reason;
        return QImage();
    };
    const QByteArray format = sniff(data);
    if (format.isEmpty())
        return refuse(QStringLiteral("not a PNG, JPEG, GIF, or WebP image"));
    // Counted before any reader is made, since a JPEG of many scans takes
    // long to decode however small it is.
    const bool isJpeg = format == "jpeg";
    const JpegLayout jpeg = isJpeg ? jpegLayout(data) : JpegLayout{};
    if (isJpeg && jpeg.markedScans > limits.maxJpegScans)
        return refuse(QStringLiteral("the JPEG has %1 scans, too many to decode").arg(jpeg.markedScans));

    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    // Only the handler for the format its bytes name, never another one Qt
    // would pick by looking.
    QImageReader reader(&buffer, format);
    reader.setAutoDetectImageFormat(false);
    reader.setDecideFormatFromContent(false);
    reader.setAllocationLimit(limits.allocationLimitMb);
    const QSize size = reader.canRead() ? reader.size() : QSize();
    if (!size.isValid() || size.isEmpty())
        return refuse(QStringLiteral("cannot read the image's size"));
    const QString tooLarge = QStringLiteral("the %1 is %2 by %3 pixels, too large to decode");
    if (size.width() > limits.maxDimension || size.height() > limits.maxDimension
        || decodeBytes(size, isJpeg, jpeg, limits.maxWidth) > qint64(limits.allocationLimitMb) * 1024 * 1024)
        return refuse(tooLarge.arg(QStringLiteral("image")).arg(size.width()).arg(size.height()));
    // Each scan is a pass over the whole image, so a JPEG of several may
    // have fewer pixels than others.
    if (isJpeg && (jpeg.progressive || jpeg.scans > 1)
        && qint64(size.width()) * size.height() > limits.maxProgressivePixels) {
        const QString kind =
            jpeg.progressive ? QStringLiteral("progressive JPEG") : QStringLiteral("JPEG of several scans");
        return refuse(tooLarge.arg(kind).arg(size.width()).arg(size.height()));
    }

    // JPEG's handler decodes straight to about this size; the others decode
    // in full and QImageReader scales the result.
    if (size.width() > limits.maxWidth)
        reader.setScaledSize(
            QSize(limits.maxWidth, qMax(1, int(qint64(size.height()) * limits.maxWidth / size.width()))));
    QImage image = reader.read();
    if (image.isNull())
        return refuse(reader.errorString());
    // In case a format ignores the scaled size.
    if (image.width() > limits.maxWidth)
        image = image.scaledToWidth(limits.maxWidth, Qt::SmoothTransformation);
    if (error)
        error->clear();
    return image;
}

int Thumbnails::prune(const QString &directory, qint64 maxBytes, int maxAgeDays) {
    if (directory.isEmpty())
        return 0;
    const QDateTime oldest = QDateTime::currentDateTimeUtc().addDays(-maxAgeDays);
    // Newest first, so once one does not fit, it and every older one go.
    const QFileInfoList files =
        QDir(directory).entryInfoList(QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDir::Time);
    qint64 kept = 0;
    bool full = false;
    int removed = 0;
    for (const QFileInfo &file : files) {
        full = full || kept + file.size() > maxBytes;
        if (!full && file.lastModified() >= oldest) {
            kept += file.size();
            continue;
        }
        removed += QFile::remove(file.absoluteFilePath());
    }
    return removed;
}
