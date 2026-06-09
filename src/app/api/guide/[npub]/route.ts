import { NextRequest, NextResponse } from 'next/server'
import { db } from '@/server/db'
import { getNostrFetcher } from '@/lib/nostr-fetcher'
import { rateLimit, clientIpFromRequest } from '@/server/rate-limit'

// CORS headers for native apps
const corsHeaders = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type, Authorization, X-Nostr-Pubkey',
}

// Handle CORS preflight
export async function OPTIONS() {
  return new NextResponse(null, { status: 204, headers: corsHeaders })
}

/**
 * GET /api/guide/[npub]
 * 
 * Get a single guide feed by npub with recent posts
 * 
 * Query params:
 * - includePosts: boolean (default: false) - include recent long-form posts
 * - postLimit: number 1-20 (default: 5) - number of posts to include
 * 
 * Example: /api/guide/npub1abc...?includePosts=true&postLimit=10
 */
export async function GET(
  request: NextRequest,
  { params }: { params: Promise<{ npub: string }> }
) {
  try {
    const { npub } = await params
    const { searchParams } = new URL(request.url)
    
    // Validate npub format
    if (!npub.startsWith('npub1')) {
      return NextResponse.json({
        success: false,
        error: 'Invalid npub format. Must start with npub1',
      }, { status: 400, headers: corsHeaders })
    }
    
    // Parse query params
    const includePosts = searchParams.get('includePosts') === 'true'
    const postLimitParam = parseInt(searchParams.get('postLimit') || '5', 10)
    const postLimit = Math.min(Math.max(1, postLimitParam), 20)
    
    // Fetch guide feed from database
    const guideFeed = await db.guideFeed.findUnique({
      where: { npub },
    })
    
    if (!guideFeed) {
      return NextResponse.json({
        success: false,
        error: 'Feed not found in guide',
      }, { status: 404, headers: corsHeaders })
    }
    
    // Build response
    const response: any = {
      npub: guideFeed.npub,
      displayName: guideFeed.displayName,
      about: guideFeed.about,
      picture: guideFeed.picture,
      tags: guideFeed.tags,
      subscriberCount: guideFeed.subscriberCount,
      postCount: guideFeed.postCount,
      lastPublishedAt: guideFeed.lastPublishedAt?.toISOString() || null,
      createdAt: guideFeed.createdAt.toISOString(),
      // Action URLs
      subscribeUrl: `https://readstr.privkey.io/subscribe?npub=${encodeURIComponent(npub)}`,
      webUrl: `https://readstr.privkey.io/guide#${npub}`,
      rssUrl: `https://readstr.privkey.io/api/nostr-rss?npub=${encodeURIComponent(npub)}`,
    }
    
    // Optionally fetch recent posts from Nostr
    if (includePosts) {
      const ip = clientIpFromRequest(request)
      const limit = rateLimit('guide.includePosts', ip, 20, 60 * 1000)
      if (!limit.allowed) {
        return NextResponse.json({
          success: false,
          error: 'Too many requests',
        }, {
          status: 429,
          headers: { ...corsHeaders, 'Retry-After': String(limit.retryAfterSeconds) },
        })
      }

      try {
        const nostrFetcher = getNostrFetcher()
        const posts = await nostrFetcher.fetchLongFormPosts(npub, postLimit)
        
        response.recentPosts = posts.map(post => ({
          id: post.id,
          title: post.title,
          summary: post.content.length > 300 
            ? post.content.substring(0, 300) + '...' 
            : post.content,
          author: post.author,
          publishedAt: post.publishedAt.toISOString(),
          tags: post.tags,
          url: post.url,
        }))
      } catch (error) {
        console.error('Failed to fetch posts:', error)
        response.recentPosts = []
        response.postsError = 'Failed to fetch recent posts from Nostr relays'
      }
    }
    
    return NextResponse.json({
      success: true,
      data: response,
      meta: {
        version: '1.0',
      },
    }, { headers: corsHeaders })
    
  } catch (error) {
    console.error('Guide feed API error:', error)
    return NextResponse.json({
      success: false,
      error: 'Failed to fetch guide feed',
    }, { status: 500, headers: corsHeaders })
  }
}
