import { fetchRequestHandler } from '@trpc/server/adapters/fetch'
import { type NextRequest } from 'next/server'
import { appRouter } from '@/server/api/root'
import { buildRequestContext, type RequestLike } from '@/server/auth/request-context'

const handler = async (req: NextRequest) => {
  // Read the raw body once so we can both bind it into NIP-98 payload
  // verification and still hand a fresh Request to the tRPC handler.
  const method = req.method.toUpperCase()
  const rawBody = method === 'GET' || method === 'HEAD' ? null : await req.text()

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
