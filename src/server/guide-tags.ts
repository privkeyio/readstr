import type { PrismaClient } from '@prisma/client'

export type GuideTag = { tag: string; count: number }

/**
 * guide.getGuideTags is public and scans every active guide feed, so without
 * a cache one IP could batch it hundreds of times a minute and tie up the
 * database pool. The tag list changes rarely, so it is computed at most once
 * per TTL, and concurrent callers share the query already in flight.
 */
export const GUIDE_TAGS_TTL_MS = 60_000

let cached: { expiresAt: number; value: Promise<GuideTag[]> } | null = null

export function getGuideTagsCached(
  db: Pick<PrismaClient, 'guideFeed'>,
  now: number = Date.now()
): Promise<GuideTag[]> {
  if (cached && cached.expiresAt > now) return cached.value
  const value = loadGuideTags(db)
  const entry = { expiresAt: now + GUIDE_TAGS_TTL_MS, value }
  cached = entry
  // Don't keep a failure for the whole TTL; the next caller retries.
  value.catch(() => {
    if (cached === entry) cached = null
  })
  return value
}

/** Call after any change to guide feeds so new tags show up right away. */
export function invalidateGuideTags(): void {
  cached = null
}

async function loadGuideTags(db: Pick<PrismaClient, 'guideFeed'>): Promise<GuideTag[]> {
  const feeds = await db.guideFeed.findMany({
    where: { isActive: true },
    select: { tags: true },
  })
  const tagCounts = new Map<string, number>()
  for (const feed of feeds) {
    for (const tag of feed.tags) {
      tagCounts.set(tag, (tagCounts.get(tag) || 0) + 1)
    }
  }
  return Array.from(tagCounts.entries())
    .map(([tag, count]) => ({ tag, count }))
    .sort((a, b) => b.count - a.count)
}
