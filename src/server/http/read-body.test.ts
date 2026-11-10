import { describe, expect, it } from 'vitest'
import { readBodyWithLimit } from './read-body'

function streamOf(chunks: Uint8Array[]): ReadableStream<Uint8Array> {
  return new ReadableStream({
    start(controller) {
      for (const c of chunks) controller.enqueue(c)
      controller.close()
    },
  })
}

// A streamed request whose Content-Length can be omitted or lie.
function streamedRequest(chunks: Uint8Array[], contentLength?: string): Request {
  const headers = new Headers()
  if (contentLength !== undefined) headers.set('content-length', contentLength)
  return new Request('http://localhost/api/trpc/x', {
    method: 'POST',
    headers,
    body: streamOf(chunks),
    // Required by Node's fetch for stream bodies.
    duplex: 'half',
  } as RequestInit)
}

describe('readBodyWithLimit', () => {
  it('returns the body when it fits', async () => {
    const req = new Request('http://localhost/', { method: 'POST', body: '{"a":"é"}' })
    expect(await readBodyWithLimit(req, 100)).toEqual({ ok: true, body: '{"a":"é"}' })
  })

  it('accepts a body of exactly the limit', async () => {
    const req = new Request('http://localhost/', { method: 'POST', body: 'x'.repeat(10) })
    expect(await readBodyWithLimit(req, 10)).toEqual({ ok: true, body: 'x'.repeat(10) })
  })

  it('refuses an oversize declared Content-Length without reading the stream', async () => {
    let pulled = false
    // highWaterMark 0 stops the stream pre-pulling, so `pulled` means a real read.
    const body = new ReadableStream<Uint8Array>(
      {
        pull() {
          pulled = true
        },
      },
      { highWaterMark: 0 }
    )
    const req = new Request('http://localhost/', {
      method: 'POST',
      headers: { 'content-length': '1000001' },
      body,
      duplex: 'half',
    } as RequestInit)
    expect(await readBodyWithLimit(req, 1_000_000)).toEqual({ ok: false, status: 413 })
    expect(pulled).toBe(false)
  })

  it('refuses an oversize stream with no Content-Length, stopping early', async () => {
    const chunk = new Uint8Array(1000)
    let enqueued = 0
    const body = new ReadableStream<Uint8Array>({
      pull(controller) {
        enqueued++
        controller.enqueue(chunk)
      },
    })
    const req = new Request('http://localhost/', { method: 'POST', body, duplex: 'half' } as RequestInit)
    expect(await readBodyWithLimit(req, 5000)).toEqual({ ok: false, status: 413 })
    expect(enqueued).toBeLessThan(10)
  })

  it('refuses a stream larger than a lying Content-Length claims', async () => {
    const req = streamedRequest([new Uint8Array(600), new Uint8Array(600)], '10')
    expect(await readBodyWithLimit(req, 1000)).toEqual({ ok: false, status: 413 })
  })

  it('rejects a malformed Content-Length', async () => {
    const req = streamedRequest([new Uint8Array(1)], '12abc')
    expect(await readBodyWithLimit(req, 1000)).toEqual({ ok: false, status: 400 })
  })

  it('decodes multi-byte characters split across chunks', async () => {
    const bytes = new TextEncoder().encode('é')
    const req = streamedRequest([bytes.slice(0, 1), bytes.slice(1)])
    expect(await readBodyWithLimit(req, 10)).toEqual({ ok: true, body: 'é' })
  })

  it('rejects invalid UTF-8 rather than inflating it with replacement characters', async () => {
    const req = streamedRequest([new Uint8Array([0x7b, 0xff, 0xfe, 0x7d])])
    expect(await readBodyWithLimit(req, 100)).toEqual({ ok: false, status: 400 })
  })

  it('rejects a body that ends mid-character', async () => {
    const req = streamedRequest([new TextEncoder().encode('é').slice(0, 1)])
    expect(await readBodyWithLimit(req, 100)).toEqual({ ok: false, status: 400 })
  })

  it('treats a missing body as empty', async () => {
    const req = new Request('http://localhost/', { method: 'POST' })
    expect(await readBodyWithLimit(req, 10)).toEqual({ ok: true, body: '' })
  })
})
