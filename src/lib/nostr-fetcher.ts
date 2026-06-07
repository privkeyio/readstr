import { SimplePool, Event, Filter, nip19 } from 'nostr-tools'
import { safeFetch } from './safe-fetch'

export interface NostrLongFormPost {
  id: string // event id
  title: string
  content: string
  author: string // npub
  publishedAt: Date
  url?: string
  tags: string[]
}

export interface NostrVideoEvent {
  id: string // event id
  title: string
  content: string // description/summary
  author: string // npub
  publishedAt: Date
  videoUrl?: string // primary video URL
  embedUrl?: string // embed-friendly URL
  thumbnail?: string // preview image
  duration?: number // in seconds
  tags: string[]
  kind: 21 | 22 // 21 = normal video, 22 = short-form portrait
}

// Default relays for fetching Nostr content
const DEFAULT_RELAYS = [
  'wss://relay.damus.io',
  'wss://nos.lol',
  'wss://relay.snort.social',
  'wss://relay.nostr.band',
  'wss://nostr-pub.wellorder.net',
]

// Helper to get relays from localStorage or use defaults
function getRelaysFromStorage(): string[] {
  if (typeof window === 'undefined') return DEFAULT_RELAYS
  
  const savedRelays = localStorage.getItem('nostr_relays')
  if (savedRelays) {
    try {
      const relays = JSON.parse(savedRelays)
      if (Array.isArray(relays) && relays.length > 0) {
        return relays
      }
    } catch (e) {
      console.error('Failed to parse saved relays:', e)
    }
  }
  return DEFAULT_RELAYS
}

export class NostrFeedFetcher {
  private pool: SimplePool
  private relays: string[]

  constructor(relays?: string[]) {
    this.pool = new SimplePool()
    this.relays = relays || getRelaysFromStorage()
    console.log('NostrFeedFetcher initialized with relays:', this.relays)
  }

  // Convert npub to hex pubkey
  private npubToHex(npub: string): string {
    try {
      const decoded = nip19.decode(npub)
      if (decoded.type === 'npub') {
        return decoded.data
      }
      throw new Error('Invalid npub format')
    } catch (error) {
      throw new Error(`Failed to decode npub: ${error instanceof Error ? error.message : 'Unknown error'}`)
    }
  }

  // Extract metadata from NIP-23 event
  private parseNip23Event(event: Event): NostrLongFormPost {
    const tags = event.tags
    let title = ''
    let summary = ''
    let publishedAt: Date | null = null
    let url = ''
    const topicTags: string[] = []

    // Parse tags according to NIP-23
    for (const tag of tags) {
      switch (tag[0]) {
        case 'title':
          title = tag[1] || ''
          break
        case 'summary':
          summary = tag[1] || ''
          break
        case 'published_at':
          if (tag[1]) {
            publishedAt = new Date(parseInt(tag[1]) * 1000)
          }
          break
        case 'd':
          // identifier tag - can be used to construct URL
          if (tag[1]) {
            url = tag[1]
          }
          break
        case 't':
          // topic tags
          if (tag[1]) {
            topicTags.push(tag[1])
          }
          break
      }
    }

    return {
      id: event.id,
      title: title || 'Untitled',
      content: event.content || summary || 'No content available',
      author: nip19.npubEncode(event.pubkey),
      publishedAt: publishedAt || new Date(event.created_at * 1000),
      url: url || undefined,
      tags: topicTags,
    }
  }

  // Extract metadata from NIP-71 video event
  private parseNip71Event(event: Event): NostrVideoEvent {
    const tags = event.tags
    let title = ''
    let publishedAt: Date | null = null
    let videoUrl = ''
    let thumbnail = ''
    let duration: number | undefined
    const topicTags: string[] = []

    // Parse tags according to NIP-71
    for (const tag of tags) {
      switch (tag[0]) {
        case 'title':
          title = tag[1] || ''
          break
        case 'published_at':
          if (tag[1]) {
            publishedAt = new Date(parseInt(tag[1]) * 1000)
          }
          break
        case 'imeta':
          // Parse imeta tag for video metadata (NIP-92)
          for (let i = 1; i < tag.length; i++) {
            const param = tag[i]
            if (param.startsWith('url ')) {
              videoUrl = param.substring(4).trim()
            } else if (param.startsWith('image ')) {
              if (!thumbnail) { // Use first image as thumbnail
                thumbnail = param.substring(6).trim()
              }
            } else if (param.startsWith('duration ')) {
              duration = parseFloat(param.substring(9).trim())
            }
          }
          break
        case 't':
          // topic tags
          if (tag[1]) {
            topicTags.push(tag[1])
          }
          break
      }
    }

    return {
      id: event.id,
      title: title || 'Untitled Video',
      content: event.content,
      author: nip19.npubEncode(event.pubkey),
      publishedAt: publishedAt || new Date(event.created_at * 1000),
      videoUrl: videoUrl || undefined,
      embedUrl: videoUrl || undefined, // Use same URL for embed
      thumbnail: thumbnail || undefined,
      duration,
      tags: topicTags,
      kind: event.kind as 21 | 22,
    }
  }

  // Fetch long-form posts from a specific npub
  async fetchLongFormPosts(npub: string, limit: number = 50, since?: Date): Promise<NostrLongFormPost[]> {
    try {
      const authorPubkey = this.npubToHex(npub)
      
      const filter: Filter = {
        kinds: [30023], // NIP-23 long-form content
        authors: [authorPubkey],
        limit,
      }

      if (since) {
        filter.since = Math.floor(since.getTime() / 1000)
      }

      const events = await this.pool.querySync(this.relays, filter)
      
      // Sort by created_at descending (newest first)
      const sortedEvents = events.sort((a, b) => b.created_at - a.created_at)
      
      return sortedEvents.map(event => this.parseNip23Event(event))
    } catch (error) {
      throw new Error(`Failed to fetch Nostr posts: ${error instanceof Error ? error.message : 'Unknown error'}`)
    }
  }

  // Fetch video events from a specific npub (NIP-71)
  async fetchVideoEvents(npub: string, limit: number = 50, since?: Date): Promise<NostrVideoEvent[]> {
    try {
      const authorPubkey = this.npubToHex(npub)
      
      const filter: Filter = {
        kinds: [21, 22], // NIP-71 video events (21 = normal, 22 = short-form)
        authors: [authorPubkey],
        limit,
      }

      if (since) {
        filter.since = Math.floor(since.getTime() / 1000)
      }

      const events = await this.pool.querySync(this.relays, filter)
      
      // Sort by created_at descending (newest first)
      const sortedEvents = events.sort((a, b) => b.created_at - a.created_at)
      
      return sortedEvents.map(event => this.parseNip71Event(event))
    } catch (error) {
      throw new Error(`Failed to fetch Nostr video events: ${error instanceof Error ? error.message : 'Unknown error'}`)
    }
  }

  // Fetch posts from multiple npubs
  async fetchMultipleLongFormPosts(npubs: string[], limit: number = 50, since?: Date): Promise<NostrLongFormPost[]> {
    try {
      const authorPubkeys = npubs.map(npub => this.npubToHex(npub))
      
      const filter: Filter = {
        kinds: [30023], // NIP-23 long-form content
        authors: authorPubkeys,
        limit,
      }

      if (since) {
        filter.since = Math.floor(since.getTime() / 1000)
      }

      const events = await this.pool.querySync(this.relays, filter)
      
      // Sort by created_at descending (newest first)
      const sortedEvents = events.sort((a, b) => b.created_at - a.created_at)
      
      return sortedEvents.map(event => this.parseNip23Event(event))
    } catch (error) {
      throw new Error(`Failed to fetch Nostr posts: ${error instanceof Error ? error.message : 'Unknown error'}`)
    }
  }

  // Get profile information for an npub
  async getProfile(npub: string): Promise<{ name?: string; about?: string; picture?: string } | null> {
    try {
      const pubkey = this.npubToHex(npub)
      
      const filter: Filter = {
        kinds: [0], // Profile metadata
        authors: [pubkey],
        limit: 1,
      }

      const events = await this.pool.querySync(this.relays, filter)
      
      if (events.length === 0) {
        return null
      }

      const profileEvent = events[0]
      const profileData = JSON.parse(profileEvent.content)
      
      return {
        name: profileData.name || profileData.display_name,
        about: profileData.about,
        picture: profileData.picture,
      }
    } catch (error) {
      console.error('Failed to fetch profile:', error)
      return null
    }
  }

  // Validate if an npub exists and has published long-form content or videos
  async validateNostrFeed(npub: string): Promise<{ valid: boolean; profile?: any; hasContent: boolean; hasVideos?: boolean }> {
    try {
      const pubkey = this.npubToHex(npub)
      
      // Check for profile
      const profile = await this.getProfile(npub)
      
      // Check for any long-form content
      const longFormFilter: Filter = {
        kinds: [30023],
        authors: [pubkey],
        limit: 1,
      }

      const longFormEvents = await this.pool.querySync(this.relays, longFormFilter)
      const hasContent = longFormEvents.length > 0

      // Check for any video content
      const videoFilter: Filter = {
        kinds: [21, 22],
        authors: [pubkey],
        limit: 1,
      }

      const videoEvents = await this.pool.querySync(this.relays, videoFilter)
      const hasVideos = videoEvents.length > 0

      return {
        valid: true,
        profile,
        hasContent: hasContent || hasVideos,
        hasVideos,
      }
    } catch (error) {
      return {
        valid: false,
        hasContent: false,
        hasVideos: false,
      }
    }
  }

  // Search for profiles by name or NIP-05 address
  async searchProfiles(query: string, limit: number = 10): Promise<Array<{
    npub: string
    name?: string
    displayName?: string
    about?: string
    picture?: string
    nip05?: string
    verified?: boolean
  }>> {
    try {
      // Search for profiles that might match the query
      const filter: Filter = {
        kinds: [0], // Profile metadata
        limit: limit * 3, // Get more to filter through
      }

      const events = await this.pool.querySync(this.relays, filter)
      const profiles = []

      for (const event of events) {
        try {
          const profileData = JSON.parse(event.content)
          const npub = nip19.npubEncode(event.pubkey)
          
          // Check if profile matches search query
          const searchLower = query.toLowerCase()
          const name = profileData.name?.toLowerCase() || ''
          const displayName = profileData.display_name?.toLowerCase() || ''
          const nip05 = profileData.nip05?.toLowerCase() || ''
          const about = profileData.about?.toLowerCase() || ''
          
          if (
            name.includes(searchLower) ||
            displayName.includes(searchLower) ||
            nip05.includes(searchLower) ||
            about.includes(searchLower) ||
            npub.toLowerCase().includes(searchLower)
          ) {
            profiles.push({
              npub,
              name: profileData.name,
              displayName: profileData.display_name,
              about: profileData.about,
              picture: profileData.picture,
              nip05: profileData.nip05,
              verified: false, // We'll verify NIP-05 separately if needed
            })
          }
        } catch (error) {
          // Skip invalid profile data
          continue
        }
      }

      // Sort by relevance (exact matches first, then partial matches)
      const searchLower = query.toLowerCase()
      profiles.sort((a, b) => {
        const aExact = (a.name?.toLowerCase() === searchLower || a.nip05?.toLowerCase() === searchLower) ? 1 : 0
        const bExact = (b.name?.toLowerCase() === searchLower || b.nip05?.toLowerCase() === searchLower) ? 1 : 0
        return bExact - aExact
      })

      return profiles.slice(0, limit)
    } catch (error) {
      console.error('Failed to search profiles:', error)
      return []
    }
  }

  // Verify NIP-05 address
  async verifyNip05(nip05: string, pubkey: string): Promise<boolean> {
    try {
      const parts = nip05.split('@')
      if (parts.length !== 2) return false

      const [name, domain] = parts
      if (!name || !domain) return false

      const url = `https://${domain}/.well-known/nostr.json?name=${encodeURIComponent(name)}`
      
      const response = await safeFetch(url, {
        headers: { 'Accept': 'application/json' },
        signal: AbortSignal.timeout(5000), // 5 second timeout
      })

      if (!response.ok) return false

      const data = JSON.parse(await response.text())
      return data.names?.[name] === pubkey
    } catch (error) {
      return false
    }
  }

  // Get popular Nostr users (fallback for discovery)
  async getPopularUsers(limit: number = 20): Promise<Array<{
    npub: string
    name?: string
    displayName?: string
    about?: string
    picture?: string
    nip05?: string
  }>> {
    try {
      // Get recent profiles with good metadata
      const filter: Filter = {
        kinds: [0],
        limit: limit * 5, // Get more to filter through
      }

      const events = await this.pool.querySync(this.relays, filter)
      const profiles = []

      for (const event of events.slice(0, 100)) { // Process first 100
        try {
          const profileData = JSON.parse(event.content)
          
          // Only include profiles with names and some content
          if (profileData.name && (profileData.about || profileData.nip05)) {
            profiles.push({
              npub: nip19.npubEncode(event.pubkey),
              name: profileData.name,
              displayName: profileData.display_name,
              about: profileData.about,
              picture: profileData.picture,
              nip05: profileData.nip05,
            })
          }
        } catch (error) {
          continue
        }
      }

      // Sort by having NIP-05 verification and good metadata
      profiles.sort((a, b) => {
        const aScore = (a.nip05 ? 2 : 0) + (a.about ? 1 : 0) + (a.picture ? 1 : 0)
        const bScore = (b.nip05 ? 2 : 0) + (b.about ? 1 : 0) + (b.picture ? 1 : 0)
        return bScore - aScore
      })

      return profiles.slice(0, limit)
    } catch (error) {
      console.error('Failed to get popular users:', error)
      return []
    }
  }

  // Close connections
  close() {
    this.pool.close(this.relays)
  }
}

// Utility function to create a singleton instance
let globalNostrFetcher: NostrFeedFetcher | null = null

export function getNostrFetcher(): NostrFeedFetcher {
  if (!globalNostrFetcher) {
    globalNostrFetcher = new NostrFeedFetcher()
  }
  return globalNostrFetcher
}

export function closeNostrFetcher() {
  if (globalNostrFetcher) {
    globalNostrFetcher.close()
    globalNostrFetcher = null
  }
}