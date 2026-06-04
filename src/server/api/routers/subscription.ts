import { createTRPCRouter, publicProcedure, protectedProcedure } from '../trpc'
import { nip19 } from 'nostr-tools'

const MONTHLY_PRICE_SATS = 1750

export const subscriptionRouter = createTRPCRouter({
  // Get current user's subscription status
  // Make this public so anonymous visitors don't get blocked by auth errors
  getStatus: publicProcedure
    .query(async ({ ctx }) => {
      // Check if user has a paid subscription
      if (ctx.nostrPubkey) {
        const subscription = await ctx.db.userSubscription.findUnique({
          where: { userPubkey: ctx.nostrPubkey },
        })

        if (subscription) {
          const now = new Date()
          const endsAt = subscription.subscriptionEndsAt ?? subscription.trialEndsAt
          const daysRemaining = Math.max(0, Math.ceil((endsAt.getTime() - now.getTime()) / (1000 * 60 * 60 * 24)))

          return {
            userPubkey: ctx.nostrPubkey,
            status: subscription.status,
            trialEndsAt: subscription.trialEndsAt,
            subscriptionEndsAt: subscription.subscriptionEndsAt,
            daysRemaining,
            hasAccess: ['TRIAL', 'ACTIVE'].includes(subscription.status) && endsAt > now,
            price: MONTHLY_PRICE_SATS,
          }
        }
      }

      // No paid subscription - give 7 day trial
      const trialEndsAt = new Date(Date.now() + 7 * 24 * 60 * 60 * 1000) // 7 days
      return {
        userPubkey: ctx.nostrPubkey ?? null,
        status: 'TRIAL' as const,
        trialEndsAt,
        daysRemaining: 7,
        hasAccess: true,
        price: MONTHLY_PRICE_SATS,
      }
    }),

  // Create Flash checkout session
  createCheckoutSession: protectedProcedure
    .mutation(async ({ ctx }) => {
      // Return the Flash checkout URL from environment variable
      const flashCheckoutUrl = process.env.NEXT_PUBLIC_FLASH_CHECKOUT_URL || '#'
      
      // Pre-fill user data using Base64-encoded JSON params
      const npub = ctx.nostrPubkey
      if (npub) {
        // Convert npub to hex format (Flash expects hex, not npub)
        const decoded = nip19.decode(npub)
        const hexPubkey = decoded.type === 'npub' ? decoded.data : npub
        
        const params = {
          npub: hexPubkey, // Use hex format instead of npub
          external_uuid: npub, // Keep npub for our mapping
          is_verified: true, // Skip verification since they're already logged in with Nostr
        }
        const base64Params = Buffer.from(JSON.stringify(params)).toString('base64')
        // URL encode the base64 string to handle special characters
        const encodedParams = encodeURIComponent(base64Params)
        return {
          checkoutUrl: `${flashCheckoutUrl}&params=${encodedParams}`,
        }
      }
      
      return {
        checkoutUrl: flashCheckoutUrl,
      }
    }),

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
