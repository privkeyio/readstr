import { SimplePool, Event, nip19 } from 'nostr-tools'
import type { UnsignedEvent } from 'nostr-tools'

// Privacy note: sync events (kinds 30404 and 30405) are published to public
// relays in cleartext by design. The subscription list must stay readable by
// the server-side sync path (fetchSubscriptionListFromServer), which holds no
// private key under the keyless NIP-07 model and therefore cannot decrypt it.
// Do not put anything in these events that should not be public, and treat a
// user's pubkey as permanently linkable to their subscriptions and read status.

// Kind 30404 for subscription list sync
const SUBSCRIPTION_LIST_KIND = 30404
// Kind 30405 for read status sync
const READ_STATUS_KIND = 30405

const debugLog = (...args: unknown[]) => {
  if (process.env.NODE_ENV === 'development') console.log(...args)
}

// Subscription list event structure
export interface SubscriptionList {
  rss: string[] // RSS feed URLs
  nostr: string[] // Nostr npubs for long-form content
  tags?: Record<string, string[]> // Optional: tags per feed (feedUrl -> tags)
  categories?: Record<string, { name: string; color?: string; icon?: string }> // Optional: category info per feed (feedUrl -> category)
  deleted?: string[] // Feeds that were explicitly removed (URLs or npubs)
  lastUpdated?: number // Unix timestamp
}

// Read status sync using kind 30405
export interface ReadStatusList {
  itemGuids: string[] // FeedItem guids that have been read
  lastUpdated?: number // Unix timestamp
}

// Default relays for sync operations
const DEFAULT_SYNC_RELAYS = [
  'wss://relay.damus.io',
  'wss://nos.lol',
  'wss://relay.snort.social',
  'wss://relay.nostr.band',
  'wss://nostr-pub.wellorder.net',
]

// Helper to get relays from localStorage or use defaults
export function getSyncRelays(): string[] {
  if (typeof window === 'undefined') return DEFAULT_SYNC_RELAYS
  
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
  return DEFAULT_SYNC_RELAYS
}

/**
 * Get relays for server-side sync from provided list or defaults
 */
export function getSyncRelaysFromServer(providedRelays?: string[]): string[] {
  if (providedRelays && providedRelays.length > 0) {
    return providedRelays
  }
  return DEFAULT_SYNC_RELAYS
}

// Get the user's pubkey in hex format
function getPubkeyHex(npubOrHex: string): string {
  if (npubOrHex.startsWith('npub')) {
    const decoded = nip19.decode(npubOrHex)
    if (decoded.type === 'npub') {
      return decoded.data
    }
    throw new Error('Invalid npub')
  }
  return npubOrHex
}

// Verify a relay-returned event actually matches the requested author, kind, and d-tag.
// nostr-tools validates the signature but does not enforce these against the filter,
// so a malicious relay could return a validly-signed event from a different author.
function isExpectedEvent(
  event: Event,
  pubkeyHex: string,
  kind: number,
  dTag: string
): boolean {
  if (event.pubkey.toLowerCase() !== pubkeyHex.toLowerCase()) return false
  if (event.kind !== kind) return false
  const eventDTag = event.tags.find(t => t[0] === 'd')?.[1]
  return eventDTag === dTag
}

/**
 * Publish a subscription list to Nostr relays using kind 30404
 * This is a replaceable event (kind 30000-39999), so newer versions replace older ones
 */
export async function publishSubscriptionList(
  subscriptionList: SubscriptionList,
  signEvent: (event: UnsignedEvent) => Promise<Event>
): Promise<{ success: boolean; eventId?: string; error?: string }> {
  const pool = new SimplePool()
  const relays = getSyncRelays()
  
  try {
    // Create the unsigned event
    const unsignedEvent: UnsignedEvent = {
      kind: SUBSCRIPTION_LIST_KIND,
      created_at: Math.floor(Date.now() / 1000),
      tags: [
        ['d', 'nostr-feedz-subscriptions'], // d-tag for replaceable event identification
        ['client', 'nostr-feedz'],
      ],
      content: JSON.stringify({
        ...subscriptionList,
        lastUpdated: Math.floor(Date.now() / 1000),
      }),
      pubkey: '', // Will be filled by the signing process
    }

    // Sign the event using NIP-07 or provided signer
    const signedEvent = await signEvent(unsignedEvent)
    
    // Publish to all relays
    const publishPromises = pool.publish(relays, signedEvent)
    
    // Wait for at least one relay to accept (use Promise.race as fallback)
    await Promise.race(publishPromises)
    
    debugLog('Published subscription list to Nostr:', signedEvent.id)
    
    return { success: true, eventId: signedEvent.id }
  } catch (error) {
    console.error('Failed to publish subscription list:', error)
    return { 
      success: false, 
      error: error instanceof Error ? error.message : 'Unknown error' 
    }
  } finally {
    pool.close(relays)
  }
}

/**
 * Fetch the user's subscription list from Nostr relays
 */
export async function fetchSubscriptionList(
  userPubkey: string
): Promise<{ success: boolean; data?: SubscriptionList; eventId?: string; createdAt?: number; error?: string }> {
  const pool = new SimplePool()
  const relays = getSyncRelays()
  
  try {
    const pubkeyHex = getPubkeyHex(userPubkey)
    
    // Fetch the subscription list event
    const event = await pool.get(relays, {
      kinds: [SUBSCRIPTION_LIST_KIND],
      authors: [pubkeyHex],
      '#d': ['nostr-feedz-subscriptions'],
    })

    if (!event || !isExpectedEvent(event, pubkeyHex, SUBSCRIPTION_LIST_KIND, 'nostr-feedz-subscriptions')) {
      return {
        success: true,
        data: { rss: [], nostr: [] }, // Return empty list if none found
      }
    }
    
    // Parse the content
    const content = JSON.parse(event.content) as SubscriptionList
    
    return {
      success: true,
      data: content,
      eventId: event.id,
      createdAt: event.created_at,
    }
  } catch (error) {
    console.error('Failed to fetch subscription list:', error)
    return { 
      success: false, 
      error: error instanceof Error ? error.message : 'Unknown error' 
    }
  } finally {
    pool.close(relays)
  }
}

/**
 * Fetch the user's subscription list from Nostr relays (server-side version)
 * Accepts relays as parameter for server-side use
 */
export async function fetchSubscriptionListFromServer(
  userPubkey: string,
  relays?: string[]
): Promise<{ success: boolean; data?: SubscriptionList; eventId?: string; createdAt?: number; error?: string }> {
  const pool = new SimplePool()
  const syncRelays = getSyncRelaysFromServer(relays)
  
  try {
    const pubkeyHex = getPubkeyHex(userPubkey)
    
    // Fetch the subscription list event
    const event = await pool.get(syncRelays, {
      kinds: [SUBSCRIPTION_LIST_KIND],
      authors: [pubkeyHex],
      '#d': ['nostr-feedz-subscriptions'],
    })

    if (!event || !isExpectedEvent(event, pubkeyHex, SUBSCRIPTION_LIST_KIND, 'nostr-feedz-subscriptions')) {
      return {
        success: true,
        data: { rss: [], nostr: [] }, // Return empty list if none found
      }
    }
    
    // Parse the content
    const content = JSON.parse(event.content) as SubscriptionList
    
    return {
      success: true,
      data: content,
      eventId: event.id,
      createdAt: event.created_at,
    }
  } catch (error) {
    console.error('Failed to fetch subscription list:', error)
    return { 
      success: false, 
      error: error instanceof Error ? error.message : 'Unknown error' 
    }
  } finally {
    pool.close(syncRelays)
  }
}

/**
 * Build a subscription list from current feeds, including deleted feeds
 */
export function buildSubscriptionListFromFeeds(
  feeds: Array<{
    type: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'
    url: string
    tags?: string[]
    category?: { name: string; color?: string | null; icon?: string | null } | null
    deletedAt?: Date | null
  }>
): SubscriptionList {
  const rss: string[] = []
  const nostr: string[] = []
  const deleted: string[] = []
  const tags: Record<string, string[]> = {}
  const categories: Record<string, { name: string; color?: string; icon?: string }> = {}
  
  for (const feed of feeds) {
    const identifier = feed.type === 'RSS' ? feed.url : 
      (feed.url.match(/npub\w+/)?.[0] || feed.url)
    
    // Track deleted feeds
    if (feed.deletedAt) {
      deleted.push(identifier)
      continue
    }
    
    if (feed.type === 'RSS') {
      rss.push(feed.url)
      if (feed.tags && feed.tags.length > 0) {
        tags[feed.url] = feed.tags
      }
      if (feed.category) {
        categories[feed.url] = {
          name: feed.category.name,
          color: feed.category.color || undefined,
          icon: feed.category.icon || undefined,
        }
      }
    } else if (feed.type === 'NOSTR' || feed.type === 'NOSTR_VIDEO') {
      // Extract npub from URL if it's a profile URL
      const npubMatch = feed.url.match(/npub\w+/)
      if (npubMatch) {
        nostr.push(npubMatch[0])
        if (feed.tags && feed.tags.length > 0) {
          tags[npubMatch[0]] = feed.tags
        }
        if (feed.category) {
          categories[npubMatch[0]] = {
            name: feed.category.name,
            color: feed.category.color || undefined,
            icon: feed.category.icon || undefined,
          }
        }
      } else {
        // Store the URL as-is if it's not an npub-based URL
        nostr.push(feed.url)
        if (feed.tags && feed.tags.length > 0) {
          tags[feed.url] = feed.tags
        }
        if (feed.category) {
          categories[feed.url] = {
            name: feed.category.name,
            color: feed.category.color || undefined,
            icon: feed.category.icon || undefined,
          }
        }
      }
    }
  }
  
  return { 
    rss, 
    nostr, 
    tags: Object.keys(tags).length > 0 ? tags : undefined,
    categories: Object.keys(categories).length > 0 ? categories : undefined,
    deleted: deleted.length > 0 ? deleted : undefined 
  }
}

/**
 * Normalize a URL for comparison purposes
 * Handles trailing slashes, protocol, and common variations
 */
export function normalizeUrlForComparison(url: string): string {
  try {
    const urlObj = new URL(url.trim())
    // Remove trailing slash from pathname
    let pathname = urlObj.pathname.replace(/\/+$/, '')
    // Lowercase the hostname
    const hostname = urlObj.hostname.toLowerCase()
    // Sort and normalize search params
    urlObj.searchParams.sort()
    const search = urlObj.searchParams.toString()
    // Construct normalized URL without protocol
    return `${hostname}${pathname}${search ? '?' + search : ''}`
  } catch {
    // If URL parsing fails, just lowercase and trim
    return url.toLowerCase().trim().replace(/\/+$/, '')
  }
}

/**
 * Normalize an npub for comparison
 */
export function normalizeNpub(value: string): string {
  // Try to extract npub from the value (might be a URL or just npub)
  const match = value.match(/npub1[a-zA-Z0-9]+/)
  if (match) {
    return match[0].toLowerCase()
  }
  return value.toLowerCase().trim()
}

/**
 * Merge remote subscription list with local feeds
 * Returns lists of feeds to add, remove, and local-only
 */
export function mergeSubscriptionLists(
  localFeeds: Array<{ type: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'; url: string; tags?: string[]; deletedAt?: Date | null }>,
  remoteList: SubscriptionList
): {
  toAdd: Array<{ type: 'RSS' | 'NOSTR'; url: string; tags?: string[]; category?: { name: string; color?: string; icon?: string } }>
  toRemove: Array<{ type: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'; url: string }>
  localOnly: Array<{ type: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'; url: string }>
} {
  // Normalize RSS URLs for comparison
  const localRssUrls = new Set(
    localFeeds
      .filter(f => f.type === 'RSS' && f.url && !f.deletedAt)
      .map(f => normalizeUrlForComparison(f.url))
  )
  
  // For Nostr feeds, extract and normalize npubs
  const localNpubs = new Set(
    localFeeds
      .filter(f => (f.type === 'NOSTR' || f.type === 'NOSTR_VIDEO') && !f.deletedAt)
      .filter(f => f.url) // Filter out empty URLs
      .map(f => normalizeNpub(f.url))
  )
  
  debugLog('🔍 Sync merge - Local RSS URLs (normalized):', Array.from(localRssUrls))
  debugLog('🔍 Sync merge - Local Nostr npubs (normalized):', Array.from(localNpubs))
  debugLog('🔍 Sync merge - Remote RSS URLs:', remoteList.rss)
  debugLog('🔍 Sync merge - Remote Nostr npubs:', remoteList.nostr)
  debugLog('🔍 Sync merge - Remote deleted feeds:', remoteList.deleted)
  
  const toAdd: Array<{ type: 'RSS' | 'NOSTR'; url: string; tags?: string[]; category?: { name: string; color?: string; icon?: string } }> = []
  const toRemove: Array<{ type: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'; url: string }> = []
  
  // Build set of remotely deleted feeds
  const remoteDeleted = new Set<string>()
  if (remoteList.deleted) {
    for (const deletedFeed of remoteList.deleted) {
      // Try to normalize as both URL and npub
      try {
        remoteDeleted.add(normalizeUrlForComparison(deletedFeed))
      } catch {
        remoteDeleted.add(normalizeNpub(deletedFeed))
      }
    }
  }
  
  // Check local feeds against remote deleted list
  for (const localFeed of localFeeds) {
    if (localFeed.deletedAt) continue // Skip already deleted
    
    const normalized = localFeed.type === 'RSS' 
      ? normalizeUrlForComparison(localFeed.url)
      : normalizeNpub(localFeed.url)
    
    if (remoteDeleted.has(normalized)) {
      debugLog(`🗑️ Feed was deleted remotely, marking for removal: ${localFeed.url}`)
      toRemove.push(localFeed)
    }
  }
  
  // Check RSS feeds
  for (const rssUrl of remoteList.rss) {
    const normalizedRemoteUrl = normalizeUrlForComparison(rssUrl)
    const exists = localRssUrls.has(normalizedRemoteUrl)
    
    debugLog(`🔍 RSS check: "${rssUrl}" -> normalized: "${normalizedRemoteUrl}" -> exists: ${exists}`)
    
    if (!exists) {
      toAdd.push({
        type: 'RSS',
        url: rssUrl,
        tags: remoteList.tags?.[rssUrl],
        category: remoteList.categories?.[rssUrl],
      })
    }
  }
  
  // Check Nostr feeds
  for (const npub of remoteList.nostr) {
    const normalizedRemoteNpub = normalizeNpub(npub)
    const exists = localNpubs.has(normalizedRemoteNpub)
    
    debugLog(`🔍 Nostr check: "${npub}" -> normalized: "${normalizedRemoteNpub}" -> exists: ${exists}`)
    
    if (!exists) {
      toAdd.push({
        type: 'NOSTR',
        url: npub,
        tags: remoteList.tags?.[npub],
        category: remoteList.categories?.[npub],
      })
    }
  }
  
  // Find local-only feeds (not in remote and not in deleted list)
  const remoteRssNormalized = new Set(remoteList.rss.map(u => normalizeUrlForComparison(u)))
  const remoteNpubsNormalized = new Set(remoteList.nostr.map(n => normalizeNpub(n)))
  
  const localOnly = localFeeds.filter(f => {
    if (!f.url || f.deletedAt) return false // Skip empty or deleted
    
    const normalized = f.type === 'RSS' 
      ? normalizeUrlForComparison(f.url)
      : normalizeNpub(f.url)
    
    const inRemote = f.type === 'RSS' 
      ? remoteRssNormalized.has(normalized)
      : remoteNpubsNormalized.has(normalized)
    
    const inDeleted = remoteDeleted.has(normalized)
    
    return !inRemote && !inDeleted
  })
  
  debugLog(`🔍 Sync result: ${toAdd.length} to add, ${toRemove.length} to remove, ${localOnly.length} local-only`)
  
  return { toAdd, toRemove, localOnly }
}

/**
 * Get the last sync timestamp from localStorage
 */
export function getLastSyncTime(): number | null {
  if (typeof window === 'undefined') return null
  const stored = localStorage.getItem('nostr_feedz_last_sync')
  return stored ? parseInt(stored, 10) : null
}

/**
 * Save the last sync timestamp to localStorage
 */
export function setLastSyncTime(timestamp: number): void {
  if (typeof window !== 'undefined') {
    localStorage.setItem('nostr_feedz_last_sync', timestamp.toString())
  }
}

const APPLIED_CREATEDAT_PREFIX = 'nostr_feedz_applied_createdat:'

/**
 * Get the created_at of the last applied sync event for a given d-tag.
 * This is a per-d-tag freshness watermark, separate from the rate-limit timestamp.
 */
export function getLastAppliedSyncCreatedAt(dTag: string): number | null {
  if (typeof window === 'undefined') return null
  const stored = localStorage.getItem(APPLIED_CREATEDAT_PREFIX + dTag)
  return stored ? parseInt(stored, 10) : null
}

/**
 * Persist the created_at of the last applied sync event for a given d-tag.
 */
export function setLastAppliedSyncCreatedAt(dTag: string, createdAt: number): void {
  if (typeof window !== 'undefined') {
    localStorage.setItem(APPLIED_CREATEDAT_PREFIX + dTag, createdAt.toString())
  }
}

/**
 * Determine whether a fetched sync event is newer than the last applied one.
 * Equal-or-older events are stale and must be ignored to prevent rollback.
 */
export function isSyncEventFresh(dTag: string, fetchedCreatedAt?: number): boolean {
  if (fetchedCreatedAt == null) return false
  const stored = getLastAppliedSyncCreatedAt(dTag)
  return stored == null || fetchedCreatedAt > stored
}

/**
 * Publish read status to Nostr relays using kind 30405
 */
export async function publishReadStatus(
  readItemGuids: string[],
  signEvent: (event: UnsignedEvent) => Promise<Event>
): Promise<{ success: boolean; eventId?: string; error?: string }> {
  const pool = new SimplePool()
  const relays = getSyncRelays()
  
  try {
    const unsignedEvent: UnsignedEvent = {
      kind: READ_STATUS_KIND,
      created_at: Math.floor(Date.now() / 1000),
      tags: [
        ['d', 'nostr-feedz-read-status'],
        ['client', 'nostr-feedz'],
      ],
      content: JSON.stringify({
        itemGuids: readItemGuids,
        lastUpdated: Math.floor(Date.now() / 1000),
      }),
      pubkey: '',
    }

    const signedEvent = await signEvent(unsignedEvent)
    const publishPromises = pool.publish(relays, signedEvent)
    await Promise.race(publishPromises)
    
    debugLog('Published read status to Nostr:', signedEvent.id)
    
    return { success: true, eventId: signedEvent.id }
  } catch (error) {
    console.error('Failed to publish read status:', error)
    return { 
      success: false, 
      error: error instanceof Error ? error.message : 'Unknown error' 
    }
  } finally {
    pool.close(relays)
  }
}

/**
 * Fetch read status from Nostr relays
 */
export async function fetchReadStatus(
  userPubkey: string
): Promise<{ success: boolean; data?: ReadStatusList; createdAt?: number; error?: string }> {
  const pool = new SimplePool()
  const relays = getSyncRelays()
  
  try {
    const pubkeyHex = getPubkeyHex(userPubkey)
    
    const event = await pool.get(relays, {
      kinds: [READ_STATUS_KIND],
      authors: [pubkeyHex],
      '#d': ['nostr-feedz-read-status'],
    })

    if (!event || !isExpectedEvent(event, pubkeyHex, READ_STATUS_KIND, 'nostr-feedz-read-status')) {
      return {
        success: true,
        data: { itemGuids: [] },
      }
    }
    
    const content = JSON.parse(event.content) as ReadStatusList

    // Defense-in-depth: drop stale/replayed events so a relay can't roll back
    // read status. The caller advances the watermark on apply (mirrors the
    // subscription path), so it isn't persisted here.
    if (!isSyncEventFresh('nostr-feedz-read-status', event.created_at)) {
      return {
        success: true,
        data: { itemGuids: [] },
      }
    }

    return {
      success: true,
      data: content,
      createdAt: event.created_at,
    }
  } catch (error) {
    console.error('Failed to fetch read status:', error)
    return { 
      success: false, 
      error: error instanceof Error ? error.message : 'Unknown error' 
    }
  } finally {
    pool.close(relays)
  }
}
