import { initTRPC } from '@trpc/server'
import { type CreateNextContextOptions } from '@trpc/server/adapters/next'
import superjson from 'superjson'
import { ZodError } from 'zod'
import { db } from '@/server/db'
import { verifyNip98Header } from '@/server/auth/nip98'

/**
 * Reconstruct the external request URL exactly as the client fetched it.
 *
 * The app deploys behind Caddy (see Caddyfile), which terminates TLS and
 * reverse-proxies to the app over http. We therefore prefer the
 * `x-forwarded-proto` / `x-forwarded-host` headers (set by the proxy) to
 * rebuild the https URL the client actually signed in its NIP-98 `u` tag.
 * If those are absent (e.g. local dev), we fall back to the Host header.
 *
 * Note: the `u`-tag comparison in verifyNip98Header ignores scheme/host and
 * matches on path + sorted query, so minor proxy discrepancies are tolerated.
 */
function reconstructUrl(req: CreateNextContextOptions['req']): string {
  const forwardedProto = (req.headers['x-forwarded-proto'] as string | undefined)
    ?.split(',')[0]
    ?.trim()
  const proto =
    forwardedProto ?? (process.env.NODE_ENV === 'production' ? 'https' : 'http')
  const host =
    (req.headers['x-forwarded-host'] as string | undefined)?.split(',')[0]?.trim() ??
    (req.headers.host as string | undefined) ??
    'localhost'
  const path = req.url ?? '/'
  return `${proto}://${host}${path}`
}

export const createTRPCContext = async (opts: CreateNextContextOptions) => {
  const { req } = opts

  // Authenticate via NIP-98 (kind 27235) signed HTTP Auth. The verified hex
  // pubkey is derived from the signature, never from a client-asserted header.
  const url = reconstructUrl(req)
  const method = (req.method ?? 'GET').toUpperCase()
  const authHeader = req.headers['authorization'] as string | undefined

  let nostrPubkey = await verifyNip98Header(authHeader, { url, method })

  // Migration escape hatch: ONLY when explicitly enabled, fall back to the old
  // insecure plaintext header so the team can roll out without an outage.
  // This MUST be unset/false in production (anyone can impersonate any user).
  if (!nostrPubkey && process.env.ALLOW_INSECURE_HEADER_AUTH === 'true') {
    const insecurePubkey = req.headers['x-nostr-pubkey'] as string | undefined
    if (insecurePubkey) {
      console.warn(
        '⚠️ INSECURE AUTH: trusting unverified x-nostr-pubkey header because ' +
          'ALLOW_INSECURE_HEADER_AUTH=true. This is forgeable and MUST be ' +
          'disabled in production.'
      )
      nostrPubkey = insecurePubkey
    }
  }

  return {
    db,
    nostrPubkey: nostrPubkey ?? null, // verified hex pubkey, or null when unauthenticated
  }
}

const t = initTRPC.context<typeof createTRPCContext>().create({
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

  const sanitizedInput = sanitizeValue(rawInput)
  
  // Log if sanitization changed anything (for debugging)
  if (JSON.stringify(rawInput) !== JSON.stringify(sanitizedInput)) {
    console.log('🧹 Input sanitized:', {
      before: JSON.stringify(rawInput),
      after: JSON.stringify(sanitizedInput),
    })
  }
  
  return next({
    rawInput: sanitizedInput,
  })
})

export const createTRPCRouter = t.router
export const publicProcedure = t.procedure.use(inputSanitizer)

const enforceNostrAuth = t.middleware(({ ctx, next }) => {
  if (!ctx.nostrPubkey) {
    throw new Error('UNAUTHORIZED - Nostr authentication required')
  }
  return next({
    ctx: {
      nostrPubkey: ctx.nostrPubkey,
      db: ctx.db,
    },
  })
})

export const protectedProcedure = t.procedure.use(inputSanitizer).use(enforceNostrAuth)