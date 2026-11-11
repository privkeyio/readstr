import { createTRPCUntypedClient } from '@trpc/client'
import superjson from 'superjson'
import { describe, expect, it } from 'vitest'
import { appRouter, type AppRouter } from '@/server/api/root'
import { countTrpcCalls } from '@/server/http/trpc-batch'
import { MAX_TRPC_BATCH_SIZE, TRPC_MAX_URL_LENGTH } from './batch-limits'
import { createTrpcHttpLinks, singleQueryBatchUrlLength } from './links'

describe('tRPC batch limits', () => {
  it('keeps the client URL cap below the server batch cap for every procedure', () => {
    const shortest = Math.min(...Object.keys(appRouter._def.procedures).map(p => p.length))
    const fixed = '/api/trpc/'.length + '?batch=1'.length
    const maxCalls = Math.floor((TRPC_MAX_URL_LENGTH - fixed + 1) / (shortest + 1))
    expect(maxCalls).toBeLessThanOrEqual(MAX_TRPC_BATCH_SIZE)
  })

})

// A client built with the real links whose fetch records each request URL and
// answers with one null result per call it carried.
function recordingClient() {
  const urls: string[] = []
  const client = createTRPCUntypedClient<AppRouter>({
    transformer: superjson,
    links: [
      createTrpcHttpLinks<AppRouter>({
        // The real client uses this relative URL, so batch splitting here
        // sees the same URL lengths it does in the browser.
        url: '/api/trpc',
        fetch: async input => {
          const url = new URL(String(input), 'http://localhost')
          urls.push(url.href)
          const result = { result: { data: { json: null } } }
          if (!url.searchParams.has('batch')) return Response.json(result)
          const n = countTrpcCalls(url.href, '/api/trpc') ?? 0
          return Response.json(Array.from({ length: n }, () => result))
        },
      }),
    ],
  })
  return { client, urls }
}

describe('tRPC client links', () => {
  it('splits a burst of client calls into batches the server accepts', async () => {
    const { client, urls } = recordingClient()

    await Promise.all([
      ...Array.from({ length: 500 }, (_, i) => client.mutation('feed.markAsRead', { itemId: `item-${i}` })),
      ...Array.from({ length: 500 }, () => client.query('feed.getFeeds')),
    ])

    expect(urls.length).toBeGreaterThan(1)
    for (const url of urls) {
      expect(countTrpcCalls(url, '/api/trpc')).toBeLessThanOrEqual(MAX_TRPC_BATCH_SIZE)
    }
  })

  it('sends a single query too long to batch unbatched instead of rejecting it', async () => {
    const { client, urls } = recordingClient()
    const input = { tags: Array.from({ length: 300 }, (_, i) => `tag-${i}`) }
    expect(singleQueryBatchUrlLength('/api/trpc', 'feed.getFeedItems', input)).toBeGreaterThan(
      TRPC_MAX_URL_LENGTH
    )

    await expect(
      Promise.all([client.query('feed.getFeedItems', input), client.query('feed.getFeeds')])
    ).resolves.toHaveLength(2)

    const long = urls.find(u => u.includes('feed.getFeedItems'))!
    expect(new URL(long).searchParams.has('batch')).toBe(false)
    expect(countTrpcCalls(long, '/api/trpc')).toBe(1)
    // The short query still goes through the batch link.
    expect(urls.some(u => u.includes('feed.getFeeds?batch=1'))).toBe(true)
  })

  it('measures the URL the batch link would build', () => {
    expect(singleQueryBatchUrlLength('/api/trpc', 'feed.getFeeds', undefined)).toBe(
      '/api/trpc/feed.getFeeds?batch=1'.length
    )
  })
})
