# Category Sync Implementation

## Overview

Categories are now synced alongside tags in the Nostr subscription sync system (Kind 30404 events). This enables users to maintain their feed organization across different devices and clients.

**NEW: Automatic Bidirectional Sync** - Changes sync automatically in both directions:
- ✅ **Download (Import)**: Every 15 minutes, checks for new feeds/categories from other devices
- ✅ **Upload (Export)**: Immediately after any change (add/remove feed, change category, update tags)

## What Changed

### 1. Nostr Sync Library (`src/lib/nostr-sync.ts`)

**Updated `SubscriptionList` interface:**
```typescript
export interface SubscriptionList {
  rss: string[]
  nostr: string[]
  tags?: Record<string, string[]>
  categories?: Record<string, { name: string; color?: string; icon?: string }> // NEW
  deleted?: string[]
  lastUpdated?: number
}
```

**Updated `buildSubscriptionListFromFeeds` function:**
- Now accepts and exports category information for each feed
- Maps feed URLs/npubs to category objects containing name, color, and icon

**Updated `mergeSubscriptionLists` function:**
- Returns category information in the `toAdd` array
- Enables proper category restoration when importing feeds from other devices

### 2. Server-Side Sync (`src/server/api/routers/feed.ts`)

**Auto-sync in `getFeeds` endpoint:**
- When importing feeds from remote, automatically creates categories if they don't exist
- Maps remote category names to local category IDs
- Associates imported feeds with their categories

**Category creation logic:**
- Checks for existing categories by name
- Creates new categories with synced color and icon properties
- Maintains proper sort order for new categories

### 3. Client-Side Components

**Settings Dialog (`src/components/settings-dialog.tsx`):**
- Updated type definitions to include category information
- Passes category data when building subscription lists for export

**Feed Reader (`src/components/feed-reader.tsx`):**
- Updated `handleImportFeeds` to handle categories
- Automatically creates categories during import if they don't exist
- Maps feeds to categories using the synced category names
- Added `createCategoryMutation` for on-the-fly category creation

### 4. Documentation

**Updated `SUBSCRIPTION_SYNC.md`:**
- Added categories field to the schema documentation
- Updated example JSON to show category structure
- Updated code examples to handle categories

## How It Works

### Automatic Export (Upload to Nostr)

Changes are **automatically exported** to Nostr immediately after:
1. Adding a feed
2. Removing/unsubscribing from a feed
3. Updating a feed's tags
4. Changing a feed's category

**Process:**
1. User makes a change (e.g., adds a feed)
2. System waits 500ms for UI to update
3. Fetches all subscriptions (including deleted ones)
4. Builds subscription list with categories
5. Signs and publishes Kind 30404 event to Nostr relays
6. Happens silently in background (no user interruption)

### Automatic Import (Download from Nostr)

The system **automatically imports** changes every 15 minutes:
1. On page load/refresh
2. Every time feeds are fetched
3. Only if >15 minutes since last sync

**Process:**
1. Fetches Kind 30404 event from Nostr relays
2. Compares with local subscriptions
3. For each new feed with a category:
   - Checks if category exists locally by name
   - Creates category if needed (with synced color/icon)
   - Associates feed with the category
4. Imports feed with tags and category assignment

### Manual Export (Settings)

Users can still manually trigger export from Settings > Sync > "Export to Nostr"
- Useful for forcing an immediate sync
- Same process as automatic export

### Exporting Subscriptions (Legacy Documentation - Now Automatic)

1. ~~User clicks "Export to Nostr" in Settings > Sync~~ (Now happens automatically)
2. System calls `buildSubscriptionListFromFeeds` with current feeds (including categories)
3. Creates a Kind 30404 event with:
   - Feed URLs/npubs
   - Tags per feed
   - **Categories per feed** (name, color, icon)
   - **Deleted feeds** (for proper sync)
4. Publishes to configured Nostr relays

### Importing Subscriptions (Automatic Every 15 Minutes)

1. User clicks "Import from Nostr" or automatic sync triggers
2. System fetches Kind 30404 event from relays
3. For each new feed with a category:
   - Checks if category exists locally by name
   - Creates category if needed (with synced color/icon)
   - Associates feed with the category
4. Imports feed with tags and category assignment

### Cross-Device Sync

When syncing between devices:
- Categories are matched by **name** (case-sensitive)
- If a category doesn't exist, it's created with synced properties
- Feeds maintain their category associations
- Color and icon preferences are preserved

## Example Synced Data

```json
{
  "rss": ["https://example.com/feed.xml"],
  "nostr": ["npub1abc..."],
  "tags": {
    "https://example.com/feed.xml": ["tech", "news"]
  },
  "categories": {
    "https://example.com/feed.xml": {
      "name": "Technology",
      "color": "#3b82f6",
      "icon": "💻"
    },
    "npub1abc...": {
      "name": "Bitcoin",
      "color": "#f59e0b",
      "icon": "₿"
    }
  },
  "lastUpdated": 1738095789
}
```

## Benefits

1. **Full Organization Sync**: Users can switch between devices and maintain their complete feed organization
2. **Cross-Client Compatibility**: Other Nostr clients can read and preserve category information
3. **Automatic Category Creation**: No manual setup needed when moving to a new device
4. **Visual Consistency**: Colors and icons sync across devices
5. **Backwards Compatible**: Clients that don't support categories will simply ignore the field
6. **Real-Time Sync**: Changes appear on other devices within 15 minutes (or immediately on next page load)
7. **Zero User Effort**: No manual "Export" button clicking required
8. **Deleted Feed Tracking**: Properly syncs feed removals across devices

## Technical Implementation

### New Endpoint: `getAllSubscriptionsForSync`

Added to support automatic export with deleted feeds:
```typescript
getAllSubscriptionsForSync: protectedProcedure
  .query(async ({ ctx }) => {
    // Returns ALL subscriptions including deleted ones
    // Used for building complete sync payload
  })
```

### Auto-Export Function

Located in `feed-reader.tsx`:
```typescript
const autoExportToNostr = useCallback(async () => {
  // Fetches all subscriptions (including deleted)
  const allSubscriptions = await utils.feed.getAllSubscriptionsForSync.fetch()
  // Builds and publishes to Nostr
  const result = await publishSubscriptionList(subscriptionList, signEvent)
}, [user?.npub, utils.feed])
```

Called automatically after these mutations:
- `subscribeFeedMutation` (add feed)
- `unsubscribeFeedMutation` (remove feed)
- `updateTagsMutation` (update tags)
- `updateCategoryMutation` (change category)

### Throttling & Performance

- 500ms delay after mutations (allows UI to update first)
- Runs asynchronously (doesn't block UI)
- Silent failure (errors logged but don't interrupt user)
- Import happens max once per 15 minutes (prevents excessive relay queries)

## Implementation Notes

- Categories are matched by name during import
- If multiple feeds reference the same category name, only one category is created
- Category sort order is preserved locally but not synced (to allow per-device customization)
- The system gracefully handles missing categories (feeds import without category if creation fails)
- Categories are created on-demand during import to avoid conflicts

## Future Enhancements

- Category sort order syncing
- Category merge/rename detection
- Category-level metadata (description, rules)
- Hierarchical categories
