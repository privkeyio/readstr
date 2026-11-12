import { beforeEach, describe, expect, it, vi } from 'vitest'
import {
  flashApiConfig,
  listFlashSubscriptions,
  KEPT,
  reconcileWithFlash,
  shouldCheckFlashOnStatus,
  stateFromFlash,
  type FlashSubscription,
} from './flash-reconcile'
import { __resetRateLimitForTests, rateLimit } from './rate-limit'

vi.spyOn(console, 'warn').mockImplementation(() => {})

const PUBKEY = 'bc'.repeat(32)
const NOW = new Date('2026-11-15T00:00:00.000Z')
const day = (n: number) => new Date(NOW.getTime() + n * 86_400_000).toISOString()
const config = { apiKey: 'key_test', baseUrl: 'https://flash.example' }

function sub(id: string, status: string, endsInDays: number, extra: Partial<FlashSubscription> = {}): FlashSubscription {
  return { id, ref: PUBKEY, status, currentPeriodEnd: day(endsInDays), canceledAt: null, cancelEffectiveDate: null, ...extra }
}

function jsonFetch(body: unknown, status = 200) {
  return vi.fn(async () => new Response(JSON.stringify(body), { status })) as unknown as typeof fetch & ReturnType<typeof vi.fn>
}

describe('stateFromFlash', () => {
  it('maps an active subscription to its real period end', () => {
    expect(stateFromFlash([sub('S1', 'active', 20)], NOW)).toEqual({
      status: 'ACTIVE',
      subscriptionEndsAt: new Date(day(20)),
      cancelledAt: null,
      flashSubscriptionId: 'S1',
    })
  })

  it('treats an end-of-period cancel as CANCELLED until the period ends', () => {
    const s = sub('S1', 'active', 20, { canceledAt: day(-1), cancelEffectiveDate: day(20) })
    expect(stateFromFlash([s], NOW)).toMatchObject({ status: 'CANCELLED', subscriptionEndsAt: new Date(day(20)) })
  })

  it('ends access at once for an immediate cancel', () => {
    const s = sub('S1', 'canceled', 20, { canceledAt: day(-1), cancelEffectiveDate: day(-1) })
    expect(stateFromFlash([s], NOW)).toMatchObject({ status: 'CANCELLED', subscriptionEndsAt: new Date(day(-1)) })
  })

  it("keeps the user's paid subscription when a foreign one with their ref expires", () => {
    const own = sub('MINE', 'active', 20)
    const spoofed = sub('SPOOF', 'expired', -1)
    expect(stateFromFlash([spoofed, own], NOW)?.flashSubscriptionId).toBe('MINE')
  })

  it('follows the new subscription after a resubscribe', () => {
    const old = sub('S1', 'canceled', -5, { canceledAt: day(-30) })
    const renewed = sub('S2', 'active', 25)
    expect(stateFromFlash([old, renewed], NOW)).toMatchObject({ status: 'ACTIVE', flashSubscriptionId: 'S2' })
  })

  it('reports the latest lapsed subscription when none grants access', () => {
    const state = stateFromFlash([sub('S1', 'expired', -40), sub('S2', 'expired', -10)], NOW)
    expect(state).toMatchObject({ status: 'EXPIRED', flashSubscriptionId: 'S2' })
  })

  it('prefers ACTIVE with grace over a CANCELLED sub ending a day later', () => {
    const active = sub('A', 'active', 10)
    const cancelled = sub('C', 'canceled', 11, { canceledAt: day(-1), cancelEffectiveDate: day(11) })
    expect(stateFromFlash([cancelled, active], NOW)?.flashSubscriptionId).toBe('A')
    expect(stateFromFlash([active, cancelled], NOW)?.flashSubscriptionId).toBe('A')
  })

  it('prefers ACTIVE on an equal access end', () => {
    const active = sub('A', 'active', 10)
    const cancelled = sub('C', 'canceled', 17, { canceledAt: day(-1), cancelEffectiveDate: day(17) })
    expect(stateFromFlash([cancelled, active], NOW)?.flashSubscriptionId).toBe('A')
  })

  it("maps Flash's trial and past_due", () => {
    expect(stateFromFlash([sub('S1', 'trial', 5)], NOW)?.status).toBe('ACTIVE')
    expect(stateFromFlash([sub('S1', 'past_due', -2)], NOW)).toMatchObject({
      status: 'PAST_DUE',
      subscriptionEndsAt: new Date(day(-2)),
    })
  })

  it('treats a cancelled past_due subscription as CANCELLED, not PAST_DUE', () => {
    const s = sub('S1', 'past_due', -2, { canceledAt: day(-1) })
    expect(stateFromFlash([s], NOW)).toMatchObject({ status: 'CANCELLED', cancelledAt: new Date(day(-1)) })
  })

  it('treats active with canceledAt but no cancelEffectiveDate as CANCELLED at the period end', () => {
    const s = sub('S1', 'active', 12, { canceledAt: day(-1) })
    expect(stateFromFlash([s], NOW)).toMatchObject({
      status: 'CANCELLED',
      subscriptionEndsAt: new Date(day(12)),
      cancelledAt: new Date(day(-1)),
    })
  })

  it('ignores subscriptions that never started', () => {
    expect(stateFromFlash([sub('S1', 'pending', 30), sub('S2', 'abandoned', 30)], NOW)).toBeNull()
    expect(stateFromFlash([], NOW)).toBeNull()
  })
})

describe('listFlashSubscriptions', () => {
  it('asks Flash by ref with the API key', async () => {
    const fetchImpl = jsonFetch({ livemode: true, subscriptions: [sub('S1', 'active', 20)] })
    const subs = await listFlashSubscriptions(config, PUBKEY, fetchImpl)
    expect(subs).toHaveLength(1)
    const [url, init] = fetchImpl.mock.calls[0] as [URL, RequestInit]
    expect(url.toString()).toBe(`https://flash.example/api/v1/external/subscriptions?ref=${PUBKEY}`)
    expect((init.headers as Record<string, string>).Authorization).toBe('Bearer key_test')
  })

  it('scopes the list to the configured service', async () => {
    const SERVICE = '11111111-1111-4111-8111-111111111111'
    const ours = sub('OURS', 'active', 20, { serviceId: SERVICE })
    const other = sub('OTHER', 'active', 30, { serviceId: '22222222-2222-4222-8222-222222222222' })
    const unscoped = sub('NONE', 'active', 30)
    const body = { subscriptions: [ours, other, unscoped] }

    const scoped = jsonFetch(body)
    const listed = await listFlashSubscriptions({ ...config, serviceId: SERVICE }, PUBKEY, scoped)
    expect(listed?.map(s => s.id)).toEqual(['OURS'])
    const url = (scoped.mock.calls[0] as [URL])[0]
    expect(url.searchParams.get('serviceId')).toBe(SERVICE)
    expect(url.searchParams.get('ref')).toBe(PUBKEY)

    const all = jsonFetch(body)
    expect((await listFlashSubscriptions(config, PUBKEY, all))?.map(s => s.id)).toEqual(['OURS', 'OTHER', 'NONE'])
    expect((all.mock.calls[0] as [URL])[0].searchParams.has('serviceId')).toBe(false)
  })

  it("drops subscriptions for another ref even if Flash returns them", async () => {
    const other = { ...sub('X', 'active', 20), ref: 'someone-else' }
    expect(await listFlashSubscriptions(config, PUBKEY, jsonFetch({ subscriptions: [other] }))).toEqual([])
  })

  it('never follows a redirect with the key', async () => {
    const fetchImpl = jsonFetch({ subscriptions: [] })
    await listFlashSubscriptions(config, PUBKEY, fetchImpl)
    expect((fetchImpl.mock.calls[0] as [URL, RequestInit])[1].redirect).toBe('error')
  })

  it('returns null for a body over the cap', async () => {
    const big = 'x'.repeat(1_100_000)
    const chunked = vi.fn(
      async () =>
        new Response(
          new ReadableStream({
            start(c) {
              c.enqueue(new TextEncoder().encode(`{"subscriptions":[],"pad":"${big}"}`))
              c.close()
            },
          })
        )
    ) as unknown as typeof fetch
    expect(await listFlashSubscriptions(config, PUBKEY, chunked)).toBeNull()
    const declared = vi.fn(
      async () => new Response('{}', { headers: { 'content-length': '2000000' } })
    ) as unknown as typeof fetch
    expect(await listFlashSubscriptions(config, PUBKEY, declared)).toBeNull()
  })

  it('drops records with non-ISO or out-of-range dates', async () => {
    const bad = ['+275760-09-13T00:00:00.000Z', '0000-01-01T00:00:00.000Z', '1', '2026-11-20', '1969-12-31T00:00:00Z']
    const subs = bad.map((d, i) => sub(`B${i}`, 'active', 0, { currentPeriodEnd: d }))
    const good = sub('G', 'active', 20, { currentPeriodEnd: '2026-12-05T00:00:00+02:00' })
    const listed = await listFlashSubscriptions(config, PUBKEY, jsonFetch({ subscriptions: [...subs, good] }))
    expect(listed?.map(s => s.id)).toEqual(['G'])
  })

  it('keeps good records when another is malformed', async () => {
    const body = { subscriptions: [{ id: 'X', ref: PUBKEY, status: 7 }, 'junk', null, sub('S1', 'active', 20)] }
    const listed = await listFlashSubscriptions(config, PUBKEY, jsonFetch(body))
    expect(listed?.map(s => s.id)).toEqual(['S1'])
  })

  it('returns null when Flash fails or answers nonsense', async () => {
    expect(await listFlashSubscriptions(config, PUBKEY, jsonFetch({ error: 'no' }, 401))).toBeNull()
    expect(await listFlashSubscriptions(config, PUBKEY, jsonFetch({ subscriptions: 'x' }))).toBeNull()
    const throwing = vi.fn(async () => {
      throw new Error('timeout')
    }) as unknown as typeof fetch
    expect(await listFlashSubscriptions(config, PUBKEY, throwing)).toBeNull()
  })
})

describe('flashApiConfig', () => {
  it('needs both a key and an https checkout URL', () => {
    const checkout = 'https://flash.example/subscriptions/signup/a/b'
    expect(flashApiConfig({ FLASH_API_KEY: 'k', FLASH_CHECKOUT_URL: checkout } as never)).toEqual({
      apiKey: 'k',
      baseUrl: 'https://flash.example',
    })
    expect(
      flashApiConfig({ FLASH_API_KEY: 'k', FLASH_CHECKOUT_URL: checkout, FLASH_SERVICE_ID: ' svc ' } as never)
    ).toEqual({ apiKey: 'k', baseUrl: 'https://flash.example', serviceId: 'svc' })
    expect(flashApiConfig({ FLASH_CHECKOUT_URL: checkout } as never)).toBeNull()
    expect(flashApiConfig({ FLASH_API_KEY: 'k' } as never)).toBeNull()
    expect(flashApiConfig({ FLASH_API_KEY: 'k', FLASH_CHECKOUT_URL: 'http://flash.example/x' } as never)).toBeNull()
  })
})

describe('reconcileWithFlash', () => {
  it('writes the state Flash implies', async () => {
    const upsert = vi.fn(async () => ({}))
    const fetchImpl = jsonFetch({ subscriptions: [sub('S1', 'active', 20)] })
    const state = await reconcileWithFlash({ userSubscription: { upsert } } as never, PUBKEY, NOW, { config, fetchImpl })
    expect(state).toMatchObject({ status: 'ACTIVE' })
    const args = (upsert.mock.calls[0] as unknown as [{ update: Record<string, unknown> }])[0]
    expect(args.update).toMatchObject({ status: 'ACTIVE', flashSubscriptionId: 'S1', flashEventAt: NOW })
  })

  it("writes nothing when the triggering subscription isn't usable in Flash's list yet", async () => {
    const upsert = vi.fn()
    const db = { userSubscription: { upsert } } as never
    const lagging = jsonFetch({ subscriptions: [sub('OLD', 'expired', -5), sub('NEW', 'pending', 30)] })
    expect(await reconcileWithFlash(db, PUBKEY, NOW, { config, fetchImpl: lagging, subscriptionId: 'NEW' })).toBeNull()
    const missing = jsonFetch({ subscriptions: [sub('OLD', 'expired', -5)] })
    expect(await reconcileWithFlash(db, PUBKEY, NOW, { config, fetchImpl: missing, subscriptionId: 'NEW' })).toBeNull()
    expect(upsert).not.toHaveBeenCalled()
  })

  it('does not end a running local trial for a foreign canceled sub', async () => {
    const upsert = vi.fn()
    const findUnique = vi.fn(async () => ({
      status: 'TRIAL',
      trialEndsAt: new Date(day(3)),
      subscriptionEndsAt: null,
      flashSubscriptionId: null,
    }))
    const fetchImpl = jsonFetch({ subscriptions: [sub('SPOOF', 'canceled', -1, { canceledAt: day(-2), cancelEffectiveDate: day(-1) })] })
    const db = { userSubscription: { upsert, findUnique } } as never
    expect(await reconcileWithFlash(db, PUBKEY, NOW, { config, fetchImpl })).toBe(KEPT)
    expect(upsert).not.toHaveBeenCalled()
  })

  it("keeps a hand-linked active row whose sub ?ref= doesn't return", async () => {
    const upsert = vi.fn()
    const findUnique = vi.fn(async () => ({
      status: 'ACTIVE',
      trialEndsAt: new Date(day(-40)),
      subscriptionEndsAt: new Date(day(20)),
      flashSubscriptionId: 'LINKED',
    }))
    const fetchImpl = jsonFetch({ subscriptions: [sub('SPOOF', 'expired', -3)] })
    const db = { userSubscription: { upsert, findUnique } } as never
    expect(await reconcileWithFlash(db, PUBKEY, NOW, { config, fetchImpl })).toBe(KEPT)
    expect(upsert).not.toHaveBeenCalled()
  })

  it("ends access when the row's own linked sub is listed as lapsed", async () => {
    const upsert = vi.fn(async () => ({}))
    const findUnique = vi.fn(async () => ({
      status: 'ACTIVE',
      trialEndsAt: new Date(day(-40)),
      subscriptionEndsAt: new Date(day(20)),
      flashSubscriptionId: 'S1',
    }))
    const fetchImpl = jsonFetch({ subscriptions: [sub('S1', 'expired', -1)] })
    const db = { userSubscription: { upsert, findUnique } } as never
    expect(await reconcileWithFlash(db, PUBKEY, NOW, { config, fetchImpl })).toMatchObject({ status: 'EXPIRED' })
    expect(upsert).toHaveBeenCalledOnce()
  })

  it('runs overlapping calls for one user in call order', async () => {
    const upsert = vi.fn(async () => ({}))
    const db = { userSubscription: { upsert } } as never
    let releaseFirst!: () => void
    const firstGate = new Promise<void>(resolve => (releaseFirst = resolve))
    const events: string[] = []
    const body = (id: string) => JSON.stringify({ subscriptions: [sub(id, 'active', 20)] })
    const fetchImpl = vi.fn(async () => {
      const n = fetchImpl.mock.calls.length
      events.push(`fetch${n}`)
      if (n === 1) await firstGate
      return new Response(body(n === 1 ? 'FIRST' : 'SECOND'))
    }) as unknown as typeof fetch & ReturnType<typeof vi.fn>

    const first = reconcileWithFlash(db, PUBKEY, NOW, { config, fetchImpl })
    const second = reconcileWithFlash(db, PUBKEY, NOW, { config, fetchImpl })
    await new Promise(resolve => setTimeout(resolve, 10))
    expect(events).toEqual(['fetch1'])
    releaseFirst()
    await Promise.all([first, second])

    expect(events).toEqual(['fetch1', 'fetch2'])
    const ids = (upsert.mock.calls as unknown as [{ update: { flashSubscriptionId: string } }][]).map(
      c => c[0].update.flashSubscriptionId
    )
    expect(ids).toEqual(['FIRST', 'SECOND'])
  })

  it("doesn't let a failed call block the next one for the same user", async () => {
    const upsert = vi.fn(async () => {
      throw new Error('db down')
    })
    const fetchImpl = jsonFetch({ subscriptions: [sub('S1', 'active', 20)] })
    const failing = { userSubscription: { upsert } } as never
    await expect(reconcileWithFlash(failing, PUBKEY, NOW, { config, fetchImpl })).rejects.toThrow('db down')
    const ok = vi.fn(async () => ({}))
    const db = { userSubscription: { upsert: ok } } as never
    expect(await reconcileWithFlash(db, PUBKEY, NOW, { config, fetchImpl })).toMatchObject({ flashSubscriptionId: 'S1' })
    expect(ok).toHaveBeenCalledOnce()
  })

  it('does nothing without a config or when Flash has nothing usable', async () => {
    const upsert = vi.fn()
    const fetchImpl = jsonFetch({ subscriptions: [] })
    const db = { userSubscription: { upsert } } as never
    expect(await reconcileWithFlash(db, PUBKEY, NOW, { config: null, fetchImpl })).toBeNull()
    expect(fetchImpl).not.toHaveBeenCalled()
    expect(await reconcileWithFlash(db, PUBKEY, NOW, { config, fetchImpl })).toBeNull()
    expect(upsert).not.toHaveBeenCalled()
  })
})

describe('shouldCheckFlashOnStatus', () => {
  beforeEach(() => __resetRateLimitForTests())
  const row = (status: string, endsInDays: number) => ({
    userPubkey: PUBKEY,
    status,
    trialEndsAt: new Date(day(-30)),
    subscriptionEndsAt: new Date(day(endsInDays)),
  })

  it('skips rows with access that are not about to end', () => {
    expect(shouldCheckFlashOnStatus(row('ACTIVE', 10), NOW, rateLimit)).toBe(false)
  })

  it('checks lapsed or nearly-ended rows, once per window per user', () => {
    expect(shouldCheckFlashOnStatus(row('EXPIRED', -3), NOW, rateLimit)).toBe(true)
    expect(shouldCheckFlashOnStatus(row('EXPIRED', -3), NOW, rateLimit)).toBe(false)
    const soon = new Date(NOW.getTime() + 90_000)
    expect(shouldCheckFlashOnStatus(row('EXPIRED', -3), soon, rateLimit)).toBe(false)
    const later = new Date(NOW.getTime() + 121_000)
    expect(shouldCheckFlashOnStatus(row('EXPIRED', -3), later, rateLimit)).toBe(true)
    const other = { ...row('CANCELLED', 0.5), userPubkey: 'cd'.repeat(32) }
    expect(shouldCheckFlashOnStatus(other, NOW, rateLimit)).toBe(true)
  })

  it("doesn't let a user refused by their own window spend the global budget", () => {
    const t = NOW.getTime()
    expect(shouldCheckFlashOnStatus(row('EXPIRED', -3), NOW, rateLimit)).toBe(true)
    for (let i = 0; i < 10; i++) shouldCheckFlashOnStatus(row('EXPIRED', -3), new Date(t + i * 1000), rateLimit)
    // 29 of the 30 global tokens are still there for others.
    const others = Array.from({ length: 30 }, (_, i) => ({ ...row('EXPIRED', -3), userPubkey: String(i).padStart(64, '0') }))
    expect(others.filter(r => shouldCheckFlashOnStatus(r, NOW, rateLimit)).length).toBe(29)
  })
})
