#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <QThread>
#include <QUrl>
#include <atomic>
#include <functional>
#include <memory>

#include "fetcher.h"

// A video's thumbnail, fetched only when the video is opened in the reader,
// since a fetch from the list would tell the image's host what the user
// scrolled past, and only the open video's: a new fetch, or cancel(), drops
// the one before it, in flight or waiting to be decoded. It comes through a
// Fetcher of its own, so it waits on no refresh: http(s) only, never this
// machine or the local network, no cookies, at most maxBytes, and only an
// image/* response. Its first bytes must be those of a PNG, JPEG, GIF, or
// WebP image, and only that format's QImageReader handler ever reads it, so
// no other one (SVG, PDF, TIFF) parses what a stranger sent. It is decoded
// on a worker thread, which reads the dimensions first and refuses an image
// too large to decode within the allocation limit (a decompression bomb), or
// a JPEG with too many scans to decode quickly, and it is scaled down to
// maxWidth. The result is cached as JPEG under Paths::cacheDirectory(), named
// by the SHA-256 of the URL, written beside its final name and renamed into
// place, so a crash never leaves half an image there; the oldest are removed
// once the cache is past its size or age.
class Thumbnails : public QObject {
    Q_OBJECT

public:
    struct Limits {
        qint64 maxBytes = 2 * 1024 * 1024;
        int timeoutMs = 30 * 1000;
        int maxWidth = 1280;
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
        // Past either, the oldest thumbnails are removed.
        qint64 cacheMaxBytes = 64 * 1024 * 1024;
        int cacheMaxAgeDays = 30;
    };

    explicit Thumbnails(QObject *parent = nullptr);
    ~Thumbnails() override;

    void setLimits(const Limits &limits);
    Limits limits() const { return m_limits; }

    // Where `url`'s thumbnail is cached, or empty when there is no cache
    // directory.
    static QString cachePath(const QUrl &url);
    // The cached thumbnail as a file: URL, or an empty URL when there is none.
    static QUrl cached(const QUrl &url);
    // The cached thumbnail, fetching and caching it first when there is none.
    // `done` runs later on this object's thread with its file: URL, or an
    // empty URL when it cannot be had. It never runs once this object is
    // gone, or once a later fetch() or cancel() has dropped this one.
    void fetch(const QUrl &url, std::function<void(const QUrl &)> done);
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
    // the oldest until the rest come to at most `maxBytes`. Returns how many
    // it removed.
    static int prune(const QString &directory, qint64 maxBytes, int maxAgeDays);

signals:
    // A thumbnail failed, with the reason, for logging and tests; the window
    // shows nothing for it.
    void failed(const QUrl &url, const QString &error);

private:
    Fetcher::Limits fetcherLimits() const;

    Limits m_limits;
    std::unique_ptr<Fetcher> m_fetcher;
    QThread m_thread;
    QObject *m_worker = nullptr;
    bool m_closing = false;
    // The fetch that is wanted; a result for any other is dropped. The
    // worker reads it too, to skip decoding a body nobody wants any more.
    std::atomic<quint64> m_wanted{0};
    // Whether the fetcher has the wanted request in flight.
    bool m_inFlight = false;
    // Bytes written since the cache was last pruned. Only the worker thread
    // touches it.
    qint64 m_writtenSincePrune = 0;
};
