import type { PrismaClient } from '@prisma/client'
import { z } from 'zod'
import { accessFor } from '@/lib/subscription'
import type { SubscriptionChange } from '@/server/auth/flash-webhook'

/**
 * Reconcile a user's subscription row with Flash's own records.
 *
 * Flash drops a webhook after a few seconds of retries, deliveries can arrive
 * out of order, and anyone can start a subscription with someone else's pubkey
 * as `ref`. So when an API key is configured, the row is rebuilt from every
 * Flash subscription carrying the user's pubkey instead of from one event:
 * the subscription granting the latest access wins, so a stale or foreign
 * subscription cannot end a paid one, and a lost webhook is corrected on the
 * next check. See https://flashpay-1.gitbook.io/flash-documentation/subscriptions/api-reference/subscriptions
 */

const REQUEST_TIMEOUT_MS = 5_000

const flashSubscription = z.object({
  id: z.string().min(1).max(100),
  ref: z.string().nullable(),
  status: z.string(),
  currentPeriodEnd: z.string().nullable().optional(),
  canceledAt: z.string().nullable().optional(),
  cancelEffectiveDate: z.string().nullable().optional(),
  updatedAt: z.string().nullable().optional(),
})

const listResponse = z.object({
  subscriptions: z.array(flashSubscription).max(1000),
})

export type FlashSubscription = z.infer<typeof flashSubscription>

export interface ReconciledState extends Required<Pick<SubscriptionChange, 'status'>> {
  subscriptionEndsAt: Date
  cancelledAt: Date | null
  flashSubscriptionId: string
}

export interface FlashApiConfig {
  apiKey: string
  baseUrl: string
}

/**
 * The API lives on the same host as the checkout page, so its base comes from
 * FLASH_CHECKOUT_URL. Reconciliation is off unless both settings are present.
 */
export function flashApiConfig(env: NodeJS.ProcessEnv = process.env): FlashApiConfig | null {
  const apiKey = env.FLASH_API_KEY?.trim()
  const checkout = env.FLASH_CHECKOUT_URL
  if (!apiKey || !checkout) return null
  try {
    const url = new URL(checkout)
    if (url.protocol !== 'https:') return null
    return { apiKey, baseUrl: url.origin }
  } catch {
    return null
  }
}

/** Every Flash subscription whose `ref` is this pubkey, or null if Flash can't be asked. */
export async function listFlashSubscriptions(
  config: FlashApiConfig,
  pubkey: string,
  fetchImpl: typeof fetch = fetch
): Promise<FlashSubscription[] | null> {
  const url = new URL('/api/v1/external/subscriptions', config.baseUrl)
  url.searchParams.set('ref', pubkey)
  try {
    const res = await fetchImpl(url, {
      headers: { Authorization: `Bearer ${config.apiKey}`, Accept: 'application/json' },
      signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
      cache: 'no-store',
    })
    if (!res.ok) {
      console.warn('[flash-reconcile] list failed:', { status: res.status })
      return null
    }
    const parsed = listResponse.safeParse(await res.json())
    if (!parsed.success) {
      console.warn('[flash-reconcile] unexpected list response')
      return null
    }
    // The filter is Flash's; check it anyway so a bug there can't link another user's subscription.
    return parsed.data.subscriptions.filter(s => s.ref === pubkey)
  } catch (error) {
    console.warn('[flash-reconcile] list error:', error instanceof Error ? error.name : 'unknown')
    return null
  }
}

function asDate(value: string | null | undefined): Date | null {
  if (!value) return null
  const d = new Date(value)
  return Number.isNaN(d.getTime()) ? null : d
}

function candidateFor(sub: FlashSubscription): ReconciledState | null {
  // In end-of-period mode Flash sets cancelEffectiveDate to the period end, and
  // in immediate mode to the cancel time, so it is the true end once set.
  const endsAt = asDate(sub.cancelEffectiveDate) ?? asDate(sub.currentPeriodEnd)
  if (!endsAt) return null
  const canceledAt = asDate(sub.canceledAt)
  let status: ReconciledState['status']
  switch (sub.status) {
    case 'active':
    case 'trial':
      status = canceledAt ? 'CANCELLED' : 'ACTIVE'
      break
    case 'past_due':
      status = 'PAST_DUE'
      break
    case 'canceled':
      status = 'CANCELLED'
      break
    case 'expired':
      status = 'EXPIRED'
      break
    default:
      // pending, abandoned and anything new never granted access.
      return null
  }
  return { status, subscriptionEndsAt: endsAt, cancelledAt: canceledAt, flashSubscriptionId: sub.id }
}

/**
 * The row state Flash's records imply, or null when Flash has nothing usable
 * (the row is then left as it is).
 */
export function stateFromFlash(subs: FlashSubscription[], now: Date): ReconciledState | null {
  const candidates = subs.map(candidateFor).filter((c): c is ReconciledState => c !== null)
  if (candidates.length === 0) return null
  const grants = (c: ReconciledState) => accessFor({ ...c, trialEndsAt: new Date(0) }, now).hasAccess
  const live = candidates.filter(grants)
  const pool = live.length > 0 ? live : candidates
  return pool.reduce((best, c) => (c.subscriptionEndsAt > best.subscriptionEndsAt ? c : best))
}

type ReconcileDb = Pick<PrismaClient, 'userSubscription'>

/**
 * Rebuild the user's row from Flash. Returns the applied state, or null when
 * reconciliation is off, Flash is unreachable, or Flash knows no usable
 * subscription for the user; callers then keep their existing behaviour.
 */
export async function reconcileWithFlash(
  db: ReconcileDb,
  pubkey: string,
  now: Date = new Date(),
  opts: { config?: FlashApiConfig | null; fetchImpl?: typeof fetch } = {}
): Promise<ReconciledState | null> {
  const config = opts.config === undefined ? flashApiConfig() : opts.config
  if (!config) return null
  const subs = await listFlashSubscriptions(config, pubkey, opts.fetchImpl)
  if (!subs) return null
  const state = stateFromFlash(subs, now)
  if (!state) return null
  // flashEventAt marks the state as current at `now`, so webhook deliveries
  // older than this check are dropped by the out-of-order guard.
  const fields = { ...state, flashEventAt: now }
  await db.userSubscription.upsert({
    where: { userPubkey: pubkey },
    create: { userPubkey: pubkey, trialEndsAt: now, ...fields },
    update: fields,
  })
  return state
}

const STATUS_CHECK_EVERY_MS = 10 * 60_000
const STATUS_CHECKS_PER_MINUTE = 30
const NEAR_END_MS = 24 * 60 * 60_000

/**
 * Whether getStatus should ask Flash about this row now: only when the row
 * denies access or is about to run out (a lost `activated`/`renewed` would
 * show up exactly then), at most once per user per STATUS_CHECK_EVERY_MS, and
 * within a global budget so a crowd of lapsed users can't flood Flash's API.
 */
export function shouldCheckFlashOnStatus(
  row: { userPubkey: string; status: string; trialEndsAt: Date; subscriptionEndsAt: Date | null },
  now: Date,
  limiter: (name: string, key: string, limit: number, windowMs: number, now: number) => { allowed: boolean }
): boolean {
  const { hasAccess } = accessFor(row, now)
  const endsAt = row.subscriptionEndsAt ?? row.trialEndsAt
  const nearEnd = endsAt.getTime() - now.getTime() < NEAR_END_MS
  if (hasAccess && !nearEnd) return false
  const t = now.getTime()
  if (!limiter('flash-reconcile-user', row.userPubkey, 1, STATUS_CHECK_EVERY_MS, t).allowed) return false
  return limiter('flash-reconcile-all', 'all', STATUS_CHECKS_PER_MINUTE, 60_000, t).allowed
}
