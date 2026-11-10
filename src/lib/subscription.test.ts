import { describe, expect, it } from 'vitest'
import { nip19 } from 'nostr-tools'
import { accessFor, buildCheckoutUrl, normalizePubkey, trialEndFrom } from './subscription'

const HEX = '3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d'
const NPUB = nip19.npubEncode(HEX)
const NOW = new Date('2026-11-10T12:00:00Z')
const DAY = 24 * 60 * 60 * 1000

function decodeParams(url: string) {
  const raw = new URL(url).searchParams.get('params')
  return JSON.parse(Buffer.from(raw ?? '', 'base64').toString())
}

describe('accessFor', () => {
  it('grants a trial until its stored end, then denies it', () => {
    const row = { status: 'TRIAL', trialEndsAt: trialEndFrom(NOW), subscriptionEndsAt: null }
    expect(accessFor(row, NOW)).toEqual({ hasAccess: true, daysRemaining: 7 })
    expect(accessFor(row, new Date(NOW.getTime() + 8 * DAY)).hasAccess).toBe(false)
  })

  it('uses the paid period over the trial end for active rows', () => {
    const row = {
      status: 'ACTIVE',
      trialEndsAt: new Date(NOW.getTime() - DAY),
      subscriptionEndsAt: new Date(NOW.getTime() + 30 * DAY),
    }
    expect(accessFor(row, NOW)).toEqual({ hasAccess: true, daysRemaining: 30 })
  })

  it('denies statuses other than TRIAL and ACTIVE', () => {
    const row = { status: 'EXPIRED', trialEndsAt: trialEndFrom(NOW), subscriptionEndsAt: null }
    expect(accessFor(row, NOW).hasAccess).toBe(false)
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
  it('pre-fills a hex pubkey on a path-only signup URL', () => {
    const base = 'https://flash.example/subscriptions/signup/svc/plan'
    const url = buildCheckoutUrl(base, HEX)
    expect(url.startsWith(`${base}?params=`)).toBe(true)
    expect(decodeParams(url)).toEqual({ npub: HEX, external_uuid: HEX, is_verified: true })
  })

  it('keeps an existing query string', () => {
    const url = buildCheckoutUrl('https://flash.example/subscription-page?flashId=3238', NPUB)
    expect(new URL(url).searchParams.get('flashId')).toBe('3238')
    expect(decodeParams(url).npub).toBe(HEX)
  })

  it('falls back to the bare URL without a usable identity', () => {
    expect(buildCheckoutUrl('https://flash.example/x', null)).toBe('https://flash.example/x')
    expect(buildCheckoutUrl('https://flash.example/x', 'junk')).toBe('https://flash.example/x')
    expect(buildCheckoutUrl('', HEX)).toBe('#')
  })
})
