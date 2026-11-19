import { describe, expect, it } from 'vitest'
import {
  SIGNER_REQUIRED,
  SUBSCRIPTION_REQUIRED,
  TRIAL_BANNER_DISMISSED_KEY,
  dismissTrialBanner,
  gateScreen,
  isSubscriptionRequiredError,
  isTrialBannerDismissed,
  type GateStatus,
} from './paywall'

const on = (status: string | null, hasAccess: boolean, subscriptionEndsAt: Date | null = null): GateStatus => ({
  paywallActive: true,
  reason: null,
  status,
  subscriptionEndsAt,
  hasAccess,
})
const signer = { canSign: true }

describe('gateScreen', () => {
  it('keeps the reader open until the status check answers, and when it fails', () => {
    // No data yet covers loading, a refetch of a never-answered check, and an error.
    expect(gateScreen(undefined, signer)).toBe('open')
    expect(gateScreen(undefined, { canSign: false })).toBe('open')
  })

  it('opens the reader when the paywall is off', () => {
    const off: GateStatus = { paywallActive: false, reason: null, status: null, subscriptionEndsAt: null, hasAccess: true }
    expect(gateScreen(off, signer)).toBe('open')
    expect(gateScreen(off, { canSign: false })).toBe('open')
  })

  it('asks read-only sessions to sign in with a signer, and signer sessions to retry', () => {
    const readOnly: GateStatus = { paywallActive: true, reason: SIGNER_REQUIRED, status: null, subscriptionEndsAt: null, hasAccess: false }
    expect(gateScreen(readOnly, { canSign: false })).toBe('signer-required')
    expect(gateScreen(readOnly, signer)).toBe('signer-failed')
  })

  it('maps statuses to screens', () => {
    expect(gateScreen(on('TRIAL', true), signer)).toBe('trial')
    expect(gateScreen(on('TRIAL', false), signer)).toBe('trial-expired')
    expect(gateScreen(on('EXPIRED', false), signer)).toBe('trial-expired')
    // Flash's expiry of a paid subscription also writes EXPIRED, with the paid end.
    expect(gateScreen(on('EXPIRED', false, new Date('2026-10-01T00:00:00.000Z')), signer)).toBe('subscription-expired')
    for (const status of ['ACTIVE', 'PAST_DUE', 'CANCELLED']) {
      expect(gateScreen(on(status, true), signer)).toBe('open')
      expect(gateScreen(on(status, false), signer)).toBe('subscription-expired')
    }
  })
})

describe('isSubscriptionRequiredError', () => {
  it('matches only FORBIDDEN with the SUBSCRIPTION_REQUIRED message', () => {
    expect(isSubscriptionRequiredError({ message: SUBSCRIPTION_REQUIRED, data: { code: 'FORBIDDEN' } })).toBe(true)
    expect(isSubscriptionRequiredError({ message: 'Admin access required', data: { code: 'FORBIDDEN' } })).toBe(false)
    expect(isSubscriptionRequiredError({ message: SUBSCRIPTION_REQUIRED, data: { code: 'UNAUTHORIZED' } })).toBe(false)
    expect(isSubscriptionRequiredError({ message: SUBSCRIPTION_REQUIRED, data: null })).toBe(false)
    expect(isSubscriptionRequiredError(new Error(SUBSCRIPTION_REQUIRED))).toBe(false)
    expect(isSubscriptionRequiredError(null)).toBe(false)
  })
})

describe('trial banner dismissal', () => {
  const memory = () => {
    const map = new Map<string, string>()
    return { getItem: (k: string) => map.get(k) ?? null, setItem: (k: string, v: string) => void map.set(k, v), map }
  }

  it('lasts for the rest of the local day only', () => {
    const store = memory()
    const morning = new Date(2026, 10, 19, 8, 0)
    expect(isTrialBannerDismissed(morning, () => store)).toBe(false)
    dismissTrialBanner(morning, () => store)
    expect(store.map.get(TRIAL_BANNER_DISMISSED_KEY)).toBe('2026-11-19')
    expect(isTrialBannerDismissed(new Date(2026, 10, 19, 23, 59), () => store)).toBe(true)
    expect(isTrialBannerDismissed(new Date(2026, 10, 20, 0, 1), () => store)).toBe(false)
  })

  it('treats unavailable storage as not dismissed, without throwing', () => {
    const broken = () => {
      throw new Error('SecurityError')
    }
    expect(isTrialBannerDismissed(new Date(), broken)).toBe(false)
    expect(() => dismissTrialBanner(new Date(), broken)).not.toThrow()
  })
})
