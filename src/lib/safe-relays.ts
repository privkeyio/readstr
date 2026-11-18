import { lookup } from 'dns/promises'
import { isIP } from 'net'
import { WebSocket as UndiciWebSocket } from 'undici'
import { getNostrFetcher, NostrFeedFetcher } from './nostr-fetcher'
import { CONTROL_OR_SPACE } from './relays'
import { isBlockedAddress, safeDispatcher } from './safe-fetch'

export const MAX_REQUEST_RELAYS = 10
const LOOKUP_TIMEOUT_MS = 2000

// Connects through the same blocked-address lookup as safeFetch, so a host
// that passed the pre-check but rebinds to a private address before the
// socket opens is still refused. Cast because undici's event typings differ
// from the DOM WebSocket type nostr-tools expects; the runtime API matches.
export const SafeRelayWebSocket = class extends UndiciWebSocket {
  constructor(url: string | URL) {
    super(url, { dispatcher: safeDispatcher })
  }
} as unknown as typeof WebSocket

function parseRelayUrl(raw: string): URL | null {
  if (!raw || CONTROL_OR_SPACE.test(raw)) return null
  let url: URL
  try {
    url = new URL(raw)
  } catch {
    return null
  }
  if (url.protocol !== 'wss:') return null
  if (url.username || url.password) return null
  return url
}

async function resolvesToSafeAddress(hostname: string): Promise<boolean> {
  if (isIP(hostname)) return !isBlockedAddress(hostname)
  let timer: ReturnType<typeof setTimeout> | undefined
  try {
    const addresses = await Promise.race([
      lookup(hostname, { all: true }),
      new Promise<never>((_, reject) => {
        timer = setTimeout(() => reject(new Error('DNS lookup timed out')), LOOKUP_TIMEOUT_MS)
      }),
    ])
    return addresses.length > 0 && !addresses.some((a) => isBlockedAddress(a.address))
  } catch {
    return false
  } finally {
    clearTimeout(timer)
  }
}

// Client-supplied relays make the server open sockets, so drop anything that
// is not a public wss:// endpoint before it reaches the pool.
export async function filterSafeRelays(relays: readonly string[]): Promise<string[]> {
  const candidates: URL[] = []
  const seen = new Set<string>()
  for (const raw of relays) {
    const url = parseRelayUrl(raw)
    if (!url || seen.has(url.href)) continue
    seen.add(url.href)
    candidates.push(url)
  }

  const verdicts = await Promise.all(
    candidates.map((url) => resolvesToSafeAddress(url.hostname.replace(/^\[|\]$/g, ''))),
  )
  return candidates
    .filter((_, i) => verdicts[i])
    .slice(0, MAX_REQUEST_RELAYS)
    .map((url) => url.href)
}

// Runs fn against a fetcher for the client's relays, falling back to the
// shared fetcher when none survive validation. Only the per-request fetcher
// is closed; the shared singleton stays open for other callers.
export async function withRequestFetcher<T>(
  relays: readonly string[] | undefined,
  fn: (fetcher: NostrFeedFetcher) => Promise<T>,
): Promise<T> {
  const safe = relays?.length ? await filterSafeRelays(relays) : []
  if (safe.length === 0) return await fn(getNostrFetcher())

  const fetcher = new NostrFeedFetcher(safe, { websocketImplementation: SafeRelayWebSocket })
  try {
    return await fn(fetcher)
  } finally {
    fetcher.close()
  }
}
