#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <functional>
#include <optional>

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

// The web app's caps on what a view holds (src/lib/saved-views.ts): a name
// of NAME_MAX characters, an icon of 8, and up to MAX_KEYWORDS words or tags
// of KEYWORD_MAX characters each, counted in UTF-16 units as its slice()
// counts them.
inline constexpr int maxNameLength = 60;
inline constexpr int maxIconLength = 8;
inline constexpr int maxWords = 50;
inline constexpr int maxWordLength = 200;

// What the window's view dialog, or `readstr views add|edit`, sets on a
// view. `source` is "all", "favorites" (Starred), or "tags", with `tags`;
// empty keeps the view's own, as for a view of one feed or one category,
// which the web app names by ids the desktop does not have (readstr-5us),
// so only the web app can set one.
struct Draft {
    QString name;
    QString icon;
    QString source;
    QStringList tags;
    QString readState = QStringLiteral("all");
    QString sort = QStringLiteral("newest");
    QStringList include;
    // The mute words, which stay on this machine (SavedView::exclude).
    QStringList exclude;
};
// A view's rules as a Draft, its source kind included, and its mute words
// whether kept apart or, as the web app keeps them, in the view.
Draft draftOf(const SavedView &view);
// The draft of `base`, or of a new view of all articles when it is null,
// with what `fields` holds in place of its own: name, icon, source, tags (a
// list), readState, sort, and include and exclude (each a comma-separated
// list, through words()). A new view given tags and no source lists those
// tags. The source comes back empty, so build() keeps the base's own as it
// is, when it is the base's and so are its tags.
Draft edited(const SavedView *base, const QVariantMap &fields);
// Whether a view can list the tag: one a feed can carry (Sync::storableTag())
// that the web app's sanitizeView keeps whole, as it cuts a view's tags to
// maxWordLength, which a feed's can pass.
bool viewableTag(const QString &tag);
// Whether the dialog may change the view's source: not one of a feed or a
// category.
bool sourceEditable(const SavedView &view);
// An id in the web app's newViewId() format: the time in milliseconds and
// random base-36 digits.
QString newId();
// The words of a comma-separated list, trimmed, each once, none empty; or
// `before` itself when `text` is it listed, so a word of a view that holds
// a comma survives an edit that leaves the words as they were.
QStringList words(const QString &text, const QStringList &before = {});
// `base` with the rules of `draft`, or a new view with a new id when `base`
// is null; or null, with why in `error`, when the draft does not fit the
// web app's caps or schema. Whatever else `base` holds, its organizationMode
// and keywords other than include, is kept as it is, and so is its source
// when the draft leaves it. A view pulled from the relays holds only the
// fields the web app's sanitizeView keeps (Sync::parseViews()), so a field
// a later web app adds is not among them. An icon cut through an emoji, as
// a field's maximumLength cuts, loses the half character left.
std::optional<SavedView> build(const Draft &draft, const SavedView *base, QString *error);

// Changes to a list of views by id, each empty on success or else why not.
qsizetype indexOf(const QList<SavedView> &views, const QString &id);
// Why `views` can take no more views (Sync::maxViews), or empty.
QString full(const QList<SavedView> &views);
// Adds `view` at `index`, or at the end for -1, unless the list holds
// Sync::maxViews already or a view with its id.
QString add(QList<SavedView> &views, const SavedView &view, qsizetype index = -1);
// Each refuses, unless `expected` is null, when the view in the list is not
// `expected`, whatever its place, so a change worked out from a copy read
// earlier never overwrites what a pull or another process wrote since, and
// then says changedMeanwhile().
QString changedMeanwhile();
QString replace(QList<SavedView> &views, const SavedView &view, const SavedView *expected = nullptr);
QString remove(QList<SavedView> &views, const QString &id, const SavedView *expected = nullptr);
// Moves the view `id` to `index`, kept within the list.
QString move(QList<SavedView> &views, const QString &id, qsizetype index);

// Reads the saved views, lets `change` edit them, and saves them numbered
// again in their order, with them marked to publish for the sync account,
// if one is set (Store::markViewsEdited()), in one transaction, so a pull
// meanwhile is never lost and the change never stays unpublished. Returns 1 once saved, 0 when `change` refused, with
// why in `problem`, and -1 when the library failed (Store::lastError()).
int save(Store &store, const std::function<QString(QList<SavedView> &)> &change, QString *problem);
}
