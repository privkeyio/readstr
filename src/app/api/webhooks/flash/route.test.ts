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
  afterEach(() => vi.unstubAllEnvs())

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
})
