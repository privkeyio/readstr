import { createHmac } from 'crypto'
import { NextRequest } from 'next/server'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

const findUnique = vi.fn()
const upsert = vi.fn()
vi.mock('@/server/db', () => ({ db: { userSubscription: { findUnique, upsert } } }))

const { POST } = await import('./route')

const SECRET = 'whsec_test'
const PUBKEY = 'bc'.repeat(32)

function delivery(event: string, subscriptionId: string, timestamp: string) {
  const body = JSON.stringify({
    event,
    timestamp,
    data: { accountId: 'a', subscriptionId, externalRef: PUBKEY },
  })
  const t = Math.floor(Date.now() / 1000)
  const sig = createHmac('sha256', SECRET).update(`${t}.${body}`).digest('hex')
  return new NextRequest('http://localhost/api/webhooks/flash', {
    method: 'POST',
    headers: { 'content-type': 'application/json', 'flash-signature': `t=${t},v1=${sig}` },
    body,
  })
}

describe('POST /api/webhooks/flash', () => {
  beforeEach(() => {
    vi.stubEnv('FLASH_WEBHOOK_SECRET', SECRET)
    findUnique.mockReset()
    upsert.mockReset()
  })
  afterEach(() => {
    vi.unstubAllEnvs()
    vi.unstubAllGlobals()
  })

  it('skips an old subscription\'s expired without touching a newer paid row', async () => {
    // Cancelled S1, resubscribed as S2; S1's expiry arrives later.
    findUnique.mockResolvedValue({
      status: 'ACTIVE',
      flashSubscriptionId: 'S2',
      trialEndsAt: new Date('2026-10-01T00:00:00.000Z'),
      subscriptionEndsAt: new Date('2026-12-20T00:00:00.000Z'),
      flashEventAt: new Date('2026-11-20T00:00:00.000Z'),
    })
    const res = await POST(delivery('subscription.expired', 'S1', '2026-12-10T00:00:00.000Z'))
    expect(res.status).toBe(200)
    // No write, so flashEventAt stays put and a delayed S2 event can still apply.
    expect(upsert).not.toHaveBeenCalled()
  })

  it('keeps the paid-through date when canceled arrives', async () => {
    findUnique.mockResolvedValue({
      status: 'ACTIVE',
      flashSubscriptionId: 'S1',
      trialEndsAt: new Date('2026-10-01T00:00:00.000Z'),
      subscriptionEndsAt: new Date('2026-12-11T00:00:00.000Z'),
      flashEventAt: new Date('2026-11-11T00:00:00.000Z'),
    })
    const res = await POST(delivery('subscription.canceled', 'S1', '2026-11-12T00:00:00.000Z'))
    expect(res.status).toBe(200)
    const { update } = upsert.mock.calls[0]![0]
    expect(update.status).toBe('CANCELLED')
    expect('subscriptionEndsAt' in update).toBe(false)
  })

  it("writes Flash's own state when an API key is set, whatever the event says", async () => {
    vi.stubEnv('FLASH_API_KEY', 'key_test')
    vi.stubEnv('FLASH_CHECKOUT_URL', 'https://flash.example/subscriptions/signup/a/b')
    const fetchMock = vi.fn(async () =>
      Response.json({
        subscriptions: [
          { id: 'S1', ref: PUBKEY, status: 'expired', currentPeriodEnd: '2026-11-10T00:00:00.000Z' },
          { id: 'S2', ref: PUBKEY, status: 'active', currentPeriodEnd: '2099-01-01T00:00:00.000Z' },
        ],
      })
    )
    vi.stubGlobal('fetch', fetchMock)
    findUnique.mockResolvedValue(null)

    // A late `expired` for the old S1 must not end the paid S2.
    const res = await POST(delivery('subscription.expired', 'S1', '2026-11-10T00:00:00.000Z'))
    expect(res.status).toBe(200)
    expect(fetchMock).toHaveBeenCalledOnce()
    const { update } = upsert.mock.calls[0]![0]
    expect(update).toMatchObject({ status: 'ACTIVE', flashSubscriptionId: 'S2' })
    expect(upsert).toHaveBeenCalledOnce()
  })

  it("applies an activated event Flash's list doesn't reflect yet", async () => {
    vi.stubEnv('FLASH_API_KEY', 'key_test')
    vi.stubEnv('FLASH_CHECKOUT_URL', 'https://flash.example/subscriptions/signup/a/b')
    const fetchMock = vi.fn(async () =>
      Response.json({
        subscriptions: [
          { id: 'S1', ref: PUBKEY, status: 'expired', currentPeriodEnd: '2026-10-01T00:00:00.000Z' },
          { id: 'S2', ref: PUBKEY, status: 'pending', currentPeriodEnd: null },
        ],
      })
    )
    vi.stubGlobal('fetch', fetchMock)
    findUnique.mockResolvedValue({
      status: 'EXPIRED',
      flashSubscriptionId: 'S1',
      trialEndsAt: new Date('2026-08-01T00:00:00.000Z'),
      subscriptionEndsAt: new Date('2026-10-01T00:00:00.000Z'),
      flashEventAt: new Date('2026-10-01T00:00:00.000Z'),
    })

    const res = await POST(delivery('subscription.activated', 'S2', new Date().toISOString()))
    expect(res.status).toBe(200)
    expect(fetchMock).toHaveBeenCalledOnce()
    expect(upsert).toHaveBeenCalledOnce()
    expect(upsert.mock.calls[0]![0].update).toMatchObject({ status: 'ACTIVE', flashSubscriptionId: 'S2' })
  })

  it("doesn't end a running local trial for a foreign subscription's cancel", async () => {
    vi.stubEnv('FLASH_API_KEY', 'key_test')
    vi.stubEnv('FLASH_CHECKOUT_URL', 'https://flash.example/subscriptions/signup/a/b')
    const now = Date.now()
    const iso = (days: number) => new Date(now + days * 86_400_000).toISOString()
    vi.stubGlobal(
      'fetch',
      vi.fn(async () =>
        Response.json({
          subscriptions: [
            { id: 'SPOOF', ref: PUBKEY, status: 'canceled', currentPeriodEnd: iso(-1), canceledAt: iso(-1), cancelEffectiveDate: iso(-1) },
          ],
        })
      )
    )
    findUnique.mockResolvedValue({
      status: 'TRIAL',
      flashSubscriptionId: null,
      trialEndsAt: new Date(now + 3 * 86_400_000),
      subscriptionEndsAt: null,
      flashEventAt: null,
    })

    const res = await POST(delivery('subscription.canceled', 'SPOOF', new Date(now).toISOString()))
    expect(res.status).toBe(200)
    expect(upsert).not.toHaveBeenCalled()
  })

  it('falls back to applying the event when Flash cannot be reached', async () => {
    vi.stubEnv('FLASH_API_KEY', 'key_test')
    vi.stubEnv('FLASH_CHECKOUT_URL', 'https://flash.example/subscriptions/signup/a/b')
    vi.stubGlobal('fetch', vi.fn(async () => new Response('down', { status: 503 })))
    vi.spyOn(console, 'warn').mockImplementation(() => {})
    findUnique.mockResolvedValue(null)

    const res = await POST(delivery('subscription.activated', 'S1', '2026-11-12T00:00:00.000Z'))
    expect(res.status).toBe(200)
    expect(upsert.mock.calls[0]![0].update).toMatchObject({ status: 'ACTIVE', flashSubscriptionId: 'S1' })
  })
})
