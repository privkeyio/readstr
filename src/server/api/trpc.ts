import { initTRPC, TRPCError } from '@trpc/server'
import superjson from 'superjson'
import { ZodError } from 'zod'
import { type TRPCContext } from '@/server/auth/request-context'

const t = initTRPC.context<TRPCContext>().create({
  transformer: superjson,
  errorFormatter({ shape, error }) {
    return {
      ...shape,
      data: {
        ...shape.data,
        zodError:
          error.cause instanceof ZodError ? error.cause.flatten() : null,
      },
    }
  },
})

/**
 * Global middleware to sanitize inputs and prevent deserialization quirks
 * Removes 'undefined' strings, null values, and empty strings from arrays
 */
const inputSanitizer = t.middleware(({ next, rawInput }) => {
  const sanitizeValue = (value: any): any => {
    // Handle arrays - filter out invalid values
    if (Array.isArray(value)) {
      return value
        .filter(item => item !== null && item !== undefined && item !== '' && item !== 'undefined')
        .map(sanitizeValue)
    }
    
    // Handle objects - recursively sanitize
    if (value && typeof value === 'object' && !Array.isArray(value)) {
      const sanitized: any = {}
      for (const [key, val] of Object.entries(value)) {
        sanitized[key] = sanitizeValue(val)
      }
      return sanitized
    }
    
    // Return primitive values as-is
    return value
  }

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