# Readstr Guide API Implementation

## Overview

Create a REST API that exposes the Readstr guide (curated list of RSS and Nostr long-form content feeds) to native mobile apps and external integrations. The API should enable:

1. **Feed Discovery** - List all curated feeds with filtering/search
2. **Feed Details** - Get individual feed info with recent posts
3. **One-Click Subscribe** - Deep-link web pages for native app → web subscription flow

## Tech Stack

- **Framework**: Next.js App Router (API Routes)
- **Database**: PostgreSQL via Prisma ORM
- **Models**: `GuideFeed` (curated feeds), `GuideFeedPost` (cached posts)

## Database Schema Reference

```prisma
model GuideFeed {
  id          String   @id @default(cuid())
  type        FeedType // RSS | NOSTR | NOSTR_VIDEO
  url         String?  // RSS feed URL
  npub        String?  // Nostr pubkey for NOSTR types
  title       String
  description String?
  category    String?  // e.g., "Bitcoin", "Technology", "News"
  tags        String[] // ["bitcoin", "lightning", "privacy"]
  imageUrl    String?  // Feed avatar/logo
  isActive    Boolean  @default(true)
  featured    Boolean  @default(false)
  posts       GuideFeedPost[]
}

model GuideFeedPost {
  id          String    @id @default(cuid())
  feedId      String
  feed        GuideFeed @relation(fields: [feedId], references: [id])
  title       String
  content     String?
  url         String?
  author      String?
  publishedAt DateTime
}
```

## API Endpoints

### 1. `GET /api/guide` - List All Feeds

**Query Parameters:**

| Parameter  | Type    | Description                                    |
|------------|---------|------------------------------------------------|
| `category` | string  | Filter by category                             |
| `tag`      | string  | Filter by tag                                  |
| `type`     | string  | Filter by feed type: `RSS`, `NOSTR`, `NOSTR_VIDEO` |
| `featured` | boolean | Only featured feeds                            |
| `search`   | string  | Search title/description                       |
| `limit`    | number  | Results per page (default: 50, max: 100)       |
| `offset`   | number  | Pagination offset                              |

**Response:**

```json
{
  "feeds": [
    {
      "id": "clxx...",
      "type": "NOSTR",
      "npub": "npub1abc...",
      "title": "Author Name",
      "description": "Long-form Bitcoin content",
      "category": "Bitcoin",
      "tags": ["bitcoin", "lightning"],
      "imageUrl": "https://...",
      "featured": true,
      "subscribeUrl": "https://readstr.privkey.io/subscribe?npub=npub1abc..."
    }
  ],
  "total": 42,
  "limit": 50,
  "offset": 0,
  "categories": ["Bitcoin", "Technology", "News"],
  "tags": ["bitcoin", "nostr", "privacy", "lightning"]
}
```

### 2. `GET /api/guide/[id]` - Single Feed Details

**Path Parameter:** Feed ID or npub

**Query Parameters:**

| Parameter      | Type    | Description                    |
|----------------|---------|--------------------------------|
| `includePosts` | boolean | Include recent posts           |
| `postLimit`    | number  | Number of posts (default: 10)  |

**Response:**

```json
{
  "feed": {
    "id": "clxx...",
    "type": "NOSTR",
    "npub": "npub1abc...",
    "title": "Author Name",
    "description": "...",
    "category": "Bitcoin",
    "tags": ["bitcoin"],
    "imageUrl": "https://...",
    "subscribeUrl": "https://readstr.privkey.io/subscribe?npub=npub1abc...",
    "posts": [
      {
        "id": "post123",
        "title": "Article Title",
        "content": "Preview text...",
        "url": "https://habla.news/a/naddr...",
        "author": "npub1abc...",
        "publishedAt": "2025-11-26T12:00:00Z"
      }
    ]
  }
}
```

### 3. `GET /subscribe` - Subscribe Redirect Page

This is a **web page** (not JSON API) for handling deep-link subscriptions from native apps.

**Query Parameters:**

| Parameter | Type   | Description                                                                                  |
|-----------|--------|----------------------------------------------------------------------------------------------|
| `npub`    | string | **Required.** The guide feed's Nostr pubkey to subscribe to. Missing → error "Missing npub parameter". |
| `tags`    | string | Optional comma-separated tags. Falls back to the guide feed's own tags when omitted.          |
| `return`  | string | Optional same-origin path to redirect to after subscribing. Must start with a single `/` (e.g. `/reader`). Off-origin URLs, protocol-relative `//host`, and custom schemes are rejected and fall back to `/reader`. |

**Behavior:**

1. If user is logged in → Add subscription and redirect to the `return` path
2. If user is not logged in → Show login prompt, then subscribe
3. After success → Redirect to the sanitized same-origin `return` path, defaulting to `/reader`

**Implementation Notes:**

- Use Next.js page component at `src/app/subscribe/page.tsx`
- Check auth state via `useNostrAuth()` context
- Call `api.feed.subscribeFeed.useMutation()` for subscription
- Authenticate via NIP-07 browser extension (`connect('nip07')`)

### 4. `GET /api/guide/docs` - API Documentation

Return OpenAPI-style documentation as JSON for developer reference.

## Implementation Details

### File Structure

```
src/app/api/guide/
├── route.ts              # GET /api/guide (list feeds)
├── [id]/
│   └── route.ts          # GET /api/guide/[id] (single feed)
└── docs/
    └── route.ts          # GET /api/guide/docs (API docs)

src/app/guide/
└── subscribe/
    └── page.tsx          # Subscribe page (web, not API)
```

### Response Headers

All API responses should include:

```typescript
const headers = {
  'Content-Type': 'application/json',
  'Cache-Control': 'public, s-maxage=300, stale-while-revalidate=600',
  'Access-Control-Allow-Origin': '*',  // Allow native apps
  'Access-Control-Allow-Methods': 'GET, OPTIONS',
}
```

### Error Responses

```json
{
  "error": "Feed not found",
  "code": "NOT_FOUND"
}
```

**Status codes:** 200 (success), 400 (bad request), 404 (not found), 500 (server error)

## Subscribe Page UX Flow

```
┌─────────────────────────────────────────────────┐
│  Native App                                      │
│  ┌─────────────────────────────────────────────┐│
│  │ Feed: Bitcoin Magazine                      ││
│  │ [Subscribe on Readstr]  ← Opens browser ││
│  └─────────────────────────────────────────────┘│
└─────────────────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────┐
│  Browser: readstr.privkey.io/subscribe?npub=... │
│  ┌─────────────────────────────────────────────┐│
│  │       Subscribe to Bitcoin Magazine          ││
│  │                                              ││
│  │  ┌────────────────────────────────────────┐ ││
│  │  │ 🔐 Sign in with Nostr Extension       │ ││
│  │  └────────────────────────────────────────┘ ││
│  └─────────────────────────────────────────────┘│
└─────────────────────────────────────────────────┘
                      │
                      ▼ (after login)
┌─────────────────────────────────────────────────┐
│  ✓ Subscribed! Redirecting to reader...         │
└─────────────────────────────────────────────────┘
```

## Native App Integration Examples

A native app simply opens the subscribe URL in the browser. There is **no app-scheme callback**: the `/subscribe` page only accepts `npub`, optional `tags`, and an optional same-origin `return` path. Custom schemes like `myapp://subscribed` are rejected by the page and fall back to `/reader`, so do not pass them.

### iOS Swift

```swift
func subscribeToFeed(npub: String) {
    let subscribeUrl = "https://readstr.privkey.io/subscribe?npub=\(npub)&return=/reader"
    UIApplication.shared.open(URL(string: subscribeUrl)!)
}
```

### Android Kotlin

```kotlin
fun subscribeToFeed(npub: String) {
    val intent = Intent(Intent.ACTION_VIEW, Uri.parse(
        "https://readstr.privkey.io/subscribe?npub=$npub&return=/reader"
    ))
    startActivity(intent)
}
```

### React Native

```javascript
import { Linking } from 'react-native';

const subscribeToFeed = (npub) => {
  const url = `https://readstr.privkey.io/subscribe?npub=${npub}&return=/reader`;
  Linking.openURL(url);
};
```

### Flutter

```dart
import 'package:url_launcher/url_launcher.dart';

void subscribeToFeed(String npub) async {
  final url = 'https://readstr.privkey.io/subscribe?npub=$npub&return=/reader';
  if (await canLaunch(url)) {
    await launch(url);
  }
}
```

## Testing

```bash
# List all feeds
curl "https://readstr.privkey.io/api/guide"

# Filter by category
curl "https://readstr.privkey.io/api/guide?category=Bitcoin&featured=true"

# Search feeds
curl "https://readstr.privkey.io/api/guide?search=bitcoin&limit=10"

# Get single feed with posts
curl "https://readstr.privkey.io/api/guide/npub1abc...?includePosts=true&postLimit=5"

# Get API documentation
curl "https://readstr.privkey.io/api/guide/docs"

# Test subscribe page in browser
open "https://readstr.privkey.io/subscribe?npub=npub1abc..."
```

## Summary

This API design enables native Nostr apps to:

- **Discover** curated RSS and Nostr long-form content feeds
- **Display** feed previews with recent posts
- **Subscribe** seamlessly via web handoff with Nostr authentication

The one-click subscribe flow leverages the user's existing Nostr identity (via NIP-07 browser extension) without requiring native apps to implement their own authentication.
