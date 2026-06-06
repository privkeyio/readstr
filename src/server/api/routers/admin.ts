import { z } from "zod";
import { TRPCError } from "@trpc/server";
import { createTRPCRouter, protectedProcedure } from "@/server/api/trpc";
import { nip19 } from 'nostr-tools';

const ADMIN_NPUB = 'npub13hyx3qsqk3r7ctjqrr49uskut4yqjsxt8uvu4rekr55p08wyhf0qq90nt7';

const decodedAdmin = nip19.decode(ADMIN_NPUB);
if (decodedAdmin.type !== 'npub' || !/^[0-9a-f]{64}$/.test(decodedAdmin.data)) {
  throw new Error('admin: ADMIN_NPUB did not decode to a valid hex pubkey');
}
const ADMIN_HEX = decodedAdmin.data;

const adminProcedure = protectedProcedure.use(({ ctx, next }) => {
  if (ctx.nostrPubkey !== ADMIN_HEX) {
    throw new TRPCError({ code: 'FORBIDDEN' });
  }
  return next();
});

export const adminRouter = createTRPCRouter({
  getStats: adminProcedure
    .query(async ({ ctx }) => {
      const db = ctx.db;
      const now = new Date();
      const thirtyDaysAgo = new Date(now.getTime() - 30 * 24 * 60 * 60 * 1000);
      const sevenDaysAgo = new Date(now.getTime() - 7 * 24 * 60 * 60 * 1000);

      // Total unique users (based on subscriptions)
      const totalUsers = await db.subscription.findMany({
        distinct: ['userPubkey'],
        select: { userPubkey: true }
      });

      // Active users in last 30 days (users who created subscriptions or read items)
      const activeUsers30d = await db.subscription.groupBy({
        by: ['userPubkey'],
        where: {
          createdAt: {
            gte: thirtyDaysAgo
          }
        }
      });

      // Active users in last 7 days
      const activeUsers7d = await db.subscription.groupBy({
        by: ['userPubkey'],
        where: {
          createdAt: {
            gte: sevenDaysAgo
          }
        }
      });

      // Total feeds
      const totalFeeds = await db.feed.count();

      // Active feeds
      const activeFeeds = await db.feed.count({
        where: {
          isActive: true
        }
      });

      // Total feed items
      const totalFeedItems = await db.feedItem.count();

      // Total subscriptions
      const totalSubscriptions = await db.subscription.count();

      // Read items in last 30 days
      const readItems30d = await db.readItem.count({
        where: {
          readAt: {
            gte: thirtyDaysAgo
          }
        }
      });

      // Guide feeds
      const totalGuideFeeds = await db.guideFeed.count();

      // User subscriptions (paid users)
      const paidSubscriptions = await db.userSubscription.count({
        where: {
          status: 'ACTIVE'
        }
      });

      const trialSubscriptions = await db.userSubscription.count({
        where: {
          status: 'TRIAL'
        }
      });

      return {
        users: {
          total: totalUsers.length,
          active30d: activeUsers30d.length,
          active7d: activeUsers7d.length,
        },
        feeds: {
          total: totalFeeds,
          active: activeFeeds,
          guideFeeds: totalGuideFeeds,
        },
        items: {
          total: totalFeedItems,
          read30d: readItems30d,
        },
        subscriptions: {
          total: totalSubscriptions,
          paid: paidSubscriptions,
          trial: trialSubscriptions,
        },
      };
    }),

  getUserGrowth: adminProcedure
    .input(z.object({
      days: z.number().optional().default(30),
    }))
    .query(async ({ input, ctx }) => {
      const db = ctx.db;
      const now = new Date();
      const startDate = new Date(now.getTime() - input.days * 24 * 60 * 60 * 1000);

      // Get all subscriptions grouped by day
      const subscriptions = await db.subscription.findMany({
        where: {
          createdAt: {
            gte: startDate
          }
        },
        select: {
          userPubkey: true,
          createdAt: true,
        },
        orderBy: {
          createdAt: 'asc'
        }
      });

      // Group by date and count unique users per day
      const usersByDay = new Map<string, Set<string>>();
      
      subscriptions.forEach(sub => {
        const dateKey = sub.createdAt.toISOString().split('T')[0];
        if (!usersByDay.has(dateKey!)) {
          usersByDay.set(dateKey!, new Set());
        }
        usersByDay.get(dateKey!)!.add(sub.userPubkey);
      });

      // Convert to cumulative count
      const allUsers = new Set<string>();
      const growthData: { date: string; count: number }[] = [];

      // Fill in all days even if no data
      for (let i = input.days - 1; i >= 0; i--) {
        const date = new Date(now.getTime() - i * 24 * 60 * 60 * 1000);
        const dateKey = date.toISOString().split('T')[0]!;
        
        const usersOnDay = usersByDay.get(dateKey);
        if (usersOnDay) {
          usersOnDay.forEach(user => allUsers.add(user));
        }
        
        growthData.push({
          date: dateKey,
          count: allUsers.size,
        });
      }

      return growthData;
    }),

  getActivityHistory: adminProcedure
    .input(z.object({
      days: z.number().optional().default(30),
    }))
    .query(async ({ input, ctx }) => {
      const db = ctx.db;
      const now = new Date();
      const startDate = new Date(now.getTime() - input.days * 24 * 60 * 60 * 1000);

      // Get read items per day
      const readItems = await db.readItem.findMany({
        where: {
          readAt: {
            gte: startDate
          }
        },
        select: {
          readAt: true,
        },
        orderBy: {
          readAt: 'asc'
        }
      });

      // Group by date
      const itemsByDay = new Map<string, number>();
      
      readItems.forEach(item => {
        const dateKey = item.readAt.toISOString().split('T')[0];
        itemsByDay.set(dateKey!, (itemsByDay.get(dateKey!) || 0) + 1);
      });

      // Fill in all days
      const activityData: { date: string; reads: number }[] = [];
      for (let i = input.days - 1; i >= 0; i--) {
        const date = new Date(now.getTime() - i * 24 * 60 * 60 * 1000);
        const dateKey = date.toISOString().split('T')[0]!;
        
        activityData.push({
          date: dateKey,
          reads: itemsByDay.get(dateKey) || 0,
        });
      }

      return activityData;
    }),
});
