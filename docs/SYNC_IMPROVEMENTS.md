# Sync System Improvements - Implementation Summary

## Overview
Enhanced the Nostr subscription sync system to properly track feed deletions and read status across devices.

## Changes Made

### 1. Database Schema Updates (`prisma/schema.prisma`)

#### Subscription Model
- Added `updatedAt` field to track when subscriptions change
- Added `deletedAt` field for soft-delete tracking (instead of hard deletes)
- Added indexes for efficient querying of deleted items

#### ReadItem Model
- Added `syncedAt` field to track when read status was last synced to Nostr
- Added index for efficient querying of unsynced items

### 2. Sync Library Updates (`src/lib/nostr-sync.ts`)

#### New Interfaces
- Extended `SubscriptionList` with `deleted?: string[]` array
- Added `ReadStatusList` interface for read status sync

#### New Event Kind
- Added `READ_STATUS_KIND = 30405` for syncing read status

#### Enhanced Functions
- `buildSubscriptionListFromFeeds()` - Now includes deleted feeds in output
- `mergeSubscriptionLists()` - Now returns `toRemove` array for feeds deleted remotely
- Added `publishReadStatus()` - Publish read items to Nostr
- Added `fetchReadStatus()` - Fetch read items from Nostr

### 3. API Updates (`src/server/api/routers/feed.ts`)

#### Modified Endpoints
- `getFeeds` - Now filters out soft-deleted subscriptions (`deletedAt: null`)
- `unsubscribeFeed` - Changed from hard delete to soft delete (sets `deletedAt`)

#### New Endpoints
- `getSubscriptionsForSync` - Returns all subscriptions including deleted ones for sync
- `cleanupDeletedSubscriptions` - Hard deletes old soft-deleted records (cleanup)
- `markReadItemsSynced` - Marks read items as synced after publishing
- `getUnsyncedReadItems` - Gets read items that haven't been synced yet

### 4. Documentation Updates (`SUBSCRIPTION_SYNC.md`)
- Added documentation for kind 30405 (read status sync)
- Added `deleted` field to subscription list schema
- Added `ReadStatusList` interface documentation

## How It Works

### Deletion Tracking
1. When a user unsubscribes from a feed, it's **soft-deleted** (deletedAt timestamp set)
2. During sync, deleted feeds are included in the `deleted` array in the Nostr event
3. When another device syncs, it sees the deleted feeds and removes them locally
4. Old soft-deleted records can be cleaned up after 90 days (configurable)

### Read Status Sync
1. When items are marked as read, they're tracked in the local database
2. Unsynced read items can be published to Nostr (kind 30405)
3. When syncing on another device, the read status is fetched and applied
4. Items are marked with `syncedAt` timestamp after successful sync

## Migration

A database migration has been created:
- File: `prisma/migrations/20260128144700_add_sync_tracking/migration.sql`
- Adds `deletedAt` and `updatedAt` to Subscription table
- Adds `syncedAt` to ReadItem table
- Creates necessary indexes

**To apply:** Run `npx prisma migrate deploy` in production or `npx prisma migrate dev` in development

## Benefits

1. **No More Re-adding Deleted Feeds**: Deletions are now tracked and synced across devices
2. **Read Status Sync**: Read/unread status can be synced across devices (optional feature)
3. **Conflict Resolution**: Soft deletes allow for better conflict resolution in sync scenarios
4. **Data Integrity**: No loss of information during sync operations
5. **Cleanup**: Old deleted records can be purged after a grace period

## Next Steps for Implementation

1. Apply the database migration
2. Update client-side sync logic to use new `toRemove` array from merge function
3. Implement UI for read status sync (optional feature)
4. Add periodic cleanup job for old deleted subscriptions
5. Test cross-device sync scenarios

## Backward Compatibility

- Existing sync events (kind 30404) without `deleted` field will continue to work
- The `deleted` field is optional and only included when there are deletions
- Read status sync (kind 30405) is completely new and optional
