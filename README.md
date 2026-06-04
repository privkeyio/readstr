# Nostr-Feedz (Active Fork)

**This is an active fork of the original [Nostr-Feedz](https://github.com/PlebOne/Nostr-Feedz).**
The original repository appears to be no longer maintained. This fork is actively maintained and improved.

A modern, Google Reader-style feed aggregator that combines traditional RSS feeds with Nostr's decentralized long-form content (NIP-23) and video content.

## Improvements in This Fork

- **Automatic bidirectional sync** with category support
- **Dual organization modes**: Tags or Categories, with an Uncategorized fallback
- **Chrome extension** with feed detection, background sync, offline caching, and desktop notifications
- **Admin Dashboard** for managing the feed directory
- **Go/Charm TUI client** with a CLI development guide
- Configurable Nostr relays with the missing `nostrRelays` migration restored
- Fixes to organization mode switching and overall stability

## Overview

Nostr-Feedz is a full-stack web application built with Next.js that provides a unified reading experience for RSS feeds, Nostr long-form content, and video feeds from YouTube and Rumble. It features a clean, three-panel interface reminiscent of Google Reader, allowing users to subscribe to their favorite blogs, Nostr authors, and video channels in one place.

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
- Nostr-based authentication using browser extensions (nos2x, Alby, etc.)
- No centralized account system
- Your npub is your identity

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

## Getting Started

### Prerequisites
- Node.js 18+
- PostgreSQL database
- Nostr browser extension (nos2x, Alby, etc.)

### Installation

1. Clone the repository:
```bash
git clone https://github.com/privkeyio/Nostr-Feedz.git
cd Nostr-Feedz
```

2. Install dependencies:
```bash
npm install
```

3. Set up environment variables:
Create a `.env` file in the root directory:
```env
DATABASE_URL="postgresql://user:password@localhost:5432/nostr_feedz"
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
Public API for accessing the Nostr feed directory. See [GUIDE_API.md](./GUIDE_API.md) for full documentation.

### Subscription Sync Protocol
Cross-app subscription sync using Nostr kind 30404 events. See [SUBSCRIPTION_SYNC.md](./SUBSCRIPTION_SYNC.md) for implementation details.

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
```

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## License

MIT

## Acknowledgments

- Inspired by Google Reader's clean interface
- Built on the Nostr protocol for decentralized identity
- Thanks to the T3 Stack for the excellent Next.js template
