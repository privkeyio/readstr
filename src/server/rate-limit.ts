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
const MAX_BUCKETS = 10_000

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
  windowMs: number,
  now: number = Date.now()
): RateLimitResult {
  prune(now)

  const key = `${name}:${identifier}`
  const state = buckets.get(key)

  if (!state || state.resetAt <= now) {
    // Hard cap the map to bound memory under key-flooding. When full and the key
    // is new, deny rather than grow unbounded.
    if (!state && buckets.size >= MAX_BUCKETS) {
      return {
        allowed: false,
        retryAfterSeconds: Math.max(1, Math.ceil(windowMs / 1000)),
      }
    }
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
 * Extract a best-effort client IP from proxy headers. The app sits behind a
 * single trusted Caddy proxy that OVERWRITES x-forwarded-for with the real peer
 * IP at the trust boundary (see Caddyfile), so the only trustworthy hop is the
 * rightmost entry — earlier entries would be client-controlled and must not be
 * trusted. x-real-ip is intentionally ignored: it is client-settable and only
 * ever reaches the app if the proxy is bypassed. When x-forwarded-for is absent
 * we fall back to a constant so such requests share a single conservative bucket
 * rather than bypassing the limit.
 */
export function clientIpFromHeaders(
  getHeader: (name: string) => string | undefined
): string {
  const forwardedFor = getHeader('x-forwarded-for')
  if (forwardedFor) {
    const parts = forwardedFor.split(',')
    const last = parts[parts.length - 1]?.trim()
    if (last) return last
  }
  return 'unknown'
}

/**
 * Adapter for the Fetch `Request` used by the route handlers.
 */
export function clientIpFromRequest(request: Request): string {
  return clientIpFromHeaders(name => request.headers.get(name) ?? undefined)
}
