import { createHmac } from 'crypto'
import { describe, expect, it, vi } from 'vitest'
import {
  changeFor,
  isForeignSubscriptionEvent,
  TOLERANCE_SECONDS,
  verifyFlashWebhook,
  type FlashEvent,
} from './flash-webhook'

vi.spyOn(console, 'warn').mockImplementation(() => {})
vi.spyOn(console, 'error').mockImplementation(() => {})

const SECRET = 'whsec_test'
const HEX = '3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d'
const NOW = 1_800_000_000

function body(event: string, data: Record<string, unknown> = {}) {
  return JSON.stringify({
    event,
    timestamp: '2026-11-10T12:00:00.000Z',
    data: { accountId: 'acc', subscriptionId: 'sub_1', externalRef: HEX, ...data },
  })
}

function sign(raw: string, t = NOW, secret = SECRET) {
  return `t=${t},v1=${createHmac('sha256', secret).update(`${t}.${raw}`).digest('hex')}`
}

function verified(raw: string): FlashEvent {
  const result = verifyFlashWebhook(sign(raw), raw, SECRET, NOW)
  if (!result.ok || !('event' in result)) throw new Error(`not verified: ${JSON.stringify(result)}`)
  return result.event
}

describe('verifyFlashWebhook', () => {
  it('accepts a correctly signed delivery and reads externalRef as the pubkey', () => {
    const event = verified(body('subscription.activated'))
    expect(event).toMatchObject({ event: 'subscription.activated', subscriptionId: 'sub_1', userPubkey: HEX })
  })

  it('rejects a tampered body, a wrong secret, and a missing or malformed header', () => {
    const raw = body('subscription.activated')
    const tampered = raw.replace(HEX, 'f'.repeat(64))
    expect(verifyFlashWebhook(sign(raw), tampered, SECRET, NOW)).toMatchObject({ ok: false, status: 400 })
    expect(verifyFlashWebhook(sign(raw, NOW, 'whsec_other'), raw, SECRET, NOW)).toMatchObject({ ok: false })
    expect(verifyFlashWebhook(null, raw, SECRET, NOW)).toMatchObject({ ok: false, error: 'bad signature' })
    expect(verifyFlashWebhook('t=abc,v1=zz', raw, SECRET, NOW)).toMatchObject({ ok: false, error: 'bad signature' })
  })

  it('rejects stale and far-future timestamps (replay window)', () => {
    const raw = body('subscription.activated')
    const old = NOW - TOLERANCE_SECONDS - 1
    const future = NOW + TOLERANCE_SECONDS + 1
    expect(verifyFlashWebhook(sign(raw, old), raw, SECRET, NOW)).toMatchObject({ ok: false, error: 'stale' })
    expect(verifyFlashWebhook(sign(raw, future), raw, SECRET, NOW)).toMatchObject({ ok: false, error: 'stale' })
  })

  it('accepts when any of several v1 signatures matches (secret rotation)', () => {
    const raw = body('subscription.renewed')
    const header = `t=${NOW},v1=${'0'.repeat(64)},${sign(raw).split(',')[1]}`
    expect(verifyFlashWebhook(header, raw, SECRET, NOW)).toMatchObject({ ok: true })
  })

  it('fails closed without a configured secret', () => {
    const raw = body('subscription.activated')
    expect(verifyFlashWebhook(sign(raw), raw, undefined, NOW)).toMatchObject({ ok: false, status: 500 })
  })

  it('acknowledges signed events it does not handle', () => {
    const raw = body('checkout.completed')
    expect(verifyFlashWebhook(sign(raw), raw, SECRET, NOW)).toMatchObject({ ok: true, ignored: expect.any(String) })
  })

  it('yields a null pubkey when externalRef is missing or not a key', () => {
    expect(verified(body('subscription.activated', { externalRef: undefined })).userPubkey).toBeNull()
    expect(verified(body('subscription.activated', { externalRef: 'user-42' })).userPubkey).toBeNull()
  })
})

describe('changeFor', () => {
  it('activates and renews for a 30-day period from the payment time', () => {
    expect(changeFor(verified(body('subscription.activated', { activatedAt: '2026-11-01T00:00:00.000Z' })))).toEqual({
      status: 'ACTIVE',
      subscriptionEndsAt: new Date('2026-12-01T00:00:00.000Z'),
      cancelledAt: null,
    })
    expect(changeFor(verified(body('subscription.renewed', { paidAt: '2026-12-01T00:00:00.000Z' }))).subscriptionEndsAt).toEqual(
      new Date('2026-12-31T00:00:00.000Z')
    )
  })

  it('keeps the paid period on past_due and canceled, and expires', () => {
    expect(changeFor(verified(body('subscription.past_due')))).toEqual({ status: 'PAST_DUE' })
    // Flash sends `canceled` when an end-of-period cancel is requested, so the
    // paid-through date must survive it; `expired` ends access later.
    const canceledAt = new Date('2026-11-10T11:00:00.000Z')
    const canceled = changeFor(verified(body('subscription.canceled', { canceledAt: canceledAt.toISOString() })))
    expect(canceled).toEqual({ status: 'CANCELLED', cancelledAt: canceledAt })
    expect('subscriptionEndsAt' in canceled).toBe(false)
    expect(changeFor(verified(body('subscription.expired'))).status).toBe('EXPIRED')
  })
})

describe('isForeignSubscriptionEvent', () => {
  const NOW = new Date('2026-11-15T00:00:00.000Z')
  const days = (n: number) => new Date(NOW.getTime() + n * 86_400_000)
  const row = (status: string, flashSubscriptionId: string | null, endsInDays: number | null) => ({
    status,
    flashSubscriptionId,
    trialEndsAt: days(-30),
    subscriptionEndsAt: endsInDays === null ? null : days(endsInDays),
  })
  const ev = (event: string, subscriptionId: string) =>
    ({ event, subscriptionId, occurredAt: NOW }) as Pick<FlashEvent, 'event' | 'subscriptionId' | 'occurredAt'>

  it('applies events from the linked subscription', () => {
    expect(isForeignSubscriptionEvent(row('CANCELLED', 'S1', 10), ev('subscription.expired', 'S1'))).toBe(false)
  })

  it('ignores another subscription\'s events while the row still grants access', () => {
    // Cancel S1, resubscribe as S2: S1's later expiry must not end S2.
    const resubscribed = row('ACTIVE', 'S2', 25)
    for (const e of ['subscription.expired', 'subscription.canceled', 'subscription.past_due', 'subscription.renewed']) {
      expect(isForeignSubscriptionEvent(resubscribed, ev(e, 'S1'))).toBe(true)
    }
  })

  it('lets activated relink a live row to a new subscription', () => {
    expect(isForeignSubscriptionEvent(row('CANCELLED', 'S1', 10), ev('subscription.activated', 'S2'))).toBe(false)
  })

  it('lets renewed relink a cancelled row that still grants access, but not other events', () => {
    // S1 cancelled with time left; S2's activated was dropped, so its renewal must land.
    const cancelled = row('CANCELLED', 'S1', 10)
    expect(isForeignSubscriptionEvent(cancelled, ev('subscription.renewed', 'S2'))).toBe(false)
    for (const e of ['subscription.expired', 'subscription.canceled', 'subscription.past_due']) {
      expect(isForeignSubscriptionEvent(cancelled, ev(e, 'S2'))).toBe(true)
    }
  })

  it('applies any event once the linked subscription no longer grants access', () => {
    // S1's expired and S2's activated were both dropped: S2's renewals must still land.
    expect(isForeignSubscriptionEvent(row('CANCELLED', 'S1', -1), ev('subscription.renewed', 'S2'))).toBe(false)
    expect(isForeignSubscriptionEvent(row('ACTIVE', 'S1', -8), ev('subscription.renewed', 'S2'))).toBe(false)
    expect(isForeignSubscriptionEvent(row('EXPIRED', 'S1', 5), ev('subscription.renewed', 'S2'))).toBe(false)
  })

  it('applies any event to rows that are not linked', () => {
    expect(isForeignSubscriptionEvent(null, ev('subscription.expired', 'S1'))).toBe(false)
    expect(isForeignSubscriptionEvent(row('TRIAL', null, null), ev('subscription.canceled', 'S1'))).toBe(false)
  })
})
