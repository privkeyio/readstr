import { createHmac } from 'crypto'
import { describe, expect, it, vi } from 'vitest'
import { changeFor, TOLERANCE_SECONDS, verifyFlashWebhook, type FlashEvent } from './flash-webhook'

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

  it('keeps the period on past_due, ends access at cancel time, and expires', () => {
    expect(changeFor(verified(body('subscription.past_due')))).toEqual({ status: 'PAST_DUE' })
    const canceledAt = new Date('2026-11-10T11:00:00.000Z')
    expect(changeFor(verified(body('subscription.canceled', { canceledAt: canceledAt.toISOString() })))).toEqual({
      status: 'CANCELLED',
      subscriptionEndsAt: canceledAt,
      cancelledAt: canceledAt,
    })
    expect(changeFor(verified(body('subscription.expired'))).status).toBe('EXPIRED')
  })
})
