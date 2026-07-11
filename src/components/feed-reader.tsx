'use client'

import { useNostrAuth, getLastSigningError } from '@/contexts/NostrAuthContext'
import { useTheme, themeConfig } from '@/contexts/ThemeContext'
import { ThemeToggleButton } from './theme-selector'
import { useState, useEffect, useRef, useMemo, useCallback } from 'react'
import { useRouter } from 'next/navigation'
import { api } from '@/trpc/react'
import { env } from '@/env.mjs'
import { AddFeedModal } from './add-feed-modal'
import { SettingsDialog, MarkReadBehavior, LayoutMode, OrganizationMode } from './settings-dialog'
import { useAiConfig } from '@/lib/ai/config'
import { applyFilters, loadFilterRules, type FilterRule } from '@/lib/keyword-filter'
import {
  loadViews,
  saveViews,
  loadActiveViewId,
  saveActiveViewId,
  newViewId,
  matchesView,
  reorderViews,
  mergeViewLists,
  type SavedView,
  type ViewSource,
} from '@/lib/saved-views'
import { recordRead, searchHistory, clearHistory, type HistoryRecord } from '@/lib/reading-history'
import { SimplePool } from 'nostr-tools'
import { 
  fetchSubscriptionList,
  mergeSubscriptionLists,
  getLastSyncTime,
  isSyncEventFresh,
  advanceSyncWatermarkIfFresh,
  publishSubscriptionList,
  buildSubscriptionListFromFeeds,
  fetchViewList,
  publishViewList,
} from '@/lib/nostr-sync'
import type { UnsignedEvent } from 'nostr-tools'
import { ArticlePane } from './reader/ArticlePane'
import { ItemList } from './reader/ItemList'
import { ReaderSidebar } from './reader/ReaderSidebar'
import type { Category, Feed, FeedItem, FavoriteItem } from './reader/types'

const FAVORITES_QUERY_INPUT = { limit: 50 } as const
const QUICK_MARK_READ_OPTIONS: { value: MarkReadBehavior; label: string; helper: string }[] = [
  { value: 'on-open', label: 'On open', helper: 'Mark as soon as I open the story' },
  { value: 'after-10s', label: 'After 10 seconds', helper: 'Give me a short buffer before marking read' },
  { value: 'never', label: 'Never automatically', helper: 'Only change when I click Mark as Read' },
]

export function FeedReader() {
  const { user, disconnect, canSign, signEventOrThrow } = useNostrAuth()
  const { theme } = useTheme()
  const router = useRouter()
  const utils = api.useUtils()
  
  const [selectedFeed, setSelectedFeed] = useState<string | null>('all')
  const [selectedItem, setSelectedItem] = useState<string | null>(null)
  const [showAddFeed, setShowAddFeed] = useState(false)
  const [showSettings, setShowSettings] = useState(false)
  const [feedError, setFeedError] = useState<string>('')
  const [sidebarView, setSidebarView] = useState<'feeds' | 'tags' | 'favorites' | 'history'>('feeds')
  const [historyQuery, setHistoryQuery] = useState('')
  const [historyResults, setHistoryResults] = useState<HistoryRecord[]>([])
  const [selectedHistoryItem, setSelectedHistoryItem] = useState<HistoryRecord | null>(null)
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
  const [layoutMode, setLayoutMode] = useState<LayoutMode>('split')
  const { config: aiConfig } = useAiConfig()
  const aiEnabled = aiConfig.enabled && (aiConfig.features.summarize || aiConfig.features.insights || aiConfig.features.translate)
  const [showAiPanel, setShowAiPanel] = useState(false)
  const [filterRules, setFilterRules] = useState<FilterRule[]>([])
  const [showHiddenByFilter, setShowHiddenByFilter] = useState(false)
  const [views, setViews] = useState<SavedView[]>([])
  const [activeViewId, setActiveViewId] = useState<string | null>(null)
  const [showSaveViewModal, setShowSaveViewModal] = useState(false)
  const [newViewName, setNewViewName] = useState('')
  const [newViewIcon, setNewViewIcon] = useState('')
  const [isSharing, setIsSharing] = useState(false)
  const [shareSuccess, setShareSuccess] = useState(false)
  const [showSyncPrompt, setShowSyncPrompt] = useState(false)
  const [pendingSyncImport, setPendingSyncImport] = useState<Array<{ type: 'RSS' | 'NOSTR'; url: string; tags?: string[] }> | null>(null)
  const [pendingSyncCreatedAt, setPendingSyncCreatedAt] = useState<number | null>(null)
  const [isRefreshingAll, setIsRefreshingAll] = useState(false)
  const [lastRefreshTime, setLastRefreshTime] = useState<number | null>(null)
  const [selectedCategoryId, setSelectedCategoryId] = useState<string | null>(null)
  const markReadTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const viewsExportTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const autoMarkReadTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const autoMarkedRef = useRef<Set<string>>(new Set())
  const hasCheckedSyncRef = useRef(false)
  const hasSyncedViewsRef = useRef(false)
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

  const handleOrganizationModeChangeRef = useRef(handleOrganizationModeChange)
  const organizationModeRef = useRef(organizationMode)
  useEffect(() => {
    handleOrganizationModeChangeRef.current = handleOrganizationModeChange
    organizationModeRef.current = organizationMode
  })
  
  // Categories query
  const { data: categories = [] } = api.feed.getCategories.useQuery(undefined, {
    enabled: !!user?.npub,
  })
  
  const { data: categoriesWithUnread = [] } = api.feed.getCategoriesWithUnread.useQuery(undefined, {
    enabled: !!user?.npub && organizationMode === 'categories',
  })

  useEffect(() => {
    if (typeof window === 'undefined') return
    const storedLayout = localStorage.getItem('readstr_layout')
    if (storedLayout === 'split' || storedLayout === 'single' || storedLayout === 'grid') {
      setLayoutMode(storedLayout)
    }
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
    setFilterRules(loadFilterRules())
  }, [])

  useEffect(() => {
    setShowHiddenByFilter(false)
  }, [selectedFeed, viewFilter])

  const handleMarkReadBehaviorChange = (behavior: MarkReadBehavior) => {
    setMarkReadBehavior(behavior)
    if (typeof window !== 'undefined') {
      localStorage.setItem('mark_read_behavior', behavior)
    }
  }

  const handleLayoutModeChange = (mode: LayoutMode) => {
    setLayoutMode(mode)
    if (typeof window !== 'undefined') {
      localStorage.setItem('readstr_layout', mode)
    }
  }

  // Sign out handler
  const handleSignOut = async () => {
    try {
      await clearHistory()
    } catch {
    } finally {
      disconnect()
      router.push('/')
    }
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
  
  const { data: feedsData = [], isLoading: isFeedsLoading, isFetched: isFeedsFetched, error: feedsQueryError } = api.feed.getFeeds.useQuery(
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
      // If the feeds query failed (e.g. auth failure), local state is unknown —
      // don't offer an import that can't be persisted anyway. Leave the ref
      // unset so the check re-runs once the query recovers.
      if (feedsQueryError) return
      hasCheckedSyncRef.current = true

      // Skip if synced recently (within last hour)
      const lastSync = getLastSyncTime()
      if (lastSync && Date.now() / 1000 - lastSync < 3600) return

      try {
        const result = await fetchSubscriptionList(user.npub)
        if (!result.success || !result.data) return

        // Ignore stale events: a relay must not roll back state with an
        // equal-or-older subscription list than the one we last applied. Skip this
        // gate when there are no local feeds — nothing applied locally to protect,
        // so a stale/persisted watermark must not leave the device silently empty.
        if (feeds.length > 0 && !isSyncEventFresh('readstr-subscriptions', result.createdAt)) return

        // Nothing remote to import: local already reflects this event, so accept
        // it as the new freshness basis to avoid re-evaluating it forever.
        if (result.data.rss.length === 0 && result.data.nostr.length === 0) {
          advanceSyncWatermarkIfFresh('readstr-subscriptions', result.createdAt)
          return
        }

        const currentFeeds = feeds.map((f: Feed) => ({
          type: f.type,
          url: f.url || f.npub || '',
          tags: f.tags,
        }))

        const mergeResult = mergeSubscriptionLists(currentFeeds, result.data)

        if (mergeResult.toAdd.length > 0) {
          // Defer the watermark advance to the accept handler so a dismissed
          // import can still be re-imported later.
          setPendingSyncImport(mergeResult.toAdd)
          setPendingSyncCreatedAt(result.createdAt!)
          setShowSyncPrompt(true)
        } else {
          // Everything remote is already subscribed locally; safe to advance.
          advanceSyncWatermarkIfFresh('readstr-subscriptions', result.createdAt)
        }
      } catch (error) {
        console.error('Auto-sync check failed:', error)
      }
    }
    
    checkRemoteSync()
    // 'feeds' is derived from feedsData each render (new reference); depending on
    // feedsData here is the stable proxy and the check is guarded to run once per session.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [user?.npub, feedsData, isFeedsFetched, isFeedsLoading, feedsQueryError])
  
  const { data: userTags = [] } = api.feed.getUserTags.useQuery(undefined, {
    enabled: !!user && !!user.npub,
  })
  
  // Favorites query
  const { data: favoritesData, isLoading: favoritesLoading } = api.feed.getFavorites.useQuery(
    FAVORITES_QUERY_INPUT,
    { enabled: !!user && !!user.npub && sidebarView === 'favorites' }
  )

  // Reading history query (client-side, offline)
  useEffect(() => {
    if (sidebarView !== 'history') return
    let cancelled = false
    const run = () => {
      searchHistory(historyQuery)
        .then((results) => {
          if (!cancelled) setHistoryResults(results)
        })
        .catch(() => {})
    }
    const delay = historyQuery.trim() ? 200 : 0
    const timer = setTimeout(run, delay)
    return () => {
      cancelled = true
      clearTimeout(timer)
    }
  }, [sidebarView, historyQuery])

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
  const allFeedItems = useMemo(() => feedItemsData?.items ?? [], [feedItemsData])

  const activeView = activeViewId ? views.find((v) => v.id === activeViewId) : undefined

  // Apply read/unread filter and local keyword filter rules (client-side only)
  const { items: filteredItems, hiddenCount: hiddenByFilterCount, outcomes: filterOutcomes } = useMemo(() => {
    let items = allFeedItems as FeedItem[]
    if (viewFilter === 'unread') {
      items = items.filter((item) => !item.isRead)
    } else if (viewFilter === 'read') {
      items = items.filter((item) => item.isRead)
    }
    const result = applyFilters(items, filterRules, showHiddenByFilter)
    if (activeView?.keywords) {
      return { ...result, items: result.items.filter((item) => matchesView(item, activeView)) }
    }
    return result
  }, [allFeedItems, viewFilter, filterRules, showHiddenByFilter, activeView])

  let feedItems = filteredItems

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

  useEffect(() => {
    autoMarkedRef.current.clear()
  }, [filterRules])

  // Auto-mark-read filter action: applied post-render (never in the pure
  // predicate), debounced to coalesce. Marking syncs to a public Nostr event,
  // so this only runs for rules the user explicitly opted into.
  useEffect(() => {
    if (autoMarkReadTimerRef.current) {
      clearTimeout(autoMarkReadTimerRef.current)
      autoMarkReadTimerRef.current = null
    }
    const pending = filteredItems.filter(
      (item) =>
        filterOutcomes.get(item.id)?.markRead &&
        !item.isRead &&
        !autoMarkedRef.current.has(item.id)
    )
    if (pending.length === 0) return
    autoMarkReadTimerRef.current = setTimeout(() => {
      for (const item of pending) {
        if (autoMarkedRef.current.has(item.id)) continue
        autoMarkedRef.current.add(item.id)
        markAsReadMutation.mutate({ itemId: item.id })
      }
    }, 500)
    return () => {
      if (autoMarkReadTimerRef.current) {
        clearTimeout(autoMarkReadTimerRef.current)
        autoMarkReadTimerRef.current = null
      }
    }
  }, [filteredItems, filterOutcomes, markAsReadMutation])

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
  const recordReadHistory = (item: {
    id: string
    title: string
    content: string
    author: string | null
    feedTitle: string
    url?: string | null
    originalUrl?: string | null
    feedType: string
  }) => {
    recordRead({
      id: item.id,
      title: item.title,
      content: item.content,
      author: item.author,
      feedTitle: item.feedTitle,
      url: item.originalUrl ?? item.url,
      feedType: item.feedType,
    }).catch(() => {})
  }

  const handleItemClick = (itemId: string) => {
    setSelectedItem(itemId)
    setSelectedHistoryItem(null)
    const item = allFeedItems.find((i: FeedItem) => i.id === itemId)
    if (item) {
      recordReadHistory(item)
    }
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
    if (!canSign || !user?.pubkey) {
      alert('Connect with a Nostr signer (browser extension or remote signer) to share posts.')
      return
    }

    setIsSharing(true)
    setShareSuccess(false)

    try {
      // Build the share URL - for Nostr posts use habla.news, otherwise use original URL
      const shareUrl = originalUrl || item.url || ''

      // Create the note content with attribution
      const noteContent = `📖 ${item.title}\n\n${shareUrl}\n\n— shared from readstr.privkey.io`

      const unsignedEvent: UnsignedEvent = {
        kind: 1,
        pubkey: user.pubkey,
        created_at: Math.floor(Date.now() / 1000),
        tags: [] as string[][],
        content: noteContent,
      }

      const signedEvent = await signEventOrThrow(unsignedEvent)

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
    if (!canSign || !user?.npub) {
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
  }, [user?.npub, canSign, signEventOrThrow, utils.feed])

  // Auto-export saved views to Nostr (kind 30406) after changes
  const autoExportViewsToNostr = useCallback(async (viewsToExport: SavedView[]) => {
    if (!canSign || !user?.npub) return
    try {
      const result = await publishViewList(viewsToExport, signEventOrThrow)
      if (!result.success) {
        console.warn('⚠️ Views auto-export failed:', result.error)
      }
    } catch {
      // Silently fail - don't interrupt user experience
    }
  }, [user?.npub, canSign, signEventOrThrow])

  // Views sync (kind 30406): only for signers. Read-only npub stays purely local.
  // Runs once per session but guards before setting the ref so a canSign false→true
  // transition still triggers the one-time sync.
  useEffect(() => {
    if (hasSyncedViewsRef.current) return
    if (!user?.npub || !canSign) return
    hasSyncedViewsRef.current = true

    const syncViews = async () => {
      try {
        const viewsResult = await fetchViewList(user.npub)
        if (!viewsResult.success || !viewsResult.data) return
        const remoteViews = viewsResult.data.views
        const hasRemoteEvent = viewsResult.createdAt != null

        if (hasRemoteEvent && isSyncEventFresh('readstr-views', viewsResult.createdAt)) {
          const merged = mergeViewLists(loadViews(), remoteViews)
          saveViews(merged)
          setViews(loadViews())
          advanceSyncWatermarkIfFresh('readstr-views', viewsResult.createdAt)
        }

        // Publish local additions the remote lacks, or seed the first event when
        // none exists. One-shot; do not advance the watermark here (next login
        // converges) and only publish on a real delta so this can't loop.
        const localViews = loadViews()
        const remoteIds = new Set(remoteViews.map((v) => v.id))
        const hasLocalOnly = localViews.some((v) => !remoteIds.has(v.id))
        if (localViews.length > 0 && (!hasRemoteEvent || hasLocalOnly)) {
          void autoExportViewsToNostr(localViews)
        }
      } catch (error) {
        console.error('Views sync check failed:', error)
      }
    }
    syncViews()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [user?.npub, canSign])

  // Handle importing feeds from Nostr sync
  const handleImportFeeds = async (feedsToImport: Array<{ type: 'RSS' | 'NOSTR'; url: string; tags?: string[]; category?: { name: string; color?: string; icon?: string } }>) => {
    // First, create a map of category names to IDs for existing categories
    const categoryMap = new Map<string, string>()
    for (const cat of categories) {
      categoryMap.set(cat.name, cat.id)
    }

    const failures: Array<{ url: string; message: string }> = []

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
        failures.push({
          url: feed.url,
          message: error instanceof Error ? error.message : String(error),
        })
      }
    }

    // Surface failures instead of silently pretending the import succeeded —
    // otherwise an auth failure looks like a successful import of nothing.
    if (failures.length > 0) {
      const detail = failures[0]!.message
      alert(
        `${failures.length} of ${feedsToImport.length} subscription(s) failed to import.\n\n` +
        `First error: ${detail}\n\n` +
        `If this mentions authentication, your signer may not be approving requests — check your signer app and try again.`
      )
      throw Object.assign(
        new Error(`Import failed for ${failures.length} of ${feedsToImport.length} feeds: ${detail}`),
        { importFailure: { failed: failures.length, total: feedsToImport.length } }
      )
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

  const clearActiveView = () => {
    setActiveViewId(null)
    saveActiveViewId(null)
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
    clearActiveView()
  }

  const handleClearTags = () => {
    clearActiveView()
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

  // Saved views (localStorage only)
  const applyView = useCallback((view: SavedView) => {
    const source = view.source
    switch (source.kind) {
      case 'all':
        setSidebarView('feeds')
        setSelectedFeed('all')
        setSelectedTags([])
        setSelectedCategoryId(null)
        break
      case 'feed':
        setSidebarView('feeds')
        setSelectedFeed(source.feedId)
        setSelectedTags([])
        setSelectedCategoryId(null)
        break
      case 'tags':
        setSidebarView('tags')
        setSelectedTags(source.tags)
        setSelectedFeed('all')
        setSelectedCategoryId(null)
        break
      case 'category':
        setSidebarView('tags')
        setSelectedCategoryId(source.categoryId)
        setSelectedFeed('all')
        setSelectedTags([])
        break
      case 'favorites':
        setSidebarView('favorites')
        setSelectedTags([])
        setSelectedCategoryId(null)
        break
    }
    if (
      view.organizationMode &&
      (source.kind === 'tags' || source.kind === 'category') &&
      view.organizationMode !== organizationModeRef.current
    ) {
      handleOrganizationModeChangeRef.current(view.organizationMode)
    }
    setViewFilter(view.readState)
    setSortOrder(view.sort)
    setActiveViewId(view.id)
    saveActiveViewId(view.id)
  }, [])

  useEffect(() => {
    const loaded = loadViews()
    setViews(loaded)
    const active = loadActiveViewId()
    const match = active ? loaded.find((v) => v.id === active) : undefined
    if (match) applyView(match)
    else setActiveViewId(null)
  }, [applyView])

  useEffect(() => {
    return () => {
      if (viewsExportTimerRef.current) {
        clearTimeout(viewsExportTimerRef.current)
        viewsExportTimerRef.current = null
      }
    }
  }, [])

  const persistViews = (next: SavedView[]) => {
    saveViews(next)
    const saved = loadViews()
    setViews(saved)
    if (viewsExportTimerRef.current) {
      clearTimeout(viewsExportTimerRef.current)
    }
    viewsExportTimerRef.current = setTimeout(() => {
      void autoExportViewsToNostr(saved)
    }, 500)
  }

  const captureCurrentSource = (): ViewSource => {
    if (sidebarView === 'favorites') return { kind: 'favorites' }
    if (organizationMode === 'categories' && selectedCategoryId) {
      return { kind: 'category', categoryId: selectedCategoryId }
    }
    if (selectedTags.length > 0) return { kind: 'tags', tags: [...selectedTags] }
    if (selectedFeed && selectedFeed !== 'all') return { kind: 'feed', feedId: selectedFeed }
    return { kind: 'all' }
  }

  const handleSaveCurrentView = () => {
    const name = newViewName.trim()
    if (!name) return
    const source = captureCurrentSource()
    const view: SavedView = {
      id: newViewId(),
      name,
      order: views.length,
      source,
      readState: viewFilter,
      sort: sortOrder,
    }
    if (source.kind === 'tags' || source.kind === 'category') {
      view.organizationMode = organizationMode
    }
    if (newViewIcon.trim()) view.icon = newViewIcon.trim()
    persistViews([...views, view])
    setActiveViewId(view.id)
    saveActiveViewId(view.id)
    setShowSaveViewModal(false)
    setNewViewName('')
    setNewViewIcon('')
  }

  const handleRenameView = (id: string, name: string) => {
    persistViews(views.map((v) => (v.id === id ? { ...v, name } : v)))
  }

  const handleDeleteView = (id: string) => {
    persistViews(views.filter((v) => v.id !== id))
    if (activeViewId === id) clearActiveView()
  }

  const handleMoveView = (index: number, direction: -1 | 1) => {
    const next = reorderViews(views, index, direction)
    if (next === views) return
    persistViews(next)
  }

  return (
    <div className="flex h-screen bg-theme-secondary">
      {/* Subscriptions failed to load (e.g. signer auth failure) — without this
          banner an auth problem renders as a silently empty reader. */}
      {feedsQueryError && !showAddFeed && !showSettings && !showSyncPrompt && !editingFeedId && (
        <div className="fixed top-16 md:top-0 left-0 right-0 z-[60] bg-red-600 text-white text-sm px-4 py-2 flex items-center justify-between gap-3">
          <span className="min-w-0 break-words">
            {feedsQueryError.data?.code === 'UNAUTHORIZED'
              ? `Could not authenticate with your signer — subscriptions can't be loaded or saved.${
                  getLastSigningError() ? ` Signing error: ${getLastSigningError()}.` : ''
                } Check your signer app, then retry.`
              : `Failed to load subscriptions: ${feedsQueryError.message}`}
          </span>
          <button
            onClick={() => invalidateFeedData()}
            className="flex-shrink-0 underline font-semibold"
          >
            Retry
          </button>
        </div>
      )}
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
                clearActiveView()
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
                clearActiveView()
              }}
              className={`flex-1 px-3 py-2 text-sm rounded-lg font-medium transition-all duration-200 ${
                sidebarView === 'favorites'
                  ? 'bg-theme-surface shadow-theme-sm text-theme-primary'
                  : 'text-theme-secondary hover:text-theme-primary'
              }`}
            >
              ⭐ Saved
            </button>
            <button
              onClick={() => {
                setSidebarView('history')
                setSelectedTags([])
                setShowSidebar(true)
                clearActiveView()
              }}
              className={`flex-1 px-3 py-2 text-sm rounded-lg font-medium transition-all duration-200 ${
                sidebarView === 'history'
                  ? 'bg-theme-surface shadow-theme-sm text-theme-primary'
                  : 'text-theme-secondary hover:text-theme-primary'
              }`}
            >
              📖 History
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
      <ReaderSidebar
        showSidebar={showSidebar}
        onRefreshAllFeeds={handleRefreshAllFeeds}
        isRefreshingAll={isRefreshingAll}
        lastRefreshTime={lastRefreshTime}
        setShowSettings={setShowSettings}
        setShowAddFeed={setShowAddFeed}
        user={user}
        sidebarView={sidebarView}
        setSidebarView={setSidebarView}
        setSelectedTags={setSelectedTags}
        selectedCategoryId={selectedCategoryId}
        setSelectedCategoryId={setSelectedCategoryId}
        onClearActiveView={clearActiveView}
        organizationMode={organizationMode}
        selectedTags={selectedTags}
        onClearTags={handleClearTags}
        onToggleTag={handleToggleTag}
        allFeeds={allFeeds}
        selectedFeed={selectedFeed}
        setSelectedFeed={setSelectedFeed}
        setShowSidebar={setShowSidebar}
        openMenuFeedId={openMenuFeedId}
        setOpenMenuFeedId={setOpenMenuFeedId}
        showCategoryPicker={showCategoryPicker}
        setShowCategoryPicker={setShowCategoryPicker}
        onRefreshFeed={handleRefreshFeed}
        refreshFeedLoading={refreshFeedMutation.isLoading}
        refreshNostrFeedLoading={refreshNostrFeedMutation.isLoading}
        onMarkAllAsRead={handleMarkAllAsRead}
        markAllAsReadLoading={markAllAsReadMutation.isLoading}
        categories={categories}
        feeds={feeds}
        onOpenEditMenu={handleOpenEditMenu}
        onSetCategory={(feedId, categoryId) => updateCategoryMutation.mutate({ feedId, categoryId })}
        onRemoveFeed={handleRemoveFeed}
        categoriesWithUnread={categoriesWithUnread}
        tagSortOrder={tagSortOrder}
        setTagSortOrder={setTagSortOrder}
        filteredTags={filteredTags}
        favoritesLoading={favoritesLoading}
        favoritesData={favoritesData}
        selectedItem={selectedItem}
        setSelectedItem={setSelectedItem}
        setSelectedHistoryItem={setSelectedHistoryItem}
        setMobileView={setMobileView}
        onRecordReadHistory={recordReadHistory}
        onToggleFavorite={handleToggleFavorite}
        historyQuery={historyQuery}
        setHistoryQuery={setHistoryQuery}
        historyResults={historyResults}
        selectedHistoryItem={selectedHistoryItem}
        onNavigateAdmin={() => router.push('/admin')}
        onSignOut={handleSignOut}
      />

      {/* Center Panel - Article List */}
      <ItemList
        layoutMode={layoutMode}
        mobileView={mobileView}
        selectedItem={selectedItem}
        selectedHistoryItem={selectedHistoryItem}
        selectedFeed={selectedFeed}
        allFeeds={allFeeds}
        showViewOptions={showViewOptions}
        setShowViewOptions={setShowViewOptions}
        viewFilter={viewFilter}
        setViewFilter={setViewFilter}
        sortOrder={sortOrder}
        setSortOrder={setSortOrder}
        markReadBehavior={markReadBehavior}
        onMarkReadBehaviorChange={handleMarkReadBehaviorChange}
        quickMarkReadOptions={QUICK_MARK_READ_OPTIONS}
        onClearActiveView={clearActiveView}
        views={views}
        activeViewId={activeViewId}
        onApplyView={applyView}
        onSaveCurrent={() => {
          setNewViewName('')
          setNewViewIcon('')
          setShowSaveViewModal(true)
        }}
        allFeedItems={allFeedItems}
        hiddenByFilterCount={hiddenByFilterCount}
        showHiddenByFilter={showHiddenByFilter}
        setShowHiddenByFilter={setShowHiddenByFilter}
        itemsLoading={itemsLoading}
        feedItems={feedItems}
        filterOutcomes={filterOutcomes}
        onItemClick={handleItemClick}
        setMobileView={setMobileView}
        onToggleFavorite={handleToggleFavorite}
      />

      {/* Right Panel - Article Content */}
      <ArticlePane
        layoutMode={layoutMode}
        mobileView={mobileView}
        selectedItem={selectedItem}
        selectedHistoryItem={selectedHistoryItem}
        selectedItemData={selectedItemData}
        selectedItemOriginalUrl={selectedItemOriginalUrl}
        setMobileView={setMobileView}
        onToggleReadStatus={handleToggleReadStatus}
        onShareToNostr={handleShareToNostr}
        isSharing={isSharing}
        shareSuccess={shareSuccess}
        onToggleFavorite={handleToggleFavorite}
        aiEnabled={aiEnabled}
        showAiPanel={showAiPanel}
        setShowAiPanel={setShowAiPanel}
        aiConfig={aiConfig}
      />

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

      {/* Save Current View Modal */}
      {showSaveViewModal && (
        <div className="modal-overlay">
          <div className="modal-content w-96 max-w-[90vw] animate-slide-in">
            <div className="p-6 border-b border-theme-primary">
              <h2 className="text-xl font-bold text-theme-primary">Save current as view</h2>
            </div>
            <div className="p-6 space-y-4">
              <div>
                <label className="block text-xs text-theme-tertiary mb-1 uppercase tracking-wider">Name</label>
                <input
                  type="text"
                  value={newViewName}
                  autoFocus
                  onChange={(e) => setNewViewName(e.target.value)}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter') handleSaveCurrentView()
                  }}
                  placeholder="e.g. Unread tech"
                  maxLength={60}
                  className="input-theme w-full"
                />
              </div>
              <div>
                <label className="block text-xs text-theme-tertiary mb-1 uppercase tracking-wider">Emoji (optional)</label>
                <input
                  type="text"
                  value={newViewIcon}
                  onChange={(e) => setNewViewIcon(e.target.value)}
                  placeholder="⭐"
                  maxLength={8}
                  className="input-theme w-24"
                />
              </div>
            </div>
            <div className="p-6 border-t border-theme-primary flex gap-3 justify-end">
              <button
                onClick={() => {
                  setShowSaveViewModal(false)
                  setNewViewName('')
                  setNewViewIcon('')
                }}
                className="btn-theme-secondary"
              >
                Cancel
              </button>
              <button
                onClick={handleSaveCurrentView}
                disabled={!newViewName.trim()}
                className="btn-theme-primary disabled:opacity-50"
              >
                Save View
              </button>
            </div>
          </div>
        </div>
      )}

      <SettingsDialog
        isOpen={showSettings}
        onClose={() => setShowSettings(false)}
        markReadBehavior={markReadBehavior}
        onChangeMarkReadBehavior={handleMarkReadBehaviorChange}
        layoutMode={layoutMode}
        onChangeLayoutMode={handleLayoutModeChange}
        onFilterRulesChange={() => setFilterRules(loadFilterRules())}
        views={views}
        onRenameView={handleRenameView}
        onDeleteView={handleDeleteView}
        onMoveView={handleMoveView}
        organizationMode={organizationMode}
        onChangeOrganizationMode={(mode) => { clearActiveView(); handleOrganizationModeChange(mode) }}
        feeds={feeds.map((f: Feed) => ({
          type: f.type,
          url: f.url || f.npub || '',
          title: f.title,
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
                  setPendingSyncCreatedAt(null)
                }}
                className="btn-theme-secondary"
              >
                Not Now
              </button>
              <button
                onClick={async () => {
                  try {
                    await handleImportFeeds(pendingSyncImport)
                  } catch {
                    // Import failed (already surfaced to the user). Keep the
                    // prompt open and the watermark unadvanced so retry works.
                    return
                  }
                  // Re-check freshness: another sync path may have advanced the
                  // watermark while this prompt was open, so keep it monotonic.
                  advanceSyncWatermarkIfFresh('readstr-subscriptions', pendingSyncCreatedAt ?? undefined)
                  setShowSyncPrompt(false)
                  setPendingSyncImport(null)
                  setPendingSyncCreatedAt(null)
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