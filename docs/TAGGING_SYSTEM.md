# Tagging System Implementation

## Overview
The tagging system allows users to organize their RSS and Nostr feeds using custom tags/categories. Tags enable powerful filtering and organization of feeds.

## Features

### 1. Tag Management
- **Add tags when subscribing**: Users can add multiple tags when subscribing to a new feed
- **Tag input with autocomplete**: Simple input field with "Add" button or press Enter
- **Visual tag display**: Tags shown as pills/badges on feeds
- **Remove tags**: Click × on tag to remove it

### 2. Sidebar Views
The left sidebar has two views that users can toggle between:

#### Feeds View
- Shows all subscribed feeds (filtered by selected tags if any)
- Displays tags for each feed
- Shows unread count per feed
- Includes "All Items" aggregated view

#### Tags View
- Shows all user's tags
- Displays feed count per tag (how many feeds have this tag)
- Shows unread count per tag (total unread items across all feeds with this tag)
- Click a tag to filter feeds by that tag

### 3. Tag Filtering
- **Multi-tag filtering**: Select multiple tags to narrow down feeds
- **Active filter indicator**: Blue banner shows currently selected tags
- **Clear filters**: One-click to remove all tag filters
- **Drill-down**: Switch to Feeds view to see filtered results

## Database Schema

### Subscription Model
```prisma
model Subscription {
  id          String   @id @default(cuid())
  userPubkey  String
  feedId      String
  createdAt   DateTime @default(now())
  tags        String[] @default([])  // Array of tag strings
  
  feed        Feed     @relation(fields: [feedId], references: [id], onDelete: Cascade)

  @@unique([userPubkey, feedId])
  @@index([userPubkey, tags])
}
```

## API Endpoints

### `subscribeFeed`
**Input:**
```typescript
{
  type: 'RSS' | 'NOSTR',
  url?: string,
  npub?: string,
  title?: string,
  tags?: string[]  // Optional array of tags
}
```

### `updateSubscriptionTags`
**Input:**
```typescript
{
  feedId: string,
  tags: string[]  // Replace existing tags with new array
}
```

### `getUserTags`
**Output:**
```typescript
[{
  tag: string,
  unreadCount: number,  // Total unread items across all feeds with this tag
  feedCount: number     // Number of feeds with this tag
}]
```

### `getFeeds`
**Input:**
```typescript
{
  tags?: string[]  // Optional: filter feeds that have ALL specified tags
}
```

**Output:**
```typescript
[{
  id: string,
  title: string,
  type: 'RSS' | 'NOSTR',
  unreadCount: number,
  tags: string[]  // Tags assigned to this feed
  // ... other fields
}]
```

## UI Components

### AddFeedModal
- Tag input field at the bottom of the modal
- Live tag preview (pills that can be removed before submitting)
- Tags persist when toggling between RSS/Nostr tabs
- Tags cleared on modal close

### FeedReader Sidebar
- **View Toggle**: Feeds ↔️ Tags tabs at top
- **Active Filters Banner**: Shows when tags are selected (can clear all or remove individually)
- **Feeds List**: Shows tag pills on each feed
- **Tags List**: Shows each tag with unread count badge and feed count

## Usage Examples

### Add a feed with tags
1. Click "Add Feed"
2. Enter feed URL or search for Nostr profile
3. Type tag name in "Tags" field and click "Add" or press Enter
4. Add multiple tags as needed
5. Click "Add Feed"

### Filter feeds by tags
1. Click "Tags" tab in sidebar
2. Click on one or more tags to filter
3. Switch to "Feeds" tab to see filtered results
4. Click "Clear" in blue banner to remove filters

### View unread counts per tag
1. Click "Tags" tab in sidebar
2. See unread count badge next to each tag
3. Also see how many feeds have each tag

## Implementation Notes

### Tag Filtering Logic
- Uses PostgreSQL's `hasEvery` operator for array field filtering
- Filters are AND-based: selecting multiple tags shows only feeds with ALL selected tags
- Unread counts are calculated by aggregating across all feeds matching the tag(s)

### Performance Considerations
- Added composite index on `[userPubkey, tags]` for efficient tag queries
- Tag aggregation happens in-memory for better performance
- Consider caching tag counts for large datasets

### Future Enhancements
- [ ] Tag editing for existing subscriptions
- [ ] Tag rename/merge functionality
- [ ] Tag color customization
- [ ] Export/import tags with feed subscriptions
- [ ] Tag-based RSS export (OPML with categories)
- [ ] Smart tag suggestions based on feed content
- [ ] Tag hierarchies (parent/child tags)
