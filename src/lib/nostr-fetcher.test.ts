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
