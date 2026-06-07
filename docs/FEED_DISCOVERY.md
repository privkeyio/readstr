# RSS Feed Discovery Feature

## Overview
Implemented intelligent RSS/Atom feed discovery that automatically finds feeds when users enter URLs that aren't direct feed URLs.

## How It Works

### 3-Step Discovery Process

1. **Direct Feed Check** (`checkIfFeed`)
   - Attempts to fetch the URL directly
   - Validates Content-Type header (application/rss+xml, application/atom+xml, etc.)
   - Returns the URL if it's already a valid feed

2. **HTML Parsing** (`findFeedInHTML`)
   - Fetches the page as HTML
   - Uses cheerio to parse `<link>` tags with `rel="alternate"`
   - Looks for `type="application/rss+xml"` or `type="application/atom+xml"`
   - Returns the first valid feed URL found

3. **Common Locations** (`tryCommonFeedLocations`)
   - Tests standard feed paths relative to the domain:
     - `/feed`
     - `/feed.xml`
     - `/rss`
     - `/rss.xml`
     - `/atom.xml`
     - `/index.xml`
   - Returns the first valid feed found

## User Experience

### What Users Can Enter
- Direct feed URLs: `https://example.com/feed.xml` ✅
- Homepage URLs: `https://example.com` ✅ (will find `/feed` automatically)
- Blog URLs: `https://example.com/blog` ✅ (will check HTML for feed links)

### Error Messages
Provides descriptive errors when feeds can't be found:
- "Could not find RSS/Atom feed at this URL. Please check the URL and try again."
- Displays in red error box in the Add Feed modal
- Clears when user modifies the URL input

## Implementation Files

### `/src/lib/feed-discovery.ts`
- Core discovery logic
- Three helper functions + main `discoverFeed()` orchestrator
- Uses cheerio for HTML parsing
- Implements 10-second timeout for network requests

### `/src/server/api/routers/feed.ts`
- Integrated into `subscribeFeed` mutation
- Calls `discoverFeed()` for RSS feeds before saving
- Preserves discovered feed title if available
- Throws descriptive TRPCError on failure

### `/src/components/add-feed-modal.tsx`
- Displays error messages from backend
- Clears errors when user types
- Shows helpful hint: "Enter a feed URL or website homepage - we'll find the feed for you"

### `/src/components/feed-reader.tsx`
- Manages error state from tRPC mutation
- Passes errors to modal via props
- Clears errors on modal close

## Technical Details

### Dependencies
- **cheerio**: HTML parsing to find feed links
- **node-fetch**: Already available in Next.js server environment

### Timeout Handling
All HTTP requests have a 10-second timeout using `AbortController`:
```typescript
const controller = new AbortController()
const timeout = setTimeout(() => controller.abort(), 10000)
```

### Feed Type Detection
Checks Content-Type headers for:
- `application/rss+xml`
- `application/atom+xml`
- `application/xml`
- `text/xml`
- `application/feed+json`

## Testing Scenarios

1. **Direct Feed URL**: Should work immediately
2. **Homepage with feed**: Should find feed in HTML
3. **Blog with `/feed` path**: Should discover via common locations
4. **Invalid URL**: Should show error message
5. **Timeout**: Should fail gracefully after 10 seconds

## Future Enhancements

Potential improvements:
- Cache discovered feed URLs to avoid re-discovery
- Support for more feed formats (JSON Feed, etc.)
- Show discovery progress indicator
- Allow users to choose from multiple feeds if found
