import { beforeEach, describe, expect, it } from 'vitest'
import {
  MAX_KEYS_PER_LIMITER,
  __rateLimitBucketCountForTests,
  __resetRateLimitForTests,
  clientIpFromHeaders,
  clientIpFromRequest,
  rateLimit,
  rateLimitKeyForIp,
} from './rate-limit'

beforeEach(() => {
  __resetRateLimitForTests()
})

describe('rateLimit', () => {
  it('charges weighted calls and refuses one that would overrun the limit', () => {
    expect(rateLimit('ep', 'ip', 10, 1000, 0, 6).allowed).toBe(true)
    expect(rateLimit('ep', 'ip', 10, 1000, 0, 5).allowed).toBe(false)
    expect(rateLimit('ep', 'ip', 10, 1000, 0, 4).allowed).toBe(true)
    expect(rateLimit('ep', 'ip', 10, 1000, 0, 1).allowed).toBe(false)
    expect(rateLimit('other', 'ip', 10, 1000, 0, 11).allowed).toBe(false)
  })

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

  it('ceils a sub-second remaining window up to 1 second', () => {
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

  it('evicts the oldest window instead of locking out new keys when a limiter is full', () => {
    for (let i = 0; i < MAX_KEYS_PER_LIMITER; i++) {
      expect(rateLimit('trpc', `ip-${i}`, 5, 60_000, 1000).allowed).toBe(true)
    }
    expect(__rateLimitBucketCountForTests()).toBe(MAX_KEYS_PER_LIMITER)

    // A legitimate new visitor is still served, and memory stays capped.
    expect(rateLimit('trpc', 'legit', 5, 60_000, 1000).allowed).toBe(true)
    expect(__rateLimitBucketCountForTests()).toBe(MAX_KEYS_PER_LIMITER)

    // The oldest window made room; a newer one keeps its count.
    expect(rateLimit('trpc', `ip-${MAX_KEYS_PER_LIMITER - 1}`, 5, 60_000, 1000, 5).allowed).toBe(
      false
    )
  })

  it('keeps limiters isolated so an hour-long flood on one cannot affect another', () => {
    for (let i = 0; i < MAX_KEYS_PER_LIMITER; i++) {
      rateLimit('guide.submitFeed', `ip-${i}`, 5, 60 * 60 * 1000, 0)
    }
    expect(rateLimit('trpc', 'legit', 1, 60_000, 0).allowed).toBe(true)
    expect(rateLimit('trpc', 'legit', 1, 60_000, 0).allowed).toBe(false)
    expect(__rateLimitBucketCountForTests()).toBe(MAX_KEYS_PER_LIMITER + 1)
  })

  it('restarts an expired window and keeps the oldest-first order', () => {
    rateLimit('ep', 'first', 5, 1000, 0)
    rateLimit('ep', 'second', 5, 1000, 500)
    // 'first' expires at 1000 and restarts, so it now ends after 'second'.
    rateLimit('ep', 'first', 5, 1000, 1000)
    // At 1500 'second' has expired and is pruned; 'first' is still live.
    rateLimit('ep', 'third', 5, 1000, 1500)
    expect(__rateLimitBucketCountForTests()).toBe(2)
    expect(rateLimit('ep', 'first', 1, 1000, 1500).allowed).toBe(false)
  })

  it('prunes expired windows on the next call', () => {
    rateLimit('ep', 'stale', 5, 100, 1000)
    expect(__rateLimitBucketCountForTests()).toBe(1)

    rateLimit('ep', 'fresh', 5, 100, 1100)
    expect(__rateLimitBucketCountForTests()).toBe(1)
  })

  it('charges a NaN, negative or fractional cost as at least one whole unit', () => {
    expect(rateLimit('ep', 'ip', 3, 1000, 0, Number.NaN).allowed).toBe(true)
    expect(rateLimit('ep', 'ip', 3, 1000, 0, -5).allowed).toBe(true)
    expect(rateLimit('ep', 'ip', 3, 1000, 0, Number.NEGATIVE_INFINITY).allowed).toBe(true)
    expect(rateLimit('ep', 'ip', 3, 1000, 0, 0).allowed).toBe(false)
    expect(rateLimit('ep', 'ip', 3, 1000, 0, Number.NaN).allowed).toBe(false)

    expect(rateLimit('frac', 'ip', 2, 1000, 0, 1.5).allowed).toBe(true)
    expect(rateLimit('frac', 'ip', 2, 1000, 0, 0.5).allowed).toBe(false)
  })

  it('refuses an infinite cost', () => {
    expect(rateLimit('ep', 'ip', 3, 1000, 0, Number.POSITIVE_INFINITY).allowed).toBe(false)
    expect(rateLimit('ep', 'ip', 3, 1000, 0, 3).allowed).toBe(true)
  })
})

describe('rateLimitKeyForIp', () => {
  it('keeps IPv4 addresses as they are', () => {
    expect(rateLimitKeyForIp('203.0.113.5')).toBe('203.0.113.5')
  })

  it('drops a port from IPv4', () => {
    expect(rateLimitKeyForIp('203.0.113.5:4433')).toBe('203.0.113.5')
  })

  it('keys IPv6 on its /64 prefix', () => {
    expect(rateLimitKeyForIp('2001:db8:1:2:aaaa:bbbb:cccc:dddd')).toBe('2001:db8:1:2::/64')
    expect(rateLimitKeyForIp('2001:db8:1:2::1')).toBe('2001:db8:1:2::/64')
    expect(rateLimitKeyForIp('2001:DB8:1:2::ffff')).toBe('2001:db8:1:2::/64')
  })

  it('expands :: compression wherever it appears', () => {
    expect(rateLimitKeyForIp('2001:db8::1')).toBe('2001:db8:0:0::/64')
    expect(rateLimitKeyForIp('2001:0db8:0000:0000::1')).toBe('2001:db8:0:0::/64')
    expect(rateLimitKeyForIp('::1')).toBe('0:0:0:0::/64')
    expect(rateLimitKeyForIp('::')).toBe('0:0:0:0::/64')
    expect(rateLimitKeyForIp('fe80::')).toBe('fe80:0:0:0::/64')
    expect(rateLimitKeyForIp('1:2:3:4:5:6:7::')).toBe('1:2:3:4::/64')
  })

  it('strips brackets, ports and zone ids', () => {
    expect(rateLimitKeyForIp('[2001:db8:1:2::1]')).toBe('2001:db8:1:2::/64')
    expect(rateLimitKeyForIp('[2001:db8:1:2::1]:443')).toBe('2001:db8:1:2::/64')
    expect(rateLimitKeyForIp('fe80::1%eth0')).toBe('fe80:0:0:0::/64')
  })

  it('keys IPv4-mapped IPv6 as the IPv4 address', () => {
    expect(rateLimitKeyForIp('::ffff:203.0.113.5')).toBe('203.0.113.5')
    expect(rateLimitKeyForIp('::FFFF:cb00:7105')).toBe('203.0.113.5')
    expect(rateLimitKeyForIp('0:0:0:0:0:ffff:203.0.113.5')).toBe('203.0.113.5')
  })

  it('keys other embedded-IPv4 forms by their /64 like any IPv6 address', () => {
    expect(rateLimitKeyForIp('64:ff9b::203.0.113.5')).toBe('64:ff9b:0:0::/64')
  })

  it('returns unparseable values unchanged', () => {
    for (const raw of [
      'unknown',
      'not-an-ip',
      '1:2:3:4:5:6:7:8:9',
      '1:2:3:4:5:6:7',
      '1::2::3',
      '12345::1',
      'g::1',
      '1:2:3:4:5:6:7:8::',
      '1.2.3.4::1',
      '::ffff:1.2.3.256',
      '[2001:db8::1',
      '[2001:db8::1]x',
      '999.1.1.1',
    ]) {
      expect(rateLimitKeyForIp(raw)).toBe(raw)
    }
  })

  it('gives every address in one /64 the same budget', () => {
    expect(rateLimit('ep', rateLimitKeyForIp('2001:db8:1:2::1'), 1, 1000, 0).allowed).toBe(true)
    expect(rateLimit('ep', rateLimitKeyForIp('2001:db8:1:2::2'), 1, 1000, 0).allowed).toBe(false)
    expect(rateLimit('ep', rateLimitKeyForIp('2001:db8:1:3::1'), 1, 1000, 0).allowed).toBe(true)
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

  it('reduces an IPv6 peer to its /64', () => {
    expect(fromXff('198.51.100.1, 2001:db8:1:2::abcd')).toBe('2001:db8:1:2::/64')
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
