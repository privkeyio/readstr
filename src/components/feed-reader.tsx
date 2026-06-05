'use client'

import { useNostrAuth } from '@/contexts/NostrAuthContext'
import { useTheme, themeConfig } from '@/contexts/ThemeContext'
import { ThemeSelector, ThemeToggleButton } from './theme-selector'
import { useState, useEffect, useRef, useMemo, useCallback } from 'react'
import { useRouter } from 'next/navigation'
import { api } from '@/trpc/react'
import { AddFeedModal } from './add-feed-modal'
import { SettingsDialog, MarkReadBehavior, OrganizationMode } from './settings-dialog'
import { FormattedContent } from './formatted-content'
import { SimplePool } from 'nostr-tools'
import { 
  fetchSubscriptionList, 
  mergeSubscriptionLists,
  getLastSyncTime,
  publishSubscriptionList,
  buildSubscriptionListFromFeeds,
} from '@/lib/nostr-sync'
import type { UnsignedEvent } from 'nostr-tools'
import type { inferRouterOutputs } from '@trpc/server'
import type { AppRouter } from '@/server/api/root'

interface Category {
  id: string
  name: string
  color: string | null
  icon: string | null
}

interface Feed {
  id: string
  title: string
  type: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'
  unreadCount: number
  url?: string | null
  npub?: string | null
  tags?: string[]
  categoryId?: string | null
  category?: Category | null
}

type RouterOutputs = inferRouterOutputs<AppRouter>
type FeedItemsResponse = RouterOutputs['feed']['getFeedItems']
type FeedItem = FeedItemsResponse['items'][number]
type FavoritesResponse = RouterOutputs['feed']['getFavorites']
type FavoriteItem = FavoritesResponse['items'][number]

const FAVORITES_QUERY_INPUT = { limit: 50 } as const
const QUICK_MARK_READ_OPTIONS: { value: MarkReadBehavior; label: string; helper: string }[] = [
  { value: 'on-open', label: 'On open', helper: 'Mark as soon as I open the story' },
  { value: 'after-10s', label: 'After 10 seconds', helper: 'Give me a short buffer before marking read' },
  { value: 'never', label: 'Never automatically', helper: 'Only change when I click Mark as Read' },
]

export function FeedReader() {
  const { user, disconnect, authMethod, signEvent: signNostrEvent, signEventOrThrow } = useNostrAuth()
  const { theme } = useTheme()
  const router = useRouter()
  const utils = api.useUtils()
  
  const [selectedFeed, setSelectedFeed] = useState<string | null>('all')
  const [selectedItem, setSelectedItem] = useState<string | null>(null)
  const [showAddFeed, setShowAddFeed] = useState(false)
  const [showSettings, setShowSettings] = useState(false)
  const [feedError, setFeedError] = useState<string>('')
  const [sidebarView, setSidebarView] = useState<'feeds' | 'tags' | 'favorites'>('feeds')
  const [selectedTags, setSelectedTags] = useState<string[]>([])
  const [openMenuFeedId, setOpenMenuFeedId] = useState<string | null>(null)
  const [editingFeedId, setEditingFeedId] = useState<string | null>(null)
  const [editTags, setEditTags] = useState<string[]>([])
  const [editTagInput, setEditTagInput] = useState('')
  const [showCategoryPicker, setShowCategoryPicker] = useState<string | null>(null) // feedId when showing category picker
  const [showViewOptions, setShowViewOptions] = useState(false)
  const [viewFilter, setViewFilter] = useState<'all' | 'unread' | 'read'>('all')
  const [sortOrder, setSortOrder] = useState<'newest' | 'oldest'>('newest')
  const [tagSortOrder, setTagSortOrder] = useState<'alphabetical' | 'unread'>('alphabetical')
  const [markReadBehavior, setMarkReadBehavior] = useState<MarkReadBehavior>('on-open')
  const [isSharing, setIsSharing] = useState(false)
  const [shareSuccess, setShareSuccess] = useState(false)
  const [showSyncPrompt, setShowSyncPrompt] = useState(false)
  const [pendingSyncImport, setPendingSyncImport] = useState<Array<{ type: 'RSS' | 'NOSTR'; url: string; tags?: string[] }> | null>(null)
  const [isRefreshingAll, setIsRefreshingAll] = useState(false)
  const [lastRefreshTime, setLastRefreshTime] = useState<number | null>(null)
  const [selectedCategoryId, setSelectedCategoryId] = useState<string | null>(null)
  const markReadTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const hasCheckedSyncRef = useRef(false)
  const hasRefreshedOnLoginRef = useRef(false)
  const refreshIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null)
  
  // Mobile responsive state
  const [showSidebar, setShowSidebar] = useState(false)
  const [mobileView, setMobileView] = useState<'list' | 'content'>('list')
  
  // Organization mode (tags vs categories) - fetched from server
  const { data: userPreference } = api.feed.getUserPreference.useQuery(undefined, {
    enabled: !!user?.npub,
  })
  const organizationMode: OrganizationMode = userPreference?.organizationMode ?? 'tags'
  
  const updatePreferenceMutation = api.feed.updateUserPreference.useMutation({
    onSuccess: () => {
      void utils.feed.getUserPreference.invalidate()
    },
  })
  
  const handleOrganizationModeChange = (mode: OrganizationMode) => {
    updatePreferenceMutation.mutate({ organizationMode: mode })
    // Switch to tags sidebar view when changing organization mode
    setSidebarView('tags')
  }
  
  // Categories query
  const { data: categories = [] } = api.feed.getCategories.useQuery(undefined, {
    enabled: !!user?.npub,
  })
  
  const { data: categoriesWithUnread = [] } = api.feed.getCategoriesWithUnread.useQuery(undefined, {
    enabled: !!user?.npub && organizationMode === 'categories',
  })

  useEffect(() => {
    if (typeof window === 'undefined') return
    const stored = localStorage.getItem('mark_read_behavior') as MarkReadBehavior | 'manual' | null
    if (stored === 'manual') {
      setMarkReadBehavior('never')
      localStorage.setItem('mark_read_behavior', 'never')
      return
    }
    if (stored === 'on-open' || stored === 'after-10s' || stored === 'never') {
      setMarkReadBehavior(stored)
    }
    // Load last refresh time from localStorage
    const storedRefreshTime = localStorage.getItem('last_feed_refresh')
    if (storedRefreshTime) {
      setLastRefreshTime(parseInt(storedRefreshTime, 10))
    }
  }, [])

  const handleMarkReadBehaviorChange = (behavior: MarkReadBehavior) => {
    setMarkReadBehavior(behavior)
    if (typeof window !== 'undefined') {
      localStorage.setItem('mark_read_behavior', behavior)
    }
  }

  // Sign out handler
  const handleSignOut = () => {
    disconnect()
    router.push('/')
  }

  // Debug: Log session info
  useEffect(() => {
    console.log('FeedReader - User:', user)
    if (typeof window !== 'undefined') {
      const session = localStorage.getItem('nostr_session')
      console.log('FeedReader - Session in localStorage:', session)
    }
  }, [user])

  // Close menu when clicking outside
  useEffect(() => {
    const handleClickOutside = (e: MouseEvent) => {
      if (openMenuFeedId) {
        setOpenMenuFeedId(null)
      }
      if (showViewOptions) {
        setShowViewOptions(false)
      }
    }
    
    if (openMenuFeedId || showViewOptions) {
      document.addEventListener('click', handleClickOutside)
      return () => document.removeEventListener('click', handleClickOutside)
    }
  }, [openMenuFeedId, showViewOptions])
  
  // tRPC queries - only run when user is authenticated
  const getFeedsInput = useMemo(() => {
    if (organizationMode === 'categories' && selectedCategoryId) {
      return { categoryId: selectedCategoryId }
    }
    if (organizationMode === 'tags' && selectedTags.length > 0) {
      return { tags: selectedTags }
    }
    return undefined
  }, [organizationMode, selectedCategoryId, selectedTags])
  
  const { data: feedsData = [], refetch: refetchFeeds, isLoading: isFeedsLoading, isFetched: isFeedsFetched } = api.feed.getFeeds.useQuery(
    getFeedsInput,
    {
      enabled: !!user && !!user.npub,
    }
  )
  
  // Filter out any feeds with invalid IDs
  const feeds = feedsData.filter((f: any) => f && f.id && typeof f.id === 'string' && f.id !== 'undefined')

  // Auto-fetch sync on login - check if user has remote subscriptions to import
  useEffect(() => {
    const checkRemoteSync = async () => {
      // Only check once per session and AFTER feeds query has completed (not just started)
      if (hasCheckedSyncRef.current || !user?.npub || !isFeedsFetched || isFeedsLoading) return
      hasCheckedSyncRef.current = true

      // Skip if synced recently (within last hour)
      const lastSync = getLastSyncTime()
      if (lastSync && Date.now() / 1000 - lastSync < 3600) return

      try {
        const result = await fetchSubscriptionList(user.npub)
        if (!result.success || !result.data) return
        
        // Check if there are new subscriptions to import
        if (result.data.rss.length === 0 && result.data.nostr.length === 0) return
        
        const currentFeeds = feeds.map((f: Feed) => ({
          type: f.type,
          url: f.url || f.npub || '',
          tags: f.tags,
        }))

        const mergeResult = mergeSubscriptionLists(currentFeeds, result.data)
        
        if (mergeResult.toAdd.length > 0) {
          setPendingSyncImport(mergeResult.toAdd)
          setShowSyncPrompt(true)
        }
      } catch (error) {
        console.error('Auto-sync check failed:', error)
      }
    }
    
    checkRemoteSync()
  }, [user?.npub, feedsData, isFeedsFetched, isFeedsLoading])
  
  const { data: userTags = [] } = api.feed.getUserTags.useQuery(undefined, {
    enabled: !!user && !!user.npub,
  })
  
  // Favorites query
  const { data: favoritesData, isLoading: favoritesLoading } = api.feed.getFavorites.useQuery(
    FAVORITES_QUERY_INPUT,
    { enabled: !!user && !!user.npub && sidebarView === 'favorites' }
  )
  
  // When tags are selected OR a category is selected, and viewing "All Items", 
  // we need to filter items to only show items from feeds that match the selected tags/category
  const filteredFeedIds = useMemo(() => {
    // Only filter when viewing "All Items" and feeds are loaded
    if (selectedFeed !== 'all' || feeds.length === 0) return undefined
    
    // Filter by tags (tags mode) or by category (when a category is selected)
    const shouldFilter = (organizationMode === 'tags' && selectedTags.length > 0) || 
                         (organizationMode === 'categories' && selectedCategoryId)
    
    if (!shouldFilter) return undefined
    
    // Return all feed IDs from the filtered feeds array (already filtered by getFeeds query)
    return feeds.map((f: any) => f.id).filter((id: any) => id && typeof id === 'string' && id !== 'undefined')
  }, [selectedFeed, feeds, organizationMode, selectedTags, selectedCategoryId])
  
  // Ensure we don't pass invalid feedId values
  const safeFeedId = selectedFeed === 'all' ? undefined : (selectedFeed && typeof selectedFeed === 'string' && selectedFeed !== 'undefined' ? selectedFeed : undefined)
  
  // Build query input conditionally to avoid serialization issues with undefined
  const feedQueryInput = useMemo(() => {
    const input: { feedId?: string; feedIds?: string[] } = {}
    if (safeFeedId) input.feedId = safeFeedId
    if (filteredFeedIds && filteredFeedIds.length > 0) input.feedIds = filteredFeedIds
    return input
  }, [safeFeedId, filteredFeedIds])

  const updateFeedItemCache = (itemId: string, updater: (item: FeedItem) => Partial<FeedItem>) => {
    utils.feed.getFeedItems.setData(feedQueryInput, (data) => {
      if (!data) return data
      return {
        ...data,
        items: data.items.map((item: FeedItem) =>
          item.id === itemId ? { ...item, ...updater(item) } : item
        ),
      }
    })
  }

  const removeFavoriteFromCache = (itemId: string) => {
    utils.feed.getFavorites.setData(FAVORITES_QUERY_INPUT, (data) => {
      if (!data) return data
      return {
        ...data,
        items: data.items.filter((favorite: FavoriteItem) => favorite.id !== itemId),
      }
    })
  }

  const addFavoriteToCache = (item: FeedItem) => {
    utils.feed.getFavorites.setData(FAVORITES_QUERY_INPUT, (data) => {
      if (!data) return data
      const alreadyExists = data.items.some((fav: FavoriteItem) => fav.id === item.id)
      if (alreadyExists) return data

      const newFavorite: FavoriteItem = {
        ...item,
        favoritedAt: new Date(),
        isFavorited: true,
      }

      return {
        ...data,
        items: [newFavorite, ...data.items].slice(0, FAVORITES_QUERY_INPUT.limit),
      }
    })
  }

  const { data: feedItemsData, isLoading: itemsLoading } = api.feed.getFeedItems.useQuery(
    feedQueryInput,
    { 
      enabled: !!user && !!user.npub,
      // Don't retry on 500 errors to avoid spamming the server
      retry: false,
    }
  )
  
  // Mutations
  const subscribeFeedMutation = api.feed.subscribeFeed.useMutation({
    onSuccess: () => {
      invalidateFeedData()
      setShowAddFeed(false)
      setFeedError('')
      // Auto-export after adding a feed
      setTimeout(() => autoExportToNostr(), 500)
    },
    onError: (error) => {
      setFeedError(error.message)
    },
  })
  
  const unsubscribeFeedMutation = api.feed.unsubscribeFeed.useMutation({
    onSuccess: () => {
      invalidateFeedData()
      // If the deleted feed was selected, switch to "All Items"
      setSelectedFeed('all')
      // Auto-export after removing a feed
      setTimeout(() => autoExportToNostr(), 500)
    },
  })
  
  const refreshFeedMutation = api.feed.refreshFeed.useMutation({
    onSuccess: () => {
      invalidateFeedData()
    },
  })
  
  const refreshNostrFeedMutation = api.feed.refreshNostrFeed.useMutation({
    onSuccess: () => {
      invalidateFeedData()
    },
  })

  const refreshAllFeedsMutation = api.feed.refreshAllFeeds.useMutation({
    onSuccess: (result) => {
      invalidateFeedData()
      const now = Date.now()
      setLastRefreshTime(now)
      if (typeof window !== 'undefined') {
        localStorage.setItem('last_feed_refresh', now.toString())
      }
      console.log(`✅ Refreshed ${result.refreshed}/${result.total} feeds, ${result.newItems} new items`)
      setIsRefreshingAll(false)
    },
    onError: (error) => {
      console.error('Failed to refresh feeds:', error)
      setIsRefreshingAll(false)
    },
  })
  
  const updateTagsMutation = api.feed.updateSubscriptionTags.useMutation({
    onSuccess: () => {
      invalidateFeedData()
      setEditingFeedId(null)
      setEditTags([])
      setEditTagInput('')
      // Auto-export after updating tags
      setTimeout(() => autoExportToNostr(), 500)
    },
  })
  
  const updateCategoryMutation = api.feed.updateSubscriptionCategory.useMutation({
    onSuccess: () => {
      invalidateFeedData()
      void utils.feed.getCategoriesWithUnread.invalidate()
      setShowCategoryPicker(null)
      // Auto-export after updating category
      setTimeout(() => autoExportToNostr(), 500)
    },
  })

  const createCategoryMutation = api.feed.createCategory.useMutation({
    onSuccess: () => {
      void utils.feed.getCategories.invalidate()
      void utils.feed.getCategoriesWithUnread.invalidate()
    },
  })
  
  const invalidateFeedData = () => {
    // Use refetch instead of invalidate for immediate UI updates
    void utils.feed.getFeeds.refetch()
    void utils.feed.getUserTags.refetch()
    void utils.feed.getFeedItems.invalidate()
    void utils.feed.getFavorites.invalidate()
  }

  // Auto-refresh all feeds function
  const handleRefreshAllFeeds = useCallback(() => {
    if (isRefreshingAll || !user?.npub) return
    setIsRefreshingAll(true)
    refreshAllFeedsMutation.mutate()
  }, [isRefreshingAll, user?.npub, refreshAllFeedsMutation])

  // Auto-refresh on login (once per session)
  // Separate effect to detect when feeds are first loaded
  const [feedsLoaded, setFeedsLoaded] = useState(false)
  
  useEffect(() => {
    if (feeds.length > 0 && !feedsLoaded) {
      setFeedsLoaded(true)
    }
  }, [feeds.length, feedsLoaded])
  
  useEffect(() => {
    if (!user?.npub || hasRefreshedOnLoginRef.current || !feedsLoaded) return
    
    // Check if we've refreshed recently (within 5 minutes)
    const now = Date.now()
    const fiveMinutes = 5 * 60 * 1000
    if (lastRefreshTime && now - lastRefreshTime < fiveMinutes) {
      hasRefreshedOnLoginRef.current = true
      return
    }

    hasRefreshedOnLoginRef.current = true
    console.log('🔄 Auto-refreshing feeds on login...')
    handleRefreshAllFeeds()
  }, [user?.npub, feedsLoaded, lastRefreshTime, handleRefreshAllFeeds])

  // Set up 30-minute refresh interval
  useEffect(() => {
    if (!user?.npub) return

    // Clear any existing interval
    if (refreshIntervalRef.current) {
      clearInterval(refreshIntervalRef.current)
    }

    // Set up new interval (30 minutes = 1800000ms)
    const thirtyMinutes = 30 * 60 * 1000
    refreshIntervalRef.current = setInterval(() => {
      console.log('🔄 Auto-refreshing feeds (30-minute interval)...')
      handleRefreshAllFeeds()
    }, thirtyMinutes)

    return () => {
      if (refreshIntervalRef.current) {
        clearInterval(refreshIntervalRef.current)
      }
    }
  }, [user?.npub, handleRefreshAllFeeds])

  const markAsReadMutation = api.feed.markAsRead.useMutation({
    onMutate: async ({ itemId }) => {
      // Cancel outgoing refetches to avoid overwriting optimistic update
      await utils.feed.getFeeds.cancel()
      
      // Optimistically update the feed item cache
      updateFeedItemCache(itemId, () => ({ isRead: true }))
      
      // Optimistically update the feeds list unread counts
      const previousFeeds = utils.feed.getFeeds.getData()
      if (previousFeeds) {
        const item = feedItemsData?.items.find(i => i.id === itemId)
        if (item) {
          utils.feed.getFeeds.setData(undefined, (old) => {
            if (!old) return old
            return old.map(feed => {
              if (feed.id === item.feedId && feed.unreadCount > 0) {
                return { ...feed, unreadCount: feed.unreadCount - 1 }
              }
              return feed
            })
          })
        }
      }

      return { previousFeeds }
    },
    onError: (_err, _vars, context) => {
      // Rollback on error
      if (context?.previousFeeds) {
        utils.feed.getFeeds.setData(undefined, context.previousFeeds)
      }
    },
    onSettled: () => {
      invalidateFeedData()
    },
  })
  const markAsUnreadMutation = api.feed.markAsUnread.useMutation({
    onMutate: async ({ itemId }) => {
      await utils.feed.getFeeds.cancel()
      updateFeedItemCache(itemId, () => ({ isRead: false }))

      const previousFeeds = utils.feed.getFeeds.getData()
      if (previousFeeds) {
        const item = feedItemsData?.items.find(i => i.id === itemId)
        if (item) {
          utils.feed.getFeeds.setData(undefined, (old) => {
            if (!old) return old
            return old.map(feed => {
              if (feed.id === item.feedId) {
                return { ...feed, unreadCount: feed.unreadCount + 1 }
              }
              return feed
            })
          })
        }
      }
      
      return { previousFeeds }
    },
    onError: (_err, _vars, context) => {
      if (context?.previousFeeds) {
        utils.feed.getFeeds.setData(undefined, context.previousFeeds)
      }
    },
    onSettled: () => {
      invalidateFeedData()
    },
  })
  
  const markAllAsReadMutation = api.feed.markFeedAsRead.useMutation({
    onSuccess: () => {
      invalidateFeedData()
    },
  })
  
  const addFavoriteMutation = api.feed.addFavorite.useMutation({
    onSuccess: invalidateFeedData,
  })
  
  const removeFavoriteMutation = api.feed.removeFavorite.useMutation({
    onSuccess: invalidateFeedData,
  })
  
  // Prepare feeds list with "All Items" option
  const allFeeds: Feed[] = [
    {
      id: 'all',
      title: 'All Items',
      type: 'RSS' as const,
      unreadCount: feedItemsData?.items.filter((item: FeedItem) => !item.isRead).length || 0,
    },
    ...feeds.map((feed: Feed) => ({
      id: feed.id,
      title: feed.title,
      type: feed.type as 'RSS' | 'NOSTR' | 'NOSTR_VIDEO',
      unreadCount: feed.unreadCount,
      url: feed.url || undefined,
      npub: feed.npub || undefined,
      tags: feed.tags,
      categoryId: feed.categoryId || undefined,
    })),
  ]
  
  // Calculate filtered tags based on currently visible feeds
  // When tags are selected, only show tags that appear on the filtered feeds
  const filteredTags = selectedTags.length > 0 
    ? (() => {
        const tagMap = new Map<string, { tag: string; unreadCount: number; feedCount: number }>()
        
        // Only count tags from feeds that match the current filter
        for (const feed of feeds) {
          const feedUnreadCount = feed.unreadCount || 0
          
          for (const tag of (feed.tags || [])) {
            const existing = tagMap.get(tag)
            if (existing) {
              existing.unreadCount += feedUnreadCount
              existing.feedCount += 1
            } else {
              tagMap.set(tag, {
                tag,
                unreadCount: feedUnreadCount,
                feedCount: 1,
              })
            }
          }
        }
        
        const tags = Array.from(tagMap.values())
        return tagSortOrder === 'unread'
          ? tags.sort((a, b) => b.unreadCount - a.unreadCount || a.tag.localeCompare(b.tag))
          : tags.sort((a, b) => a.tag.localeCompare(b.tag))
      })()
    : tagSortOrder === 'unread'
      ? [...userTags].sort((a, b) => b.unreadCount - a.unreadCount || a.tag.localeCompare(b.tag))
      : userTags
  
  // Filter and sort feed items based on view options
  const allFeedItems = feedItemsData?.items || []
  let feedItems = allFeedItems
  
  // Apply read/unread filter
  if (viewFilter === 'unread') {
    feedItems = feedItems.filter((item: any) => !item.isRead)
  } else if (viewFilter === 'read') {
    feedItems = feedItems.filter((item: any) => item.isRead)
  }
  
  // Apply sort order
  if (sortOrder === 'oldest') {
    feedItems = [...feedItems].reverse()
  }
  
  const selectedItemData = selectedItem
    ? (feedItems.find((item: FeedItem) => item.id === selectedItem) || allFeedItems.find((item: FeedItem) => item.id === selectedItem) || null)
    : null
  const selectedItemOriginalUrl = selectedItemData?.originalUrl ?? selectedItemData?.url
  const selectedItemIsRead = selectedItemData?.isRead ?? false

  useEffect(() => {
    if (markReadTimeoutRef.current) {
      clearTimeout(markReadTimeoutRef.current)
      markReadTimeoutRef.current = null
    }

    if (markReadBehavior !== 'after-10s' || !selectedItem || selectedItemIsRead) {
      return
    }

    const timeoutId = setTimeout(() => {
      markAsReadMutation.mutate({ itemId: selectedItem })
    }, 10000)

    markReadTimeoutRef.current = timeoutId

    return () => {
      if (markReadTimeoutRef.current) {
        clearTimeout(markReadTimeoutRef.current)
        markReadTimeoutRef.current = null
      }
    }
  }, [selectedItem, markReadBehavior, selectedItemIsRead, markAsReadMutation])
  
  // Handle adding new feed
  const handleAddFeed = async (type: 'RSS' | 'NOSTR', url?: string, npub?: string, title?: string, tags?: string[], categoryId?: string) => {
    try {
      await subscribeFeedMutation.mutateAsync({
        type,
        url,
        npub,
        title,
        tags,
        categoryId,
      })
    } catch (error) {
      // Error is handled by onError callback
    }
  }
  
  // Handle marking item as read when clicked
  const handleItemClick = (itemId: string) => {
    setSelectedItem(itemId)
    const item = allFeedItems.find((i: FeedItem) => i.id === itemId)
    if (markReadBehavior === 'on-open' && item && !item.isRead) {
      updateFeedItemCache(itemId, () => ({ isRead: true }))
      markAsReadMutation.mutate({ itemId })
    }
  }

  const handleToggleReadStatus = (item: FeedItem | null) => {
    if (!item) return
    if (item.isRead) {
      markAsUnreadMutation.mutate({ itemId: item.id })
    } else {
      markAsReadMutation.mutate({ itemId: item.id })
    }
  }
  
  // Handle toggling favorite status
  const handleToggleFavorite = (itemId: string, isFavorited: boolean) => {
    updateFeedItemCache(itemId, () => ({ isFavorited: !isFavorited }))
    if (isFavorited) {
      removeFavoriteFromCache(itemId)
      removeFavoriteMutation.mutate({ itemId })
    } else {
      const sourceItem = allFeedItems.find((item: FeedItem) => item.id === itemId)
      if (sourceItem) {
        addFavoriteToCache({ ...sourceItem, isFavorited: true })
      }
      addFavoriteMutation.mutate({ itemId })
    }
  }

  // Handle sharing to Nostr
  const handleShareToNostr = async (item: FeedItem, originalUrl: string | null | undefined) => {
    if (authMethod !== 'nip07' || !user?.pubkey) {
      alert('Connect with a Nostr browser extension (NIP-07) to share posts.')
      return
    }

    setIsSharing(true)
    setShareSuccess(false)

    try {
      // Build the share URL - for Nostr posts use habla.news, otherwise use original URL
      const shareUrl = originalUrl || item.url || ''

      // Create the note content with attribution
      const noteContent = `📖 ${item.title}\n\n${shareUrl}\n\n— shared from nostrfeedz.com`

      const unsignedEvent: UnsignedEvent = {
        kind: 1,
        pubkey: user.pubkey,
        created_at: Math.floor(Date.now() / 1000),
        tags: [] as string[][],
        content: noteContent,
      }

      const signedEvent = await signNostrEvent(unsignedEvent)

      if (!signedEvent) {
        throw new Error('Failed to sign event')
      }

      // Publish to relays
      const pool = new SimplePool()
      const relays = [
        'wss://relay.damus.io',
        'wss://nos.lol',
        'wss://relay.nostr.band',
        'wss://relay.snort.social',
      ]

      // Publish and wait for at least one relay to confirm
      const publishPromises = pool.publish(relays, signedEvent)
      await Promise.race(publishPromises)
      pool.close(relays)

      setShareSuccess(true)
      setTimeout(() => setShareSuccess(false), 3000)
    } catch (error) {
      console.error('Failed to share to Nostr:', error)
      alert('Failed to share to Nostr. Please try again.')
    } finally {
      setIsSharing(false)
    }
  }

  // Auto-export subscriptions to Nostr after changes
  const autoExportToNostr = useCallback(async () => {
    // Only auto-export if signing is available through the active auth method
    if (authMethod !== 'nip07' || !user?.npub) {
      return
    }

    try {
      console.log('🔄 Auto-exporting subscriptions to Nostr...')

      // Fetch all subscriptions including deleted ones for proper sync
      const allSubscriptions = await utils.feed.getAllSubscriptionsForSync.fetch()

      const subscriptionList = buildSubscriptionListFromFeeds(allSubscriptions)

      const result = await publishSubscriptionList(subscriptionList, signEventOrThrow)
      
      if (result.success) {
        console.log('✅ Auto-export successful:', result.eventId)
      } else {
        console.warn('⚠️ Auto-export failed:', result.error)
      }
    } catch (error) {
      console.error('❌ Auto-export error:', error)
      // Silently fail - don't interrupt user experience
    }
  }, [user?.npub, authMethod, signEventOrThrow, utils.feed])

  // Handle importing feeds from Nostr sync
  const handleImportFeeds = async (feedsToImport: Array<{ type: 'RSS' | 'NOSTR'; url: string; tags?: string[]; category?: { name: string; color?: string; icon?: string } }>) => {
    // First, create a map of category names to IDs for existing categories
    const categoryMap = new Map<string, string>()
    for (const cat of categories) {
      categoryMap.set(cat.name, cat.id)
    }

    for (const feed of feedsToImport) {
      try {
        let categoryId: string | undefined

        // If feed has a category, ensure it exists and get its ID
        if (feed.category) {
          const existingCategoryId = categoryMap.get(feed.category.name)
          
          if (existingCategoryId) {
            // Category already exists
            categoryId = existingCategoryId
          } else {
            // Create new category
            try {
              const newCategory = await createCategoryMutation.mutateAsync({
                name: feed.category.name,
                color: feed.category.color,
                icon: feed.category.icon,
              })
              categoryId = newCategory.id
              categoryMap.set(feed.category.name, newCategory.id)
            } catch (error) {
              console.error(`Failed to create category: ${feed.category.name}`, error)
              // Continue without category if creation fails
            }
          }
        }

        if (feed.type === 'RSS') {
          await subscribeFeedMutation.mutateAsync({
            type: 'RSS',
            url: feed.url,
            tags: feed.tags,
            categoryId,
          })
        } else {
          // For Nostr feeds, the url field contains the npub
          await subscribeFeedMutation.mutateAsync({
            type: 'NOSTR',
            npub: feed.url,
            tags: feed.tags,
            categoryId,
          })
        }
      } catch (error) {
        console.error(`Failed to import feed: ${feed.url}`, error)
      }
    }
  }
  
  // Handle removing a feed
  const handleRemoveFeed = (feedId: string, feedTitle: string) => {
    if (confirm(`Are you sure you want to unsubscribe from "${feedTitle}"?`)) {
      unsubscribeFeedMutation.mutate({ feedId })
    }
  }
  
  // Handle refreshing a feed
  const handleRefreshFeed = (feedId: string, feedType: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO') => {
    if (feedType === 'RSS') {
      refreshFeedMutation.mutate({ feedId })
    } else {
      refreshNostrFeedMutation.mutate({ feedId })
    }
  }

  // Handle marking all items in a feed as read
  const handleMarkAllAsRead = (feedId: string) => {
    markAllAsReadMutation.mutate({ feedId })
    setOpenMenuFeedId(null)
  }

  // Handle tag selection
  const handleToggleTag = (tag: string) => {
    setSelectedTags(prev => 
      prev.includes(tag) 
        ? prev.filter(t => t !== tag)
        : [...prev, tag]
    )
    // Reset to "All Items" when filtering by tags
    setSelectedFeed('all')
    // Close sidebar on mobile when tag is selected
    setShowSidebar(false)
  }

  const handleClearTags = () => {
    setSelectedTags([])
  }

  // Handle opening edit menu
  const handleOpenEditMenu = (feedId: string, currentTags: string[]) => {
    setEditingFeedId(feedId)
    setEditTags([...currentTags])
    setEditTagInput('')
    setOpenMenuFeedId(null)
  }

  // Handle adding tag in edit mode
  const handleAddEditTag = () => {
    const trimmed = editTagInput.trim()
    if (trimmed && !editTags.includes(trimmed)) {
      setEditTags([...editTags, trimmed])
      setEditTagInput('')
    }
  }

  // Handle removing tag in edit mode
  const handleRemoveEditTag = (tag: string) => {
    setEditTags(editTags.filter(t => t !== tag))
  }

  // Handle saving edited tags
  const handleSaveEditedTags = (feedId: string) => {
    updateTagsMutation.mutate({ feedId, tags: editTags })
  }

  // Handle canceling edit
  const handleCancelEdit = () => {
    setEditingFeedId(null)
    setEditTags([])
    setEditTagInput('')
  }

  return (
    <div className="flex h-screen bg-theme-secondary">
      {/* Mobile Header */}
      <div className="md:hidden fixed top-0 left-0 right-0 bg-theme-surface border-b border-theme-primary z-50 shadow-theme-sm">
        <div className="flex items-center justify-between p-4">
          <button
            onClick={() => setShowSidebar(!showSidebar)}
            className="p-2 rounded-lg hover:bg-theme-hover text-theme-secondary transition-colors"
          >
            <svg className="w-6 h-6" fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M4 6h16M4 12h16M4 18h16" />
            </svg>
          </button>
          <h1 className="text-lg font-bold text-theme-primary tracking-tight">
            Readstr
          </h1>
          <div className="flex items-center gap-1">
            <ThemeToggleButton />
            <button
              onClick={() => setShowSettings(true)}
              className="p-2 rounded-lg hover:bg-theme-hover text-theme-secondary transition-colors"
              title="Settings"
            >
              <svg className="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.065 2.572c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.572 1.065c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.065-2.572c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z" />
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M15 12a3 3 0 11-6 0 3 3 0 016 0z" />
              </svg>
            </button>
            <button
              onClick={() => setShowAddFeed(true)}
              className="p-2 rounded-lg bg-theme-accent hover:bg-theme-accent-hover text-white transition-colors"
              title="Add Feed"
            >
              <svg className="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M12 4v16m8-8H4" />
              </svg>
            </button>
          </div>
        </div>
        
        {/* Mobile Feeds/Tags Toggle */}
        <div className="px-4 pb-3">
          <div className="flex bg-theme-tertiary rounded-xl p-1">
            <button
              onClick={() => {
                setSidebarView('feeds')
                setSelectedTags([])
                setShowSidebar(true)
              }}
              className={`flex-1 px-3 py-2 text-sm rounded-lg font-medium transition-all duration-200 ${
                sidebarView === 'feeds'
                  ? 'bg-theme-surface shadow-theme-sm text-theme-primary'
                  : 'text-theme-secondary hover:text-theme-primary'
              }`}
            >
              📰 Feeds
            </button>
            <button
              onClick={() => {
                setSidebarView('tags')
                setShowSidebar(true)
              }}
              className={`flex-1 px-3 py-2 text-sm rounded-lg font-medium transition-all duration-200 ${
                sidebarView === 'tags'
                  ? 'bg-theme-surface shadow-theme-sm text-theme-primary'
                  : 'text-theme-secondary hover:text-theme-primary'
              }`}
            >
              🏷️ {organizationMode === 'categories' ? 'Categories' : 'Tags'}
            </button>
            <button
              onClick={() => {
                setSidebarView('favorites')
                setSelectedTags([])
                setShowSidebar(true)
              }}
              className={`flex-1 px-3 py-2 text-sm rounded-lg font-medium transition-all duration-200 ${
                sidebarView === 'favorites'
                  ? 'bg-theme-surface shadow-theme-sm text-theme-primary'
                  : 'text-theme-secondary hover:text-theme-primary'
              }`}
            >
              ⭐ Saved
            </button>
          </div>
        </div>
      </div>

      {/* Backdrop for mobile sidebar */}
      {showSidebar && (
        <div 
          className="md:hidden fixed inset-0 bg-black/50 backdrop-blur-sm z-[45] transition-opacity"
          onClick={() => setShowSidebar(false)}
        />
      )}

      {/* Left Sidebar - Feeds/Tags */}
      <div className={`
        ${showSidebar ? 'translate-x-0' : '-translate-x-full'}
        md:translate-x-0 md:relative
        fixed inset-y-0 left-0 z-50
        w-72 bg-theme-surface border-r border-theme-primary flex flex-col max-h-screen
        transition-transform duration-300 ease-out
        pt-32 md:pt-0
        shadow-theme-lg md:shadow-none
      `}>
        {/* Header - Hidden on mobile (shown in top bar instead) */}
        <div className="hidden md:block p-5 border-b border-theme-primary flex-shrink-0">
          <div className="flex items-center justify-between mb-4">
            <h1 className="text-xl font-bold text-theme-primary tracking-tight">
              Readstr
            </h1>
            <div className="flex items-center gap-1">
              <button
                onClick={handleRefreshAllFeeds}
                disabled={isRefreshingAll}
                className={`p-2 rounded-lg hover:bg-theme-hover text-theme-secondary transition-all ${isRefreshingAll ? 'animate-spin' : ''}`}
                title={isRefreshingAll ? 'Refreshing...' : `Refresh all feeds${lastRefreshTime ? ` (last: ${new Date(lastRefreshTime).toLocaleTimeString()})` : ''}`}
              >
                <svg className="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                  <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15" />
                </svg>
              </button>
              <ThemeSelector showLabels={false} />
              <button
                onClick={() => setShowSettings(true)}
                className="p-2 rounded-lg hover:bg-theme-hover text-theme-secondary transition-colors"
                title="Settings"
              >
                <svg className="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                  <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.065 2.572c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.572 1.065c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.065-2.572c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z" />
                  <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M15 12a3 3 0 11-6 0 3 3 0 016 0z" />
                </svg>
              </button>
            </div>
          </div>
          <button
            onClick={() => setShowAddFeed(true)}
            className="w-full flex items-center justify-center gap-2 px-4 py-2.5 bg-theme-accent hover:bg-theme-accent-hover text-white rounded-xl font-medium transition-colors shadow-theme-sm"
          >
            <svg className="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M12 4v16m8-8H4" />
            </svg>
            Add Feed
          </button>
          <div className="mt-3 text-xs text-theme-tertiary truncate flex items-center gap-2">
            <span className="font-mono">{user?.npub?.slice(0, 20)}...</span>
            {isRefreshingAll && (
              <span className="text-theme-accent animate-pulse-subtle">Refreshing...</span>
            )}
          </div>
        </div>

        {/* View Toggle */}
        <div className="px-4 py-3 border-b border-theme-primary flex-shrink-0">
          <div className="flex bg-theme-tertiary rounded-xl p-1">
            <button
              onClick={() => {
                setSidebarView('feeds')
                setSelectedTags([])
                setSelectedCategoryId(null)
              }}
              className={`flex-1 px-3 py-2 text-sm rounded-lg font-medium transition-all duration-200 ${
                sidebarView === 'feeds'
                  ? 'bg-theme-surface shadow-theme-sm text-theme-primary'
                  : 'text-theme-secondary hover:text-theme-primary'
              }`}
            >
              Feeds
            </button>
            <button
              onClick={() => setSidebarView('tags')}
              className={`flex-1 px-3 py-2 text-sm rounded-lg font-medium transition-all duration-200 ${
                sidebarView === 'tags'
                  ? 'bg-theme-surface shadow-theme-sm text-theme-primary'
                  : 'text-theme-secondary hover:text-theme-primary'
              }`}
            >
              {organizationMode === 'categories' ? 'Categories' : 'Tags'}
            </button>
            <button
              onClick={() => {
                setSidebarView('favorites')
                setSelectedTags([])
                setSelectedCategoryId(null)
              }}
              className={`flex-1 px-3 py-2 text-sm rounded-lg font-medium transition-all duration-200 ${
                sidebarView === 'favorites'
                  ? 'bg-theme-surface shadow-theme-sm text-theme-primary'
                  : 'text-theme-secondary hover:text-theme-primary'
              }`}
            >
              ⭐
            </button>
          </div>
        </div>

        {/* Active Tag Filters */}
        {selectedTags.length > 0 && (
          <div className="px-4 py-3 border-b border-theme-primary bg-theme-accent-light flex-shrink-0">
            <div className="flex items-center justify-between mb-2">
              <span className="text-xs font-semibold text-theme-accent uppercase tracking-wider">Filtered by</span>
              <button
                onClick={handleClearTags}
                className="text-xs font-medium text-theme-accent hover:underline"
              >
                Clear all
              </button>
            </div>
            <div className="flex flex-wrap gap-1.5">
              {selectedTags.map(tag => (
                <span
                  key={tag}
                  className="inline-flex items-center px-2.5 py-1 bg-theme-surface rounded-full text-xs font-medium text-theme-accent shadow-theme-sm"
                >
                  {tag}
                  <button
                    onClick={() => handleToggleTag(tag)}
                    className="ml-1.5 hover:text-theme-accent-hover"
                  >
                    ×
                  </button>
                </span>
              ))}
            </div>
          </div>
        )}

        {/* Feeds List */}
        {sidebarView === 'feeds' && (
          <div className="flex-1 overflow-y-auto themed-scrollbar">
          {allFeeds.map((feed) => (
            <div
              key={feed.id}
              className={`relative group w-full text-left transition-all duration-150 ${
                selectedFeed === feed.id 
                  ? 'bg-theme-accent-light border-l-4 border-l-[rgb(var(--color-accent))]' 
                  : 'hover:bg-theme-hover border-l-4 border-l-transparent'
              }`}
            >
              <button
                onClick={() => {
                  setSelectedFeed(feed.id)
                  setShowSidebar(false)
                }}
                className="w-full px-4 py-3"
              >
                <div className="flex items-center justify-between">
                  <div className="flex items-center gap-2.5 min-w-0">
                    <span className={`text-base flex-shrink-0 ${feed.id === 'all' ? 'opacity-80' : ''}`}>
                      {feed.id === 'all' ? '📚' : feed.type === 'RSS' ? '📰' : feed.type === 'NOSTR_VIDEO' ? '🎬' : '⚡'}
                    </span>
                    <span className={`text-sm font-medium truncate ${
                      selectedFeed === feed.id ? 'text-theme-accent' : 'text-theme-primary'
                    }`}>
                      {feed.title}
                    </span>
                  </div>
                  {feed.unreadCount > 0 && (
                    <span className="unread-badge flex-shrink-0 ml-2">
                      {feed.unreadCount > 99 ? '99+' : feed.unreadCount}
                    </span>
                  )}
                </div>
                {feed.url && (
                  <div className="text-xs text-theme-tertiary truncate mt-1 ml-7">
                    {new URL(feed.url).hostname}
                  </div>
                )}
                {feed.npub && (
                  <div className="text-xs text-theme-tertiary truncate mt-1 ml-7 font-mono">
                    {feed.npub.slice(0, 16)}...
                  </div>
                )}
                {/* Show tags if any */}
                {feed.tags && feed.tags.length > 0 && (
                  <div className="flex flex-wrap gap-1 mt-2 ml-7">
                    {feed.tags.map(tag => (
                      <span
                        key={tag}
                        className="tag-badge"
                      >
                        {tag}
                      </span>
                    ))}
                  </div>
                )}
              </button>
              
              {/* Menu button - only show for actual feeds, not "All Items" */}
              {feed.id !== 'all' && (
                <div className="absolute right-3 top-3">
                  <button
                    onClick={(e) => {
                      e.stopPropagation()
                      setOpenMenuFeedId(openMenuFeedId === feed.id ? null : feed.id)
                    }}
                    className="opacity-0 group-hover:opacity-100 p-1.5 rounded-lg bg-theme-tertiary hover:bg-theme-hover text-theme-secondary transition-all"
                    title="Menu"
                  >
                    <svg className="w-4 h-4" fill="currentColor" viewBox="0 0 20 20">
                      <path d="M10 6a2 2 0 110-4 2 2 0 010 4zM10 12a2 2 0 110-4 2 2 0 010 4zM10 18a2 2 0 110-4 2 2 0 010 4z" />
                    </svg>
                  </button>
                  
                  {/* Dropdown menu */}
                  {openMenuFeedId === feed.id && !showCategoryPicker && (
                    <div className="absolute right-0 mt-1 w-52 dropdown-menu animate-slide-in">
                      <button
                        onClick={(e) => {
                          e.stopPropagation()
                          handleRefreshFeed(feed.id, feed.type)
                          setOpenMenuFeedId(null)
                        }}
                        disabled={refreshFeedMutation.isLoading || refreshNostrFeedMutation.isLoading}
                        className="dropdown-item flex items-center gap-2 disabled:opacity-50"
                      >
                        <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15" />
                        </svg>
                        Refresh
                      </button>
                      <button
                        onClick={(e) => {
                          e.stopPropagation()
                          handleMarkAllAsRead(feed.id)
                        }}
                        disabled={markAllAsReadMutation.isLoading || feed.unreadCount === 0}
                        className="dropdown-item flex items-center gap-2 disabled:opacity-50"
                      >
                        <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M5 13l4 4L19 7" />
                        </svg>
                        Mark All Read
                      </button>
                      {organizationMode === 'categories' && (
                        <button
                          onClick={(e) => {
                            e.stopPropagation()
                            setShowCategoryPicker(feed.id)
                          }}
                          className="dropdown-item flex items-center gap-2"
                        >
                          <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M3 7v10a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-6l-2-2H5a2 2 0 00-2 2z" />
                          </svg>
                          {(() => {
                            const cat = categories?.find((c: any) => c.id === feed.categoryId)
                            return cat 
                              ? <span className="truncate">{cat.icon || '📁'} {cat.name}</span>
                              : 'Set Category'
                          })()}
                        </button>
                      )}
                      {organizationMode === 'tags' && (
                        <button
                          onClick={(e) => {
                            e.stopPropagation()
                            const currentFeed = feeds.find((f: any) => f.id === feed.id)
                            handleOpenEditMenu(feed.id, currentFeed?.tags || [])
                          }}
                          className="dropdown-item flex items-center gap-2"
                        >
                          <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M7 7h.01M7 3h5c.512 0 1.024.195 1.414.586l7 7a2 2 0 010 2.828l-7 7a2 2 0 01-2.828 0l-7-7A1.994 1.994 0 013 12V7a4 4 0 014-4z" />
                          </svg>
                          Edit Tags
                        </button>
                      )}
                      <div className="border-t border-theme-primary my-1" />
                      <button
                        onClick={(e) => {
                          e.stopPropagation()
                          handleRemoveFeed(feed.id, feed.title)
                          setOpenMenuFeedId(null)
                        }}
                        className="dropdown-item flex items-center gap-2 text-red-600 hover:bg-red-50"
                      >
                        <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" />
                        </svg>
                        Unsubscribe
                      </button>
                    </div>
                  )}
                  
                  {/* Category Picker Dropdown */}
                  {showCategoryPicker === feed.id && (
                    <div className="absolute right-0 mt-1 w-60 dropdown-menu animate-slide-in">
                      <div className="px-3 py-2 border-b border-theme-primary flex items-center justify-between">
                        <span className="text-sm font-semibold text-theme-primary">Set Category</span>
                        <button
                          onClick={(e) => {
                            e.stopPropagation()
                            setShowCategoryPicker(null)
                            setOpenMenuFeedId(null)
                          }}
                          className="p-1 rounded hover:bg-theme-hover text-theme-tertiary"
                        >
                          <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M6 18L18 6M6 6l12 12" />
                          </svg>
                        </button>
                      </div>
                      <div className="max-h-48 overflow-y-auto themed-scrollbar py-1">
                        <button
                          onClick={(e) => {
                            e.stopPropagation()
                            updateCategoryMutation.mutate({ feedId: feed.id, categoryId: null })
                          }}
                          className={`dropdown-item flex items-center gap-2 ${
                            !feed.categoryId ? 'active' : ''
                          }`}
                        >
                          <span className="text-base">📋</span>
                          <span>No Category</span>
                        </button>
                        {categories.map((cat: Category) => (
                          <button
                            key={cat.id}
                            onClick={(e) => {
                              e.stopPropagation()
                              updateCategoryMutation.mutate({ feedId: feed.id, categoryId: cat.id })
                            }}
                            className={`dropdown-item flex items-center gap-2 ${
                              feed.categoryId === cat.id ? 'active' : ''
                            }`}
                          >
                            <span 
                              className="w-6 h-6 rounded-lg flex items-center justify-center text-sm"
                              style={{ backgroundColor: cat.color ?? 'rgb(var(--color-bg-tertiary))' }}
                            >
                              {cat.icon || '📁'}
                            </span>
                            <span className="truncate">{cat.name}</span>
                          </button>
                        ))}
                      </div>
                      {categories.length === 0 && (
                        <div className="px-3 py-4 text-xs text-theme-tertiary text-center">
                          No categories yet. Create them in Settings.
                        </div>
                      )}
                    </div>
                  )}
                </div>
              )}
            </div>
          ))}
        </div>
        )}

        {/* Tags/Categories List */}
        {sidebarView === 'tags' && (
          <div className="flex-1 overflow-y-auto themed-scrollbar flex flex-col">
            {/* Sort Options - only for tags mode */}
            {organizationMode === 'tags' && (
              <div className="px-4 py-3 border-b border-theme-primary flex items-center justify-between flex-shrink-0">
                <span className="text-xs font-semibold text-theme-tertiary uppercase tracking-wider">Sort by</span>
                <div className="flex bg-theme-tertiary rounded-lg p-0.5">
                  <button
                    onClick={() => setTagSortOrder('alphabetical')}
                    className={`px-2.5 py-1 text-xs rounded-md font-medium transition-all ${
                      tagSortOrder === 'alphabetical'
                        ? 'bg-theme-surface shadow-theme-sm text-theme-primary'
                        : 'text-theme-secondary hover:text-theme-primary'
                    }`}
                  >
                    A-Z
                  </button>
                  <button
                    onClick={() => setTagSortOrder('unread')}
                    className={`px-2.5 py-1 text-xs rounded-md font-medium transition-all ${
                      tagSortOrder === 'unread'
                        ? 'bg-theme-surface shadow-theme-sm text-theme-primary'
                        : 'text-theme-secondary hover:text-theme-primary'
                    }`}
                  >
                    Unread
                  </button>
                </div>
              </div>
            )}
            
            {/* Categories Mode */}
            {organizationMode === 'categories' && (
              <>
                {categoriesWithUnread.length === 0 ? (
                  <div className="p-6 text-center">
                    <div className="text-4xl mb-3">📁</div>
                    <p className="text-sm text-theme-secondary">No categories yet</p>
                    <p className="text-xs text-theme-tertiary mt-1">Create them in Settings!</p>
                  </div>
                ) : (
                  <div className="flex-1 overflow-y-auto themed-scrollbar">
                    {/* All Items option */}
                    <button
                      onClick={() => setSelectedCategoryId(null)}
                      className={`w-full px-4 py-3 text-left transition-all duration-150 ${
                        !selectedCategoryId 
                          ? 'bg-theme-accent-light border-l-4 border-l-[rgb(var(--color-accent))]' 
                          : 'hover:bg-theme-hover border-l-4 border-l-transparent'
                      }`}
                    >
                      <div className="flex items-center justify-between">
                        <div className="flex items-center gap-2.5">
                          <span className="text-lg">📋</span>
                          <span className={`text-sm font-medium ${!selectedCategoryId ? 'text-theme-accent' : 'text-theme-primary'}`}>
                            All Categories
                          </span>
                        </div>
                      </div>
                    </button>
                    {categoriesWithUnread.map((cat: Category & { unreadCount: number; feedCount: number }) => (
                      <button
                        key={cat.id}
                        onClick={() => setSelectedCategoryId(cat.id)}
                        className={`w-full px-4 py-3 text-left transition-all duration-150 ${
                          selectedCategoryId === cat.id 
                            ? 'bg-theme-accent-light border-l-4 border-l-[rgb(var(--color-accent))]' 
                            : 'hover:bg-theme-hover border-l-4 border-l-transparent'
                        }`}
                      >
                        <div className="flex items-center justify-between">
                          <div className="flex items-center gap-2.5">
                            <span 
                              className="w-7 h-7 rounded-lg flex items-center justify-center text-base shadow-theme-sm"
                              style={{ backgroundColor: cat.color ?? 'rgb(var(--color-bg-tertiary))' }}
                            >
                              {cat.icon || '📁'}
                            </span>
                            <span className={`text-sm font-medium ${selectedCategoryId === cat.id ? 'text-theme-accent' : 'text-theme-primary'}`}>
                              {cat.name}
                            </span>
                          </div>
                          <div className="flex items-center gap-2 text-xs">
                            <span className="text-theme-tertiary">{cat.feedCount}</span>
                            {cat.unreadCount > 0 && (
                              <span className="unread-badge">
                                {cat.unreadCount > 99 ? '99+' : cat.unreadCount}
                              </span>
                            )}
                          </div>
                        </div>
                      </button>
                    ))}
                  </div>
                )}
              </>
            )}
            
            {/* Tags Mode */}
            {organizationMode === 'tags' && (
              <>
                {filteredTags.length === 0 ? (
                  <div className="p-6 text-center">
                    <div className="text-4xl mb-3">🏷️</div>
                    <p className="text-sm text-theme-secondary">
                      {selectedTags.length > 0 
                        ? 'No additional tags found'
                        : 'No tags yet'}
                    </p>
                    <p className="text-xs text-theme-tertiary mt-1">
                      {selectedTags.length > 0 
                        ? 'Try clearing your filter'
                        : 'Add tags when subscribing to feeds!'}
                    </p>
                  </div>
                ) : (
                  <div className="flex-1 overflow-y-auto themed-scrollbar">
                    {filteredTags.map(({ tag, unreadCount, feedCount }) => (
                      <button
                        key={tag}
                        onClick={() => handleToggleTag(tag)}
                        className={`w-full px-4 py-3 text-left transition-all duration-150 ${
                          selectedTags.includes(tag) 
                            ? 'bg-theme-accent-light border-l-4 border-l-[rgb(var(--color-accent))]' 
                            : 'hover:bg-theme-hover border-l-4 border-l-transparent'
                        }`}
                      >
                        <div className="flex items-center justify-between">
                          <div className="flex items-center gap-2.5">
                            <span className="text-base">🏷️</span>
                            <span className={`text-sm font-medium ${selectedTags.includes(tag) ? 'text-theme-accent' : 'text-theme-primary'}`}>
                              {tag}
                            </span>
                          </div>
                          <div className="flex items-center gap-2 text-xs">
                            <span className="text-theme-tertiary">{feedCount} feeds</span>
                            {unreadCount > 0 && (
                              <span className="unread-badge">
                                {unreadCount > 99 ? '99+' : unreadCount}
                              </span>
                            )}
                          </div>
                        </div>
                      </button>
                    ))}
                  </div>
                )}
              </>
            )}
          </div>
        )}

        {/* Favorites List */}
        {sidebarView === 'favorites' && (
          <div className="flex-1 overflow-y-auto themed-scrollbar">
            {favoritesLoading ? (
              <div className="p-6 space-y-3">
                {[1, 2, 3].map(i => (
                  <div key={i} className="space-y-2">
                    <div className="skeleton h-4 w-3/4" />
                    <div className="skeleton h-3 w-1/2" />
                  </div>
                ))}
              </div>
            ) : !favoritesData?.items || favoritesData.items.length === 0 ? (
              <div className="p-6 text-center">
                <div className="text-4xl mb-3">⭐</div>
                <p className="text-sm text-theme-secondary">No favorites yet</p>
                <p className="text-xs text-theme-tertiary mt-1">Star items to save them here!</p>
              </div>
            ) : (
              favoritesData.items.map((item: any) => (
                <div
                  key={item.id}
                  className={`w-full px-4 py-3 text-left transition-all duration-150 group ${
                    selectedItem === item.id 
                      ? 'bg-theme-accent-light border-l-4 border-l-[rgb(var(--color-accent))]' 
                      : 'hover:bg-theme-hover border-l-4 border-l-transparent'
                  }`}
                >
                  <div className="flex items-start gap-3">
                    <button
                      onClick={() => {
                        setSelectedItem(item.id)
                        setMobileView('content')
                      }}
                      className="flex-1 text-left min-w-0"
                    >
                      <div className="flex items-center gap-2 mb-1">
                        <span className="text-xs">⭐</span>
                        <span className="text-xs text-theme-tertiary truncate">
                          {item.feedTitle || 'Unknown Feed'}
                        </span>
                      </div>
                      <h3 className="text-sm font-medium text-theme-primary mb-1 line-clamp-2">
                        {item.title}
                      </h3>
                      {item.snippet && (
                        <p className="text-xs text-theme-secondary line-clamp-2">
                          {item.snippet}
                        </p>
                      )}
                    </button>
                    <button
                      onClick={(e) => {
                        e.stopPropagation()
                        handleToggleFavorite(item.id, true)
                      }}
                      className="opacity-0 group-hover:opacity-100 p-1.5 text-xs text-red-500 hover:bg-red-50 rounded-lg transition-all"
                      title="Remove from favorites"
                    >
                      <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                        <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M6 18L18 6M6 6l12 12" />
                      </svg>
                    </button>
                  </div>
                </div>
              ))
            )}
          </div>
        )}

        {/* Footer */}
        <div className="p-4 border-t border-theme-primary flex-shrink-0 space-y-2">
          {user?.npub === 'npub13hyx3qsqk3r7ctjqrr49uskut4yqjsxt8uvu4rekr55p08wyhf0qq90nt7' && (
            <button
              onClick={() => router.push('/admin')}
              className="w-full flex items-center gap-2 text-sm text-theme-secondary hover:text-theme-primary transition-colors"
            >
              <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M9 19v-6a2 2 0 00-2-2H5a2 2 0 00-2 2v6a2 2 0 002 2h2a2 2 0 002-2zm0 0V9a2 2 0 012-2h2a2 2 0 012 2v10m-6 0a2 2 0 002 2h2a2 2 0 002-2m0 0V5a2 2 0 012-2h2a2 2 0 012 2v14a2 2 0 01-2 2h-2a2 2 0 01-2-2z" />
              </svg>
              Admin Dashboard
            </button>
          )}
          <button
            onClick={handleSignOut}
            className="w-full flex items-center gap-2 text-sm text-theme-secondary hover:text-theme-primary transition-colors"
          >
            <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M17 16l4-4m0 0l-4-4m4 4H7m6 4v1a3 3 0 01-3 3H6a3 3 0 01-3-3V7a3 3 0 013-3h4a3 3 0 013 3v1" />
            </svg>
            Sign Out
          </button>
        </div>
      </div>

      {/* Center Panel - Article List */}
      <div className={`
        ${mobileView === 'content' && selectedItem ? 'hidden md:flex' : 'flex'}
        w-full md:w-96 bg-theme-surface border-r border-theme-primary flex-col max-h-screen
        pt-32 md:pt-0
      `}>
        <div className="p-5 border-b border-theme-primary flex-shrink-0">
          <div className="flex items-center justify-between mb-3">
            <h2 className="font-bold text-lg text-theme-primary">
              {selectedFeed === 'all' ? 'All Items' :
               allFeeds.find(f => f.id === selectedFeed)?.title || 'Select a feed'}
            </h2>
            
            {/* View Options Dropdown */}
            <div className="relative">
              <button
                onClick={(e) => {
                  e.stopPropagation()
                  setShowViewOptions(!showViewOptions)
                }}
                className="p-2 hover:bg-theme-hover rounded-lg text-theme-secondary hover:text-theme-primary transition-colors"
                title="View options"
              >
                <svg className="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                  <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M12 5v.01M12 12v.01M12 19v.01M12 6a1 1 0 110-2 1 1 0 010 2zm0 7a1 1 0 110-2 1 1 0 010 2zm0 7a1 1 0 110-2 1 1 0 010 2z" />
                </svg>
              </button>
              
              {/* Dropdown menu */}
              {showViewOptions && (
                <div className="absolute right-0 mt-2 w-48 dropdown-menu animate-slide-in">
                  <div className="py-1">
                    <div className="px-3 py-2 text-xs font-semibold text-theme-tertiary uppercase tracking-wider">Show</div>
                    <button
                      onClick={(e) => {
                        e.stopPropagation()
                        setViewFilter('all')
                        setShowViewOptions(false)
                      }}
                      className={`dropdown-item ${viewFilter === 'all' ? 'active' : ''}`}
                    >
                      {viewFilter === 'all' && '✓ '}All Items
                    </button>
                    <button
                      onClick={(e) => {
                        e.stopPropagation()
                        setViewFilter('unread')
                        setShowViewOptions(false)
                      }}
                      className={`dropdown-item ${viewFilter === 'unread' ? 'active' : ''}`}
                    >
                      {viewFilter === 'unread' && '✓ '}Unread Only
                    </button>
                    <button
                      onClick={(e) => {
                        e.stopPropagation()
                        setViewFilter('read')
                        setShowViewOptions(false)
                      }}
                      className={`dropdown-item ${viewFilter === 'read' ? 'active' : ''}`}
                    >
                      {viewFilter === 'read' && '✓ '}Read Only
                    </button>
                    
                    <div className="border-t border-theme-primary mt-1 pt-1">
                      <div className="px-3 py-2 text-xs font-semibold text-theme-tertiary uppercase tracking-wider">Sort</div>
                      <button
                        onClick={(e) => {
                          e.stopPropagation()
                          setSortOrder('newest')
                          setShowViewOptions(false)
                        }}
                        className={`dropdown-item ${sortOrder === 'newest' ? 'active' : ''}`}
                      >
                        {sortOrder === 'newest' && '✓ '}Newest First
                      </button>
                      <button
                        onClick={(e) => {
                          e.stopPropagation()
                          setSortOrder('oldest')
                          setShowViewOptions(false)
                        }}
                        className={`dropdown-item ${sortOrder === 'oldest' ? 'active' : ''}`}
                      >
                        {sortOrder === 'oldest' && '✓ '}Oldest First
                      </button>
                    </div>

                    <div className="border-t border-theme-primary mt-1 pt-1">
                      <div className="px-3 py-2 text-xs font-semibold text-theme-tertiary uppercase tracking-wider">Mark as read</div>
                      {QUICK_MARK_READ_OPTIONS.map(option => (
                        <button
                          key={option.value}
                          onClick={(e) => {
                            e.stopPropagation()
                            handleMarkReadBehaviorChange(option.value)
                            setShowViewOptions(false)
                          }}
                          className={`dropdown-item ${markReadBehavior === option.value ? 'active' : ''}`}
                        >
                          <div className="flex flex-col text-left">
                            <span>{markReadBehavior === option.value ? '✓ ' : ''}{option.label}</span>
                            <span className="text-[11px] text-theme-tertiary">{option.helper}</span>
                          </div>
                        </button>
                      ))}
                    </div>
                  </div>
                </div>
              )}
            </div>
          </div>
          
          <div className="flex items-center gap-2 text-sm text-theme-secondary">
            <span className="font-medium">{allFeedItems.filter(item => !item.isRead).length}</span>
            <span>unread</span>
            {viewFilter !== 'all' && (
              <span className="px-2 py-0.5 text-xs rounded-full bg-theme-accent-light text-theme-accent font-medium">
                {viewFilter === 'unread' ? 'Unread only' : 'Read only'}
              </span>
            )}
          </div>
        </div>

        <div className="flex-1 overflow-y-auto themed-scrollbar">
          {itemsLoading ? (
            <div className="p-6 space-y-4">
              {[1, 2, 3, 4].map(i => (
                <div key={i} className="space-y-2 p-4 border-b border-theme-secondary">
                  <div className="skeleton h-4 w-3/4" />
                  <div className="skeleton h-3 w-1/2" />
                  <div className="skeleton h-3 w-full" />
                  <div className="skeleton h-3 w-2/3" />
                </div>
              ))}
            </div>
          ) : feedItems.length === 0 ? (
            <div className="flex flex-col items-center justify-center p-8 text-center">
              <div className="text-5xl mb-4">📭</div>
              <p className="text-lg font-medium text-theme-primary mb-1">
                {viewFilter === 'unread' && 'All caught up!'}
                {viewFilter === 'read' && 'No read items'}
                {viewFilter === 'all' && 'No items yet'}
              </p>
              <p className="text-sm text-theme-tertiary">
                {viewFilter === 'unread' && 'No unread articles in this feed'}
                {viewFilter === 'read' && 'You haven\'t read any articles yet'}
                {viewFilter === 'all' && 'Subscribe to feeds to see content here'}
              </p>
            </div>
          ) : (
            feedItems.map((item: FeedItem) => (
              <div
                key={item.id}
                className={`article-card relative group ${
                  selectedItem === item.id ? 'active' : ''
                } ${item.isRead ? 'read' : ''}`}
              >
                <button
                  onClick={() => {
                    handleItemClick(item.id)
                    setMobileView('content')
                  }}
                  className="w-full pr-12 text-left"
                >
                  <div className="space-y-2">
                    <div className="flex items-start justify-between gap-2">
                      <h3 className={`text-sm font-semibold leading-snug ${
                        item.isRead ? 'text-theme-secondary' : 'text-theme-primary'
                      }`}>
                        {item.title}
                      </h3>
                      {!item.isRead && (
                        <div className="w-2 h-2 rounded-full bg-theme-accent flex-shrink-0 mt-1.5" />
                      )}
                    </div>
                    <div className="flex items-center gap-2 text-xs text-theme-tertiary">
                      <span className="font-medium">{item.author}</span>
                      <span>•</span>
                      <span>{item.publishedAt.toLocaleDateString()}</span>
                    </div>
                    <p className="text-xs text-theme-secondary line-clamp-2 leading-relaxed">
                      {item.content.replace(/<[^>]*>/g, '').substring(0, 140)}...
                    </p>
                    <div className="flex items-center gap-2 pt-1">
                      <span className="tag-badge">
                        {item.feedTitle}
                      </span>
                    </div>
                  </div>
                </button>
                <button
                  onClick={(e) => {
                    e.stopPropagation()
                    handleToggleFavorite(item.id, item.isFavorited || false)
                  }}
                  className="absolute top-4 right-4 p-2 rounded-lg opacity-0 group-hover:opacity-100 hover:bg-theme-hover transition-all"
                  title={item.isFavorited ? 'Remove from favorites' : 'Add to favorites'}
                >
                  <span className="text-xl">
                    {item.isFavorited ? '⭐' : '☆'}
                  </span>
                </button>
              </div>
            ))
          )}
        </div>
      </div>

      {/* Right Panel - Article Content */}
      <div className={`
        ${mobileView === 'list' && selectedItem ? 'hidden md:flex' : 'flex'}
        flex-1 bg-theme-secondary flex-col max-h-screen
        pt-32 md:pt-0
      `}>
        {/* Mobile Back Button */}
        {selectedItem && (
          <button
            onClick={() => setMobileView('list')}
            className="md:hidden fixed top-20 left-4 z-50 p-2.5 bg-theme-surface rounded-full shadow-theme-lg border border-theme-primary"
          >
            <svg className="w-5 h-5 text-theme-primary" fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M15 19l-7-7 7-7" />
            </svg>
          </button>
        )}
        
        {selectedItemData ? (
          <>
            <div className="bg-theme-surface-raised p-6 md:p-8 border-b border-theme-primary flex-shrink-0 shadow-theme-sm">
              <div className="max-w-3xl mx-auto">
                <div className="flex items-start justify-between gap-4 mb-4">
                  <h1 className="text-2xl md:text-3xl font-bold text-theme-primary leading-tight" style={{ fontFamily: 'var(--heading-font)' }}>
                    {selectedItemData.title}
                  </h1>
                  <div className="flex items-center gap-1 flex-shrink-0">
                    <button
                      onClick={() => handleToggleReadStatus(selectedItemData)}
                      className="btn-theme-secondary text-sm hidden sm:flex items-center gap-2"
                    >
                      {selectedItemData.isRead ? (
                        <>
                          <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M3 19v-8.93a2 2 0 01.89-1.664l7-4.666a2 2 0 012.22 0l7 4.666A2 2 0 0121 10.07V19M3 19a2 2 0 002 2h14a2 2 0 002-2M3 19l6.75-4.5M21 19l-6.75-4.5M3 10l6.75 4.5M21 10l-6.75 4.5m0 0l-1.14.76a2 2 0 01-2.22 0l-1.14-.76" />
                          </svg>
                          <span>Mark Unread</span>
                        </>
                      ) : (
                        <>
                          <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M5 13l4 4L19 7" />
                          </svg>
                          <span>Mark Read</span>
                        </>
                      )}
                    </button>
                    <button
                      onClick={() => handleShareToNostr(selectedItemData, selectedItemOriginalUrl)}
                      disabled={isSharing}
                      className="p-2.5 hover:bg-theme-hover rounded-lg transition-colors"
                      title="Share to Nostr"
                    >
                      {isSharing ? (
                        <svg className="w-5 h-5 text-theme-tertiary animate-spin" fill="none" viewBox="0 0 24 24">
                          <circle className="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="4" />
                          <path className="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z" />
                        </svg>
                      ) : shareSuccess ? (
                        <svg className="w-5 h-5 text-green-500" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M5 13l4 4L19 7" />
                        </svg>
                      ) : (
                        <svg className="w-5 h-5 text-theme-secondary" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M8.684 13.342C8.886 12.938 9 12.482 9 12c0-.482-.114-.938-.316-1.342m0 2.684a3 3 0 110-2.684m0 2.684l6.632 3.316m-6.632-6l6.632-3.316m0 0a3 3 0 105.367-2.684 3 3 0 00-5.367 2.684zm0 9.316a3 3 0 105.368 2.684 3 3 0 00-5.368-2.684z" />
                        </svg>
                      )}
                    </button>
                    <button
                      onClick={() => handleToggleFavorite(selectedItemData.id, selectedItemData.isFavorited || false)}
                      className="p-2.5 hover:bg-theme-hover rounded-lg transition-colors"
                      title={selectedItemData.isFavorited ? 'Remove from favorites' : 'Add to favorites'}
                    >
                      <span className="text-2xl">
                        {selectedItemData.isFavorited ? '⭐' : '☆'}
                      </span>
                    </button>
                  </div>
                </div>
                <div className="flex flex-wrap items-center gap-x-3 gap-y-1 text-sm text-theme-secondary">
                  <span className="font-medium">{selectedItemData.author}</span>
                  <span className="text-theme-muted">•</span>
                  <span>{selectedItemData.publishedAt.toLocaleDateString('en-US', { 
                    weekday: 'long', 
                    year: 'numeric', 
                    month: 'long', 
                    day: 'numeric' 
                  })}</span>
                  <span className="text-theme-muted">•</span>
                  <span className="tag-badge">{selectedItemData.feedTitle}</span>
                  {selectedItemOriginalUrl && (
                    <>
                      <span className="text-theme-muted">•</span>
                      <a
                        href={selectedItemOriginalUrl}
                        target="_blank"
                        rel="noopener noreferrer" 
                        className="inline-flex items-center gap-1 text-theme-accent hover:underline"
                      >
                        {selectedItemData.feedType === 'NOSTR' || selectedItemData.feedType === 'NOSTR_VIDEO' ? 'View on Nostr' : 'View Original'}
                        <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M10 6H6a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2v-4M14 4h6m0 0v6m0-6L10 14" />
                        </svg>
                      </a>
                    </>
                  )}
                </div>
              </div>
            </div>
            <div className="flex-1 overflow-y-auto themed-scrollbar">
              <div className="max-w-3xl mx-auto p-6 md:p-8">
                <article className="article-content-inner p-6 md:p-10 rounded-xl">
                  <FormattedContent 
                    content={selectedItemData.content}
                    embedUrl={selectedItemData.embedUrl ?? undefined}
                    thumbnail={selectedItemData.thumbnail ?? undefined}
                    title={selectedItemData.title}
                    className="prose-theme"
                  />
                </article>
              </div>
            </div>
          </>
        ) : (
          <div className="flex-1 flex items-center justify-center">
            <div className="text-center">
              <div className="text-6xl mb-4 opacity-50">📖</div>
              <p className="text-xl font-medium text-theme-secondary mb-2">Select an article to read</p>
              <p className="text-sm text-theme-tertiary">Choose from the list on the left</p>
            </div>
          </div>
        )}
      </div>

      {/* Enhanced Add Feed Modal */}
      <AddFeedModal
        isOpen={showAddFeed}
        onClose={() => {
          setShowAddFeed(false)
          setFeedError('')
        }}
        onAddFeed={handleAddFeed}
        isLoading={subscribeFeedMutation.isLoading}
        error={feedError}
        organizationMode={organizationMode}
        categories={categories}
      />

      {/* Settings Dialog */}
      <SettingsDialog
        isOpen={showSettings}
        onClose={() => setShowSettings(false)}
        markReadBehavior={markReadBehavior}
        onChangeMarkReadBehavior={handleMarkReadBehaviorChange}
        organizationMode={organizationMode}
        onChangeOrganizationMode={handleOrganizationModeChange}
        feeds={feeds.map((f: Feed) => ({
          type: f.type,
          url: f.url || f.npub || '',
          tags: f.tags,
          category: f.category
        }))}
        userPubkey={user?.npub || user?.pubkey}
        onImportFeeds={handleImportFeeds}
      />

      {/* Sync Prompt Dialog */}
      {showSyncPrompt && pendingSyncImport && (
        <div className="modal-overlay">
          <div className="modal-content w-96 max-w-[90vw] max-h-[80vh] flex flex-col animate-slide-in">
            <div className="p-6 border-b border-theme-primary">
              <h2 className="text-xl font-bold text-theme-primary flex items-center gap-2">
                <span className="text-2xl">📡</span> Sync Available
              </h2>
            </div>
            <div className="p-6 overflow-y-auto themed-scrollbar">
              <p className="text-theme-secondary mb-4">
                Found <span className="font-semibold text-theme-primary">{pendingSyncImport.length}</span> subscription(s) from another device. Would you like to import them?
              </p>
              <ul className="text-sm text-theme-tertiary mb-4 max-h-32 overflow-y-auto space-y-1.5 bg-theme-tertiary rounded-lg p-3">
                {pendingSyncImport.map((feed, i) => (
                  <li key={i} className="truncate flex items-center gap-2">
                    <span className="text-xs">{feed.type === 'RSS' ? '📰' : '⚡'}</span>
                    <span className="truncate">{feed.url}</span>
                  </li>
                ))}
              </ul>
            </div>
            <div className="p-6 border-t border-theme-primary flex gap-3 justify-end">
              <button
                onClick={() => {
                  setShowSyncPrompt(false)
                  setPendingSyncImport(null)
                }}
                className="btn-theme-secondary"
              >
                Not Now
              </button>
              <button
                onClick={async () => {
                  await handleImportFeeds(pendingSyncImport)
                  setShowSyncPrompt(false)
                  setPendingSyncImport(null)
                }}
                className="btn-theme-primary"
              >
                Import All
              </button>
            </div>
          </div>
        </div>
      )}

      {/* Edit Tags Dialog */}
      {editingFeedId && (
        <div className="modal-overlay">
          <div className="modal-content w-96 max-h-[80vh] flex flex-col animate-slide-in">
            <div className="p-6 border-b border-theme-primary">
              <h2 className="text-xl font-bold text-theme-primary flex items-center gap-2">
                <span className="text-xl">🏷️</span> Edit Tags
              </h2>
            </div>
            
            <div className="p-6 flex-1 overflow-y-auto themed-scrollbar">
              <div className="mb-4">
                <label className="block text-sm font-medium text-theme-secondary mb-2">
                  Add tags to organize this feed
                </label>
                <div className="flex gap-2">
                  <input
                    type="text"
                    value={editTagInput}
                    onChange={(e) => setEditTagInput(e.target.value)}
                    onKeyDown={(e) => {
                      if (e.key === 'Enter') {
                        e.preventDefault()
                        handleAddEditTag()
                      }
                    }}
                    placeholder="Type a tag..."
                    className="input-theme flex-1"
                  />
                  <button
                    type="button"
                    onClick={handleAddEditTag}
                    className="btn-theme-secondary"
                  >
                    Add
                  </button>
                </div>
              </div>
              
              {editTags.length > 0 && (
                <div className="flex flex-wrap gap-2">
                  {editTags.map((tag: string) => (
                    <span
                      key={tag}
                      className="inline-flex items-center px-3 py-1.5 bg-theme-accent-light text-theme-accent rounded-full text-sm font-medium"
                    >
                      {tag}
                      <button
                        type="button"
                        onClick={() => handleRemoveEditTag(tag)}
                        className="ml-2 hover:text-theme-accent-hover"
                      >
                        ×
                      </button>
                    </span>
                  ))}
                </div>
              )}
              
              {editTags.length === 0 && (
                <div className="text-center py-6 text-theme-tertiary">
                  <div className="text-3xl mb-2">🏷️</div>
                  <p className="text-sm">No tags yet. Add some above!</p>
                </div>
              )}
            </div>

            <div className="flex justify-end gap-3 p-6 border-t border-theme-primary">
              <button
                onClick={handleCancelEdit}
                className="btn-theme-secondary"
              >
                Cancel
              </button>
              <button
                onClick={() => handleSaveEditedTags(editingFeedId)}
                disabled={updateTagsMutation.isLoading}
                className="btn-theme-primary disabled:opacity-50"
              >
                {updateTagsMutation.isLoading ? 'Saving...' : 'Save Tags'}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}