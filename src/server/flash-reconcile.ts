import type { PrismaClient } from '@prisma/client'
import { z } from 'zod'
import { accessEndsAt, accessFor } from '@/lib/subscription'
import type { SubscriptionChange } from '@/server/auth/flash-webhook'
import { readBodyWithLimit } from '@/server/http/read-body'

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
/** 1000 records at a few hundred bytes each. */
const MAX_LIST_BODY_BYTES = 1_000_000

// ISO-8601 with a 4-digit year only: Prisma throws on years outside 0-9999,
// and Date would accept loose forms like "1".
const isoDate = z.iso
  .datetime({ offset: true })
  .refine(v => {
    const year = new Date(v).getUTCFullYear()
    return year >= 1970 && year <= 9999
  })
  .nullable()
  .optional()

const flashSubscription = z.object({
  id: z.string().min(1).max(100),
  ref: z.string().nullable(),
  status: z.string(),
  currentPeriodEnd: isoDate,
  canceledAt: isoDate,
  cancelEffectiveDate: isoDate,
})

// Records are checked one by one, so a single odd record (which anyone can
// create with a victim's ref) can't make the whole list unusable.
const listResponse = z.object({
  subscriptions: z.array(z.unknown()).max(1000),
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
      // The API key must never follow a redirect anywhere.
      redirect: 'error',
    })
    if (!res.ok) {
      await res.body?.cancel().catch(() => {})
      console.warn('[flash-reconcile] list failed:', { status: res.status })
      return null
    }
    const read = await readBodyWithLimit(res, MAX_LIST_BODY_BYTES)
    if (!read.ok) {
      console.warn('[flash-reconcile] list body refused:', { status: read.status })
      return null
    }
    const parsed = listResponse.safeParse(JSON.parse(read.body))
    if (!parsed.success) {
      console.warn('[flash-reconcile] unexpected list response')
      return null
    }
    const subs: FlashSubscription[] = []
    for (const raw of parsed.data.subscriptions) {
      const record = flashSubscription.safeParse(raw)
      // The filter is Flash's; check it anyway so a bug there can't link another user's subscription.
      if (record.success && record.data.ref === pubkey) subs.push(record.data)
    }
    return subs
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
  const live = candidates.filter(c => grants(c, now))
  const pool = live.length > 0 ? live : candidates
  // By access end including grace, so a CANCELLED sub ending a day later
  // doesn't beat an ACTIVE one that keeps a week of grace; ACTIVE wins ties.
  const end = (c: ReconciledState) => accessEndsAt({ ...c, trialEndsAt: c.subscriptionEndsAt }).getTime()
  return pool.reduce((best, c) => {
    const diff = end(c) - end(best)
    return diff > 0 || (diff === 0 && c.status === 'ACTIVE' && best.status !== 'ACTIVE') ? c : best
  })
}

function grants(state: ReconciledState, now: Date): boolean {
  return accessFor({ ...state, trialEndsAt: state.subscriptionEndsAt }, now).hasAccess
}

type ReconcileDb = Pick<PrismaClient, 'userSubscription'>

/** reconcileWithFlash's answer when the row is deliberately left unchanged. */
export const KEPT = 'kept' as const

/**
 * Rebuild the user's row from Flash. Returns the applied state; KEPT when
 * Flash answered but the row must stay as it is, because the result would end
 * access the row grants through something Flash didn't list (callers must not
 * apply the event either); or null when Flash couldn't decide: reconciliation
 * is off, Flash is unreachable, Flash knows no usable subscription for the
 * user, or `subscriptionId` (the triggering webhook's) isn't usable in Flash's
 * list yet. On null callers keep their existing behaviour.
 */
export async function reconcileWithFlash(
  db: ReconcileDb,
  pubkey: string,
  now: Date = new Date(),
  opts: { config?: FlashApiConfig | null; fetchImpl?: typeof fetch; subscriptionId?: string } = {}
): Promise<ReconciledState | typeof KEPT | null> {
  const config = opts.config === undefined ? flashApiConfig() : opts.config
  if (!config) return null
  const subs = await listFlashSubscriptions(config, pubkey, opts.fetchImpl)
  if (!subs) return null
  // Flash's list can lag its webhooks (a new sub still pending or missing).
  // Writing then would stamp flashEventAt past the event and drop it for good.
  if (opts.subscriptionId && !subs.some(s => s.id === opts.subscriptionId && candidateFor(s))) return null
  const state = stateFromFlash(subs, now)
  if (!state) return null
  if (!grants(state, now)) {
    // A local trial, or a row linked by hand to a sub ?ref= doesn't return,
    // must not be ended by a lapsed subscription anyone could create.
    const current = await db.userSubscription.findUnique({ where: { userPubkey: pubkey } })
    if (current && accessFor(current, now).hasAccess && !subs.some(s => s.id === current.flashSubscriptionId)) {
      return KEPT
    }
  }
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

const STATUS_CHECK_EVERY_MS = 2 * 60_000
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
  // Only allowed calls are charged, so the per-user gate goes first: a user it
  // refuses never spends a global token, and a global refusal costs the user
  // one short window.
  if (!limiter('flash-reconcile-user', row.userPubkey, 1, STATUS_CHECK_EVERY_MS, t).allowed) return false
  return limiter('flash-reconcile-all', 'all', STATUS_CHECKS_PER_MINUTE, 60_000, t).allowed
}
