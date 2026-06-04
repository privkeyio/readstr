# Readstr Chrome Extension

A Chrome browser extension for Readstr. Makes it easy to check feeds, get notifications, and subscribe to new RSS/Nostr content right from the browser.

Similar to the Feeder.co RSS extension.

## Features

- Popup shows list of subscribed feeds and unread count
- Background checks for new items and shows notifications
- One-click subscribe: detects RSS/Atom feeds on pages and adds them
- Right-click context menu to subscribe
- Options page to add/remove feeds and change settings

Built with TypeScript and Manifest V3.

## Installation

1. Clone the repo or download the extension folder.
2. Run `npm install` then `npm run build` to compile the extension.
3. Open Chrome and go to `chrome://extensions/`
4. Enable "Developer mode"
5. Click "Load unpacked"
6. Select the `dist/` folder (with manifest.json)

## Usage

- Click the extension icon for popup with feeds
- Get notifications for new items
- On any page with RSS, use context menu or auto-detect to add feed

## Development

- Edit files in `src/`
- Build with your setup (e.g., tsc or esbuild)
- Reload extension in Chrome to test

## License

Same as Readstr repo (MIT or whatever it uses).

Feedback welcome!
