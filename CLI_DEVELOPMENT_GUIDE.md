# Nostr-Feedz CLI Development Guide

## Project Overview

Build a **command-line interface (CLI) client** for Nostr-Feedz using **Go** and **Charm's Bubble Tea framework** for TUI components. The CLI should work both as a **standalone RSS/Nostr feed reader** and optionally sync with the **Nostr-Feedz web app** via Nostr protocol and REST APIs.

### Target Features
- **RSS & Nostr Feed Management**: Subscribe, list, read feeds
- **TUI Interface**: Beautiful terminal UI using Bubble Tea, Lip Gloss, and Bubbles
- **Nostr Sync**: Cross-device subscription sync via Nostr events (kind 30404)
- **Offline Support**: Local SQLite database for feeds and read status
- **Categories & Tags**: Organize feeds like the web version
- **Mark as Read**: Track read status locally and optionally sync
- **Favorites**: Star articles for later
- **Search & Filter**: Find articles across feeds

---

## Tech Stack

### Core
- **Language**: Go 1.21+
- **TUI Framework**: [Bubble Tea](https://github.com/charmbracelet/bubbletea)
- **Styling**: [Lip Gloss](https://github.com/charmbracelet/lipgloss)
- **Components**: [Bubbles](https://github.com/charmbracelet/bubbles)
- **Database**: SQLite with [go-sqlite3](https://github.com/mattn/go-sqlite3)
- **Nostr**: [go-nostr](https://github.com/nbd-wtf/go-nostr)
- **RSS Parsing**: [gofeed](https://github.com/mmcdole/gofeed)
- **Markdown Rendering**: [glamour](https://github.com/charmbracelet/glamour)

### Optional Dependencies
- **HTTP Client**: Standard library `net/http`
- **JSON**: Standard library `encoding/json`
- **Config**: [viper](https://github.com/spf13/viper) for config management
- **Logging**: [zerolog](https://github.com/rs/zerolog)

---

## Architecture

```
nostrfeedz-cli/
├── cmd/
│   └── nostrfeedz/
│       └── main.go              # Entry point
├── internal/
│   ├── app/
│   │   └── app.go               # Main Bubble Tea model
│   ├── ui/
│   │   ├── feeds.go             # Feed list view
│   │   ├── articles.go          # Article list view
│   │   ├── reader.go            # Article reader view
│   │   ├── categories.go        # Category picker
│   │   └── help.go              # Help/key bindings
│   ├── db/
│   │   ├── sqlite.go            # SQLite operations
│   │   └── models.go            # Data models
│   ├── nostr/
│   │   ├── client.go            # Nostr client wrapper
│   │   ├── sync.go              # Subscription sync (kind 30404)
│   │   └── signer.go            # Event signing
│   ├── feed/
│   │   ├── fetcher.go           # RSS/Nostr feed fetcher
│   │   └── parser.go            # Content parser
│   ├── api/
│   │   └── client.go            # Nostr-Feedz API client
│   └── config/
│       └── config.go            # Configuration management
├── pkg/
│   └── styles/
│       └── theme.go             # Lip Gloss styles
├── go.mod
├── go.sum
└── README.md
```

---

## Database Schema (SQLite)

```sql
-- Feeds
CREATE TABLE feeds (
    id TEXT PRIMARY KEY,
    type TEXT NOT NULL,           -- 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'
    url TEXT,                     -- RSS URL or empty for Nostr
    npub TEXT,                    -- Nostr pubkey (npub format)
    title TEXT NOT NULL,
    description TEXT,
    last_fetched_at INTEGER,      -- Unix timestamp
    category_id TEXT,             -- FK to categories.id
    created_at INTEGER NOT NULL,
    UNIQUE(type, url),
    UNIQUE(type, npub)
);

-- Feed Items
CREATE TABLE feed_items (
    id TEXT PRIMARY KEY,
    feed_id TEXT NOT NULL,
    guid TEXT NOT NULL,           -- RSS guid or Nostr event ID
    title TEXT NOT NULL,
    content TEXT,
    url TEXT,
    author TEXT,
    published_at INTEGER NOT NULL,
    is_read INTEGER DEFAULT 0,    -- Boolean: 0=unread, 1=read
    is_favorite INTEGER DEFAULT 0,
    thumbnail TEXT,               -- Video thumbnail URL
    video_id TEXT,                -- YouTube/Rumble video ID
    created_at INTEGER NOT NULL,
    FOREIGN KEY(feed_id) REFERENCES feeds(id) ON DELETE CASCADE,
    UNIQUE(feed_id, guid)
);

CREATE INDEX idx_feed_items_feed_id ON feed_items(feed_id);
CREATE INDEX idx_feed_items_published_at ON feed_items(published_at DESC);
CREATE INDEX idx_feed_items_is_read ON feed_items(is_read);

-- Tags (many-to-many with feeds)
CREATE TABLE tags (
    id TEXT PRIMARY KEY,
    name TEXT UNIQUE NOT NULL
);

CREATE TABLE feed_tags (
    feed_id TEXT NOT NULL,
    tag_id TEXT NOT NULL,
    PRIMARY KEY(feed_id, tag_id),
    FOREIGN KEY(feed_id) REFERENCES feeds(id) ON DELETE CASCADE,
    FOREIGN KEY(tag_id) REFERENCES tags(id) ON DELETE CASCADE
);

-- Categories
CREATE TABLE categories (
    id TEXT PRIMARY KEY,
    name TEXT UNIQUE NOT NULL,
    color TEXT,                   -- Hex color code
    icon TEXT,                    -- Emoji icon
    sort_order INTEGER DEFAULT 0
);

-- User Preferences
CREATE TABLE preferences (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

-- Common preferences:
-- 'user_npub' - User's Nostr public key
-- 'user_nsec' - User's Nostr private key (encrypted)
-- 'organization_mode' - 'tags' | 'categories'
-- 'mark_read_behavior' - 'on-open' | 'after-10s' | 'never'
-- 'sync_enabled' - '1' | '0'
-- 'nostr_relays' - JSON array of relay URLs
```

---

## Nostr Integration

Nostr-Feedz uses two types of Nostr events for cross-device synchronization:
- **Kind 30404** - Subscription list sync (which feeds you're subscribed to)
- **Kind 30405** - Read status sync (which articles you've read)

### Subscription Sync (Kind 30404)

Use Nostr **replaceable events** (kind 30404) to sync subscriptions across devices.

**Event Structure:**
```json
{
  "kind": 30404,
  "pubkey": "<user's hex pubkey>",
  "created_at": 1732645747,
  "tags": [
    ["d", "nostr-feedz-subscriptions"],
    ["client", "nostrfeedz-cli"]
  ],
  "content": "{\"rss\":[...],\"nostr\":[...],\"tags\":{...},\"deleted\":[...],\"lastUpdated\":1732645747}"
}
```

**Content Schema:**
```go
type SubscriptionList struct {
    RSS         []string            `json:"rss"`         // RSS feed URLs
    Nostr       []string            `json:"nostr"`       // Nostr npubs
    Tags        map[string][]string `json:"tags"`        // URL/npub -> tags
    Deleted     []string            `json:"deleted"`     // Removed feeds
    LastUpdated int64               `json:"lastUpdated"` // Unix timestamp
}
```

**Implementation:**

```go
package nostr

import (
    "context"
    "encoding/json"
    "github.com/nbd-wtf/go-nostr"
)

const (
    SubscriptionListKind = 30404
    ReadStatusKind       = 30405
    SubscriptionDTag     = "nostr-feedz-subscriptions"
    ReadStatusDTag       = "nostr-feedz-read-status"
)

type SyncClient struct {
    pool   *nostr.SimplePool
    relays []string
    signer nostr.EventSigner
}

func (c *SyncClient) PublishSubscriptions(list SubscriptionList) error {
    content, _ := json.Marshal(list)
    
    event := nostr.Event{
        Kind:      SubscriptionListKind,
        CreatedAt: nostr.Now(),
        Tags: nostr.Tags{
            {"d", SubscriptionDTag},
            {"client", "nostrfeedz-cli"},
        },
        Content: string(content),
    }
    
    event.Sign(c.signer)
    
    ctx := context.Background()
    for _, relay := range c.relays {
        c.pool.Publish(ctx, relay, event)
    }
    
    return nil
}

func (c *SyncClient) FetchSubscriptions(pubkey string) (*SubscriptionList, error) {
    ctx := context.Background()
    filter := nostr.Filter{
        Kinds:   []int{SubscriptionListKind},
        Authors: []string{pubkey},
        Tags:    nostr.TagMap{"d": []string{SubscriptionDTag}},
        Limit:   1,
    }
    
    events := c.pool.QuerySync(ctx, c.relays, filter)
    if len(events) == 0 {
        return nil, nil
    }
    
    var list SubscriptionList
    json.Unmarshal([]byte(events[0].Content), &list)
    return &list, nil
}
```

**Default Relays:**
```go
var DefaultRelays = []string{
    "wss://relay.damus.io",
    "wss://nos.lol",
    "wss://relay.snort.social",
    "wss://relay.nostr.band",
    "wss://nostr-pub.wellorder.net",
}
```

### Read Status Sync (Kind 30405)

Use Nostr **replaceable events** (kind 30405) to sync which articles have been read across devices.

**Event Structure:**
```json
{
  "kind": 30405,
  "pubkey": "<user's hex pubkey>",
  "created_at": 1732645747,
  "tags": [
    ["d", "nostr-feedz-read-status"],
    ["client", "nostrfeedz-cli"]
  ],
  "content": "{\"itemGuids\":[\"guid1\",\"guid2\",\"guid3\",...],\"lastUpdated\":1732645747}"
}
```

**Content Schema:**
```go
type ReadStatusList struct {
    ItemGuids   []string `json:"itemGuids"`   // GUIDs of read feed items
    LastUpdated int64    `json:"lastUpdated"` // Unix timestamp
}
```

**Implementation:**

```go
func (c *SyncClient) PublishReadStatus(readStatus ReadStatusList) error {
    content, _ := json.Marshal(readStatus)
    
    event := nostr.Event{
        Kind:      ReadStatusKind,
        CreatedAt: nostr.Now(),
        Tags: nostr.Tags{
            {"d", ReadStatusDTag},
            {"client", "nostrfeedz-cli"},
        },
        Content: string(content),
    }
    
    event.Sign(c.signer)
    
    ctx := context.Background()
    for _, relay := range c.relays {
        c.pool.Publish(ctx, relay, event)
    }
    
    return nil
}

func (c *SyncClient) FetchReadStatus(pubkey string) (*ReadStatusList, error) {
    ctx := context.Background()
    filter := nostr.Filter{
        Kinds:   []int{ReadStatusKind},
        Authors: []string{pubkey},
        Tags:    nostr.TagMap{"d": []string{ReadStatusDTag}},
        Limit:   1,
    }
    
    events := c.pool.QuerySync(ctx, c.relays, filter)
    if len(events) == 0 {
        return nil, nil
    }
    
    var status ReadStatusList
    json.Unmarshal([]byte(events[0].Content), &status)
    return &status, nil
}
```

**Sync Strategy:**

The read status list can grow large over time. Consider these strategies:

1. **Incremental Sync**: Only sync GUIDs from the last 90 days
2. **Batch Updates**: Accumulate local changes and sync every N items or M minutes
3. **Merge Logic**: When importing, mark items as read if they exist in remote list

```go
func (c *SyncClient) MergeReadStatus(local, remote ReadStatusList) []string {
    // Create a set from remote GUIDs
    remoteSet := make(map[string]bool)
    for _, guid := range remote.ItemGuids {
        remoteSet[guid] = true
    }
    
    // Add local GUIDs
    for _, guid := range local.ItemGuids {
        remoteSet[guid] = true
    }
    
    // Convert back to slice
    merged := []string{}
    for guid := range remoteSet {
        merged = append(merged, guid)
    }
    
    return merged
}
```

### Nostr Feed Fetching (NIP-23 Long-form Content)

Fetch long-form articles from Nostr users:

```go
func (c *NostrFetcher) FetchUserArticles(npub string, since time.Time) ([]*FeedItem, error) {
    pubkey, _ := nostr.GetPublicKey(npub) // Convert npub to hex
    
    filter := nostr.Filter{
        Kinds:   []int{30023}, // NIP-23 long-form
        Authors: []string{pubkey},
        Since:   nostr.Timestamp(since.Unix()),
        Limit:   50,
    }
    
    events := c.pool.QuerySync(context.Background(), c.relays, filter)
    
    items := []*FeedItem{}
    for _, event := range events {
        item := &FeedItem{
            GUID:        event.ID,
            Title:       event.Tags.GetFirst([]string{"title", ""}).Value(),
            Content:     event.Content,
            Author:      npub,
            PublishedAt: time.Unix(int64(event.CreatedAt), 0),
            URL:         buildNostrURL(event), // Link to habla.news or njump.me
        }
        items = append(items, item)
    }
    
    return items, nil
}
```

---

## API Integration

### Guide API (Public Feed Directory)

**Base URL:** `https://nostrfeedz.com/api/guide`

#### List Feeds
```go
type GuideFeed struct {
    ID          string   `json:"id"`
    Type        string   `json:"type"` // "RSS" | "NOSTR" | "NOSTR_VIDEO"
    NPUB        string   `json:"npub,omitempty"`
    URL         string   `json:"url,omitempty"`
    Title       string   `json:"title"`
    Description string   `json:"description"`
    Category    string   `json:"category"`
    Tags        []string `json:"tags"`
    ImageURL    string   `json:"imageUrl"`
    Featured    bool     `json:"featured"`
}

func (c *APIClient) GetGuideFeeds(category, tag string, limit int) ([]GuideFeed, error) {
    url := fmt.Sprintf("%s/api/guide?category=%s&tag=%s&limit=%d", 
        c.baseURL, category, tag, limit)
    
    resp, _ := http.Get(url)
    defer resp.Body.Close()
    
    var result struct {
        Feeds []GuideFeed `json:"feeds"`
    }
    json.NewDecoder(resp.Body).Decode(&result)
    
    return result.Feeds, nil
}
```

#### Get Feed Details
```go
func (c *APIClient) GetFeedDetails(id string, includePosts bool) (*GuideFeed, error) {
    url := fmt.Sprintf("%s/api/guide/%s?includePosts=%v", c.baseURL, id, includePosts)
    
    resp, _ := http.Get(url)
    defer resp.Body.Close()
    
    var result struct {
        Feed GuideFeed `json:"feed"`
    }
    json.NewDecoder(resp.Body).Decode(&result)
    
    return &result.Feed, nil
}
```

---

## UI Components (Bubble Tea)

### Main App Model

```go
package app

import (
    tea "github.com/charmbracelet/bubbletea"
    "github.com/charmbracelet/bubbles/list"
    "github.com/charmbracelet/bubbles/viewport"
)

type View int

const (
    FeedsView View = iota
    ArticlesView
    ReaderView
    CategoriesView
)

type Model struct {
    currentView View
    
    // Components
    feedList     list.Model
    articleList  list.Model
    readerView   viewport.Model
    
    // Data
    feeds        []Feed
    articles     []Article
    currentFeed  *Feed
    currentArticle *Article
    
    // State
    width, height int
    err           error
}

func (m Model) Init() tea.Cmd {
    return tea.Batch(
        loadFeedsCmd(),
        tea.EnterAltScreen,
    )
}

func (m Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
    switch msg := msg.(type) {
    case tea.KeyMsg:
        switch msg.String() {
        case "q", "ctrl+c":
            return m, tea.Quit
        case "1":
            m.currentView = FeedsView
        case "2":
            m.currentView = ArticlesView
        case "3":
            m.currentView = ReaderView
        case "enter":
            return m.handleEnter()
        }
    
    case tea.WindowSizeMsg:
        m.width = msg.Width
        m.height = msg.Height
        m.readerView.Width = msg.Width - 4
        m.readerView.Height = msg.Height - 10
    }
    
    return m, nil
}

func (m Model) View() string {
    switch m.currentView {
    case FeedsView:
        return renderFeedsView(m)
    case ArticlesView:
        return renderArticlesView(m)
    case ReaderView:
        return renderReaderView(m)
    default:
        return "Unknown view"
    }
}
```

### Three-Panel Layout (like Google Reader)

```
┌─────────────────────────────────────────────────────────────────────┐
│ Nostr-Feedz CLI                                          [Help: ?]  │
├───────────────┬───────────────────────┬─────────────────────────────┤
│   Feeds       │   Articles            │   Reader                    │
│               │                       │                             │
│ 📰 All Items  │ ▸ Article Title 1     │ # Article Title             │
│               │   Published: 2h ago   │                             │
│ 📁 Bitcoin    │                       │ Lorem ipsum dolor sit       │
│   ├─ Blog 1   │ ▸ Article Title 2     │ amet, consectetur...        │
│   └─ Blog 2   │   Published: 5h ago   │                             │
│               │                       │ - Bullet point 1            │
│ 📁 Tech       │ ▸ Article Title 3     │ - Bullet point 2            │
│   ├─ HN       │   Published: 1d ago   │                             │
│   └─ Blog 3   │                       │ More content here...        │
│               │   [10 unread]         │                             │
│ ⭐ Favorites  │                       │                             │
│               │                       │                             │
│               │                       │ [Space: Scroll Down]        │
│ [2 feeds]     │ [Page 1/3]            │ [j/k: Navigate]             │
└───────────────┴───────────────────────┴─────────────────────────────┘
│ q: Quit  1: Feeds  2: Articles  3: Reader  a: Add Feed  r: Refresh │
└─────────────────────────────────────────────────────────────────────┘
```

### Styles (Lip Gloss)

```go
package styles

import "github.com/charmbracelet/lipgloss"

var (
    // Colors
    PrimaryColor   = lipgloss.Color("#7C3AED")
    SecondaryColor = lipgloss.Color("#6B7280")
    AccentColor    = lipgloss.Color("#3B82F6")
    ErrorColor     = lipgloss.Color("#EF4444")
    
    // Component styles
    TitleStyle = lipgloss.NewStyle().
        Bold(true).
        Foreground(PrimaryColor).
        PaddingLeft(2)
    
    FeedItemStyle = lipgloss.NewStyle().
        PaddingLeft(2).
        Foreground(lipgloss.Color("#1F2937"))
    
    SelectedStyle = lipgloss.NewStyle().
        Bold(true).
        Foreground(AccentColor).
        Background(lipgloss.Color("#EFF6FF")).
        PaddingLeft(2)
    
    UnreadBadge = lipgloss.NewStyle().
        Background(AccentColor).
        Foreground(lipgloss.Color("#FFFFFF")).
        Padding(0, 1).
        Bold(true)
        
    PanelBorder = lipgloss.NewStyle().
        Border(lipgloss.RoundedBorder()).
        BorderForeground(lipgloss.Color("#E5E7EB"))
)
```

---

## Key Features to Implement

### Phase 1: Core Functionality
- ✅ SQLite database setup
- ✅ RSS feed fetching and parsing
- ✅ Nostr feed fetching (NIP-23)
- ✅ Three-panel TUI layout
- ✅ Navigate between feeds, articles, and reader
- ✅ Mark articles as read
- ✅ Basic keyboard shortcuts

### Phase 2: Organization
- ✅ Tag support (assign multiple tags to feeds)
- ✅ Category support (folders with icons)
- ✅ Filter by tags/categories
- ✅ Favorites system

### Phase 3: Sync
- ✅ Nostr subscription sync (kind 30404)
- ✅ Export subscriptions to Nostr
- ✅ Import subscriptions from Nostr
- ✅ Merge local and remote subscriptions
- ✅ Conflict resolution

### Phase 4: Polish
- ✅ Search across articles
- ✅ Video feed support (YouTube, Rumble)
- ✅ Markdown rendering with syntax highlighting
- ✅ Configuration file (~/.config/nostrfeedz/config.yaml)
- ✅ Color themes
- ✅ Guide directory integration

---

## Configuration File

**Location:** `~/.config/nostrfeedz/config.yaml`

```yaml
# User Identity
nostr:
  npub: "npub1..."              # Your Nostr public key
  nsec: "nsec1..."              # Your Nostr private key (optional, for signing)
  relays:
    - "wss://relay.damus.io"
    - "wss://nos.lol"
    - "wss://relay.snort.social"

# Sync Settings
sync:
  enabled: true
  auto_sync_interval: 15m       # Auto-sync every 15 minutes

# Reading Preferences
reading:
  mark_read_behavior: "on-open" # "on-open" | "after-10s" | "never"
  organization_mode: "tags"     # "tags" | "categories"

# Display
display:
  theme: "default"              # "default" | "dark" | "light"
  feed_list_width: 30
  article_list_width: 40

# Database
database:
  path: "~/.local/share/nostrfeedz/feeds.db"
```

---

## Commands and Key Bindings

### Global
- `q` / `Ctrl+C` - Quit
- `?` - Show help
- `1` - Switch to Feeds view
- `2` - Switch to Articles view
- `3` - Switch to Reader view
- `Tab` - Cycle between panels
- `/` - Search

### Feed List
- `↑` / `k` - Previous feed
- `↓` / `j` - Next feed
- `Enter` - Open feed (show articles)
- `a` - Add new feed
- `d` - Delete feed
- `e` - Edit feed (tags/category)
- `r` - Refresh feed
- `R` - Refresh all feeds
- `s` - Sync with Nostr
- `t` - Filter by tags
- `c` - Filter by category

### Article List
- `↑` / `k` - Previous article
- `↓` / `j` - Next article
- `Enter` - Open article
- `m` - Mark as read/unread
- `M` - Mark all as read
- `f` - Toggle favorite
- `o` - Open in browser
- `Space` - Preview article

### Reader
- `↑` / `k` - Scroll up
- `↓` / `j` - Scroll down
- `g` - Go to top
- `G` - Go to bottom
- `Space` - Page down
- `b` - Page up
- `o` - Open in browser
- `f` - Toggle favorite
- `n` - Next article
- `p` - Previous article

---

## CLI Commands

```bash
# Basic usage
nostrfeedz                      # Launch TUI

# Feed management
nostrfeedz add <url>            # Add RSS feed
nostrfeedz add -n <npub>        # Add Nostr feed
nostrfeedz list                 # List all feeds
nostrfeedz remove <id>          # Remove feed
nostrfeedz refresh              # Refresh all feeds
nostrfeedz refresh <id>         # Refresh specific feed

# Sync
nostrfeedz sync export          # Export subscriptions to Nostr
nostrfeedz sync import          # Import subscriptions from Nostr
nostrfeedz sync status          # Show sync status

# Articles
nostrfeedz articles             # List recent articles
nostrfeedz read <id>            # Read article in terminal
nostrfeedz mark-read <id>       # Mark article as read
nostrfeedz favorite <id>        # Add to favorites
nostrfeedz search <query>       # Search articles

# Organization
nostrfeedz tags                 # List all tags
nostrfeedz categories           # List all categories
nostrfeedz tag <feed-id> <tag>  # Add tag to feed

# Guide
nostrfeedz guide list           # Browse guide directory
nostrfeedz guide search <query> # Search guide
nostrfeedz guide add <id>       # Subscribe to guide feed

# Config
nostrfeedz config init          # Create config file
nostrfeedz config show          # Show current config
nostrfeedz config set <key> <value>
```

---

## Testing Checklist

### RSS Feeds
- [ ] Add RSS feed by URL
- [ ] Fetch and parse articles
- [ ] Mark articles as read
- [ ] Refresh feeds
- [ ] Handle feed errors gracefully

### Nostr Feeds
- [ ] Add Nostr user by npub
- [ ] Fetch NIP-23 long-form articles
- [ ] Display author profiles
- [ ] Handle relay errors

### Sync
- [ ] Export subscriptions to Nostr
- [ ] Import subscriptions from Nostr
- [ ] Merge local and remote lists
- [ ] Handle conflicts (deleted feeds)
- [ ] Auto-sync on interval

### UI
- [ ] Three-panel layout renders correctly
- [ ] Keyboard navigation works
- [ ] Resize handling
- [ ] Color themes apply
- [ ] Help screen displays

### Organization
- [ ] Assign tags to feeds
- [ ] Create categories
- [ ] Filter by tags
- [ ] Filter by categories
- [ ] View unread counts per tag/category

---

## Resources

### Libraries
- **Bubble Tea**: https://github.com/charmbracelet/bubbletea
- **Lip Gloss**: https://github.com/charmbracelet/lipgloss
- **Bubbles**: https://github.com/charmbracelet/bubbles
- **Glamour**: https://github.com/charmbracelet/glamour
- **go-nostr**: https://github.com/nbd-wtf/go-nostr
- **gofeed**: https://github.com/mmcdole/gofeed

### Nostr Specifications
- **NIP-01** (Events): https://github.com/nostr-protocol/nips/blob/master/01.md
- **NIP-07** (Browser Extension): https://github.com/nostr-protocol/nips/blob/master/07.md
- **NIP-23** (Long-form Content): https://github.com/nostr-protocol/nips/blob/master/23.md
- **NIP-33** (Replaceable Events): https://github.com/nostr-protocol/nips/blob/master/33.md

### API Documentation
- **Guide API**: https://nostrfeedz.com/api/guide/docs
- **Subscription Sync**: See `SUBSCRIPTION_SYNC.md` in web repo

---

## Example: Complete Feed Fetcher

```go
package feed

import (
    "time"
    "github.com/mmcdole/gofeed"
)

type Fetcher struct {
    rssParser   *gofeed.Parser
    nostrClient *nostr.Client
}

func (f *Fetcher) FetchFeed(feed *Feed) ([]*FeedItem, error) {
    switch feed.Type {
    case "RSS":
        return f.fetchRSS(feed)
    case "NOSTR":
        return f.fetchNostr(feed)
    default:
        return nil, fmt.Errorf("unknown feed type: %s", feed.Type)
    }
}

func (f *Fetcher) fetchRSS(feed *Feed) ([]*FeedItem, error) {
    parsed, err := f.rssParser.ParseURL(feed.URL)
    if err != nil {
        return nil, err
    }
    
    items := []*FeedItem{}
    for _, item := range parsed.Items {
        feedItem := &FeedItem{
            FeedID:      feed.ID,
            GUID:        item.GUID,
            Title:       item.Title,
            Content:     getContent(item),
            URL:         item.Link,
            Author:      getAuthor(item),
            PublishedAt: *item.PublishedParsed,
        }
        items = append(items, feedItem)
    }
    
    return items, nil
}

func (f *Fetcher) fetchNostr(feed *Feed) ([]*FeedItem, error) {
    since := time.Now().AddDate(0, 0, -30) // Last 30 days
    return f.nostrClient.FetchUserArticles(feed.NPUB, since)
}
```

---

## Next Steps

1. **Initialize Go Module**
   ```bash
   go mod init github.com/yourusername/nostrfeedz-cli
   go get github.com/charmbracelet/bubbletea
   go get github.com/charmbracelet/lipgloss
   go get github.com/charmbracelet/bubbles
   go get github.com/nbd-wtf/go-nostr
   go get github.com/mmcdole/gofeed
   go get github.com/mattn/go-sqlite3
   ```

2. **Set up SQLite Database**
   - Create database schema
   - Write CRUD operations
   - Add migrations support

3. **Build Basic TUI**
   - Implement main Bubble Tea model
   - Create three-panel layout
   - Add keyboard navigation

4. **Implement Feed Fetching**
   - RSS parser
   - Nostr client
   - Background refresh

5. **Add Sync**
   - Nostr event signing
   - Subscription sync
   - Merge logic

6. **Polish**
   - Add color themes
   - Implement search
   - Write documentation

---

## Contact

For questions or collaboration:
- **Web App**: https://nostrfeedz.com
- **GitHub**: https://github.com/privkeyio/Nostr-Feedz
- **Nostr**: npub13hyx3qsqk3r7ctjqrr49uskut4yqjsxt8uvu4rekr55p08wyhf0qq90nt7

Happy coding! 🚀
