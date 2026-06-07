import { parseString } from 'xml2js'
import { promisify } from 'util'
import { getVideoMetadata } from './video-parser'
import { safeFetch } from './safe-fetch'

const parseXML = promisify(parseString)

export interface ParsedFeedItem {
  title: string
  content: string
  author?: string
  publishedAt: Date
  url?: string
  guid?: string
  videoId?: string
  embedUrl?: string
  thumbnail?: string
}

export interface ParsedFeed {
  title: string
  description?: string
  url?: string
  items: ParsedFeedItem[]
}

// Helper to safely extract text from XML elements
function extractText(element: any): string {
  if (!element) return ''
  if (typeof element === 'string') return element
  if (Array.isArray(element)) return extractText(element[0])
  if (element._) return element._
  return String(element)
}

// Helper to safely extract date
function extractDate(dateStr: string | undefined): Date {
  if (!dateStr) return new Date()
  
  const date = new Date(dateStr)
  if (isNaN(date.getTime())) {
    // Try to parse common RSS date formats
    const normalizedDate = dateStr.replace(/^\s+|\s+$/g, '')
    return new Date(normalizedDate) || new Date()
  }
  return date
}

// Parse RSS 2.0 format
function parseRSS(data: any): ParsedFeed {
  const channel = data.rss?.channel?.[0] || data.rss?.channel
  
  if (!channel) {
    throw new Error('Invalid RSS format: no channel found')
  }

  const items: ParsedFeedItem[] = (channel.item || []).map((item: any) => {
    const title = extractText(item.title)
    const description = extractText(item.description)
    const content = extractText(item['content:encoded']) || description
    const link = extractText(item.link)
    const author = extractText(item.author) || extractText(item['dc:creator'])
    const pubDate = extractText(item.pubDate)
    const guid = extractText(item.guid)

    // Extract video metadata if this is a video feed (YouTube/Rumble)
    const videoMetadata = link ? getVideoMetadata(link) : null
    
    // Extract thumbnail from media:thumbnail or media:content
    let thumbnail = videoMetadata?.thumbnail
    if (!thumbnail && item['media:thumbnail']?.[0]?.$?.url) {
      thumbnail = item['media:thumbnail'][0].$.url
    }
    if (!thumbnail && item['media:content']?.[0]?.$?.url) {
      thumbnail = item['media:content'][0].$.url
    }
    if (!thumbnail && item['media:group']?.[0]?. ['media:thumbnail']?.[0]?.$?.url) {
      thumbnail = item['media:group'][0]['media:thumbnail'][0].$.url
    }

    return {
      title: title || 'Untitled',
      content: content || '',
      author: author || undefined,
      publishedAt: extractDate(pubDate),
      url: link || undefined,
      guid: guid || link || undefined,
      videoId: videoMetadata?.videoId,
      embedUrl: videoMetadata?.embedUrl,
      thumbnail,
    }
  })

  return {
    title: extractText(channel.title) || 'Untitled Feed',
    description: extractText(channel.description),
    url: extractText(channel.link),
    items,
  }
}

// Parse Atom format
function parseAtom(data: any): ParsedFeed {
  const feed = data.feed
  
  if (!feed) {
    throw new Error('Invalid Atom format: no feed found')
  }

  const items: ParsedFeedItem[] = (feed.entry || []).map((entry: any) => {
    const title = extractText(entry.title)
    const content = extractText(entry.content) || extractText(entry.summary)
    const link = entry.link?.find((l: any) => l.$.type !== 'text/html' || !l.$.type)
    const url = link?.$?.href
    const author = entry.author?.[0]?.name?.[0] || extractText(entry.author?.name)
    const published = extractText(entry.published) || extractText(entry.updated)
    const id = extractText(entry.id)

    // Extract video metadata if this is a video feed
    const videoMetadata = url ? getVideoMetadata(url) : null
    
    // Extract thumbnail from media:thumbnail or media:group
    let thumbnail = videoMetadata?.thumbnail
    if (!thumbnail && entry['media:thumbnail']?.[0]?.$?.url) {
      thumbnail = entry['media:thumbnail'][0].$.url
    }
    if (!thumbnail && entry['media:group']?.[0]?.['media:thumbnail']?.[0]?.$?.url) {
      thumbnail = entry['media:group'][0]['media:thumbnail'][0].$.url
    }

    return {
      title: title || 'Untitled',
      content: content || '',
      author: author || undefined,
      publishedAt: extractDate(published),
      url: url || undefined,
      guid: id || url || undefined,
      videoId: videoMetadata?.videoId,
      embedUrl: videoMetadata?.embedUrl,
      thumbnail,
    }
  })

  return {
    title: extractText(feed.title) || 'Untitled Feed',
    description: extractText(feed.subtitle) || extractText(feed.description),
    url: feed.link?.find((l: any) => l.$.rel === 'alternate')?.$?.href,
    items,
  }
}

export async function parseFeed(xmlContent: string): Promise<ParsedFeed> {
  try {
    const data = await parseXML(xmlContent) as any
    
    // Determine feed type and parse accordingly
    if (data.rss) {
      return parseRSS(data)
    } else if (data.feed) {
      return parseAtom(data)
    } else {
      throw new Error('Unsupported feed format. Only RSS and Atom feeds are supported.')
    }
  } catch (error) {
    throw new Error(`Failed to parse feed: ${error instanceof Error ? error.message : 'Unknown error'}`)
  }
}

export async function fetchAndParseFeed(url: string): Promise<ParsedFeed> {
  try {
    const response = await safeFetch(url, {
      headers: {
        'User-Agent': 'Readstr/1.0 Feed Reader',
        'Accept': 'application/rss+xml, application/atom+xml, application/xml, text/xml',
      },
      // Set a reasonable timeout
      signal: AbortSignal.timeout(10000), // 10 seconds
    })

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}: ${response.statusText}`)
    }

    const contentType = response.headers.get('content-type') || ''
    if (!contentType.includes('xml') && !contentType.includes('rss') && !contentType.includes('atom')) {
      console.warn(`Unexpected content type: ${contentType}. Attempting to parse anyway.`)
    }

    const xmlContent = await response.text()
    
    if (!xmlContent.trim()) {
      throw new Error('Empty response from feed URL')
    }

    return await parseFeed(xmlContent)
  } catch (error) {
    if (error instanceof TypeError && error.message.includes('fetch')) {
      throw new Error(`Network error: Unable to fetch feed from ${url}`)
    }
    throw error
  }
}

// Helper to discover feed URLs from a webpage
export async function discoverFeedUrl(url: string): Promise<string[]> {
  try {
    const response = await safeFetch(url, {
      headers: {
        'User-Agent': 'Readstr/1.0 Feed Reader',
      },
      signal: AbortSignal.timeout(5000),
    })

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}: ${response.statusText}`)
    }

    const html = await response.text()
    const feedUrls: string[] = []

    // Look for feed links in HTML
    const feedLinkRegex = /<link[^>]+type=["'](application\/(?:rss|atom)\+xml|text\/xml)["'][^>]*>/gi
    const matches = html.match(feedLinkRegex) || []

    for (const match of matches) {
      const hrefMatch = match.match(/href=["']([^"']+)["']/)
      if (hrefMatch) {
        const feedUrl = hrefMatch[1]
        // Convert relative URLs to absolute
        const absoluteUrl = feedUrl.startsWith('http') ? feedUrl : new URL(feedUrl, url).href
        feedUrls.push(absoluteUrl)
      }
    }

    // Common feed paths to try
    if (feedUrls.length === 0) {
      const commonPaths = [
        '/feed',
        '/rss',
        '/feed.xml',
        '/rss.xml',
        '/atom.xml',
        '/feeds/all.atom.xml',
      ]
      
      for (const path of commonPaths) {
        try {
          const feedUrl = new URL(path, url).href
          await fetchAndParseFeed(feedUrl)
          feedUrls.push(feedUrl)
          break // Stop after finding the first working feed
        } catch {
          // Continue trying other paths
        }
      }
    }

    return feedUrls
  } catch (error) {
    throw new Error(`Failed to discover feeds: ${error instanceof Error ? error.message : 'Unknown error'}`)
  }
}