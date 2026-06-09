import { NextRequest, NextResponse } from 'next/server';
import { db } from '@/server/db';
import { getNostrFetcher } from '@/lib/nostr-fetcher';

// This endpoint refreshes all guide feed metadata
// Can be called by a cron job (e.g., Vercel Cron, external service)
export async function GET(request: NextRequest) {
  // Authenticate via secret token. Fail closed: refuse to run if no secret is
  // configured so an unconfigured deploy can't be triggered anonymously.
  const authHeader = request.headers.get('authorization');
  const cronSecret = process.env.CRON_SECRET;

  if (!cronSecret) {
    return new NextResponse('Server misconfigured: CRON_SECRET not set', { status: 500 });
  }

  if (authHeader !== `Bearer ${cronSecret}`) {
    return new NextResponse('Unauthorized', { status: 401 });
  }

  try {
    // Get all active guide feeds
    const guideFeeds = await db.guideFeed.findMany({
      where: { isActive: true },
      select: { id: true, npub: true },
    });

    const nostrFetcher = getNostrFetcher();
    const results = {
      total: guideFeeds.length,
      updated: 0,
      failed: 0,
      errors: [] as string[],
    };

    // Refresh each feed
    for (const feed of guideFeeds) {
      try {
        const posts = await nostrFetcher.fetchLongFormPosts(feed.npub, 50);

        const lastPublishedAt = posts.length > 0 
          ? posts.reduce((latest, post) => 
              post.publishedAt > latest ? post.publishedAt : latest, 
              posts[0].publishedAt
            )
          : null;

        await db.guideFeed.update({
          where: { id: feed.id },
          data: {
            postCount: posts.length,
            lastPublishedAt,
            updatedAt: new Date(),
          },
        });

        results.updated++;
      } catch (error) {
        results.failed++;
        results.errors.push(`Failed to update ${feed.npub}: ${error instanceof Error ? error.message : 'Unknown error'}`);
        console.error(`Failed to refresh feed ${feed.npub}:`, error);
      }

      // Add a small delay to avoid overwhelming relays
      await new Promise(resolve => setTimeout(resolve, 500));
    }

    nostrFetcher.close();

    return NextResponse.json({
      success: true,
      timestamp: new Date().toISOString(),
      results,
    });
  } catch (error) {
    console.error('Failed to refresh guide feeds:', error);
    return NextResponse.json(
      { 
        success: false, 
        error: error instanceof Error ? error.message : 'Unknown error' 
      },
      { status: 500 }
    );
  }
}
