import { z } from 'zod'
import { TRPCError } from '@trpc/server'
import { createTRPCRouter, protectedProcedure } from '@/server/api/trpc'
import { fetchAndParseFeed } from '@/lib/rss-parser'
import { getNostrFetcher, NostrFeedFetcher } from '@/lib/nostr-fetcher'
import { discoverFeed } from '@/lib/feed-discovery'
import { FeedType, type Prisma } from '@prisma/client'
import { nip19 } from 'nostr-tools'
import {
  fetchSubscriptionListFromServer,
  getSyncRelaysFromServer,
  normalizeUrlForComparison,
  normalizeNpub
} from '@/lib/nostr-sync'

// Type for subscription with feed include
type SubscriptionWithFeed = Prisma.SubscriptionGetPayload<{ include: { feed: true } }>

// Type for subscription with full feed details including counts
type SubscriptionWithFeedAndCategory = Prisma.SubscriptionGetPayload<{
  include: {
    feed: { include: { _count: { select: { items: true } } } }
    category: true
  }
}>

// Type for feed item with feed
type FeedItemWithFeedAndReads = Prisma.FeedItemGetPayload<{
  include: {
    feed: true
    readItems: true
    favorites: true
  }
}>

// Type for favorite with feed item and feed
type FavoriteWithFeedItem = Prisma.FavoriteGetPayload<{
  include: { feedItem: { include: { feed: true } } }
}>

// Type for category with counts
type CategoryWithCount = Prisma.CategoryGetPayload<{
  include: { _count: { select: { subscriptions: true } } }
}>

// Type for category with subscriptions
type CategoryWithSubscriptions = Prisma.CategoryGetPayload<{
  include: {
    subscriptions: {
      include: {
        feed: { include: { items: true } }
      }
    }
  }
}>

// Habla.news is a dedicated long-form content viewer that handles naddr links well
const NOSTR_ARTICLE_VIEWER_URL = 'https://habla.news'
// Njump.me as fallback for videos and other content
const NOSTR_EVENT_VIEWER_URL = 'https://njump.me'

const buildNostrOriginalUrl = (feedType: FeedType, guid?: string | null, authorNpub?: string | null, dTag?: string | null) => {
  if (!guid) return undefined
  if (feedType !== 'NOSTR' && feedType !== 'NOSTR_VIDEO') return undefined

  try {
    // Decode author npub to hex if available
    let authorHex: string | undefined
    if (authorNpub) {
      try {
        const decoded = nip19.decode(authorNpub)
        if (decoded.type === 'npub') {
          authorHex = decoded.data
        }
      } catch (e) {
        // Ignore invalid npub
      }
    }

    // For NIP-23 long-form articles (NOSTR type), use Habla.news with naddr
    if (feedType === 'NOSTR' && dTag && authorHex) {
      const naddr = nip19.naddrEncode({
        kind: 30023,
        pubkey: authorHex,
        identifier: dTag,
      })
      return `${NOSTR_ARTICLE_VIEWER_URL}/a/${naddr}`
    }

    // Fallback to njump.me with nevent for videos or if d-tag missing
    const nevent = nip19.neventEncode({
      id: guid,
      author: authorHex,
      relays: ['wss://relay.damus.io', 'wss://nos.lol']
    })
    return `${NOSTR_EVENT_VIEWER_URL}/${nevent}`
  } catch (error) {
    console.error('Failed to build Nostr original URL', { feedType, guid, error })
    return undefined
  }
}

const assertCategoryOwnership = async (db: any, userPubkey: string, categoryId: string) => {
  const category = await db.category.findFirst({
    where: { id: categoryId, userPubkey },
    select: { id: true },
  })
  if (!category) {
    throw new TRPCError({ code: 'FORBIDDEN', message: 'Category not found' })
  }
}

const refreshAllUserFeedsInternal = async (db: any, userPubkey: string, force = false) => {
  // 1. Get all active subscriptions for this user
  const subscriptions = await db.subscription.findMany({
    where: { userPubkey: userPubkey },
    include: { feed: true },
  })

  // 2. Identify unique active feeds that need refreshing
  const feedsToRefresh = subscriptions
    .map((s: any) => s.feed)
    .filter((f: any) => f.isActive)

  // Deduplicate by feed ID to avoid double-processing
  const uniqueFeeds = Array.from(new Map(feedsToRefresh.map((f: any) => [f.id, f])).values())

  const results = {
    total: uniqueFeeds.length,
    refreshed: 0,
    newItems: 0,
    errors: [] as string[],
  }

  if (uniqueFeeds.length === 0) return results

  // 3. Process feeds with a concurrency limit to prevent resource exhaustion
  const CONCURRENCY_LIMIT = 5
  // Throttle individual feed fetches: don't hit the same feed more than once every 5 minutes globally
  const GLOBAL_REFRESH_COOLDOWN_MS = 5 * 60 * 1000

  for (let i = 0; i < uniqueFeeds.length; i += CONCURRENCY_LIMIT) {
    const chunk = uniqueFeeds.slice(i, i + CONCURRENCY_LIMIT)

    await Promise.all(chunk.map(async (feed: any) => {
      try {
        // Global cooldown check: skip if recently fetched by ANY user (unless forced)
        const now = new Date()
        const isCooldownActive = feed.lastFetchedAt && (now.getTime() - feed.lastFetchedAt.getTime() < GLOBAL_REFRESH_COOLDOWN_MS)
        if (isCooldownActive && !force) {
          results.refreshed++
          return
        }

        let fetchedItems: any[] = []

        // A. Handle RSS Feeds
        if (feed.type === 'RSS' && feed.url) {
          const parsedFeed = await fetchAndParseFeed(feed.url)
          fetchedItems = parsedFeed.items.map((item: any) => ({
            feedId: feed.id,
            title: item.title || 'Untitled',
            content: item.content || '',
            author: item.author,
            publishedAt: item.publishedAt,
            url: item.url || null,
            guid: item.guid || item.url || null,
            videoId: item.videoId,
            embedUrl: item.embedUrl,
            thumbnail: item.thumbnail,
          }))
        }
        // B. Handle Nostr Feeds
        else if ((feed.type === 'NOSTR' || feed.type === 'NOSTR_VIDEO') && feed.npub) {
          const nostrFetcher = getNostrFetcher()
          if (feed.type === 'NOSTR') {
            const posts = await nostrFetcher.fetchLongFormPosts(feed.npub, 50, feed.lastFetchedAt || undefined)
            fetchedItems = posts.map((post: any) => ({
              feedId: feed.id,
              title: post.title || 'Untitled',
              content: post.content || '',
              author: post.author,
              publishedAt: post.publishedAt,
              url: post.url || null,
              guid: post.id, // Event ID as GUID
            }))
          } else {
            const videos = await nostrFetcher.fetchVideoEvents(feed.npub, 50, feed.lastFetchedAt || undefined)
            fetchedItems = videos.map((video: any) => ({
              feedId: feed.id,
              title: video.title || 'Untitled Video',
              content: video.content || '',
              author: video.author,
              publishedAt: video.publishedAt,
              url: video.videoUrl || null,
              guid: video.id, // Event ID as GUID
              embedUrl: video.embedUrl,
              thumbnail: video.thumbnail,
            }))
          }
        }

        // 4. Update feed metadata (timestamp)
        await db.feed.update({
          where: { id: feed.id },
          data: { lastFetchedAt: new Date() },
        })

        // 5. Bulk insert new items
        if (fetchedItems.length > 0) {
          // Filter out items without a GUID (Prisma requirement for unique constraint)
          const validItems = fetchedItems.filter((item: any) => item.guid)

          if (validItems.length > 0) {
            const created = await db.feedItem.createMany({
              data: validItems,
              skipDuplicates: true,
            })
            results.newItems += created.count
          }
        }

        results.refreshed++
      } catch (error) {
        const errorMsg = error instanceof Error ? error.message : 'Unknown error'
        results.errors.push(`Failed to refresh ${feed.title || feed.id}: ${errorMsg}`)
        console.error(`❌ Sync error for feed ${feed.id}:`, error)
      }
    }))
  }

  return results
}

export const feedRouter = createTRPCRouter({
  // Update user's Nostr relays (deprecated - relays are managed client-side now)
  updateNostrRelays: protectedProcedure
    .input(z.object({
      relays: z.array(z.string()),
    }))
    .mutation(async ({ ctx, input }) => {
      // This endpoint is kept for backward compatibility but no longer stores relays
      // Relays are now managed client-side in localStorage
      return { success: true }
    }),

  // Get all user feeds with unread counts
  getFeeds: protectedProcedure
    .input(z.object({
      tags: z.array(z.string()).optional(),
      categoryId: z.string().optional(),
      autoSync: z.boolean().default(true),
      forceSync: z.boolean().optional(),
    }).optional())
    .query(async ({ ctx, input }) => {
      // 1. Check for automatic sync if enabled
      if (input?.autoSync !== false) {
        try {
          // Get user preferences for relays and last sync
          const prefs = await ctx.db.userPreference.findUnique({
            where: { userPubkey: ctx.nostrPubkey },
          })

          // Only sync if never synced, synced more than 15 minutes ago, or forced
          const lastSync = prefs?.updatedAt
          const SYNC_INTERVAL = 15 * 60 * 1000 // 15 minutes
          const syncThreshold = new Date(Date.now() - SYNC_INTERVAL)

          if (!lastSync || lastSync < syncThreshold || input?.forceSync === true) {
            // Use default relays for sync
            const remoteResult = await fetchSubscriptionListFromServer(ctx.nostrPubkey)

            // Ignore stale events: a relay must not roll back state with an
            // equal-or-older subscription list than the one we last applied.
            const remoteIsFresh =
              remoteResult.createdAt != null &&
              (prefs?.lastSubscriptionSyncCreatedAt == null ||
                remoteResult.createdAt > Number(prefs.lastSubscriptionSyncCreatedAt))

            if (remoteResult.success && remoteResult.data) {
              const remoteList = remoteResult.data

              // Get current subscriptions to compare
              const currentSubs = await ctx.db.subscription.findMany({
                where: { userPubkey: ctx.nostrPubkey },
                include: { feed: true },
              })

              const localRssUrls = new Set(
                currentSubs
                  .filter((s: SubscriptionWithFeed) => s.feed.type === 'RSS')
                  .map((s: SubscriptionWithFeed) => normalizeUrlForComparison(s.feed.url!))
              )

              const localNpubs = new Set(
                currentSubs
                  .filter((s: SubscriptionWithFeed) => s.feed.type === 'NOSTR' || s.feed.type === 'NOSTR_VIDEO')
                  .map((s: SubscriptionWithFeed) => normalizeNpub(s.feed.npub || s.feed.url!))
              )

              // Identify new feeds from remote
              const feedsToAdd: Array<{ type: 'RSS' | 'NOSTR'; url: string; tags: string[]; category?: { name: string; color?: string; icon?: string } }> = []

              for (const rssUrl of remoteList.rss) {
                if (!localRssUrls.has(normalizeUrlForComparison(rssUrl))) {
                  feedsToAdd.push({ 
                    type: 'RSS', 
                    url: rssUrl, 
                    tags: remoteList.tags?.[rssUrl] || [],
                    category: remoteList.categories?.[rssUrl]
                  })
                }
              }

              for (const npub of remoteList.nostr) {
                if (!localNpubs.has(normalizeNpub(npub))) {
                  feedsToAdd.push({ 
                    type: 'NOSTR', 
                    url: npub, 
                    tags: remoteList.tags?.[npub] || [],
                    category: remoteList.categories?.[npub]
                  })
                }
              }

              if (remoteIsFresh && feedsToAdd.length > 0) {
                for (const feed of feedsToAdd) {
                  try {
                    // Handle category if present
                    let categoryId: string | undefined
                    if (feed.category) {
                      // Try to find existing category by name
                      let category = await ctx.db.category.findFirst({
                        where: {
                          userPubkey: ctx.nostrPubkey,
                          name: feed.category.name
                        }
                      })

                      // Create category if it doesn't exist
                      if (!category) {
                        const maxSortOrder = await ctx.db.category.aggregate({
                          where: { userPubkey: ctx.nostrPubkey },
                          _max: { sortOrder: true }
                        })

                        category = await ctx.db.category.create({
                          data: {
                            userPubkey: ctx.nostrPubkey,
                            name: feed.category.name,
                            color: feed.category.color,
                            icon: feed.category.icon,
                            sortOrder: (maxSortOrder._max.sortOrder ?? -1) + 1
                          }
                        })
                      }

                      categoryId = category.id
                    }

                    let feedUrl = feed.url
                    let feedRecord = await ctx.db.feed.findFirst({
                      where: feed.type === 'RSS'
                        ? { type: 'RSS', url: feedUrl }
                        : { type: 'NOSTR', npub: feed.url, title: { not: { endsWith: '(Videos)' } } }
                    })

                    if (!feedRecord) {
                      if (feed.type === 'RSS') {
                        const discovery = await discoverFeed(feedUrl)
                        if (discovery.found) {
                          feedUrl = discovery.feedUrl!
                          feedRecord = await ctx.db.feed.create({
                            data: {
                              type: 'RSS',
                              url: feedUrl,
                              title: discovery.title || new URL(feedUrl).hostname,
                            }
                          })
                        }
                      } else {
                        feedRecord = await ctx.db.feed.create({
                          data: {
                            type: 'NOSTR',
                            npub: feed.url,
                            title: `Nostr Feed (${feed.url.slice(0, 8)}...)`,
                          }
                        })
                      }
                    }

                    if (feedRecord) {
                      await ctx.db.subscription.upsert({
                        where: {
                          userPubkey_feedId: {
                            userPubkey: ctx.nostrPubkey,
                            feedId: feedRecord.id,
                          }
                        },
                        create: {
                          userPubkey: ctx.nostrPubkey,
                          feedId: feedRecord.id,
                          tags: feed.tags,
                          categoryId,
                        },
                        update: {
                          tags: feed.tags,
                          categoryId,
                        }
                      })
                    }
                  } catch (e) {
                    console.error(`❌ Auto-sync error adding feed ${feed.url}:`, e)
                  }
                }
              }

              // Update updatedAt manually to track last sync even if no feeds were added.
              // Advance the freshness watermark only for fresh events so older
              // events are rejected later and cannot roll back state.
              await ctx.db.userPreference.upsert({
                where: { userPubkey: ctx.nostrPubkey },
                create: {
                  userPubkey: ctx.nostrPubkey,
                  ...(remoteIsFresh ? { lastSubscriptionSyncCreatedAt: BigInt(remoteResult.createdAt!) } : {}),
                },
                update: {
                  updatedAt: new Date(),
                  ...(remoteIsFresh ? { lastSubscriptionSyncCreatedAt: BigInt(remoteResult.createdAt!) } : {}),
                },
              })

              // Also trigger a refresh of feed contents if forced or on sync
              await refreshAllUserFeedsInternal(ctx.db, ctx.nostrPubkey)
            } else if (input?.forceSync === true) {
              // Even if Nostr sync wasn't needed, if forced, refresh feed contents
              await refreshAllUserFeedsInternal(ctx.db, ctx.nostrPubkey)
            }
          }
        } catch (e) {
          console.error('❌ Auto-sync failed:', e)
        }
      }

      const whereClause: Prisma.SubscriptionWhereInput = {
        userPubkey: ctx.nostrPubkey,
        deletedAt: null, // Exclude soft-deleted subscriptions
      }

      // Filter by tags if provided
      if (input?.tags && input.tags.length > 0) {
        whereClause.tags = {
          hasEvery: input.tags,
        }
      }

      // Filter by category if provided
      if (input?.categoryId) {
        // Handle "uncategorized" as a special case
        if (input.categoryId === 'uncategorized') {
          whereClause.categoryId = null
        } else {
          whereClause.categoryId = input.categoryId
        }
      }

      const subscriptions = await ctx.db.subscription.findMany({
        where: whereClause,
        include: {
          feed: {
            include: {
              _count: {
                select: {
                  items: {
                    where: {
                      readItems: {
                        none: {
                          userPubkey: ctx.nostrPubkey,
                        },
                      },
                    },
                  },
                },
              },
            },
          },
          category: true,
        },
      })

      return subscriptions.map((sub: SubscriptionWithFeedAndCategory) => ({
        id: sub.feed.id,
        title: sub.feed.title,
        type: sub.feed.type,
        url: sub.feed.url,
        npub: sub.feed.npub,
        unreadCount: sub.feed._count.items,
        subscribedAt: sub.createdAt,
        tags: sub.tags,
        categoryId: sub.categoryId,
        category: sub.category ? {
          id: sub.category.id,
          name: sub.category.name,
          color: sub.category.color,
          icon: sub.category.icon,
        } : null,
      }))
    }),

  // Get all subscriptions for sync (including deleted)
  getAllSubscriptionsForSync: protectedProcedure
    .query(async ({ ctx }) => {
      const subscriptions = await ctx.db.subscription.findMany({
        where: { 
          userPubkey: ctx.nostrPubkey,
        },
        include: {
          feed: true,
          category: true,
        },
      })

      return subscriptions.map(sub => ({
        type: sub.feed.type,
        url: sub.feed.url || sub.feed.npub || '',
        tags: sub.tags,
        category: sub.category ? {
          name: sub.category.name,
          color: sub.category.color,
          icon: sub.category.icon,
        } : null,
        deletedAt: sub.deletedAt,
      }))
    }),

  // Get feed items for a specific feed or all feeds
  getFeedItems: protectedProcedure
    .input(z.object({
      feedId: z.string().optional(),
      feedIds: z.array(z.string()).optional(), // Array of feed IDs for tag filtering
      limit: z.number().min(1).max(100).default(50),
      cursor: z.string().optional(),
      unreadOnly: z.boolean().optional(),
    }))
    .query(async ({ ctx, input }) => {
      const whereClause: Prisma.FeedItemWhereInput = {}

      // Sanitize feedIds to guard against deserialization quirks (e.g. ['undefined'])
      const sanitizedFeedIds = Array.isArray(input.feedIds)
        ? input.feedIds.filter((id): id is string => !!id && typeof id === 'string' && id !== 'undefined')
        : []

      if (input.feedId) {
        whereClause.feedId = input.feedId
      } else {
        let derivedFeedIds: string[] = []

        if (sanitizedFeedIds.length > 0) {
          derivedFeedIds = sanitizedFeedIds
        } else {
          // Only show items from feeds the user is subscribed to
          const subscriptions = await ctx.db.subscription.findMany({
            where: { userPubkey: ctx.nostrPubkey },
            select: { feedId: true },
          })
          derivedFeedIds = subscriptions.map((s: { feedId: string }) => s.feedId)
        }

        if (derivedFeedIds.length === 0) {
          return {
            items: [],
            nextCursor: undefined,
          }
        }

        whereClause.feedId = {
          in: derivedFeedIds,
        }
      }

      if (input.unreadOnly) {
        whereClause.readItems = {
          none: {
            userPubkey: ctx.nostrPubkey,
          },
        }
      }

      let items
      try {
        items = await ctx.db.feedItem.findMany({
          where: whereClause,
          include: {
            feed: true,
            readItems: {
              where: {
                userPubkey: ctx.nostrPubkey,
              },
            },
            favorites: {
              where: {
                userPubkey: ctx.nostrPubkey,
              },
            },
          },
          orderBy: [
            { publishedAt: 'desc' },
            { id: 'desc' },
          ],
          take: input.limit + 1,
          cursor: input.cursor ? { id: input.cursor } : undefined,
        })
      } catch (error) {
        console.error('❌ Error querying feed items:', {
          user: ctx.nostrPubkey,
          whereClause,
          input,
          error,
        })
        throw error
      }

      let nextCursor: string | undefined = undefined
      if (items.length > input.limit) {
        const nextItem = items.pop()
        nextCursor = nextItem!.id
      }

      try {
        const mappedItems = items.map((item: FeedItemWithFeedAndReads) => {
          // For NOSTR items, item.url holds the d-tag
          const originalUrl = buildNostrOriginalUrl(item.feed.type, item.guid, item.author, item.url) ?? item.url ?? undefined

          return {
            id: item.id,
            feedId: item.feedId,
            title: item.title,
            content: item.content,
            author: item.author,
            publishedAt: item.publishedAt,
            url: item.url,
            originalUrl,
            videoId: item.videoId,
            embedUrl: item.embedUrl,
            thumbnail: item.thumbnail,
            isRead: item.readItems.length > 0,
            isFavorited: item.favorites.length > 0,
            feedTitle: item.feed.title,
            feedType: item.feed.type,
          }
        })

        return {
          items: mappedItems,
          nextCursor,
        }
      } catch (error) {
        console.error('❌ Error mapping feed items:', error)
        console.error('Items count:', items.length)
        console.error('Sample item:', items[0] ? JSON.stringify({
          id: items[0].id,
          hasReadItems: !!items[0].readItems,
          hasFavorites: !!items[0].favorites,
          hasFeed: !!items[0].feed,
          feedType: items[0].feed?.type
        }) : 'no items')
        throw error
      }
    }),

  // Subscribe to a new feed
  subscribeFeed: protectedProcedure
    .input(z.object({
      type: z.enum(['RSS', 'NOSTR']),
      url: z.string().optional(),
      npub: z.string().optional(),
      title: z.string().optional(),
      tags: z.array(z.string()).optional(),
      categoryId: z.string().optional().transform(v => v || null),
    }))
    .mutation(async ({ ctx, input }) => {
      // Validate input based on type
      if (input.type === 'RSS' && !input.url) {
        throw new Error('RSS feeds require a URL')
      }
      if (input.type === 'NOSTR' && !input.npub) {
        throw new Error('Nostr feeds require an npub')
      }

      // Ensure the target category, if any, belongs to the requesting user
      if (input.categoryId) {
        await assertCategoryOwnership(ctx.db, ctx.nostrPubkey, input.categoryId)
      }

      let feedUrl = input.url
      let feedTitle = input.title

      // For RSS feeds, try to discover the actual feed URL
      if (input.type === 'RSS' && feedUrl) {
        const discovery = await discoverFeed(feedUrl)

        if (!discovery.found) {
          throw new Error(discovery.error || 'Could not find a valid RSS or Atom feed at this URL. Please check the URL and try again.')
        }

        feedUrl = discovery.feedUrl

        // If discovery didn't find a proper title (or just found generic "RSS"), parse the feed
        if (!discovery.title || discovery.title.toLowerCase() === 'rss' || discovery.title.toLowerCase() === 'atom') {
          try {
            const parsedFeed = await fetchAndParseFeed(discovery.feedUrl!)
            feedTitle = parsedFeed.title || input.title || new URL(discovery.feedUrl!).hostname
          } catch (error) {
            console.error('Error parsing feed for title:', error)
            feedTitle = input.title || new URL(discovery.feedUrl!).hostname
          }
        } else {
          feedTitle = discovery.title || input.title || new URL(discovery.feedUrl!).hostname
        }
      }

      // Check if feed already exists
      let existingFeed
      if (input.type === 'RSS') {
        existingFeed = await ctx.db.feed.findFirst({
          where: {
            type: 'RSS',
            url: feedUrl,
          },
        })
      } else {
        existingFeed = await ctx.db.feed.findFirst({
          where: {
            type: 'NOSTR',
            npub: input.npub,
            title: { not: { endsWith: '(Videos)' } }, // Exclude video feeds
          },
        })
      }

      let feed
      let mainFeedHasVideos = false
      if (existingFeed) {
        feed = existingFeed

        // If it's a Nostr feed and doesn't have a proper title (still shows npub), update it
        if (feed.type === 'NOSTR' && feed.npub && feed.title.includes('npub1')) {
          const nostrFetcher = getNostrFetcher()
          const profile = await nostrFetcher.getProfile(feed.npub)
          if (profile?.name) {
            feed = await ctx.db.feed.update({
              where: { id: feed.id },
              data: { title: profile.name },
            })
          }
        }

        // Check if feed has any items - if not, fetch them
        const itemCount = await ctx.db.feedItem.count({
          where: { feedId: feed.id },
        })

        if (itemCount === 0) {
          // Feed exists but has no items - fetch them now
          if (feed.type === 'RSS' && feed.url) {
            try {
              const parsedFeed = await fetchAndParseFeed(feed.url)

              for (const item of parsedFeed.items) {
                try {
                  // Mark if this feed contains video items
                  if (item.videoId || item.embedUrl) mainFeedHasVideos = true
                  await ctx.db.feedItem.create({
                    data: {
                      feedId: feed.id,
                      title: item.title,
                      content: item.content,
                      author: item.author,
                      publishedAt: item.publishedAt,
                      url: item.url,
                      guid: item.guid,
                      videoId: item.videoId,
                      embedUrl: item.embedUrl,
                      thumbnail: item.thumbnail,
                    },
                  })
                } catch (error) {
                  console.error('Error adding RSS item:', error)
                }
              }
            } catch (error) {
              console.error('Error fetching RSS feed:', error)
            }
          }

          // For existing Nostr article feeds with no items: fetch posts and ensure video feed exists
          if (feed.type === 'NOSTR' && feed.npub) {
            const nostrFetcher = getNostrFetcher()
            const [posts, videos] = await Promise.all([
              nostrFetcher.fetchLongFormPosts(feed.npub, 50),
              nostrFetcher.fetchVideoEvents(feed.npub, 50),
            ])

            // Add long-form posts to the article feed
            for (const post of posts) {
              try {
                await ctx.db.feedItem.create({
                  data: {
                    feedId: feed.id,
                    title: post.title,
                    content: post.content,
                    author: post.author,
                    publishedAt: post.publishedAt,
                    url: post.url,
                    guid: post.id,
                  },
                })
              } catch (error) {
                console.error('Error adding Nostr post:', error)
              }
            }

            // Ensure separate video feed exists and add videos there
            if (videos.length > 0) {
              let videoFeed = await ctx.db.feed.findFirst({
                where: { type: 'NOSTR_VIDEO' as const, npub: feed.npub },
              })

              if (!videoFeed) {
                try {
                  videoFeed = await ctx.db.feed.create({
                    data: {
                      type: 'NOSTR_VIDEO' as const,
                      title: `${feed.title} (Videos)`,
                      npub: feed.npub,
                    },
                  })
                } catch (error) {
                  console.error('Error creating video feed for npub:', error)
                }
              }

              if (videoFeed) {
                for (const video of videos) {
                  try {
                    await ctx.db.feedItem.create({
                      data: {
                        feedId: videoFeed.id,
                        title: video.title,
                        content: video.content,
                        author: video.author,
                        publishedAt: video.publishedAt,
                        url: video.videoUrl,
                        guid: video.id,
                        embedUrl: video.embedUrl,
                        thumbnail: video.thumbnail,
                      },
                    })
                  } catch (error) {
                    console.error('Error adding Nostr video:', error)
                  }
                }
              }
            }
          }
        }
      } else {
        // For Nostr feeds, fetch profile info to get the display name
        let finalTitle = feedTitle
        let videoFeedTitle = feedTitle
        if (input.type === 'NOSTR' && input.npub) {
          const nostrFetcher = getNostrFetcher()
          const profile = await nostrFetcher.getProfile(input.npub)
          const displayName = profile?.name || `${input.npub.slice(0, 16)}...`
          finalTitle = displayName
          videoFeedTitle = `${displayName} (Videos)`
        } else if (input.type === 'RSS') {
          finalTitle = feedTitle || new URL(feedUrl!).hostname
        }

        // Create main feed (articles for Nostr, all content for RSS)
        feed = await ctx.db.feed.create({
          data: {
            type: input.type,
            title: finalTitle || 'Untitled Feed',
            url: feedUrl,
            npub: input.npub,
          },
        })

        // For RSS feeds, immediately fetch initial items
        if (input.type === 'RSS' && feedUrl) {
          try {
            const parsedFeed = await fetchAndParseFeed(feedUrl)

            // Add items to database
            for (const item of parsedFeed.items) {
              try {
                if (item.videoId || item.embedUrl) mainFeedHasVideos = true
                await ctx.db.feedItem.create({
                  data: {
                    feedId: feed.id,
                    title: item.title,
                    content: item.content,
                    author: item.author,
                    publishedAt: item.publishedAt,
                    url: item.url,
                    guid: item.guid,
                    videoId: item.videoId,
                    embedUrl: item.embedUrl,
                    thumbnail: item.thumbnail,
                  },
                })
              } catch (error) {
                console.error('Error adding RSS item:', error)
                // Continue with other items
              }
            }
          } catch (error) {
            console.error('Error fetching RSS feed:', error)
            // Feed is created but empty - user can refresh later
          }
        }

        // For Nostr feeds, immediately fetch initial posts and videos
        if (input.type === 'NOSTR' && input.npub) {
          const nostrFetcher = getNostrFetcher()

          // Fetch both long-form posts and video events
          const [posts, videos] = await Promise.all([
            nostrFetcher.fetchLongFormPosts(input.npub, 50),
            nostrFetcher.fetchVideoEvents(input.npub, 50),
          ])

          // Add long-form posts to main feed
          for (const post of posts) {
            try {
              await ctx.db.feedItem.create({
                data: {
                  feedId: feed.id,
                  title: post.title,
                  content: post.content,
                  author: post.author,
                  publishedAt: post.publishedAt,
                  url: post.url,
                  guid: post.id,
                },
              })
            } catch (error) {
              console.error('Error adding Nostr post:', error)
              // Continue with other posts
            }
          }

          // If there are videos, create a separate video feed
          let videoFeed = null
          if (videos.length > 0) {
            // Check if video feed already exists
            const existingVideoFeed = await ctx.db.feed.findFirst({
              where: {
                type: 'NOSTR_VIDEO' as const,
                npub: input.npub,
                title: { endsWith: '(Videos)' },
              },
            })

            if (existingVideoFeed) {
              videoFeed = existingVideoFeed
            } else {
              // Create separate video feed
              videoFeed = await ctx.db.feed.create({
                data: {
                  type: 'NOSTR_VIDEO' as const,
                  title: videoFeedTitle || `${finalTitle} (Videos)`,
                  npub: input.npub,
                },
              })
            }

            // Add video events to video feed
            for (const video of videos) {
              try {
                await ctx.db.feedItem.create({
                  data: {
                    feedId: videoFeed.id,
                    title: video.title,
                    content: video.content,
                    author: video.author,
                    publishedAt: video.publishedAt,
                    url: video.videoUrl,
                    guid: video.id,
                    embedUrl: video.embedUrl,
                    thumbnail: video.thumbnail,
                  },
                })
              } catch (error) {
                console.error('Error adding Nostr video:', error)
                // Continue with other videos
              }
            }
          }
        }
      }

      // Check if user is already subscribed to the main feed
      const existingSubscription = await ctx.db.subscription.findUnique({
        where: {
          userPubkey_feedId: {
            userPubkey: ctx.nostrPubkey,
            feedId: feed.id,
          },
        },
      })

      if (existingSubscription) {
        // If already subscribed, just return the feed info without error
        return {
          id: feed.id,
          title: feed.title,
          type: feed.type,
          url: feed.url,
          npub: feed.npub,
          tags: existingSubscription.tags || [],
        }
      }

      // Create subscription for main feed
      const subscriptionTags = (input.tags || []).concat(mainFeedHasVideos ? ['video'] : [])
      await ctx.db.subscription.create({
        data: {
          userPubkey: ctx.nostrPubkey,
          feedId: feed.id,
          tags: subscriptionTags,
          categoryId: input.categoryId,
        },
      })

      // If this is a Nostr feed with videos, also subscribe to video feed
      if (input.type === 'NOSTR' && input.npub) {
        const videoFeed = await ctx.db.feed.findFirst({
          where: {
            type: 'NOSTR_VIDEO' as const,
            npub: input.npub,
            title: { endsWith: '(Videos)' },
          },
        })

        if (videoFeed) {
          // Check if already subscribed to video feed
          const existingVideoSub = await ctx.db.subscription.findUnique({
            where: {
              userPubkey_feedId: {
                userPubkey: ctx.nostrPubkey,
                feedId: videoFeed.id,
              },
            },
          })

          if (!existingVideoSub) {
            // Auto-tag video feed with 'video' tag plus user's tags
            const videoTags = [...(input.tags || []), 'video']
            await ctx.db.subscription.create({
              data: {
                userPubkey: ctx.nostrPubkey,
                feedId: videoFeed.id,
                tags: videoTags,
                categoryId: input.categoryId, // Use same category as main feed
              },
            })
          }
        }
      }

      return {
        id: feed.id,
        title: feed.title,
        type: feed.type,
        url: feed.url,
        npub: feed.npub,
        tags: input.tags || [],
        categoryId: input.categoryId,
      }
    }),

  // Unsubscribe from a feed (soft delete for sync tracking)
  unsubscribeFeed: protectedProcedure
    .input(z.object({
      feedId: z.string(),
    }))
    .mutation(async ({ ctx, input }) => {
      // Soft delete by setting deletedAt timestamp
      await ctx.db.subscription.update({
        where: {
          userPubkey_feedId: {
            userPubkey: ctx.nostrPubkey,
            feedId: input.feedId,
          },
        },
        data: {
          deletedAt: new Date(),
        },
      })

      return { success: true }
    }),

  // Update subscription tags
  updateSubscriptionTags: protectedProcedure
    .input(z.object({
      feedId: z.string(),
      tags: z.array(z.string()),
    }))
    .mutation(async ({ ctx, input }) => {
      await ctx.db.subscription.update({
        where: {
          userPubkey_feedId: {
            userPubkey: ctx.nostrPubkey,
            feedId: input.feedId,
          },
        },
        data: {
          tags: input.tags,
        },
      })

      return { success: true }
    }),

  // Get all user tags with unread counts
  getUserTags: protectedProcedure
    .query(async ({ ctx }) => {
      const subscriptions = await ctx.db.subscription.findMany({
        where: {
          userPubkey: ctx.nostrPubkey,
        },
        include: {
          feed: {
            include: {
              items: {
                where: {
                  readItems: {
                    none: {
                      userPubkey: ctx.nostrPubkey,
                    },
                  },
                },
              },
            },
          },
        },
      })

      // Aggregate tags with unread counts
      const tagMap = new Map<string, { tag: string; unreadCount: number; feedCount: number }>()

      for (const sub of subscriptions) {
        const unreadCount = sub.feed.items.length

        for (const tag of sub.tags) {
          const existing = tagMap.get(tag)
          if (existing) {
            existing.unreadCount += unreadCount
            existing.feedCount += 1
          } else {
            tagMap.set(tag, {
              tag,
              unreadCount,
              feedCount: 1,
            })
          }
        }
      }

      return Array.from(tagMap.values()).sort((a, b) => a.tag.localeCompare(b.tag))
    }),

  // Mark an item as read
  markAsRead: protectedProcedure
    .input(z.object({
      itemId: z.string(),
    }))
    .mutation(async ({ ctx, input }) => {
      await ctx.db.readItem.upsert({
        where: {
          userPubkey_itemId: {
            userPubkey: ctx.nostrPubkey,
            itemId: input.itemId,
          },
        },
        create: {
          userPubkey: ctx.nostrPubkey,
          itemId: input.itemId,
        },
        update: {},
      })

      return { success: true }
    }),

  // Mark an item as unread
  markAsUnread: protectedProcedure
    .input(z.object({
      itemId: z.string(),
    }))
    .mutation(async ({ ctx, input }) => {
      await ctx.db.readItem.delete({
        where: {
          userPubkey_itemId: {
            userPubkey: ctx.nostrPubkey,
            itemId: input.itemId,
          },
        },
      })

      return { success: true }
    }),

  // Add item to favorites
  addFavorite: protectedProcedure
    .input(z.object({
      itemId: z.string(),
    }))
    .mutation(async ({ ctx, input }) => {
      await ctx.db.favorite.create({
        data: {
          userPubkey: ctx.nostrPubkey,
          itemId: input.itemId,
        },
      })

      return { success: true }
    }),

  // Remove item from favorites
  removeFavorite: protectedProcedure
    .input(z.object({
      itemId: z.string(),
    }))
    .mutation(async ({ ctx, input }) => {
      await ctx.db.favorite.delete({
        where: {
          userPubkey_itemId: {
            userPubkey: ctx.nostrPubkey,
            itemId: input.itemId,
          },
        },
      })

      return { success: true }
    }),

  // Get all favorited items
  getFavorites: protectedProcedure
    .input(z.object({
      limit: z.number().min(1).max(100).default(50),
      cursor: z.string().optional(),
    }))
    .query(async ({ ctx, input }) => {
      const favorites = await ctx.db.favorite.findMany({
        where: {
          userPubkey: ctx.nostrPubkey,
        },
        include: {
          feedItem: {
            include: {
              feed: true,
            },
          },
        },
        orderBy: {
          createdAt: 'desc',
        },
        take: input.limit + 1,
        cursor: input.cursor ? { id: input.cursor } : undefined,
      })

      let nextCursor: string | undefined = undefined
      if (favorites.length > input.limit) {
        const nextItem = favorites.pop()
        nextCursor = nextItem!.id
      }

      return {
        items: favorites.map((fav: FavoriteWithFeedItem) => ({
          id: fav.feedItem.id,
          title: fav.feedItem.title,
          content: fav.feedItem.content,
          author: fav.feedItem.author,
          publishedAt: fav.feedItem.publishedAt,
          url: fav.feedItem.url,
          originalUrl: buildNostrOriginalUrl(fav.feedItem.feed.type, fav.feedItem.guid) ?? fav.feedItem.url ?? undefined,
          videoId: fav.feedItem.videoId,
          embedUrl: fav.feedItem.embedUrl,
          thumbnail: fav.feedItem.thumbnail,
          isRead: true, // Favorites are typically already read
          isFavorited: true,
          feedTitle: fav.feedItem.feed.title,
          feedType: fav.feedItem.feed.type,
          favoritedAt: fav.createdAt,
        })),
        nextCursor,
      }
    }),

  // Mark all items in a feed as read
  markFeedAsRead: protectedProcedure
    .input(z.object({
      feedId: z.string(),
    }))
    .mutation(async ({ ctx, input }) => {
      // Get all items in the feed
      const feedItems = await ctx.db.feedItem.findMany({
        where: { feedId: input.feedId },
        select: { id: true },
      })

      // Create read items for all unread items
      const readItemsData = feedItems.map((item: { id: string }) => ({
        userPubkey: ctx.nostrPubkey,
        itemId: item.id,
      }))

      await ctx.db.readItem.createMany({
        data: readItemsData,
        skipDuplicates: true,
      })

      return { success: true }
    }),

  // Discover feed URLs from a website
  discoverFeeds: protectedProcedure
    .input(z.object({
      url: z.string().url(),
    }))
    .mutation(async ({ input }) => {
      const feedUrls = await discoverFeed(input.url)
      return { feedUrls }
    }),

  // Preview a feed before subscribing
  previewFeed: protectedProcedure
    .input(z.object({
      url: z.string().url(),
    }))
    .mutation(async ({ input }) => {
      const parsedFeed = await fetchAndParseFeed(input.url)
      return {
        title: parsedFeed.title,
        description: parsedFeed.description,
        url: parsedFeed.url,
        itemCount: parsedFeed.items.length,
        latestItems: parsedFeed.items.slice(0, 3).map(item => ({
          title: item.title,
          publishedAt: item.publishedAt,
        })),
      }
    }),

  // Refresh a feed (fetch new items)
  refreshFeed: protectedProcedure
    .input(z.object({
      feedId: z.string(),
    }))
    .mutation(async ({ ctx, input }) => {
      const feed = await ctx.db.feed.findUnique({
        where: { id: input.feedId },
      })

      if (!feed) {
        throw new Error('Feed not found')
      }

      if (feed.type !== 'RSS' || !feed.url) {
        throw new Error('Only RSS feeds can be refreshed')
      }

      const parsedFeed = await fetchAndParseFeed(feed.url)

      // Update feed title if it has changed
      await ctx.db.feed.update({
        where: { id: feed.id },
        data: {
          title: parsedFeed.title,
          lastFetchedAt: new Date(),
        },
      })

      // Add new items to database
      let newItemsCount = 0
      for (const item of parsedFeed.items) {
        try {
          // Check if item already exists (by URL or GUID)
          const existingItem = await ctx.db.feedItem.findFirst({
            where: {
              feedId: feed.id,
              OR: [
                { url: item.url },
                { guid: item.guid },
              ].filter(condition =>
                (condition.url && condition.url !== null) ||
                (condition.guid && condition.guid !== null)
              ),
            },
          })

          if (!existingItem) {
            await ctx.db.feedItem.create({
              data: {
                feedId: feed.id,
                title: item.title,
                content: item.content,
                author: item.author,
                publishedAt: item.publishedAt,
                url: item.url,
                guid: item.guid,
                videoId: item.videoId,
                embedUrl: item.embedUrl,
                thumbnail: item.thumbnail,
              },
            })
            newItemsCount++
          }
        } catch (error) {
          console.error('Error adding feed item:', error)
          // Continue processing other items
        }
      }

      return {
        success: true,
        newItemsCount,
        totalItems: parsedFeed.items.length,
      }
    }),

  // Validate a Nostr npub for feed subscription
  validateNostrFeed: protectedProcedure
    .input(z.object({
      npub: z.string().startsWith('npub1'),
    }))
    .mutation(async ({ input }) => {
      const nostrFetcher = getNostrFetcher()
      const result = await nostrFetcher.validateNostrFeed(input.npub)
      return result
    }),

  // Preview Nostr feed content
  previewNostrFeed: protectedProcedure
    .input(z.object({
      npub: z.string().startsWith('npub1'),
    }))
    .mutation(async ({ input }) => {
      const nostrFetcher = getNostrFetcher()

      // Get profile info
      const profile = await nostrFetcher.getProfile(input.npub)

      // Get recent posts
      const posts = await nostrFetcher.fetchLongFormPosts(input.npub, 5)

      return {
        npub: input.npub,
        profile: profile || {},
        postCount: posts.length,
        latestPosts: posts.map(post => ({
          title: post.title,
          publishedAt: post.publishedAt,
          tags: post.tags.slice(0, 3), // Show first 3 tags
        })),
      }
    }),

  // Refresh Nostr feed (fetch new long-form posts and videos)
  refreshNostrFeed: protectedProcedure
    .input(z.object({
      feedId: z.string(),
    }))
    .mutation(async ({ ctx, input }) => {
      const feed = await ctx.db.feed.findUnique({
        where: { id: input.feedId },
      })

      if (!feed) {
        throw new Error('Feed not found')
      }

      if ((feed.type !== 'NOSTR' && feed.type !== 'NOSTR_VIDEO') || !feed.npub) {
        throw new Error('Only Nostr feeds can be refreshed with this method')
      }

      const nostrFetcher = getNostrFetcher()

      // Get the last fetch time to only get new content
      const lastFetched = feed.lastFetchedAt

      // Decide what to fetch based on feed type
      let posts: any[] = []
      let videos: any[] = []
      if (feed.type === 'NOSTR') {
        posts = await nostrFetcher.fetchLongFormPosts(feed.npub, 50, lastFetched || undefined)
      } else if (feed.type === 'NOSTR_VIDEO') {
        videos = await nostrFetcher.fetchVideoEvents(feed.npub, 50, lastFetched || undefined)
      }

      // Update feed last fetched time
      await ctx.db.feed.update({
        where: { id: feed.id },
        data: {
          lastFetchedAt: new Date(),
        },
      })

      // Add new long-form posts to database
      let newItemsCount = 0
      for (const post of posts) {
        try {
          // Check if post already exists by Nostr event ID
          const existingItem = await ctx.db.feedItem.findFirst({
            where: {
              feedId: feed.id,
              guid: post.id, // Nostr event ID as GUID
            },
          })

          if (!existingItem) {
            await ctx.db.feedItem.create({
              data: {
                feedId: feed.id,
                title: post.title,
                content: post.content,
                author: post.author,
                publishedAt: post.publishedAt,
                url: post.url,
                guid: post.id, // Nostr event ID
              },
            })
            newItemsCount++
          }
        } catch (error) {
          console.error('Error adding Nostr feed item:', error)
          // Continue processing other items
        }
      }

      // Add new video events to database
      for (const video of videos) {
        try {
          // Check if video already exists by Nostr event ID
          const existingItem = await ctx.db.feedItem.findFirst({
            where: {
              feedId: feed.id,
              guid: video.id, // Nostr event ID as GUID
            },
          })

          if (!existingItem) {
            await ctx.db.feedItem.create({
              data: {
                feedId: feed.id,
                title: video.title,
                content: video.content,
                author: video.author,
                publishedAt: video.publishedAt,
                url: video.videoUrl,
                guid: video.id, // Nostr event ID
                embedUrl: video.embedUrl,
                thumbnail: video.thumbnail,
              },
            })
            newItemsCount++
          }
        } catch (error) {
          console.error('Error adding Nostr video item:', error)
          // Continue processing other items
        }
      }

      return {
        success: true,
        newItemsCount,
        totalItems: posts.length + videos.length,
      }
    }),

  // Search for Nostr profiles
  searchProfiles: protectedProcedure
    .input(z.object({
      query: z.string().min(1),
      limit: z.number().min(1).max(20).default(10),
      relays: z.array(z.string()).optional(),
    }))
    .mutation(async ({ input }) => {
      const nostrFetcher = input.relays ? new NostrFeedFetcher(input.relays) : getNostrFetcher()
      const profiles = await nostrFetcher.searchProfiles(input.query, input.limit)
      return { profiles }
    }),

  // Get popular Nostr users for discovery
  getPopularUsers: protectedProcedure
    .input(z.object({
      limit: z.number().min(1).max(50).default(20),
      relays: z.array(z.string()).optional(),
    }))
    .mutation(async ({ input }) => {
      const nostrFetcher = input.relays ? new NostrFeedFetcher(input.relays) : getNostrFetcher()
      const profiles = await nostrFetcher.getPopularUsers(input.limit)
      return { profiles }
    }),

  // Refresh all user's feeds at once
  refreshAllFeeds: protectedProcedure
    .input(z.object({ force: z.boolean().optional() }).optional())
    .mutation(async ({ ctx, input }) => {
      return await refreshAllUserFeedsInternal(ctx.db, ctx.nostrPubkey, input?.force ?? true)
    }),

  // ==================== CATEGORIES ====================

  // Get all user categories
  getCategories: protectedProcedure
    .query(async ({ ctx }) => {
      const categories = await ctx.db.category.findMany({
        where: { userPubkey: ctx.nostrPubkey },
        include: {
          _count: {
            select: { subscriptions: true },
          },
        },
        orderBy: { sortOrder: 'asc' },
      })

      return categories.map((cat: CategoryWithCount) => ({
        id: cat.id,
        name: cat.name,
        color: cat.color,
        icon: cat.icon,
        sortOrder: cat.sortOrder,
        feedCount: cat._count.subscriptions,
      }))
    }),

  // Create a new category
  createCategory: protectedProcedure
    .input(z.object({
      name: z.string().min(1).max(50),
      color: z.string().optional(),
      icon: z.string().optional(),
    }))
    .mutation(async ({ ctx, input }) => {
      // Get max sortOrder for this user
      const maxSortOrder = await ctx.db.category.aggregate({
        where: { userPubkey: ctx.nostrPubkey },
        _max: { sortOrder: true },
      })

      const category = await ctx.db.category.create({
        data: {
          userPubkey: ctx.nostrPubkey,
          name: input.name,
          color: input.color,
          icon: input.icon,
          sortOrder: (maxSortOrder._max.sortOrder ?? -1) + 1,
        },
      })

      return category
    }),

  // Update a category
  updateCategory: protectedProcedure
    .input(z.object({
      id: z.string(),
      name: z.string().min(1).max(50).optional(),
      color: z.string().optional(),
      icon: z.string().optional(),
    }))
    .mutation(async ({ ctx, input }) => {
      const category = await ctx.db.category.update({
        where: {
          id: input.id,
          userPubkey: ctx.nostrPubkey,
        },
        data: {
          ...(input.name && { name: input.name }),
          ...(input.color !== undefined && { color: input.color }),
          ...(input.icon !== undefined && { icon: input.icon }),
        },
      })

      return category
    }),

  // Delete a category
  deleteCategory: protectedProcedure
    .input(z.object({
      id: z.string(),
    }))
    .mutation(async ({ ctx, input }) => {
      // Delete the category - subscriptions will have categoryId set to null (due to onDelete: SetNull)
      await ctx.db.category.delete({
        where: {
          id: input.id,
          userPubkey: ctx.nostrPubkey,
        },
      })

      return { success: true }
    }),

  // Reorder categories
  reorderCategories: protectedProcedure
    .input(z.object({
      categoryIds: z.array(z.string()),
    }))
    .mutation(async ({ ctx, input }) => {
      // Update sortOrder for each category
      await Promise.all(
        input.categoryIds.map((id, index) =>
          ctx.db.category.update({
            where: {
              id,
              userPubkey: ctx.nostrPubkey,
            },
            data: { sortOrder: index },
          })
        )
      )

      return { success: true }
    }),

  // Update subscription category
  updateSubscriptionCategory: protectedProcedure
    .input(z.object({
      feedId: z.string(),
      categoryId: z.string().nullable().transform(v => v || null),
    }))
    .mutation(async ({ ctx, input }) => {
      // Ensure the target category, if any, belongs to the requesting user
      if (input.categoryId) {
        await assertCategoryOwnership(ctx.db, ctx.nostrPubkey, input.categoryId)
      }

      await ctx.db.subscription.update({
        where: {
          userPubkey_feedId: {
            userPubkey: ctx.nostrPubkey,
            feedId: input.feedId,
          },
        },
        data: {
          categoryId: input.categoryId,
        },
      })

      return { success: true }
    }),

  // Get categories with unread counts
  getCategoriesWithUnread: protectedProcedure
    .query(async ({ ctx }) => {
      const categories = await ctx.db.category.findMany({
        where: { userPubkey: ctx.nostrPubkey },
        include: {
          subscriptions: {
            include: {
              feed: {
                include: {
                  items: {
                    where: {
                      readItems: {
                        none: {
                          userPubkey: ctx.nostrPubkey,
                        },
                      },
                    },
                  },
                },
              },
            },
          },
        },
        orderBy: { sortOrder: 'asc' },
      })

      // Get uncategorized subscriptions
      const uncategorizedSubs = await ctx.db.subscription.findMany({
        where: {
          userPubkey: ctx.nostrPubkey,
          categoryId: null,
          deletedAt: null,
        },
        include: {
          feed: {
            include: {
              items: {
                where: {
                  readItems: {
                    none: {
                      userPubkey: ctx.nostrPubkey,
                    },
                  },
                },
              },
            },
          },
        },
      })

      const result = categories.map((cat: CategoryWithSubscriptions) => ({
        id: cat.id,
        name: cat.name,
        color: cat.color,
        icon: cat.icon,
        feedCount: cat.subscriptions.length,
        unreadCount: cat.subscriptions.reduce((sum: number, sub: CategoryWithSubscriptions['subscriptions'][number]) => sum + sub.feed.items.length, 0),
      }))

      // Add uncategorized as a special category if there are any uncategorized subscriptions
      if (uncategorizedSubs.length > 0) {
        result.push({
          id: 'uncategorized',
          name: 'Uncategorized',
          color: '#64748b', // slate gray
          icon: '📦',
          feedCount: uncategorizedSubs.length,
          unreadCount: uncategorizedSubs.reduce((sum, sub) => sum + sub.feed.items.length, 0),
        })
      }

      return result
    }),

  // ==================== USER PREFERENCES ====================

  // Get user preference (organization mode)
  getUserPreference: protectedProcedure
    .query(async ({ ctx }) => {
      const preference = await ctx.db.userPreference.findUnique({
        where: { userPubkey: ctx.nostrPubkey },
      })

      return {
        organizationMode: (preference?.organizationMode ?? 'tags') as 'tags' | 'categories',
      }
    }),

  // Update user preference
  updateUserPreference: protectedProcedure
    .input(z.object({
      organizationMode: z.enum(['tags', 'categories']),
    }))
    .mutation(async ({ ctx, input }) => {
      const preference = await ctx.db.userPreference.upsert({
        where: { userPubkey: ctx.nostrPubkey },
        create: {
          userPubkey: ctx.nostrPubkey,
          organizationMode: input.organizationMode,
        },
        update: {
          organizationMode: input.organizationMode,
        },
      })

      return preference
    }),

  // ==================== SYNC OPERATIONS ====================

  // Get all subscriptions including soft-deleted for sync
  getSubscriptionsForSync: protectedProcedure
    .query(async ({ ctx }) => {
      const subscriptions = await ctx.db.subscription.findMany({
        where: {
          userPubkey: ctx.nostrPubkey,
        },
        include: {
          feed: true,
        },
      })

      return subscriptions.map(sub => ({
        type: sub.feed.type,
        url: sub.feed.url || sub.feed.npub || '',
        tags: sub.tags,
        deletedAt: sub.deletedAt,
      }))
    }),

  // Hard delete old soft-deleted subscriptions (cleanup)
  cleanupDeletedSubscriptions: protectedProcedure
    .input(z.object({
      olderThanDays: z.number().default(90),
    }))
    .mutation(async ({ ctx, input }) => {
      const cutoffDate = new Date()
      cutoffDate.setDate(cutoffDate.getDate() - input.olderThanDays)

      const result = await ctx.db.subscription.deleteMany({
        where: {
          userPubkey: ctx.nostrPubkey,
          deletedAt: {
            not: null,
            lt: cutoffDate,
          },
        },
      })

      return { deletedCount: result.count }
    }),

  // Mark read items as synced
  markReadItemsSynced: protectedProcedure
    .input(z.object({
      itemIds: z.array(z.string()),
    }))
    .mutation(async ({ ctx, input }) => {
      await ctx.db.readItem.updateMany({
        where: {
          userPubkey: ctx.nostrPubkey,
          itemId: {
            in: input.itemIds,
          },
        },
        data: {
          syncedAt: new Date(),
        },
      })

      return { success: true }
    }),

  // Get unsynced read items
  getUnsyncedReadItems: protectedProcedure
    .query(async ({ ctx }) => {
      const readItems = await ctx.db.readItem.findMany({
        where: {
          userPubkey: ctx.nostrPubkey,
          syncedAt: null,
        },
        include: {
          feedItem: {
            select: {
              guid: true,
            },
          },
        },
      })

      return readItems
        .filter(item => item.feedItem.guid)
        .map(item => ({
          itemId: item.itemId,
          guid: item.feedItem.guid!,
          readAt: item.readAt,
        }))
    }),
})