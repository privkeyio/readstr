import { safeFetch } from './safe-fetch'
import {
  detectVideoPlatform,
  getYouTubeFeedUrl,
  getRumbleFeedUrl,
} from './video-parser'

/**
 * Discover channel ID from YouTube URL by scraping the page
 */
export async function discoverYouTubeChannelId(channelUrl: string): Promise<string | null> {
  try {
    const response = await safeFetch(channelUrl, {
      headers: {
        'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36',
      },
      signal: AbortSignal.timeout(5000),
    })

    if (!response.ok) return null

    const html = await response.text()

    // Look for channel ID in meta tags or page source
    const channelIdMatch = html.match(/"channelId":"([^"]+)"/) ||
                          html.match(/channel_id=([^"&]+)/) ||
                          html.match(/<link[^>]+href="https:\/\/www\.youtube\.com\/feeds\/videos\.xml\?channel_id=([^"]+)"/)

    if (channelIdMatch) {
      const channelId = channelIdMatch[1].trim()
      if (/^UC[a-zA-Z0-9_-]{22}$/.test(channelId)) {
        return channelId
      }
    }

    return null
  } catch {
    return null
  }
}

/**
 * Get RSS feed URL for a video channel
 */
export async function getVideoChannelFeedUrl(channelUrl: string): Promise<string | null> {
  const platform = detectVideoPlatform(channelUrl)

  if (platform === 'youtube') {
    // Try direct extraction first
    let feedUrl = getYouTubeFeedUrl(channelUrl)
    if (feedUrl) return feedUrl

    // Try to discover channel ID from the page
    try {
      const channelId = await discoverYouTubeChannelId(channelUrl)
      if (channelId) {
        return `https://www.youtube.com/feeds/videos.xml?channel_id=${channelId}`
      }
    } catch (error) {
      console.error('Failed to discover YouTube channel ID:', error)
    }

    // Last resort: try using OpenRSS for YouTube too
    console.log('Using OpenRSS fallback for YouTube channel')
    return `https://openrss.org/rss?url=${encodeURIComponent(channelUrl)}`
  }

  if (platform === 'rumble') {
    return getRumbleFeedUrl(channelUrl)
  }

  return null
}
