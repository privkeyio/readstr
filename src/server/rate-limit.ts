// Dependency-free, per-process fixed-window rate limiter. Used to throttle the
// public endpoints that drive outbound Nostr relay traffic. This is best-effort
// (single-process, in-memory) and resets on restart — it exists to blunt simple
// abuse/amplification, not as a hard authorization boundary.

interface WindowState {
  count: number
  resetAt: number
}

const buckets = new Map<string, WindowState>()

let lastPrune = 0
const PRUNE_INTERVAL_MS = 60_000

function prune(now: number): void {
  if (now - lastPrune < PRUNE_INTERVAL_MS) return
  lastPrune = now
  for (const [key, state] of buckets) {
    if (state.resetAt <= now) buckets.delete(key)
  }
}

export interface RateLimitResult {
  allowed: boolean
  retryAfterSeconds: number
}

/**
 * Fixed-window rate limit. `name` scopes the limit per endpoint, `identifier`
 * is typically the client IP. Returns whether the call is allowed and, when
 * not, how many seconds until the window resets.
 */
export function rateLimit(
  name: string,
  identifier: string,
  limit: number,
  windowMs: number
): RateLimitResult {
  const now = Date.now()
  prune(now)

  const key = `${name}:${identifier}`
  const state = buckets.get(key)

  if (!state || state.resetAt <= now) {
    buckets.set(key, { count: 1, resetAt: now + windowMs })
    return { allowed: true, retryAfterSeconds: 0 }
  }

  if (state.count >= limit) {
    return {
      allowed: false,
      retryAfterSeconds: Math.max(1, Math.ceil((state.resetAt - now) / 1000)),
    }
  }

  state.count++
  return { allowed: true, retryAfterSeconds: 0 }
}

/**
 * Extract a best-effort client IP from proxy headers. The app sits behind Caddy,
 * which sets x-forwarded-for / x-real-ip. Falls back to a constant so missing
 * headers share a single conservative bucket rather than bypassing the limit.
 */
export function clientIpFromHeaders(
  getHeader: (name: string) => string | undefined
): string {
  const forwardedFor = getHeader('x-forwarded-for')
  if (forwardedFor) {
    const first = forwardedFor.split(',')[0]?.trim()
    if (first) return first
  }
  const realIp = getHeader('x-real-ip')?.trim()
  if (realIp) return realIp
  return 'unknown'
}
