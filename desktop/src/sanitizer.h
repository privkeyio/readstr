#pragma once

#include <QString>
#include <QUrl>

// Turns a feed's untrusted HTML into the small subset of HTML that Qt's rich
// text shows, and nothing else: p, br, h1-h6, ul, ol, li, blockquote, pre,
// code, em, i, strong, b, s, a (href only, http and https only, made absolute
// against the article's URL), table, tr, td, th, and hr. Script, style,
// iframe, object, embed, form, svg, math, and the like go with everything in
// them; any other tag goes but keeps its text; every attribute but a's href
// goes, so no style, class, or event handler survives.
//
// It never emits <img>: Qt's rich text fetches image sources over the
// network as it lays the text out, which would let any feed track who reads
// it. An image becomes a text link, "[image: alt]", to its http(s) source.
//
// Qt's layout, not just this pass, must stay fast on hostile markup: tables
// never nest (an inner one is shown flat) and keep at most 1000 rows of 32
// cells, links longer than 8 KiB are dropped, and text gets a zero-width
// space after 80 characters without a break opportunity.
//
// One pass over the input, with a bounded nesting stack, so hostile markup
// stays linear. Input past maxInputLength is cut off first, and output stops
// once past maxOutputLength, ending in an ellipsis either way.
namespace Sanitizer {
inline constexpr qsizetype maxInputLength = 512 * 1024;
inline constexpr qsizetype maxOutputLength = 1024 * 1024;
inline constexpr int maxDepth = 64;

struct Options {
    // Input past this is cut off first.
    qsizetype maxInputLength = Sanitizer::maxInputLength;
    // The input is QTextDocument::toHtml()'s, which spells emphasis, code,
    // and strikethrough as a span's style and a quote as a paragraph's
    // margins: those few styles become em, strong, code, s, and blockquote.
    // Every other style still goes.
    bool qtStyles = false;
    // Off for the search index: an image becomes its description alone, not
    // the "[image: description]" label, whose word (in the window's language)
    // would otherwise match every article with an image.
    bool imageLabels = true;
};

QString toRichText(const QString &html, const QUrl &base, const Options &options = {});
// Plain text (a summary) as rich text: escaped, in one paragraph, without
// the direction overrides and embeddings.
QString plainToRichText(const QString &text);
// Plain text with line breaks as rich text: the same, a paragraph per run
// of text between blank lines and its single line breaks kept.
QString linesToRichText(const QString &text);
// The URL `href` points at, made absolute against `base`, if it is http(s)
// with a host; an empty URL otherwise.
QUrl webUrl(const QString &href, const QUrl &base);
}
