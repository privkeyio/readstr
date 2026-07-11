import { SimplePool, Event, Filter, nip19 } from 'nostr-tools'
import { useEffect, useState } from 'react'
import { getSyncRelays } from './nostr-sync'

export interface NostrProfile {
  name?: string
  nip05?: string
  about?: string
  picture?: string
}

const PROFILE_STORAGE_KEY = 'readstr_profiles'
const PROFILE_TTL_MS = 24 * 60 * 60 * 1000 // 24h
const PROFILE_QUERY_TIMEOUT_MS = 8000
const MAX_PROFILE_CONTENT_BYTES = 256 * 1024
const MAX_PROFILE_FIELD_CHARS = 256

interface CachedProfile {
  profile: NostrProfile | null
  fetchedAt: number
}

const memoryCache = new Map<string, CachedProfile>()
const inFlight = new Map<string, Promise<NostrProfile | null>>()

function cleanString(value: unknown, maxChars?: number): string | undefined {
  if (typeof value !== 'string') return undefined
  const trimmed = value.trim()
  if (!trimmed) return undefined
  return maxChars ? trimmed.slice(0, maxChars) : trimmed
}

function npubToHex(npub: string): string | null {
  try {
    const decoded = nip19.decode(npub)
    if (decoded.type === 'npub') return decoded.data
    return null
  } catch {
    return null
  }
}

function readStorageCache(): Record<string, CachedProfile> {
  if (typeof window === 'undefined') return {}
  try {
    const raw = localStorage.getItem(PROFILE_STORAGE_KEY)
    if (!raw) return {}
    const parsed = JSON.parse(raw)
    if (parsed && typeof parsed === 'object') return parsed as Record<string, CachedProfile>
  } catch {
    // ignore malformed cache
  }
  return {}
}

function writeStorageCache(npub: string, entry: CachedProfile): void {
  if (typeof window === 'undefined') return
  try {
    const all = readStorageCache()
    const now = Date.now()
    for (const [k, v] of Object.entries(all)) {
      if (!v || now - v.fetchedAt >= PROFILE_TTL_MS) delete all[k]
    }
    all[npub] = entry
    localStorage.setItem(PROFILE_STORAGE_KEY, JSON.stringify(all))
  } catch {
    // storage quota / serialization failure — non-fatal
  }
}

function getCached(npub: string): CachedProfile | null {
  const fromMemory = memoryCache.get(npub)
  if (fromMemory && Date.now() - fromMemory.fetchedAt < PROFILE_TTL_MS) {
    return fromMemory
  }
  const fromStorage = readStorageCache()[npub]
  if (fromStorage && Date.now() - fromStorage.fetchedAt < PROFILE_TTL_MS) {
    memoryCache.set(npub, fromStorage)
    return fromStorage
  }
  return null
}

// Verify a relay-returned event actually is a kind-0 authored by the requested
// pubkey. nostr-tools validates the signature but not the author/kind against the
// filter, so a malicious relay could otherwise return a different author's event.
function isExpectedProfileEvent(event: Event, pubkeyHex: string): boolean {
  if (event.kind !== 0) return false
  return event.pubkey.toLowerCase() === pubkeyHex.toLowerCase()
}

export async function fetchNostrProfile(npub: string): Promise<NostrProfile | null> {
  const pubkey = npubToHex(npub)
  if (!pubkey) return null

  const pool = new SimplePool()
  const relays = getSyncRelays()
  try {
    const filter: Filter = {
      kinds: [0],
      authors: [pubkey],
    }
    const events = await pool.querySync(relays, filter, {
      maxWait: PROFILE_QUERY_TIMEOUT_MS,
    })

    let newest: Event | null = null
    for (const event of events) {
      if (!isExpectedProfileEvent(event, pubkey)) continue
      if (event.content.length > MAX_PROFILE_CONTENT_BYTES) continue
      if (!newest || event.created_at > newest.created_at) newest = event
    }
    if (!newest) return null

    const data = JSON.parse(newest.content)
    return {
      name: cleanString(data.name, MAX_PROFILE_FIELD_CHARS) ?? cleanString(data.display_name, MAX_PROFILE_FIELD_CHARS),
      nip05: cleanString(data.nip05, MAX_PROFILE_FIELD_CHARS),
      about: cleanString(data.about),
      picture: cleanString(data.picture),
    }
  } catch {
    return null
  } finally {
    pool.close(relays)
  }
}

function dedupedFetch(key: string): Promise<NostrProfile | null> {
  const existing = inFlight.get(key)
  if (existing) return existing
  const promise = fetchNostrProfile(key).finally(() => inFlight.delete(key))
  inFlight.set(key, promise)
  return promise
}

export function useNostrProfile(npub: string | null | undefined): {
  profile: NostrProfile | null
  loading: boolean
} {
  const isNpub = typeof npub === 'string' && npub.startsWith('npub')
  const key = isNpub ? (npub as string) : null
  const [, setTick] = useState(0)
  const [loading, setLoading] = useState(false)

  useEffect(() => {
    if (!key || getCached(key)) return

    const controller = new AbortController()
    // localStorage-backed cache is client-only, so the fetch and its loading
    // flag must live in an effect rather than at render.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    setLoading(true)
    dedupedFetch(key)
      .then(result => {
        const entry: CachedProfile = { profile: result, fetchedAt: Date.now() }
        memoryCache.set(key, entry)
        writeStorageCache(key, entry)
        if (!controller.signal.aborted) setTick(t => t + 1)
      })
      .finally(() => {
        if (!controller.signal.aborted) setLoading(false)
      })

    return () => controller.abort()
  }, [key])

  const profile = key ? getCached(key)?.profile ?? null : null
  return { profile, loading }
}
