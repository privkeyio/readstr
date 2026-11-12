import { beforeEach, describe, expect, it, vi } from 'vitest'
import {
  flashApiConfig,
  listFlashSubscriptions,
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

  it("drops subscriptions for another ref even if Flash returns them", async () => {
    const other = { ...sub('X', 'active', 20), ref: 'someone-else' }
    expect(await listFlashSubscriptions(config, PUBKEY, jsonFetch({ subscriptions: [other] }))).toEqual([])
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
    expect(state?.status).toBe('ACTIVE')
    const args = (upsert.mock.calls[0] as unknown as [{ update: Record<string, unknown> }])[0]
    expect(args.update).toMatchObject({ status: 'ACTIVE', flashSubscriptionId: 'S1', flashEventAt: NOW })
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
    const other = { ...row('CANCELLED', 0.5), userPubkey: 'cd'.repeat(32) }
    expect(shouldCheckFlashOnStatus(other, NOW, rateLimit)).toBe(true)
  })
})
