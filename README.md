# Readstr

A modern, Google Reader-style feed aggregator that combines traditional RSS feeds with Nostr's decentralized long-form content (NIP-23) and video content.

## Improvements

- **Automatic bidirectional sync** with category support
- **Dual organization modes**: Tags or Categories, with an Uncategorized fallback
- **Chrome extension** with feed detection, background sync, offline caching, and desktop notifications
- **Admin Dashboard** for managing the feed directory
- **Go/Charm TUI client** with a CLI development guide
- **Readstr for Omarchy**: a native Linux desktop app, now in development ([plan](docs/OMARCHY_APP_PLAN.md))
- Configurable Nostr relays with the missing `nostrRelays` migration restored
- Fixes to organization mode switching and overall stability

## Overview

Readstr is a full-stack web application built with Next.js that provides a unified reading experience for RSS feeds, Nostr long-form content, and video feeds from YouTube and Rumble. It features a clean, three-panel interface reminiscent of Google Reader, allowing users to subscribe to their favorite blogs, Nostr authors, and video channels in one place.

Readstr runs on the web, installs as a PWA, has a Chrome extension, and is getting a native desktop app for Linux and [Omarchy](https://omarchy.org). Your Nostr key keeps subscriptions and read state in sync across all of them.

## Features

### Feed Management
- Subscribe to RSS/Atom feeds with automatic feed discovery
- Subscribe to Nostr users for their long-form content (NIP-23)
- **Subscribe to video channels** (YouTube and Rumble)
- Manual refresh or automatic feed updates
- Remove feeds with one click
- Unread count tracking per feed
- **Dual organization modes**: Tags or Categories
- Filter feeds by tags or categories

### Content Reading
- Three-panel Google Reader-style interface
- Clean, readable content formatting for Markdown, HTML, and plain text
- **Embedded video player** for YouTube and Rumble content
- Mark articles as read/unread
- **Configurable mark-as-read behavior** (on open, after 10 seconds, or manual)
- Full-screen article view with proper typography
- Independent scrolling for feed list, article list, and content pane
- **Favorites system** - Star articles for later reading
- **Dark mode support** with optimized readability

### Organization & Discovery
- **Sidebar view toggle**: Switch between Feeds, Tags, Categories, and Favorites
- **Tag-based filtering**: Select one or more tags to filter feeds
- **Category system**: Create custom categories with colors and icons
- **Unread counts per tag/category**: See total unread items for each
- **Feed counts per tag/category**: Track how many feeds belong to each
- **Tag sorting**: Alphabetically or by unread count
- Smart tag management with visual pill interface

### Nostr Integration
- Profile search across configured relays
- Popular user discovery
- Customizable relay configuration
- NIP-23 long-form content support
- **NIP-94 video content support**
- Display user names and profile information
- **Share articles to Nostr** with attribution

### Cross-Device Sync
- **Subscription sync via Nostr** (kind 30404 events)
- Export subscriptions to Nostr relays
- Import subscriptions from Nostr relays
- Automatic sync detection on login
- **Cross-app compatibility** with documented sync protocol

### RSS Features
- Intelligent feed discovery (supports homepage URLs)
- Checks common feed locations (/feed, /rss, /atom.xml, etc.)
- Parses HTML for feed links
- Supports RSS, Atom, and JSON Feed formats
- **YouTube channel RSS feeds** auto-discovery
- **Rumble channel RSS feeds** support

### Video Support
- **YouTube video embedding** with proper iframe integration
- **YouTube Shorts support**
- **Rumble video embedding**
- Video thumbnail extraction
- Clean video player interface

### Authentication
- **NIP-07** signing via browser extensions (nos2x, Alby, etc.) on desktop
- **NIP-46 remote signing (Nostr Connect)** — pair a remote signer such as Amber with a `bunker://` connection string, for Android and other browsers without an extension
- Read-only npub view for browsing without a signer
- No centralized account system — your npub is your identity

### Guide Directory
- **Public catalog of Nostr feeds** for discovery
- Browse featured Nostr authors by topic
- Submit new feeds to the directory
- Featured feeds curation

### Progressive Web App (PWA)
- **Installable as a mobile app**
- Offline-capable service worker
- App manifest for home screen installation

### Chrome Extension
- **Browser toolbar access** with unread count badge
- **Feed detection** on any web page with RSS/Atom links
- **One-click subscribe** via context menu or toolbar popup
- **Background sync** with configurable polling interval
- **Offline support** using IndexedDB for cached feeds and items
- **Desktop notifications** for new articles
- **Nostr authentication** via NIP-07 extension or nsec key

## Readstr for Omarchy (desktop)

> **In development.** Nothing below is released yet. This section describes the planned app;
> progress is tracked in the beads epic `readstr-gp3`, and the full design is in
> [docs/OMARCHY_APP_PLAN.md](docs/OMARCHY_APP_PLAN.md).

A native reader for RSS and Nostr on Linux and Omarchy, written in C++ and Qt Quick.
Its three panes (sources, article list and reading view) keep your subscriptions and
articles on your computer. If you sign in with your npub, it stays in sync with Readstr
on the web, your phone and the Chrome extension.

### Features

- **RSS, Atom and JSON Feed**, with feed discovery from a site's homepage
- **Nostr long-form (NIP-23)** and **video (NIP-94)** from the authors you follow, plus YouTube and Rumble channels; videos play in `mpv`
- **Sync over Nostr** using the same events as the web app (kinds 30404, 30405 and 30406)
  - Read-only with just an npub
  - Publish changes with a NIP-46 remote signer (such as Amber) or a key kept in your system keyring
- **Tags, categories, saved views and keyword filters**, like on the web
- **OPML import and export** through the desktop file picker
- **Full-text search** across everything stored locally
- **Follows your Omarchy theme** and language as they change, and window corners match Hyprland's rounding
- **Keyboard first**, with every action on a key and `?` to list them
- **Command line with `--json`** for scripts, agents and the Omarchy bar
- **Free and local-first**: no account required, no telemetry, nothing runs in the background after you close it. An optional paid Readstr account (through Flash) will add server-side extras later; reading always stays free.

### Install

Once released:

```bash
omarchy pkg add readstr
```

or use **Omarchy menu → Install → Package**. The app then shows up in the launcher (`Super + Space`).
An Omarchy bar widget, `readstr-bar`, will show your unread count:

```bash
omarchy plugin add <readstr-bar repo URL> --enable
```

It runs on any Linux desktop with Qt 6, not just Omarchy. Elsewhere it falls back to a built-in dark theme and your system language.

### Keyboard

| Key | Action |
|---|---|
| `J` / `K` or `↓` / `↑` | Next / previous article |
| `N` | Next unread |
| `Space` / `Shift+Space` | Scroll forward / back |
| `M` / `S` | Toggle read / star |
| `O` | Open original in browser |
| `V` | Play video in mpv |
| `1`–`5` | Feeds / Tags / Categories / Favorites / Views |
| `Ctrl+R` / `Ctrl+F` | Refresh / search |
| `Ctrl+N` | Add feed |
| `Ctrl+Shift+A` / `Ctrl+Z` | Mark view read / undo |
| `Ctrl+Shift+S` | Sync now |
| `Q` | Quit |
| `?` | Show all shortcuts |

### Command line

`readstr` on its own opens the window. With a command it runs without a window, and every command takes `--json`:

```text
readstr feeds | articles [--unread] | article <id> | search <query>
readstr subscribe <url|npub> | unsubscribe <id> | read|unread|star <id...>
readstr refresh | import <file.opml> | export | unread
readstr sync pull|push|status
```

Feed content is written by strangers, so scripts and AI agents should treat article text as untrusted input.

### Storage and privacy

- Articles and settings live in `~/.local/share/readstr`, `~/.config/readstr` and `~/.cache/readstr`.
- Sync events are public Nostr events, as on the web: they link your npub to your subscriptions and read state.
- Signing keys never go in a config file. Use a remote signer, or the system keyring.
- Article scripts, frames and forms are never run. Loading remote images can be turned off.

## Tech Stack

### Frontend
- **Next.js 16** - React framework with App Router
- **TypeScript** - Type safety throughout
- **Tailwind CSS** - Utility-first styling
- **tRPC** - End-to-end type-safe APIs
- **TanStack Query** - Data fetching and caching

### Backend
- **Next.js API Routes** - Serverless functions
- **Prisma** - Type-safe database ORM
- **PostgreSQL** - Primary database
- **tRPC** - Type-safe API layer

### Nostr
- **nostr-tools** - Nostr protocol implementation
- **SimplePool** - Relay connection pooling
- Configurable relay support

### Content Processing
- **xml2js** - RSS/Atom feed parsing
- **cheerio** - HTML parsing for feed discovery
- **react-markdown** - Markdown rendering
- **rehype-sanitize** - HTML sanitization
- **remark-gfm** - GitHub Flavored Markdown support

### Deployment
- **Docker** - Containerized deployment
- **Docker Compose** - Multi-container orchestration
- **Caddy** - Reverse proxy with automatic HTTPS

## Dependency Overrides

The `overrides` block in `package.json` pins transitive dependencies to patched
versions that remediate known advisories:

- **serialize-javascript** `^7.1.3` — fixes RCE via `RegExp.flags` /
  `Date.prototype.toISOString()` ([GHSA-5c6j-r48x-rmvq](https://github.com/advisories/GHSA-5c6j-r48x-rmvq), affected `<= 7.0.2`)
  and XSS via an unescaped `</script` at the end of a serialized function body
  ([GHSA-m5mm-p8h7-m29v](https://github.com/advisories/GHSA-m5mm-p8h7-m29v), affected `< 7.1.3`).
- **postcss** `$postcss` — forces transitive consumers (including `next`'s
  pinned copy) onto the top-level `postcss` (`^8.5.23`), past the line-return
  parsing flaw ([GHSA-7fh5-64p2-3v2j](https://github.com/advisories/GHSA-7fh5-64p2-3v2j), affected `< 8.4.31`)
  and the `sourceMappingURL` arbitrary `.map` file reads
  ([GHSA-r28c-9q8g-f849](https://github.com/advisories/GHSA-r28c-9q8g-f849), affected `<= 8.5.17`;
  [GHSA-fxqj-rqcc-2cmp](https://github.com/advisories/GHSA-fxqj-rqcc-2cmp), affected `<= 8.5.22`).
- **postcss-selector-parser** `^6.1.5` (scoped to `@tailwindcss/typography`,
  which pins `6.0.10`) — fixes quadratic-time parsing of flat selectors
  ([GHSA-rj75-hqrm-r3gf](https://github.com/advisories/GHSA-rj75-hqrm-r3gf), affected `< 6.1.5`)
  and uncontrolled AST recursion
  ([GHSA-w9m9-85wc-3x92](https://github.com/advisories/GHSA-w9m9-85wc-3x92), affected `>= 6.1.0, < 6.1.3`).

## Getting Started

### Prerequisites
- Node.js 18+
- PostgreSQL database
- Nostr browser extension (nos2x, Alby, etc.)

### Installation

1. Clone the repository:
```bash
git clone https://github.com/privkeyio/readstr.git
cd readstr
```

2. Install dependencies:
```bash
npm install
```

3. Set up environment variables:
Create a `.env` file in the root directory:
```env
DATABASE_URL="postgresql://user:password@localhost:5432/readstr"
```

4. Initialize the database:
```bash
npx prisma generate
npx prisma db push
```

5. Run the development server:
```bash
npm run dev
```

6. Open [http://localhost:3000](http://localhost:3000) in your browser

### Docker Deployment

1. Copy the environment example:
```bash
cp .env.production.example .env.production
```

2. Configure your environment variables in `.env.production`

3. Build and run with Docker Compose:
```bash
docker compose up -d
```

## Usage

### First Time Setup
1. Click "Connect with Nostr" on the homepage
2. Authorize the connection using your Nostr browser extension
3. You'll be redirected to the feed reader

### Adding Feeds

**RSS Feeds:**
1. Click "Add Feed" in the sidebar
2. Select "RSS Feed" tab
3. Enter the feed URL (or website homepage)
4. Optionally assign tags or a category
5. The app will automatically discover and subscribe to the feed

**Nostr Feeds:**
1. Click "Add Feed" in the sidebar
2. Select "Nostr User" tab
3. Search for users by name or NIP-05 address
4. Or manually enter an npub
5. Click on a profile to subscribe

**Video Channels:**
1. Click "Add Feed" in the sidebar
2. Select "Video Channel" tab
3. Paste a YouTube or Rumble channel URL
4. The app will discover and subscribe to the channel's RSS feed

### Managing Feeds
- Click the ⋮ menu on a feed to see options
- Mark all items as read
- Set category (in categories mode)
- Edit tags (in tags mode)
- Delete the subscription

### Organizing with Tags vs Categories

**Tags Mode:**
- Assign multiple tags to each feed
- Filter by selecting one or more tags
- Sort tags alphabetically or by unread count

**Categories Mode:**
- Create categories with custom icons and colors
- Assign one category per feed
- View feeds grouped by category in the sidebar

To switch modes:
1. Open Settings (gear icon)
2. Go to "Feed Organization" tab
3. Choose Tags or Categories

### Configuring Relays
1. Click the gear icon in the sidebar
2. Go to "Nostr Relays" tab
3. Add or remove Nostr relays
4. Use quick-add buttons for popular relays
5. Click "Reset to Defaults" to restore default relays

### Syncing Subscriptions
1. Open Settings (gear icon)
2. Go to "Sync" tab
3. Click "Export to Nostr" to save your subscriptions
4. Click "Import from Nostr" to restore on another device
5. Requires a Nostr browser extension

### Reading Preferences
1. Open Settings (gear icon)
2. Go to "Reading" tab
3. Choose when to mark articles as read:
   - When you open an article
   - After 10 seconds of reading
   - Never (manual only)

## API Documentation

### Guide API
Public API for accessing the Nostr feed directory. See [GUIDE_API.md](./docs/GUIDE_API.md) for full documentation.

### Subscription Sync Protocol
Cross-app subscription sync using Nostr kind 30404 events. See [SUBSCRIPTION_SYNC.md](./docs/SUBSCRIPTION_SYNC.md) for implementation details.

## Database Schema

The application uses the following main models:

- **Feed** - Stores RSS and Nostr feed information
- **FeedItem** - Individual articles/posts with video metadata
- **Subscription** - User subscriptions to feeds with tags/category
- **Category** - User-defined categories with colors and icons
- **UserPreference** - User settings including organization mode
- **ReadItem** - Tracks which items users have read
- **Favorite** - User's favorited articles
- **GuideFeed** - Public directory of Nostr feeds
- **NostrRelay** - Configurable relay list

## Project Structure

```plaintext
src/
├── app/                    # Next.js app router pages
│   ├── api/               # API routes
│   │   ├── guide/        # Guide directory API
│   │   ├── nostr-rss/    # Nostr-to-RSS converter
│   │   └── webhooks/     # External webhooks
│   ├── guide/            # Guide directory pages
│   ├── reader/           # Main feed reader
│   └── subscribe/        # Subscription management
├── components/            # React components
│   ├── feed-reader.tsx   # Main reader interface
│   ├── add-feed-modal.tsx# Feed subscription UI
│   ├── settings-dialog.tsx# Settings with tabs
│   ├── formatted-content.tsx # Content renderer
│   └── video-embed.tsx   # Video player component
├── contexts/             # React contexts
│   └── NostrAuthContext.tsx
├── lib/                  # Utilities
│   ├── nostr-fetcher.ts # Nostr protocol interactions
│   ├── nostr-sync.ts    # Subscription sync
│   ├── rss-parser.ts    # RSS feed parsing
│   ├── feed-discovery.ts# RSS feed discovery
│   └── video-parser.ts  # Video URL parsing
├── server/              # Backend code
│   └── api/
│       └── routers/
│           ├── feed.ts  # Main feed router
│           ├── guide.ts # Guide directory router
│           └── subscription.ts
└── prisma/
    └── schema.prisma    # Database schema

chrome-extension/
├── src/
│   ├── background.ts    # Service worker for sync and notifications
│   ├── content.ts       # Feed detection on web pages
│   ├── nostr.ts         # Nostr authentication helpers
│   └── db/              # IndexedDB caching layer
├── manifest.json        # Extension manifest (V3)
└── dist/                # Built extension (load unpacked)

desktop/                 # Readstr for Omarchy (in development)
├── src/                 # C++ backend + QML interface
├── tests/               # Qt Test suite
└── pkgbuild/            # PKGBUILD, .desktop entry, icon
```

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## License

MIT

## Acknowledgments

- Originally based on [Nostr-Feedz](https://github.com/PlebOne/Nostr-Feedz)
- Inspired by Google Reader's clean interface
- Built on the Nostr protocol for decentralized identity
- Thanks to the T3 Stack for the excellent Next.js template
- The desktop app follows [omafeed](https://github.com/pch/omafeed) and Omarchy's app conventions
