import { Resolver } from 'dns/promises'
import { isIP, type LookupFunction, type Socket } from 'net'
import { Agent, buildConnector, WebSocket as UndiciWebSocket } from 'undici'
import { getNostrFetcher, NostrFeedFetcher } from './nostr-fetcher'
import { CONTROL_OR_SPACE } from './relays'
import { isBlockedAddress } from './safe-fetch'

export const MAX_REQUEST_RELAYS = 10
const LOOKUP_TIMEOUT_MS = 2000
// Bounds queueing for a slot plus c-ares moving on to further nameservers.
const LOOKUP_DEADLINE_MS = 3000
// Relay lookups in flight across all requests.
const MAX_CONCURRENT_LOOKUPS = 16
// undici's default is 128 MB per message, and permessage-deflate inflates up
// to it; kind-0 profiles are a few KB.
export const MAX_RELAY_PAYLOAD_BYTES = 256 * 1024
// How long a closed relay socket gets to finish the close handshake before
// it is destroyed.
const CLOSE_GRACE_MS = 1000

// getaddrinfo runs on the libuv threadpool and cannot be cancelled, so a slow
// authoritative server would pin threads shared with fs and crypto. c-ares
// queries run off the pool and give up after the timeout. It skips
// /etc/hosts, which only makes resolution stricter.
const resolver = new Resolver({ timeout: LOOKUP_TIMEOUT_MS, tries: 1 })

let lookupsInFlight = 0
const lookupQueue: (() => void)[] = []

async function withLookupSlot<T>(fn: () => Promise<T>): Promise<T> {
  if (lookupsInFlight >= MAX_CONCURRENT_LOOKUPS) {
    await new Promise<void>((resolve) => lookupQueue.push(resolve))
  } else {
    lookupsInFlight++
  }
  try {
    return await fn()
  } finally {
    const next = lookupQueue.shift()
    if (next) next()
    else lookupsInFlight--
  }
}

// Resolves both families and fails closed unless every address returned is
// public. A family whose query fails just contributes no addresses: sockets
// only ever connect to addresses returned (and checked) here, so a broken
// AAAA answer can't hide a private address, and relays behind resolvers that
// mishandle AAAA still work. Fails closed when both families fail.
async function resolvePublicAddresses(hostname: string): Promise<{ address: string; family: 4 | 6 }[]> {
  let expired = false
  let timer: ReturnType<typeof setTimeout> | undefined
  let v4: string[], v6: string[]
  try {
    ;[v4, v6] = await Promise.race([
      withLookupSlot(async () => {
        // Skip the query if the caller gave up while queued for a slot.
        if (expired) return [[], []]
        // allSettled so the slot is held until both queries finish.
        const [a, aaaa] = await Promise.allSettled([resolver.resolve4(hostname), resolver.resolve6(hostname)])
        if (a.status === 'rejected' && aaaa.status === 'rejected') throw a.reason
        return [a.status === 'fulfilled' ? a.value : [], aaaa.status === 'fulfilled' ? aaaa.value : []]
      }),
      new Promise<never>((_, reject) => {
        timer = setTimeout(() => {
          expired = true
          reject(new Error('DNS lookup timed out'))
        }, LOOKUP_DEADLINE_MS)
      }),
    ])
  } finally {
    clearTimeout(timer)
  }
  const list = [
    ...v4.map((address) => ({ address, family: 4 as const })),
    ...v6.map((address) => ({ address, family: 6 as const })),
  ]
  if (list.length === 0 || list.some((a) => isBlockedAddress(a.address))) {
    throw new Error('Relay resolves to a disallowed address')
  }
  return list
}

// Re-resolves at connect time, so a host that passed the pre-check but
// rebinds to a private address before the socket opens is still refused.
const relayLookup: LookupFunction = (hostname, options, callback) => {
  resolvePublicAddresses(hostname).then(
    (all) => {
      const family = (options as { family?: number }).family
      const list = family === 4 || family === 6 ? all.filter((a) => a.family === family) : all
      if (list.length === 0) callback(new Error('Relay has no address in the requested family'), '', 0)
      else if ((options as { all?: boolean }).all) callback(null, list as never)
      else callback(null, list[0].address, list[0].family)
    },
    (err) => callback(err, '', 0),
  )
}

function createRelayAgent(onSocket?: (socket: Socket) => void): Agent {
  const connector = buildConnector({ lookup: relayLookup })
  return new Agent({
    // buildConnector returns the raw socket synchronously, so it is tracked
    // even while still connecting.
    connect: onSocket
      ? (opts, callback) => {
          const socket = (connector as unknown as (...args: Parameters<typeof connector>) => Socket | undefined)(
            opts,
            callback,
          )
          if (socket) onSocket(socket)
        }
      : connector,
    webSocket: { maxPayloadSize: MAX_RELAY_PAYLOAD_BYTES },
  })
}

// Separate from safeFetch's dispatcher so the payload cap and resolver only
// apply to relay sockets.
const relayDispatcher = createRelayAgent()

// URL keeps IPv6 literals bracketed ("[::1]"), which isIP rejects.
function bareHostname(url: URL): string {
  return url.hostname.replace(/^\[|\]$/g, '')
}

// Node skips lookup for IP literals, so those are checked here.
class SafeRelaySocket extends UndiciWebSocket {
  constructor(url: string | URL, dispatcher: Agent = relayDispatcher) {
    const host = bareHostname(new URL(url))
    if (isIP(host) && isBlockedAddress(host)) throw new Error('Relay resolves to a disallowed address')
    super(url, { dispatcher })
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
  try {
    await resolvePublicAddresses(hostname)
    return true
  } catch {
    return false
  }
}

// Relays differing only in query, hash or a trailing-dot hostname reach the
// same endpoint, so they count once.
function relayKey(url: URL): string {
  const host = url.hostname.replace(/\.+$/, '')
  return `${url.protocol}//${host}${url.port ? `:${url.port}` : ''}${url.pathname}`
}

// Client-supplied relays make the server open sockets, so drop anything that
// is not a public wss:// endpoint before it reaches the pool. Candidates are
// capped before resolving because each lookup (here and again at connect
// time) takes a slot under the shared lookup cap.
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

  const verdicts = await Promise.all(candidates.map((url) => resolvesToSafeAddress(bareHostname(url))))
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
  // After the upgrade undici detaches the socket from every timer, and
  // close() on an OPEN socket waits for the peer's Close frame forever. A
  // per-call agent lets us destroy the raw sockets once the grace period ends.
  const rawSockets = new Set<Socket>()
  const agent = createRelayAgent((socket) => rawSockets.add(socket))
  const TrackedSocket = class extends SafeRelaySocket {
    constructor(url: string | URL) {
      super(url, agent)
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
    setTimeout(() => {
      for (const socket of rawSockets) socket.destroy()
      agent.destroy().catch(() => {})
    }, CLOSE_GRACE_MS).unref()
  }
}
