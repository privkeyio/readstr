import { z } from 'zod'

const server = z.object({
  DATABASE_URL: z.string().url().optional(),
  NODE_ENV: z.enum(['development', 'test', 'production']).default('development'),
  DEFAULT_RELAYS: z.string().optional(),
  NOSTR_BUNKER_URL: z.string().optional(),
  // Comma-separated list of bare hostnames the NIP-98 `u` tag is allowed to
  // point at (entries with a scheme/port are normalized to the hostname).
  // Rejects foreign-origin tokens replayed against this server. Compared against
  // the signed `u` host only — never the proxied request Host header. In
  // development localhost is permitted by default; in production the canonical
  // host is always allowed.
  //
  // Deployment: set this when the production host differs from
  // readstr.privkey.io. Every entry must be mutually trust-equivalent — the
  // check only confirms the signed host is in the set, not that it matches the
  // host actually serving the request.
  NIP98_ALLOWED_HOSTS: z.string().optional(),
  FLASH_SUBSCRIPTION_KEY: z.string().optional(),
  // Temporary fallback escape hatch. When 'true', the Flash webhook verifier
  // falls back to the unsigned request body identity when the verified JWT
  // carries no user_public_key. MUST stay unset once Flash's token schema is
  // confirmed to populate user_public_key.
  ALLOW_FLASH_BODY_IDENTITY: z.string().optional(),
})

const client = z.object({
  NEXT_PUBLIC_ADMIN_NPUB: z.string().default('npub13hyx3qsqk3r7ctjqrr49uskut4yqjsxt8uvu4rekr55p08wyhf0qq90nt7'),
})

// The escape-hatch flag is a full auth-bypass foot-gun. Refuse to boot if it
// is enabled in production.
const refineInsecureFlags = (val, ctx) => {
  if (val.NODE_ENV !== 'production') return
  if (val.ALLOW_FLASH_BODY_IDENTITY === 'true') {
    ctx.addIssue({
      code: z.ZodIssueCode.custom,
      path: ['ALLOW_FLASH_BODY_IDENTITY'],
      message: `ALLOW_FLASH_BODY_IDENTITY must not be 'true' in production (auth bypass)`,
    })
  }
}

const processEnv = {
  DATABASE_URL: process.env.DATABASE_URL,
  NODE_ENV: process.env.NODE_ENV,
  DEFAULT_RELAYS: process.env.DEFAULT_RELAYS,
  NOSTR_BUNKER_URL: process.env.NOSTR_BUNKER_URL,
  NIP98_ALLOWED_HOSTS: process.env.NIP98_ALLOWED_HOSTS,
  FLASH_SUBSCRIPTION_KEY: process.env.FLASH_SUBSCRIPTION_KEY,
  ALLOW_FLASH_BODY_IDENTITY: process.env.ALLOW_FLASH_BODY_IDENTITY,
  NEXT_PUBLIC_ADMIN_NPUB: process.env.NEXT_PUBLIC_ADMIN_NPUB,
}

const merged = server.merge(client).superRefine(refineInsecureFlags)

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