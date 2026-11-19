import { describe, expect, it } from 'vitest'
import { TRIAL_DAYS } from './subscription'
import {
  TRIAL_DAYS as SCRIPT_TRIAL_DAYS,
  paywallFlagOn,
  resetWhere,
} from '../../scripts/reset-pre-paywall-trials.mjs'

describe('reset-pre-paywall-trials script', () => {
  it('keeps its trial length in step with the app', () => {
    expect(SCRIPT_TRIAL_DAYS).toBe(TRIAL_DAYS)
  })

  it('only matches unlinked, never-paid trial rows', () => {
    const where = resetWhere(new Date(0))
    expect(where).toMatchObject({ status: 'TRIAL', flashSubscriptionId: null, subscriptionEndsAt: null })
  })

  it('recognises the same flag values as the paywall', () => {
    for (const on of ['true', '1', ' true ']) expect(paywallFlagOn(on)).toBe(true)
    for (const off of [undefined, '', 'TRUE', 'yes', '0', 'false']) expect(paywallFlagOn(off)).toBe(false)
  })
})
