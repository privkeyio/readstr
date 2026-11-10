# Readstr for Omarchy — Research & Plan

Status: proposal · 2026-11-10

A native desktop Readstr for [Omarchy](https://omarchy.org), in the spirit of
[omafeed](https://github.com/pch/omafeed): one window, three panes, keyboard
first, follows the Omarchy theme live, installs as an Arch package, and has a
headless `--json` CLI that agents and a bar widget can drive. What sets it apart
from omafeed is Readstr's own feature: **Nostr**. It reads NIP-23 long-form and
video, and it syncs with the Readstr web app, PWA and Chrome extension through
kinds 30404/30405/30406.

---

## 1. Research

### 1.1 omafeed (reference app)

| Aspect | omafeed |
|---|---|
| Stack | Rust workspace (`omafeed-core` + `omafeed-app`), GTK 4, libadwaita, WebKitGTK 6 |
| Storage | SQLite at `$XDG_DATA_HOME/omafeed/omafeed.db`, settings at `$XDG_CONFIG_HOME/omafeed/settings.toml`, icons in `$XDG_CACHE_HOME`; `OMAFEED_HOME` overrides all three (for tests) |
| Threads | DB on a dedicated worker thread, HTTP on Tokio |
| Features | RSS/Atom/JSON Feed, nested folders, keyword/regex filters, OPML, typo-tolerant full-text search, sanitized HTML |
| Theme | Reads Omarchy `colors.toml`, re-themes live, falls back to a built-in dark palette |
| Install | `omarchy pkg add omafeed` (in `omacom/omarchy-pkgs`), plus PKGBUILD, `-git` package, .deb/.rpm, `make install` into `~/.local` |
| CLI | No args opens the window. Otherwise headless: `import export refresh feeds articles search article read star subscribe unsubscribe`, with `--json` on most. README warns that feed text is untrusted input for LLMs |
| Bar | Separate repo [omafeed-bar](https://github.com/pch/omafeed-bar), a QML shell plugin (`manifest.json`, `Widget.qml`, `Model.js`). It **never opens the DB** and only shells out to the `omafeed` CLI, polls every 60 s, and has a `recount` IPC. Installed with `omarchy plugin add <git-url> --enable` |
| Privacy | No accounts, telemetry or daemon; refresh stops when the window closes; remote images can be turned off; JS, frames and forms disabled in articles |

**omafeed keymap** (worth matching so muscle memory carries over):
`J/K` next/prev · `N` next unread · `Space/Shift+Space` scroll · `M` read ·
`S` star · `O` open original · `Ctrl+R` refresh · `Ctrl+F` search ·
`Ctrl+N` add feed · `Ctrl+Shift+N` new folder · `Ctrl+Shift+A` mark view read ·
`Ctrl+Z` undo · `Ctrl+Q` quit · `?` shortcuts.

### 1.2 Omarchy app guide (`omarchy-app` skill)

Omarchy's first-party apps (Hype, Monologue, Omacut, Signed) follow these conventions:

- **C++17 + Qt Quick**, built with `qmake6 && make` into one binary, `QGuiApplication`,
  Material style (dark) with the theme accent. C++ owns state and work as `QObject`s
  with `Q_PROPERTY/Q_INVOKABLE`, and QML owns layout and keys.
- Layout: `<name>.pro`, `bin/{build,test,install}`, flat `src/`, `src/Main.qml`,
  `src/resources.qrc`, `src/i18n/<code>.json`, `tests/`, `pkgbuild/{PKGBUILD,.desktop,.svg}`.
- **Theme live**: watch `~/.local/state/omarchy/current/theme/colors.toml` (and re-arm
  the watcher, because theme switches replace symlinks). Fall back to `#FFD60A`.
  Corners follow Hyprland `decoration:rounding` via `hyprctl` (`theme.radius`).
- **i18n live**: `~/.config/omarchy/language`, `qsTr()` everywhere, RTL mirroring,
  locale-aware numbers and dates, translated `.desktop` `Name`/`Comment`.
- **Keyboard first**: `?` overlay, `Q` quits, Space is the main action, undo/redo.
- **File dialogs** go through xdg-desktop-portal (copy Omacut's `portalfilepicker`), never `QFileDialog`.
- **Agent-drivable CLI**: headless subcommands with `--json`, exit codes 0 or 1,
  `QT_QPA_PLATFORM=offscreen`, and `<name> skill install` bundling an agent skill (Hype's `src/cli.cpp`).
- **Ship**: tag + GitHub release, then a PR to `omacom/omarchy-pkgs` under
  `pkgbuilds/<name>/` (`.omarchy/package.json`: `source: local`, `auto_merge`,
  `upstream.watch`).

> **Stack decision:** omafeed is Rust/GTK, but the Omarchy guide asks for Qt Quick, and
> Omarchy's shell and bar plugins are QML too. **Recommendation: follow the Omarchy guide
> (C++/Qt Quick).** The theme, i18n and portal code comes from the templates, and
> QML skills carry over to the bar plugin.

### 1.3 What Readstr already has to reuse or port

| Readstr source | Desktop use |
|---|---|
| `docs/SUBSCRIPTION_SYNC.md`, `src/lib/nostr-sync.ts` | **Wire protocol** to implement: kind 30404 (subs, `d=readstr-subscriptions`), 30405 (read GUIDs), 30406 (saved views, with `keywords.exclude` kept local). These events are **cleartext on public relays by design**, so a read-only npub can pull subscriptions without a signer |
| `docs/CLI_DEVELOPMENT_GUIDE.md` | Already-designed local SQLite schema (feeds, feed_items, tags, categories, preferences). Reuse it nearly as-is |
| `src/lib/rss-parser.ts`, `video-parser.ts`, `video-feed.ts` | RSS/Atom/JSON Feed + YouTube/Rumble edge cases to port |
| `src/lib/feed-discovery.ts` | Homepage → feed discovery (`/feed`, `/rss`, `<link rel=alternate>`) |
| `src/lib/nostr-fetcher.ts`, `nostr-profile.ts`, `relays.ts` | NIP-23 (kind 30023) fetch, profile (kind 0) lookup, default relays |
| `src/lib/opml.ts`, `keyword-filter.ts`, `saved-views.ts` (+ tests) | OPML, mute/include filters, smart views. **Port the vitest cases as Qt Test fixtures** |
| `src/lib/safe-url.ts` | Scheme allow-listing for links and images |
| `src/server/auth/nip98.ts` | Only needed for the optional server mode (§2.2) |
| Chrome extension | Has the same offline-first model (IndexedDB plus background sync). Use it to check the sync merge semantics |

---

## 2. Product decisions

### 2.1 The one job

> *Read your RSS, Nostr long-form and video subscriptions in a native Omarchy window,
> and keep them in sync with Readstr everywhere else.*

Out of scope for v1: guide directory/admin, AI summaries, sharing to Nostr, and
the Flash/subscription paywall. These stay on the web.

### 2.2 Data model: local-first, Nostr for sync (recommended)

| Option | Pros | Cons |
|---|---|---|
| **A. Local-first SQLite + Nostr sync (recommended)** | Works offline and with no account, like omafeed. Interoperates with web, PWA and extension through 30404/30405/30406. No dependency on the server | Has to fetch RSS itself and port the parsers |
| B. Thin client of `readstr.privkey.io` tRPC with NIP-98 | Least parsing code | Needs a signer for every request, tRPC isn't a stable public API, and no offline use. It's also against the omafeed and Omarchy model |

Go with A. B can be added later as an optional "server account" backend behind the same `Store` interface.

### 2.3 Identity and signing

1. **No identity**: a pure local reader, like omafeed.
2. **npub (read-only)**: pulls subscriptions, read status and views from relays. No secrets.
3. **Signer** to *publish* sync events, in order of preference:
   - **NIP-46 bunker** (`bunker://`, e.g. Amber). The key never touches the desktop. This mirrors the web app.
   - **nsec in the Secret Service** (gnome-keyring via `org.freedesktop.secrets` over D-Bus), never in plaintext config.

### 2.4 Article rendering

- **v1: sanitized HTML → Qt rich-text subset** (`QTextDocument`) in a `Flickable`,
  with C++ doing the sanitizing (allow-list tags and attributes, `http/https` only).
  This means no JS engine and a small binary, which matches omafeed's "no JS, frames or
  forms" stance. Images go through an async `QQuickImageProvider`/cache in
  `$XDG_CACHE_HOME/readstr`, behind a "load remote images" toggle.
- **Markdown** (NIP-23): `QTextDocument::setMarkdown` (CommonMark + GFM) and then the same sanitizer.
- **Video** (YouTube, Rumble, NIP-94): show the thumbnail. `Space`/`Enter` plays in **mpv**
  (on Omarchy, with yt-dlp), and `O` opens the browser. No embedded web view.
- *Escape hatch:* if the typography falls short, put a locked-down `QtWebEngine` view
  (JS off) behind a build flag. It costs about 100 MB of runtime, so it isn't the default.

---

## 3. Architecture

```text
readstr/desktop/                (binary, package, app id: readstr)
├── readstr.pro
├── bin/{build,test,install}
├── src/
│   ├── main.cpp            arg dispatch: no args / `open` → GUI, else cli.cpp
│   ├── cli.cpp             headless commands, --json, offscreen platform
│   ├── store.{h,cpp}       SQLite (QtSql) on a worker thread, migrations, FTS5
│   ├── fetcher.{h,cpp}     QNetworkAccessManager, ETag/Last-Modified, scheduler
│   ├── feedparser.{h,cpp}  RSS / Atom / JSON Feed (QXmlStreamReader / QJson)
│   ├── discovery.{h,cpp}   homepage → feed URL
│   ├── nostr/
│   │   relaypool.{h,cpp}   QWebSocket pool, REQ/EOSE/CLOSE, timeouts
│   │   event.{h,cpp}       NIP-01 serialize/id, schnorr verify (libsecp256k1)
│   │   nip19.{h,cpp}       npub/nprofile/naddr bech32
│   │   signer.{h,cpp}      abstract; LocalKeySigner, Nip46Signer (NIP-44)
│   │   sync.{h,cpp}        30404/30405/30406 pull, merge, publish
│   ├── sanitizer.{h,cpp}   HTML allow-list → Qt rich text
│   ├── opml.{h,cpp}  filters.{h,cpp}  video.{h,cpp}
│   ├── models/             QAbstractListModels: sources, articles
│   ├── backend.{h,cpp}     façade exposed to QML
│   ├── theme.{h,cpp}  translator.{h,cpp}   (from templates)
│   ├── filepicker.h  portalfilepicker.{h,cpp}  (from Omacut, for OPML)
│   ├── Main.qml  SourcePane.qml  ArticleList.qml  ReaderPane.qml
│   ├── AddFeedDialog.qml  ShortcutsOverlay.qml  SettingsDialog.qml
│   ├── i18n/<code>.json   skill/SKILL.md (bundled)
│   └── resources.qrc
├── tests/readstr_tests.{pro,cpp}  + fixtures/ (feeds, events, OPML)
└── pkgbuild/{PKGBUILD,readstr.desktop,readstr.svg}
```

**Dependencies** (Qt modules beyond the skill's base set): `qt6-websockets` (relays),
`QtSql` with SQLite (in `qt6-base`), `libsecp256k1` (schnorr sign/verify), and
OpenSSL (already pulled in by Qt) for NIP-44's ChaCha20 and HMAC.
Runtime is optional: `mpv`, `yt-dlp`. **Neither `qt6-websockets` nor `libsecp256k1` is
installed on this machine.** Add them to `depends`.

**Paths**: `$XDG_DATA_HOME/readstr/readstr.db`, `$XDG_CONFIG_HOME/readstr/settings.toml`
(or `.json`), `$XDG_CACHE_HOME/readstr/` (images, favicons). `READSTR_HOME`
overrides all three for tests and the offscreen harness.

**Threads**: the DB worker thread owns the connection. Network and WebSockets stay on the
main event loop (Qt is async). Parsing and sanitizing go to `QtConcurrent`.

**Refresh**: only while the window is open (as omafeed does), every 15–30 min with jitter,
using conditional GET. The bar plugin's "Refresh" calls `readstr refresh`.

### 3.1 Sync semantics (must match web/extension)

- **30404**: union of `rss` and `nostr` minus `deleted`, merged by `lastUpdated`.
  Tags and categories are per-source maps. Publish after local subscribe/unsubscribe
  (debounced), with `["client","readstr-omarchy"]`.
- **30405**: union of read GUIDs, capped the same way the web app caps them (check `nostr-sync.ts`).
- **30406**: saved views. **Never publish `keywords.exclude`**, keep it local only.
- Verify every event signature and that `pubkey` equals the user before merging.
- Show "Sync: last pulled / last published / no signer" in the status bar. Say what's
  wrong, never fail silently.

---

## 4. UX

Three panes (Sources · Articles · Reader), collapsible, matching the web app's layout.
Sidebar modes: **Feeds / Tags / Categories / Favorites / Views**, with unread badges.

**Keymap**: the omafeed table above, plus:

| Key | Action |
|---|---|
| `V` | Play video in mpv |
| `1`–`5` | Sidebar mode (Feeds/Tags/Categories/Favorites/Views) |
| `Ctrl+Shift+S` | Sync now (pull + publish) |
| `Q` | Quit (Omarchy convention; `Ctrl+Q` also works) |

Theme: background, foreground and accent come from `colors.toml`, and radius from `hyprctl`.
The reader typography uses the Omarchy monospace/sans font from the theme when present.

---

## 5. CLI (agent and bar surface)

`readstr` with no args (or `readstr open`) opens the GUI. Everything else runs headless,
supports `--json`, and exits 0 or 1 with errors on stderr:

```text
readstr feeds [--json]                         readstr subscribe <url|npub|nprofile>
readstr articles [--feed ID] [--unread] [--limit N]   readstr unsubscribe <id>
readstr article <id>                           readstr read|unread|star <id...>
readstr search <query>                         readstr refresh [--feed ID]
readstr unread --json   → {"total":N,"byFeed":{...}}   (bar widget)
readstr import <file.opml> | export            readstr sync pull|push|status
readstr skill install                          (bundled agent skill)
```

As omafeed does, the README and the bundled skill say that article content is untrusted input.

---

## 6. Omarchy bar plugin — `readstr-bar` (separate repo)

Copy omafeed-bar's shape: `manifest.json`, `Widget.qml`, `Model.js`. It shells out to
`readstr unread --json` every `pollSeconds` (60), has a `recount` IPC, and a click opens or focuses
the app (`hyprctl dispatch focuswindow class:readstr || readstr`). It never touches the DB.
Validate with `omarchy plugin validate`, and install with
`omarchy plugin add <git-url> --enable`.

---

## 7. Phased plan

| Phase | Deliverable | Done when |
|---|---|---|
| **0. Scaffold** | Repo from `omarchy-app` templates. Theme, i18n, `?` overlay, `bin/*`, PKGBUILD | `bin/install` puts an empty, themed window in `Super+Space` |
| **1. Local RSS reader** | Store + migrations, fetcher, parser, discovery, three panes, read/star, OPML import/export, CLI `feeds/articles/refresh/subscribe/import/export` | Parity with omafeed's basic flow. Parser tests ported from readstr fixtures |
| **2. Nostr reading** | Relay pool, NIP-19, kind 0/30023 fetch, NIP-94 video, mpv playback, Markdown rendering | Can subscribe to an npub and read long-form |
| **3. Read-only sync** | npub login, pull 30404/30405/30406 and merge | Same subscriptions and read state as the web app for the same npub |
| **4. Publishing sync** | Signer abstraction, NIP-46 bunker, Secret Service nsec, publish on change | Change on desktop → visible on web, and the other way round |
| **5. Polish** | Search (FTS5), saved views and keyword filters, categories/tags management, undo, remote-image toggle, translations, offscreen screenshot for README | Language switch and theme switch look right. `?` lists everything |
| **6. Ship** | `readstr-bar` plugin, `desktop-v0.1.0` tag + GitHub release, PR to `omacom/omarchy-pkgs` | `omarchy pkg add readstr` works |
| **7. Readstr account (optional, paid)** | `RemoteStore`, NIP-98 client, Flash checkout hand-off, `getStatus` check, server-only extras | A paid npub unlocks the extras on desktop. An unpaid or offline user loses nothing from Phases 1–6 |

Testing at every phase: `bin/test` (Qt Test, offscreen, temp `READSTR_HOME`), with
fixtures for feeds, signed events and OPML. Use a local relay (e.g. `nak serve`)
for sync integration tests.

---

## 8. Decisions

1. **Repo location: `desktop/` in this monorepo**, alongside `chrome-extension/`. It
   shares fixtures and the sync docs with the web app. Releases are tagged `desktop-v<version>`,
   and the `omarchy-pkgs` PKGBUILD builds from that tag's tarball with `cd desktop`.
   CI builds `desktop/` only when files under it change.
2. **Name: `readstr`** for the binary, package, `.desktop` file, icon and Wayland app id.
   The web app is never packaged as an Arch package, so nothing clashes.
3. **The desktop app is free.** Reading, local storage, search, OPML, Nostr sync, the
   bar widget and the CLI never require payment. Paid value comes later from an
   optional **Readstr account** (Phase 7) that unlocks server-only extras through the
   existing Flash subscription. There is no server-account mode in v1 (see 8.1).
4. **Rendering: Qt rich text by default. QtWebEngine is optional and never required**
   (see 8.2).
5. *Still open:* **Sync event versioning.** Should we bump the `d`-tag schema if the
   desktop needs fields the web doesn't publish (e.g. folders)?

### 8.1 Server-account mode

**What it would be:** a second backend that signs in to `readstr.privkey.io` with
NIP-98 and uses the server's Postgres copy (feeds the server already fetched, read state,
categories, preferences) instead of the local SQLite.

**What it gets us:**
- Feeds keep refreshing while the laptop is off, and the history is complete on first launch.
- Server-only features: Guide directory, admin, AI summaries.
- A way to respect the paid plan (Flash, 1750 sats/month): the desktop could check
  `subscription.getStatus`.

**What it costs:**
- A NIP-98 signature on every request, so a signer is required. That means no read-only
  npub and no anonymous use.
- tRPC is an internal API, so every web refactor could break the desktop.
- The app is offline-useless in this mode.
- Two backends to test.

**The real question is the paid plan.** A local-first desktop app fetches feeds itself
and syncs over public relays, so it's effectively a free Readstr. The server can't gate it,
because sync events are cleartext relay events. Sharing the paid server's features is a
pricing choice, not an engineering one.

**Decision (accepted):** ship v1 local-first and free, like omafeed. Treat it as the on-ramp
to Readstr's Nostr sync and the web app. If paid value is needed later, add an
**optional "Readstr account" sign-in** for server-only extras (always-on refresh and full
history, Guide, AI summaries), and gate those behind `subscription.getStatus`. Don't gate
reading. To keep that door open, `Backend` talks to an abstract `Store`, so a
`RemoteStore` can be added without touching QML.

**How the optional account uses Flash.** The desktop app never handles payments.
It reuses the web app's existing flow, so the server needs no new payment code:

1. Settings → **Upgrade to Readstr account** opens the Flash checkout in the browser
   (`xdg-open`), with the user's npub filled in, the same way `subscription.createCheckoutSession` does on the web.
2. The user pays over Lightning. Flash calls `/api/webhooks/flash`, and the server marks the npub ACTIVE.
3. The app calls `subscription.getStatus` with a NIP-98-signed request and unlocks the
   extras: server-side refresh and full history, Guide directory, AI summaries.
   This needs a signer (NIP-46 or a keyring nsec), because NIP-98 proves npub ownership.
4. The status is cached locally with its expiry and re-checked on launch. An expired
   or offline status only hides the extras. Reading keeps working.

### 8.2 Article rendering: Qt rich text vs QtWebEngine

**Qt rich text** (`QTextDocument`) supports a subset of HTML 4 and CSS 2: paragraphs,
headings, lists, links, emphasis, blockquotes, `pre`/`code`, basic tables and images.

- **What it can't do:** flexbox and grid, most modern CSS, `figure`/`figcaption` styling,
  inline SVG, iframes, `srcset`/`<picture>`, and lazy-loaded `data-src` images.
- **What we'd have to handle:** the sanitizer rewrites the common cases (pick a `srcset`
  candidate, promote `data-src`, scale images to the pane width).
- **Cost:** zero extra size, no JS engine, and the theme colors and fonts apply directly.

**QtWebEngine** is Chromium inside Qt, and it renders faithfully, the way omafeed's
WebKitGTK does.

- **Size and memory:** ~100+ MB installed and 150–300 MB of RAM for the renderer process.
- **Security:** Chromium's patch cadence, so a larger attack surface even with JS off.
- **Theming:** we'd have to inject CSS to theme it.

**Recommendation:** default to rich text, because most feed content is simple prose that
reads well in it.

- Build a **rendering fixture set of ~30 real feeds** (Substack, WordPress, Ghost,
  Medium, NIP-23 Markdown, GitHub releases) and screenshot them in the offscreen harness
  to see where it falls short.
- `O` (open original in browser) is always the escape hatch.
- If the fixtures show real gaps, add WebEngine as an **optional dependency**
  (`optdepends=('qt6-webengine: faithful article rendering')`). The QML `Loader` tries
  `ReaderWebView.qml` and falls back to the rich-text view when the module isn't installed.
  A settings toggle, "Full web rendering (uses more memory)", turns it on.
- Nobody pays for Chromium unless they choose to, and the default install stays small.

## References

- omafeed: <https://github.com/pch/omafeed> · omafeed-bar: <https://github.com/pch/omafeed-bar>
- Omarchy apps: Hype <https://github.com/omacom/hype>, Omacut <https://github.com/omacom-io/omacut>,
  Monologue <https://github.com/omacom/monologue>, Signed <https://github.com/omacom/signed>
- Packaging: <https://github.com/omacom/omarchy-pkgs>
- Readstr: `docs/SUBSCRIPTION_SYNC.md`, `docs/CLI_DEVELOPMENT_GUIDE.md`, `src/lib/nostr-sync.ts`
