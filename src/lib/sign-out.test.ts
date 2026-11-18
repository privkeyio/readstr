import { afterEach, describe, expect, it, vi } from 'vitest'
import { getCachedSummary, setCachedSummary } from './ai/summary-cache'
import { getHistory, recordRead } from './reading-history'
import { USER_CACHE_NAMES, USER_STORAGE_KEYS, clearUserClientState } from './sign-out'

function fakeStorage(initial: Record<string, string>) {
  const data = new Map(Object.entries(initial))
  return {
    data,
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
})
