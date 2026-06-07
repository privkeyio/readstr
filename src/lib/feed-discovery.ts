import * as cheerio from 'cheerio'
import { safeFetch } from './safe-fetch'

export interface FeedDiscoveryResult {
  found: boolean
  feedUrl?: string
  title?: string
  type?: 'rss' | 'atom' | 'json'
  error?: string
}

/**
 * Discovers RSS/Atom feeds from a given URL
 * 1. First checks if the URL itself is a valid feed
 * 2. If not, tries to find feed links in the HTML
 * 3. Common feed locations like /feed, /rss, /atom.xml
 */
export async function discoverFeed(url: string): Promise<FeedDiscoveryResult> {
  try {
    // Normalize URL
    const normalizedUrl = url.trim()
    if (!normalizedUrl.startsWith('http://') && !normalizedUrl.startsWith('https://')) {
      return {
        found: false,
        error: 'URL must start with http:// or https://'
      }
    }

    // Step 1: Try the URL directly as a feed
    const directCheck = await checkIfFeed(normalizedUrl)
    if (directCheck.found) {
      return directCheck
    }

    // Step 2: Fetch the page and look for feed links
    const htmlCheck = await findFeedInHTML(normalizedUrl)
    if (htmlCheck.found) {
      return htmlCheck
    }

    // Step 3: Try common feed locations
    const commonCheck = await tryCommonFeedLocations(normalizedUrl)
    if (commonCheck.found) {
      return commonCheck
    }

    return {
      found: false,
      error: 'No RSS or Atom feed found at this URL or domain'
    }
  } catch (error) {
    console.error('Feed discovery error:', error)
    return {
      found: false,
      error: error instanceof Error ? error.message : 'Failed to discover feed'
    }
  }
}

/**
 * Check if a URL directly points to a valid feed
 */
async function checkIfFeed(url: string): Promise<FeedDiscoveryResult> {
  try {
    const response = await safeFetch(url, {
      headers: {
        'User-Agent': 'Mozilla/5.0 (compatible; readstr/1.0; +https://readstr.privkey.io)'
      },
      signal: AbortSignal.timeout(10000), // 10 second timeout
    })

    if (!response.ok) {
      return { found: false }
    }

    const contentType = response.headers.get('content-type') || ''
    const text = await response.text()

    // Check for RSS/Atom/JSON feed markers
    if (
      contentType.includes('xml') ||
      contentType.includes('rss') ||
      contentType.includes('atom') ||
      text.includes('<rss') ||
      text.includes('<feed') ||
      text.includes('xmlns="http://www.w3.org/2005/Atom"')
    ) {
      // Parse title from feed
      let title: string | undefined
      let type: 'rss' | 'atom' | 'json' = 'rss'

      if (text.includes('<rss')) {
        type = 'rss'
        const titleMatch = text.match(/<title>(?:<!\[CDATA\[)?\s*([^<]+?)\s*(?:\]\]>)?<\/title>/)
        if (titleMatch) title = titleMatch[1].trim()
      } else if (text.includes('<feed') || text.includes('xmlns="http://www.w3.org/2005/Atom"')) {
        type = 'atom'
        const titleMatch = text.match(/<title[^>]*>(?:<!\[CDATA\[)?\s*([^<]+?)\s*(?:\]\]>)?<\/title>/)
        if (titleMatch) title = titleMatch[1].trim()
      }

      return {
        found: true,
        feedUrl: url,
        title,
        type
      }
    }

    // Check for JSON Feed
    if (contentType.includes('json')) {
      try {
        const json = JSON.parse(text)
        if (json.version && json.version.startsWith('https://jsonfeed.org/version/')) {
          return {
            found: true,
            feedUrl: url,
            title: json.title,
            type: 'json'
          }
        }
      } catch (e) {
        // Not valid JSON or not a JSON feed
      }
    }

    return { found: false }
  } catch (error) {
    return { found: false }
  }
}

/**
 * Parse HTML to find feed links
 */
async function findFeedInHTML(url: string): Promise<FeedDiscoveryResult> {
  try {
    const response = await safeFetch(url, {
      headers: {
        'User-Agent': 'Mozilla/5.0 (compatible; readstr/1.0)'
      },
      signal: AbortSignal.timeout(10000),
    })

    if (!response.ok) {
      return { found: false }
    }

    const html = await response.text()
    const $ = cheerio.load(html)

    // Look for feed link tags
    const feedLinks = $('link[type="application/rss+xml"], link[type="application/atom+xml"], link[type="application/json"]')
    
    if (feedLinks.length > 0) {
      const feedLink = feedLinks.first()
      let feedUrl = feedLink.attr('href')
      
      if (feedUrl) {
        // Make absolute URL
        feedUrl = new URL(feedUrl, url).href
        
        // Verify it's actually a feed
        const verification = await checkIfFeed(feedUrl)
        if (verification.found) {
          return {
            found: true,
            feedUrl: verification.feedUrl,
            title: feedLink.attr('title') || verification.title,
            type: verification.type
          }
        }
      }
    }

    return { found: false }
  } catch (error) {
    return { found: false }
  }
}

/**
 * Try common feed locations
 */
async function tryCommonFeedLocations(url: string): Promise<FeedDiscoveryResult> {
  try {
    const baseUrl = new URL(url)
    const commonPaths = [
      '/feed',
      '/feed/',
      '/rss',
      '/rss.xml',
      '/atom.xml',
      '/feed.xml',
      '/index.xml',
      '/blog/feed',
      '/blog/rss',
      '/?feed=rss2',
      '/feeds/posts/default' // Blogger
    ]

    for (const path of commonPaths) {
      const testUrl = `${baseUrl.origin}${path}`
      const result = await checkIfFeed(testUrl)
      if (result.found) {
        return result
      }
    }

    return { found: false }
  } catch (error) {
    return { found: false }
  }
}
