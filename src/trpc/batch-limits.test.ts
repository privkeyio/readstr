import { createTRPCUntypedClient, httpBatchLink } from '@trpc/client'
import superjson from 'superjson'
import { describe, expect, it } from 'vitest'
import { appRouter, type AppRouter } from '@/server/api/root'
import { countTrpcCalls } from '@/server/http/trpc-batch'
import { MAX_TRPC_BATCH_SIZE, TRPC_MAX_URL_LENGTH } from './batch-limits'

describe('tRPC batch limits', () => {
  it('keeps the client URL cap below the server batch cap for every procedure', () => {
    const shortest = Math.min(...Object.keys(appRouter._def.procedures).map(p => p.length))
    const fixed = '/api/trpc/'.length + '?batch=1'.length
    const maxCalls = Math.floor((TRPC_MAX_URL_LENGTH - fixed + 1) / (shortest + 1))
    expect(maxCalls).toBeLessThanOrEqual(MAX_TRPC_BATCH_SIZE)
  })

  it('splits a burst of client calls into batches the server accepts', async () => {
    const urls: string[] = []
    const client = createTRPCUntypedClient<AppRouter>({
      transformer: superjson,
      links: [
        httpBatchLink({
          url: 'http://localhost/api/trpc',
          maxURLLength: TRPC_MAX_URL_LENGTH,
          fetch: async input => {
            const url = String(input)
            urls.push(url)
            const n = countTrpcCalls(url, '/api/trpc') ?? 0
            const body = Array.from({ length: n }, () => ({ result: { data: { json: null } } }))
            return new Response(JSON.stringify(body), {
              headers: { 'content-type': 'application/json' },
            })
          },
        }),
      ],
    })

    await Promise.all([
      ...Array.from({ length: 500 }, (_, i) => client.mutation('feed.markAsRead', { itemId: `item-${i}` })),
      ...Array.from({ length: 500 }, () => client.query('feed.getFeeds')),
    ])

    expect(urls.length).toBeGreaterThan(1)
    for (const url of urls) {
      expect(countTrpcCalls(url, '/api/trpc')).toBeLessThanOrEqual(MAX_TRPC_BATCH_SIZE)
    }
  })
})
