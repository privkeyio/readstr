import { TRPCError, type DefaultErrorShape } from '@trpc/server'
import { afterEach, describe, expect, it, vi } from 'vitest'
import superjson from 'superjson'
import { z, type ZodError } from 'zod'
import { fetchRequestHandler } from '@trpc/server/adapters/fetch'
import {
  assertInputDepth,
  assertValueAnnotations,
  createTRPCRouter,
  formatError,
  publicProcedure,
  sanitizeValue,
  stripReferentialEqualities,
  transformer,
} from './trpc'

function countObjects(value: unknown): number {
  if (!value || typeof value !== 'object') return 0
  return 1 + Object.values(value).reduce<number>((n, v) => n + countObjects(v), 0)
}

// Nested {a:{a:...,b:0},b:0} where each level's `b` is annotated as the same
// object as its `a`, deepest first. Honoured, this expands to 2^depth objects.
function referentialBomb(depth: number) {
  let json: Record<string, unknown> = { a: 0, b: 0 }
  for (let i = 1; i < depth; i++) json = { a: json, b: 0 }
  const refs: Record<string, string[]> = {}
  for (let i = depth; i >= 1; i--) {
    const path = Array<string>(i).fill('a')
    refs[path.join('.')] = [[...path.slice(0, -1), 'b'].join('.')]
  }
  return JSON.parse(JSON.stringify({ json, meta: { referentialEqualities: refs } })) as unknown
}

describe('input transformer', () => {
  it('ignores client-supplied referentialEqualities', () => {
    const started = Date.now()
    const out = sanitizeValue(transformer.input.deserialize(referentialBomb(25)))
    expect(countObjects(out)).toBeLessThanOrEqual(25)
    expect(Date.now() - started).toBeLessThan(1000)
  })

  it('cannot be made to build a cycle', () => {
    const out = transformer.input.deserialize({
      json: { x: { y: 1 } },
      meta: { referentialEqualities: { x: ['x.y'] } },
    })
    expect(out).toEqual({ x: { y: 1 } })
    expect(() => sanitizeValue(out)).not.toThrow()
  })

  it('keeps other superjson annotations', () => {
    const out = transformer.input.deserialize(
      stripReferentialEqualities(transformer.input.serialize({ at: new Date(0) }))
    ) as { at: Date }
    expect(out.at).toBeInstanceOf(Date)
  })

  it('leaves payloads without meta untouched', () => {
    const payload = { json: { a: 1 } }
    expect(stripReferentialEqualities(payload)).toBe(payload)
    expect(stripReferentialEqualities(undefined)).toBeUndefined()
  })
})

function thrownBy(fn: () => unknown): unknown {
  try {
    fn()
  } catch (err) {
    expect(err).toBeInstanceOf(TRPCError)
    return err
  }
  throw new Error('expected a throw')
}

describe('sanitizeValue', () => {
  it('still filters empty values out of arrays', () => {
    expect(sanitizeValue({ tags: ['a', '', null, 'undefined', 'b'], n: 1 })).toEqual({ tags: ['a', 'b'], n: 1 })
  })

  it('rejects excessive depth with BAD_REQUEST', () => {
    let deep: unknown = 0
    for (let i = 0; i < 40; i++) deep = { a: deep }
    expect(thrownBy(() => sanitizeValue(deep))).toMatchObject({ code: 'BAD_REQUEST' })
  })

  it('rejects excessive node counts with BAD_REQUEST', () => {
    const wide = { ids: Array.from({ length: 20_000 }, (_, i) => `id${i}`) }
    expect(thrownBy(() => sanitizeValue(wide))).toMatchObject({ code: 'BAD_REQUEST' })
  })

  it('accepts the largest real inputs', () => {
    const ids = Array.from({ length: 1000 }, (_, i) => `id${i}`)
    expect(sanitizeValue({ itemIds: ids })).toEqual({ itemIds: ids })
  })
})

function nestedArrays(depth: number): unknown {
  let deep: unknown = 0
  for (let i = 0; i < depth; i++) deep = [deep]
  return deep
}

describe('assertInputDepth', () => {
  it('rejects hostile nesting without overflowing the stack', () => {
    expect(thrownBy(() => assertInputDepth({ json: nestedArrays(30_000) }))).toMatchObject({ code: 'BAD_REQUEST' })
    expect(thrownBy(() => transformer.input.deserialize({ json: nestedArrays(30_000) }))).toMatchObject({
      code: 'BAD_REQUEST',
    })
  })

  it('accepts input as deep as the sanitizer allows', () => {
    const json = nestedArrays(32)
    expect(transformer.input.deserialize({ json, meta: { values: {} } })).toEqual(json)
  })
})

describe('assertValueAnnotations', () => {
  it('rejects a non-object annotation node, which superjson recurses into forever', () => {
    const payload = { json: 0, meta: { values: { a: 'b' } } }
    expect(thrownBy(() => transformer.input.deserialize(payload))).toMatchObject({ code: 'BAD_REQUEST' })
    expect(thrownBy(() => assertValueAnnotations(payload))).toMatchObject({ code: 'BAD_REQUEST' })
    expect(thrownBy(() => assertValueAnnotations({ json: 0, meta: { values: { a: ['class', 'X'] } } }))).toMatchObject({
      code: 'BAD_REQUEST',
    })
  })

  it('rejects long annotation paths quickly', () => {
    const children = Object.fromEntries(Array.from({ length: 4500 }, (_, i) => [String(i), ['undefined']]))
    const payload = { json: {}, meta: { values: { [Array(13_000).fill('a').join('.')]: ['undefined', children] } } }
    const started = performance.now()
    expect(thrownBy(() => transformer.input.deserialize(payload))).toMatchObject({ code: 'BAD_REQUEST' })
    expect(performance.now() - started).toBeLessThan(200)
  })

  it('accepts what the client really sends', () => {
    const deep: Record<string, unknown> = {}
    let cursor = deep
    for (let i = 0; i < 30; i++) cursor = (cursor.n = {}) as Record<string, unknown>
    cursor.at = new Date(0)
    const inputs = [
      undefined,
      { feedId: undefined, tags: ['a'] },
      { at: new Date(0), s: new Set([1]), m: new Map([['k', new Date(0)]]), big: BigInt(1), 'dotted.key': undefined },
      deep,
    ]
    for (const input of inputs) {
      const wire = JSON.parse(JSON.stringify(superjson.serialize(input))) as unknown
      expect(transformer.input.deserialize(wire)).toEqual(input)
    }
  })

  it('turns superjson errors into 400s', () => {
    const payload = { json: { a: 1 }, meta: { values: { a: ['bogus'] } } }
    expect(thrownBy(() => transformer.input.deserialize(payload))).toMatchObject({ code: 'BAD_REQUEST' })
  })
})

describe('deeply nested input through tRPC', () => {
  const router = createTRPCRouter({ echo: publicProcedure.input(z.unknown()).query(({ input }) => input) })
  const call = (rawInput: string) =>
    fetchRequestHandler({
      endpoint: '/api/trpc',
      router,
      req: new Request(`http://localhost/api/trpc/echo?input=${encodeURIComponent(rawInput)}`),
      createContext: () => ({}) as never,
    })

  it('answers 400, not 500', async () => {
    vi.spyOn(console, 'error').mockImplementation(() => {})
    // Sent as raw text, the way a client would: ~60 KB, under the body cap.
    const n = 30_000
    const res = await call(`{"json":${'['.repeat(n)}0${']'.repeat(n)}}`)
    expect(res.status).toBe(400)
  })

  it('still serves normal input', async () => {
    const res = await call(JSON.stringify({ json: { tags: ['a', 'b'] } }))
    expect(res.status).toBe(200)
  })
})

function shapeFor(error: TRPCError): DefaultErrorShape {
  return {
    message: error.message,
    code: -32600,
    data: { code: error.code, httpStatus: 400 },
  }
}

describe('formatError', () => {
  afterEach(() => {
    vi.unstubAllEnvs()
  })

  it('returns a fixed message and a capped zodError for huge ZodErrors', () => {
    const parsed = z.array(z.string()).safeParse(new Array(30_000).fill(0))
    expect(parsed.success).toBe(false)
    const zodError = (parsed as { error: ZodError }).error
    expect(zodError.issues).toHaveLength(30_000)
    const error = new TRPCError({ code: 'BAD_REQUEST', message: zodError.message, cause: zodError })

    const formatted = formatError({ shape: shapeFor(error), error })

    expect(formatted.message).toBe('Invalid input')
    const flat = formatted.data.zodError!
    const reported =
      flat.formErrors.length + Object.values(flat.fieldErrors).reduce((n, e) => n + (e?.length ?? 0), 0)
    expect(reported).toBeLessThanOrEqual(20)
    expect(JSON.stringify(formatted).length).toBeLessThan(10_000)
  })

  it('hides internal error messages in production', () => {
    vi.stubEnv('NODE_ENV', 'production')
    const cause = new RangeError('Maximum call stack size exceeded')
    const error = new TRPCError({ code: 'INTERNAL_SERVER_ERROR', message: cause.message, cause })
    expect(formatError({ shape: shapeFor(error), error }).message).toBe('Internal server error')
    const bare = new TRPCError({ code: 'INTERNAL_SERVER_ERROR', message: 'db timeout' })
    expect(formatError({ shape: shapeFor(bare), error: bare }).message).toBe('Internal server error')
  })

  it('keeps user-facing messages from plain Errors thrown by routers in production', () => {
    vi.stubEnv('NODE_ENV', 'production')
    const cause = new Error('This feed is already in the guide')
    const error = new TRPCError({ code: 'INTERNAL_SERVER_ERROR', message: cause.message, cause })
    expect(formatError({ shape: shapeFor(error), error }).message).toBe('This feed is already in the guide')
  })

  it('keeps messages for expected errors', () => {
    vi.stubEnv('NODE_ENV', 'production')
    const error = new TRPCError({ code: 'UNAUTHORIZED', message: 'Nostr authentication required' })
    const formatted = formatError({ shape: shapeFor(error), error })
    expect(formatted.message).toBe('Nostr authentication required')
    expect(formatted.data.zodError).toBeNull()
  })
})
