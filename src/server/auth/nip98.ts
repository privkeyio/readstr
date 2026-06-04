import { createHash } from 'crypto'
import { verifyEvent, type Event as NostrToolsEvent } from 'nostr-tools'

/**
 * NIP-98 (kind 27235) HTTP Auth verification.
 *
 * Verifies a signed Nostr event presented in the `Authorization: Nostr <base64>`
 * header and returns the authenticated hex pubkey, or null on any failure.
 *
 * This is the *only* trustworthy source of the caller's identity. The previous
 * `x-nostr-pubkey` plaintext header was forgeable and must not be trusted.
 */

const NIP98_KIND = 27235

// Allowed clock skew between the signed `created_at` and server time, in seconds.
// Rejects both stale and future-dated events.
const TIMESTAMP_TOLERANCE_SECONDS = 60

/**
 * Compare two URLs for NIP-98 `u`-tag matching.
 *
 * We do NOT require a byte-for-byte string match. tRPC v10 GET requests batch
 * multiple procedures into a single URL and serialize input into the query
 * string (e.g. `/api/trpc/feed.getFeeds,feed.getStatus?batch=1&input=...`).
 * The client signs the exact fetch URL, but we want to be robust to:
 *   - http vs https (reconstructed from x-forwarded-proto behind Caddy),
 *   - host/port differences introduced by the reverse proxy,
 *   - query-parameter ordering.
 *
 * Tradeoff: we match on pathname + the *sorted* set of query parameters, and we
 * ignore scheme/host. This is slightly looser than a full-URL match but is the
 * tightest comparison that survives a reverse proxy reliably. The signature
 * binds the event to this exact path + query input. For GET requests the input
 * lives in the query string, so it is covered by the `u` tag. For mutations the
 * body is NOT in the URL, so a captured header could otherwise be replayed with
 * a different body within the clock-skew window — the `payload` tag (see
 * verifyNip98Header) closes that gap by binding sha256(body) into the signature.
 */
function urlTagMatches(signedUrl: string, requestUrl: string): boolean {
  try {
    // Use a dummy base so relative URLs (just in case) still parse.
    const a = new URL(signedUrl, 'http://localhost')
    const b = new URL(requestUrl, 'http://localhost')

    if (a.pathname !== b.pathname) return false

    const sortParams = (u: URL) => {
      const params = [...u.searchParams.entries()]
      params.sort(([k1, v1], [k2, v2]) =>
        k1 === k2 ? v1.localeCompare(v2) : k1.localeCompare(k2)
      )
      return JSON.stringify(params)
    }

    return sortParams(a) === sortParams(b)
  } catch {
    return false
  }
}

function decodeAuthEvent(authHeader: string): NostrToolsEvent | null {
  const match = /^Nostr\s+(.+)$/i.exec(authHeader.trim())
  if (!match || !match[1]) return null

  try {
    const json = Buffer.from(match[1], 'base64').toString('utf8')
    const parsed = JSON.parse(json) as unknown
    if (
      parsed &&
      typeof parsed === 'object' &&
      'kind' in parsed &&
      'sig' in parsed &&
      'pubkey' in parsed
    ) {
      return parsed as NostrToolsEvent
    }
    return null
  } catch {
    return null
  }
}

function getTag(event: NostrToolsEvent, name: string): string | undefined {
  const tag = event.tags.find(t => t[0] === name)
  return tag?.[1]
}

// Single-use tracking of consumed event ids to prevent replay within the
// acceptance window. An id is only valid until its signed `created_at` falls
// out of tolerance, after which the timestamp check rejects it regardless, so
// we can safely evict entries once they expire.
const seenEventIds = new Map<string, number>()

function pruneSeen(now: number): void {
  for (const [id, expiresAt] of seenEventIds) {
    if (expiresAt <= now) seenEventIds.delete(id)
  }
}

// Returns true if the id was already consumed; otherwise records it.
function consumeEventId(id: string, now: number): boolean {
  pruneSeen(now)
  if (seenEventIds.has(id)) return true
  seenEventIds.set(id, now + TIMESTAMP_TOLERANCE_SECONDS)
  return false
}

/**
 * Verify a NIP-98 Authorization header.
 *
 * @returns the verified hex pubkey on success, or null on any failure.
 *          Fail-closed: never throws.
 */
export async function verifyNip98Header(
  authHeader: string | undefined,
  opts: { url: string; method: string; body?: string | null }
): Promise<string | null> {
  try {
    if (!authHeader) return null

    const event = decodeAuthEvent(authHeader)
    if (!event) return null

    // 1. Correct kind.
    if (event.kind !== NIP98_KIND) return null

    // 2. Valid signature + event id.
    if (!verifyEvent(event)) return null

    // 3. Timestamp within tolerance (reject stale/future).
    const now = Math.floor(Date.now() / 1000)
    if (Math.abs(now - event.created_at) > TIMESTAMP_TOLERANCE_SECONDS) {
      return null
    }

    // 4. Method tag matches (when present).
    const methodTag = getTag(event, 'method')
    if (methodTag && methodTag.toUpperCase() !== opts.method.toUpperCase()) {
      return null
    }

    // 5. `u` tag matches the request URL (path + sorted query).
    const uTag = getTag(event, 'u')
    if (!uTag) return null
    if (!urlTagMatches(uTag, opts.url)) return null

    // 6. Body binding via `payload` tag (NIP-98). For non-GET methods the body
    // is not covered by the `u` tag, so we require the signature to commit to
    // sha256(rawBody). When present, it must match; for mutations it is
    // mandatory so a captured header cannot be replayed against a new body.
    const method = opts.method.toUpperCase()
    const payloadTag = getTag(event, 'payload')
    const isMutation = method !== 'GET' && method !== 'HEAD'
    if (payloadTag || isMutation) {
      if (!payloadTag) return null
      const body = opts.body ?? ''
      const digest = createHash('sha256').update(body, 'utf8').digest('hex')
      if (digest !== payloadTag.toLowerCase()) return null
    }

    // 7. Single-use: reject an event id we have already accepted in-window.
    if (consumeEventId(event.id, now)) return null

    // All checks passed; the pubkey is now trustworthy.
    return event.pubkey
  } catch {
    return null
  }
}
