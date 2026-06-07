# Mobile & PWA Support

## Mobile Responsive Design

The app is fully responsive and optimized for mobile devices:

### Features
- **Hamburger Menu**: On mobile, tap the menu icon to access feeds and tags
- **Adaptive Layout**: 3-panel layout adapts to single-column on mobile
- **Touch-Friendly**: All buttons and controls are optimized for touch input
- **Back Navigation**: Swipe or tap back button to navigate between article list and content
- **Mobile Header**: Fixed header with quick access to add feeds

### Breakpoints
- **Mobile**: < 768px (single column, hamburger menu)
- **Tablet/Desktop**: >= 768px (multi-column layout)

## Progressive Web App (PWA)

Install Nostr Feedz as a standalone app on your device!

### Installation

#### On Mobile (iOS/Android)
1. Open the app in your mobile browser
2. Tap the "Share" or "Menu" button
3. Select "Add to Home Screen"
4. The app will install with an icon on your home screen

#### On Desktop (Chrome/Edge)
1. Look for the install icon in the address bar
2. Click "Install Nostr Feedz"
3. The app will open in its own window

### PWA Features
- **Offline Support**: Service worker caches static assets for offline access
- **App-like Experience**: Runs in standalone mode without browser UI
- **Fast Loading**: Cached resources load instantly
- **Install Prompt**: Browser prompts users to install the app
- **Manifest**: Full PWA manifest with app name, icons, and theme colors

### Technical Details

#### Service Worker
- Automatically registered via `next-pwa`
- Caches static assets (JS, CSS, images, fonts)
- Network-first strategy for API calls
- Cache-first for static resources

#### Build
To build with PWA support:
```bash
npm run build -- --webpack
```

**Note**: Next.js 16 uses Turbopack by default, but `next-pwa` requires webpack. The `--webpack` flag ensures proper PWA generation.

#### Icons
- SVG icon at `/public/icon.svg`
- PNG icons: 192x192 and 512x512
- Apple touch icon support
- Maskable icons for Android

## Development

When running in development mode (`npm run dev`), the service worker is disabled to prevent caching issues.

## Browser Support

- **iOS Safari**: 11.3+
- **Android Chrome**: Full support
- **Desktop Chrome/Edge**: Full support
- **Firefox**: Partial support (no install prompt)
