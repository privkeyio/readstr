import { nip19 } from 'nostr-tools'

export const MONTHLY_PRICE_SATS = 1750
export const TRIAL_DAYS = 7
/** A paid period, counted from payment. Flash's webhooks carry no period end. */
export const PERIOD_DAYS = 30
/** Matches the Flash service's dunning grace period. */
export const GRACE_DAYS = 7

const DAY_MS = 24 * 60 * 60 * 1000

export function trialEndFrom(now: Date): Date {
  return new Date(now.getTime() + TRIAL_DAYS * DAY_MS)
}

interface SubscriptionRow {
  status: string
  trialEndsAt: Date
  subscriptionEndsAt: Date | null
}

/**
 * Whether a stored subscription grants access right now, and for how long.
 *
 * Access is decided from the stored row only. The trial's end date must come
 * from a persisted row (see getStatus), never be recomputed from "now", or the
 * trial restarts on every check and never ends.
 *
 * ACTIVE and PAST_DUE keep access for GRACE_DAYS past the paid period, covering
 * Flash's payment retries and a late or dropped `renewed` webhook. A CANCELLED
 * row keeps access until its subscriptionEndsAt, the end of the period already
 * paid for (Flash's `canceled` leaves it unchanged). EXPIRED never has access.
 * daysRemaining counts to the paid end, without grace.
 */
export function accessFor(row: SubscriptionRow, now: Date) {
  const endsAt = row.subscriptionEndsAt ?? row.trialEndsAt
  const daysRemaining = Math.max(0, Math.ceil((endsAt.getTime() - now.getTime()) / DAY_MS))
  const hasAccess =
    ['TRIAL', 'ACTIVE', 'PAST_DUE', 'CANCELLED'].includes(row.status) &&
    accessEndsAt(row).getTime() > now.getTime()
  return { hasAccess, daysRemaining }
}

/** When the row's access ends, including any grace (statuses aside). */
export function accessEndsAt(row: SubscriptionRow): Date {
  const endsAt = row.subscriptionEndsAt ?? row.trialEndsAt
  const grace = row.status === 'ACTIVE' || row.status === 'PAST_DUE' ? GRACE_DAYS * DAY_MS : 0
  return new Date(endsAt.getTime() + grace)
}

/**
 * Normalize an authenticated Nostr identity to a lowercase hex pubkey.
 *
 * NIP-98 auth yields a hex pubkey, but older callers passed an npub. Returns
 * null for anything that is neither, rather than throwing.
 */
export function normalizePubkey(pubkey: string): string | null {
  if (/^[0-9a-f]{64}$/i.test(pubkey)) {
    return pubkey.toLowerCase()
  }
  try {
    const decoded = nip19.decode(pubkey)
    if (decoded.type === 'npub') return decoded.data
  } catch {
    // Not bech32; fall through.
  }
  return null
}

/**
 * Build the Flash signup URL with the user's identity attached, so the signed
 * webhooks Flash sends back name the account that paid.
 *
 * Flash stores the `ref` query value as the subscription's `externalRef` and
 * echoes it in every webhook. It carries the lowercase hex pubkey, because
 * subscription rows are keyed by the hex key NIP-98 yields. Uses URL so it
 * works whether or not the base URL already has a query string.
 */
export function buildCheckoutUrl(baseUrl: string, pubkey: string | null): string {
  if (!baseUrl) return '#'
  const hex = pubkey ? normalizePubkey(pubkey) : null
  if (!hex) return baseUrl

  let url: URL
  try {
    url = new URL(baseUrl)
  } catch {
    return baseUrl
  }
  url.searchParams.set('ref', hex)
  return url.toString()
}
