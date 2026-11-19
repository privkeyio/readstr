import { Prisma, type PrismaClient, type UserSubscription } from '@prisma/client'
import { TRPCError } from '@trpc/server'
import { accessFor, trialEndFrom } from '@/lib/subscription'
import { SUBSCRIPTION_REQUIRED } from '@/lib/paywall'

/**
 * Whether the paywall is on. Read from the server env on every call (not
 * NEXT_PUBLIC_, which is baked in at build time), so flipping it needs only a
 * restart. Needs both PAYWALL_ENABLED ("true" or "1"; anything else is off)
 * and an https checkout URL, so self-hosted installs without Flash stay free.
 */
export function paywallActive(env: Record<string, string | undefined> = process.env): boolean {
  const flag = env.PAYWALL_ENABLED?.trim()
  const flagOn = flag === 'true' || flag === '1'
  const active = flagOn && isHttpsUrl(env.FLASH_CHECKOUT_URL)
  if (!active && flag && flag !== 'false' && flag !== '0') warnInactiveOnce(flag, flagOn)
  return active
}

function isHttpsUrl(value: string | undefined): boolean {
  const url = value?.trim()
  if (!url) return false
  try {
    return new URL(url).protocol === 'https:'
  } catch {
    return false
  }
}

let warnedInactive = false

function warnInactiveOnce(flag: string, flagOn: boolean): void {
  if (warnedInactive) return
  warnedInactive = true
  console.warn(
    flagOn
      ? '[paywall] PAYWALL_ENABLED is set but FLASH_CHECKOUT_URL is missing or not an https URL; the paywall is OFF'
      : `[paywall] PAYWALL_ENABLED=${JSON.stringify(flag)} is not "true" or "1"; the paywall is OFF`
  )
}

export function __resetPaywallWarningForTests(): void {
  warnedInactive = false
}

type SubscriptionDb = Pick<PrismaClient, 'userSubscription'>

/**
 * Start the trial on first sight and persist it, so it really ends. The empty
 * update keeps an existing row (and its dates) untouched. Prisma runs this
 * upsert as SELECT then INSERT, so parallel first-sight calls race on the
 * unique key; the loser reads the winner's row instead of failing.
 */
export async function startTrialIfMissing(db: SubscriptionDb, pubkey: string, now: Date): Promise<UserSubscription> {
  try {
    return await db.userSubscription.upsert({
      where: { userPubkey: pubkey },
      create: { userPubkey: pubkey, status: 'TRIAL', trialEndsAt: trialEndFrom(now) },
      update: {},
    })
  } catch (error) {
    if (error instanceof Prisma.PrismaClientKnownRequestError && error.code === 'P2002') {
      return db.userSubscription.findUniqueOrThrow({ where: { userPubkey: pubkey } })
    }
    throw error
  }
}

/** Per-request cache of subscription lookups, keyed by pubkey. */
export type SubscriptionLookups = Map<string, Promise<UserSubscription>>

function loadSubscription(db: SubscriptionDb, pubkey: string, now: Date, cache?: SubscriptionLookups) {
  const cached = cache?.get(pubkey)
  if (cached) return cached
  const lookup = (async () =>
    (await db.userSubscription.findUnique({ where: { userPubkey: pubkey } })) ??
    (await startTrialIfMissing(db, pubkey, now)))()
  if (cache) {
    cache.set(pubkey, lookup)
    // A failed lookup must not stick: a later call in the same request retries.
    lookup.catch(() => {
      if (cache.get(pubkey) === lookup) cache.delete(pubkey)
    })
  }
  return lookup
}

/**
 * Throw FORBIDDEN (message SUBSCRIPTION_REQUIRED) unless the stored row grants
 * access. One indexed lookup per request (shared through `cache` by every
 * gated call in a batch); Flash is never called here; getStatus reconciles
 * lapsed rows (throttled) and enforcement reads the result. A signer user
 * with no row yet (e.g. a first visit straight to a /subscribe link) gets the
 * same first-sight trial getStatus would start, never a new or longer one for
 * an existing row.
 */
export async function assertSubscribed(
  db: SubscriptionDb,
  pubkey: string,
  now: Date = new Date(),
  cache?: SubscriptionLookups
): Promise<void> {
  const row = await loadSubscription(db, pubkey, now, cache)
  if (!accessFor(row, now).hasAccess) {
    throw new TRPCError({ code: 'FORBIDDEN', message: SUBSCRIPTION_REQUIRED })
  }
}
