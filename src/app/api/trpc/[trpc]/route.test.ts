import { describe, expect, it, vi } from 'vitest'

// The cap must run before tRPC parses input or builds auth context.
const fetchRequestHandler = vi.fn(async () => new Response('handled'))
vi.mock('@trpc/server/adapters/fetch', () => ({ fetchRequestHandler }))
vi.mock('@/server/api/root', () => ({ appRouter: {} }))
vi.mock('@/server/auth/request-context', () => ({ buildRequestContext: vi.fn() }))

const { POST } = await import('./route')

const URL_ = 'http://localhost/api/trpc/guide.submitFeed'

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
})
