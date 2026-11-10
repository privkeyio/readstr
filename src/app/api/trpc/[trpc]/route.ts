import { fetchRequestHandler } from '@trpc/server/adapters/fetch'
import { type NextRequest } from 'next/server'
import { appRouter } from '@/server/api/root'
import { buildRequestContext, type RequestLike } from '@/server/auth/request-context'
import { readBodyWithLimit } from '@/server/http/read-body'

/**
 * Largest tRPC request body accepted. The biggest real input is a 500-id
 * feedIds list (~15 KB); most calls are well under 1 KB. The cap keeps zod v3's
 * per-element parsing of hostile arrays (~150x heap amplification,
 * GHSA-mrhx-c7xc-m3w5) to roughly 10 MB per request, and runs before NIP-98
 * auth and any procedure's rate limit, so it also protects public mutations.
 */
const MAX_TRPC_BODY_BYTES = 64_000

const handler = async (req: NextRequest) => {
  // Read the raw body once so we can both bind it into NIP-98 payload
  // verification and still hand a fresh Request to the tRPC handler.
  const method = req.method.toUpperCase()
  let rawBody: string | null = null
  if (method !== 'GET' && method !== 'HEAD') {
    const read = await readBodyWithLimit(req, MAX_TRPC_BODY_BYTES)
    if (!read.ok) {
      return Response.json(
        { error: read.status === 413 ? 'Request body too large' : 'Invalid Content-Length' },
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
    endpoint: '/api/trpc',
    req: reqForHandler,
    router: appRouter,
    createContext: () => buildRequestContext(reqLike, rawBody),
  })
}

export { handler as GET, handler as POST }
