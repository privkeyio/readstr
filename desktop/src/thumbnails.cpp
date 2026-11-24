#include "thumbnails.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPointer>
#include <QStringList>
#include <cmath>
#include <fcntl.h>
#include <sys/stat.h>

#include "library.h"
#include "paths.h"

namespace {
// Where the images are cached, or empty when there is no cache directory.
QString imageDirectory(const QString &cacheName) {
    const QString cache = Paths::cacheDirectory();
    return cache.isEmpty() ? QString() : cache + '/' + cacheName;
}

// The cache holds what the user watched and read, so only they may read it: the
// directories 0700, the file 0600 (Library::writePrivately). Past `maxBytes`
// (0 is no limit), it is encoded again at lower qualities, and refused when
// even the lowest is past it: noise, which JPEG cannot compress, would
// otherwise fill the cache with a few images and push out the rest.
bool savePrivately(const QImage &image, const QString &path, qint64 maxBytes, QString *error) {
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
    for (const int quality : {85, 70, 50}) {
        jpeg.clear();
        QBuffer buffer(&jpeg);
        if (!buffer.open(QIODevice::WriteOnly) || !opaque.save(&buffer, "JPEG", quality)) {
            *error = QStringLiteral("cannot encode the image");
            return false;
        }
        if (maxBytes <= 0 || jpeg.size() <= maxBytes)
            return Library::writePrivately(path, jpeg, error);
    }
    *error = QStringLiteral("the image comes to %1 MiB as JPEG, too large to keep")
                 .arg(QString::number(double(jpeg.size()) / (1024 * 1024), 'g', 3));
    return false;
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

Thumbnails::Thumbnails(const QString &cacheName, const Limits &limits, QObject *parent)
    : QObject(parent), m_cacheName(cacheName), m_fetcher(std::make_unique<Fetcher>()) {
    setLimits(limits);
    m_worker = new QObject;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.setObjectName(QStringLiteral("readstr-") + cacheName);
    m_thread.start();
    // Pruned once at the start, off this thread, as a directory of many
    // files takes a while to list.
    QMetaObject::invokeMethod(
        m_worker,
        [directory = imageDirectory(m_cacheName), limits = m_limits] {
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
    fetch.maxConcurrent = m_limits.maxConcurrent;
    // Tests lift publicHostsOnly to reach their server on 127.0.0.1, which
    // listens on a port of its own, so the port rule goes with it.
    fetch.defaultPortsOnly = m_limits.publicHostsOnly;
    fetch.images = true;
    return fetch;
}

void Thumbnails::setLimits(const Limits &limits) {
    m_limits = limits;
    m_fetcher->setLimits(fetcherLimits());
}

QString Thumbnails::cachePath(const QUrl &url, const QString &cacheName) {
    const QString directory = imageDirectory(cacheName);
    // An invalid URL encodes as nothing, and every one would share the hash
    // of nothing, and so one cached image.
    if (directory.isEmpty() || !url.isValid() || url.isEmpty())
        return {};
    const QString name = url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
    if (name.isEmpty())
        return {};
    const QByteArray key = QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex();
    return directory + '/' + QString::fromLatin1(key) + QStringLiteral(".jpg");
}

QUrl Thumbnails::cached(const QUrl &url, const QString &cacheName) {
    const QString path = cachePath(url, cacheName);
    if (path.isEmpty() || !QFileInfo(path).isFile())
        return {};
    // prune() goes by the modification time, so dating the file now makes
    // it the last to go. The file is not opened for this, as opening one a
    // stranger had made a FIFO would hang.
    ::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), nullptr, 0);
    return QUrl::fromLocalFile(path);
}

void Thumbnails::dropFetcher() {
    m_inFlight = false;
    // A Fetcher aborts what it has in flight when it goes, and has no other
    // way to. Nothing of the caller's ever runs inside the fetcher's own
    // callbacks (see fetchEach()), so it is never gone from under itself.
    m_fetcher = std::make_unique<Fetcher>();
    m_fetcher->setLimits(fetcherLimits());
}

void Thumbnails::cancel() {
    ++m_wanted;
    if (m_inFlight)
        dropFetcher();
}

void Thumbnails::fetch(const QUrl &url, std::function<void(const QUrl &)> done) {
    fetchEach({url}, [done = std::move(done)](const QUrl &, const QUrl &file) { done(file); });
}

void Thumbnails::fetchEach(const QList<QUrl> &urls, Each each) {
    cancel();
    const quint64 wanted = m_wanted;
    const Limits limits = m_limits;
    // Run on this object's thread with the outcome, unless the fetch has
    // been dropped meanwhile.
    const auto finish = [this, wanted, each](const QUrl &url, const QUrl &file, const QString &error) {
        QMetaObject::invokeMethod(
            this,
            [this, wanted, each, url, file, error] {
                if (wanted != m_wanted)
                    return;
                if (!file.isValid()) {
                    // A slot on failed may cancel or replace this fetch, or
                    // even delete this object, and each must not run after
                    // either.
                    const QPointer<Thumbnails> alive(this);
                    emit failed(url, error);
                    if (!alive || wanted != m_wanted)
                        return;
                }
                each(url, file);
            },
            Qt::QueuedConnection);
    };

    // What the requests of this fetch share; only this thread touches it.
    struct Batch {
        QList<QUrl> urls;
        QStringList paths;
        QList<bool> answered;
        qint64 bytes = 0;
        bool spent = false;
    };
    auto batch = std::make_shared<Batch>();
    // The cached copies of this fetch's images, which the prunes it makes
    // never remove: the caller shows them, and a broken file: image is worse
    // than the link it replaced.
    QSet<QString> used;
    QList<FetchRequest> requests;
    for (qsizetype i = 0; i < urls.size(); ++i) {
        const QUrl &url = urls.at(i);
        if (limits.maxImages > 0 && i >= limits.maxImages) {
            finish(url, {}, QStringLiteral("past the %1 images fetched at once").arg(limits.maxImages));
            continue;
        }
        if (!url.isValid() || url.isEmpty()) {
            finish(url, {}, QStringLiteral("not a valid URL"));
            continue;
        }
        const QString path = cachePath(url, m_cacheName);
        if (path.isEmpty()) {
            finish(url, {}, QStringLiteral("no cache directory"));
            continue;
        }
        used.insert(QFileInfo(path).absoluteFilePath());
        const QUrl hit = cached(url, m_cacheName);
        if (hit.isValid()) {
            finish(url, hit, {});
        } else {
            requests << FetchRequest{url, {}, {}};
            batch->urls << url;
            batch->paths << path;
            batch->answered << false;
        }
    }
    if (requests.isEmpty())
        return;
    const QString spent =
        QStringLiteral("past the %1 MiB fetched at once")
            .arg(QString::number(double(limits.maxTotalBytes) / (1024 * 1024), 'g', 3));
    m_inFlight = true;
    auto keep = std::make_shared<const QSet<QString>>(std::move(used));
    m_fetcher->fetchEachLater(
        requests,
        [this, wanted, limits, batch, finish, spent, keep](qsizetype index, const FetchResult &result) {
            // The outcome is posted rather than handed over here, so no slot
            // of the caller's runs inside the fetcher, and cancel() can
            // destroy it.
            if (m_closing || wanted != m_wanted)
                return;
            batch->answered[index] = true;
            const QUrl url = batch->urls.at(index);
            const QString path = batch->paths.at(index);
            QString error = result.error;
            const QString type = result.contentType.trimmed();
            if (error.isEmpty() && !type.startsWith(QLatin1String("image/"), Qt::CaseInsensitive))
                error = QStringLiteral("not an image (%1)").arg(type.isEmpty() ? QStringLiteral("no type") : type);
            // Every body counts, an error page's or a stray type's too,
            // since each cost as much to fetch as an image would have.
            if (limits.maxTotalBytes > 0 && !batch->spent) {
                batch->bytes += result.received;
                if (batch->bytes > limits.maxTotalBytes) {
                    // The rest are refused too, and those in flight aborted,
                    // once out of the fetcher's callback.
                    batch->spent = true;
                    QMetaObject::invokeMethod(
                        this,
                        [this, wanted, batch, finish, spent] {
                            if (wanted != m_wanted)
                                return;
                            if (m_inFlight)
                                dropFetcher();
                            for (qsizetype i = 0; i < batch->urls.size(); ++i)
                                if (!batch->answered.at(i)) {
                                    batch->answered[i] = true;
                                    finish(batch->urls.at(i), {}, spent);
                                }
                        },
                        Qt::QueuedConnection);
                }
            }
            if (error.isEmpty() && batch->spent)
                error = spent;
            if (!error.isEmpty()) {
                finish(url, {}, error);
                return;
            }
            QMetaObject::invokeMethod(
                m_worker,
                [this, wanted, url, path, limits, finish, keep, body = result.body] {
                    // This object waits for the worker before it goes, so it
                    // is still there to ask, and to take the result.
                    if (wanted != m_wanted)
                        return;
                    QString error;
                    const QImage image = decode(body, limits, &error);
                    // Asked again, as a decode takes a while: an image that
                    // is no longer wanted is not cached.
                    if (wanted != m_wanted)
                        return;
                    const bool saved =
                        !image.isNull() && savePrivately(image, path, limits.maxStoredBytes, &error);
                    if (saved) {
                        // Pruned now and then rather than after every write,
                        // which would list the directory each time.
                        m_writtenSincePrune += QFileInfo(path).size();
                        if (m_writtenSincePrune > limits.cacheMaxBytes / 16) {
                            m_writtenSincePrune = 0;
                            prune(QFileInfo(path).absolutePath(), limits.cacheMaxBytes, limits.cacheMaxAgeDays,
                                  *keep);
                        }
                    }
                    finish(url, saved ? QUrl::fromLocalFile(path) : QUrl(), error);
                },
                Qt::QueuedConnection);
        },
        [this, wanted] {
            if (!m_closing && wanted == m_wanted)
                m_inFlight = false;
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

    // At most maxWidth wide, and then at most maxStoredPixels, both at the
    // image's proportions. JPEG's handler decodes straight to about this
    // size; the others decode in full and QImageReader scales the result.
    QSize target = size;
    if (target.width() > limits.maxWidth)
        target = QSize(limits.maxWidth, qMax(1, int(qint64(size.height()) * limits.maxWidth / size.width())));
    const qint64 pixels = qint64(target.width()) * target.height();
    if (limits.maxStoredPixels > 0 && pixels > limits.maxStoredPixels) {
        const double scale = std::sqrt(double(limits.maxStoredPixels) / double(pixels));
        target = QSize(qMax(1, int(target.width() * scale)), qMax(1, int(target.height() * scale)));
        // A side scaled below one pixel is held at one, which leaves the
        // other too long for the cap (1 by 8192 would come to 1 by 5792), so
        // the other side is cut to what the cap allows beside it.
        if (qint64(target.width()) * target.height() > limits.maxStoredPixels) {
            if (target.width() == 1)
                target.setHeight(int(limits.maxStoredPixels));
            else
                target.setWidth(int(limits.maxStoredPixels));
        }
    }
    if (target != size)
        reader.setScaledSize(target);
    QImage image = reader.read();
    if (image.isNull())
        return refuse(reader.errorString());
    // In case a format ignores the scaled size.
    if (image.width() > target.width() || image.height() > target.height())
        image = image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (error)
        error->clear();
    return image;
}

int Thumbnails::prune(const QString &directory, qint64 maxBytes, int maxAgeDays, const QSet<QString> &keep) {
    if (directory.isEmpty())
        return 0;
    const QDateTime oldest = QDateTime::currentDateTimeUtc().addDays(-maxAgeDays);
    // Most recently used first (cached() dates a hit now), so once one does
    // not fit, it and every one used before it go.
    const QFileInfoList files =
        QDir(directory).entryInfoList(QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDir::Time);
    // The files kept whatever happens take their share of maxBytes first,
    // wherever they fall in the order, so the rest fit in what they leave.
    qint64 kept = 0;
    for (const QFileInfo &file : files)
        if (keep.contains(file.absoluteFilePath()))
            kept += file.size();
    bool full = false;
    int removed = 0;
    for (const QFileInfo &file : files) {
        if (keep.contains(file.absoluteFilePath()))
            continue;
        full = full || kept + file.size() > maxBytes;
        if (!full && file.lastModified() >= oldest) {
            kept += file.size();
            continue;
        }
        removed += QFile::remove(file.absoluteFilePath());
    }
    return removed;
}
