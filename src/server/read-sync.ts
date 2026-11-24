import { z } from 'zod'
import { isAcceptableListCreatedAt, MAX_READ_ITEMS, readGuidSchema } from '@/lib/nostr-sync'
import { READ_SYNC_FEED_IDS_MAX } from '@/lib/read-sync'

export { ReadSyncGuidCollector } from '@/lib/read-sync'

// Rows per `in` query, so a 100k-guid import never builds one giant statement.
export const GUID_QUERY_CHUNK = 1000

export const readGuidsInputSchema = z.object({
  guids: z.array(readGuidSchema).max(MAX_READ_ITEMS),
})

// Feed ids are cuids. The charset and length bound keep a hostile list from
// building a huge `in` query, and the count bound keeps a scoped
// markReadByGuids call under the request body cap (see READ_SYNC_FEED_IDS_MAX).
const feedIdSchema = z.string().min(1).max(64).regex(/^[A-Za-z0-9_-]+$/)
export const readSyncFeedIdsSchema = z.array(feedIdSchema).min(1).max(READ_SYNC_FEED_IDS_MAX)

export const markReadByGuidsInputSchema = readGuidsInputSchema.extend({
  feedIds: readSyncFeedIdsSchema.optional(),
})

export const markReadListAppliedInputSchema = z.object({
  feedIds: readSyncFeedIdsSchema,
})

// The account only keys the client's query cache, so a request still in flight
// for the account signed in before is never joined by the next one. The
// server answers for the caller (ctx.nostrPubkey) and ignores it.
export const getReadListAppliedInputSchema = z
  .object({ account: z.string().max(200) })
  .optional()

// A created_at far in the future would stop every later event from counting as
// newer, so it is refused, as the subscription sync refuses such lists.
export const advanceReadListAppliedInputSchema = z.object({
  createdAt: z.number().refine((createdAt) => isAcceptableListCreatedAt(createdAt), {
    message: 'created_at is not a valid time or is too far in the future',
  }),
})

/**
 * The key an item has in a kind 30405 event: its guid, or its link when it has
 * none. The desktop keys items the same way (feedparser.cpp finishItem, with the
 * guid trimmed) and uses the Nostr event id for Nostr items, which the web
 * stores as the guid.
 */
export function readSyncKey(item: { guid: string | null; url: string | null }): string | null {
  const guid = item.guid?.trim()
  if (guid) return guid
  const url = item.url?.trim()
  return url || null
}

export function chunk<T>(items: T[], size: number): T[][] {
  // A size below one would loop forever; that is a caller bug, not a request.
  if (!(size >= 1)) throw new RangeError(`chunk size must be at least 1, got ${size}`)
  const step = Math.floor(size)
  const chunks: T[][] = []
  for (let i = 0; i < items.length; i += step) chunks.push(items.slice(i, i + step))
  return chunks
}
