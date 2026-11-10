import { createTRPCRouter, publicProcedure, protectedProcedure } from '../trpc'
import {
  MONTHLY_PRICE_SATS,
  accessFor,
  buildCheckoutUrl,
  trialEndFrom,
} from '@/lib/subscription'

export const subscriptionRouter = createTRPCRouter({
  // Get current user's subscription status
  // Make this public so anonymous visitors don't get blocked by auth errors
  getStatus: publicProcedure
    .query(async ({ ctx }) => {
      const now = new Date()

      if (ctx.nostrPubkey) {
        // Start the trial on first sight and persist it, so it really ends.
        // The empty update keeps an existing row (and its dates) untouched.
        const subscription = await ctx.db.userSubscription.upsert({
          where: { userPubkey: ctx.nostrPubkey },
          create: { userPubkey: ctx.nostrPubkey, status: 'TRIAL', trialEndsAt: trialEndFrom(now) },
          update: {},
        })

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

  // Create Flash checkout session
  createCheckoutSession: protectedProcedure
    .mutation(({ ctx }) => ({
      checkoutUrl: buildCheckoutUrl(process.env.NEXT_PUBLIC_FLASH_CHECKOUT_URL ?? '', ctx.nostrPubkey),
    })),

  // Cancel subscription
  cancelSubscription: protectedProcedure
    .mutation(async ({ ctx }) => {
      const subscription = await ctx.db.userSubscription.findUnique({
        where: { userPubkey: ctx.nostrPubkey },
      })

      if (!subscription || subscription.status !== 'ACTIVE') {
        throw new Error('No active subscription to cancel')
      }

      // TODO: Cancel in Square
      
      await ctx.db.userSubscription.update({
        where: { userPubkey: ctx.nostrPubkey },
        data: {
          status: 'CANCELLED',
          cancelledAt: new Date(),
        },
      })

      return { success: true }
    }),
})
