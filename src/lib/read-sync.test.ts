import type { Event, UnsignedEvent } from 'nostr-tools'
import superjson from 'superjson'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { MAX_TRPC_BODY_BYTES } from '@/trpc/batch-limits'
import {
  APPLIED_CREATEDAT_PREFIX,
  getReadSyncEnabled,
  MAX_GUID_LEN,
  MAX_READ_ITEMS,
  READ_SYNC_KEY,
  readStatusListSchema,
  setReadSyncEnabled,
  type ReadStatusFetchResult,
} from './nostr-sync'
import {
  buildPublishedReadList,
  chunkGuidsForRequest,
  cleanPulledGuids,
  MARK_READ_REQUEST_BYTES,
  MAX_PUBLISHED_READ_CONTENT,
  newReadSyncSession,
  nextReadSyncRetryDelay,
  publishReadStatusIfChanged,
  pullReadStatus,
  READ_STATUS_D_TAG,
  READ_SYNC_RETRY_MAX_MS,
  READ_SYNC_RETRY_MIN_MS,
  readSyncAllowed,
  withdrawReadStatus,
} from './read-sync'

const NPUB = 'npub1test'
const WATERMARK = APPLIED_CREATEDAT_PREFIX + READ_STATUS_D_TAG

function fakeStorage() {
  const data = new Map<string, string>()
  return {
    data,
    getItem: (k: string) => data.get(k) ?? null,
    setItem: (k: string, v: string) => void data.set(k, v),
    removeItem: (k: string) => void data.delete(k),
  }
}

let storage: ReturnType<typeof fakeStorage>

beforeEach(() => {
  storage = fakeStorage()
  vi.stubGlobal('window', {})
  vi.stubGlobal('localStorage', storage)
})

afterEach(() => {
  vi.unstubAllGlobals()
  vi.restoreAllMocks()
})

describe('read sync setting', () => {
  it('defaults to off and round-trips', () => {
    expect(getReadSyncEnabled()).toBe(false)
    setReadSyncEnabled(true)
    expect(storage.data.get(READ_SYNC_KEY)).toBe('true')
    expect(getReadSyncEnabled()).toBe(true)
    setReadSyncEnabled(false)
    expect(getReadSyncEnabled()).toBe(false)
  })

  it('is off on the server and when storage throws', () => {
    vi.stubGlobal('localStorage', {
      getItem: () => {
        throw new Error('SecurityError')
      },
    })
    expect(getReadSyncEnabled()).toBe(false)
    vi.stubGlobal('window', undefined)
    expect(getReadSyncEnabled()).toBe(false)
  })

  it('stays off for the page session when turning it off cannot be saved', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    setReadSyncEnabled(true)
    storage.setItem = () => {
      throw new Error('QuotaExceededError')
    }
    setReadSyncEnabled(false)
    expect(storage.data.get(READ_SYNC_KEY)).toBe('true')
    expect(getReadSyncEnabled()).toBe(false)
    expect(warn).toHaveBeenCalled()
    setReadSyncEnabled(true)
    expect(getReadSyncEnabled()).toBe(false)
    // Saving works again, so the stored setting counts once more.
    storage.setItem = (k: string, v: string) => void storage.data.set(k, v)
    setReadSyncEnabled(true)
    expect(getReadSyncEnabled()).toBe(true)
  })
})

describe('nextReadSyncRetryDelay', () => {
  it('doubles from 30 seconds up to 10 minutes', () => {
    const delays = [READ_SYNC_RETRY_MIN_MS]
    for (let i = 0; i < 6; i++) delays.push(nextReadSyncRetryDelay(delays.at(-1)!))
    expect(delays).toEqual([30_000, 60_000, 120_000, 240_000, 480_000, 600_000, 600_000])
    expect(READ_SYNC_RETRY_MAX_MS).toBe(600_000)
  })
})

describe('readSyncAllowed', () => {
  it('needs the setting, a signer and an npub', () => {
    expect(readSyncAllowed(true, NPUB)).toBe(false)
    setReadSyncEnabled(true)
    expect(readSyncAllowed(true, NPUB)).toBe(true)
    expect(readSyncAllowed(false, NPUB)).toBe(false)
    expect(readSyncAllowed(true, undefined)).toBe(false)
    expect(readSyncAllowed(true, '')).toBe(false)
  })
})

describe('chunkGuidsForRequest', () => {
  it('keeps every guid in order and each chunk within the byte budget', () => {
    const guids = Array.from({ length: 5000 }, (_, i) => `https://example.com/post/${i}?ü=${'x'.repeat(i % 200)}`)
    const chunks = chunkGuidsForRequest(guids)
    expect(chunks.flat()).toEqual(guids)
    expect(chunks.length).toBeGreaterThan(1)
    for (const part of chunks) {
      expect(new TextEncoder().encode(JSON.stringify(part)).length).toBeLessThanOrEqual(MARK_READ_REQUEST_BYTES + 1)
    }
  })

  it('returns no chunks for no guids', () => {
    expect(chunkGuidsForRequest([])).toEqual([])
  })

  it('keeps every markReadByGuids request body under the route cap, even with the worst guids', () => {
    // JSON escapes a control character to six bytes, the most any one UTF-16
    // unit costs, so these are the largest guids the schema accepts.
    const worst = Array.from({ length: 2000 }, (_, i) => `${i}`.padEnd(MAX_GUID_LEN, '\u0001'))
    const mixed = Array.from({ length: 2000 }, (_, i) => `${i}`.padEnd(MAX_GUID_LEN / 2, '😀'))
    for (const guids of [worst, mixed]) {
      for (const part of chunkGuidsForRequest(guids)) {
        // The httpBatchLink body: inputs keyed by their index in the batch.
        // Room is left for ~200 markAsRead calls (~45 bytes each) batched alongside.
        const body = JSON.stringify({ 0: superjson.serialize({ guids: part }) })
        expect(new TextEncoder().encode(body).length + 200 * 45).toBeLessThan(MAX_TRPC_BODY_BYTES)
      }
    }
  })
})

describe('readStatusListSchema caps', () => {
  it('accepts the maximum and rejects past it', () => {
    expect(readStatusListSchema.safeParse({ itemGuids: ['a'.repeat(MAX_GUID_LEN)] }).success).toBe(true)
    expect(readStatusListSchema.safeParse({ itemGuids: ['a'.repeat(MAX_GUID_LEN + 1)] }).success).toBe(false)
    expect(readStatusListSchema.safeParse({ itemGuids: Array(MAX_READ_ITEMS).fill('g') }).success).toBe(true)
    expect(readStatusListSchema.safeParse({ itemGuids: Array(MAX_READ_ITEMS + 1).fill('g') }).success).toBe(false)
  })

  it('measures guids in UTF-16 units, as the desktop does, not code points', () => {
    // 129 code points but 258 UTF-16 units.
    expect(readStatusListSchema.safeParse({ itemGuids: ['😀'.repeat(129)] }).success).toBe(false)
    expect(readStatusListSchema.safeParse({ itemGuids: ['😀'.repeat(MAX_GUID_LEN / 2)] }).success).toBe(true)
  })
})

describe('cleanPulledGuids', () => {
  it('drops empty, whitespace-only and duplicate guids, keeping order', () => {
    expect(cleanPulledGuids(['b', '', '  ', 'a', 'b', '\t', ' c '])).toEqual(['b', 'a', ' c '])
  })
})

describe('buildPublishedReadList', () => {
  it('puts local keys first, then the remote keys it lacks', () => {
    expect(buildPublishedReadList(['c', 'a'], ['a', 'x', 'y'])).toEqual(['c', 'a', 'x', 'y'])
  })

  it('stays under the publish cap, keeping the newest local keys', () => {
    const local = Array.from({ length: 5000 }, (_, i) => `local-${i}`)
    const list = buildPublishedReadList(local, ['remote'])
    expect(list).toEqual(local.slice(0, list.length))
    expect(JSON.stringify({ itemGuids: list, lastUpdated: 1_900_000_000 }).length).toBeLessThanOrEqual(MAX_PUBLISHED_READ_CONTENT)
  })
})

const ok = (createdAt = 200) =>
  vi.fn(async (_guids: string[], sign: (event: UnsignedEvent) => Promise<Event>, _minCreatedAt?: number) => {
    await sign({} as UnsignedEvent)
    return { success: true, createdAt }
  })
const signEvent = vi.fn(async (event: UnsignedEvent) => event as Event)

function pulledSession(remote: string[] | null = null) {
  return { ...newReadSyncSession(NPUB), pulled: true, remote }
}

describe('pullReadStatus', () => {
  const remote = (itemGuids: string[], createdAt = 100, fresh = true, complete = true) =>
    vi.fn(async (): Promise<ReadStatusFetchResult> => ({ success: true, found: true, data: { itemGuids }, createdAt, fresh, complete }))
  const base = (session = newReadSyncSession(NPUB)) => ({
    canSign: () => true,
    npub: () => NPUB,
    session,
    onApplied: vi.fn(),
  })

  it('does nothing when the setting is off or the user cannot sign', async () => {
    const fetchRemote = remote(['a'])
    const markRead = vi.fn(async () => {})
    await pullReadStatus({ ...base(), markRead, fetchRemote })
    setReadSyncEnabled(true)
    await pullReadStatus({ ...base(), canSign: () => false, markRead, fetchRemote })
    await pullReadStatus({ ...base(), npub: () => undefined, markRead, fetchRemote })
    expect(fetchRemote).not.toHaveBeenCalled()
    expect(markRead).not.toHaveBeenCalled()
  })

  it('applies a fresh event, then advances the watermark and records what the relays hold', async () => {
    setReadSyncEnabled(true)
    const session = newReadSyncSession(NPUB)
    const markRead = vi.fn(async () => {})
    const opts = { ...base(session), markRead, fetchRemote: remote(['b', '', 'a', 'b']) }
    await expect(pullReadStatus(opts)).resolves.toBe(true)
    expect(markRead).toHaveBeenCalledWith(['b', 'a'])
    expect(opts.onApplied).toHaveBeenCalledOnce()
    expect(storage.data.get(WATERMARK)).toBe('100')
    expect(session).toMatchObject({ pulled: true, remote: ['b', 'a'] })
  })

  it('skips stale events but still learns what the relays hold', async () => {
    setReadSyncEnabled(true)
    storage.data.set(WATERMARK, '100')
    const markRead = vi.fn(async () => {})
    for (const fetchRemote of [remote(['a'], 100), remote(['a'], 50), remote(['a'], 150, false)]) {
      const session = newReadSyncSession(NPUB)
      await expect(pullReadStatus({ ...base(session), markRead, fetchRemote })).resolves.toBe(true)
      expect(session).toMatchObject({ pulled: true, remote: ['a'] })
    }
    expect(markRead).not.toHaveBeenCalled()
    expect(storage.data.get(WATERMARK)).toBe('100')
  })

  it('applies an event found while a relay did not answer, but keeps publishing locked', async () => {
    setReadSyncEnabled(true)
    const markRead = vi.fn(async () => {})
    const session = newReadSyncSession(NPUB)
    const opts = { ...base(session), markRead, fetchRemote: remote(['a'], 100, true, false) }
    await expect(pullReadStatus(opts)).resolves.toBe(false)
    expect(markRead).toHaveBeenCalledWith(['a'])
    expect(session).toMatchObject({ pulled: false, remote: ['a'] })
    const publish = ok()
    await publishReadStatusIfChanged({ ...base(session), fetchLocal: async () => ['a', 'b'], signEvent, publish })
    expect(publish).not.toHaveBeenCalled()

    // The same for an event already applied here.
    const stale = newReadSyncSession(NPUB)
    await expect(pullReadStatus({ ...base(stale), markRead, fetchRemote: remote(['c'], 100, true, false) })).resolves.toBe(false)
    expect(stale).toMatchObject({ pulled: false, remote: ['c'] })
  })

  it('unlocks publishing when no relay has an event', async () => {
    setReadSyncEnabled(true)
    const session = newReadSyncSession(NPUB)
    const fetchRemote = vi.fn(async (): Promise<ReadStatusFetchResult> => ({ success: true, found: false }))
    await expect(pullReadStatus({ ...base(session), markRead: vi.fn(), fetchRemote })).resolves.toBe(true)
    expect(session).toMatchObject({ pulled: true, remote: null })
  })

  it('keeps publishing locked when no relay answered or the payload was malformed', async () => {
    setReadSyncEnabled(true)
    vi.spyOn(console, 'warn').mockImplementation(() => {})
    for (const error of ['No relay answered', 'Malformed read status payload']) {
      const session = newReadSyncSession(NPUB)
      const fetchRemote = vi.fn(async (): Promise<ReadStatusFetchResult> => ({ success: false, error }))
      await expect(pullReadStatus({ ...base(session), markRead: vi.fn(), fetchRemote })).resolves.toBe(false)
      expect(session.pulled).toBe(false)
      const publish = ok()
      await publishReadStatusIfChanged({ ...base(session), fetchLocal: async () => ['a'], signEvent, publish })
      expect(publish).not.toHaveBeenCalled()
    }
  })

  it('keeps the watermark and the lock when applying fails, so the next pull retries', async () => {
    setReadSyncEnabled(true)
    vi.spyOn(console, 'error').mockImplementation(() => {})
    const session = newReadSyncSession(NPUB)
    const markRead = vi.fn(async () => {
      throw new Error('429')
    })
    const opts = { ...base(session), markRead, fetchRemote: remote(['a']) }
    await expect(pullReadStatus(opts)).resolves.toBe(false)
    expect(opts.onApplied).not.toHaveBeenCalled()
    expect(storage.data.has(WATERMARK)).toBe(false)
    expect(session).toMatchObject({ pulled: false, remote: null })
  })

  it('stops between chunks when the account changes, the setting turns off or it is cancelled', async () => {
    setReadSyncEnabled(true)
    const guids = Array.from({ length: 3000 }, (_, i) => `https://example.com/${'p'.repeat(40)}/${i}`)
    expect(chunkGuidsForRequest(guids).length).toBeGreaterThan(2)
    const stops: Array<(state: { npub: string; cancelled: boolean }) => void> = [
      (state) => void (state.npub = 'npub1other'),
      () => setReadSyncEnabled(false),
      (state) => void (state.cancelled = true),
    ]
    for (const stop of stops) {
      setReadSyncEnabled(true)
      const state = { npub: NPUB, cancelled: false }
      const session = newReadSyncSession(NPUB)
      const markRead = vi.fn(async () => stop(state))
      const opts = {
        ...base(session),
        npub: () => state.npub,
        isCancelled: () => state.cancelled,
        markRead,
        fetchRemote: remote(guids),
      }
      await expect(pullReadStatus(opts)).resolves.toBe(false)
      expect(markRead).toHaveBeenCalledOnce()
      expect(opts.onApplied).not.toHaveBeenCalled()
      expect(storage.data.has(WATERMARK)).toBe(false)
      expect(session.pulled).toBe(false)
    }
  })

  it('drops a result that arrives after the account changed', async () => {
    setReadSyncEnabled(true)
    let npub = NPUB
    const markRead = vi.fn()
    const fetchRemote = vi.fn(async (): Promise<ReadStatusFetchResult> => {
      npub = 'npub1other'
      return { success: true, found: true, data: { itemGuids: ['a'] }, createdAt: 100, fresh: true, complete: true }
    })
    await expect(pullReadStatus({ ...base(), npub: () => npub, markRead, fetchRemote })).resolves.toBe(false)
    expect(markRead).not.toHaveBeenCalled()
  })
})

describe('publishReadStatusIfChanged', () => {
  const opts = (session = pulledSession()) => ({ canSign: () => true, npub: () => NPUB, session, signEvent })

  it('does nothing when the setting is off, the user cannot sign or the account changed', async () => {
    const fetchLocal = vi.fn(async () => ['a'])
    const publish = ok()
    await publishReadStatusIfChanged({ ...opts(), fetchLocal, publish })
    setReadSyncEnabled(true)
    await publishReadStatusIfChanged({ ...opts(), canSign: () => false, fetchLocal, publish })
    await publishReadStatusIfChanged({ ...opts(), npub: () => 'npub1other', fetchLocal, publish })
    expect(fetchLocal).not.toHaveBeenCalled()
    expect(publish).not.toHaveBeenCalled()
  })

  it('waits for a successful pull', async () => {
    setReadSyncEnabled(true)
    const fetchLocal = vi.fn(async () => ['a'])
    const publish = ok()
    await publishReadStatusIfChanged({ ...opts(newReadSyncSession(NPUB)), fetchLocal, publish })
    expect(fetchLocal).not.toHaveBeenCalled()
    expect(publish).not.toHaveBeenCalled()
  })

  it('publishes a changed list once and skips one the relays already hold', async () => {
    setReadSyncEnabled(true)
    const session = pulledSession()
    const publish = ok()
    await publishReadStatusIfChanged({ ...opts(session), publish, fetchLocal: async () => ['a', 'b'] })
    await publishReadStatusIfChanged({ ...opts(session), publish, fetchLocal: async () => ['b', 'a'] })
    expect(publish).toHaveBeenCalledOnce()
    expect(publish).toHaveBeenCalledWith(['a', 'b'], expect.any(Function), 0)

    // Echo guard: right after a pull applied the list, the server's list adds nothing.
    const pulled = pulledSession(['c', 'd'])
    await publishReadStatusIfChanged({ ...opts(pulled), publish, fetchLocal: async () => ['d', 'c'] })
    await publishReadStatusIfChanged({ ...opts(pulled), publish, fetchLocal: async () => [] })
    expect(publish).toHaveBeenCalledOnce()
  })

  it('publishes local keys first, then the pulled keys this server lacks', async () => {
    setReadSyncEnabled(true)
    const session = pulledSession(['old', 'elsewhere'])
    const publish = ok()
    await publishReadStatusIfChanged({ ...opts(session), publish, fetchLocal: async () => ['new', 'old'] })
    expect(publish).toHaveBeenCalledWith(['new', 'old', 'elsewhere'], expect.any(Function), 0)
    expect(session.remote).toEqual(['new', 'old', 'elsewhere'])
  })

  it('does not republish a capped subset of a bigger pulled list', async () => {
    setReadSyncEnabled(true)
    const big = Array.from({ length: 5000 }, (_, i) => `remote-${i}`)
    const session = pulledSession(big)
    const publish = ok()
    await publishReadStatusIfChanged({ ...opts(session), publish, fetchLocal: async () => big.slice(0, 10) })
    expect(publish).not.toHaveBeenCalled()
  })

  it('advances the watermark to its own event, so the next pull skips it', async () => {
    setReadSyncEnabled(true)
    storage.data.set(WATERMARK, '100')
    await publishReadStatusIfChanged({ ...opts(), publish: ok(250), fetchLocal: async () => ['a'] })
    expect(storage.data.get(WATERMARK)).toBe('250')
  })

  it('does not advance the shared watermark once the account changed during the publish', async () => {
    setReadSyncEnabled(true)
    storage.data.set(WATERMARK, '100')
    let npub = NPUB
    const session = pulledSession()
    const publish = vi.fn(async () => {
      npub = 'npub1other'
      return { success: true, createdAt: 250 }
    })
    await publishReadStatusIfChanged({ ...opts(session), npub: () => npub, publish, fetchLocal: async () => ['a'] })
    expect(storage.data.get(WATERMARK)).toBe('100')
    expect(session.remote).toEqual(['a'])
  })

  it('gives each publish a created_at after the last one this session published', async () => {
    setReadSyncEnabled(true)
    const session = pulledSession()
    const publish = ok(500)
    await publishReadStatusIfChanged({ ...opts(session), publish, fetchLocal: async () => ['a'] })
    await publishReadStatusIfChanged({ ...opts(session), publish, fetchLocal: async () => ['a', 'b'] })
    expect(publish.mock.calls.map((call) => call[2])).toEqual([0, 501])
    expect(session.lastPublishedAt).toBe(500)
  })

  it('re-checks the setting before signing a publish already past its gate', async () => {
    setReadSyncEnabled(true)
    vi.spyOn(console, 'warn').mockImplementation(() => {})
    const sign = vi.fn(async (event: UnsignedEvent) => event as Event)
    const publish = vi.fn(async (_guids: string[], s: (event: UnsignedEvent) => Promise<Event>) => {
      try {
        await s({} as UnsignedEvent)
        return { success: true, createdAt: 200 }
      } catch (error) {
        return { success: false, error: String(error) }
      }
    })
    const session = pulledSession()
    await publishReadStatusIfChanged({
      ...opts(session),
      signEvent: sign,
      publish,
      fetchLocal: async () => {
        setReadSyncEnabled(false)
        return ['a']
      },
    })
    expect(sign).not.toHaveBeenCalled()
    expect(session.remote).toBeNull()
    expect(storage.data.has(WATERMARK)).toBe(false)
  })

  it('retries a list whose publish failed and never throws', async () => {
    setReadSyncEnabled(true)
    vi.spyOn(console, 'warn').mockImplementation(() => {})
    vi.spyOn(console, 'error').mockImplementation(() => {})
    const session = pulledSession()
    const publish = vi.fn(async () => ({ success: false, error: 'no relay' }))
    const o = { ...opts(session), publish, fetchLocal: async () => ['a'] }
    await publishReadStatusIfChanged(o)
    await publishReadStatusIfChanged(o)
    expect(publish).toHaveBeenCalledTimes(2)
    await expect(
      publishReadStatusIfChanged({ ...o, fetchLocal: async () => Promise.reject(new Error('down')) })
    ).resolves.toBeUndefined()
  })

  it('keeps running queued publishes after a rejected one', async () => {
    setReadSyncEnabled(true)
    const session = pulledSession()
    session.queue = Promise.reject(new Error('earlier failure'))
    const publish = ok()
    await publishReadStatusIfChanged({ ...opts(session), publish, fetchLocal: async () => ['a'] })
    expect(publish).toHaveBeenCalledOnce()
  })

  it('runs queued publishes one at a time', async () => {
    setReadSyncEnabled(true)
    const session = pulledSession()
    let active = 0
    let maxActive = 0
    const publish = vi.fn(async () => {
      active++
      maxActive = Math.max(maxActive, active)
      await new Promise((r) => setTimeout(r, 5))
      active--
      return { success: true }
    })
    await Promise.all([
      publishReadStatusIfChanged({ ...opts(session), publish, fetchLocal: async () => ['a'] }),
      publishReadStatusIfChanged({ ...opts(session), publish, fetchLocal: async () => ['a', 'b'] }),
    ])
    expect(publish).toHaveBeenCalledTimes(2)
    expect(maxActive).toBe(1)
  })
})

describe('withdrawReadStatus', () => {
  const opts = (session = pulledSession(['a'])) => ({ canSign: () => true, npub: () => NPUB, session, signEvent })

  it('publishes an empty list once the setting is off', async () => {
    const session = pulledSession(['a'])
    const publish = ok(300)
    await withdrawReadStatus({ ...opts(session), publish })
    expect(publish).toHaveBeenCalledWith([], signEvent, 0)
    expect(session.remote).toEqual([])
    expect(storage.data.get(WATERMARK)).toBe('300')
  })

  it('does not advance the shared watermark once the account changed during the withdrawal', async () => {
    storage.data.set(WATERMARK, '100')
    let npub = NPUB
    const session = pulledSession(['a'])
    const publish = vi.fn(async () => {
      npub = 'npub1other'
      return { success: true, createdAt: 300 }
    })
    await withdrawReadStatus({ ...opts(session), npub: () => npub, publish })
    expect(publish).toHaveBeenCalledOnce()
    expect(storage.data.get(WATERMARK)).toBe('100')
  })

  it('skips when the setting is back on, the user cannot sign or the account changed', async () => {
    const publish = ok()
    await withdrawReadStatus({ ...opts(), canSign: () => false, publish })
    await withdrawReadStatus({ ...opts(), npub: () => 'npub1other', publish })
    setReadSyncEnabled(true)
    await withdrawReadStatus({ ...opts(), publish })
    expect(publish).not.toHaveBeenCalled()
  })

  it('runs after a publish already queued, which then stops before signing', async () => {
    setReadSyncEnabled(true)
    vi.spyOn(console, 'warn').mockImplementation(() => {})
    const session = pulledSession()
    const calls: string[][] = []
    const publish = vi.fn(async (guids: string[], s: (event: UnsignedEvent) => Promise<Event>) => {
      try {
        await s({} as UnsignedEvent)
      } catch (error) {
        return { success: false, error: String(error) }
      }
      calls.push(guids)
      return { success: true, createdAt: 400 }
    })
    const queued = publishReadStatusIfChanged({ ...opts(session), publish, fetchLocal: async () => ['a'] })
    setReadSyncEnabled(false)
    await Promise.all([queued, withdrawReadStatus({ ...opts(session), publish })])
    expect(calls).toEqual([[]])
  })

  it('goes after a list this session published in the same second', async () => {
    setReadSyncEnabled(true)
    const session = pulledSession()
    const publish = ok(400)
    await publishReadStatusIfChanged({ ...opts(session), publish, fetchLocal: async () => ['a'] })
    setReadSyncEnabled(false)
    await withdrawReadStatus({ ...opts(session), publish })
    expect(publish).toHaveBeenLastCalledWith([], signEvent, 401)
  })
})
