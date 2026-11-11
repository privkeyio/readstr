// Dependency-free, per-process fixed-window rate limiter. Used to throttle the
// public endpoints that drive outbound Nostr relay traffic. This is best-effort
// (single-process, in-memory) and resets on restart — it exists to blunt simple
// abuse/amplification, not as a hard authorization boundary.

interface WindowState {
  count: number
  resetAt: number
}

/**
 * Most identifiers one limiter tracks at once. Each limiter has its own map, so
 * flooding one (say, hour-long guide.submitFeed windows) cannot crowd out the
 * others. An entry is roughly 100-200 bytes, so a full limiter costs ~10 MB.
 */
export const MAX_KEYS_PER_LIMITER = 50_000

const limiters = new Map<string, Map<string, WindowState>>()

/**
 * Drop expired windows from the front of a limiter's map. A window is
 * re-inserted at the end whenever it restarts, and every caller uses a constant
 * window length per limiter name, so the map stays ordered by resetAt and the
 * scan can stop at the first live window. That keeps pruning cheap enough to run
 * on every call, even while an attacker holds the map full.
 */
function prune(buckets: Map<string, WindowState>, now: number): void {
  for (const [key, state] of buckets) {
    if (state.resetAt > now) return
    buckets.delete(key)
  }
}

export interface RateLimitResult {
  allowed: boolean
  retryAfterSeconds: number
}

/**
 * Fixed-window rate limit. `name` scopes the limit per endpoint, `identifier`
 * is typically the client IP. `cost` is how many units this call consumes, so
 * one request can be charged for the work it fans out to; it is rounded up to
 * a whole number and anything below one (or NaN) is charged as one. Returns whether the
 * call is allowed and, when not, how many seconds until the window resets. A
 * denied call consumes nothing.
 */
export function rateLimit(
  name: string,
  identifier: string,
  limit: number,
  windowMs: number,
  now: number = Date.now(),
  cost: number = 1
): RateLimitResult {
  // A cost below one or NaN is a caller bug; charge the minimum rather than let
  // it hand budget back (negative) or poison the count (NaN). An infinite cost
  // stays infinite and is refused below.
  const units = cost >= 1 ? Math.ceil(cost) : 1

  let buckets = limiters.get(name)
  if (!buckets) {
    buckets = new Map()
    limiters.set(name, buckets)
  }
  prune(buckets, now)

  const state = buckets.get(identifier)

  if (!state || state.resetAt <= now) {
    if (units > limit) {
      return {
        allowed: false,
        retryAfterSeconds: Math.max(1, Math.ceil(windowMs / 1000)),
      }
    }
    // Defensive: prune normally removes an expired window before we get here,
    // but if a caller ever varies windowMs (or the clock steps back) this
    // keeps a restarted window at the end of the map, ordered by resetAt.
    buckets.delete(identifier)
    // Bound memory under key flooding by evicting the oldest window instead of
    // refusing new keys. Refusing would let ~MAX_KEYS_PER_LIMITER distinct
    // addresses lock every new visitor out. Evicting can reset a live window,
    // which only hands that client its budget back early; a flooder holding
    // that many addresses already has that many budgets, and each request is
    // still capped by the route's per-request limits.
    if (buckets.size >= MAX_KEYS_PER_LIMITER) {
      const oldest = buckets.keys().next().value
      if (oldest !== undefined) buckets.delete(oldest)
    }
    buckets.set(identifier, { count: units, resetAt: now + windowMs })
    return { allowed: true, retryAfterSeconds: 0 }
  }

  if (state.count + units > limit) {
    return {
      allowed: false,
      retryAfterSeconds: Math.max(1, Math.ceil((state.resetAt - now) / 1000)),
    }
  }

  state.count += units
  return { allowed: true, retryAfterSeconds: 0 }
}

/**
 * Extract a best-effort client IP from proxy headers. The app sits behind a
 * single trusted Caddy proxy that OVERWRITES x-forwarded-for with the real peer
 * IP at the trust boundary (see Caddyfile), so the only trustworthy hop is the
 * rightmost entry — earlier entries would be client-controlled and must not be
 * trusted. x-real-ip is intentionally ignored: it is client-settable and only
 * ever reaches the app if the proxy is bypassed. When x-forwarded-for is absent
 * we fall back to a constant so such requests share a single conservative bucket
 * rather than bypassing the limit. The address is then reduced to its rate
 * limit key by rateLimitKeyForIp, so an IPv6 client is limited per /64.
 */
export function clientIpFromHeaders(
  getHeader: (name: string) => string | undefined
): string {
  const forwardedFor = getHeader('x-forwarded-for')
  if (forwardedFor) {
    const parts = forwardedFor.split(',')
    const last = parts[parts.length - 1]?.trim()
    if (last) return rateLimitKeyForIp(last)
  }
  return 'unknown'
}

const IPV4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/
const HEX_GROUP = /^[0-9a-f]{1,4}$/i

function parseIpv4(value: string): number[] | null {
  const match = IPV4.exec(value)
  if (!match) return null
  const octets = match.slice(1).map(Number)
  return octets.every(o => o <= 255) ? octets : null
}

function parseIpv6Groups(parts: string[]): number[] | null {
  const groups: number[] = []
  for (let i = 0; i < parts.length; i++) {
    const part = parts[i]
    if (i === parts.length - 1 && part.includes('.')) {
      const octets = parseIpv4(part)
      if (!octets) return null
      groups.push((octets[0] << 8) | octets[1], (octets[2] << 8) | octets[3])
    } else if (HEX_GROUP.test(part)) {
      groups.push(parseInt(part, 16))
    } else {
      return null
    }
  }
  return groups
}

/** Parse an IPv6 address into its eight 16-bit groups, or null if invalid. */
function parseIpv6(value: string): number[] | null {
  const halves = value.split('::')
  if (halves.length > 2) return null
  const head = halves[0] ? halves[0].split(':') : []
  const tail = halves.length === 2 && halves[1] ? halves[1].split(':') : []
  const headGroups = parseIpv6Groups(head)
  // An embedded IPv4 address may only end the address, never precede '::'.
  if (!headGroups || (halves.length === 2 && head.some(p => p.includes('.')))) return null
  const tailGroups = parseIpv6Groups(tail)
  if (!tailGroups) return null
  if (halves.length === 1) return headGroups.length === 8 ? headGroups : null
  const missing = 8 - headGroups.length - tailGroups.length
  if (missing < 1) return null
  return [...headGroups, ...new Array<number>(missing).fill(0), ...tailGroups]
}

/**
 * Map a client IP to the key it is rate limited under. One IPv6 host is
 * normally handed a whole /64, so keying on the full address would give it
 * billions of separate budgets; IPv6 addresses are therefore keyed on their /64
 * prefix. IPv4-mapped IPv6 (::ffff:a.b.c.d) is keyed as the IPv4 address so a
 * dual-stack listener cannot split one client into two budgets. IPv4 is keyed
 * as is. Brackets, a trailing port and a zone id are stripped first. Anything
 * that does not parse is returned unchanged, which still limits it, just
 * per exact string.
 */
export function rateLimitKeyForIp(raw: string): string {
  let value = raw.trim()
  if (value.startsWith('[')) {
    const close = value.indexOf(']')
    if (close === -1) return raw
    const rest = value.slice(close + 1)
    if (rest !== '' && !/^:\d+$/.test(rest)) return raw
    value = value.slice(1, close)
  } else if (/^[\d.]+:\d+$/.test(value)) {
    value = value.slice(0, value.indexOf(':'))
  }
  if (!value.includes(':')) return parseIpv4(value) ? value : raw

  const zone = value.indexOf('%')
  if (zone !== -1) value = value.slice(0, zone)
  const groups = parseIpv6(value)
  if (!groups) return raw

  if (groups.slice(0, 5).every(g => g === 0) && groups[5] === 0xffff) {
    return [groups[6] >> 8, groups[6] & 0xff, groups[7] >> 8, groups[7] & 0xff].join('.')
  }
  return `${groups.slice(0, 4).map(g => g.toString(16)).join(':')}::/64`
}

/**
 * Adapter for the Fetch `Request` used by the route handlers.
 */
export function clientIpFromRequest(request: Request): string {
  return clientIpFromHeaders(name => request.headers.get(name) ?? undefined)
}

// Test-only seam: clears the module-level fixed-window state so each test starts
// from a clean slate. Not part of the runtime API.
export function __resetRateLimitForTests(): void {
  if (process.env.NODE_ENV === 'production') return
  limiters.clear()
}

// Test-only seam: number of live windows currently tracked, used to assert prune
// eviction. Not part of the runtime API.
export function __rateLimitBucketCountForTests(): number {
  if (process.env.NODE_ENV === 'production') return 0
  let total = 0
  for (const buckets of limiters.values()) total += buckets.size
  return total
}
