import { afterEach, describe, expect, it, vi } from 'vitest'
import { getCachedSummary, setCachedSummary } from './ai/summary-cache'
import { getHistory, recordRead } from './reading-history'
import { USER_STATE_CLEARED_EVENT } from './user-state-events'
import {
  OWNER_PUBKEY_KEY,
  USER_CACHE_NAMES,
  USER_STORAGE_KEYS,
  claimDeviceForUser,
  clearUserClientState,
  shouldClearOnAccountChange,
  signOut,
} from './sign-out'

// The node test environment has no indexedDB, so these exercise the in-memory
// fallbacks of reading history and the summary cache, not the IndexedDB paths.

function fakeStorage(initial: Record<string, string>) {
  const data = new Map(Object.entries(initial))
  return {
    data,
    get length() {
      return data.size
    },
    key: (i: number) => [...data.keys()][i] ?? null,
    getItem: (k: string) => data.get(k) ?? null,
    setItem: (k: string, v: string) => void data.set(k, v),
    removeItem: (k: string) => void data.delete(k),
  }
}

describe('clearUserClientState', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('removes per-user storage and caches but keeps device preferences', async () => {
    const storage = fakeStorage({
      readstr_ai_config: JSON.stringify({ apiKey: 'sk-secret' }),
      readstr_views: '[]',
      readstr_filters: '[]',
      readstr_profiles: '{}',
      readstr_last_sync: '1',
      'readstr_applied_createdat:readstr-views': '1700000000',
      'readstr_applied_createdat:readstr-prefs': '1700000000',
      [OWNER_PUBKEY_KEY]: 'alice',
      theme: 'dark',
      readstr_reading: '{}',
      nostr_relays: '["wss://relay.example"]',
    })
    const deleted: string[] = []
    vi.stubGlobal('localStorage', storage)
    vi.stubGlobal('caches', { delete: async (name: string) => (deleted.push(name), true) })

    await recordRead({ id: 'a', title: 't', content: 'c', author: null, feedTitle: 'f', feedType: 'RSS' })
    await setCachedSummary('k', 'summary')

    await clearUserClientState()

    for (const key of USER_STORAGE_KEYS) expect(storage.data.has(key)).toBe(false)
    expect([...storage.data.keys()].sort()).toEqual(['nostr_relays', 'readstr_reading', 'theme'])
    expect(deleted.sort()).toEqual([...USER_CACHE_NAMES].sort())
    expect(await getHistory()).toEqual([])
    expect(await getCachedSummary('k')).toBeNull()
  })

  it('still clears the rest when storage and a cache throw', async () => {
    vi.stubGlobal('localStorage', {
      removeItem: () => {
        throw new Error('SecurityError')
      },
    })
    const deleted: string[] = []
    vi.stubGlobal('caches', {
      delete: async (name: string) => {
        if (name === 'apis') throw new Error('blocked')
        deleted.push(name)
        return true
      },
    })
    await setCachedSummary('k', 'summary')

    await expect(clearUserClientState()).resolves.toBeUndefined()
    expect(deleted).toContain('cross-origin')
    expect(await getCachedSummary('k')).toBeNull()
  })

  it('re-clears storage after disconnecting and reloads', async () => {
    const storage = fakeStorage({ readstr_views: '[]', theme: 'dark' })
    const replace = vi.fn()
    vi.stubGlobal('localStorage', storage)
    vi.stubGlobal('window', { location: { replace } })
    const disconnect = () => {
      // A writer re-creating a key after disconnect is still cleared.
      storage.data.set('readstr_views', '[1]')
    }

    await signOut(disconnect)

    expect([...storage.data.keys()]).toEqual(['theme'])
    expect(replace).toHaveBeenCalledWith('/')
  })
})

describe('claimDeviceForUser', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('clears a different previous owner\'s data and records the new owner', async () => {
    const storage = fakeStorage({ [OWNER_PUBKEY_KEY]: 'alice', readstr_views: '[]', theme: 'dark' })
    const events: string[] = []
    vi.stubGlobal('localStorage', storage)
    vi.stubGlobal('window', { dispatchEvent: (e: Event) => (events.push(e.type), true) })
    const pending = claimDeviceForUser('bob')
    expect(events).toEqual([USER_STATE_CLEARED_EVENT])
    // localStorage is cleared synchronously, before the new user's UI reads it.
    expect(storage.data.has('readstr_views')).toBe(false)
    await pending
    expect(storage.data.get(OWNER_PUBKEY_KEY)).toBe('bob')
    expect(storage.data.get('theme')).toBe('dark')
  })

  it('keeps data for the same owner or when no owner was recorded', async () => {
    for (const owner of ['alice', undefined]) {
      const storage = fakeStorage(owner ? { [OWNER_PUBKEY_KEY]: owner, readstr_views: '[]' } : { readstr_views: '[]' })
      const events: string[] = []
      vi.stubGlobal('localStorage', storage)
      vi.stubGlobal('window', { dispatchEvent: (e: Event) => (events.push(e.type), true) })
      await claimDeviceForUser('alice')
      expect(events).toEqual([])
      expect(storage.data.get('readstr_views')).toBe('[]')
      expect(storage.data.get(OWNER_PUBKEY_KEY)).toBe('alice')
    }
  })
})

describe('shouldClearOnAccountChange', () => {
  it('skips session restore and clears on sign-out or account switch', () => {
    expect(shouldClearOnAccountChange(null, 'alice')).toBe(false)
    expect(shouldClearOnAccountChange('alice', null)).toBe(true)
    expect(shouldClearOnAccountChange('alice', 'bob')).toBe(true)
    expect(shouldClearOnAccountChange('alice', 'alice')).toBe(false)
  })
})
