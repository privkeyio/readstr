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
  hasAccess: boolean
}

export type GateScreen =
  | 'loading'
  | 'open'
  | 'trial'
  | 'signer-required'
  | 'signer-failed'
  | 'trial-expired'
  | 'subscription-expired'

/**
 * Which screen the subscription gate shows. A failed status check opens the
 * reader: the server enforces access on every gated procedure, so the client
 * gate is only the UI for it, and a transient error must not lock out
 * self-hosted installs where the paywall is off.
 */
export function gateScreen(
  status: GateStatus | undefined,
  opts: { isLoading: boolean; canSign: boolean }
): GateScreen {
  if (opts.isLoading) return 'loading'
  if (!status || !status.paywallActive) return 'open'
  if (status.reason === SIGNER_REQUIRED) {
    // A signer that failed to sign sends the request unauthenticated too.
    return opts.canSign ? 'signer-failed' : 'signer-required'
  }
  if (!status.hasAccess) {
    return status.status === 'TRIAL' || status.status === 'EXPIRED' ? 'trial-expired' : 'subscription-expired'
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
