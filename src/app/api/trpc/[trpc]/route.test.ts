import { beforeEach, describe, expect, it, vi } from 'vitest'

// The cap must run before tRPC parses input or builds auth context.
const fetchRequestHandler = vi.fn(async () => new Response('handled'))
vi.mock('@trpc/server/adapters/fetch', () => ({ fetchRequestHandler }))
vi.mock('@/server/api/root', () => ({ appRouter: {} }))
vi.mock('@/server/auth/request-context', () => ({ buildRequestContext: vi.fn() }))

const { GET, POST } = await import('./route')
const { __resetRateLimitForTests } = await import('@/server/rate-limit')
const { MAX_TRPC_BATCH_SIZE } = await import('@/trpc/batch-limits')

const URL_ = 'http://localhost/api/trpc/guide.submitFeed'

beforeEach(() => {
  fetchRequestHandler.mockClear()
  __resetRateLimitForTests()
})

describe('tRPC route body cap', () => {
  it('rejects an oversize array body with 413 before tRPC sees it', async () => {
    const body = JSON.stringify({ tags: new Array(500_000).fill(0) })
    const res = await POST(new Request(URL_, { method: 'POST', body }) as never)
    expect(res.status).toBe(413)
    expect(fetchRequestHandler).not.toHaveBeenCalled()
  })

  it('passes a normal body through to tRPC', async () => {
    const res = await POST(new Request(URL_, { method: 'POST', body: '{"tags":["nostr"]}' }) as never)
    expect(res.status).toBe(200)
    expect(fetchRequestHandler).toHaveBeenCalledOnce()
  })

  it('rejects a malformed Content-Length with 400', async () => {
    const req = new Request(URL_, {
      method: 'POST',
      headers: { 'content-length': '12abc' },
      body: new ReadableStream({
        start(controller) {
          controller.enqueue(new Uint8Array(1))
          controller.close()
        },
      }),
      duplex: 'half',
    } as RequestInit)
    const res = await POST(req as never)
    expect(res.status).toBe(400)
    expect(fetchRequestHandler).not.toHaveBeenCalled()
  })

  it('passes a GET through without reading a body', async () => {
    const req = new Request(`${URL_}?input=%7B%7D`)
    const res = await GET(req as never)
    expect(res.status).toBe(200)
    expect(fetchRequestHandler).toHaveBeenCalledOnce()
    const opts = (fetchRequestHandler.mock.calls[0] as unknown as [{ req: Request }])[0]
    expect(opts.req).toBe(req)
  })

  it('returns 400 when reading the body fails', async () => {
    const req = new Request(URL_, {
      method: 'POST',
      body: new ReadableStream({
        pull(controller) {
          controller.error(new Error('client aborted'))
        },
      }),
      duplex: 'half',
    } as RequestInit)
    const res = await POST(req as never)
    expect(res.status).toBe(400)
    expect(fetchRequestHandler).not.toHaveBeenCalled()
  })
})

const batchUrl = (n: number) =>
  `http://localhost/api/trpc/${new Array(n).fill('guide.getGuideTags').join(',')}?batch=1`
const fromIp = (url: string, ip = '203.0.113.7') =>
  new Request(url, { headers: { 'x-forwarded-for': ip } })

describe('tRPC route batch cap', () => {
  it('rejects a batch over the cap with 400 before tRPC sees it', async () => {
    const res = await GET(fromIp(batchUrl(MAX_TRPC_BATCH_SIZE + 1)) as never)
    expect(res.status).toBe(400)
    expect(await res.json()).toEqual({ error: 'Too many calls in one batch' })
    expect(fetchRequestHandler).not.toHaveBeenCalled()
  })

  it('counts percent-encoded commas toward the cap', async () => {
    const path = new Array(MAX_TRPC_BATCH_SIZE + 1).fill('guide.getGuideTags').join('%2C')
    const res = await GET(fromIp(`http://localhost/api/trpc/${path}?batch=1`) as never)
    expect(res.status).toBe(400)
    expect(fetchRequestHandler).not.toHaveBeenCalled()
  })

  it('rejects an oversize mutation batch without reading the body', async () => {
    const req = new Request(batchUrl(MAX_TRPC_BATCH_SIZE + 1), {
      method: 'POST',
      body: new ReadableStream({
        pull() {
          throw new Error('body must not be read')
        },
      }),
      duplex: 'half',
    } as RequestInit)
    const res = await POST(req as never)
    expect(res.status).toBe(400)
    expect(await res.json()).toEqual({ error: 'Too many calls in one batch' })
  })

  it('passes a batch at the cap through to tRPC', async () => {
    const res = await GET(fromIp(batchUrl(MAX_TRPC_BATCH_SIZE)) as never)
    expect(res.status).toBe(200)
    expect(fetchRequestHandler).toHaveBeenCalledOnce()
  })

  it('rejects an invalid percent-encoded path with 400', async () => {
    const res = await GET(fromIp('http://localhost/api/trpc/guide.getGuideTags%E0%A4%A') as never)
    expect(res.status).toBe(400)
    expect(fetchRequestHandler).not.toHaveBeenCalled()
  })
})

describe('tRPC route per-IP rate limit', () => {
  // TRPC_CALLS_PER_MINUTE is 1,200: six full batches of 200 use it up.
  const exhaust = async (ip: string) => {
    for (let i = 0; i < 6; i++) {
      const res = await GET(fromIp(batchUrl(MAX_TRPC_BATCH_SIZE), ip) as never)
      expect(res.status).toBe(200)
    }
  }

  it('charges each batched call so batching cannot bypass the limit', async () => {
    await exhaust('203.0.113.7')
    fetchRequestHandler.mockClear()
    const res = await GET(fromIp(batchUrl(1)) as never)
    expect(res.status).toBe(429)
    expect(Number(res.headers.get('retry-after'))).toBeGreaterThan(0)
    expect(await res.json()).toEqual({ error: 'Too many requests' })
    expect(fetchRequestHandler).not.toHaveBeenCalled()
  })

  it('refuses a batch that would overrun the remaining budget', async () => {
    for (let i = 0; i < 5; i++) await GET(fromIp(batchUrl(MAX_TRPC_BATCH_SIZE)) as never)
    for (let i = 0; i < 199; i++) await GET(fromIp(batchUrl(1)) as never)
    expect((await GET(fromIp(batchUrl(2)) as never)).status).toBe(429)
    expect((await GET(fromIp(batchUrl(1)) as never)).status).toBe(200)
  })

  it('lets non-batched requests through well under the limit', async () => {
    for (let i = 0; i < 100; i++) {
      const res = await GET(fromIp('http://localhost/api/trpc/feed.getFeeds') as never)
      expect(res.status).toBe(200)
    }
    expect(fetchRequestHandler).toHaveBeenCalledTimes(100)
  })

  it('keeps separate budgets per client IP', async () => {
    await exhaust('203.0.113.7')
    expect((await GET(fromIp(batchUrl(1), '203.0.113.7') as never)).status).toBe(429)
    expect((await GET(fromIp(batchUrl(1), '198.51.100.9') as never)).status).toBe(200)
  })
})
