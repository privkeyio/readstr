import { beforeEach, describe, expect, it } from 'vitest'
import {
  __rateLimitBucketCountForTests,
  __resetRateLimitForTests,
  clientIpFromHeaders,
  clientIpFromRequest,
  rateLimit,
} from './rate-limit'

beforeEach(() => {
  __resetRateLimitForTests()
})

describe('rateLimit', () => {
  it('allows calls under the limit', () => {
    const r1 = rateLimit('ep', 'ip', 3, 1000, 0)
    const r2 = rateLimit('ep', 'ip', 3, 1000, 0)
    expect(r1.allowed).toBe(true)
    expect(r2.allowed).toBe(true)
    expect(r1.retryAfterSeconds).toBe(0)
    expect(r2.retryAfterSeconds).toBe(0)
  })

  it('allows exactly the Nth call and denies the N+1th at the limit boundary', () => {
    const limit = 3
    for (let i = 0; i < limit; i++) {
      expect(rateLimit('ep', 'ip', limit, 1000, 0).allowed).toBe(true)
    }
    const denied = rateLimit('ep', 'ip', limit, 1000, 0)
    expect(denied.allowed).toBe(false)
  })

  it('reports retryAfterSeconds as the ceil of remaining window seconds', () => {
    rateLimit('ep', 'ip', 1, 10_000, 0)
    // First call consumed the only slot at t=0; deny at t=2500 leaves 7500ms.
    const denied = rateLimit('ep', 'ip', 1, 10_000, 2500)
    expect(denied.allowed).toBe(false)
    expect(denied.retryAfterSeconds).toBe(8)
  })

  it('rounds retryAfterSeconds up to a minimum of 1 second', () => {
    rateLimit('ep', 'ip', 1, 1000, 0)
    const denied = rateLimit('ep', 'ip', 1, 1000, 999)
    expect(denied.allowed).toBe(false)
    expect(denied.retryAfterSeconds).toBe(1)
  })

  it('resets the count when the window expires (injected now, no fake timers)', () => {
    const limit = 2
    rateLimit('ep', 'ip', limit, 1000, 0)
    rateLimit('ep', 'ip', limit, 1000, 0)
    expect(rateLimit('ep', 'ip', limit, 1000, 0).allowed).toBe(false)

    // At t = windowMs the window has elapsed (resetAt <= now), so the count resets.
    const afterReset = rateLimit('ep', 'ip', limit, 1000, 1000)
    expect(afterReset.allowed).toBe(true)
    expect(rateLimit('ep', 'ip', limit, 1000, 1000).allowed).toBe(true)
    expect(rateLimit('ep', 'ip', limit, 1000, 1000).allowed).toBe(false)
  })

  it('scopes limits independently per name and identifier', () => {
    expect(rateLimit('a', 'ip', 1, 1000, 0).allowed).toBe(true)
    expect(rateLimit('a', 'ip', 1, 1000, 0).allowed).toBe(false)
    expect(rateLimit('b', 'ip', 1, 1000, 0).allowed).toBe(true)
    expect(rateLimit('a', 'other', 1, 1000, 0).allowed).toBe(true)
  })

  it('denies new keys when MAX_BUCKETS is full but keeps serving existing keys', () => {
    const MAX_BUCKETS = 10_000
    // Constant now keeps prune from firing (now - lastPrune < PRUNE_INTERVAL_MS),
    // so the map fills to exactly MAX_BUCKETS.
    for (let i = 0; i < MAX_BUCKETS; i++) {
      expect(rateLimit('ep', `ip-${i}`, 5, 1000, 1000).allowed).toBe(true)
    }
    expect(__rateLimitBucketCountForTests()).toBe(MAX_BUCKETS)

    const newKey = rateLimit('ep', 'ip-new', 5, 1000, 1000)
    expect(newKey.allowed).toBe(false)
    expect(newKey.retryAfterSeconds).toBe(1)
    expect(__rateLimitBucketCountForTests()).toBe(MAX_BUCKETS)

    // Existing keys are still served while the map is full.
    expect(rateLimit('ep', 'ip-0', 5, 1000, 1000).allowed).toBe(true)
  })

  it('prunes expired windows once the prune interval elapses', () => {
    // Seed a short window at t=1000; resetAt = 1100. lastPrune stays 0 because
    // 1000 - 0 < PRUNE_INTERVAL_MS, so this key is not yet pruned.
    rateLimit('ep', 'stale', 5, 100, 1000)
    expect(__rateLimitBucketCountForTests()).toBe(1)

    // At t=65000 the prune interval (60s) has elapsed, so the expired 'stale'
    // window is evicted before the new key is recorded.
    rateLimit('ep', 'fresh', 5, 100, 65_000)
    expect(__rateLimitBucketCountForTests()).toBe(1)
  })
})

describe('clientIpFromHeaders', () => {
  const fromXff = (value: string | undefined) =>
    clientIpFromHeaders(name => (name === 'x-forwarded-for' ? value : undefined))

  it('selects the rightmost entry for a multi-hop XFF chain', () => {
    expect(fromXff('a, b')).toBe('b')
    expect(fromXff('client, proxy1, proxy2')).toBe('proxy2')
  })

  it('returns the single value when there is one hop', () => {
    expect(fromXff('203.0.113.5')).toBe('203.0.113.5')
  })

  it('trims surrounding whitespace on the selected entry', () => {
    expect(fromXff('a ,  b  ')).toBe('b')
  })

  it('falls through to unknown on a trailing comma / empty rightmost segment', () => {
    expect(fromXff('a, b,')).toBe('unknown')
    expect(fromXff('a,  ')).toBe('unknown')
  })

  it('returns unknown when XFF is absent (no x-real-ip fallback)', () => {
    expect(fromXff(undefined)).toBe('unknown')
    // x-real-ip must be ignored even when present.
    expect(
      clientIpFromHeaders(name => (name === 'x-real-ip' ? '198.51.100.7' : undefined))
    ).toBe('unknown')
  })

  it('returns unknown for an empty XFF string', () => {
    expect(fromXff('')).toBe('unknown')
  })
})

describe('clientIpFromRequest', () => {
  it('extracts the rightmost XFF entry from a Fetch Request', () => {
    const req = new Request('https://example.test', {
      headers: { 'x-forwarded-for': 'client, proxy' },
    })
    expect(clientIpFromRequest(req)).toBe('proxy')
  })

  it('returns unknown when the request has no XFF header', () => {
    const req = new Request('https://example.test')
    expect(clientIpFromRequest(req)).toBe('unknown')
  })
})
