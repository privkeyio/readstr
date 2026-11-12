import { createTRPCRouter, publicProcedure, protectedProcedure } from '../trpc'
import {
  MONTHLY_PRICE_SATS,
  accessFor,
  buildCheckoutUrl,
  trialEndFrom,
} from '@/lib/subscription'
import { flashApiConfig, KEPT, reconcileWithFlash, shouldCheckFlashOnStatus } from '@/server/flash-reconcile'
import { rateLimit } from '@/server/rate-limit'

export const subscriptionRouter = createTRPCRouter({
  // Get current user's subscription status
  // Make this public so anonymous visitors don't get blocked by auth errors
  getStatus: publicProcedure
    .query(async ({ ctx }) => {
      const now = new Date()

      if (ctx.nostrPubkey) {
        // Start the trial on first sight and persist it, so it really ends.
        // The empty update keeps an existing row (and its dates) untouched.
        let subscription = await ctx.db.userSubscription.upsert({
          where: { userPubkey: ctx.nostrPubkey },
          create: { userPubkey: ctx.nostrPubkey, status: 'TRIAL', trialEndsAt: trialEndFrom(now) },
          update: {},
        })

        // A lost webhook shows up as a row that denies access or is about to
        // run out, so re-check Flash then (throttled, and best effort).
        if (flashApiConfig() && shouldCheckFlashOnStatus(subscription, now, rateLimit)) {
          const reconciled = await reconcileWithFlash(ctx.db, ctx.nostrPubkey, now).catch(() => null)
          if (reconciled && reconciled !== KEPT) subscription = { ...subscription, ...reconciled }
        }

        return {
          userPubkey: ctx.nostrPubkey,
          status: subscription.status,
          trialEndsAt: subscription.trialEndsAt,
          subscriptionEndsAt: subscription.subscriptionEndsAt,
          ...accessFor(subscription, now),
          price: MONTHLY_PRICE_SATS,
        }
      }

      // Without a NIP-98 identity there is no row to persist a trial against,
      // so this keeps the previous behaviour of granting access. Read-only npub
      // sessions therefore are not gated.
      const trialEndsAt = trialEndFrom(now)
      return {
        userPubkey: null,
        status: 'TRIAL' as const,
        trialEndsAt,
        subscriptionEndsAt: null,
        daysRemaining: 7,
        hasAccess: true,
        price: MONTHLY_PRICE_SATS,
      }
    }),

  // Create Flash checkout session. FLASH_CHECKOUT_URL is read at runtime (not
  // NEXT_PUBLIC_, which is baked in at build time and absent from the image).
  createCheckoutSession: protectedProcedure
    .mutation(({ ctx }) => ({
      checkoutUrl: buildCheckoutUrl(process.env.FLASH_CHECKOUT_URL ?? '', ctx.nostrPubkey),
    })),
})
