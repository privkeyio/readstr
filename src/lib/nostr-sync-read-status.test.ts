import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

type SubParams = { onevent: (event: unknown) => void; oneose: () => void; onclose: (reason: string) => void; eoseTimeout: number }
type Behaviour = 'eose' | 'silent' | 'down' | { events: unknown[] }

const behaviours = new Map<string, Behaviour>()
const closed: string[][] = []

vi.mock('nostr-tools', async (importOriginal) => {
  const actual = await importOriginal<typeof import('nostr-tools')>()
  class FakePool {
    async ensureRelay(url: string) {
      const behaviour = behaviours.get(url)
      if (behaviour === 'down') throw new Error('connection failed')
      return {
        subscribe: (_filters: unknown[], params: SubParams) => {
          if (typeof behaviour === 'object') {
            setTimeout(() => {
              for (const event of behaviour.events) params.onevent(event)
              params.oneose()
            }, 10)
          } else if (behaviour === 'eose') {
            setTimeout(() => params.oneose(), 10)
          }
          return { close: () => params.onclose('closed by caller') }
        },
      }
    }
    close(relays: string[]) {
      closed.push(relays)
    }
  }
  return { ...actual, SimplePool: FakePool }
})

const { fetchReadStatus, APPLIED_CREATEDAT_PREFIX } = await import('./nostr-sync')

const PUBKEY = '3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d'
const RELAYS = ['wss://a.example', 'wss://b.example']

function event(content: string, created_at = 100) {
  return { kind: 30405, pubkey: PUBKEY, created_at, tags: [['d', 'readstr-read-status']], content, id: 'x', sig: 'y' }
}

beforeEach(() => {
  vi.useFakeTimers()
  behaviours.clear()
  closed.length = 0
  const data = new Map<string, string>([['nostr_relays', JSON.stringify(RELAYS)]])
  vi.stubGlobal('window', { localStorage: { getItem: (k: string) => data.get(k) ?? null } })
  vi.stubGlobal('localStorage', { getItem: (k: string) => data.get(k) ?? null, setItem: (k: string, v: string) => void data.set(k, v) })
  vi.spyOn(console, 'error').mockImplementation(() => {})
})

afterEach(() => {
  vi.useRealTimers()
  vi.unstubAllGlobals()
  vi.restoreAllMocks()
})

async function fetchWithTimers() {
  const pending = fetchReadStatus(PUBKEY)
  await vi.runAllTimersAsync()
  return pending
}

describe('fetchReadStatus', () => {
  it('reports "no event" only when a relay sent EOSE', async () => {
    behaviours.set(RELAYS[0]!, 'down')
    behaviours.set(RELAYS[1]!, 'eose')
    await expect(fetchWithTimers()).resolves.toEqual({ success: true, found: false })
    expect(closed).toEqual([RELAYS])
  })

  it('fails when every relay times out or is down, rather than returning an empty list', async () => {
    behaviours.set(RELAYS[0]!, 'down')
    behaviours.set(RELAYS[1]!, 'silent')
    await expect(fetchWithTimers()).resolves.toEqual({ success: false, error: 'Not every connected relay answered' })
  })

  it('does not report "no event" when one relay answered empty and another connected but timed out', async () => {
    // The timed-out relay may hold the only copy another device published.
    behaviours.set(RELAYS[0]!, 'eose')
    behaviours.set(RELAYS[1]!, 'silent')
    await expect(fetchWithTimers()).resolves.toEqual({ success: false, error: 'Not every connected relay answered' })
  })

  it('fails on a malformed payload', async () => {
    behaviours.set(RELAYS[0]!, { events: [event('{"itemGuids":[1]}')] })
    await expect(fetchWithTimers()).resolves.toMatchObject({ success: false })
    behaviours.set(RELAYS[0]!, { events: [event('not json')] })
    await expect(fetchWithTimers()).resolves.toMatchObject({ success: false })
  })

  it('returns the newest event and whether it is fresh', async () => {
    behaviours.set(RELAYS[0]!, { events: [event('{"itemGuids":["old"]}', 50), event('{"itemGuids":["a"]}', 100)] })
    behaviours.set(RELAYS[1]!, 'silent')
    await expect(fetchWithTimers()).resolves.toEqual({
      success: true,
      found: true,
      data: { itemGuids: ['a'] },
      createdAt: 100,
      fresh: true,
    })
    localStorage.setItem(APPLIED_CREATEDAT_PREFIX + 'readstr-read-status', '100')
    await expect(fetchWithTimers()).resolves.toMatchObject({ found: true, fresh: false })
  })
})
