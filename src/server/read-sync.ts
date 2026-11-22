import { z } from 'zod'
import { MAX_READ_ITEMS, readGuidSchema } from '@/lib/nostr-sync'

export { ReadSyncGuidCollector } from '@/lib/read-sync'

// Rows per `in` query, so a 100k-guid import never builds one giant statement.
export const GUID_QUERY_CHUNK = 1000

export const readGuidsInputSchema = z.object({
  guids: z.array(readGuidSchema).max(MAX_READ_ITEMS),
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
