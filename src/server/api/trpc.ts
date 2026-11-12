import { initTRPC, TRPCError, type DefaultErrorShape } from '@trpc/server'
import superjson from 'superjson'
import { ZodError } from 'zod'
import { type TRPCContext } from '@/server/auth/request-context'

/**
 * Drop superjson's referential-equality annotations from client input.
 *
 * They let a client point many paths at one object, so a ~2 KB payload can
 * describe a DAG that expands to ~1e9 nodes once anything walks it as a tree.
 * Inputs are plain data, so ignoring them only turns shared references into
 * equal copies. Both the object form and the `[root, other]` array form live
 * under `meta.referentialEqualities`.
 */
export function stripReferentialEqualities(payload: unknown): unknown {
  if (!payload || typeof payload !== 'object') return payload
  const { meta } = payload as { meta?: unknown }
  if (!meta || typeof meta !== 'object' || !('referentialEqualities' in meta)) return payload
  const safeMeta: Record<string, unknown> = { ...meta }
  delete safeMeta.referentialEqualities
  return { ...payload, meta: safeMeta }
}

const MAX_ZOD_ISSUES = 20

/** True only for `new Error(...)` itself, not subclasses like RangeError or library errors. */
function isPlainError(value: unknown): value is Error {
  return value instanceof Error && Object.getPrototypeOf(value) === Error.prototype
}

export function formatError({ shape, error }: { shape: DefaultErrorShape; error: TRPCError }) {
  const zodError = error.cause instanceof ZodError ? error.cause : null
  let message = shape.message
  if (zodError) {
    // A ZodError's message pretty-prints every issue, so a 64 KB body of bad
    // array elements would otherwise come back as megabytes of JSON.
    message = 'Invalid input'
  } else if (
    error.code === 'INTERNAL_SERVER_ERROR' &&
    process.env.NODE_ENV === 'production' &&
    !isPlainError(error.cause)
  ) {
    // Routers throw plain `Error`s for expected, user-facing failures ("This
    // feed is already in the guide"), and tRPC wraps those as internal errors,
    // so their messages must reach the client. Anything else (RangeError from
    // hostile nesting, TypeError, Prisma and other library errors) can carry
    // internals clients have no use for, so production reports a generic one.
    message = 'Internal server error'
  }
  return {
    ...shape,
    message,
    data: {
      ...shape.data,
      zodError: zodError ? (new ZodError(zodError.issues.slice(0, MAX_ZOD_ISSUES)) as ZodError<Record<string, unknown>>).flatten() : null,
    },
  }
}

// Wire-compatible with the client's plain `superjson`; only the server's
// parsing of client input differs.
export const transformer = {
  input: {
    serialize: (value: unknown) => superjson.serialize(value),
    deserialize: (value: unknown) =>
      superjson.deserialize(stripReferentialEqualities(value) as Parameters<typeof superjson.deserialize>[0]),
  },
  output: superjson,
}

const t = initTRPC.context<TRPCContext>().create({
  transformer,
  errorFormatter: formatError,
})

const MAX_INPUT_DEPTH = 32
const MAX_INPUT_NODES = 10_000

/**
 * Remove 'undefined' strings, null values, and empty strings from arrays to
 * prevent deserialization quirks. The walk is bounded in depth and node count
 * because it runs before auth on every procedure, and an unbounded recursive
 * copy of hostile input could exhaust the stack or the heap.
 */
export function sanitizeValue(input: unknown): unknown {
  let nodes = 0
  const walk = (value: any, depth: number): any => {
    if (++nodes > MAX_INPUT_NODES || depth > MAX_INPUT_DEPTH) {
      throw new TRPCError({ code: 'BAD_REQUEST', message: 'Input is too large or too deeply nested' })
    }

    // Handle arrays - filter out invalid values
    if (Array.isArray(value)) {
      return value
        .filter(item => item !== null && item !== undefined && item !== '' && item !== 'undefined')
        .map(item => walk(item, depth + 1))
    }

    // Handle objects - recursively sanitize
    if (value && typeof value === 'object') {
      const sanitized: any = {}
      for (const [key, val] of Object.entries(value)) {
        sanitized[key] = walk(val, depth + 1)
      }
      return sanitized
    }

    // Return primitive values as-is
    return value
  }
  return walk(input, 0)
}

/**
 * Global middleware to sanitize inputs and prevent deserialization quirks
 */
const inputSanitizer = t.middleware(({ next, rawInput }) => {
  return next({
    rawInput: sanitizeValue(rawInput),
  })
})

export const createTRPCRouter = t.router
export const publicProcedure = t.procedure.use(inputSanitizer)

const enforceNostrAuth = t.middleware(({ ctx, next }) => {
  if (!ctx.nostrPubkey) {
    throw new TRPCError({
      code: 'UNAUTHORIZED',
      message: 'Nostr authentication required',
    })
  }
  return next({
    ctx: {
      ...ctx,
      nostrPubkey: ctx.nostrPubkey,
    },
  })
})

export const protectedProcedure = t.procedure.use(inputSanitizer).use(enforceNostrAuth)