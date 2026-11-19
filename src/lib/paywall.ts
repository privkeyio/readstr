/**
 * Paywall pieces shared by the server and the client gate. The server decides
 * whether the paywall is on (src/server/paywall.ts); this module only names
 * its answers and maps them to screens, so it is safe to bundle.
 */

/** Message of the FORBIDDEN TRPCError a gated procedure throws without access. */
export const SUBSCRIPTION_REQUIRED = 'SUBSCRIPTION_REQUIRED' as const

/** getStatus `reason` for a request without a NIP-98 signer (read-only npub view). */
export const SIGNER_REQUIRED = 'SIGNER_REQUIRED' as const

export function isSubscriptionRequiredError(error: unknown): boolean {
  if (!error || typeof error !== 'object') return false
  const { message, data } = error as { message?: unknown; data?: { code?: unknown } | null }
  return data?.code === 'FORBIDDEN' && message === SUBSCRIPTION_REQUIRED
}

export interface GateStatus {
  paywallActive: boolean
  reason: typeof SIGNER_REQUIRED | null
  status: string | null
  /** Set once the user has paid; EXPIRED alone doesn't say whether a trial or a subscription lapsed. */
  subscriptionEndsAt: Date | null
  hasAccess: boolean
}

export type GateScreen =
  | 'open'
  | 'trial'
  | 'signer-required'
  | 'signer-failed'
  | 'trial-expired'
  | 'subscription-expired'

/**
 * Which screen the subscription gate shows. Until the status check has
 * answered (loading, refetching with no data yet, or failed) the reader stays
 * open: the server enforces access on every gated procedure, so the client
 * gate is only the UI for it, and with the paywall off the reader must load
 * exactly as it does without a gate. Only an answer saying the paywall is on
 * and denies access replaces the reader.
 */
export function gateScreen(status: GateStatus | undefined, opts: { canSign: boolean }): GateScreen {
  if (!status || !status.paywallActive) return 'open'
  if (status.reason === SIGNER_REQUIRED) {
    // A signer that failed to sign sends the request unauthenticated too.
    return opts.canSign ? 'signer-failed' : 'signer-required'
  }
  if (!status.hasAccess) {
    const neverPaid = status.status === 'TRIAL' || (status.status === 'EXPIRED' && !status.subscriptionEndsAt)
    return neverPaid ? 'trial-expired' : 'subscription-expired'
  }
  return status.status === 'TRIAL' ? 'trial' : 'open'
}

export const TRIAL_BANNER_DISMISSED_KEY = 'readstr_trial_banner_dismissed'

/** Local calendar day, so a dismissal lasts until the user's own midnight. */
export function dayKey(date: Date): string {
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}`
}

// Storage is passed as a getter: reading window.localStorage itself throws
// when storage is disabled, so it must happen inside the try.
export function isTrialBannerDismissed(now: Date, storage: () => Pick<Storage, 'getItem'> = () => localStorage): boolean {
  try {
    return storage().getItem(TRIAL_BANNER_DISMISSED_KEY) === dayKey(now)
  } catch {
    return false
  }
}

export function dismissTrialBanner(now: Date, storage: () => Pick<Storage, 'setItem'> = () => localStorage): void {
  try {
    storage().setItem(TRIAL_BANNER_DISMISSED_KEY, dayKey(now))
  } catch {
    // Storage can be disabled; the banner then just comes back next load.
  }
}
