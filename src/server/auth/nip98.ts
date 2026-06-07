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
 * Hostnames the signed `u` tag is allowed to target.
 *
 * Sourced from trusted config (NIP98_ALLOWED_HOSTS, comma-separated) — never
 * from the proxied request Host header, which an attacker controls. When
 * NIP98_ALLOWED_HOSTS is set, only those hosts are honored. When it is unset, a
 * default is used: the canonical production host in production, or localhost +
 * 127.0.0.1 outside production so dev keeps working without accepting an
 * attacker-minted localhost-origin token replayed against the real API.
 *
 * Scope: this rejects tokens whose signed `u` host is not in the trusted
 * allow-list (e.g. a foreign-origin token replayed here). It does NOT prevent a
 * malicious page served from an allowed origin from minting a valid token — that
 * is inherent to NIP-98 with auto-approving signers and is out of scope here.
 */
function normalizeHost(entry: string): string {
  const trimmed = entry.trim().toLowerCase()
  const withScheme = /^[a-z][a-z0-9+.-]*:\/\//.test(trimmed)
    ? trimmed
    : `http://${trimmed}`
  try {
    return new URL(withScheme).hostname
  } catch {
    return trimmed
  }
}

let allowedHosts: Set<string> | undefined

function getAllowedHosts(): Set<string> {
  if (allowedHosts) return allowedHosts
  const hosts = new Set<string>()
  const configured = process.env.NIP98_ALLOWED_HOSTS
  if (configured) {
    for (const h of configured.split(',')) {
      const trimmed = h.trim()
      if (trimmed) hosts.add(normalizeHost(trimmed))
    }
  }
  if (hosts.size === 0) {
    if (process.env.NODE_ENV === 'production') {
      hosts.add('readstr.privkey.io')
    } else {
      hosts.add('localhost')
      hosts.add('127.0.0.1')
    }
  }
  allowedHosts = hosts
  return hosts
}

/**
 * Compare two URLs for NIP-98 `u`-tag matching.
 *
 * We do NOT require a byte-for-byte string match. tRPC v10 GET requests batch
 * multiple procedures into a single URL and serialize input into the query
 * string (e.g. `/api/trpc/feed.getFeeds,feed.getStatus?batch=1&input=...`).
 * The client signs the exact fetch URL, but we want to be robust to:
 *   - http vs https (reconstructed from x-forwarded-proto behind Caddy),
 *   - port differences introduced by the reverse proxy,
 *   - query-parameter ordering.
 *
 * Tradeoff: we match on pathname + the *sorted* set of query parameters, and we
 * ignore scheme/port. The signed host, however, IS validated against a trusted
 * allow-list (getAllowedHosts) so a token whose `u` points at a foreign or
 * localhost origin cannot be replayed against this server. We deliberately do
 * not compare the request URL's host here — that comes from the proxy and is not
 * trustworthy. The signature binds the event to this exact path + query input.
 * For GET requests the input lives in the query string, so it is covered by the
 * `u` tag. For mutations the body is NOT in the URL, so a captured header could
 * otherwise be replayed with a different body within the clock-skew window — the
 * `payload` tag (see verifyNip98Header) closes that gap by binding sha256(body)
 * into the signature.
 */
/**
 * Does `u` carry its own authority/host, or does it inherit the host from the
 * base it is resolved against?
 *
 * We must NOT use "is it an absolute (scheme-bearing) URL" for this: scheme-less
 * authority forms (`//evil.com/x`, `\\evil.com/x`, `/\evil.com/x`, plus leading
 * whitespace / embedded-tab variants) carry a foreign host yet fail a bare
 * `new URL(u)` parse — that gap reopened a cross-origin token-minting bypass.
 *
 * Instead we resolve `u` against two distinct dummy bases with different hosts.
 * If the resolved hostname differs between them, `u` inherited the host (it is
 * genuinely host-less / same-origin). If the hostname is identical across both
 * bases, `u` supplies its own host and must be validated against the allow-list.
 */
function carriesOwnHost(url: string): { ownHost: boolean; hostname: string } {
  const a = new URL(url, 'http://base-a.invalid')
  const b = new URL(url, 'http://base-b.invalid')
  return { ownHost: a.hostname === b.hostname, hostname: a.hostname }
}

function urlTagMatches(signedUrl: string, requestUrl: string): boolean {
  try {
    // Dummy base so a host-less `u` parses for path/query comparison.
    const a = new URL(signedUrl, 'http://localhost')
    const b = new URL(requestUrl, 'http://localhost')

    // A `u` that supplies its own authority/host (absolute OR scheme-less
    // `//host`, `\\host`, etc.) must be validated against the trusted allow-list
    // so a foreign-origin token cannot be replayed here. A genuinely host-less
    // (same-origin) `u` inherits the dummy base host and is exempt.
    const carries = carriesOwnHost(signedUrl)
    if (carries.ownHost && !getAllowedHosts().has(carries.hostname)) {
      return false
    }

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

// Returns true if the id was already consumed; otherwise records it. The entry
// is retained until the event's own acceptance window closes
// (`created_at + tolerance`) so a future-dated event cannot be evicted early and
// replayed while its timestamp is still valid.
function consumeEventId(id: string, createdAt: number, now: number): boolean {
  pruneSeen(now)
  if (seenEventIds.has(id)) return true
  seenEventIds.set(id, createdAt + TIMESTAMP_TOLERANCE_SECONDS)
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

    // 4. Method tag is required and must match.
    const methodTag = getTag(event, 'method')
    if (!methodTag || methodTag.toUpperCase() !== opts.method.toUpperCase()) {
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
    if (consumeEventId(event.id, event.created_at, now)) return null

    // All checks passed; the pubkey is now trustworthy.
    return event.pubkey
  } catch {
    return null
  }
}
