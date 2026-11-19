import type { PrismaClient } from '@prisma/client'
import { TRPCError } from '@trpc/server'
import { accessFor, trialEndFrom } from '@/lib/subscription'
import { SUBSCRIPTION_REQUIRED } from '@/lib/paywall'

/**
 * Whether the paywall is on. Read from the server env on every call (not
 * NEXT_PUBLIC_, which is baked in at build time), so flipping it needs only a
 * restart. Needs both PAYWALL_ENABLED ("true" or "1"; anything else is off)
 * and a checkout URL, so self-hosted installs without Flash stay free.
 */
export function paywallActive(env: Record<string, string | undefined> = process.env): boolean {
  const flag = env.PAYWALL_ENABLED?.trim()
  return (flag === 'true' || flag === '1') && !!env.FLASH_CHECKOUT_URL?.trim()
}

type SubscriptionDb = Pick<PrismaClient, 'userSubscription'>

/**
 * Start the trial on first sight and persist it, so it really ends. The empty
 * update keeps an existing row (and its dates) untouched.
 */
export function startTrialIfMissing(db: SubscriptionDb, pubkey: string, now: Date) {
  return db.userSubscription.upsert({
    where: { userPubkey: pubkey },
    create: { userPubkey: pubkey, status: 'TRIAL', trialEndsAt: trialEndFrom(now) },
    update: {},
  })
}

/**
 * Throw FORBIDDEN (message SUBSCRIPTION_REQUIRED) unless the stored row grants
 * access. One indexed lookup; Flash is never called here; getStatus
 * reconciles lapsed rows (throttled) and enforcement reads the result. A
 * signer user with no row yet (e.g. a first visit straight to a /subscribe
 * link) gets the same first-sight trial getStatus would start, never a new or
 * longer one for an existing row.
 */
export async function assertSubscribed(db: SubscriptionDb, pubkey: string, now: Date = new Date()): Promise<void> {
  const row =
    (await db.userSubscription.findUnique({ where: { userPubkey: pubkey } })) ??
    (await startTrialIfMissing(db, pubkey, now))
  if (!accessFor(row, now).hasAccess) {
    throw new TRPCError({ code: 'FORBIDDEN', message: SUBSCRIPTION_REQUIRED })
  }
}
