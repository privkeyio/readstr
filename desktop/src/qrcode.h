#pragma once

#include <QImage>
#include <QQuickImageProvider>
#include <QString>

// QR codes through libqrencode, for a nostrconnect:// link a phone scans.
namespace QrCode {
// The widest image drawn, in pixels, whatever size QML asks for.
inline constexpr int maxSize = 4096;
// `text` as a black-on-white QR code (error correction level M), each module
// `scale` pixels square, with the four-module quiet zone scanners need; a
// null image when it does not fit in a QR code, or would be wider than
// maxSize.
QImage image(const QString &text, int scale = 8);
// The image://qr/ URL QrImageProvider draws `text` for: its UTF-8 in hex,
// which no step between QML and the provider decodes or mangles.
QString source(const QString &text);
}

// Serves image://qr/<hex> to QML. The text is in the URL rather than held
// here, so whatever thread asks gets a code without any state to share; the
// link it encodes is shown as text beside it anyway.
class QrImageProvider : public QQuickImageProvider {
public:
    QrImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
