#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThread>
#include <QUrl>
#include <atomic>
#include <functional>
#include <memory>

#include "fetcher.h"

// Remote images, fetched only for what is open in the reader, since a fetch
// from the list would tell the image's host what the user scrolled past: a
// video's thumbnail, or an article's images once the user asks for them. A
// new fetch, or cancel(), drops the one before it, in flight or waiting to be
// decoded. It comes through a Fetcher of its own, so it waits on no refresh:
// http(s) only, never this machine or the local network, no cookies, at most
// maxBytes each and maxTotalBytes for one fetchEach(), at most maxImages of
// them, and only an image/* response. Its first bytes must be those of a
// PNG, JPEG, GIF, or WebP image, and only that format's QImageReader handler
// ever reads it, so no other one (SVG, PDF, TIFF) parses what a stranger
// sent; an animated GIF or WebP keeps its first frame. It is decoded on a
// worker thread, which reads the dimensions first and refuses an image too
// large to decode within the allocation limit (a decompression bomb), or a
// JPEG with too many scans to decode quickly, and it is scaled down to
// maxWidth and maxStoredPixels. The result is cached as JPEG under
// Paths::cacheDirectory(), in a directory of its own for each kind
// (cacheName), named by the SHA-256 of the URL, written beside its final name
// and renamed into place, so a crash never leaves half an image there; the
// least recently used are removed once the cache is past its size or age.
class Thumbnails : public QObject {
    Q_OBJECT

public:
    struct Limits {
        qint64 maxBytes = 2 * 1024 * 1024;
        int timeoutMs = 30 * 1000;
        int maxWidth = 1280;
        // What is kept, after scaling to maxWidth: an image of more pixels
        // than this is scaled down further, keeping its proportions, and one
        // whose JPEG still comes to more bytes than this at the lowest
        // quality tried is refused. 0 is no limit.
        qint64 maxStoredPixels = 0;
        qint64 maxStoredBytes = 0;
        // Either side, read from the header before anything is decoded.
        int maxDimension = 8192;
        // What decoding may hold at once, in MiB: the image, the copy that
        // scaling it makes, and for a progressive JPEG its coefficients.
        // QImageReader is held to it too.
        int allocationLimitMb = 128;
        // A JPEG decodes each scan over the whole image, so one with more
        // scans than this, or a progressive one (or any of several scans)
        // with more pixels, would take long enough to hold up the window's
        // exit. At these, one decodes in well under a second.
        int maxJpegScans = 100;
        qint64 maxProgressivePixels = 16 * 1000 * 1000;
        bool publicHostsOnly = true;
        // Requests in flight at once.
        int maxConcurrent = 2;
        // For one fetchEach(): past this many URLs the rest are refused, and
        // once the bodies received (every one, whether it made an image or
        // not) come to more than this many bytes the rest are refused and
        // those in flight aborted. 0 is no limit.
        int maxImages = 0;
        qint64 maxTotalBytes = 0;
        // Past either, the least recently used are removed.
        qint64 cacheMaxBytes = 64 * 1024 * 1024;
        int cacheMaxAgeDays = 30;
    };

    explicit Thumbnails(QObject *parent = nullptr) : Thumbnails(QStringLiteral("thumbnails"), parent) {}
    // Cached in their own directory, `cacheName`, under the cache directory.
    explicit Thumbnails(const QString &cacheName, QObject *parent = nullptr)
        : Thumbnails(cacheName, Limits{}, parent) {}
    // With `limits` from the start, which the prune at the start keeps to.
    Thumbnails(const QString &cacheName, const Limits &limits, QObject *parent = nullptr);
    ~Thumbnails() override;

    void setLimits(const Limits &limits);
    Limits limits() const { return m_limits; }

    QString cacheName() const { return m_cacheName; }
    // Where `url`'s image is cached, or empty when there is no cache
    // directory or `url` is not a valid one. Its fragment names no other
    // image, so it is left out.
    static QString cachePath(const QUrl &url, const QString &cacheName = QStringLiteral("thumbnails"));
    // The cached image as a file: URL, or an empty URL when there is none.
    // A hit is marked as used now, so the cache removes the least recently
    // used first.
    static QUrl cached(const QUrl &url, const QString &cacheName = QStringLiteral("thumbnails"));
    // The cached thumbnail, fetching and caching it first when there is none.
    // `done` runs later on this object's thread with its file: URL, or an
    // empty URL when it cannot be had. It never runs once this object is
    // gone, or once a later fetch() or cancel() has dropped this one.
    void fetch(const QUrl &url, std::function<void(const QUrl &)> done);
    // fetch() for several at once: `each` runs once for each URL, in the
    // order they arrive, with the URL and its file: URL or an empty one, on
    // the same terms as `done`. While it runs, pruning never removes the
    // cached copy of any of `urls`.
    using Each = std::function<void(const QUrl &url, const QUrl &file)>;
    void fetchEach(const QList<QUrl> &urls, Each each);
    // Drops the fetch under way, if any: a request in flight is aborted, and
    // a body waiting to be decoded is neither decoded nor cached.
    void cancel();
    // `data` as a thumbnail within `limits`, or a null image with the reason
    // in `error`.
    static QImage decode(const QByteArray &data, const Limits &limits, QString *error = nullptr);
    // The format `data` is by its first bytes ("png", "jpeg", "gif", or
    // "webp"), or empty when it is none of them.
    static QByteArray sniff(const QByteArray &data);
    // Removes from `directory` the files older than `maxAgeDays`, and then
    // the least recently used until the rest come to at most `maxBytes`,
    // but never one of `keep` (paths), which counts toward the size all the
    // same. Returns how many it removed.
    static int prune(const QString &directory, qint64 maxBytes, int maxAgeDays, const QSet<QString> &keep = {});

signals:
    // A thumbnail failed, with the reason, for logging and tests; the window
    // shows nothing for it.
    void failed(const QUrl &url, const QString &error);

private:
    Fetcher::Limits fetcherLimits() const;
    // Aborts what the fetcher has in flight, which it does only as it goes.
    void dropFetcher();

    QString m_cacheName;
    Limits m_limits;
    std::unique_ptr<Fetcher> m_fetcher;
    QThread m_thread;
    QObject *m_worker = nullptr;
    bool m_closing = false;
    // The fetch that is wanted; a result for any other is dropped. The
    // worker reads it too, to skip decoding a body nobody wants any more.
    std::atomic<quint64> m_wanted{0};
    // Whether the fetcher has the wanted requests in flight.
    bool m_inFlight = false;
    // Bytes written since the cache was last pruned. Only the worker thread
    // touches it.
    qint64 m_writtenSincePrune = 0;
};
