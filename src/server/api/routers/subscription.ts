import { createTRPCRouter, publicProcedure, protectedProcedure } from '../trpc'
import { MONTHLY_PRICE_SATS, accessFor, buildCheckoutUrl } from '@/lib/subscription'
import { SIGNER_REQUIRED } from '@/lib/paywall'
import { paywallActive, startTrialIfMissing } from '@/server/paywall'
import { flashApiConfig, KEPT, reconcileWithFlash, shouldCheckFlashOnStatus } from '@/server/flash-reconcile'
import { rateLimit } from '@/server/rate-limit'

export const subscriptionRouter = createTRPCRouter({
  // Public so anonymous and read-only visitors get an answer, not an auth error.
  getStatus: publicProcedure
    .query(async ({ ctx }) => {
      const now = new Date()

      // Off: no DB work and no new trial rows. Rows the pre-flag getStatus
      // already started keep their (likely past) trialEndsAt; run
      // scripts/reset-pre-paywall-trials.mjs once before turning the paywall on.
      if (!paywallActive()) {
        return {
          paywallActive: false,
          reason: null,
          userPubkey: ctx.nostrPubkey,
          status: null,
          trialEndsAt: null,
          subscriptionEndsAt: null,
          daysRemaining: 0,
          hasAccess: true,
          price: MONTHLY_PRICE_SATS,
        }
      }

      // Without a NIP-98 identity (read-only npub view) there is no verified
      // account to start a trial for or to attach a Flash checkout to.
      if (!ctx.nostrPubkey) {
        return {
          paywallActive: true,
          reason: SIGNER_REQUIRED,
          userPubkey: null,
          status: null,
          trialEndsAt: null,
          subscriptionEndsAt: null,
          daysRemaining: 0,
          hasAccess: false,
          price: MONTHLY_PRICE_SATS,
        }
      }

      let subscription = await startTrialIfMissing(ctx.db, ctx.nostrPubkey, now)

      // A lost webhook shows up as a row that denies access or is about to
      // run out, so re-check Flash then (throttled, and best effort).
      if (flashApiConfig() && shouldCheckFlashOnStatus(subscription, now, rateLimit)) {
        const reconciled = await reconcileWithFlash(ctx.db, ctx.nostrPubkey, now).catch(() => null)
        if (reconciled && reconciled !== KEPT) subscription = { ...subscription, ...reconciled }
      }

      return {
        paywallActive: true,
        reason: null,
        userPubkey: ctx.nostrPubkey,
        status: subscription.status,
        trialEndsAt: subscription.trialEndsAt,
        subscriptionEndsAt: subscription.subscriptionEndsAt,
        ...accessFor(subscription, now),
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
