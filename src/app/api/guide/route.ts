import { NextRequest, NextResponse } from 'next/server'
import { db } from '@/server/db'
import type { GuideFeed } from '@prisma/client'

// CORS headers for native apps
const corsHeaders = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type, Authorization, X-Nostr-Pubkey',
}

// Handle CORS preflight
export async function OPTIONS() {
  return new NextResponse(null, { status: 204, headers: corsHeaders })
}

/**
 * GET /api/guide
 * 
 * Get list of guide feeds with optional filtering
 * 
 * Query params:
 * - tags: comma-separated list of tags to filter by
 * - orderBy: 'newest' | 'popular' | 'recent_posts' (default: 'popular')
 * - limit: number 1-100 (default: 50)
 * - format: 'full' | 'minimal' (default: 'full')
 * 
 * Example: /api/guide?tags=bitcoin,nostr&orderBy=popular&limit=20
 */
export async function GET(request: NextRequest) {
  try {
    const { searchParams } = new URL(request.url)
    
    // Parse query params
    const tagsParam = searchParams.get('tags')
    const tags = tagsParam ? tagsParam.split(',').map(t => t.trim()).filter(Boolean) : []
    const orderByParam = searchParams.get('orderBy') || 'popular'
    const limitParam = parseInt(searchParams.get('limit') || '50', 10)
    const format = searchParams.get('format') || 'full'
    
    // Validate params
    const limit = Math.min(Math.max(1, limitParam), 100)
    const validOrderBy = ['newest', 'popular', 'recent_posts'].includes(orderByParam) 
      ? orderByParam 
      : 'popular'
    
    // Build where clause
    const whereClause: any = {
      isActive: true,
    }
    
    if (tags.length > 0) {
      whereClause.tags = {
        hasSome: tags,
      }
    }
    
    // Determine sorting
    let orderBy: any = { subscriberCount: 'desc' }
    if (validOrderBy === 'newest') {
      orderBy = { createdAt: 'desc' }
    } else if (validOrderBy === 'recent_posts') {
      orderBy = { lastPublishedAt: 'desc' }
    }
    
    // Fetch feeds
    const feeds = await db.guideFeed.findMany({
      where: whereClause,
      orderBy,
      take: limit,
    })
    
    // Format response based on format param
    const formattedFeeds = feeds.map((feed: GuideFeed) => {
      const base = {
        npub: feed.npub,
        displayName: feed.displayName,
        picture: feed.picture,
        tags: feed.tags,
        subscriberCount: feed.subscriberCount,
        postCount: feed.postCount,
        // Deep link URLs for one-click subscription
        subscribeUrl: `https://readstr.privkey.io/subscribe?npub=${encodeURIComponent(feed.npub)}`,
        webUrl: `https://readstr.privkey.io/guide#${feed.npub}`,
      }
      
      if (format === 'minimal') {
        return base
      }
      
      // Full format includes more data
      return {
        ...base,
        about: feed.about,
        lastPublishedAt: feed.lastPublishedAt?.toISOString() || null,
        createdAt: feed.createdAt.toISOString(),
        // RSS feed URL for this author
        rssUrl: `https://readstr.privkey.io/api/nostr-rss?npub=${encodeURIComponent(feed.npub)}`,
      }
    })
    
    // Also fetch available tags for discovery
    const allFeeds = await db.guideFeed.findMany({
      where: { isActive: true },
      select: { tags: true },
    })
    
    const tagCounts = new Map<string, number>()
    for (const feed of allFeeds) {
      for (const tag of feed.tags) {
        tagCounts.set(tag, (tagCounts.get(tag) || 0) + 1)
      }
    }
    
    const availableTags = Array.from(tagCounts.entries())
      .map(([tag, count]) => ({ tag, count }))
      .sort((a, b) => b.count - a.count)
    
    return NextResponse.json({
      success: true,
      data: {
        feeds: formattedFeeds,
        totalCount: formattedFeeds.length,
        availableTags,
        filters: {
          tags,
          orderBy: validOrderBy,
          limit,
        },
      },
      meta: {
        version: '1.0',
        docsUrl: 'https://readstr.privkey.io/api/guide/docs',
      },
    }, { headers: corsHeaders })
    
  } catch (error) {
    console.error('Guide API error:', error)
    return NextResponse.json({
      success: false,
      error: 'Failed to fetch guide feeds',
    }, { status: 500, headers: corsHeaders })
  }
}
