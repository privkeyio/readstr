# Changelog

All notable changes to the Readstr Extension will be documented in this file.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [3.1.0] - 2026-01-05

### Added
- Sync button with visual feedback (spinning, success, error states)
- Timeout cleanup for sync status to prevent memory leaks

### Fixed
- React state update on unmounted component warning

## [3.0.0] - 2026-01-05

### Added
- Initial release
- Feed synchronization with Readstr web app
- Offline caching with IndexedDB
- Real-time unread badge count
- Desktop notifications for new items
- Dark/light/system theme support
- Keyboard navigation (j/k, Enter, m, r, u, t)
- Feed detection on web pages
- Context menu to add feeds
- Search across cached items
- Favorites sync with server
- Virtual scrolling for large lists
- Bulk actions (mark as read)
- Statistics view
- OPML import/export in options
