import { describe, expect, it } from 'vitest'
import { nip19 } from 'nostr-tools'
import { accessFor, buildCheckoutUrl, GRACE_DAYS, normalizePubkey, trialEndFrom } from './subscription'

const HEX = '3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d'
const NPUB = nip19.npubEncode(HEX)
const NOW = new Date('2026-11-10T12:00:00Z')
const DAY = 24 * 60 * 60 * 1000
const at = (days: number) => new Date(NOW.getTime() + days * DAY)

describe('accessFor', () => {
  it('grants a trial until its stored end, then denies it', () => {
    const row = { status: 'TRIAL', trialEndsAt: trialEndFrom(NOW), subscriptionEndsAt: null }
    expect(accessFor(row, NOW)).toEqual({ hasAccess: true, daysRemaining: 7 })
    expect(accessFor(row, at(8)).hasAccess).toBe(false)
  })

  it('keeps ACTIVE and PAST_DUE access through the grace window, not beyond', () => {
    for (const status of ['ACTIVE', 'PAST_DUE']) {
      const row = { status, trialEndsAt: at(-40), subscriptionEndsAt: at(-1) }
      expect(accessFor(row, NOW)).toEqual({ hasAccess: true, daysRemaining: 0 })
      expect(accessFor(row, at(GRACE_DAYS)).hasAccess).toBe(false)
    }
  })

  it('keeps CANCELLED access only until its end date, with no grace', () => {
    expect(accessFor({ status: 'CANCELLED', trialEndsAt: at(-40), subscriptionEndsAt: at(3) }, NOW).hasAccess).toBe(true)
    expect(accessFor({ status: 'CANCELLED', trialEndsAt: at(-40), subscriptionEndsAt: NOW }, NOW).hasAccess).toBe(false)
  })

  it('never grants EXPIRED', () => {
    expect(accessFor({ status: 'EXPIRED', trialEndsAt: at(-40), subscriptionEndsAt: at(10) }, NOW).hasAccess).toBe(false)
  })
})

describe('normalizePubkey', () => {
  it('accepts hex (what NIP-98 yields) and npub, returning lowercase hex', () => {
    expect(normalizePubkey(HEX)).toBe(HEX)
    expect(normalizePubkey(HEX.toUpperCase())).toBe(HEX)
    expect(normalizePubkey(NPUB)).toBe(HEX)
  })

  it('returns null instead of throwing on garbage', () => {
    expect(normalizePubkey('not-a-key')).toBeNull()
    expect(normalizePubkey('')).toBeNull()
  })
})

describe('buildCheckoutUrl', () => {
  it('adds the hex pubkey as ref on a path-only signup URL', () => {
    const base = 'https://flash.example/subscriptions/signup/svc/plan'
    expect(buildCheckoutUrl(base, NPUB)).toBe(`${base}?ref=${HEX}`)
  })

  it('keeps an existing query string', () => {
    const url = new URL(buildCheckoutUrl('https://flash.example/signup?x=1', HEX))
    expect(url.searchParams.get('x')).toBe('1')
    expect(url.searchParams.get('ref')).toBe(HEX)
  })

  it('falls back to the bare URL without a usable identity', () => {
    expect(buildCheckoutUrl('https://flash.example/x', null)).toBe('https://flash.example/x')
    expect(buildCheckoutUrl('https://flash.example/x', 'junk')).toBe('https://flash.example/x')
    expect(buildCheckoutUrl('', HEX)).toBe('#')
  })
})
