# Readstr

*A Google Reader-style aggregator for RSS feeds and Nostr long-form content.*

Readstr is a full-stack web application built with Next.js that provides a unified reading
experience for RSS/Atom feeds, Nostr long-form content (NIP-23), and video feeds from
YouTube and Rumble. It features a clean, three-panel interface reminiscent of Google
Reader, with keyless Nostr authentication (NIP-07) and cross-device sync of subscriptions
and read status over Nostr relays — the app never handles a private key.

## Where to start

- **[Mobile & PWA](./MOBILE_PWA.md)** — responsive design and installable PWA support.
- **[Feed Discovery](./FEED_DISCOVERY.md)** — how Readstr finds feeds from any URL.
- **[Tagging System](./TAGGING_SYSTEM.md)** — organizing feeds with tags and categories.
- **[Subscription Sync](./SUBSCRIPTION_SYNC.md)** — cross-device sync via Nostr events
  (kind 30404/30405), including [bidirectional sync](./BIDIRECTIONAL_SYNC.md),
  [category sync](./CATEGORY_SYNC.md), and [deletion/read-status
  tracking](./SYNC_IMPROVEMENTS.md).
- **[Guide API](./GUIDE_API.md)** — REST API exposing the curated feed directory.
- **[CLI Development Guide](./CLI_DEVELOPMENT_GUIDE.md)** — building the Go/Charm TUI
  client.
- **[Flash Integration](./FLASH_INTEGRATION.md)** — Bitcoin Lightning subscription
  payments via PayWithFlash.

## Project links

- Source: <https://github.com/privkeyio/readstr>
- Live app: <https://readstr.privkey.io>
- License: [MIT](https://github.com/privkeyio/readstr/blob/main/LICENSE)
