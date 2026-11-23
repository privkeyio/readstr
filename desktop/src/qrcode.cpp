#include "qrcode.h"

#include <qrencode.h>

QString QrCode::source(const QString &text) {
    return QStringLiteral("image://qr/") + QString::fromLatin1(text.toUtf8().toHex());
}

QImage QrCode::image(const QString &text, int scale) {
    const QByteArray data = text.toUtf8();
    if (data.isEmpty() || scale < 1)
        return {};
    QRcode *code = QRcode_encodeString8bit(data.constData(), 0, QR_ECLEVEL_M);
    if (!code)
        return {};
    constexpr int quiet = 4;
    const int modules = code->width + 2 * quiet;
    if (scale > maxSize / modules) {
        QRcode_free(code);
        return {};
    }
    QImage image(modules * scale, modules * scale, QImage::Format_Grayscale8);
    if (image.isNull()) {
        QRcode_free(code);
        return {};
    }
    image.fill(255);
    for (int y = 0; y < code->width; ++y)
        for (int x = 0; x < code->width; ++x) {
            // The low bit of each byte is the module: 1 is dark.
            if (!(code->data[y * code->width + x] & 1))
                continue;
            for (int dy = 0; dy < scale; ++dy) {
                uchar *line = image.scanLine((y + quiet) * scale + dy);
                memset(line + (x + quiet) * scale, 0, size_t(scale));
            }
        }
    QRcode_free(code);
    return image;
}

QImage QrImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    const QString text = QString::fromUtf8(QByteArray::fromHex(id.toLatin1()));
    // Scaled by whole modules, so the code stays sharp, to fit the size
    // asked for when there is one.
    QImage code = QrCode::image(text, 1);
    if (code.isNull())
        return {};
    const int wanted = requestedSize.isValid() ? qMin(requestedSize.width(), requestedSize.height()) : 0;
    const int scale = qMin(wanted > 0 ? qMax(1, wanted / code.width()) : 6, QrCode::maxSize / code.width());
    code = QrCode::image(text, scale);
    if (size)
        *size = code.size();
    return code;
}
