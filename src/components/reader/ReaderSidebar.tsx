'use client'

import { ThemeSelector } from '../theme-selector'
import { env } from '@/env.mjs'
import type { HistoryRecord } from '@/lib/reading-history'
import type { Category, Feed, FavoritesResponse } from './types'

interface ReaderSidebarProps {
  showSidebar: boolean
  onRefreshAllFeeds: () => void
  isRefreshingAll: boolean
  lastRefreshTime: number | null
  setShowSettings: React.Dispatch<React.SetStateAction<boolean>>
  setShowAddFeed: React.Dispatch<React.SetStateAction<boolean>>
  user: { npub?: string | null } | null | undefined
  sidebarView: 'feeds' | 'tags' | 'favorites' | 'history'
  setSidebarView: React.Dispatch<React.SetStateAction<'feeds' | 'tags' | 'favorites' | 'history'>>
  setSelectedTags: React.Dispatch<React.SetStateAction<string[]>>
  selectedCategoryId: string | null
  setSelectedCategoryId: React.Dispatch<React.SetStateAction<string | null>>
  onClearActiveView: () => void
  organizationMode: 'tags' | 'categories'
  selectedTags: string[]
  onClearTags: () => void
  onToggleTag: (tag: string) => void
  allFeeds: Feed[]
  selectedFeed: string | null
  setSelectedFeed: React.Dispatch<React.SetStateAction<string | null>>
  setShowSidebar: React.Dispatch<React.SetStateAction<boolean>>
  openMenuFeedId: string | null
  setOpenMenuFeedId: React.Dispatch<React.SetStateAction<string | null>>
  showCategoryPicker: string | null
  setShowCategoryPicker: React.Dispatch<React.SetStateAction<string | null>>
  onRefreshFeed: (feedId: string, feedType: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO') => void
  refreshFeedLoading: boolean
  refreshNostrFeedLoading: boolean
  onMarkAllAsRead: (feedId: string) => void
  markAllAsReadLoading: boolean
  categories: Category[]
  feeds: Feed[]
  onOpenEditMenu: (feedId: string, currentTags: string[]) => void
  onSetCategory: (feedId: string, categoryId: string | null) => void
  onRemoveFeed: (feedId: string, feedTitle: string) => void
  categoriesWithUnread: (Category & { unreadCount: number; feedCount: number })[]
  tagSortOrder: 'alphabetical' | 'unread'
  setTagSortOrder: React.Dispatch<React.SetStateAction<'alphabetical' | 'unread'>>
  filteredTags: { tag: string; unreadCount: number; feedCount: number }[]
  favoritesLoading: boolean
  favoritesData: FavoritesResponse | undefined
  selectedItem: string | null
  setSelectedItem: React.Dispatch<React.SetStateAction<string | null>>
  setSelectedHistoryItem: React.Dispatch<React.SetStateAction<HistoryRecord | null>>
  setMobileView: (view: 'list' | 'content') => void
  onRecordReadHistory: (item: {
    id: string
    title: string
    content: string
    author: string | null
    feedTitle: string
    url?: string | null
    originalUrl?: string | null
    feedType: string
  }) => void
  onToggleFavorite: (itemId: string, isFavorited: boolean) => void
  historyQuery: string
  setHistoryQuery: React.Dispatch<React.SetStateAction<string>>
  historyResults: HistoryRecord[]
  selectedHistoryItem: HistoryRecord | null
  onNavigateAdmin: () => void
  onSignOut: () => void
}

export function ReaderSidebar({
  showSidebar,
  onRefreshAllFeeds,
  isRefreshingAll,
  lastRefreshTime,
  setShowSettings,
  setShowAddFeed,
  user,
  sidebarView,
  setSidebarView,
  setSelectedTags,
  selectedCategoryId,
  setSelectedCategoryId,
  onClearActiveView,
  organizationMode,
  selectedTags,
  onClearTags,
  onToggleTag,
  allFeeds,
  selectedFeed,
  setSelectedFeed,
  setShowSidebar,
  openMenuFeedId,
  setOpenMenuFeedId,
  showCategoryPicker,
  setShowCategoryPicker,
  onRefreshFeed,
  refreshFeedLoading,
  refreshNostrFeedLoading,
  onMarkAllAsRead,
  markAllAsReadLoading,
  categories,
  feeds,
  onOpenEditMenu,
  onSetCategory,
  onRemoveFeed,
  categoriesWithUnread,
  tagSortOrder,
  setTagSortOrder,
  filteredTags,
  favoritesLoading,
  favoritesData,
  selectedItem,
  setSelectedItem,
  setSelectedHistoryItem,
  setMobileView,
  onRecordReadHistory,
  onToggleFavorite,
  historyQuery,
  setHistoryQuery,
  historyResults,
  selectedHistoryItem,
  onNavigateAdmin,
  onSignOut,
}: ReaderSidebarProps) {
  return (
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
                onClick={onRefreshAllFeeds}
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
                onClearActiveView()
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
                onClearActiveView()
              }}
              className={`flex-1 px-3 py-2 text-sm rounded-lg font-medium transition-all duration-200 ${
                sidebarView === 'favorites'
                  ? 'bg-theme-surface shadow-theme-sm text-theme-primary'
                  : 'text-theme-secondary hover:text-theme-primary'
              }`}
            >
              ⭐
            </button>
            <button
              onClick={() => {
                setSidebarView('history')
                setSelectedTags([])
                setSelectedCategoryId(null)
                onClearActiveView()
              }}
              className={`flex-1 px-3 py-2 text-sm rounded-lg font-medium transition-all duration-200 ${
                sidebarView === 'history'
                  ? 'bg-theme-surface shadow-theme-sm text-theme-primary'
                  : 'text-theme-secondary hover:text-theme-primary'
              }`}
            >
              📖
            </button>
          </div>
        </div>

        {/* Active Tag Filters */}
        {selectedTags.length > 0 && (
          <div className="px-4 py-3 border-b border-theme-primary bg-theme-accent-light flex-shrink-0">
            <div className="flex items-center justify-between mb-2">
              <span className="text-xs font-semibold text-theme-accent uppercase tracking-wider">Filtered by</span>
              <button
                onClick={onClearTags}
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
                    onClick={() => onToggleTag(tag)}
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
                  onClearActiveView()
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
                          onRefreshFeed(feed.id, feed.type)
                          setOpenMenuFeedId(null)
                        }}
                        disabled={refreshFeedLoading || refreshNostrFeedLoading}
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
                          onMarkAllAsRead(feed.id)
                        }}
                        disabled={markAllAsReadLoading || feed.unreadCount === 0}
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
                            onOpenEditMenu(feed.id, currentFeed?.tags || [])
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
                          onRemoveFeed(feed.id, feed.title)
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
                            onSetCategory(feed.id, null)
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
                              onSetCategory(feed.id, cat.id)
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
                      onClick={() => {
                        setSelectedCategoryId(null)
                        onClearActiveView()
                      }}
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
                        onClick={() => {
                          setSelectedCategoryId(cat.id)
                          onClearActiveView()
                        }}
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
                        onClick={() => onToggleTag(tag)}
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
                        setSelectedHistoryItem(null)
                        setMobileView('content')
                        onRecordReadHistory(item)
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
                        onToggleFavorite(item.id, true)
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

        {/* Reading History */}
        {sidebarView === 'history' && (
          <div className="flex-1 overflow-y-auto themed-scrollbar flex flex-col">
            <div className="p-4 border-b border-theme-primary flex-shrink-0">
              <input
                type="text"
                value={historyQuery}
                onChange={(e) => setHistoryQuery(e.target.value)}
                placeholder="Search history..."
                className="input-theme w-full text-sm"
              />
            </div>
            {historyResults.length === 0 ? (
              <div className="p-6 text-center">
                <div className="text-4xl mb-3">📖</div>
                <p className="text-sm text-theme-secondary">
                  {historyQuery.trim() ? 'No matches found' : 'No reading history yet'}
                </p>
                <p className="text-xs text-theme-tertiary mt-1">Items you open are saved here for offline reading.</p>
              </div>
            ) : (
              historyResults.map((record) => (
                <button
                  key={record.id}
                  onClick={() => {
                    setSelectedHistoryItem(record)
                    setSelectedItem(null)
                    setMobileView('content')
                  }}
                  className={`w-full px-4 py-3 text-left transition-all duration-150 ${
                    selectedHistoryItem?.id === record.id
                      ? 'bg-theme-accent-light border-l-4 border-l-[rgb(var(--color-accent))]'
                      : 'hover:bg-theme-hover border-l-4 border-l-transparent'
                  }`}
                >
                  <div className="flex items-center gap-2 mb-1">
                    <span className="text-xs">📖</span>
                    <span className="text-xs text-theme-tertiary truncate">
                      {record.author || record.feedTitle || 'Unknown'}
                    </span>
                  </div>
                  <h3 className="text-sm font-medium text-theme-primary mb-1 line-clamp-2">
                    {record.title}
                  </h3>
                  <p className="text-xs text-theme-tertiary">
                    {new Date(record.readAt).toLocaleString()}
                  </p>
                </button>
              ))
            )}
          </div>
        )}

        {/* Footer */}
        <div className="p-4 border-t border-theme-primary flex-shrink-0 space-y-2">
          {user?.npub === env.NEXT_PUBLIC_ADMIN_NPUB && (
            <button
              onClick={() => onNavigateAdmin()}
              className="w-full flex items-center gap-2 text-sm text-theme-secondary hover:text-theme-primary transition-colors"
            >
              <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M9 19v-6a2 2 0 00-2-2H5a2 2 0 00-2 2v6a2 2 0 002 2h2a2 2 0 002-2zm0 0V9a2 2 0 012-2h2a2 2 0 012 2v10m-6 0a2 2 0 002 2h2a2 2 0 002-2m0 0V5a2 2 0 012-2h2a2 2 0 012 2v14a2 2 0 01-2 2h-2a2 2 0 01-2-2z" />
              </svg>
              Admin Dashboard
            </button>
          )}
          <button
            onClick={onSignOut}
            className="w-full flex items-center gap-2 text-sm text-theme-secondary hover:text-theme-primary transition-colors"
          >
            <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M17 16l4-4m0 0l-4-4m4 4H7m6 4v1a3 3 0 01-3 3H6a3 3 0 01-3-3V7a3 3 0 013-3h4a3 3 0 013 3v1" />
            </svg>
            Sign Out
          </button>
        </div>
      </div>
  )
}
