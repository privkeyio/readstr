import { env } from '@/env.mjs'

export const FALLBACK_RELAYS: readonly string[] = [
  'wss://relay.damus.io',
  'wss://relay.primal.net',
  'wss://nos.lol',
  'wss://relay.snort.social',
  'wss://relay.nostr.band',
  'wss://nostr-pub.wellorder.net',
]

const CONTROL_OR_SPACE = /[\u0000-\u0020\u007f-\u009f\s]/

export function parseRelayList(raw: string | undefined | null): string[] {
  if (!raw) return []
  const relays: string[] = []
  for (const entry of raw.split(/[\s,]+/)) {
    if (!entry || CONTROL_OR_SPACE.test(entry)) continue
    try {
      const url = new URL(entry)
      if (url.protocol !== 'wss:' && url.protocol !== 'ws:') continue
      if (url.username || url.password) continue
    } catch {
      continue
    }
    if (!relays.includes(entry)) relays.push(entry)
  }
  return relays
}

export function resolveDefaultRelays(raw: string | undefined | null): string[] {
  const parsed = parseRelayList(raw)
  return parsed.length > 0 ? parsed : [...FALLBACK_RELAYS]
}

let runtimeDefaultRelays: string[] | null = null

export function setRuntimeDefaultRelays(relays: string[]): void {
  runtimeDefaultRelays = relays.length > 0 ? [...relays] : null
}

// Server: read DEFAULT_RELAYS from the environment at call time.
// Browser: use the value the root layout handed down at request time.
export function getDefaultRelays(): string[] {
  if (typeof window === 'undefined') return resolveDefaultRelays(env.DEFAULT_RELAYS)
  return runtimeDefaultRelays ? [...runtimeDefaultRelays] : [...FALLBACK_RELAYS]
}

export function pickRelays(saved: string | null, defaults: string[]): string[] {
  if (!saved) return [...defaults]
  try {
    const parsed: unknown = JSON.parse(saved)
    if (Array.isArray(parsed) && parsed.length > 0 && parsed.every((r) => typeof r === 'string')) {
      return parsed as string[]
    }
  } catch {
    // fall through to defaults
  }
  return [...defaults]
}

export function getUserOrDefaultRelays(): string[] {
  if (typeof window === 'undefined') return getDefaultRelays()
  let saved: string | null = null
  try {
    saved = window.localStorage.getItem('nostr_relays')
  } catch {
    saved = null
  }
  return pickRelays(saved, getDefaultRelays())
}
