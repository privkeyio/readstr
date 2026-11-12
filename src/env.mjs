import { z } from 'zod'

const server = z.object({
  DATABASE_URL: z.string().url().optional(),
  NODE_ENV: z.enum(['development', 'test', 'production']).default('development'),
  DEFAULT_RELAYS: z.string().optional(),
  NOSTR_BUNKER_URL: z.string().optional(),
  // Comma-separated list of bare hostnames the NIP-98 `u` tag is allowed to
  // point at (entries with a scheme/port are normalized to the hostname).
  // Rejects foreign-origin tokens replayed against this server. Compared against
  // the signed `u` host only — never the proxied request Host header. When
  // unset, a default is used: localhost in development, the canonical host
  // (readstr.privkey.io) in production.
  //
  // Deployment: set this to your own host(s) when the production host differs
  // from readstr.privkey.io. When set, ONLY the listed hosts are honored — the
  // readstr.privkey.io default is not added. Every entry must be mutually
  // trust-equivalent — the check only confirms the signed host is in the set,
  // not that it matches the host actually serving the request.
  NIP98_ALLOWED_HOSTS: z.string().optional(),
  // Signing secret (whsec_...) of the Flash webhook endpoint. Verifies the
  // Flash-Signature HMAC on /api/webhooks/flash. Secret: server env only.
  FLASH_WEBHOOK_SECRET: z.string().optional(),
  // Plan signup URL from the Flash dashboard. Read at runtime by the server.
  FLASH_CHECKOUT_URL: z.string().url().optional(),
  // Flash API key with only the "View Subscriptions" scope. When set, the
  // server re-reads a user's subscriptions from Flash on each webhook and when
  // a row lapses (src/server/flash-reconcile.ts). Secret: server env only.
  FLASH_API_KEY: z.string().optional(),
})

const client = z.object({
  NEXT_PUBLIC_ADMIN_NPUB: z.string().default('npub13hyx3qsqk3r7ctjqrr49uskut4yqjsxt8uvu4rekr55p08wyhf0qq90nt7'),
})

const processEnv = {
  DATABASE_URL: process.env.DATABASE_URL,
  NODE_ENV: process.env.NODE_ENV,
  DEFAULT_RELAYS: process.env.DEFAULT_RELAYS,
  NOSTR_BUNKER_URL: process.env.NOSTR_BUNKER_URL,
  NIP98_ALLOWED_HOSTS: process.env.NIP98_ALLOWED_HOSTS,
  FLASH_WEBHOOK_SECRET: process.env.FLASH_WEBHOOK_SECRET,
  FLASH_CHECKOUT_URL: process.env.FLASH_CHECKOUT_URL,
  FLASH_API_KEY: process.env.FLASH_API_KEY,
  NEXT_PUBLIC_ADMIN_NPUB: process.env.NEXT_PUBLIC_ADMIN_NPUB,
}

const merged = server.merge(client)

/** @type {z.infer<typeof merged>} */
let env = {}

if (!!process.env.SKIP_ENV_VALIDATION === false) {
  const isServer = typeof window === 'undefined'

  const parsed = isServer
    ? merged.safeParse(processEnv)
    : client.safeParse(processEnv)

  if (parsed.success === false) {
    console.error(
      '❌ Invalid environment variables:',
      parsed.error.flatten().fieldErrors,
    )
    throw new Error('Invalid environment variables')
  }

  env = new Proxy(parsed.data, {
    get(target, prop) {
      if (typeof prop !== 'string') return undefined
      if (!isServer && !prop.startsWith('NEXT_PUBLIC_'))
        throw new Error(
          process.env.NODE_ENV === 'production'
            ? '❌ Attempted to access a server-side environment variable on the client'
            : `❌ Attempted to access server-side environment variable '${prop}' on the client`
        )
      return target[prop]
    },
  })
}

export { env }