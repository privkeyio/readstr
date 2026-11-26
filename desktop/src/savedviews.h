#pragma once

#include <QString>
#include <QStringList>

#include "store.h"

// Saved views as the web app defines them (src/lib/saved-views.ts) and
// kind 30406 carries them, which Store::savedViews() holds, turned into what
// the library can list: no GUI types, so the window, the headless commands,
// and the worker thread all read a view the same way.
namespace SavedViews {
// What a view lists here: its rules as an ArticleFilter, which the caller
// gives its own limit, search, and open article, and the rules it cannot
// apply, by the field the view holds them in ("source.feedId",
// "source.categoryId", or any field this version does not know). A view of
// tags lists the articles of the feeds that carry every one of them
// (ArticleFilter::tags), as the web app's does.
struct Evaluation {
    ArticleFilter filter;
    QStringList unsupported;
    // Its source is one feed or one category, which the web app names by its
    // server's own ids, so which articles it means cannot be told here
    // (readstr-5us carries what would tell them): it lists nothing, rather
    // than every feed, and cannot be selected. Any other rule left out is
    // left out of a view that still lists, which then lists more here than
    // in the web app, never less.
    bool unknownSource = false;
};
Evaluation evaluate(const SavedView &view);
// Whether `a` and `b` list the same articles, whatever their names, icons,
// or places.
bool listsSame(const SavedView &a, const SavedView &b);

// Its name and icon as text for display, with no line breaks or
// bidirectional overrides, and its name with the id when it has none.
QString name(const SavedView &view);
QString icon(const SavedView &view);
}
