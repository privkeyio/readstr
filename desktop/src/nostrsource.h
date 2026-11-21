#pragma once

#include <QList>
#include <QString>
#include <optional>

#include "feedparser.h"
#include "nostrevent.h"

// Turns an author's verified events into what the library stores for a
// feed: kind 0 into the feed's details, kind 30023 (NIP-23 long-form) and
// kinds 21 and 22 (NIP-71 video) into its articles. Text fields are cleaned
// as a feed's are; an article's content stays the author's Markdown, and a
// video's is its description as HTML, line breaks kept.
namespace NostrSource {
inline constexpr int profileKind = 0;
inline constexpr int longFormKind = 30023;
inline constexpr int videoKind = 21;
inline constexpr int shortVideoKind = 22;
// Media and thumbnail URLs longer than this are dropped.
inline constexpr qsizetype maxMediaUrlLength = 8 * 1024;
// Profile metadata past this is ignored rather than parsed.
inline constexpr int maxProfileBytes = 64 * 1024;
// A video's duration past this many seconds (about 115 days) is taken for
// none, so a hostile one cannot make a label hundreds of digits long.
inline constexpr double maxDuration = 1e7;

struct Profile {
    // Its title, site, and description.
    ParsedFeed feed;
    // The kind 0 event's created_at, and its id, which breaks a tie.
    qint64 revision = 0;
    QString eventId;
};
// The feed's details from the newest kind 0 event among `events`, or null
// when there is none that reads as a profile.
std::optional<Profile> profile(const QList<NostrEvent> &events, const QString &pubkey);
// The newest version of each long-form article among `events`, newest
// first, at most FeedParser::maxItems. Each one's guid is the event id, as
// in the web app, and its link opens it on njump.me.
QList<ParsedItem> longForm(const QList<NostrEvent> &events, const QString &author);
// The videos among `events`, newest first, at most FeedParser::maxItems,
// as the web app's parseNip71Event reads them. Videos are regular events, so
// each is its own article, its guid the event id, its link the nevent on
// njump.me. What plays comes from the NIP-92 imeta tags: the first whose url
// is http(s), not on this machine or the local network, and whose m (MIME
// type), if any, is video/*. Its image is the thumbnail, or else the first
// such image of any imeta, and its duration counts when it is a finite
// number of seconds above 0 and at most maxDuration; 0 means unknown.
QList<ParsedItem> videos(const QList<NostrEvent> &events, const QString &author);
// longForm() and videos() together, newest first, at most FeedParser::maxItems
// in all: what the library saves for an author.
QList<ParsedItem> articles(const QList<NostrEvent> &events, const QString &author);
}
