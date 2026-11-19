import { Prisma } from '@prisma/client'
import { TRPCError } from '@trpc/server'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { SIGNER_REQUIRED, SUBSCRIPTION_REQUIRED } from '@/lib/paywall'
import { createTRPCRouter, subscribedProcedure } from '@/server/api/trpc'
import { feedRouter } from '@/server/api/routers/feed'
import { subscriptionRouter } from '@/server/api/routers/subscription'
import { __resetPaywallWarningForTests, assertSubscribed, paywallActive, startTrialIfMissing, type SubscriptionLookups } from './paywall'

const PUBKEY = '3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d'
const DAY = 24 * 60 * 60 * 1000
const CHECKOUT = 'https://flash.example/checkout/plan'

type Row = { userPubkey: string; status: string; trialEndsAt: Date; subscriptionEndsAt: Date | null }

function fakeDb(row: Row | null) {
  const created = (args: { create: Omit<Row, 'subscriptionEndsAt'> }): Row => ({ ...args.create, subscriptionEndsAt: null })
  return {
    userSubscription: {
      findUnique: vi.fn(async () => row),
      upsert: vi.fn(async (args: { create: Omit<Row, 'subscriptionEndsAt'> }) => row ?? created(args)),
    },
  }
}

function ctx(db: ReturnType<typeof fakeDb>, nostrPubkey: string | null = PUBKEY) {
  return { db: db as never, nostrPubkey, clientIp: '127.0.0.1', subscriptionLookups: new Map() as SubscriptionLookups }
}

const at = (days: number) => new Date(Date.now() + days * DAY)
const expired: Row = { userPubkey: PUBKEY, status: 'EXPIRED', trialEndsAt: at(-30), subscriptionEndsAt: at(-10) }
const trial: Row = { userPubkey: PUBKEY, status: 'TRIAL', trialEndsAt: at(3), subscriptionEndsAt: null }
const endedTrial: Row = { userPubkey: PUBKEY, status: 'TRIAL', trialEndsAt: at(-1), subscriptionEndsAt: null }
const active: Row = { userPubkey: PUBKEY, status: 'ACTIVE', trialEndsAt: at(-40), subscriptionEndsAt: at(20) }

function paywall(on: boolean) {
  vi.stubEnv('PAYWALL_ENABLED', on ? 'true' : '')
  vi.stubEnv('FLASH_CHECKOUT_URL', CHECKOUT)
}

beforeEach(() => {
  // Keep Flash reconciliation off so getStatus never reaches the network.
  vi.stubEnv('FLASH_API_KEY', '')
})

afterEach(() => {
  vi.unstubAllEnvs()
})

describe('paywallActive', () => {
  it('is on only with a "true"/"1" flag and an https checkout URL', () => {
    vi.spyOn(console, 'warn').mockImplementation(() => {})
    const flags: [string | undefined, boolean][] = [
      ['true', true],
      ['1', true],
      [' true ', true],
      [undefined, false],
      ['', false],
      ['false', false],
      ['0', false],
      ['yes', false],
      ['on', false],
      ['TRUE', false],
      ['True', false],
      ['2', false],
    ]
    for (const [flag, on] of flags) {
      for (const url of [undefined, '', '   ', 'not a url', 'javascript:alert(1)', 'http://flash.example/checkout', CHECKOUT, ` ${CHECKOUT} `]) {
        const env = { PAYWALL_ENABLED: flag, FLASH_CHECKOUT_URL: url }
        expect(paywallActive(env), `flag=${flag} url=${url}`).toBe(on && url?.trim() === CHECKOUT)
      }
    }
  })

  it('warns once when PAYWALL_ENABLED is set but the paywall stays off', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    warn.mockClear()
    __resetPaywallWarningForTests()
    paywallActive({ PAYWALL_ENABLED: 'false', FLASH_CHECKOUT_URL: CHECKOUT })
    paywallActive({ PAYWALL_ENABLED: '', FLASH_CHECKOUT_URL: '' })
    expect(warn).not.toHaveBeenCalled()
    for (let i = 0; i < 3; i++) paywallActive({ PAYWALL_ENABLED: 'true', FLASH_CHECKOUT_URL: 'not a url' })
    paywallActive({ PAYWALL_ENABLED: 'TRUE', FLASH_CHECKOUT_URL: CHECKOUT })
    expect(warn).toHaveBeenCalledOnce()
    warn.mockRestore()
  })

  it('reads process.env at call time, so no rebuild is needed', () => {
    vi.stubEnv('FLASH_CHECKOUT_URL', CHECKOUT)
    vi.stubEnv('PAYWALL_ENABLED', '')
    expect(paywallActive()).toBe(false)
    vi.stubEnv('PAYWALL_ENABLED', '1')
    expect(paywallActive()).toBe(true)
  })
})

describe('subscription.getStatus', () => {
  it('denies a read-only session with SIGNER_REQUIRED when the paywall is active', async () => {
    paywall(true)
    const db = fakeDb(null)
    const status = await subscriptionRouter.createCaller(ctx(db, null)).getStatus()
    expect(status).toMatchObject({ paywallActive: true, reason: SIGNER_REQUIRED, hasAccess: false, userPubkey: null })
    expect(db.userSubscription.upsert).not.toHaveBeenCalled()
  })

  it('grants a read-only session access, with no DB work, when the paywall is inactive', async () => {
    paywall(false)
    const db = fakeDb(null)
    const status = await subscriptionRouter.createCaller(ctx(db, null)).getStatus()
    expect(status).toMatchObject({ paywallActive: false, reason: null, hasAccess: true })
    expect(db.userSubscription.upsert).not.toHaveBeenCalled()
    expect(db.userSubscription.findUnique).not.toHaveBeenCalled()
  })

  it('does not start a trial for a signer user while the paywall is inactive', async () => {
    paywall(false)
    const db = fakeDb(expired)
    const status = await subscriptionRouter.createCaller(ctx(db)).getStatus()
    expect(status).toMatchObject({ paywallActive: false, hasAccess: true })
    expect(db.userSubscription.upsert).not.toHaveBeenCalled()
  })

  it('starts the trial on first sight for a signer user when the paywall is active', async () => {
    paywall(true)
    const db = fakeDb(null)
    const status = await subscriptionRouter.createCaller(ctx(db)).getStatus()
    expect(status).toMatchObject({ paywallActive: true, reason: null, status: 'TRIAL', hasAccess: true, daysRemaining: 7 })
    expect(db.userSubscription.upsert).toHaveBeenCalledWith(expect.objectContaining({ update: {} }))
  })

  it('reports an expired row as no access', async () => {
    paywall(true)
    const status = await subscriptionRouter.createCaller(ctx(fakeDb(expired))).getStatus()
    expect(status).toMatchObject({ status: 'EXPIRED', hasAccess: false })
  })
})

describe('subscribedProcedure', () => {
  const router = createTRPCRouter({ read: subscribedProcedure.query(() => 'ok') })

  it('refuses an expired row with FORBIDDEN SUBSCRIPTION_REQUIRED when the paywall is on', async () => {
    paywall(true)
    for (const row of [expired, endedTrial]) {
      const err = await router.createCaller(ctx(fakeDb(row))).read().catch(e => e)
      expect(err).toBeInstanceOf(TRPCError)
      expect(err).toMatchObject({ code: 'FORBIDDEN', message: SUBSCRIPTION_REQUIRED })
    }
  })

  it('allows a live TRIAL and an ACTIVE row with one lookup and no writes', async () => {
    paywall(true)
    for (const row of [trial, active]) {
      const db = fakeDb(row)
      await expect(router.createCaller(ctx(db)).read()).resolves.toBe('ok')
      expect(db.userSubscription.findUnique).toHaveBeenCalledTimes(1)
      expect(db.userSubscription.upsert).not.toHaveBeenCalled()
    }
  })

  it('starts the first-sight trial for a signer user with no row yet', async () => {
    paywall(true)
    const db = fakeDb(null)
    await expect(router.createCaller(ctx(db)).read()).resolves.toBe('ok')
    expect(db.userSubscription.upsert).toHaveBeenCalledWith(expect.objectContaining({ update: {} }))
  })

  it('allows an expired row without touching the DB when the paywall is off', async () => {
    for (const env of [{ flag: '', url: CHECKOUT }, { flag: 'true', url: '' }]) {
      vi.stubEnv('PAYWALL_ENABLED', env.flag)
      vi.stubEnv('FLASH_CHECKOUT_URL', env.url)
      const db = fakeDb(expired)
      await expect(router.createCaller(ctx(db)).read()).resolves.toBe('ok')
      expect(db.userSubscription.findUnique).not.toHaveBeenCalled()
    }
  })

  it('still requires NIP-98 auth first', async () => {
    paywall(true)
    const db = fakeDb(trial)
    await expect(router.createCaller(ctx(db, null)).read()).rejects.toMatchObject({ code: 'UNAUTHORIZED' })
    expect(db.userSubscription.findUnique).not.toHaveBeenCalled()
  })

  it('gates every feed procedure', async () => {
    paywall(true)
    const caller = feedRouter.createCaller(ctx(fakeDb(expired))) as unknown as Record<string, () => Promise<unknown>>
    const names = Object.keys(feedRouter._def.procedures)
    expect(names.length).toBeGreaterThan(30)
    for (const name of names) {
      await expect(caller[name](), name).rejects.toMatchObject({ code: 'FORBIDDEN', message: SUBSCRIPTION_REQUIRED })
    }
  })
})

describe('startTrialIfMissing', () => {
  it('reads the existing row when a parallel first-sight insert wins the race', async () => {
    const winner: Row = { ...trial }
    const db = {
      userSubscription: {
        upsert: vi.fn(async () => {
          throw new Prisma.PrismaClientKnownRequestError('Unique constraint failed on the fields: (`userPubkey`)', {
            code: 'P2002',
            clientVersion: '5.22.0',
          })
        }),
        findUniqueOrThrow: vi.fn(async () => winner),
      },
    }
    await expect(startTrialIfMissing(db as never, PUBKEY, new Date())).resolves.toBe(winner)
    expect(db.userSubscription.findUniqueOrThrow).toHaveBeenCalledWith({ where: { userPubkey: PUBKEY } })
  })

  it('propagates the lookup error when the conflicting row cannot be read', async () => {
    const db = {
      userSubscription: {
        upsert: vi.fn(async () => {
          throw new Prisma.PrismaClientKnownRequestError('Unique constraint failed', { code: 'P2002', clientVersion: '5.22.0' })
        }),
        findUniqueOrThrow: vi.fn(async () => {
          throw new Prisma.PrismaClientKnownRequestError('No record found', { code: 'P2025', clientVersion: '5.22.0' })
        }),
      },
    }
    await expect(startTrialIfMissing(db as never, PUBKEY, new Date())).rejects.toMatchObject({ code: 'P2025' })
  })

  it('rethrows other database errors', async () => {
    const db = {
      userSubscription: {
        upsert: vi.fn(async () => {
          throw new Prisma.PrismaClientKnownRequestError('Timed out', { code: 'P1008', clientVersion: '5.22.0' })
        }),
        findUniqueOrThrow: vi.fn(),
      },
    }
    await expect(startTrialIfMissing(db as never, PUBKEY, new Date())).rejects.toMatchObject({ code: 'P1008' })
    expect(db.userSubscription.findUniqueOrThrow).not.toHaveBeenCalled()
  })
})

describe('assertSubscribed lookup cache', () => {
  it('does one lookup for every gated call in a request', async () => {
    paywall(true)
    const router = createTRPCRouter({ a: subscribedProcedure.query(() => 'a'), b: subscribedProcedure.query(() => 'b') })
    const db = fakeDb(trial)
    const caller = router.createCaller(ctx(db))
    await expect(Promise.all([caller.a(), caller.b(), caller.a()])).resolves.toEqual(['a', 'b', 'a'])
    expect(db.userSubscription.findUnique).toHaveBeenCalledTimes(1)
  })

  it('does not keep a failed lookup, so a later call in the request retries', async () => {
    const cache: SubscriptionLookups = new Map()
    const db = fakeDb(trial)
    db.userSubscription.findUnique.mockRejectedValueOnce(new Error('connection reset'))
    await expect(assertSubscribed(db as never, PUBKEY, new Date(), cache)).rejects.toThrow('connection reset')
    await expect(assertSubscribed(db as never, PUBKEY, new Date(), cache)).resolves.toBeUndefined()
    expect(db.userSubscription.findUnique).toHaveBeenCalledTimes(2)
  })
})
