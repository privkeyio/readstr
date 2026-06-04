import { db } from '@/server/db'
import { verifyNip98Header } from '@/server/auth/nip98'

/**
 * Minimal view of the inbound request we need to build auth context. Works for
 * both the Next Fetch adapter (`Request`) and node-style header bags.
 */
export interface RequestLike {
  url: string
  method: string
  getHeader(name: string): string | undefined
}

/**
 * Reconstruct the external request URL the client signed in its NIP-98 `u` tag.
 *
 * The app deploys behind Caddy (see Caddyfile), which terminates TLS and
 * proxies to the app over http. We prefer x-forwarded-proto/host when present
 * so the scheme/host line up with what the browser used. When the proxy strips
 * x-forwarded-proto we fall back to https in production (the only way the app is
 * reachable) and http otherwise. The server-side u-tag comparison ignores
 * scheme/host (path + sorted query only), so this is mostly belt-and-suspenders.
 */
export function reconstructUrl(req: RequestLike): string {
  const forwardedProto = req.getHeader('x-forwarded-proto')?.split(',')[0]?.trim()
  const forwardedHost = req.getHeader('x-forwarded-host')?.split(',')[0]?.trim()
  const url = new URL(req.url, 'http://localhost')
  if (forwardedProto) {
    url.protocol = `${forwardedProto}:`
  } else if (process.env.NODE_ENV === 'production') {
    url.protocol = 'https:'
  }
  if (forwardedHost) url.host = forwardedHost
  return url.toString()
}

export interface TRPCContext {
  db: typeof db
  nostrPubkey: string | null
}

/**
 * Build the tRPC request context: authenticate via NIP-98 (kind 27235) signed
 * HTTP Auth. The verified hex pubkey is derived from the signature, never from a
 * client-asserted header.
 */
export async function buildRequestContext(
  req: RequestLike,
  body?: string | null
): Promise<TRPCContext> {
  const url = reconstructUrl(req)
  const method = req.method.toUpperCase()
  const authHeader = req.getHeader('authorization')

  let nostrPubkey = await verifyNip98Header(authHeader, { url, method, body })

  // Migration escape hatch — MUST be unset/false in production (env.mjs refuses
  // to boot with it enabled in production). Falls back to the old insecure
  // plaintext header so the team can roll out without an outage.
  if (!nostrPubkey && process.env.ALLOW_INSECURE_HEADER_AUTH === 'true') {
    const insecurePubkey = req.getHeader('x-nostr-pubkey')
    if (insecurePubkey) {
      console.warn(
        '⚠️ INSECURE AUTH: trusting unverified x-nostr-pubkey header because ' +
          'ALLOW_INSECURE_HEADER_AUTH=true. This is forgeable and MUST be ' +
          'disabled in production.'
      )
      nostrPubkey = insecurePubkey
    }
  }

  return { db, nostrPubkey: nostrPubkey ?? null }
}
