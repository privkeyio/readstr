import { createHash } from 'crypto'
import { createServer, type Server, type Socket } from 'net'
import { WebSocket as UndiciWebSocket } from 'undici'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

const mocks = vi.hoisted(() => {
  const shared = { close: vi.fn(), name: 'shared' }
  const created: { relays: string[]; options: unknown; close: ReturnType<typeof vi.fn> }[] = []
  return { resolve: vi.fn(), resolverOptions: [] as unknown[], shared, created, closeThrows: false, allowLoopback: false }
})

vi.mock('dns/promises', () => ({
  Resolver: class {
    constructor(options: unknown) {
      mocks.resolverOptions.push(options)
    }
    resolve4 = (hostname: string) => mocks.resolve(hostname, 4)
    resolve6 = (hostname: string) => mocks.resolve(hostname, 6)
  },
}))

// The live tests reach a local server via trusted.test, so loopback is let
// through only while allowLoopback is set.
vi.mock('./safe-fetch', async (importOriginal) => {
  const actual = await importOriginal<typeof import('./safe-fetch')>()
  return {
    ...actual,
    isBlockedAddress: (ip: string) => !(mocks.allowLoopback && ip === '127.0.0.1') && actual.isBlockedAddress(ip),
  }
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

const DNS: Record<string, { 4?: string[]; 6?: string[] }> = {
  'relay.example': { 4: ['93.184.216.34'] },
  'evil.example': { 4: ['10.0.0.5'] },
  'mixed.example': { 4: ['93.184.216.34'], 6: ['::1'] },
  'v6only.example': { 6: ['2606:2800:220:1:248:1893:25c8:1946'] },
  'v6private.example': { 4: ['93.184.216.34'], 6: ['fd00::1'] },
  'trusted.test': { 4: ['127.0.0.1'] },
}

function dnsError(code: string) {
  return Object.assign(new Error(code), { code })
}

const lookupsOf = (family: 4 | 6) => mocks.resolve.mock.calls.filter(([, f]) => f === family).length

beforeEach(() => {
  mocks.resolve.mockReset()
  mocks.resolve.mockImplementation(async (hostname: string, family: 4 | 6) => {
    const host = hostname.replace(/\.+$/, '')
    const records = DNS[host] ?? (/^r\d+\.example$/.test(host) ? { 4: ['93.184.216.34'] } : undefined)
    if (!records) throw dnsError('ENOTFOUND')
    const list = records[family]
    if (!list) throw dnsError('ENODATA')
    return list
  })
  mocks.created.length = 0
  mocks.closeThrows = false
  mocks.allowLoopback = false
  mocks.shared.close.mockReset()
})

describe('filterSafeRelays', () => {
  it('accepts a public wss relay', async () => {
    expect(await filterSafeRelays(['wss://relay.example'])).toEqual(['wss://relay.example/'])
  })

  it('accepts a relay with only AAAA records', async () => {
    expect(await filterSafeRelays(['wss://v6only.example'])).toEqual(['wss://v6only.example/'])
  })

  it('resolves with a bounded c-ares resolver', () => {
    expect(mocks.resolverOptions).toEqual([{ timeout: 2000, tries: 1 }])
  })

  it('uses the family that answered when the other errors', async () => {
    mocks.resolve.mockImplementation(async (_: string, family: 4 | 6) => {
      if (family === 6) throw dnsError('ETIMEOUT')
      return ['93.184.216.34']
    })
    expect(await filterSafeRelays(['wss://relay.example'])).toEqual(['wss://relay.example/'])
  })

  it('still rejects a private address from the family that answered', async () => {
    mocks.resolve.mockImplementation(async (_: string, family: 4 | 6) => {
      if (family === 6) throw dnsError('ESERVFAIL')
      return ['10.0.0.5']
    })
    expect(await filterSafeRelays(['wss://relay.example'])).toEqual([])
  })

  it('fails closed when both families error', async () => {
    mocks.resolve.mockImplementation(async () => {
      throw dnsError('ETIMEOUT')
    })
    expect(await filterSafeRelays(['wss://relay.example'])).toEqual([])
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
    'wss://v6private.example',
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

  it('collapses hash and trailing-dot variants of one relay', async () => {
    const relays = ['wss://relay.example/', 'wss://relay.example./', 'wss://relay.example../', 'wss://relay.example/#x']
    expect(await filterSafeRelays(relays)).toEqual(['wss://relay.example/'])
    expect(lookupsOf(4)).toBe(1)
  })

  it('keeps query variants apart, capped, with one lookup for the shared host', async () => {
    const relays = ['wss://relay.example/?tenant=a', 'wss://relay.example/?tenant=b']
    for (let i = 0; i < 20; i++) relays.push(`wss://relay.example/?${i}`)
    const result = await filterSafeRelays(relays)
    expect(result.slice(0, 2)).toEqual(['wss://relay.example/?tenant=a', 'wss://relay.example/?tenant=b'])
    expect(result).toHaveLength(MAX_REQUEST_RELAYS)
    expect(lookupsOf(4)).toBe(1)
  })

  it('keeps relays on distinct ports or paths apart', async () => {
    const result = await filterSafeRelays(['wss://relay.example', 'wss://relay.example:444', 'wss://relay.example/a'])
    expect(result).toEqual(['wss://relay.example/', 'wss://relay.example:444/', 'wss://relay.example/a'])
  })

  it('caps candidates before resolving any of them', async () => {
    const relays = Array.from({ length: 20 }, (_, i) => `wss://r${i}.example`)
    expect(await filterSafeRelays(relays)).toHaveLength(MAX_REQUEST_RELAYS)
    expect(lookupsOf(4)).toBe(MAX_REQUEST_RELAYS)
  })

  it('treats a hung lookup as unsafe', async () => {
    vi.useFakeTimers()
    const release: (() => void)[] = []
    try {
      mocks.resolve.mockImplementation(() => new Promise<string[]>((resolve) => release.push(() => resolve([]))))
      const pending = filterSafeRelays(['wss://slow.example'])
      await vi.advanceTimersByTimeAsync(5000)
      expect(await pending).toEqual([])
    } finally {
      vi.useRealTimers()
      // Free the lookup slot; c-ares itself always settles.
      release.forEach((resolve) => resolve())
    }
  })

  it('caps lookups in flight across concurrent calls', async () => {
    const pending: (() => void)[] = []
    mocks.resolve.mockImplementation(
      () => new Promise<string[]>((resolve) => pending.push(() => resolve(['93.184.216.34']))),
    )
    const calls = Array.from({ length: 3 }, (_, c) =>
      filterSafeRelays(Array.from({ length: MAX_REQUEST_RELAYS }, (_, i) => `wss://r${c * 100 + i}.example`)),
    )
    await vi.waitFor(() => expect(lookupsOf(4)).toBe(16))
    await new Promise((resolve) => setTimeout(resolve, 20))
    expect(lookupsOf(4)).toBe(16)
    while (lookupsOf(4) < 3 * MAX_REQUEST_RELAYS || pending.length) {
      pending.splice(0).forEach((resolve) => resolve())
      await new Promise((resolve) => setTimeout(resolve, 0))
    }
    for (const result of await Promise.all(calls)) expect(result).toHaveLength(MAX_REQUEST_RELAYS)
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
    mocks.allowLoopback = true
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

  // A relay that completes the upgrade but never answers our Close frame:
  // undici would otherwise keep the socket open indefinitely.
  it('destroys open sockets whose peer ignores the close handshake', async () => {
    const serverSockets: Socket[] = []
    const server = createServer((socket) => {
      serverSockets.push(socket)
      let head = ''
      socket.on('data', (chunk) => {
        if (head.includes('\r\n\r\n')) return // ignore every frame after the upgrade
        head += chunk.toString('latin1')
        const key = /sec-websocket-key: *(\S+)/i.exec(head)?.[1]
        if (!head.includes('\r\n\r\n') || !key) return
        const accept = createHash('sha1').update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`).digest('base64')
        socket.write(
          `HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`,
        )
      })
    })
    await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
    const { port } = server.address() as { port: number }
    mocks.allowLoopback = true
    try {
      await withRequestFetcher(['wss://relay.example'], async (fetcher) => {
        const impl = (fetcher as unknown as { options: { websocketImplementation: typeof WebSocket } }).options
          .websocketImplementation
        const ws = new impl(`ws://trusted.test:${port}`)
        await new Promise((resolve) => ws.addEventListener('open', resolve))
      })
      const returnedAt = Date.now()
      await vi.waitFor(() => expect(serverSockets[0].readableEnded || serverSockets[0].destroyed).toBe(true), {
        timeout: 2500,
      })
      expect(Date.now() - returnedAt).toBeLessThan(2000)
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
