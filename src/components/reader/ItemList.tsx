'use client'

import { SavedViewsBar } from '../saved-views-bar'
import { AuthorByline } from './author-byline'
import type { MarkReadBehavior, LayoutMode } from '../settings-dialog'
import type { SavedView } from '@/lib/saved-views'
import type { HistoryRecord } from '@/lib/reading-history'
import type { FilterOutcome } from '@/lib/keyword-filter'
import type { Feed, FeedItem } from './types'

interface ItemListProps {
  layoutMode: LayoutMode
  mobileView: 'list' | 'content'
  selectedItem: string | null
  selectedHistoryItem: HistoryRecord | null
  selectedFeed: string | null
  allFeeds: Feed[]
  showViewOptions: boolean
  setShowViewOptions: React.Dispatch<React.SetStateAction<boolean>>
  viewFilter: 'all' | 'unread' | 'read'
  setViewFilter: React.Dispatch<React.SetStateAction<'all' | 'unread' | 'read'>>
  sortOrder: 'newest' | 'oldest'
  setSortOrder: React.Dispatch<React.SetStateAction<'newest' | 'oldest'>>
  markReadBehavior: MarkReadBehavior
  onMarkReadBehaviorChange: (behavior: MarkReadBehavior) => void
  quickMarkReadOptions: { value: MarkReadBehavior; label: string; helper: string }[]
  onClearActiveView: () => void
  views: SavedView[]
  activeViewId: string | null
  onApplyView: (view: SavedView) => void
  onSaveCurrent: () => void
  allFeedItems: FeedItem[]
  hiddenByFilterCount: number
  showHiddenByFilter: boolean
  setShowHiddenByFilter: React.Dispatch<React.SetStateAction<boolean>>
  itemsLoading: boolean
  feedItems: FeedItem[]
  filterOutcomes: Map<string, FilterOutcome>
  onItemClick: (itemId: string) => void
  setMobileView: (view: 'list' | 'content') => void
  onToggleFavorite: (itemId: string, isFavorited: boolean) => void
}

export function ItemList({
  layoutMode,
  mobileView,
  selectedItem,
  selectedHistoryItem,
  selectedFeed,
  allFeeds,
  showViewOptions,
  setShowViewOptions,
  viewFilter,
  setViewFilter,
  sortOrder,
  setSortOrder,
  markReadBehavior,
  onMarkReadBehaviorChange,
  quickMarkReadOptions,
  onClearActiveView,
  views,
  activeViewId,
  onApplyView,
  onSaveCurrent,
  allFeedItems,
  hiddenByFilterCount,
  showHiddenByFilter,
  setShowHiddenByFilter,
  itemsLoading,
  feedItems,
  filterOutcomes,
  onItemClick,
  setMobileView,
  onToggleFavorite,
}: ItemListProps) {
  const contentActive = mobileView === 'content' && (selectedItem || selectedHistoryItem)
  const visibilityClass = layoutMode === 'split'
    ? (contentActive ? 'hidden md:flex' : 'flex')
    : (contentActive ? 'hidden' : 'flex')
  const widthClass = layoutMode === 'split' ? 'w-full md:w-96' : 'w-full flex-1'

  const renderItem = (item: FeedItem) => {
    const outcome = filterOutcomes.get(item.id)
    return (
      <div
        key={item.id}
        className={`article-card relative group ${
          selectedItem === item.id ? 'active' : ''
        } ${item.isRead ? 'read' : ''}`}
        style={{
          ...(outcome?.highlight ? { boxShadow: `inset 3px 0 0 ${outcome.highlight}` } : {}),
          ...(outcome?.hidden ? { opacity: 0.4 } : {}),
        }}
      >
        <button
          onClick={() => {
            onItemClick(item.id)
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
              <AuthorByline
                author={item.author}
                feedType={item.feedType}
                feedTitle={item.feedTitle}
                className="font-medium"
              />
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
            onToggleFavorite(item.id, item.isFavorited || false)
          }}
          className="absolute top-4 right-4 p-2 rounded-lg opacity-0 group-hover:opacity-100 hover:bg-theme-hover transition-all"
          title={item.isFavorited ? 'Remove from favorites' : 'Add to favorites'}
        >
          <span className="text-xl">
            {item.isFavorited ? '⭐' : '☆'}
          </span>
        </button>
      </div>
    )
  }

  return (
      <div className={`
        ${visibilityClass}
        ${widthClass} bg-theme-surface border-r border-theme-primary flex-col max-h-screen
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
                        onClearActiveView()
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
                        onClearActiveView()
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
                        onClearActiveView()
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
                          onClearActiveView()
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
                          onClearActiveView()
                        }}
                        className={`dropdown-item ${sortOrder === 'oldest' ? 'active' : ''}`}
                      >
                        {sortOrder === 'oldest' && '✓ '}Oldest First
                      </button>
                    </div>

                    <div className="border-t border-theme-primary mt-1 pt-1">
                      <div className="px-3 py-2 text-xs font-semibold text-theme-tertiary uppercase tracking-wider">Mark as read</div>
                      {quickMarkReadOptions.map(option => (
                        <button
                          key={option.value}
                          onClick={(e) => {
                            e.stopPropagation()
                            onMarkReadBehaviorChange(option.value)
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

          <SavedViewsBar
            views={views}
            activeViewId={activeViewId}
            onSelectCurrent={onClearActiveView}
            onSelectView={onApplyView}
            onSaveCurrent={onSaveCurrent}
          />

          <div className="mt-3 flex items-center gap-2 text-sm text-theme-secondary">
            <span className="font-medium">{allFeedItems.filter(item => !item.isRead).length}</span>
            <span>unread</span>
            {viewFilter !== 'all' && (
              <span className="px-2 py-0.5 text-xs rounded-full bg-theme-accent-light text-theme-accent font-medium">
                {viewFilter === 'unread' ? 'Unread only' : 'Read only'}
              </span>
            )}
          </div>
        </div>

        {hiddenByFilterCount > 0 && (
          <div className="flex items-center justify-between gap-3 px-4 py-2 text-sm bg-theme-tertiary border-b border-theme-primary text-theme-secondary">
            <span>{hiddenByFilterCount} hidden by filters</span>
            <button
              onClick={() => setShowHiddenByFilter(v => !v)}
              className="flex-shrink-0 underline font-medium text-theme-accent"
            >
              {showHiddenByFilter ? 'Hide' : 'Show'}
            </button>
          </div>
        )}

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
          ) : layoutMode === 'grid' ? (
            <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-3 gap-3 p-3">
              {feedItems.map(renderItem)}
            </div>
          ) : (
            feedItems.map(renderItem)
          )}
        </div>
      </div>
  )
}
