import { z } from 'zod'
import { TRPCError } from '@trpc/server'
import { createTRPCRouter, publicProcedure, protectedProcedure } from '@/server/api/trpc'
import { getNostrFetcher } from '@/lib/nostr-fetcher'
import { rateLimit } from '@/server/rate-limit'
import { nip19 } from 'nostr-tools'

export const guideRouter = createTRPCRouter({
  // Get all guide feeds with optional tag filtering
  getGuideFeeds: publicProcedure
    .input(z.object({
      tags: z.array(z.string()).optional(),
      limit: z.number().min(1).max(100).default(50),
      orderBy: z.enum(['newest', 'popular', 'recent_posts']).default('popular'),
    }).optional())
    .query(async ({ ctx, input }) => {
      const whereClause: any = {
        isActive: true,
      }

      // Filter by tags if provided
      if (input?.tags && input.tags.length > 0) {
        whereClause.tags = {
          hasSome: input.tags,
        }
      }

      // Determine sorting
      let orderBy: any = { subscriberCount: 'desc' } // default to popular
      if (input?.orderBy === 'newest') {
        orderBy = { createdAt: 'desc' }
      } else if (input?.orderBy === 'recent_posts') {
        orderBy = { lastPublishedAt: 'desc' }
      }

      const feeds = await ctx.db.guideFeed.findMany({
        where: whereClause,
        orderBy,
        take: input?.limit || 50,
      })

      return feeds
    }),

  // Get all unique tags from guide feeds
  getGuideTags: publicProcedure
    .query(async ({ ctx }) => {
      const feeds = await ctx.db.guideFeed.findMany({
        where: { isActive: true },
        select: { tags: true },
      })

      // Aggregate all tags and count occurrences
      const tagCounts = new Map<string, number>()
      
      for (const feed of feeds) {
        for (const tag of feed.tags) {
          tagCounts.set(tag, (tagCounts.get(tag) || 0) + 1)
        }
      }

      // Convert to array and sort by count
      return Array.from(tagCounts.entries())
        .map(([tag, count]) => ({ tag, count }))
        .sort((a, b) => b.count - a.count)
    }),

  // Submit a new feed to the guide
  // Allow anonymous submissions so people can add feeds without signing in
  submitFeed: publicProcedure
    .input(z.object({
      npub: z.string().startsWith('npub1'),
      tags: z.array(z.string()).min(1).max(10),
    }))
    .mutation(async ({ ctx, input }) => {
      const limit = rateLimit('guide.submitFeed', ctx.clientIp, 5, 60 * 60 * 1000)
      if (!limit.allowed) {
        throw new TRPCError({
          code: 'TOO_MANY_REQUESTS',
          message: `Too many submissions. Try again in ${limit.retryAfterSeconds}s.`,
        })
      }

      // Check if feed already exists
      const existing = await ctx.db.guideFeed.findUnique({
        where: { npub: input.npub },
      })

      if (existing) {
        throw new Error('This feed is already in the guide')
      }

      // Fetch profile and posts from Nostr
      const nostrFetcher = getNostrFetcher()
      
      const profile = await nostrFetcher.getProfile(input.npub)
      const posts = await nostrFetcher.fetchLongFormPosts(input.npub, 10)

      if (!profile) {
        throw new Error('Could not find profile for this npub')
      }

      if (posts.length === 0) {
        throw new Error('This user has no long-form posts. Only users with long-form content can be added to the guide.')
      }

      // Find the most recent post
      const lastPublishedAt = posts.length > 0 
        ? posts.reduce((latest, post) => 
            post.publishedAt > latest ? post.publishedAt : latest, 
            posts[0].publishedAt
          )
        : null

      // Create the guide feed entry
      const guideFeed = await ctx.db.guideFeed.create({
        data: {
          npub: input.npub,
          displayName: profile.name || input.npub.slice(0, 16) + '...',
          about: profile.about,
          picture: profile.picture,
          tags: input.tags,
          // Allow null for anonymous submissions
          submittedBy: ctx.nostrPubkey ?? null,
          lastPublishedAt,
          postCount: posts.length,
        },
      })

      return guideFeed
    }),

  // Get a single guide feed by npub
  getGuideFeed: publicProcedure
    .input(z.object({
      npub: z.string().startsWith('npub1'),
    }))
    .query(async ({ ctx, input }) => {
      return await ctx.db.guideFeed.findUnique({
        where: { npub: input.npub },
      })
    }),

  // Update subscriber count when someone subscribes via the guide
  incrementSubscriberCount: protectedProcedure
    .input(z.object({
      npub: z.string().startsWith('npub1'),
    }))
    .mutation(async ({ ctx, input }) => {
      const guideFeed = await ctx.db.guideFeed.findUnique({
        where: { npub: input.npub },
      })

      if (!guideFeed) {
        return { success: false }
      }

      await ctx.db.guideFeed.update({
        where: { npub: input.npub },
        data: {
          subscriberCount: {
            increment: 1,
          },
        },
      })

      return { success: true }
    }),

  // Refresh feed data (update post count and last published date)
  refreshFeedData: publicProcedure
    .input(z.object({
      npub: z.string().startsWith('npub1'),
    }))
    .mutation(async ({ ctx, input }) => {
      const limit = rateLimit('guide.refreshFeedData', ctx.clientIp, 20, 60 * 1000)
      if (!limit.allowed) {
        throw new TRPCError({
          code: 'TOO_MANY_REQUESTS',
          message: `Too many requests. Try again in ${limit.retryAfterSeconds}s.`,
        })
      }

      const nostrFetcher = getNostrFetcher()
      const posts = await nostrFetcher.fetchLongFormPosts(input.npub, 50)

      const lastPublishedAt = posts.length > 0 
        ? posts.reduce((latest, post) => 
            post.publishedAt > latest ? post.publishedAt : latest, 
            posts[0].publishedAt
          )
        : null

      await ctx.db.guideFeed.update({
        where: { npub: input.npub },
        data: {
          postCount: posts.length,
          lastPublishedAt,
        },
      })

      return { success: true, postCount: posts.length }
    }),

  // Update tags for own guide entry (user must be logged in as this npub)
  updateOwnTags: protectedProcedure
    .input(z.object({
      tags: z.array(z.string()).min(1).max(10),
    }))
    .mutation(async ({ ctx, input }) => {
      // Convert hex pubkey to npub
      const npub = nip19.npubEncode(ctx.nostrPubkey)
      
      const guideFeed = await ctx.db.guideFeed.findUnique({
        where: { npub },
      })

      if (!guideFeed) {
        throw new Error('You are not in the guide. Submit your profile first.')
      }

      await ctx.db.guideFeed.update({
        where: { npub },
        data: { tags: input.tags },
      })

      return { success: true, tags: input.tags }
    }),

  // Delete own guide entry (user must be logged in as this npub)
  deleteOwnEntry: protectedProcedure
    .mutation(async ({ ctx }) => {
      // Convert hex pubkey to npub
      const npub = nip19.npubEncode(ctx.nostrPubkey)
      
      const guideFeed = await ctx.db.guideFeed.findUnique({
        where: { npub },
      })

      if (!guideFeed) {
        throw new Error('You are not in the guide.')
      }

      await ctx.db.guideFeed.delete({
        where: { npub },
      })

      return { success: true }
    }),

  // Get own guide entry (if exists)
  getOwnEntry: protectedProcedure
    .query(async ({ ctx }) => {
      // Convert hex pubkey to npub
      const npub = nip19.npubEncode(ctx.nostrPubkey)
      
      return await ctx.db.guideFeed.findUnique({
        where: { npub },
      })
    }),
})
