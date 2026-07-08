# Chrome Web Store Listing

## Extension Name
Readstr - RSS & Atom Feed Reader

## Short Description (132 characters max)
Discover and subscribe to RSS/Atom feeds on any webpage. Sync with your Readstr account for cross-device reading.

## Detailed Description

**Readstr Extension** makes it effortless to discover and subscribe to RSS and Atom feeds across the web.

### Features

**Automatic Feed Detection**
The extension automatically detects RSS and Atom feeds on any webpage you visit. A floating button appears when feeds are found, showing you exactly how many are available.

**One-Click Subscribe**
Subscribe to feeds instantly with a single click. No need to hunt for feed URLs or copy-paste them manually.

**Context Menu Integration**
Right-click on any link to subscribe to it as a feed, or use the context menu to see all detected feeds on a page.

**Nostr Account Sync**
Sign in with your Nostr identity (nsec or NIP-07 signer) to sync your subscriptions with the Readstr web app. Your feeds and read state stay synchronized across all your devices.

**Desktop Notifications**
Get notified when new items arrive in your feeds. Control notification frequency and limits to avoid interruptions.

**Local Storage**
Feeds can be stored locally in your browser and synced via Chrome's built-in sync, even without a Nostr account.

**OPML Import/Export**
Easily migrate your subscriptions from other feed readers using standard OPML format.

**Self-Hostable**
Works with self-hosted Readstr instances. Just configure your web app URL in settings.

### Privacy

- No data collection or analytics
- Private keys never leave your device
- All communication uses HTTPS
- Open source: https://github.com/privkeyio/readstr

### Permissions Explained

- **Host Permissions**: Required to detect feeds on any website and communicate with your configured Readstr instance
- **Storage**: Store your feed subscriptions and settings locally
- **Notifications**: Alert you when new feed items arrive
- **Alarms**: Periodically check for new content in the background

---

## Category
Productivity

## Language
English

---

## Screenshot Ideas

### Screenshot 1: Feed Detection (1280x800)
**Title:** Automatic Feed Detection
**Description:** Shows the floating button on a blog/news site with "3 feeds found" badge visible.
**Content:**
- Browser showing a popular blog (like Hacker News or a tech blog)
- Floating purple/blue button in bottom-right corner
- Badge showing number of detected feeds
- Clean, minimal browser UI

### Screenshot 2: Feed Menu (1280x800)
**Title:** One-Click Subscribe
**Description:** Shows the expanded feed menu with multiple detected feeds.
**Content:**
- Same blog page
- Expanded menu showing 2-3 detected feeds
- Each feed with title, type icon (RSS/Atom), and "Add" button
- "Add All Feeds" button at bottom

### Screenshot 3: Popup Overview (1280x800)
**Title:** Quick Access Popup
**Description:** Shows the extension popup with feed list and unread counts.
**Content:**
- Extension popup open
- 4-5 feeds listed with unread count badges
- "Refresh" and "Open App" buttons
- "Last sync: 2 min ago" at bottom
- Total unread badge in header

### Screenshot 4: Options Page (1280x800)
**Title:** Full Control in Settings
**Description:** Shows the options page with Nostr login and feed management.
**Content:**
- Options page in full tab
- Nostr Account section showing "Connected" with npub
- Local Feeds section with 3-4 feeds listed
- Notification settings visible
- Clean, organized layout

### Screenshot 5: Context Menu (1280x800)
**Title:** Right-Click to Subscribe
**Description:** Shows the context menu on a feed link.
**Content:**
- Browser showing a page with visible RSS link
- Right-click context menu open
- "Readstr" submenu expanded
- "Subscribe to this link as feed" option highlighted

### Screenshot 6: Notifications (1280x800)
**Title:** Stay Updated
**Description:** Shows a desktop notification for a new feed item.
**Content:**
- Browser in background
- Desktop notification visible
- Notification shows feed title, article title
- "Open" and "Mark as Read" buttons visible

---

## Promotional Tile Ideas

### Small Tile (440x280)
- Readstr logo centered
- "RSS feeds, everywhere" tagline
- Green gradient background

### Large Tile (920x680)
- Split view: left side shows feed detection on a webpage, right side shows the popup
- "Discover • Subscribe • Sync" at bottom
- Readstr branding

### Marquee (1400x560)
- Panoramic view of the extension in action
- Multiple browser windows showing different features
- "Your feeds. Synced everywhere." headline

---

## Generated Assets

Listing images live in `store-assets/` (regenerate from `store-assets/src/` via headless Chrome):

- `store-assets/promo-tile-440x280.png` — small promo tile
- `store-assets/screenshot-1-feed-detection-1280x800.png`
- `store-assets/screenshot-2-popup-1280x800.png`
- `store-assets/screenshot-3-options-1280x800.png`
- `store-assets/screenshot-4-context-menu-1280x800.png`

## Privacy Policy URL

https://readstr.privkey.io/legal/privacy

## Store Listing Checklist

- [x] Extension name (45 chars max): `Readstr - RSS & Atom Feed Reader`
- [x] Short description (132 chars max): See above
- [x] Detailed description: See above
- [x] Category: Productivity
- [x] Language: English
- [x] Screenshots: 4 at 1280x800 (`store-assets/`)
- [x] Small tile: 440x280 PNG (`store-assets/promo-tile-440x280.png`)
- [ ] Large tile: 920x680 PNG (optional)
- [ ] Marquee: 1400x560 PNG (optional)
- [x] Icon: Already included (128x128)
- [x] Privacy policy URL: https://readstr.privkey.io/legal/privacy
- [ ] Website URL (optional): https://readstr.privkey.io
