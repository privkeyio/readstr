import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { MAX_GUID_LEN, MAX_READ_ITEMS, readStatusListSchema } from '@/lib/nostr-sync'
import { MAX_PUBLISHED_READ_CONTENT, READ_SYNC_FEED_IDS_MAX } from '@/lib/read-sync'
import { feedRouter } from '@/server/api/routers/feed'
import { __resetRateLimitForTests } from '@/server/rate-limit'
import type { SubscriptionLookups } from '@/server/paywall'
import { chunk, GUID_QUERY_CHUNK, readGuidsInputSchema, readSyncKey, ReadSyncGuidCollector } from './read-sync'

const PUBKEY = '3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d'

function ctx(db: unknown) {
  return { db: db as never, nostrPubkey: PUBKEY, clientIp: '127.0.0.1', subscriptionLookups: new Map() as SubscriptionLookups }
}

beforeEach(() => {
  __resetRateLimitForTests()
})

afterEach(() => {
  vi.unstubAllEnvs()
})

describe('readSyncKey', () => {
  it('uses the trimmed guid, else the link, as the desktop does', () => {
    expect(readSyncKey({ guid: ' tag:a ', url: 'https://a.example/1' })).toBe('tag:a')
    expect(readSyncKey({ guid: null, url: 'https://a.example/1' })).toBe('https://a.example/1')
    expect(readSyncKey({ guid: '  ', url: 'https://a.example/1' })).toBe('https://a.example/1')
    expect(readSyncKey({ guid: null, url: null })).toBeNull()
  })
})

describe('ReadSyncGuidCollector', () => {
  it('keeps order, drops duplicates and over-long keys', () => {
    const c = new ReadSyncGuidCollector()
    for (const key of ['b', 'a', 'b', null, 'x'.repeat(MAX_GUID_LEN + 1), 'x'.repeat(MAX_GUID_LEN)]) c.add(key)
    expect(c.guids).toEqual(['b', 'a', 'x'.repeat(MAX_GUID_LEN)])
  })

  it('stops at the item cap', () => {
    const c = new ReadSyncGuidCollector(3)
    for (const key of ['a', 'b', 'c', 'd']) c.add(key)
    expect(c.guids).toEqual(['a', 'b', 'c'])
    expect(c.full).toBe(true)
  })

  it('keeps the published event under the publish cap and the relays\' 64 KiB limit', () => {
    // Short keys carry the most quoting overhead, escaped and non-ASCII keys
    // the most bytes per UTF-16 unit.
    const shapes = [
      (i: number) => `${i}`,
      (i: number) => `${i}`.padStart(MAX_GUID_LEN, 'g'),
      (i: number) => `${i}"\\`.padEnd(40, '"'),
      (i: number) => `${i}`.padEnd(60, 'ü€'),
    ]
    for (const shape of shapes) {
      const c = new ReadSyncGuidCollector()
      for (let i = 0; !c.full; i++) c.add(shape(i))
      const content = JSON.stringify({ itemGuids: c.guids, lastUpdated: 1_900_000_000 })
      const event = JSON.stringify({
        id: 'f'.repeat(64),
        pubkey: 'f'.repeat(64),
        created_at: 1_900_000_000,
        kind: 30405,
        tags: [['d', 'readstr-read-status'], ['client', 'readstr']],
        content,
        sig: 'f'.repeat(128),
      })
      expect(content.length).toBeLessThanOrEqual(MAX_PUBLISHED_READ_CONTENT)
      expect(new TextEncoder().encode(event).length).toBeLessThanOrEqual(MAX_PUBLISHED_READ_CONTENT)
      expect(c.guids.length).toBeGreaterThan(100)
      expect(readStatusListSchema.safeParse(JSON.parse(content)).success).toBe(true)
    }
  })
})

describe('chunk', () => {
  it('splits into fixed-size parts', () => {
    expect(chunk([1, 2, 3, 4, 5], 2)).toEqual([[1, 2], [3, 4], [5]])
    expect(chunk([], 2)).toEqual([])
  })

  it('refuses a size below one instead of looping forever', () => {
    for (const size of [0, -1, 0.5, Number.NaN]) expect(() => chunk([1], size)).toThrow(RangeError)
  })
})

describe('readGuidsInputSchema', () => {
  it('enforces the read-status caps', () => {
    expect(readGuidsInputSchema.safeParse({ guids: Array(MAX_READ_ITEMS).fill('g') }).success).toBe(true)
    expect(readGuidsInputSchema.safeParse({ guids: Array(MAX_READ_ITEMS + 1).fill('g') }).success).toBe(false)
    expect(readGuidsInputSchema.safeParse({ guids: ['g'.repeat(MAX_GUID_LEN + 1)] }).success).toBe(false)
  })

  it('measures guids in UTF-16 units, not code points', () => {
    expect(readGuidsInputSchema.safeParse({ guids: ['😀'.repeat(129)] }).success).toBe(false)
    expect(readGuidsInputSchema.safeParse({ guids: ['😀'.repeat(MAX_GUID_LEN / 2)] }).success).toBe(true)
  })
})

describe('feed.getReadGuidsForSync', () => {
  type Row = { id: string; feedItem: { guid: string | null; url: string | null } }
  type FindArgs = { take: number; cursor?: { id: string }; skip?: number; where: unknown }

  // Serves rows newest first in pages, as Prisma's cursor pagination does.
  function fakeDb(rows: Row[], feedIds = ['feed-1']) {
    return {
      subscription: { findMany: vi.fn(async () => feedIds.map((feedId) => ({ feedId }))) },
      readItem: {
        findMany: vi.fn(async (args: FindArgs) => {
          const start = args.cursor ? rows.findIndex((r) => r.id === args.cursor!.id) + (args.skip ?? 0) : 0
          return rows.slice(start, start + args.take)
        }),
      },
    }
  }

  it('returns the user\'s read keys newest first, only from active subscriptions', async () => {
    const db = fakeDb([
      { id: 'r2', feedItem: { guid: 'nostr-event-id', url: null } },
      { id: 'r1', feedItem: { guid: null, url: 'https://a.example/1' } },
      { id: 'r0', feedItem: { guid: null, url: null } },
    ])
    const result = await feedRouter.createCaller(ctx(db)).getReadGuidsForSync()
    expect(result).toEqual({ guids: ['nostr-event-id', 'https://a.example/1'] })
    expect(db.subscription.findMany).toHaveBeenCalledWith(
      expect.objectContaining({ where: { userPubkey: PUBKEY, deletedAt: null } })
    )
    expect(db.readItem.findMany).toHaveBeenCalledWith(
      expect.objectContaining({
        where: { userPubkey: PUBKEY, feedItem: { feedId: { in: ['feed-1'] } } },
        orderBy: [{ readAt: 'desc' }, { id: 'desc' }],
      })
    )
  })

  it('skips the read lookup without active subscriptions', async () => {
    const db = fakeDb([{ id: 'r0', feedItem: { guid: 'g', url: null } }], [])
    await expect(feedRouter.createCaller(ctx(db)).getReadGuidsForSync()).resolves.toEqual({ guids: [] })
    expect(db.readItem.findMany).not.toHaveBeenCalled()
  })

  it('pages through reads with a cursor', async () => {
    const rows = Array.from({ length: 1500 }, (_, i) => ({ id: `r${i}`, feedItem: { guid: `${i}`, url: null } }))
    const db = fakeDb(rows)
    const { guids } = await feedRouter.createCaller(ctx(db)).getReadGuidsForSync()
    expect(guids).toEqual(rows.map((r) => r.feedItem.guid))
    expect(db.readItem.findMany).toHaveBeenCalledTimes(2)
    expect(db.readItem.findMany.mock.calls[1]![0]).toMatchObject({ cursor: { id: 'r999' }, skip: 1 })
  })

  it('stops reading once the publish cap fills mid-page', async () => {
    const rows = Array.from({ length: 5000 }, (_, i) => ({
      id: `r${i}`,
      feedItem: { guid: `${i}`.padStart(MAX_GUID_LEN, 'g'), url: null },
    }))
    const db = fakeDb(rows)
    const { guids } = await feedRouter.createCaller(ctx(db)).getReadGuidsForSync()
    expect(guids.length).toBeGreaterThan(0)
    expect(guids.length).toBeLessThan(1000)
    expect(guids).toEqual(rows.slice(0, guids.length).map((r) => r.feedItem.guid))
    expect(db.readItem.findMany).toHaveBeenCalledOnce()
  })

  it('is rate limited per user', async () => {
    const db = fakeDb([])
    const caller = feedRouter.createCaller(ctx(db))
    for (let i = 0; i < 6; i++) await caller.getReadGuidsForSync()
    await expect(caller.getReadGuidsForSync()).rejects.toMatchObject({ code: 'TOO_MANY_REQUESTS' })
  })
})

// Subscriptions as rows, so the where clauses the procedures build are
// evaluated rather than only inspected: a row matches every filter the clause
// gives, and a filter the clause leaves out matches anything.
type SubRow = { userPubkey: string; feedId: string; deletedAt: Date | null; readListAppliedAt: Date | null; createdAt: Date; hasItems: boolean }
type SubWhere = {
  userPubkey?: string
  deletedAt?: null
  readListAppliedAt?: null
  feedId?: { in: string[] }
  feed?: { items: { some: Record<string, never> } }
}
const OTHER = 'f'.repeat(64)

function sub(feedId: string, extra: Partial<SubRow> = {}): SubRow {
  return { userPubkey: PUBKEY, feedId, deletedAt: null, readListAppliedAt: null, createdAt: new Date(0), hasItems: true, ...extra }
}

function matches(row: SubRow, where: SubWhere): boolean {
  return (
    (where.userPubkey === undefined || row.userPubkey === where.userPubkey) &&
    (where.deletedAt === undefined || row.deletedAt === null) &&
    (where.readListAppliedAt === undefined || row.readListAppliedAt === null) &&
    (where.feedId === undefined || where.feedId.in.includes(row.feedId)) &&
    (where.feed === undefined || row.hasItems)
  )
}

function fakeSubscriptions(rows: SubRow[]) {
  return {
    rows,
    findMany: vi.fn(async (args: { where: SubWhere; take?: number }) =>
      rows
        .filter((row) => matches(row, args.where))
        .sort((x, y) => x.createdAt.getTime() - y.createdAt.getTime())
        .slice(0, args.take ?? Infinity)
        .map((row) => ({ feedId: row.feedId }))
    ),
    updateMany: vi.fn(async (args: { where: SubWhere; data: { readListAppliedAt: Date } }) => {
      const hit = rows.filter((row) => matches(row, args.where))
      for (const row of hit) row.readListAppliedAt = args.data.readListAppliedAt
      return { count: hit.length }
    }),
  }
}

describe('feed.markReadByGuids', () => {
  type ItemArgs = { where: { feedId: { in: string[] }; OR: [{ guid: { in: string[] } }, unknown] } }

  // Every guid exists in feed-1, feed-2 and feed-3; the lookup honours the feedId filter.
  function fakeDb(subscriptions: Array<string | SubRow>, itemFeeds = ['feed-1', 'feed-2', 'feed-3']) {
    return {
      subscription: fakeSubscriptions(subscriptions.map((row) => (typeof row === 'string' ? sub(row) : row))),
      feedItem: {
        findMany: vi.fn(async (args: ItemArgs) =>
          itemFeeds
            .filter((feedId) => args.where.feedId.in.includes(feedId))
            .flatMap((feedId) => args.where.OR[0].guid.in.map((guid) => ({ id: `${feedId}-${guid}` })))
        ),
      },
      readItem: {
        createMany: vi.fn(async (args: { data: unknown[] }) => ({ count: args.data.length })),
        deleteMany: vi.fn(),
        delete: vi.fn(),
      },
    }
  }

  it('marks matching subscribed items read in chunks, never unread', async () => {
    const db = fakeDb(['feed-1'])
    const guids = Array.from({ length: GUID_QUERY_CHUNK * 2 + 5 }, (_, i) => `g${i}`)
    const result = await feedRouter.createCaller(ctx(db)).markReadByGuids({ guids: [...guids, 'g0', ''] })
    expect(result).toEqual({ marked: guids.length })
    expect(db.feedItem.findMany).toHaveBeenCalledTimes(3)
    const where = (db.feedItem.findMany.mock.calls[0]![0] as { where: Record<string, unknown> }).where
    expect(where).toEqual({
      feedId: { in: ['feed-1'] },
      OR: [{ guid: { in: guids.slice(0, GUID_QUERY_CHUNK) } }, { guid: null, url: { in: guids.slice(0, GUID_QUERY_CHUNK) } }],
      readItems: { none: { userPubkey: PUBKEY } },
    })
    expect(db.readItem.createMany).toHaveBeenCalledWith(
      expect.objectContaining({ data: expect.arrayContaining([{ userPubkey: PUBKEY, itemId: 'feed-1-g0' }]), skipDuplicates: true })
    )
    expect(db.readItem.delete).not.toHaveBeenCalled()
    expect(db.readItem.deleteMany).not.toHaveBeenCalled()
  })

  it('marks only items in the user\'s active subscribed feeds', async () => {
    const db = fakeDb(['feed-2', sub('feed-1', { deletedAt: new Date(1) }), sub('feed-3', { userPubkey: OTHER })])
    await expect(feedRouter.createCaller(ctx(db)).markReadByGuids({ guids: ['a', 'b'] })).resolves.toEqual({ marked: 2 })
    expect(db.readItem.createMany).toHaveBeenCalledWith({
      data: [
        { userPubkey: PUBKEY, itemId: 'feed-2-a' },
        { userPubkey: PUBKEY, itemId: 'feed-2-b' },
      ],
      skipDuplicates: true,
    })
  })

  it('with feedIds, touches only those of the caller\'s active subscriptions, so covered feeds keep their unreads', async () => {
    const db = fakeDb([
      'feed-1',
      'feed-2',
      sub('feed-3', { deletedAt: new Date(1) }),
      sub('feed-4', { userPubkey: OTHER }),
    ], ['feed-1', 'feed-2', 'feed-3', 'feed-4'])
    const caller = feedRouter.createCaller(ctx(db))
    await expect(caller.markReadByGuids({ guids: ['a'], feedIds: ['feed-2', 'feed-3', 'feed-4', 'feed-9', 'feed-2'] })).resolves.toEqual({
      marked: 1,
    })
    expect(db.readItem.createMany).toHaveBeenCalledOnce()
    expect(db.readItem.createMany).toHaveBeenCalledWith({ data: [{ userPubkey: PUBKEY, itemId: 'feed-2-a' }], skipDuplicates: true })

    // Only foreign, unsubscribed or unknown feeds: nothing is looked up.
    db.feedItem.findMany.mockClear()
    await expect(caller.markReadByGuids({ guids: ['a'], feedIds: ['feed-3', 'feed-4', 'feed-9'] })).resolves.toEqual({ marked: 0 })
    expect(db.feedItem.findMany).not.toHaveBeenCalled()
  })

  it('is rate limited per user, with room for a full pull and a separate budget for covering new feeds', async () => {
    const caller = feedRouter.createCaller(ctx(fakeDb(['feed-1'])))
    for (let i = 0; i < 250; i++) await caller.markReadByGuids({ guids: ['a'] })
    await expect(caller.markReadByGuids({ guids: ['a'] })).rejects.toMatchObject({ code: 'TOO_MANY_REQUESTS' })
    // The pull's budget is spent, yet the cover run still has all of its own.
    for (let i = 0; i < 250; i++) await caller.markReadByGuids({ guids: ['a'], feedIds: ['feed-1'] })
    await expect(caller.markReadByGuids({ guids: ['a'], feedIds: ['feed-1'] })).rejects.toMatchObject({ code: 'TOO_MANY_REQUESTS' })
  })

  it('skips the lookups without subscriptions or guids', async () => {
    for (const [feeds, guids] of [[[], ['a']], [['feed-1'], []]] as [string[], string[]][]) {
      const db = fakeDb(feeds)
      await expect(feedRouter.createCaller(ctx(db)).markReadByGuids({ guids })).resolves.toEqual({ marked: 0 })
      expect(db.feedItem.findMany).not.toHaveBeenCalled()
    }
  })

  it('rejects input past the caps', async () => {
    const db = fakeDb(['feed-1'])
    const caller = feedRouter.createCaller(ctx(db))
    const bad = { code: 'BAD_REQUEST' }
    await expect(caller.markReadByGuids({ guids: ['g'.repeat(MAX_GUID_LEN + 1)] })).rejects.toMatchObject(bad)
    await expect(caller.markReadByGuids({ guids: Array(MAX_READ_ITEMS + 1).fill('g') })).rejects.toMatchObject(bad)
    await expect(caller.markReadByGuids({ guids: ['a'], feedIds: Array(READ_SYNC_FEED_IDS_MAX + 1).fill('feed-1') })).rejects.toMatchObject(bad)
    await expect(caller.markReadByGuids({ guids: ['a'], feedIds: [] })).rejects.toMatchObject(bad)
    for (const feedId of ['', 'x'.repeat(65), 'feed 1', "feed'1", 'feed/1']) {
      await expect(caller.markReadByGuids({ guids: ['a'], feedIds: [feedId] })).rejects.toMatchObject(bad)
    }
    await expect(caller.markReadByGuids({ guids: ['a'], feedIds: 'feed-1' as never })).rejects.toMatchObject(bad)
    expect(db.subscription.findMany).not.toHaveBeenCalled()
    await expect(caller.markReadByGuids({ guids: ['a'], feedIds: Array(READ_SYNC_FEED_IDS_MAX).fill('x'.repeat(64)) })).resolves.toEqual({ marked: 0 })
  })
})

describe('feed.getReadSyncUncoveredFeeds', () => {
  it('lists the caller\'s active subscriptions the read list was never applied to, oldest first', async () => {
    const subscription = fakeSubscriptions([
      sub('feed-new', { createdAt: new Date(3) }),
      sub('feed-covered', { readListAppliedAt: new Date(1) }),
      sub('feed-gone', { deletedAt: new Date(1) }),
      sub('feed-old', { createdAt: new Date(1) }),
      sub('feed-foreign', { userPubkey: OTHER }),
      sub('feed-empty', { createdAt: new Date(2), hasItems: false }),
    ])
    const caller = feedRouter.createCaller(ctx({ subscription }))
    await expect(caller.getReadSyncUncoveredFeeds()).resolves.toEqual({ feedIds: ['feed-old', 'feed-new'], more: false })
  })

  it('returns at most READ_SYNC_FEED_IDS_MAX and says when more remain', async () => {
    const rows = Array.from({ length: READ_SYNC_FEED_IDS_MAX + 1 }, (_, i) => sub(`feed-${i}`, { createdAt: new Date(i) }))
    const caller = feedRouter.createCaller(ctx({ subscription: fakeSubscriptions(rows) }))
    const first = await caller.getReadSyncUncoveredFeeds()
    expect(first.feedIds).toEqual(rows.slice(0, READ_SYNC_FEED_IDS_MAX).map((row) => row.feedId))
    expect(first.more).toBe(true)
    rows.pop()
    await expect(caller.getReadSyncUncoveredFeeds()).resolves.toMatchObject({ more: false })
  })

  it('is rate limited per user', async () => {
    const caller = feedRouter.createCaller(ctx({ subscription: fakeSubscriptions([]) }))
    for (let i = 0; i < 120; i++) await caller.getReadSyncUncoveredFeeds()
    await expect(caller.getReadSyncUncoveredFeeds()).rejects.toMatchObject({ code: 'TOO_MANY_REQUESTS' })
  })
})

describe('feed.markReadListApplied', () => {
  it('marks only the given feeds among the caller\'s active, uncovered subscriptions', async () => {
    const earlier = new Date(5)
    const subscription = fakeSubscriptions([
      sub('feed-1'),
      sub('feed-2'),
      sub('feed-covered', { readListAppliedAt: earlier }),
      sub('feed-gone', { deletedAt: new Date(1) }),
      sub('feed-foreign', { userPubkey: OTHER }),
    ])
    const caller = feedRouter.createCaller(ctx({ subscription }))
    await expect(
      caller.markReadListApplied({ feedIds: ['feed-1', 'feed-covered', 'feed-gone', 'feed-foreign', 'feed-9', 'feed-1'] })
    ).resolves.toEqual({ updated: 1 })
    const applied = Object.fromEntries(subscription.rows.map((row) => [`${row.userPubkey === PUBKEY ? '' : 'other:'}${row.feedId}`, row.readListAppliedAt]))
    expect(applied['feed-1']).toBeInstanceOf(Date)
    expect(applied['feed-2']).toBeNull()
    expect(applied['feed-covered']).toBe(earlier)
    expect(applied['feed-gone']).toBeNull()
    expect(applied['other:feed-foreign']).toBeNull()
  })

  it('needs feedIds within the caps', async () => {
    const subscription = fakeSubscriptions([sub('feed-1')])
    const caller = feedRouter.createCaller(ctx({ subscription }))
    const bad = { code: 'BAD_REQUEST' }
    await expect(caller.markReadListApplied({} as never)).rejects.toMatchObject(bad)
    await expect(caller.markReadListApplied({ feedIds: [] })).rejects.toMatchObject(bad)
    await expect(caller.markReadListApplied({ feedIds: Array(READ_SYNC_FEED_IDS_MAX + 1).fill('feed-1') })).rejects.toMatchObject(bad)
    await expect(caller.markReadListApplied({ feedIds: ['feed;1'] })).rejects.toMatchObject(bad)
    expect(subscription.updateMany).not.toHaveBeenCalled()
  })

  it('is rate limited per user', async () => {
    const caller = feedRouter.createCaller(ctx({ subscription: fakeSubscriptions([]) }))
    for (let i = 0; i < 60; i++) await caller.markReadListApplied({ feedIds: ['feed-1'] })
    await expect(caller.markReadListApplied({ feedIds: ['feed-1'] })).rejects.toMatchObject({ code: 'TOO_MANY_REQUESTS' })
  })
})
