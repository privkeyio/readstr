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
  readStatusWatermarkTag,
  setReadSyncEnabled,
  type ReadStatusFetchResult,
} from './nostr-sync'
import {
  buildPublishedReadList,
  chunkGuidsForRequest,
  cleanPulledGuids,
  coverNewFeedsWithReadList,
  MARK_READ_REQUEST_BYTES,
  MAX_PUBLISHED_READ_CONTENT,
  newReadSyncSession,
  nextReadSyncRetryDelay,
  publishReadStatusIfChanged,
  pullReadStatus,
  READ_STATUS_D_TAG,
  READ_SYNC_FEED_IDS_MAX,
  READ_SYNC_RETRY_MAX_MS,
  READ_SYNC_RETRY_MIN_MS,
  readSyncAllowed,
  withdrawReadStatus,
} from './read-sync'

const NPUB = 'npub1test'
const WATERMARK = APPLIED_CREATEDAT_PREFIX + readStatusWatermarkTag(NPUB)
// The unkeyed watermark older builds kept, shared by every account here.
const LEGACY_WATERMARK = APPLIED_CREATEDAT_PREFIX + READ_STATUS_D_TAG

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

  it('keeps a call scoped to the most and longest feed ids under the route cap too', async () => {
    setReadSyncEnabled(true)
    const worst = Array.from({ length: 2000 }, (_, i) => `${i}`.padEnd(MAX_GUID_LEN, '\u0001'))
    const feedIds = Array.from({ length: READ_SYNC_FEED_IDS_MAX }, (_, i) => `${i}`.padEnd(64, 'x'))
    const server = fakeServer(feedIds)
    const session = { ...newReadSyncSession(NPUB), pulled: true, remote: worst }
    await expect(coverNewFeedsWithReadList({ canSign: () => true, npub: () => NPUB, session, ...server })).resolves.toBeNull()
    expect(server.markRead.mock.calls.flatMap(([part]) => part)).toEqual(worst)
    for (const [part, scope] of server.markRead.mock.calls) {
      expect(scope).toEqual(feedIds)
      const body = JSON.stringify({ 0: superjson.serialize({ guids: part, feedIds: scope }) })
      expect(new TextEncoder().encode(body).length + 200 * 45).toBeLessThan(MAX_TRPC_BODY_BYTES)
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

// The applied created_at the server keeps for the account, raised only.
function fakeApplied(initial: number | null = null) {
  const state = {
    value: initial,
    getAppliedCreatedAt: vi.fn(async () => state.value),
    advanceAppliedCreatedAt: vi.fn(async (createdAt: number) => {
      if (state.value == null || createdAt > state.value) state.value = createdAt
    }),
  }
  return state
}

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
    getUncovered: vi.fn(async () => ({ feedIds: [] as string[], more: false })),
    markApplied: vi.fn(async () => {}),
    ...fakeApplied(),
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

  // Sync turned off and on again while the pull is in flight: the withdrawal
  // records [], and the older pulled list must not replace it.
  async function withdrawMidPull(session: ReturnType<typeof newReadSyncSession>) {
    setReadSyncEnabled(false)
    await withdrawReadStatus({ canSign: () => true, npub: () => NPUB, session, signEvent, publish: ok(300), ...fakeApplied() })
    setReadSyncEnabled(true)
  }

  it('keeps a withdrawal made while a fresh event was being applied', async () => {
    setReadSyncEnabled(true)
    const session = newReadSyncSession(NPUB)
    const markRead = vi.fn(async () => withdrawMidPull(session))
    const opts = { ...base(session), markRead, fetchRemote: remote(['old'], 100, true, false) }
    await expect(pullReadStatus(opts)).resolves.toBe(true)
    expect(markRead).toHaveBeenCalledWith(['old'])
    expect(session).toMatchObject({ pulled: true, remote: [] })
  })

  it('keeps a withdrawal made while fetching an event already applied here', async () => {
    setReadSyncEnabled(true)
    const session = newReadSyncSession(NPUB)
    const fetchRemote = vi.fn(async (): Promise<ReadStatusFetchResult> => {
      await withdrawMidPull(session)
      return { success: true, found: true, data: { itemGuids: ['old'] }, createdAt: 100, fresh: false, complete: true }
    })
    await expect(pullReadStatus({ ...base(session), markRead: vi.fn(), fetchRemote })).resolves.toBe(true)
    expect(session).toMatchObject({ pulled: true, remote: [] })
  })

  it('makes the next publish and withdrawal go after the newest pulled event', async () => {
    setReadSyncEnabled(true)
    storage.data.set(WATERMARK, '2000')
    const markRead = vi.fn(async () => {})
    // A stale event from a pull a relay did not fully answer counts as much as a complete one.
    for (const fetchRemote of [remote(['a'], 1000, false, false), remote(['a'], 1000, true, true)]) {
      const session = newReadSyncSession(NPUB)
      await pullReadStatus({ ...base(session), markRead, fetchRemote })
      expect(session.latestSeenAt).toBe(1000)
      session.pulled = true
      const publish = ok(1001)
      await publishReadStatusIfChanged({ ...base(session), signEvent, publish, fetchLocal: async () => ['b'] })
      expect(publish).toHaveBeenLastCalledWith(['b', 'a'], expect.any(Function), 1001)
      setReadSyncEnabled(false)
      await withdrawReadStatus({ ...base(session), signEvent, publish })
      expect(publish).toHaveBeenLastCalledWith([], signEvent, 1002)
      setReadSyncEnabled(true)
    }
  })

  it('never lowers the publish floor when a pull finds an older event', async () => {
    setReadSyncEnabled(true)
    const session = pulledSession()
    await publishReadStatusIfChanged({ ...base(session), signEvent, publish: ok(500), fetchLocal: async () => ['a'] })
    await pullReadStatus({ ...base(session), markRead: vi.fn(async () => {}), fetchRemote: remote(['x'], 100, false) })
    expect(session.latestSeenAt).toBe(500)
    expect(newReadSyncSession(NPUB).latestSeenAt).toBeNull()
  })

  it('raises the publish floor from an event found just before the pull stopped', async () => {
    setReadSyncEnabled(true)
    const session = newReadSyncSession(NPUB)
    const fetchRemote = vi.fn(async (): Promise<ReadStatusFetchResult> => {
      setReadSyncEnabled(false)
      return { success: true, found: true, data: { itemGuids: ['a'] }, createdAt: 900, fresh: true, complete: true }
    })
    await expect(pullReadStatus({ ...base(session), markRead: vi.fn(), fetchRemote })).resolves.toBe(false)
    const publish = ok(901)
    await withdrawReadStatus({ ...base(session), signEvent, publish })
    expect(publish).toHaveBeenCalledWith([], signEvent, 901)
  })
})

describe('publishReadStatusIfChanged', () => {
  const opts = (session = pulledSession()) => ({ canSign: () => true, npub: () => NPUB, session, signEvent, ...fakeApplied() })

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
    expect(session.latestSeenAt).toBe(500)
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

describe('pullReadStatus and covered feeds', () => {
  const fresh = (itemGuids: string[], isFresh = true) =>
    vi.fn(async (): Promise<ReadStatusFetchResult> => ({ success: true, found: true, data: { itemGuids }, createdAt: 100, fresh: isFresh, complete: true }))
  const auth = { canSign: () => true, npub: () => NPUB }

  it('marks applied the feeds uncovered when a fresh pull starts, not ones subscribed during it', async () => {
    setReadSyncEnabled(true)
    const guids = Array.from({ length: 3000 }, (_, i) => `https://example.com/${'p'.repeat(40)}/${i}`)
    const server = fakeServer(['old', 'new-before'], ['old'])
    server.markRead.mockImplementation(async () => {
      // A feed subscribed while the chunks go out misses the earlier ones.
      if (!server.subscribed.includes('mid-pull')) server.subscribed.push('mid-pull')
      return { marked: 0 }
    })
    const session = newReadSyncSession(NPUB)
    await expect(pullReadStatus({ ...auth, ...server, session, fetchRemote: fresh(guids) })).resolves.toBe(true)
    // The pull applies to every feed, unscoped.
    expect(server.markRead.mock.calls.map((call) => call.length)).toEqual(chunkGuidsForRequest(guids).map(() => 1))
    expect(server.markApplied).toHaveBeenCalledOnce()
    expect(server.markApplied).toHaveBeenCalledWith(['new-before'])
    expect([...server.applied].sort()).toEqual(['new-before', 'old'])

    // The cover run then gives the list to the feed subscribed mid-pull.
    server.markRead.mockClear()
    await expect(coverNewFeedsWithReadList({ ...auth, ...server, session })).resolves.toBeNull()
    expect(server.markRead).toHaveBeenCalledTimes(chunkGuidsForRequest(guids, MARK_READ_REQUEST_BYTES - JSON.stringify(['mid-pull']).length).length)
    expect(new Set(server.markRead.mock.calls.map(([, scope]) => JSON.stringify(scope)))).toEqual(new Set(['["mid-pull"]']))
    expect(server.applied.has('mid-pull')).toBe(true)
  })

  it('marks nothing applied for a stale event, and still applies when the snapshot fails', async () => {
    setReadSyncEnabled(true)
    const server = fakeServer(['f1'])
    await pullReadStatus({ ...auth, ...server, session: newReadSyncSession(NPUB), fetchRemote: fresh(['a'], false) })
    expect(server.getUncovered).not.toHaveBeenCalled()
    expect(server.markApplied).not.toHaveBeenCalled()

    vi.spyOn(console, 'warn').mockImplementation(() => {})
    server.getUncovered.mockRejectedValueOnce(new Error('429'))
    const session = newReadSyncSession(NPUB)
    await expect(pullReadStatus({ ...auth, ...server, session, fetchRemote: fresh(['a']) })).resolves.toBe(true)
    expect(server.markRead).toHaveBeenCalledWith(['a'])
    expect(server.markApplied).not.toHaveBeenCalled()
    expect(storage.data.get(WATERMARK)).toBe('100')
  })

  it('never throws when an opts callback does', async () => {
    setReadSyncEnabled(true)
    vi.spyOn(console, 'error').mockImplementation(() => {})
    const boom = () => {
      throw new Error('boom')
    }
    const server = fakeServer(['f1'])
    const session = newReadSyncSession(NPUB)
    for (const broken of [{ isCancelled: boom }, { canSign: boom }, { npub: boom }]) {
      await expect(pullReadStatus({ ...auth, ...server, session, fetchRemote: fresh(['a']), ...broken })).resolves.toBe(false)
    }
  })
})

// The account's subscriptions as the server keeps them: which feeds are
// subscribed and which ones the read list was applied to.
function fakeServer(subscribed: string[], appliedAlready: string[] = []) {
  const applied = new Set(appliedAlready)
  const server = {
    subscribed,
    applied,
    markRead: vi.fn(async (_guids: string[], _feedIds?: string[]): Promise<unknown> => ({ marked: 0 })),
    getUncovered: vi.fn(async () => {
      const feedIds = server.subscribed.filter((feedId) => !applied.has(feedId))
      return { feedIds: feedIds.slice(0, READ_SYNC_FEED_IDS_MAX), more: feedIds.length > READ_SYNC_FEED_IDS_MAX }
    }),
    markApplied: vi.fn(async (feedIds: string[]) => {
      for (const feedId of feedIds) if (server.subscribed.includes(feedId)) applied.add(feedId)
    }),
    onApplied: vi.fn(),
    ...fakeApplied(),
  }
  return server
}

describe('coverNewFeedsWithReadList', () => {
  const pulled = (remote: string[] | null = ['a', 'b'], npub = NPUB) => ({ ...newReadSyncSession(npub), pulled: true, remote })
  const run = (server: ReturnType<typeof fakeServer>, session = pulled(), extra: Record<string, unknown> = {}) =>
    coverNewFeedsWithReadList({ canSign: () => true, npub: () => session.npub, session, ...server, ...extra })
  const bigList = () => Array.from({ length: 3000 }, (_, i) => `https://example.com/${'p'.repeat(40)}/${i}`)

  it('applies the list only to uncovered feeds, then marks them applied', async () => {
    setReadSyncEnabled(true)
    const server = fakeServer(['covered', 'f2', 'f3'], ['covered'])
    await expect(run(server)).resolves.toBeNull()
    expect(server.markRead).toHaveBeenCalledOnce()
    expect(server.markRead).toHaveBeenCalledWith(['a', 'b'], ['f2', 'f3'])
    expect(server.markApplied).toHaveBeenCalledWith(['f2', 'f3'])
    expect(server.onApplied).toHaveBeenCalledOnce()
    expect([...server.applied].sort()).toEqual(['covered', 'f2', 'f3'])

    // Covered now: the next check sends nothing but the one lookup.
    await run(server)
    expect(server.getUncovered).toHaveBeenCalledTimes(2)
    expect(server.markRead).toHaveBeenCalledOnce()
    expect(server.markApplied).toHaveBeenCalledOnce()
  })

  it('leaves a feed another browser covered alone, so its unreads stay unread', async () => {
    setReadSyncEnabled(true)
    const server = fakeServer(['old'], ['old'])
    const browserX = pulled(['k'])
    const browserY = pulled(['k'])
    // The user subscribes F, browser Y covers it, then marks F:k unread.
    server.subscribed.push('F')
    await run(server, browserY)
    expect(server.markRead).toHaveBeenCalledWith(['k'], ['F'])
    server.markRead.mockClear()
    // Browser X, which never saw F before, checks later and sends nothing.
    await expect(run(server, browserX)).resolves.toBeNull()
    expect(server.markRead).not.toHaveBeenCalled()
    expect(server.markApplied).toHaveBeenCalledOnce()
  })

  it('marks uncovered feeds applied without sending anything when there is no list', async () => {
    setReadSyncEnabled(true)
    for (const remote of [null, []]) {
      const server = fakeServer(['f1'])
      await expect(run(server, pulled(remote))).resolves.toBeNull()
      expect(server.markRead).not.toHaveBeenCalled()
      expect(server.markApplied).toHaveBeenCalledWith(['f1'])
      expect(server.onApplied).not.toHaveBeenCalled()
    }
  })

  it('does not even look before the pull, or when sync is not allowed', async () => {
    const notPulled = pulled()
    notPulled.pulled = false
    const cases: Array<[ReturnType<typeof pulled>, Record<string, unknown>]> = [
      [pulled(), {}], // the setting is off
      [pulled(), { canSign: () => false }],
      [pulled(), { npub: () => 'npub1other' }],
      [pulled(), { isCancelled: () => true }],
      [notPulled, {}],
    ]
    for (const [i, [session, extra]] of cases.entries()) {
      if (i === 1) setReadSyncEnabled(true)
      const server = fakeServer(['f1'])
      await expect(run(server, session, extra)).resolves.toBeNull()
      expect(server.getUncovered).not.toHaveBeenCalled()
    }
  })

  it('stops between steps when cancelled or the account changes, leaving the feeds uncovered', async () => {
    setReadSyncEnabled(true)
    const steps = ['getUncovered', 'markRead', 'markApplied'] as const
    for (const step of steps) {
      for (const stop of ['cancel', 'account'] as const) {
        const server = fakeServer(['f1'])
        const state = { cancelled: false, npub: NPUB }
        const original = server[step].getMockImplementation()!
        server[step].mockImplementation((async (...args: never[]) => {
          const result = await (original as (...a: never[]) => Promise<unknown>)(...args)
          if (stop === 'cancel') state.cancelled = true
          else state.npub = 'npub1other'
          return result
        }) as never)
        const session = pulled(bigList())
        const result = await run(server, session, { isCancelled: () => state.cancelled, npub: () => state.npub })
        expect(result).toBeNull()
        expect(server.onApplied).not.toHaveBeenCalled()
        if (step === 'getUncovered') expect(server.markRead).not.toHaveBeenCalled()
        if (step === 'markRead') {
          expect(server.markRead).toHaveBeenCalledOnce()
          expect(server.markApplied).not.toHaveBeenCalled()
          expect(server.applied.has('f1')).toBe(false)
        }
      }
    }
  })

  it('keeps the feeds uncovered on failure and backs off exponentially, then resets', async () => {
    setReadSyncEnabled(true)
    vi.spyOn(console, 'error').mockImplementation(() => {})
    const server = fakeServer(['f1'])
    const session = pulled()
    server.markRead.mockRejectedValue(new Error('429'))
    await expect(run(server, session)).resolves.toBe(READ_SYNC_RETRY_MIN_MS)
    await expect(run(server, session)).resolves.toBe(READ_SYNC_RETRY_MIN_MS * 2)
    await expect(run(server, session)).resolves.toBe(READ_SYNC_RETRY_MIN_MS * 4)
    expect(server.applied.has('f1')).toBe(false)
    expect(server.markApplied).not.toHaveBeenCalled()

    server.getUncovered.mockRejectedValueOnce(new Error('429'))
    await expect(run(server, session)).resolves.toBe(READ_SYNC_RETRY_MIN_MS * 8)
    server.markApplied.mockRejectedValueOnce(new Error('429'))
    server.markRead.mockResolvedValue({ marked: 1 })
    await expect(run(server, session)).resolves.toBe(READ_SYNC_RETRY_MIN_MS * 16)
    expect(server.applied.has('f1')).toBe(false)

    await expect(run(server, session)).resolves.toBeNull()
    expect(server.applied.has('f1')).toBe(true)
    expect(session.coverRetryDelay).toBe(READ_SYNC_RETRY_MIN_MS)
    for (let i = 0; i < 20; i++) session.coverRetryDelay = nextReadSyncRetryDelay(session.coverRetryDelay)
    expect(session.coverRetryDelay).toBe(READ_SYNC_RETRY_MAX_MS)
  })

  it('never throws when an opts callback does, and reports it as a failure', async () => {
    setReadSyncEnabled(true)
    vi.spyOn(console, 'error').mockImplementation(() => {})
    const boom = () => {
      throw new Error('boom')
    }
    for (const broken of [{ isCancelled: boom }, { canSign: boom }, { npub: boom }, { onApplied: boom }]) {
      const session = pulled()
      await expect(run(fakeServer(['f1']), session, broken)).resolves.toBe(READ_SYNC_RETRY_MIN_MS)
      expect(session.covering).toBe(false)
    }
  })

  it('never runs twice at once, and runs again for a feed subscribed meanwhile', async () => {
    setReadSyncEnabled(true)
    const server = fakeServer(['f1'])
    const session = pulled()
    let inFlight = 0
    let release!: () => void
    server.markRead.mockImplementation(async () => {
      inFlight++
      expect(inFlight).toBe(1)
      if (server.markRead.mock.calls.length === 1) await new Promise<void>((resolve) => (release = resolve))
      inFlight--
      return { marked: 1 }
    })
    const first = run(server, session)
    await vi.waitFor(() => expect(server.markRead).toHaveBeenCalledOnce())
    server.subscribed.push('f2')
    await expect(run(server, session)).resolves.toBeNull()
    expect(server.getUncovered).toHaveBeenCalledOnce()
    release()
    await first
    expect(server.markRead.mock.calls.map(([, scope]) => scope)).toEqual([['f1'], ['f2']])
    expect([...server.applied]).toEqual(['f1', 'f2'])
  })

  it('pages through more uncovered feeds than one call carries', async () => {
    setReadSyncEnabled(true)
    const feeds = Array.from({ length: READ_SYNC_FEED_IDS_MAX + 3 }, (_, i) => `f${i}`)
    const server = fakeServer(feeds)
    await expect(run(server)).resolves.toBeNull()
    expect(server.markApplied.mock.calls.map(([ids]) => ids.length)).toEqual([READ_SYNC_FEED_IDS_MAX, 3])
    expect(server.applied.size).toBe(feeds.length)
  })
})

describe('withdrawReadStatus', () => {
  const opts = (session = pulledSession(['a'])) => ({ canSign: () => true, npub: () => NPUB, session, signEvent, ...fakeApplied() })

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

describe('the applied created_at kept on the server', () => {
  const event = (itemGuids: string[], createdAt: number, complete = true) =>
    vi.fn(async (): Promise<ReadStatusFetchResult> => ({ success: true, found: true, data: { itemGuids }, createdAt, fresh: true, complete }))
  const auth = { canSign: () => true, npub: () => NPUB }

  it('does not apply in a new browser an event another browser applied, but still covers new feeds with it', async () => {
    setReadSyncEnabled(true)
    for (const recorded of [100, 150]) {
      const server = { ...fakeServer(['old', 'new'], ['old']), ...fakeApplied(recorded) }
      const session = newReadSyncSession(NPUB)
      storage.data.delete(WATERMARK)
      await expect(pullReadStatus({ ...auth, ...server, session, fetchRemote: event(['a'], 100) })).resolves.toBe(true)
      expect(server.markRead).not.toHaveBeenCalled()
      expect(server.onApplied).not.toHaveBeenCalled()
      expect(server.advanceAppliedCreatedAt).not.toHaveBeenCalled()
      expect(session).toMatchObject({ pulled: true, remote: ['a'] })
      expect(storage.data.get(WATERMARK)).toBe('100')

      await expect(coverNewFeedsWithReadList({ ...auth, ...server, session })).resolves.toBeNull()
      expect(server.markRead).toHaveBeenCalledWith(['a'], ['new'])
      expect([...server.applied].sort()).toEqual(['new', 'old'])
    }
  })

  it('applies a newer event and records it on the server', async () => {
    setReadSyncEnabled(true)
    for (const recorded of [null, 99]) {
      const applied = fakeApplied(recorded)
      const server = { ...fakeServer([]), ...applied }
      storage.data.delete(WATERMARK)
      await expect(pullReadStatus({ ...auth, ...server, session: newReadSyncSession(NPUB), fetchRemote: event(['a'], 100) })).resolves.toBe(true)
      expect(server.markRead).toHaveBeenCalledWith(['a'])
      expect(applied.advanceAppliedCreatedAt).toHaveBeenCalledWith(100)
      expect(applied.value).toBe(100)
      expect(storage.data.get(WATERMARK)).toBe('100')
    }
  })

  it('skips the server for an event this browser already applied', async () => {
    setReadSyncEnabled(true)
    storage.data.set(WATERMARK, '100')
    const server = { ...fakeServer([]), ...fakeApplied() }
    await pullReadStatus({ ...auth, ...server, session: newReadSyncSession(NPUB), fetchRemote: event(['a'], 100) })
    expect(server.getAppliedCreatedAt).not.toHaveBeenCalled()
    expect(server.markRead).not.toHaveBeenCalled()
  })

  it('does not apply when the server cannot say, and keeps publishing locked so the pull is retried', async () => {
    setReadSyncEnabled(true)
    vi.spyOn(console, 'warn').mockImplementation(() => {})
    const server = { ...fakeServer([]), ...fakeApplied() }
    server.getAppliedCreatedAt.mockRejectedValueOnce(new Error('429'))
    const session = newReadSyncSession(NPUB)
    await expect(pullReadStatus({ ...auth, ...server, session, fetchRemote: event(['a'], 100) })).resolves.toBe(false)
    expect(server.markRead).not.toHaveBeenCalled()
    expect(storage.data.has(WATERMARK)).toBe(false)
    expect(session).toMatchObject({ pulled: false, remote: ['a'] })

    // The retry applies it.
    await expect(pullReadStatus({ ...auth, ...server, session, fetchRemote: event(['a'], 100) })).resolves.toBe(true)
    expect(server.markRead).toHaveBeenCalledWith(['a'])
  })

  it('still finishes the pull when recording the applied event fails', async () => {
    setReadSyncEnabled(true)
    vi.spyOn(console, 'warn').mockImplementation(() => {})
    const applied = fakeApplied()
    const server = { ...fakeServer([]), ...applied }
    server.advanceAppliedCreatedAt.mockRejectedValueOnce(new Error('429'))
    await expect(pullReadStatus({ ...auth, ...server, session: newReadSyncSession(NPUB), fetchRemote: event(['a'], 100) })).resolves.toBe(true)
    expect(server.markRead).toHaveBeenCalledWith(['a'])
    expect(storage.data.get(WATERMARK)).toBe('100')
    expect(applied.value).toBeNull()

    // A later session in this browser records it without applying it again.
    await expect(pullReadStatus({ ...auth, ...server, session: newReadSyncSession(NPUB), fetchRemote: event(['a'], 100) })).resolves.toBe(true)
    expect(server.markRead).toHaveBeenCalledOnce()
    expect(applied.advanceAppliedCreatedAt).toHaveBeenCalledTimes(2)
    expect(applied.advanceAppliedCreatedAt).toHaveBeenLastCalledWith(100)
    expect(applied.value).toBe(100)
  })

  it('neither applies nor records an event dated far in the future', async () => {
    setReadSyncEnabled(true)
    const future = Math.floor(Date.now() / 1000) + 3600
    const server = { ...fakeServer([]), ...fakeApplied() }
    const session = newReadSyncSession(NPUB)
    await expect(pullReadStatus({ ...auth, ...server, session, fetchRemote: event(['a'], future) })).resolves.toBe(true)
    expect(server.markRead).not.toHaveBeenCalled()
    expect(server.getAppliedCreatedAt).not.toHaveBeenCalled()
    expect(server.advanceAppliedCreatedAt).not.toHaveBeenCalled()
    expect(storage.data.has(WATERMARK)).toBe(false)
    expect(session).toMatchObject({ pulled: true, remote: ['a'] })

    // A publish after it goes later still, and is not recorded either.
    const publish = ok(future + 1)
    await publishReadStatusIfChanged({ ...auth, ...server, session, signEvent, publish, fetchLocal: async () => ['b'] })
    expect(publish).toHaveBeenCalledOnce()
    expect(server.advanceAppliedCreatedAt).not.toHaveBeenCalled()
  })

  it('records publishes and withdrawals, only for the account still signed in', async () => {
    setReadSyncEnabled(true)
    const server = fakeApplied(100)
    const session = pulledSession()
    await publishReadStatusIfChanged({ ...auth, ...server, session, signEvent, publish: ok(200), fetchLocal: async () => ['a'] })
    expect(server.value).toBe(200)

    setReadSyncEnabled(false)
    await withdrawReadStatus({ ...auth, ...server, session, signEvent, publish: ok(300) })
    expect(server.value).toBe(300)

    setReadSyncEnabled(true)
    let npub = NPUB
    const publish = vi.fn(async (_guids: string[], sign: (event: UnsignedEvent) => Promise<Event>) => {
      await sign({} as UnsignedEvent)
      npub = 'npub1other'
      return { success: true, createdAt: 400 }
    })
    await publishReadStatusIfChanged({ ...auth, ...server, npub: () => npub, session, signEvent, publish, fetchLocal: async () => ['b'] })
    expect(publish).toHaveBeenCalledOnce()
    expect(server.advanceAppliedCreatedAt).toHaveBeenCalledTimes(2)
    expect(server.value).toBe(300)
  })

  it('never lowers what the server holds when an older publish is recorded late', async () => {
    setReadSyncEnabled(true)
    const server = fakeApplied(500)
    await publishReadStatusIfChanged({ ...auth, ...server, session: pulledSession(), signEvent, publish: ok(200), fetchLocal: async () => ['a'] })
    expect(server.advanceAppliedCreatedAt).toHaveBeenCalledWith(200)
    expect(server.value).toBe(500)
  })

  it("applies an account's list that another account's watermark here would have hidden, before recording publishes", async () => {
    setReadSyncEnabled(true)
    // Account A applied an event at 500 in this browser, under its own key and,
    // in an older build, under the shared one.
    storage.data.set(APPLIED_CREATEDAT_PREFIX + readStatusWatermarkTag('npub1a'), '500')
    storage.data.set(LEGACY_WATERMARK, '500')
    const applied = fakeApplied()
    const server = { ...fakeServer([]), ...applied }
    const session = newReadSyncSession(NPUB)
    await expect(pullReadStatus({ ...auth, ...server, session, fetchRemote: event(['b-desktop'], 400) })).resolves.toBe(true)
    expect(server.markRead).toHaveBeenCalledWith(['b-desktop'])
    expect(applied.value).toBe(400)
    expect(storage.data.get(WATERMARK)).toBe('400')
    expect(storage.data.get(LEGACY_WATERMARK)).toBe('500')

    // The publish after it is recorded only once the list it builds on is applied.
    await publishReadStatusIfChanged({ ...auth, ...server, session, signEvent, publish: ok(401), fetchLocal: async () => ['b-web'] })
    expect(applied.value).toBe(401)
    expect(server.markRead).toHaveBeenCalledTimes(1)
  })

  it('neither applies nor re-records an event the server has when only the shared watermark here covers it', async () => {
    setReadSyncEnabled(true)
    storage.data.set(LEGACY_WATERMARK, '500')
    const server = { ...fakeServer([]), ...fakeApplied(400) }
    await expect(pullReadStatus({ ...auth, ...server, session: newReadSyncSession(NPUB), fetchRemote: event(['a'], 400) })).resolves.toBe(true)
    expect(server.getAppliedCreatedAt).toHaveBeenCalledOnce()
    expect(server.markRead).not.toHaveBeenCalled()
    expect(server.advanceAppliedCreatedAt).not.toHaveBeenCalled()
    expect(storage.data.get(WATERMARK)).toBe('400')
  })

  it('tells the server about an event this browser applied before the server kept a record, once per session', async () => {
    setReadSyncEnabled(true)
    storage.data.set(WATERMARK, '100')
    const applied = fakeApplied()
    const server = { ...fakeServer([]), ...applied }
    const session = newReadSyncSession(NPUB)
    for (let pull = 0; pull < 2; pull++) {
      await expect(pullReadStatus({ ...auth, ...server, session, fetchRemote: event(['a'], 100) })).resolves.toBe(true)
    }
    expect(server.markRead).not.toHaveBeenCalled()
    expect(server.getAppliedCreatedAt).not.toHaveBeenCalled()
    expect(applied.advanceAppliedCreatedAt).toHaveBeenCalledOnce()
    expect(applied.advanceAppliedCreatedAt).toHaveBeenCalledWith(100)
    expect(applied.value).toBe(100)
    expect(session).toMatchObject({ pulled: true, remote: ['a'] })

    // A new browser of the account then leaves it alone.
    storage.data.delete(WATERMARK)
    const other = { ...fakeServer([]), ...applied }
    await pullReadStatus({ ...auth, ...other, session: newReadSyncSession(NPUB), fetchRemote: event(['a'], 100) })
    expect(other.markRead).not.toHaveBeenCalled()
  })
})
