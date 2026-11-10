import { nip19 } from 'nostr-tools'

export const MONTHLY_PRICE_SATS = 1750
export const TRIAL_DAYS = 7

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
 */
export function accessFor(row: SubscriptionRow, now: Date) {
  const endsAt = row.subscriptionEndsAt ?? row.trialEndsAt
  const daysRemaining = Math.max(0, Math.ceil((endsAt.getTime() - now.getTime()) / DAY_MS))
  const hasAccess = ['TRIAL', 'ACTIVE'].includes(row.status) && endsAt > now
  return { hasAccess, daysRemaining }
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
 * Build the Flash checkout URL with the user's identity pre-filled, so the
 * signed webhook Flash sends back names the account that paid.
 *
 * Flash expects the hex pubkey as `npub` (sic) and echoes `external_uuid`
 * back. Both carry the hex key, because the webhook compares the body identity
 * with the token's and the rows are keyed by the hex key NIP-98 yields. They
 * are passed as Base64-encoded JSON in the `params` query value.
 * Uses URL so it works whether or not the base URL already has a query string.
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
  const params = { npub: hex, external_uuid: hex, is_verified: true }
  url.searchParams.set('params', Buffer.from(JSON.stringify(params)).toString('base64'))
  return url.toString()
}
