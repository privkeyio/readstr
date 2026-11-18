import { createServer, type Server, type Socket } from 'net'
import { WebSocket as UndiciWebSocket } from 'undici'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

const mocks = vi.hoisted(() => {
  const shared = { close: vi.fn(), name: 'shared' }
  const created: { relays: string[]; options: unknown; close: ReturnType<typeof vi.fn> }[] = []
  return { lookup: vi.fn(), shared, created, closeThrows: false }
})

vi.mock('dns/promises', () => ({ lookup: mocks.lookup }))

// trusted.test reaches the local test server through the relay dispatcher;
// every other host goes through the real blocked-address lookup.
vi.mock('./safe-fetch', async (importOriginal) => {
  const actual = await importOriginal<typeof import('./safe-fetch')>()
  const blockedAddressLookup: typeof actual.blockedAddressLookup = (hostname, options, callback) => {
    if (hostname !== 'trusted.test') return actual.blockedAddressLookup(hostname, options, callback)
    if ((options as { all?: boolean }).all) callback(null, [{ address: '127.0.0.1', family: 4 }])
    else callback(null, '127.0.0.1', 4)
  }
  return { ...actual, blockedAddressLookup }
})

vi.mock('./nostr-fetcher', () => {
  class NostrFeedFetcher {
    close = vi.fn(() => {
      if (mocks.closeThrows) throw new Error('close failed')
    })
    constructor(public relays: string[], public options: unknown) {
      mocks.created.push(this)
    }
  }
  return { NostrFeedFetcher, getNostrFetcher: () => mocks.shared }
})

import { filterSafeRelays, MAX_REQUEST_RELAYS, SafeRelayWebSocket, withRequestFetcher } from './safe-relays'

const PUBLIC_DNS: Record<string, string> = {
  'relay.example': '93.184.216.34',
  'evil.example': '10.0.0.5',
  'mixed.example': '93.184.216.34',
}

beforeEach(() => {
  mocks.lookup.mockReset()
  mocks.lookup.mockImplementation(async (hostname: string) => {
    if (hostname === 'mixed.example') {
      return [{ address: '93.184.216.34', family: 4 }, { address: '::1', family: 6 }]
    }
    const address = PUBLIC_DNS[hostname] ?? (/^r\d+\.example$/.test(hostname) ? '93.184.216.34' : undefined)
    if (!address) throw new Error('ENOTFOUND')
    return [{ address, family: 4 }]
  })
  mocks.created.length = 0
  mocks.closeThrows = false
  mocks.shared.close.mockReset()
})

describe('filterSafeRelays', () => {
  it('accepts a public wss relay', async () => {
    expect(await filterSafeRelays(['wss://relay.example'])).toEqual(['wss://relay.example/'])
  })

  it.each([
    'ws://relay.example',
    'http://relay.example',
    'https://relay.example',
    'wss://user:pass@relay.example',
    'wss://user@relay.example',
    'wss://127.0.0.1',
    'wss://127.0.0.1:5432',
    'wss://0x7f.1',
    'wss://[::1]',
    'wss://[::ffff:127.0.0.1]',
    'wss://10.1.2.3',
    'wss://192.168.1.1',
    'wss://169.254.169.254',
    'wss://evil.example',
    'wss://mixed.example',
    'wss://unresolvable.example',
    'wss://relay.example\n',
    'wss://relay .example',
    'not a url',
    '',
  ])('rejects %j', async (relay) => {
    expect(await filterSafeRelays([relay])).toEqual([])
  })

  it('dedupes equivalent URLs and caps the result', async () => {
    const relays = ['wss://relay.example', 'wss://RELAY.example/', 'wss://relay.example']
    for (let i = 0; i < 15; i++) relays.push(`wss://r${i}.example`)
    const result = await filterSafeRelays(relays)
    expect(result).toHaveLength(MAX_REQUEST_RELAYS)
    expect(result[0]).toBe('wss://relay.example/')
    expect(new Set(result).size).toBe(result.length)
  })

  it('collapses query, hash and trailing-dot variants of one relay', async () => {
    const relays = ['wss://relay.example/', 'wss://relay.example./', 'wss://relay.example/#x']
    for (let i = 0; i < 20; i++) relays.push(`wss://relay.example/?${i}`)
    expect(await filterSafeRelays(relays)).toEqual(['wss://relay.example/'])
    expect(mocks.lookup).toHaveBeenCalledTimes(1)
  })

  it('keeps relays on distinct ports or paths apart', async () => {
    const result = await filterSafeRelays(['wss://relay.example', 'wss://relay.example:444', 'wss://relay.example/a'])
    expect(result).toEqual(['wss://relay.example/', 'wss://relay.example:444/', 'wss://relay.example/a'])
  })

  it('caps candidates before resolving any of them', async () => {
    const relays = Array.from({ length: 20 }, (_, i) => `wss://r${i}.example`)
    expect(await filterSafeRelays(relays)).toHaveLength(MAX_REQUEST_RELAYS)
    expect(mocks.lookup).toHaveBeenCalledTimes(MAX_REQUEST_RELAYS)
  })

  it('treats a hung lookup as unsafe', async () => {
    vi.useFakeTimers()
    try {
      mocks.lookup.mockImplementation(() => new Promise(() => {}))
      const pending = filterSafeRelays(['wss://slow.example'])
      await vi.advanceTimersByTimeAsync(5000)
      expect(await pending).toEqual([])
    } finally {
      vi.useRealTimers()
    }
  })
})

describe('withRequestFetcher', () => {
  it('uses the shared fetcher when no relays are given', async () => {
    const fn = vi.fn(async (f: unknown) => f)
    expect(await withRequestFetcher(undefined, fn)).toBe(mocks.shared)
    expect(await withRequestFetcher([], fn)).toBe(mocks.shared)
    expect(mocks.created).toHaveLength(0)
    expect(mocks.shared.close).not.toHaveBeenCalled()
  })

  it('uses the shared fetcher when every relay is rejected', async () => {
    const fn = vi.fn(async (f: unknown) => f)
    expect(await withRequestFetcher(['ws://127.0.0.1:5432', 'wss://evil.example'], fn)).toBe(mocks.shared)
    expect(mocks.created).toHaveLength(0)
    expect(mocks.shared.close).not.toHaveBeenCalled()
  })

  it('builds a per-request fetcher from only the safe relays and closes it', async () => {
    const result = await withRequestFetcher(['wss://relay.example', 'wss://10.0.0.1'], async () => 'ok')
    expect(result).toBe('ok')
    expect(mocks.created).toHaveLength(1)
    expect(mocks.created[0].relays).toEqual(['wss://relay.example/'])
    const impl = (mocks.created[0].options as { websocketImplementation: typeof WebSocket }).websocketImplementation
    expect(impl.prototype).toBeInstanceOf(SafeRelayWebSocket)
    expect(mocks.created[0].close).toHaveBeenCalledTimes(1)
    expect(mocks.shared.close).not.toHaveBeenCalled()
  })

  it('closes the per-request fetcher when the call throws', async () => {
    await expect(
      withRequestFetcher(['wss://relay.example'], async () => {
        throw new Error('boom')
      }),
    ).rejects.toThrow('boom')
    expect(mocks.created).toHaveLength(1)
    expect(mocks.created[0].close).toHaveBeenCalledTimes(1)
    expect(mocks.shared.close).not.toHaveBeenCalled()
  })

  it('does not let a failing close mask the original error', async () => {
    mocks.closeThrows = true
    await expect(
      withRequestFetcher(['wss://relay.example'], async () => {
        throw new Error('boom')
      }),
    ).rejects.toThrow('boom')
    expect(await withRequestFetcher(['wss://relay.example'], async () => 'ok')).toBe('ok')
  })

  // A relay that never completes the handshake: nostr-tools would give up
  // after its connect timeout without closing the socket.
  it('closes sockets still mid-handshake when the call returns', async () => {
    const serverSockets: Socket[] = []
    const server = createServer((socket) => {
      serverSockets.push(socket)
      socket.resume()
    })
    await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
    const { port } = server.address() as { port: number }
    try {
      await withRequestFetcher(['wss://relay.example'], async (fetcher) => {
        const impl = (fetcher as unknown as { options: { websocketImplementation: typeof WebSocket } }).options
          .websocketImplementation
        new impl(`ws://trusted.test:${port}`)
        await vi.waitFor(() => expect(serverSockets).toHaveLength(1))
      })
      await vi.waitFor(() => expect(serverSockets[0].readableEnded || serverSockets[0].destroyed).toBe(true))
    } finally {
      serverSockets.forEach((s) => s.destroy())
      await new Promise((resolve) => server.close(resolve))
    }
  })
})

describe('SafeRelayWebSocket', () => {
  let server: Server
  let port: number
  let connections: number
  const sockets: Socket[] = []

  beforeEach(async () => {
    connections = 0
    server = createServer((socket) => {
      connections++
      sockets.push(socket)
      socket.destroy()
    })
    await new Promise<void>((resolve) => server.listen(0, resolve))
    port = (server.address() as { port: number }).port
  })

  afterEach(async () => {
    sockets.splice(0).forEach((s) => s.destroy())
    await new Promise((resolve) => server.close(resolve))
  })

  function waitForClose(ws: { addEventListener: (type: string, cb: () => void) => void }) {
    return new Promise<void>((resolve) => ws.addEventListener('close', () => resolve()))
  }

  // localhost goes through DNS at connect time, the same path a rebinding
  // hostname takes after passing the pre-check.
  it('refuses to connect when the hostname resolves to a blocked address', async () => {
    const ws = new SafeRelayWebSocket(`ws://localhost:${port}`)
    await waitForClose(ws)
    expect(connections).toBe(0)
  })

  it.each(['127.0.0.1', '[::1]'])('refuses the blocked IP literal %s without connecting', (host) => {
    expect(() => new SafeRelayWebSocket(`ws://${host}:${port}`)).toThrow('disallowed address')
    expect(connections).toBe(0)
  })

  it('control: an unguarded socket does reach the local server', async () => {
    const ws = new UndiciWebSocket(`ws://localhost:${port}`)
    await waitForClose(ws)
    expect(connections).toBe(1)
  })
})
