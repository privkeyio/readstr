import { fetchRequestHandler } from '@trpc/server/adapters/fetch'
import { type NextRequest } from 'next/server'
import { appRouter } from '@/server/api/root'
import { buildRequestContext, type RequestLike } from '@/server/auth/request-context'
import { readBodyWithLimit, type ReadBodyResult } from '@/server/http/read-body'
import { countTrpcCalls } from '@/server/http/trpc-batch'
import { clientIpFromRequest, rateLimit } from '@/server/rate-limit'
import { MAX_TRPC_BATCH_SIZE } from '@/trpc/batch-limits'

const TRPC_ENDPOINT = '/api/trpc'

/**
 * Largest tRPC request body accepted. Only mutations send a body; queries such
 * as getFeedItems carry their input in the GET URL. The biggest real body is a
 * batch of markAsRead calls (~45 bytes each, so ~1,400 fit); most calls are
 * well under 1 KB. The cap keeps zod v3's per-element parsing of hostile arrays
 * (~150x heap amplification, GHSA-mrhx-c7xc-m3w5) to roughly 10 MB per
 * request, and runs before NIP-98 auth and any procedure's rate limit, so it
 * also protects public mutations.
 */
const MAX_TRPC_BODY_BYTES = 64_000

/**
 * Per-IP budget of procedure calls per minute, charged one unit per call in a
 * batch so batching cannot dodge it. Normal use stays far below it: a page
 * load fires about ten queries, and each markAsRead costs one mutation plus up
 * to four refetches, so reading one item a second is ~300 a minute and the
 * auto-mark-read filter on a full 50-item page is a ~250 call burst. 1,200
 * leaves room for both together while holding one IP to ~20 calls a second.
 * The budget is per client IP (per /64 for IPv6), so users behind one NAT or
 * carrier-grade NAT share it; that is an accepted trade-off for a limiter that
 * runs before authentication.
 * Requests without X-Forwarded-For (local dev, or anything bypassing Caddy)
 * share the 'unknown' bucket; in dev that is a single user, so it is fine.
 */
const TRPC_CALLS_PER_MINUTE = 1_200
const TRPC_RATE_WINDOW_MS = 60_000

const handler = async (req: NextRequest) => {
  // Size the batch from the URL alone and charge it to the caller's IP before
  // reading the body or running any procedure, so neither a huge batch nor a
  // flood of small ones reaches the database.
  const calls = countTrpcCalls(req.url, TRPC_ENDPOINT)
  if (calls === null) {
    return Response.json({ error: 'Invalid procedure path' }, { status: 400 })
  }
  if (calls > MAX_TRPC_BATCH_SIZE) {
    return Response.json({ error: 'Too many calls in one batch' }, { status: 400 })
  }
  const limit = rateLimit(
    'trpc',
    clientIpFromRequest(req),
    TRPC_CALLS_PER_MINUTE,
    TRPC_RATE_WINDOW_MS,
    Date.now(),
    calls
  )
  if (!limit.allowed) {
    return Response.json(
      { error: 'Too many requests' },
      { status: 429, headers: { 'Retry-After': String(limit.retryAfterSeconds) } }
    )
  }

  // Read the raw body once so we can both bind it into NIP-98 payload
  // verification and still hand a fresh Request to the tRPC handler.
  const method = req.method.toUpperCase()
  let rawBody: string | null = null
  if (method !== 'GET' && method !== 'HEAD') {
    let read: ReadBodyResult
    try {
      read = await readBodyWithLimit(req, MAX_TRPC_BODY_BYTES)
    } catch {
      // The client aborted or the stream broke mid-read.
      return Response.json({ error: 'Invalid body' }, { status: 400 })
    }
    if (!read.ok) {
      return Response.json(
        { error: read.status === 413 ? 'Request body too large' : 'Invalid body' },
        { status: read.status }
      )
    }
    rawBody = read.body
  }

  const reqForHandler =
    rawBody === null ? req : new Request(req.url, { method, headers: req.headers, body: rawBody })

  const reqLike: RequestLike = {
    url: req.url,
    method,
    getHeader: name => req.headers.get(name) ?? undefined,
  }

  return fetchRequestHandler({
    endpoint: TRPC_ENDPOINT,
    req: reqForHandler,
    router: appRouter,
    createContext: () => buildRequestContext(reqLike, rawBody),
  })
}

export { handler as GET, handler as POST }
