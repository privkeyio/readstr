import { lookup } from 'dns/promises'
import { isIP } from 'net'
import { Agent, WebSocket as UndiciWebSocket } from 'undici'
import { getNostrFetcher, NostrFeedFetcher } from './nostr-fetcher'
import { CONTROL_OR_SPACE } from './relays'
import { blockedAddressLookup, isBlockedAddress } from './safe-fetch'

export const MAX_REQUEST_RELAYS = 10
const LOOKUP_TIMEOUT_MS = 2000
// undici's default is 128 MB per message, and permessage-deflate inflates up
// to it; relay events are a few KB.
const MAX_RELAY_PAYLOAD_BYTES = 1024 * 1024

// Separate from safeFetch's dispatcher so the payload cap only applies to
// relay sockets.
const relayDispatcher = new Agent({
  connect: { lookup: blockedAddressLookup },
  webSocket: { maxPayloadSize: MAX_RELAY_PAYLOAD_BYTES },
})

// Connects through the same blocked-address lookup as safeFetch, so a host
// that passed the pre-check but rebinds to a private address before the
// socket opens is still refused. Node skips lookup for IP literals, so those
// are checked here.
class SafeRelaySocket extends UndiciWebSocket {
  constructor(url: string | URL) {
    const host = new URL(url).hostname.replace(/^\[|\]$/g, '')
    if (isIP(host) && isBlockedAddress(host)) throw new Error('Relay resolves to a disallowed address')
    super(url, { dispatcher: relayDispatcher })
  }
}

// Cast because undici's event typings differ from the DOM WebSocket type
// nostr-tools expects; the runtime API matches.
export const SafeRelayWebSocket = SafeRelaySocket as unknown as typeof WebSocket

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

// Relays differing only in query, hash or a trailing-dot hostname reach the
// same endpoint, so they count once.
function relayKey(url: URL): string {
  const host = url.hostname.replace(/\.$/, '')
  return `${url.protocol}//${host}${url.port ? `:${url.port}` : ''}${url.pathname}`
}

// Client-supplied relays make the server open sockets, so drop anything that
// is not a public wss:// endpoint before it reaches the pool. Candidates are
// capped before resolving because each lookup (here and again at connect
// time) occupies a getaddrinfo threadpool slot.
export async function filterSafeRelays(relays: readonly string[]): Promise<string[]> {
  const candidates: URL[] = []
  const seen = new Set<string>()
  for (const raw of relays) {
    if (candidates.length >= MAX_REQUEST_RELAYS) break
    const url = parseRelayUrl(raw)
    if (!url) continue
    const key = relayKey(url)
    if (seen.has(key)) continue
    seen.add(key)
    candidates.push(url)
  }

  const verdicts = await Promise.all(
    candidates.map((url) => resolvesToSafeAddress(url.hostname.replace(/^\[|\]$/g, ''))),
  )
  return candidates.filter((_, i) => verdicts[i]).map((url) => url.href)
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

  // nostr-tools only closes OPEN sockets and its connect timeout never closes
  // the socket, so a relay stalling the handshake would outlive the request.
  // Track every socket this call creates and close them all ourselves.
  const sockets = new Set<SafeRelaySocket>()
  const TrackedSocket = class extends SafeRelaySocket {
    constructor(url: string | URL) {
      super(url)
      sockets.add(this)
    }
  }
  const fetcher = new NostrFeedFetcher(safe, {
    websocketImplementation: TrackedSocket as unknown as typeof WebSocket,
  })
  try {
    return await fn(fetcher)
  } finally {
    // Cleanup failures must not mask fn's result or error.
    try {
      fetcher.close()
    } catch {}
    for (const ws of sockets) {
      try {
        ws.close()
      } catch {}
    }
  }
}
