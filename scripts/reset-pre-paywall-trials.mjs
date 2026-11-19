#!/usr/bin/env node
// One-off, run once right before turning PAYWALL_ENABLED on. Before the flag,
// getStatus started a TRIAL row for every signed-in visitor, so those trials
// have long run out. This gives each such row (never linked to a Flash
// subscription, never paid) the standard trial from now.
//
// Dry run by default: prints how many rows would change. Writes only with --apply,
// and refuses to while PAYWALL_ENABLED is on: after the switch, every unlinked
// trial that ran out under the paywall would match and get a fresh trial.
// Usage: DATABASE_URL=... node scripts/reset-pre-paywall-trials.mjs [--apply]
//
// Re-running within a day changes nothing: rows already reset still have more
// than TRIAL_DAYS - 1 days left and are skipped.

import { pathToFileURL } from 'node:url'

// TRIAL_DAYS in src/lib/subscription.ts.
export const TRIAL_DAYS = 7
const DAY_MS = 24 * 60 * 60 * 1000

export function resetWhere(now) {
  return {
    status: 'TRIAL',
    flashSubscriptionId: null,
    subscriptionEndsAt: null,
    trialEndsAt: { lt: new Date(now.getTime() + (TRIAL_DAYS - 1) * DAY_MS) },
  }
}

// Same values paywallActive() in src/server/paywall.ts accepts.
export function paywallFlagOn(value) {
  const flag = value?.trim()
  return flag === 'true' || flag === '1'
}

export async function resetPrePaywallTrials(db, { apply, now = new Date() }) {
  const where = resetWhere(now)
  if (!apply) return { applied: false, count: await db.userSubscription.count({ where }) }
  const { count } = await db.userSubscription.updateMany({
    where,
    data: { trialEndsAt: new Date(now.getTime() + TRIAL_DAYS * DAY_MS) },
  })
  return { applied: true, count }
}

async function main() {
  const { PrismaClient } = await import('@prisma/client')
  const db = new PrismaClient()
  try {
    const apply = process.argv.includes('--apply')
    if (apply && paywallFlagOn(process.env.PAYWALL_ENABLED)) {
      throw new Error('PAYWALL_ENABLED is on; run this before enabling the paywall, not after')
    }
    const { count } = await resetPrePaywallTrials(db, { apply })
    console.log(
      apply
        ? `Reset ${count} pre-paywall trial row(s) to a ${TRIAL_DAYS}-day trial from now.`
        : `Dry run: ${count} pre-paywall trial row(s) would get a ${TRIAL_DAYS}-day trial from now. Re-run with --apply to write.`
    )
  } finally {
    await db.$disconnect()
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch(error => {
    console.error('reset-pre-paywall-trials failed:', error)
    process.exit(1)
  })
}
