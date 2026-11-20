#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// OPML import and export, ported from the web app (src/lib/opml.ts) so files
// move between the two. Folders are outline nesting; the `category` attribute
// holds comma-separated tags. Input is bounded and entities are never
// expanded; a malformed file yields nothing rather than part of it.
namespace Opml {
inline constexpr qint64 maxBytes = 8 * 1024 * 1024;
inline constexpr int maxOutlines = 5000;
inline constexpr int maxDepth = 64;

struct Outline {
    QString title;
    QString xmlUrl;
    QString npub;
    QString folder;
    QStringList tags;
};

struct ExportFeed {
    // "RSS", "NOSTR", or "NOSTR_VIDEO", as in the web app.
    QString type = QStringLiteral("RSS");
    QString url;
    QString title;
    QString category;
    QStringList tags;
};

struct Plan {
    QList<Outline> toAdd;
    int skipped = 0;
};

QList<Outline> parse(const QByteArray &xml, QString *error = nullptr);
QByteArray build(const QList<ExportFeed> &feeds);
// Drops outlines already subscribed (or repeated in the file), comparing
// URLs with Store::urlKey and npubs case-insensitively. Folders become
// categories, which is how the desktop app organizes feeds.
Plan plan(const QList<Outline> &parsed, const QStringList &existingUrls, const QStringList &existingNpubs = {});
}
