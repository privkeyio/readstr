#include "markdown.h"

#include <QList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <algorithm>

#include "nip19.h"
#include "sanitizer.h"

namespace {
// Past this much of the HTML Qt writes the sanitizer cuts the rest off, as
// it does any feed's. It is room for maxInputLength of Markdown at up to 64
// times its size. The most verbose Markdown, at about 106 times, is cut at
// about 57% of its HTML here. Room for all of it would still see it cut, by
// the sanitizer's own output cap, and would take about a quarter longer to
// convert and lay out on the GUI thread, so the cut is left where it is.
constexpr qsizetype maxHtmlLength = 8 * 1024 * 1024;

bool isBlank(QChar character) { return character == ' ' || character == '\t'; }

// How many characters a list marker ("-", "*", "+", "1.", "1)") at `at`
// takes, with the space after it, or 0 when there is none.
qsizetype listMarker(QStringView line, qsizetype at) {
    const qsizetype size = line.size();
    qsizetype end = at;
    if (at < size && (line.at(at) == '-' || line.at(at) == '*' || line.at(at) == '+')) {
        end = at + 1;
    } else {
        while (end < size && end - at < 9 && line.at(end).isDigit())
            ++end;
        if (end == at || end >= size || (line.at(end) != '.' && line.at(end) != ')'))
            return 0;
        ++end;
    }
    if (end < size && !isBlank(line.at(end)))
        return 0;
    return end - at + (end < size ? 1 : 0);
}

// A table's delimiter row ("|---|:-:|"): only pipes, dashes, colons, and
// whitespace, with a pipe and a dash. Returns its columns, or 0. The
// whitespace is md4c's, which takes form feeds and vertical tabs between the
// cells as well as blanks, so no row Qt reads as a delimiter goes uncounted.
int delimiterColumns(QStringView rest) {
    bool pipe = false, dash = false;
    for (const QChar character : rest) {
        if (character == '|')
            pipe = true;
        else if (character == '-')
            dash = true;
        else if (character != ':' && !isBlank(character) && character != '\f' && character != '\v')
            return 0;
    }
    if (!pipe || !dash)
        return 0;
    int columns = 0;
    for (const QStringView cell : rest.split('|'))
        columns += cell.contains('-');
    return columns;
}
}

namespace Markdown {
InertDocument::InertDocument(QObject *parent) : QTextDocument(parent) {
    setDocumentLayout(new InertLayout(this));
}

QVariant InertDocument::loadResource(int, const QUrl &) { return {}; }

QString normalizedLineEndings(const QString &markdown) {
    if (!markdown.contains('\r'))
        return markdown;
    // In this order, so a "\r\n" becomes one line ending rather than two,
    // which would read as a blank line and end every table at its first row.
    return QString(markdown).replace(QLatin1String("\r\n"), QLatin1String("\n")).replace('\r', '\n');
}

bool withinCaps(const QString &input) {
    const QString markdown = normalizedLineEndings(input);
    int listItems = 0, tableRows = 0, columns = 0;
    qint64 tableCells = 0;
    for (const QStringView line : QStringView(markdown).split('\n')) {
        // Quote and list markers open containers, any number to a line.
        qsizetype at = 0;
        for (;;) {
            while (at < line.size() && (isBlank(line.at(at)) || line.at(at) == '>'))
                ++at;
            const qsizetype marker = listMarker(line, at);
            if (!marker)
                break;
            at += marker;
            if (++listItems > maxListItems)
                return false;
        }
        const QStringView rest = line.mid(at).trimmed();
        if (rest.isEmpty()) {
            // A blank line ends a table.
            columns = 0;
            continue;
        }
        // Every delimiter row is taken for a new table's, even inside one,
        // since a table also ends without a blank line when a quote or list
        // opens, and the one after it may be wider. Counting a row of dashes
        // in a table's body as a header and a delimiter is the safe mistake.
        const int delimiter = delimiterColumns(rest);
        if (delimiter) {
            if (delimiter > maxTableColumns)
                return false;
            // The header row above it, and the delimiter row. The widest
            // table so far sets the cells of the rows after it, as the
            // scan cannot tell where one ends.
            columns = std::max(columns, delimiter);
            tableRows += 2;
            tableCells += 2 * delimiter;
        } else if (!columns) {
            continue;
        } else {
            ++tableRows;
            tableCells += columns;
        }
        if (tableRows > maxTableRows || tableCells > maxTableCells)
            return false;
    }
    return true;
}

QString nostrLinkTarget(const QString &href) {
    const QString trimmed = href.trimmed();
    if (!trimmed.startsWith(QLatin1String("nostr:"), Qt::CaseInsensitive))
        return {};
    const QString entity = Nip19::entity(trimmed.mid(6));
    return entity.isEmpty() ? QString() : QStringLiteral("https://njump.me/") + entity;
}

QString toRichText(const QString &markdown, const Sanitizer::Images &images) {
    // Qt writes an empty paragraph for nothing at all.
    if (markdown.trimmed().isEmpty())
        return {};
    QString text = markdown.left(maxInputLength);
    const bool cut = markdown.size() > maxInputLength;
    // Never half of a surrogate pair.
    if (cut && !text.isEmpty() && text.back().isHighSurrogate())
        text.chop(1);
    // Once, before both the scan and the parse, so the two count the same
    // lines.
    text = normalizedLineEndings(text);
    const QString ellipsis = QLatin1String("<p>") + QChar(0x2026) + QLatin1String("</p>");

    if (!withinCaps(text)) {
        // Its lines as they are, through the sanitizer for its long-run
        // breaks like any other text.
        const QString plain = Sanitizer::toRichText(Sanitizer::linesToRichText(text), QUrl());
        return cut && !plain.endsWith(ellipsis) ? plain + ellipsis : plain;
    }

    InertDocument document;
    document.setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub
                                                               | QTextDocument::MarkdownNoHTML));
    // Links to Nostr entities go to their njump.me page; every other link
    // that is not http(s) the sanitizer drops, keeping its text. The changes
    // are collected first, since setting a format splits the fragments
    // being walked.
    struct Relink {
        int position;
        int length;
        QString target;
    };
    QList<Relink> relinks;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next())
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const QTextFragment part = fragment.fragment();
            const QTextCharFormat format = part.charFormat();
            if (!format.isAnchor())
                continue;
            const QString target = nostrLinkTarget(format.anchorHref());
            if (!target.isEmpty())
                relinks << Relink{part.position(), part.length(), target};
        }
    for (const Relink &relink : std::as_const(relinks)) {
        QTextCursor cursor(&document);
        cursor.setPosition(relink.position);
        cursor.setPosition(relink.position + relink.length, QTextCursor::KeepAnchor);
        QTextCharFormat format;
        format.setAnchorHref(relink.target);
        cursor.mergeCharFormat(format);
    }

    Sanitizer::Options options;
    options.maxInputLength = maxHtmlLength;
    options.qtStyles = true;
    options.images = images;
    // The line breaks Qt writes between its tags are no part of the text.
    const QString html = Sanitizer::toRichText(document.toHtml(), QUrl(), options).trimmed();
    // The sanitizer ends in the same ellipsis when it cut the HTML short.
    return cut && !html.endsWith(ellipsis) ? html + ellipsis : html;
}
}
