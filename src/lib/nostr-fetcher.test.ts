import { createServer, type Server } from 'net'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'

import { NostrFeedFetcher } from './nostr-fetcher'
import { SafeRelayWebSocket } from './safe-relays'

describe('NostrFeedFetcher websocketImplementation', () => {
  let server: Server
  let port: number
  let connections: number

  beforeEach(async () => {
    connections = 0
    server = createServer((socket) => {
      connections++
      socket.destroy()
    })
    await new Promise<void>((resolve) => server.listen(0, resolve))
    port = (server.address() as { port: number }).port
  })

  afterEach(async () => {
    await new Promise((resolve) => server.close(resolve))
  })

  it('opens relay sockets through the supplied implementation', async () => {
    const fetcher = new NostrFeedFetcher([`ws://localhost:${port}`], { websocketImplementation: SafeRelayWebSocket })
    try {
      expect(await fetcher.getPopularUsers(1)).toEqual([])
    } finally {
      fetcher.close()
    }
    expect(connections).toBe(0)
  })
})

describe('NostrFeedFetcher profile queries', () => {
  // A relay that ignores the filter limit and keeps streaming events.
  function floodingFetcher(relays: string[]) {
    const fetcher = new NostrFeedFetcher(relays)
    const closed: string[] = []
    let delivered = 0
    const pool = {
      subscribeEose: (_: string[], __: unknown, params: { onevent: (e: unknown) => void; onclose: () => void }) => {
        let stopped = false
        void (async () => {
          for (let i = 0; i < 10_000 && !stopped; i++) {
            delivered++
            params.onevent({
              pubkey: i.toString(16).padStart(64, '0'),
              content: JSON.stringify({ name: `alice${i}`, about: 'x' }),
            })
            if (i % 50 === 0) await Promise.resolve()
          }
          params.onclose()
        })()
        return {
          close: async (reason: string) => {
            stopped = true
            closed.push(reason)
          },
        }
      },
      close: () => {},
    }
    ;(fetcher as unknown as { pool: typeof pool }).pool = pool
    return { fetcher, closed, delivered: () => delivered }
  }

  it('stops collecting search results once the bound is reached', async () => {
    const { fetcher, closed, delivered } = floodingFetcher(['wss://a.example', 'wss://b.example'])
    const profiles = await fetcher.searchProfiles('alice', 10)
    expect(profiles).toHaveLength(10)
    expect(closed).toEqual(['enough events'])
    expect(delivered()).toBeLessThan(200)
  })

  it('stops collecting popular users after the first 100 events', async () => {
    const { fetcher, closed, delivered } = floodingFetcher(['wss://a.example'])
    expect(await fetcher.getPopularUsers(50)).toHaveLength(50)
    expect(closed).toEqual(['enough events'])
    expect(delivered()).toBeLessThan(200)
  })
})
