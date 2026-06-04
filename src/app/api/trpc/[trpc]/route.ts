import { fetchRequestHandler } from '@trpc/server/adapters/fetch'
import { type NextRequest } from 'next/server'
import { appRouter } from '@/server/api/root'
import { db } from '@/server/db'
import { verifyNip98Header } from '@/server/auth/nip98'

/**
 * Reconstruct the external request URL the client signed in its NIP-98 `u` tag.
 *
 * The app deploys behind Caddy (see Caddyfile), which terminates TLS and
 * proxies to the app over http. `req.url` on the Fetch adapter already reflects
 * the incoming request, but we prefer x-forwarded-proto/host when present so
 * the scheme/host line up with what the browser used. The server-side u-tag
 * comparison ignores scheme/host (path + sorted query only), so this is mostly
 * belt-and-suspenders.
 */
function reconstructUrl(req: NextRequest): string {
  const forwardedProto = req.headers.get('x-forwarded-proto')?.split(',')[0]?.trim()
  const forwardedHost = req.headers.get('x-forwarded-host')?.split(',')[0]?.trim()
  const url = new URL(req.url)
  if (forwardedProto) url.protocol = `${forwardedProto}:`
  if (forwardedHost) url.host = forwardedHost
  return url.toString()
}

const handler = async (req: NextRequest) => {
  return fetchRequestHandler({
    endpoint: '/api/trpc',
    req,
    router: appRouter,
    createContext: async () => {
      const url = reconstructUrl(req)
      const method = req.method.toUpperCase()
      const authHeader = req.headers.get('authorization') ?? undefined

      let nostrPubkey: string | null = await verifyNip98Header(authHeader, {
        url,
        method,
      })

      // Migration escape hatch — MUST be unset/false in production.
      if (!nostrPubkey && process.env.ALLOW_INSECURE_HEADER_AUTH === 'true') {
        const insecurePubkey = req.headers.get('x-nostr-pubkey') || undefined
        if (insecurePubkey) {
          console.warn(
            '⚠️ INSECURE AUTH: trusting unverified x-nostr-pubkey header ' +
              'because ALLOW_INSECURE_HEADER_AUTH=true. Disable in production.'
          )
          nostrPubkey = insecurePubkey
        }
      }

      return {
        db,
        nostrPubkey,
      }
    },
  })
}

export { handler as GET, handler as POST }
