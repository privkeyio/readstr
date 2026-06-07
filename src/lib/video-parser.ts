// Video platform detection and parsing utilities

export type VideoPlatform = 'youtube' | 'rumble' | 'unknown'

export interface VideoMetadata {
  platform: VideoPlatform
  videoId: string
  embedUrl: string
  thumbnail?: string
}

/**
 * Detect video platform from URL
 */
export function detectVideoPlatform(url: string): VideoPlatform {
  try {
    const urlObj = new URL(url)
    const hostname = urlObj.hostname.toLowerCase()
    
    if (hostname.includes('youtube.com') || hostname.includes('youtu.be')) {
      return 'youtube'
    }
    if (hostname.includes('rumble.com')) {
      return 'rumble'
    }
    
    return 'unknown'
  } catch {
    return 'unknown'
  }
}

/**
 * Extract YouTube video ID from various URL formats
 * Supports: youtube.com/watch?v=ID, youtu.be/ID, youtube.com/embed/ID, youtube.com/shorts/ID, etc.
 */
export function extractYouTubeVideoId(url: string): string | null {
  try {
    const urlObj = new URL(url)
    const hostname = urlObj.hostname.toLowerCase()
    
    // youtu.be/VIDEO_ID
    if (hostname.includes('youtu.be')) {
      return urlObj.pathname.slice(1).split('/')[0] || null
    }
    
    // youtube.com/watch?v=VIDEO_ID
    if (urlObj.searchParams.has('v')) {
      return urlObj.searchParams.get('v')
    }
    
    // youtube.com/shorts/VIDEO_ID (YouTube Shorts)
    const shortsMatch = urlObj.pathname.match(/\/shorts\/([^/?]+)/)
    if (shortsMatch) {
      return shortsMatch[1] || null
    }
    
    // youtube.com/embed/VIDEO_ID or youtube.com/v/VIDEO_ID
    const pathMatch = urlObj.pathname.match(/\/(embed|v)\/([^/?]+)/)
    if (pathMatch) {
      return pathMatch[2] || null
    }
    
    return null
  } catch {
    return null
  }
}

/**
 * Extract Rumble video ID from URL
 * Supports: rumble.com/v123abc-title.html, rumble.com/embed/v123abc
 */
export function extractRumbleVideoId(url: string): string | null {
  try {
    const urlObj = new URL(url)
    
    // rumble.com/embed/VIDEO_ID
    const embedMatch = urlObj.pathname.match(/\/embed\/([^/?]+)/)
    if (embedMatch) {
      return embedMatch[1] || null
    }
    
    // rumble.com/vVIDEO_ID-title.html
    const videoMatch = urlObj.pathname.match(/\/(v[a-z0-9]+)/i)
    if (videoMatch) {
      return videoMatch[1] || null
    }
    
    return null
  } catch {
    return null
  }
}

/**
 * Get video metadata from URL
 */
export function getVideoMetadata(url: string): VideoMetadata | null {
  const platform = detectVideoPlatform(url)
  
  if (platform === 'youtube') {
    const videoId = extractYouTubeVideoId(url)
    if (!videoId) return null
    
    return {
      platform: 'youtube',
      videoId,
      embedUrl: `https://www.youtube.com/embed/${videoId}`,
      thumbnail: `https://img.youtube.com/vi/${videoId}/maxresdefault.jpg`,
    }
  }
  
  if (platform === 'rumble') {
    const videoId = extractRumbleVideoId(url)
    if (!videoId) return null
    
    return {
      platform: 'rumble',
      videoId,
      embedUrl: `https://rumble.com/embed/${videoId}/`,
      thumbnail: undefined, // Rumble doesn't have a standard thumbnail URL pattern
    }
  }
  
  return null
}

/**
 * Convert YouTube channel URL to RSS feed URL
 * Supports: /channel/CHANNEL_ID, /c/CHANNEL_NAME, /user/USERNAME, /@HANDLE
 */
export function getYouTubeFeedUrl(channelUrl: string): string | null {
  try {
    const urlObj = new URL(channelUrl)
    
    // Extract channel ID from /channel/CHANNEL_ID
    const channelMatch = urlObj.pathname.match(/\/channel\/([^/?]+)/)
    if (channelMatch) {
      return `https://www.youtube.com/feeds/videos.xml?channel_id=${channelMatch[1]}`
    }
    
    // For /c/, /user/, or /@, we need the channel ID which requires scraping
    // For now, return null and handle in the discovery function
    return null
  } catch {
    return null
  }
}

/**
 * Convert Rumble channel URL to RSS feed URL using OpenRSS
 * OpenRSS provides better support for Rumble with video embeds
 */
export function getRumbleFeedUrl(channelUrl: string): string | null {
  try {
    const urlObj = new URL(channelUrl)
    
    // Validate it's a Rumble URL
    if (!urlObj.hostname.includes('rumble.com')) {
      return null
    }
    
    // Use OpenRSS to generate the feed
    // OpenRSS supports video embeds, livestream notifications, and removes tracking
    const openRssUrl = `https://openrss.org/rss?url=${encodeURIComponent(channelUrl)}`
    return openRssUrl
  } catch {
    return null
  }
}
