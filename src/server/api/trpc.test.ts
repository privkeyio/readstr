import { TRPCError, type DefaultErrorShape } from '@trpc/server'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { z, type ZodError } from 'zod'
import { formatError, sanitizeValue, stripReferentialEqualities, transformer } from './trpc'

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
