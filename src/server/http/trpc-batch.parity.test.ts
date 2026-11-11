import { initTRPC } from '@trpc/server'
import { fetchRequestHandler } from '@trpc/server/adapters/fetch'
import { describe, expect, it } from 'vitest'
import { countTrpcCalls } from './trpc-batch'

// The route trusts countTrpcCalls to bound how many procedures one request can
// run, so this pins it against the real tRPC fetch adapter: for every URL shape,
// tRPC must never execute more procedures than we counted. If a tRPC upgrade
// changes how it splits or decodes the path, this fails before production does.

const ENDPOINT = '/api/trpc'

let runs = 0
const t = initTRPC.create()
const router = t.router({
  a: t.procedure.query(() => {
    runs++
    return 1
  }),
  m: t.procedure.mutation(() => {
    runs++
    return 1
  }),
  feed: t.router({
    getFeeds: t.procedure.query(() => {
      runs++
      return 1
    }),
  }),
})

async function executedProcedures(url: string, method: 'GET' | 'POST' = 'GET'): Promise<number> {
  runs = 0
  const init: RequestInit =
    method === 'POST'
      ? { method, body: '{}', headers: { 'content-type': 'application/json' } }
      : { method }
  const res = await fetchRequestHandler({
    endpoint: ENDPOINT,
    req: new Request(url, init),
    router,
    createContext: () => ({}),
  })
  await res.text()
  return runs
}

const prefixes = [
  'http://localhost/api/trpc/',
  'http://localhost/api/trpc//',
  'http://localhost//api/trpc/',
  'http://localhost/%61pi/trpc/',
]

const shapes = [
  'a,a,a?batch=1',
  'a%2Ca%2ca?batch=1',
  'a%252Ca%252Ca?batch=1',
  'a%2C%2C%2Ca?batch=1',
  'a,a,a/?batch=1',
  'a,a,a//?batch=1',
  '/a,a?batch=1',
  ',,,?batch=1',
  'a,a?batch=0',
  'a,a?batch=',
  'a,a?batch=1&batch=0',
  'a,a',
  'a',
  'a,a#,a,a?batch=1',
  'a,a?batch=1#x,y',
  'a,feed.getFeeds,feed%2EgetFeeds?batch=1',
  'a%EF%BC%8Ca?batch=1',
]

/** Add an empty batch input so tRPC reaches the procedures instead of failing to parse. */
function withInput(shape: string): string {
  const [beforeHash, hash] = shape.split('#')
  const sep = beforeHash!.includes('?') ? '&' : '?'
  const withQuery = `${beforeHash}${sep}input=%7B%7D`
  return hash === undefined ? withQuery : `${withQuery}#${hash}`
}

describe('countTrpcCalls parity with the tRPC fetch adapter', () => {
  for (const prefix of prefixes) {
    for (const shape of shapes) {
      const url = prefix + withInput(shape)
      it(`never undercounts GET ${url}`, async () => {
        const counted = countTrpcCalls(url, ENDPOINT)
        const executed = await executedProcedures(url)
        expect(counted).not.toBeNull()
        expect(executed).toBeLessThanOrEqual(counted!)
      })
    }
  }

  it.each([
    ['a,a,a?batch=1', 3],
    ['a%2Ca%2ca?batch=1', 3],
    ['a,feed.getFeeds,feed%2EgetFeeds?batch=1', 3],
  ])('counts exactly what tRPC runs for %s', async (shape, expected) => {
    const url = `http://localhost/api/trpc/${withInput(shape)}`
    expect(countTrpcCalls(url, ENDPOINT)).toBe(expected)
    expect(await executedProcedures(url)).toBe(expected)
  })

  it.each(['m,m,m?batch=1', 'm%2Cm,m?batch=1', 'm%2cm%2cm?batch=1', 'm,m'])(
    'never undercounts POST %s',
    async shape => {
      const url = `http://localhost/api/trpc/${shape}`
      const counted = countTrpcCalls(url, ENDPOINT)
      const executed = await executedProcedures(url, 'POST')
      expect(counted).not.toBeNull()
      expect(executed).toBeLessThanOrEqual(counted!)
      if (shape.includes('batch=1')) expect(executed).toBe(3)
    }
  )
})
