import { beforeEach, describe, expect, it, vi } from 'vitest'

// The cap must run before tRPC parses input or builds auth context.
const fetchRequestHandler = vi.fn(async () => new Response('handled'))
vi.mock('@trpc/server/adapters/fetch', () => ({ fetchRequestHandler }))
vi.mock('@/server/api/root', () => ({ appRouter: {} }))
vi.mock('@/server/auth/request-context', () => ({ buildRequestContext: vi.fn() }))

const { GET, POST } = await import('./route')

const URL_ = 'http://localhost/api/trpc/guide.submitFeed'

describe('tRPC route body cap', () => {
  beforeEach(() => fetchRequestHandler.mockClear())

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
