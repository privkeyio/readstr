import { beforeEach, describe, expect, it, vi } from 'vitest'
import { feedRouter } from '@/server/api/routers/feed'
import { __resetRateLimitForTests } from '@/server/rate-limit'
import type { SubscriptionLookups } from '@/server/paywall'

vi.mock('@/lib/feed-discovery', () => ({
  discoverFeed: vi.fn(async (url: string) => ({ found: true, feedUrl: url, title: 'Example' })),
}))

vi.mock('@/lib/rss-parser', () => ({
  fetchAndParseFeed: vi.fn(async () => ({ title: 'Example', items: [] })),
}))

vi.mock('@/lib/nostr-sync', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/lib/nostr-sync')>()),
  fetchSubscriptionListFromServer: vi.fn(),
}))

// Relay events served to the real fetchSubscriptionListFromServer
const relayEvents: unknown[] = []
vi.mock('nostr-tools', async (importOriginal) => {
  const actual = await importOriginal<typeof import('nostr-tools')>()
  class FakePool {
    async querySync() {
      return relayEvents
    }
    close() {}
  }
  return { ...actual, SimplePool: FakePool }
})

const { fetchSubscriptionListFromServer } = await import('@/lib/nostr-sync')
const { discoverFeed } = await import('@/lib/feed-discovery')
const { fetchAndParseFeed } = await import('@/lib/rss-parser')
const actualSync = await vi.importActual<typeof import('@/lib/nostr-sync')>('@/lib/nostr-sync')

const PUBKEY = '3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d'
const NPUB = 'npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6'
const URL_A = 'https://a.example/feed.xml'
const URL_B = 'https://b.example/feed.xml'

function ctx(db: unknown) {
  return { db: db as never, nostrPubkey: PUBKEY, clientIp: '127.0.0.1', subscriptionLookups: new Map() as SubscriptionLookups }
}

type Feed = {
  id: string
  type: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'
  url: string | null
  npub: string | null
  title: string
  isActive: boolean
  lastFetchedAt: Date | null
}
type Row = {
  id: string
  userPubkey: string
  feedId: string
  tags: string[]
  categoryId: string | null
  deletedAt: Date | null
  readListAppliedAt: Date | null
}
type Category = { id: string; userPubkey: string; name: string; color: string | null; icon: string | null; sortOrder: number }
type Where = Record<string, unknown>

function checkDate(d: unknown) {
  if (d instanceof Date && Number.isNaN(d.getTime())) throw new Error('PrismaClientValidationError: Invalid Date')
}

function matchDate(value: Date | null, filter: unknown): boolean {
  if (filter === null) return value === null
  const f = filter as Record<string, unknown>
  for (const key of Object.keys(f)) {
    if (!['lt', 'gte', 'not'].includes(key)) throw new Error(`fake db: unsupported deletedAt filter ${key}`)
    checkDate(f[key])
  }
  if ('not' in f) {
    if (f.not !== null) throw new Error('fake db: only deletedAt.not = null is supported')
    if (value === null) return false
  }
  if (f.lt !== undefined && !(value !== null && value < (f.lt as Date))) return false
  if (f.gte !== undefined && !(value !== null && value >= (f.gte as Date))) return false
  return true
}

// Throws on a where key it does not model, so a filter the code relies on can
// never be silently ignored by the fake.
function matches(row: Row, where: Where): boolean {
  for (const [key, value] of Object.entries(where)) {
    switch (key) {
      case 'id':
      case 'userPubkey':
      case 'categoryId':
        if (row[key] !== value) return false
        break
      case 'feedId':
        if (typeof value === 'string' ? row.feedId !== value : !(value as { in: string[] }).in.includes(row.feedId)) return false
        break
      case 'deletedAt':
        if (!matchDate(row.deletedAt, value)) return false
        break
      case 'OR':
        if (!(value as Where[]).some((w) => matches(row, w))) return false
        break
      default:
        throw new Error(`fake db: unsupported subscription where key ${key}`)
    }
  }
  return true
}

type FeedWhere = { type?: string; url?: string; npub?: string; title?: { endsWith?: string; not?: { endsWith?: string } } }
function feedMatches(f: Feed, w: FeedWhere) {
  for (const key of Object.keys(w)) {
    if (!['type', 'url', 'npub', 'title'].includes(key)) throw new Error(`fake db: unsupported feed where key ${key}`)
  }
  if (w.type !== undefined && f.type !== w.type) return false
  if (w.url !== undefined && f.url !== w.url) return false
  if (w.npub !== undefined && f.npub !== w.npub) return false
  if (w.title?.endsWith && !f.title.endsWith(w.title.endsWith)) return false
  if (w.title?.not?.endsWith && f.title.endsWith(w.title.not.endsWith)) return false
  return true
}

const tick = () => new Promise((resolve) => setTimeout(resolve, 0))

// In-memory tables honouring the query shapes these paths use.
function fakeDb(feeds: Feed[], rows: Row[], categories: Category[] = []) {
  let seq = 1000
  const feedOf = (row: Row) => ({ ...feeds.find((f) => f.id === row.feedId)!, _count: { items: 0 }, items: [] })
  const withRelations = (row: Row) => ({ ...row, feed: feedOf(row), category: null })
  const byKey = (key: { userPubkey: string; feedId: string }) =>
    rows.find((r) => r.userPubkey === key.userPubkey && r.feedId === key.feedId)
  const newRow = (data: Partial<Row>) =>
    ({ id: `sub-${++seq}`, tags: [], categoryId: null, deletedAt: null, readListAppliedAt: null, ...data }) as Row
  return {
    feed: {
      findFirst: vi.fn(async ({ where }: { where: FeedWhere }) => feeds.find((f) => feedMatches(f, where)) ?? null),
      create: vi.fn(async ({ data }: { data: Partial<Feed> }) => {
        if (feeds.some((f) => f.type === data.type && ((data.url && f.url === data.url) || (data.npub && f.npub === data.npub))))
          throw new Error('P2002 Unique constraint failed on Feed')
        const feed = { id: `feed-${++seq}`, url: null, npub: null, isActive: false, lastFetchedAt: null, title: 'x', ...data } as Feed
        feeds.push(feed)
        return feed
      }),
      update: vi.fn(async ({ where, data }: { where: { id: string }; data: Partial<Feed> }) =>
        Object.assign(feeds.find((f) => f.id === where.id)!, data),
      ),
    },
    category: {
      findFirst: vi.fn(async ({ where }: { where: { userPubkey: string; name: string } }) =>
        categories.find((c) => c.userPubkey === where.userPubkey && c.name === where.name) ?? null,
      ),
      aggregate: vi.fn(async () => ({ _max: { sortOrder: categories.length ? Math.max(...categories.map((c) => c.sortOrder)) : null } })),
      create: vi.fn(async ({ data }: { data: Partial<Category> }) => {
        const category = { id: `cat-${++seq}`, color: null, icon: null, sortOrder: 0, ...data } as Category
        categories.push(category)
        return category
      }),
      findMany: vi.fn(async ({ where, include }: {
        where: { userPubkey: string }
        include: { _count?: { select: { subscriptions: true | { where: Where } } }; subscriptions?: { where?: Where } }
      }) => {
        const subsOf = (c: Category, filter: Where = {}) => rows.filter((r) => r.categoryId === c.id && matches(r, filter))
        return categories
          .filter((c) => c.userPubkey === where.userPubkey)
          .map((c) => {
            const countSelect = include._count?.select.subscriptions
            return {
              ...c,
              ...(include._count ? { _count: { subscriptions: subsOf(c, countSelect === true ? {} : countSelect?.where).length } } : {}),
              ...(include.subscriptions ? { subscriptions: subsOf(c, include.subscriptions.where).map(withRelations) } : {}),
            }
          })
      }),
    },
    feedItem: { count: vi.fn(async () => 1), createMany: vi.fn(async () => ({ count: 0 })) },
    userPreference: { findUnique: vi.fn(async () => null), upsert: vi.fn(async (args: { update: object }) => args) },
    subscription: {
      findMany: vi.fn(async ({ where }: { where: Where }) => rows.filter((r) => matches(r, where)).map(withRelations)),
      findUnique: vi.fn(async ({ where }: { where: { id?: string; userPubkey_feedId?: { userPubkey: string; feedId: string } } }) => {
        await tick()
        const row = where.id ? rows.find((r) => r.id === where.id) : byKey(where.userPubkey_feedId!)
        return row ? { ...row } : null
      }),
      create: vi.fn(async ({ data }: { data: Partial<Row> }) => {
        if (byKey(data as never)) throw new Error('P2002 Unique constraint failed on Subscription')
        const row = newRow(data)
        rows.push(row)
        return row
      }),
      update: vi.fn(async ({ where, data }: { where: { id: string }; data: Partial<Row> }) => {
        const row = rows.find((r) => r.id === where.id)
        if (!row) throw new Error('P2025 Record not found')
        return Object.assign(row, data)
      }),
      updateMany: vi.fn(async ({ where, data }: { where: Where; data: Partial<Row> }) => {
        const hit = rows.filter((r) => matches(r, where))
        for (const row of hit) Object.assign(row, data)
        return { count: hit.length }
      }),
      upsert: vi.fn(async ({ where, create, update }: {
        where: { userPubkey_feedId: { userPubkey: string; feedId: string } }
        create: Partial<Row>
        update: Partial<Row>
      }) => {
        const row = byKey(where.userPubkey_feedId)
        if (row) return Object.assign(row, update)
        const created = newRow(create)
        rows.push(created)
        return created
      }),
    },
  }
}

const rss = (id: string, url: string, extra: Partial<Feed> = {}): Feed =>
  ({ id, type: 'RSS', url, npub: null, title: id, isActive: false, lastFetchedAt: null, ...extra })
const feedA = rss('feed-a', URL_A)
const feedB = rss('feed-b', URL_B)
const mainFeed: Feed = { id: 'main', type: 'NOSTR', url: null, npub: NPUB, title: 'Alice', isActive: false, lastFetchedAt: null }
const videoFeed: Feed = { id: 'video', type: 'NOSTR_VIDEO', url: null, npub: NPUB, title: 'Alice (Videos)', isActive: false, lastFetchedAt: null }
const APPLIED = new Date('2026-11-01T00:00:00Z')
const LIST_AT = Date.parse('2026-11-15T00:00:00Z') / 1000
const BEFORE = new Date((LIST_AT - 3600) * 1000)
const AFTER = new Date((LIST_AT + 3600) * 1000)

function row(feedId: string, extra: Partial<Row> = {}): Row {
  return {
    id: `sub-${feedId}`,
    userPubkey: PUBKEY,
    feedId,
    tags: ['old'],
    categoryId: 'cat-old',
    deletedAt: null,
    readListAppliedAt: APPLIED,
    ...extra,
  }
}

function category(id: string, name: string): Category {
  return { id, userPubkey: PUBKEY, name, color: null, icon: null, sortOrder: 0 }
}

beforeEach(() => {
  __resetRateLimitForTests()
  vi.mocked(fetchSubscriptionListFromServer).mockReset()
  vi.mocked(discoverFeed).mockClear()
  vi.mocked(fetchAndParseFeed).mockClear()
  relayEvents.length = 0
  vi.restoreAllMocks()
})

describe('feed.subscribeFeed on an existing subscription row', () => {
  it('reactivates a soft-deleted row so it shows again and read sync re-applies', async () => {
    const rows = [row('feed-a', { deletedAt: new Date('2026-11-10T00:00:00Z') })]
    const db = fakeDb([feedA], rows)

    const result = await feedRouter.createCaller(ctx(db)).subscribeFeed({ type: 'RSS', url: URL_A, tags: ['news'] })

    expect(result).toMatchObject({ id: 'feed-a', tags: ['news'], categoryId: null })
    expect(db.subscription.create).not.toHaveBeenCalled()
    expect(rows).toHaveLength(1)
    expect(rows[0]).toMatchObject({ deletedAt: null, readListAppliedAt: null, tags: ['news'], categoryId: null })
  })

  it('leaves an active row untouched', async () => {
    const rows = [row('feed-a')]
    const before = { ...rows[0]! }
    const db = fakeDb([feedA], rows)

    const result = await feedRouter.createCaller(ctx(db)).subscribeFeed({ type: 'RSS', url: URL_A, tags: ['news'] })

    expect(result).toMatchObject({ id: 'feed-a', tags: ['old'] })
    expect(db.subscription.create).not.toHaveBeenCalled()
    expect(db.subscription.updateMany).not.toHaveBeenCalled()
    expect(rows[0]).toEqual(before)
  })

  it('lets the first of two concurrent resubscribes win and reports it to the second', async () => {
    const rows = [row('feed-a', { deletedAt: BEFORE })]
    const db = fakeDb([feedA], rows)
    const caller = feedRouter.createCaller(ctx(db))

    const results = await Promise.all([
      caller.subscribeFeed({ type: 'RSS', url: URL_A, tags: ['first'] }),
      caller.subscribeFeed({ type: 'RSS', url: URL_A, tags: ['second'] }),
    ])

    expect(rows).toHaveLength(1)
    expect(rows[0]).toMatchObject({ deletedAt: null, readListAppliedAt: null, tags: ['first'] })
    expect(results.map((r) => r.tags)).toEqual([['first'], ['first']])
  })

  it('reactivates both the main and the video subscription of a Nostr feed', async () => {
    const rows = [row('main', { deletedAt: BEFORE }), row('video', { deletedAt: BEFORE })]
    const db = fakeDb([mainFeed, videoFeed], rows)

    await feedRouter.createCaller(ctx(db)).subscribeFeed({ type: 'NOSTR', npub: NPUB, tags: ['x'] })

    expect(rows.map((r) => [r.feedId, r.deletedAt, r.readListAppliedAt, r.tags])).toEqual([
      ['main', null, null, ['x']],
      ['video', null, null, ['x', 'video']],
    ])
  })
})

describe('feed.getFeeds applying a kind 30404 subscription list', () => {
  function remoteList(
    list: { rss?: string[]; nostr?: string[]; deleted?: string[]; tags?: Record<string, string[]>; categories?: Record<string, { name: string }> },
    createdAt: number = LIST_AT,
  ) {
    vi.mocked(fetchSubscriptionListFromServer).mockResolvedValue({
      success: true,
      data: { rss: [], nostr: [], tags: {}, categories: {}, ...list } as never,
      createdAt,
    })
  }
  const sync = (db: unknown) => feedRouter.createCaller(ctx(db)).getFeeds({ forceSync: true })

  it('reactivates a feed unsubscribed before the list was published', async () => {
    const rows = [row('feed-a', { deletedAt: new Date('2026-11-10T00:00:00Z') })]
    const db = fakeDb([feedA], rows)
    remoteList({ rss: [URL_A] })

    const feeds = await sync(db)

    expect(rows).toHaveLength(1)
    expect(rows[0]).toMatchObject({ deletedAt: null, readListAppliedAt: null })
    expect(feeds.map((f) => f.id)).toEqual(['feed-a'])
  })

  it('keeps a feed unsubscribed after the list was published', async () => {
    const rows = [row('feed-a', { deletedAt: AFTER })]
    const db = fakeDb([feedA], rows)
    remoteList({ rss: [URL_A] })

    const feeds = await sync(db)

    expect(db.subscription.updateMany).not.toHaveBeenCalled()
    expect(db.subscription.upsert).not.toHaveBeenCalled()
    expect(rows[0]).toMatchObject({ deletedAt: AFTER, readListAppliedAt: APPLIED })
    expect(feeds).toEqual([])
  })

  it('keeps a feed unsubscribed in the same instant the list was published', async () => {
    const at = new Date(LIST_AT * 1000)
    const rows = [row('feed-a', { deletedAt: at })]
    const db = fakeDb([feedA], rows)
    remoteList({ rss: [URL_A] })

    expect(await sync(db)).toEqual([])
    expect(rows[0]).toMatchObject({ deletedAt: at, readListAppliedAt: APPLIED })
  })

  it('does not reset read sync coverage of an active feed', async () => {
    const rows = [row('feed-a'), row('feed-b', { deletedAt: BEFORE })]
    const db = fakeDb([feedA, feedB], rows)
    remoteList({ rss: [URL_A, URL_B] })

    await sync(db)

    expect(rows[0]).toMatchObject({ deletedAt: null, readListAppliedAt: APPLIED, tags: ['old'] })
    expect(rows[1]).toMatchObject({ deletedAt: null, readListAppliedAt: null })
    expect(db.subscription.upsert).not.toHaveBeenCalled()
  })

  it.each([
    ['a trailing slash', 'https://a.example/feed.xml/'],
    ['an uppercase host', 'https://A.EXAMPLE/feed.xml'],
  ])('reactivates the existing row when the list spells the URL with %s', async (_, spelling) => {
    const feeds = [feedA]
    const rows = [row('feed-a', { deletedAt: BEFORE })]
    const db = fakeDb(feeds, rows)
    remoteList({ rss: [spelling, URL_A] })

    await sync(db)

    expect(discoverFeed).not.toHaveBeenCalled()
    expect(feeds).toHaveLength(1)
    expect(rows).toHaveLength(1)
    expect(rows[0]).toMatchObject({ deletedAt: null, readListAppliedAt: null })
  })

  it('gives a reactivated row the list category, or none, rather than its old one', async () => {
    const rows = [row('feed-a', { deletedAt: BEFORE }), row('feed-b', { deletedAt: BEFORE })]
    const db = fakeDb([feedA, feedB], rows, [category('cat-news', 'News')])
    remoteList({ rss: [URL_A, URL_B], tags: { [URL_A]: ['t'] }, categories: { [URL_A]: { name: 'News' } } })

    await sync(db)

    expect(rows[0]).toMatchObject({ deletedAt: null, categoryId: 'cat-news', tags: ['t'] })
    expect(rows[1]).toMatchObject({ deletedAt: null, categoryId: null, tags: [] })
  })

  it('keeps the tags of a row the user resubscribed to while the import ran', async () => {
    const rows = [row('feed-a', { deletedAt: BEFORE })]
    const db = fakeDb([feedA], rows)
    db.category.findFirst.mockImplementation(async () => {
      Object.assign(rows[0]!, { deletedAt: null, tags: ['mine'], readListAppliedAt: null })
      return category('cat-news', 'News')
    })
    remoteList({ rss: [URL_A], tags: { [URL_A]: ['remote'] }, categories: { [URL_A]: { name: 'News' } } })

    await sync(db)

    expect(rows[0]).toMatchObject({ deletedAt: null, tags: ['mine'], categoryId: 'cat-old' })
  })

  it('reactivates many feeds in batched queries', async () => {
    const feeds: Feed[] = []
    const rows: Row[] = []
    const urls: string[] = []
    for (let i = 0; i < 1200; i++) {
      const url = `https://f${i}.example/feed.xml`
      urls.push(url)
      feeds.push(rss(`f${i}`, url))
      rows.push(row(`f${i}`, { deletedAt: BEFORE }))
    }
    const db = fakeDb(feeds, rows)
    remoteList({ rss: urls })

    await sync(db)

    expect(rows.every((r) => r.deletedAt === null && r.readListAppliedAt === null)).toBe(true)
    expect(db.subscription.updateMany).toHaveBeenCalledTimes(3)
    expect(db.feed.findFirst).not.toHaveBeenCalled()
  })

  describe('Nostr feeds', () => {
    it('reactivates the main feed and its video feed, matching the npub case-insensitively', async () => {
      const feeds = [mainFeed, videoFeed]
      const rows = [row('main', { deletedAt: BEFORE }), row('video', { deletedAt: BEFORE })]
      const db = fakeDb(feeds, rows)
      const upper = 'npub1' + NPUB.slice(5).toUpperCase()
      remoteList({ nostr: [upper], tags: { [upper]: ['n'] } })

      await sync(db)

      expect(feeds).toHaveLength(2)
      expect(rows.map((r) => [r.feedId, r.deletedAt, r.readListAppliedAt, r.tags])).toEqual([
        ['main', null, null, ['n']],
        ['video', null, null, ['n', 'video']],
      ])
    })

    it('leaves a video feed unsubscribed after the list alone', async () => {
      const rows = [row('main', { deletedAt: BEFORE }), row('video', { deletedAt: AFTER })]
      const db = fakeDb([mainFeed, videoFeed], rows)
      remoteList({ nostr: [NPUB] })

      await sync(db)

      expect(rows[0]).toMatchObject({ deletedAt: null })
      expect(rows[1]).toMatchObject({ deletedAt: AFTER })
    })

    it('reactivates the main feed although the video feed is still subscribed', async () => {
      const rows = [row('main', { deletedAt: BEFORE }), row('video')]
      const db = fakeDb([mainFeed, videoFeed], rows)
      remoteList({ nostr: [NPUB] })

      await sync(db)

      expect(rows[0]).toMatchObject({ deletedAt: null, readListAppliedAt: null })
      expect(rows[1]).toMatchObject({ deletedAt: null, readListAppliedAt: APPLIED, tags: ['old'] })
    })

    it('brings back only the video feed when the list keeps the videos and records the main feed as deleted', async () => {
      const feeds = [mainFeed, videoFeed]
      const rows = [row('main', { deletedAt: BEFORE }), row('video', { deletedAt: BEFORE })]
      const db = fakeDb(feeds, rows)
      remoteList({ nostr: [NPUB], deleted: [NPUB], tags: { [NPUB]: ['n', 'video'] } })

      await sync(db)

      expect(feeds).toHaveLength(2)
      expect(rows).toHaveLength(2)
      expect(rows[0]).toMatchObject({ deletedAt: BEFORE, readListAppliedAt: APPLIED, tags: ['old'] })
      expect(rows[1]).toMatchObject({ deletedAt: null, readListAppliedAt: null, tags: ['n', 'video'] })
    })

    it('does not create the main feed when the list records it as deleted and no video row can return', async () => {
      const feeds: Feed[] = []
      const rows: Row[] = []
      const db = fakeDb(feeds, rows)
      remoteList({ nostr: [NPUB], deleted: [NPUB] })

      await sync(db)

      expect(feeds).toEqual([])
      expect(rows).toEqual([])
    })

    it('brings back both feeds when the list has the npub in nostr only', async () => {
      const rows = [row('main', { deletedAt: BEFORE }), row('video', { deletedAt: BEFORE })]
      const db = fakeDb([mainFeed, videoFeed], rows)
      remoteList({ nostr: [NPUB] })

      await sync(db)

      expect(rows.map((r) => [r.feedId, r.deletedAt, r.readListAppliedAt, r.tags])).toEqual([
        ['main', null, null, []],
        ['video', null, null, ['video']],
      ])
    })

    it('keeps the main feed unsubscribed when the list records the npub as deleted', async () => {
      const rows = [row('main', { deletedAt: BEFORE }), row('video')]
      const db = fakeDb([mainFeed, videoFeed], rows)
      remoteList({ nostr: [NPUB], deleted: [NPUB] })

      await sync(db)

      expect(rows[0]).toMatchObject({ deletedAt: BEFORE })
    })
  })

  it('retries the list on the next sync when a revive batch fails', async () => {
    const errors = vi.spyOn(console, 'error').mockImplementation(() => {})
    const feeds: Feed[] = []
    const rows: Row[] = []
    const urls: string[] = []
    for (let i = 0; i < 700; i++) {
      const url = `https://f${i}.example/feed.xml`
      urls.push(url)
      feeds.push(rss(`f${i}`, url))
      rows.push(row(`f${i}`, { deletedAt: BEFORE }))
    }
    const db = fakeDb(feeds, rows)
    let prefs: Record<string, unknown> | null = null
    db.userPreference.findUnique.mockImplementation(async () => prefs as never)
    db.userPreference.upsert.mockImplementation(async (args: { create?: object; update: object }) => {
      prefs = prefs ? { ...prefs, ...args.update } : { ...args.create }
      return args
    })
    const updateMany = db.subscription.updateMany.getMockImplementation()!
    db.subscription.updateMany
      .mockImplementationOnce(updateMany)
      .mockImplementationOnce(async () => { throw new Error('connection reset') })
    remoteList({ rss: urls })

    await sync(db)

    expect(rows.filter((r) => r.deletedAt === null)).toHaveLength(500)
    expect(errors).toHaveBeenCalled()
    expect(db.userPreference.upsert).toHaveBeenCalledTimes(1)
    expect(db.userPreference.upsert.mock.calls[0]![0]).toHaveProperty('update.updatedAt')
    expect(db.userPreference.upsert.mock.calls[0]![0]).not.toHaveProperty('update.lastSubscriptionSyncCreatedAt')
    expect(db.userPreference.upsert.mock.calls[0]![0]).not.toHaveProperty('create.lastSubscriptionSyncCreatedAt')

    await sync(db)

    expect(rows.every((r) => r.deletedAt === null && r.readListAppliedAt === null)).toBe(true)
    expect(prefs).toMatchObject({ lastSubscriptionSyncCreatedAt: BigInt(LIST_AT) })
  })

  it.each([
    ['too large for a Date', 1e15],
    ['fractional', LIST_AT + 0.5],
    ['a day in the future', Math.floor(Date.now() / 1000) + 86400],
  ])('ignores a list whose created_at is %s', async (_, createdAt) => {
    const errors = vi.spyOn(console, 'error').mockImplementation(() => {})
    const rows = [row('feed-a', { deletedAt: new Date(Date.now() - 60_000) })]
    const db = fakeDb([feedA], rows)
    remoteList({ rss: [URL_A] }, createdAt)

    await sync(db)

    expect(errors).not.toHaveBeenCalled()
    expect(rows[0]!.deletedAt).not.toBeNull()
    expect(db.userPreference.upsert).toHaveBeenCalledTimes(1)
    expect(db.userPreference.upsert.mock.calls[0]![0]).not.toHaveProperty('update.lastSubscriptionSyncCreatedAt')
  })
})

describe('soft-deleted subscriptions are hidden', () => {
  const caller = (db: unknown) => feedRouter.createCaller(ctx(db))

  it('from the tag list', async () => {
    const rows = [row('feed-a', { tags: ['kept'] }), row('feed-b', { tags: ['gone'], deletedAt: BEFORE })]
    const tags = await caller(fakeDb([feedA, feedB], rows)).getUserTags()
    expect(tags.map((t: { tag: string }) => t.tag)).toEqual(['kept'])
  })

  it('from category feed counts', async () => {
    const rows = [row('feed-a', { categoryId: 'c' }), row('feed-b', { categoryId: 'c', deletedAt: BEFORE })]
    const categories = await caller(fakeDb([feedA, feedB], rows, [category('c', 'News')])).getCategories()
    expect(categories).toMatchObject([{ id: 'c', feedCount: 1 }])
  })

  it('from categories with unread counts, including the uncategorized bucket', async () => {
    const rows = [
      row('feed-a', { categoryId: 'c', deletedAt: BEFORE }),
      row('feed-b', { categoryId: null, deletedAt: BEFORE }),
    ]
    const categories = await caller(fakeDb([feedA, feedB], rows, [category('c', 'News')])).getCategoriesWithUnread()
    expect(categories).toEqual([expect.objectContaining({ id: 'c', feedCount: 0 })])
  })

  it('from the feed refresh', async () => {
    const feeds = [rss('feed-a', URL_A, { isActive: true }), rss('feed-b', URL_B, { isActive: true })]
    const rows = [row('feed-a'), row('feed-b', { deletedAt: BEFORE })]

    const result = await caller(fakeDb(feeds, rows)).refreshAllFeeds({ force: true })

    expect(result).toMatchObject({ total: 1, refreshed: 1 })
    expect(fetchAndParseFeed).toHaveBeenCalledTimes(1)
    expect(fetchAndParseFeed).toHaveBeenCalledWith(URL_A)
  })
})

describe('fetchSubscriptionListFromServer created_at validation', () => {
  const now = Math.floor(Date.now() / 1000)
  function listEvent(created_at: number, rssUrl: string) {
    return {
      kind: 30404,
      pubkey: PUBKEY,
      created_at,
      tags: [['d', 'readstr-subscriptions']],
      content: JSON.stringify({ rss: [rssUrl], nostr: [] }),
      id: `id-${created_at}`,
      sig: 'sig',
    }
  }

  it.each([
    ['a day in the future', now + 86400],
    ['too large for a Date', 1e15],
    ['fractional', now - 99.5],
    ['negative', -1],
  ])('skips an event dated %s and uses the newest valid one', async (_, bad) => {
    relayEvents.push(listEvent(now - 100, URL_A), listEvent(bad, URL_B))

    const result = await actualSync.fetchSubscriptionListFromServer(PUBKEY, ['wss://relay.invalid'])

    expect(result).toMatchObject({ success: true, createdAt: now - 100, data: { rss: [URL_A] } })
  })

  it('accepts an event within the allowed clock skew', async () => {
    const skewed = now + actualSync.MAX_FUTURE_SKEW_SECONDS - 60
    relayEvents.push(listEvent(now - 100, URL_A), listEvent(skewed, URL_B))

    const result = await actualSync.fetchSubscriptionListFromServer(PUBKEY, ['wss://relay.invalid'])

    expect(result).toMatchObject({ success: true, createdAt: skewed, data: { rss: [URL_B] } })
  })
})

describe('normalizeUrlForComparison', () => {
  const norm = actualSync.normalizeUrlForComparison

  it('keeps a non-default port so feeds on different ports stay distinct', () => {
    expect(norm('https://a.example:8000/feed.xml')).not.toBe(norm('https://a.example:9000/feed.xml'))
    expect(norm('https://a.example:8000/feed.xml')).not.toBe(norm(URL_A))
    expect(norm('https://A.EXAMPLE:8000/feed.xml/')).toBe(norm('http://a.example:8000/feed.xml'))
  })

  it('treats a default port as no port', () => {
    expect(norm('https://a.example:443/feed.xml')).toBe(norm(URL_A))
    expect(norm('http://a.example:80/feed.xml')).toBe(norm(URL_A))
  })
})

describe('excludeUnsubscribedSince', () => {
  const toAdd = [
    { type: 'RSS' as const, url: 'https://A.example/feed.xml/' },
    { type: 'RSS' as const, url: URL_B },
    { type: 'NOSTR' as const, url: NPUB },
  ]

  it('drops feeds unsubscribed at or after the list and keeps the rest', () => {
    const subs = [
      { type: 'RSS' as const, url: URL_A, deletedAt: new Date(LIST_AT * 1000) },
      { type: 'RSS' as const, url: URL_B, deletedAt: BEFORE },
      { type: 'NOSTR' as const, url: NPUB.toUpperCase().replace('NPUB1', 'npub1'), deletedAt: AFTER },
    ]
    expect(actualSync.excludeUnsubscribedSince(toAdd, subs, LIST_AT)).toEqual([toAdd[1]])
  })

  it('ignores video rows and active rows', () => {
    const subs = [
      { type: 'NOSTR_VIDEO' as const, url: NPUB, deletedAt: AFTER },
      { type: 'RSS' as const, url: URL_A, deletedAt: null },
    ]
    expect(actualSync.excludeUnsubscribedSince(toAdd, subs, LIST_AT)).toEqual(toAdd)
  })
})
