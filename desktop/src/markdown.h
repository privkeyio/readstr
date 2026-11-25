#pragma once

#include <QAbstractTextDocumentLayout>
#include <QString>
#include <QTextDocument>

#include "sanitizer.h"

// Nostr long-form articles (NIP-23) are Markdown. QTextDocument reads it as
// GitHub's dialect with raw HTML kept as text rather than trusted, and its HTML
// goes through Sanitizer::toRichText like any feed's, so nothing reaches the
// view that the sanitizer would not let through: no remote <img> (only the
// local copies `images` hands over, as for any feed), no network, only
// http(s) links, except that a nostr: link to a well-formed NIP-19 entity
// becomes its njump.me page first.
//
// QTextDocument builds lists and tables in time quadratic in their items,
// rows, and columns (8000 bullets took over a second, 87000 table rows
// minutes), so a document with more of them than the caps below is shown as
// its plain lines instead, which costs nothing to lay out. A line-based scan
// counts them without parsing, generously: everything that could be an item
// or a row counts as one. md4c ends a line at a lone carriage return as well
// as at a line feed, so every line ending becomes a line feed first and the
// scan sees the lines md4c sees.
namespace Markdown {
// Past this the text is cut off, ending in an ellipsis. Qt's HTML for it
// runs to about 106 times its size at worst (an indented one-character code
// block every few lines, "\ta\n\n\n"), of which the sanitizer reads the
// first 8 MiB, so at worst the end is cut off there too.
inline constexpr qsizetype maxInputLength = 128 * 1024;
inline constexpr int maxListItems = 2000;
inline constexpr int maxTableRows = 1000;
inline constexpr int maxTableColumns = 64;
inline constexpr int maxTableCells = 10000;

QString toRichText(const QString &markdown, const Sanitizer::Images &images = {});
// The words of `markdown` as toRichText() shows them, without its markup,
// link addresses, or the sanitizer's breaks in long words.
QString plainText(const QString &markdown);
// Whether QTextDocument lays `markdown` out quickly: its list items, table
// rows, columns, and cells are within the caps.
bool withinCaps(const QString &markdown);
// `markdown` with every "\r\n" and lone "\r" made a "\n".
QString normalizedLineEndings(const QString &markdown);

// A layout that lays nothing out, so a document using it never measures an
// image and so never loads one, and never spends time on layout either.
class InertLayout : public QAbstractTextDocumentLayout {
public:
    using QAbstractTextDocumentLayout::QAbstractTextDocumentLayout;
    void draw(QPainter *, const PaintContext &) override {}
    int hitTest(const QPointF &, Qt::HitTestAccuracy) const override { return -1; }
    int pageCount() const override { return 0; }
    QSizeF documentSize() const override { return {}; }
    QRectF frameBoundingRect(QTextFrame *) const override { return {}; }
    QRectF blockBoundingRect(const QTextBlock &) const override { return {}; }

protected:
    void documentChanged(int, int, int) override {}
};

// A QTextDocument that never reads a resource, so an image or a style sheet
// it names is not read from disk or the network. Refusing in loadResource()
// is not enough for images: when a document gives Qt's own image handler
// nothing, the handler loads the file by its name itself as it lays the
// image out. So the document also never lays anything out, which conversion
// to HTML does not need; Qt's real layout would make converting a document
// at the caps about three times slower. toRichText() converts with one.
class InertDocument : public QTextDocument {
public:
    explicit InertDocument(QObject *parent = nullptr);

protected:
    QVariant loadResource(int type, const QUrl &name) override;
};

// The njump.me page for a "nostr:" link to a well-formed npub, nprofile,
// note, nevent, or naddr; empty for anything else.
QString nostrLinkTarget(const QString &href);
}
