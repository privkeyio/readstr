'use client'

import { useState, useEffect } from 'react'
import { api } from '@/trpc/react'
import { useNostrAuth } from '@/contexts/NostrAuthContext'
import {
  publishSubscriptionList,
  fetchSubscriptionList,
  buildSubscriptionListFromFeeds,
  mergeSubscriptionLists,
  getLastSyncTime,
  setLastSyncTime,
  advanceSyncWatermarkIfFresh,
  type SubscriptionList,
} from '@/lib/nostr-sync'

export type MarkReadBehavior = 'on-open' | 'after-10s' | 'never'
export type OrganizationMode = 'tags' | 'categories'

// Settings tabs
type SettingsTab = 'relays' | 'organization' | 'reading' | 'sync' | 'about'

const SETTINGS_TABS: { id: SettingsTab; label: string; icon: string }[] = [
  { id: 'relays', label: 'Nostr Relays', icon: '🔌' },
  { id: 'organization', label: 'Feed Organization', icon: '📁' },
  { id: 'reading', label: 'Reading', icon: '📖' },
  { id: 'sync', label: 'Sync', icon: '🔄' },
  { id: 'about', label: 'About', icon: 'ℹ️' },
]

// Sync state type
export interface SyncState {
  status: 'idle' | 'syncing' | 'success' | 'error'
  lastSync: number | null
  error?: string
  pendingImport?: {
    toAdd: Array<{ type: 'RSS' | 'NOSTR'; url: string; tags?: string[]; category?: { name: string; color?: string; icon?: string } }>
    localOnly: Array<{ type: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'; url: string }>
    createdAt?: number
  }
}

interface Category {
  id: string
  name: string
  color: string | null
  icon: string | null
  feedCount: number
}

interface Relay {
  url: string
  status: 'connected' | 'connecting' | 'disconnected' | 'error'
}

const DEFAULT_RELAYS = [
  'wss://relay.damus.io',
  'wss://nos.lol',
  'wss://relay.snort.social',
  'wss://relay.nostr.band',
  'wss://nostr-pub.wellorder.net',
]

const POPULAR_RELAYS = [
  { url: 'wss://relay.damus.io', name: 'Damus' },
  { url: 'wss://nos.lol', name: 'nos.lol' },
  { url: 'wss://relay.snort.social', name: 'Snort' },
  { url: 'wss://relay.nostr.band', name: 'Nostr Band' },
  { url: 'wss://nostr-pub.wellorder.net', name: 'Wellorder' },
  { url: 'wss://relay.primal.net', name: 'Primal' },
  { url: 'wss://relay.nostr.bg', name: 'Nostr BG' },
  { url: 'wss://nostr.wine', name: 'Nostr Wine' },
  { url: 'wss://purplepag.es', name: 'Purple Pages' },
]

const MARK_READ_OPTIONS: { value: MarkReadBehavior; title: string; description: string }[] = [
  {
    value: 'on-open',
    title: 'When I open an article',
    description: 'Articles are marked as read immediately when you open them.',
  },
  {
    value: 'after-10s',
    title: 'After 10 seconds of reading',
    description: 'Gives you a short grace period before marking items as read.',
  },
  {
    value: 'never',
    title: 'Never automatically',
    description: 'Articles stay unread until you manually mark them as read.',
  },
]

interface SettingsDialogProps {
  isOpen: boolean
  onClose: () => void
  markReadBehavior: MarkReadBehavior
  onChangeMarkReadBehavior: (behavior: MarkReadBehavior) => void
  organizationMode: OrganizationMode
  onChangeOrganizationMode: (mode: OrganizationMode) => void
  feeds?: Array<{ type: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'; url: string; tags?: string[]; category?: { name: string; color?: string | null; icon?: string | null } | null }>
  userPubkey?: string
  onImportFeeds?: (feeds: Array<{ type: 'RSS' | 'NOSTR'; url: string; tags?: string[]; category?: { name: string; color?: string; icon?: string } }>) => Promise<void>
}

// Default category colors/icons
const CATEGORY_COLORS = [
  '#3b82f6', // blue
  '#10b981', // green
  '#f59e0b', // amber
  '#ef4444', // red
  '#8b5cf6', // violet
  '#ec4899', // pink
  '#06b6d4', // cyan
  '#84cc16', // lime
]

const CATEGORY_ICONS = ['📁', '📰', '🎬', '🎵', '💼', '🎮', '📚', '🔬', '💡', '🌍', '⚡', '🎯']

export function SettingsDialog({ isOpen, onClose, markReadBehavior, onChangeMarkReadBehavior, organizationMode, onChangeOrganizationMode, feeds = [], userPubkey, onImportFeeds }: SettingsDialogProps) {
  const { user, canSign, signEventOrThrow } = useNostrAuth()
  const [activeTab, setActiveTab] = useState<SettingsTab>('relays')
  const [relays, setRelays] = useState<Relay[]>([])
  const [newRelayUrl, setNewRelayUrl] = useState('')
  const [error, setError] = useState('')
  const [syncState, setSyncState] = useState<SyncState>({
    status: 'idle',
    lastSync: null,
  })
  
  // Category management state
  const [newCategoryName, setNewCategoryName] = useState('')
  const [newCategoryColor, setNewCategoryColor] = useState(CATEGORY_COLORS[0])
  const [newCategoryIcon, setNewCategoryIcon] = useState('📁')
  const [editingCategory, setEditingCategory] = useState<Category | null>(null)
  const [categoryError, setCategoryError] = useState('')
  
  // tRPC for categories
  const utils = api.useUtils()
  const { data: categories = [] } = api.feed.getCategories.useQuery(undefined, {
    enabled: isOpen && !!userPubkey,
  })
  
  const createCategoryMutation = api.feed.createCategory.useMutation({
    onSuccess: () => {
      void utils.feed.getCategories.invalidate()
      setNewCategoryName('')
      setNewCategoryColor(CATEGORY_COLORS[0])
      setNewCategoryIcon('📁')
      setCategoryError('')
    },
    onError: (error) => {
      setCategoryError(error.message)
    },
  })
  
  const updateCategoryMutation = api.feed.updateCategory.useMutation({
    onSuccess: () => {
      void utils.feed.getCategories.invalidate()
      setEditingCategory(null)
      setCategoryError('')
    },
    onError: (error) => {
      setCategoryError(error.message)
    },
  })
  
  const deleteCategoryMutation = api.feed.deleteCategory.useMutation({
    onSuccess: () => {
      void utils.feed.getCategories.invalidate()
    },
  })

  // Load relays from localStorage on mount
  useEffect(() => {
    const savedRelays = localStorage.getItem('nostr_relays')
    let urls: string[] = DEFAULT_RELAYS
    if (savedRelays) {
      try {
        const parsed = JSON.parse(savedRelays)
        urls = Array.isArray(parsed) ? parsed : DEFAULT_RELAYS
      } catch (e) {
        // Use defaults if parsing fails
        urls = DEFAULT_RELAYS
      }
    }
    // Reading persisted relays from localStorage on mount; localStorage is
    // unavailable during SSR so this must happen in an effect, not at render.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    setRelays(urls.map((url: string) => ({ url, status: 'disconnected' as const })))

    // Load last sync time
    const lastSync = getLastSyncTime()
    if (lastSync) {
      setSyncState(prev => ({ ...prev, lastSync }))
    }
  }, [])

  // Save relays to localStorage whenever they change
  useEffect(() => {
    if (relays.length > 0) {
      localStorage.setItem('nostr_relays', JSON.stringify(relays.map(r => r.url)))
    }
  }, [relays])

  // Export subscriptions to Nostr
  const handleExportToNostr = async () => {
    if (!canSign || !user?.pubkey) {
      alert('Connect with a Nostr signer (browser extension or remote signer) to sync.')
      return
    }

    setSyncState(prev => ({ ...prev, status: 'syncing', error: undefined }))

    try {
      const subscriptionList = buildSubscriptionListFromFeeds(feeds)

      const result = await publishSubscriptionList(subscriptionList, signEventOrThrow)
      
      if (result.success) {
        const now = Math.floor(Date.now() / 1000)
        setLastSyncTime(now)
        setSyncState({ status: 'success', lastSync: now })
        setTimeout(() => setSyncState(prev => ({ ...prev, status: 'idle' })), 3000)
      } else {
        setSyncState({ status: 'error', lastSync: syncState.lastSync, error: result.error })
      }
    } catch (error) {
      setSyncState({
        status: 'error',
        lastSync: syncState.lastSync,
        error: error instanceof Error ? error.message : 'Unknown error',
      })
    }
  }

  // Import subscriptions from Nostr
  const handleImportFromNostr = async () => {
    if (!userPubkey) {
      alert('Please sign in to import subscriptions.')
      return
    }

    setSyncState(prev => ({ ...prev, status: 'syncing', error: undefined }))

    try {
      const result = await fetchSubscriptionList(userPubkey)
      
      if (!result.success) {
        setSyncState({
          status: 'error',
          lastSync: syncState.lastSync,
          error: result.error,
        })
        return
      }

      // No freshness gate here: this is an explicit, user-initiated import and it
      // is add-only (handleConfirmImport applies only mergeResult.toAdd, never
      // removes local feeds), so there is nothing for the anti-rollback watermark
      // to protect. Gating it would permanently block re-importing the same list
      // after local subscriptions are cleared. The merge result drives the UX.
      // The watermark is advanced in handleConfirmImport (only on accept, and only
      // monotonically) so a dismissed import can still be re-imported later.

      if (!result.data || (result.data.rss.length === 0 && result.data.nostr.length === 0)) {
        setSyncState({
          status: 'success',
          lastSync: syncState.lastSync,
        })
        alert('No subscriptions found on Nostr. Export your current subscriptions first.')
        setTimeout(() => setSyncState(prev => ({ ...prev, status: 'idle' })), 3000)
        return
      }

      // Merge with current feeds
      const mergeResult = mergeSubscriptionLists(feeds, result.data)
      
      if (mergeResult.toAdd.length === 0) {
        setSyncState({ status: 'success', lastSync: syncState.lastSync })
        alert('All remote subscriptions are already in your feed list.')
        setTimeout(() => setSyncState(prev => ({ ...prev, status: 'idle' })), 3000)
        return
      }

      // Show pending import
      setSyncState({
        status: 'idle',
        lastSync: syncState.lastSync,
        pendingImport: { ...mergeResult, createdAt: result.createdAt },
      })
    } catch (error) {
      setSyncState({
        status: 'error',
        lastSync: syncState.lastSync,
        error: error instanceof Error ? error.message : 'Unknown error',
      })
    }
  }

  // Confirm import
  const handleConfirmImport = async () => {
    if (!syncState.pendingImport || !onImportFeeds) return
    
    setSyncState(prev => ({ ...prev, status: 'syncing' }))
    
    try {
      await onImportFeeds(syncState.pendingImport.toAdd)
      // Advance the freshness basis now that the data is applied (only on accept,
      // null-safe, and monotonic so a stale relay event can't roll it backward).
      advanceSyncWatermarkIfFresh('readstr-subscriptions', syncState.pendingImport.createdAt)
      const now = Math.floor(Date.now() / 1000)
      setLastSyncTime(now)
      setSyncState({ status: 'success', lastSync: now })
      setTimeout(() => setSyncState(prev => ({ ...prev, status: 'idle' })), 3000)
    } catch (error) {
      setSyncState({
        status: 'error',
        lastSync: syncState.lastSync,
        error: error instanceof Error ? error.message : 'Unknown error',
      })
    }
  }

  // Cancel import
  const handleCancelImport = () => {
    setSyncState(prev => ({ ...prev, pendingImport: undefined }))
  }

  // Format timestamp for display
  const formatLastSync = (timestamp: number | null) => {
    if (!timestamp) return 'Never'
    const date = new Date(timestamp * 1000)
    return date.toLocaleString()
  }

  const validateRelayUrl = (url: string): boolean => {
    if (!url.trim()) {
      setError('Relay URL cannot be empty')
      return false
    }
    if (!url.startsWith('wss://') && !url.startsWith('ws://')) {
      setError('Relay URL must start with wss:// or ws://')
      return false
    }
    if (relays.some(r => r.url === url)) {
      setError('This relay is already added')
      return false
    }
    setError('')
    return true
  }

  const addRelay = () => {
    if (!validateRelayUrl(newRelayUrl)) return

    setRelays([...relays, { url: newRelayUrl, status: 'disconnected' }])
    setNewRelayUrl('')
  }

  const addPopularRelay = (url: string) => {
    if (relays.some(r => r.url === url)) {
      setError('This relay is already added')
      return
    }
    setRelays([...relays, { url, status: 'disconnected' }])
  }

  const removeRelay = (url: string) => {
    setRelays(relays.filter(r => r.url !== url))
  }

  const resetToDefaults = () => {
    if (confirm('Reset to default relays? This will remove all custom relays.')) {
      setRelays(DEFAULT_RELAYS.map(url => ({ url, status: 'disconnected' })))
    }
  }

  if (!isOpen) return null

  return (
    <div className="modal-overlay">
      <div className="modal-content max-w-3xl w-full max-h-[90vh] overflow-hidden flex flex-col animate-slide-in">
        {/* Header */}
        <div className="flex items-center justify-between p-5 border-b border-theme-primary">
          <h2 className="text-xl font-bold text-theme-primary">Settings</h2>
          <button
            onClick={onClose}
            className="p-2 text-theme-secondary hover:bg-theme-hover rounded-full transition-colors"
          >
            ✕
          </button>
        </div>

        {/* Content with sidebar */}
        <div className="flex-1 flex overflow-hidden">
          {/* Sidebar Navigation */}
          <div className="w-52 border-r border-theme-primary bg-theme-tertiary p-3 overflow-y-auto themed-scrollbar">
            {SETTINGS_TABS.map((tab) => (
              <button
                key={tab.id}
                onClick={() => setActiveTab(tab.id)}
                className={`w-full flex items-center gap-3 px-4 py-2.5 rounded-xl text-left text-sm transition-all duration-200 mb-1 ${
                  activeTab === tab.id
                    ? 'bg-theme-accent text-white shadow-theme-sm'
                    : 'text-theme-secondary hover:bg-theme-hover'
                }`}
              >
                <span className="text-base">{tab.icon}</span>
                <span className="font-medium">{tab.label}</span>
              </button>
            ))}
          </div>

          {/* Tab Content */}
          <div className="flex-1 overflow-y-auto themed-scrollbar p-6 bg-theme-secondary">
            {/* Relays Tab */}
            {activeTab === 'relays' && (
              <div>
                <div className="flex items-center justify-between mb-4">
                  <h3 className="text-lg font-bold text-theme-primary">Nostr Relays</h3>
                  <button
                    onClick={resetToDefaults}
                    className="text-sm font-medium text-theme-accent hover:underline"
                  >
                    Reset to Defaults
                  </button>
                </div>
                
                <p className="text-sm text-theme-secondary mb-6">
                  Manage which Nostr relays to use for fetching content. More relays = better content discovery but slower performance.
                </p>

                {/* Add New Relay */}
                <div className="mb-6">
                  <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                    Add Custom Relay
                  </label>
                  <div className="flex gap-2">
                    <input
                      type="text"
                      value={newRelayUrl}
                      onChange={(e) => setNewRelayUrl(e.target.value)}
                      onKeyPress={(e) => e.key === 'Enter' && addRelay()}
                      placeholder="wss://relay.example.com"
                      className="input-theme flex-1"
                    />
                    <button
                      onClick={addRelay}
                      className="btn-theme-primary flex items-center gap-2"
                    >
                      <span>+</span>
                      Add
                    </button>
                  </div>
                  {error && (
                    <div className="mt-2 flex items-center gap-2 text-sm text-red-600">
                      <span>⚠</span>
                      {error}
                    </div>
                  )}
                </div>

                {/* Popular Relays */}
                <div className="mb-6">
                  <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                    Popular Relays
                  </label>
                  <div className="flex flex-wrap gap-2">
                    {POPULAR_RELAYS.map((relay) => {
                      const isAdded = relays.some(r => r.url === relay.url)
                      return (
                        <button
                          key={relay.url}
                          onClick={() => !isAdded && addPopularRelay(relay.url)}
                          disabled={isAdded}
                          className={`px-3 py-1.5 rounded-full text-sm font-medium flex items-center gap-1.5 transition-all duration-200 ${
                            isAdded
                              ? 'bg-theme-tertiary text-theme-tertiary cursor-not-allowed'
                              : 'bg-theme-accent-light text-theme-accent hover:shadow-theme-sm'
                          }`}
                        >
                          {isAdded && <span>✓</span>}
                          {relay.name}
                        </button>
                      )
                    })}
                  </div>
                </div>

                {/* Current Relays List */}
                <div>
                  <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                    Active Relays ({relays.length})
                  </label>
                  <div className="space-y-2">
                    {relays.length === 0 ? (
                      <div className="text-center py-8 text-theme-tertiary">
                        <p>No relays configured</p>
                        <p className="text-sm">Add at least one relay to fetch content</p>
                      </div>
                    ) : (
                      relays.map((relay) => (
                        <div
                          key={relay.url}
                          className="flex items-center justify-between p-3 bg-theme-tertiary rounded-xl"
                        >
                          <div className="flex-1">
                            <div className="font-mono text-sm text-theme-primary">{relay.url}</div>
                          </div>
                          <button
                            onClick={() => removeRelay(relay.url)}
                            className="p-2 text-red-500 hover:bg-red-50 rounded-lg transition-colors"
                            title="Remove relay"
                          >
                            <span>🗑️</span>
                          </button>
                        </div>
                      ))
                    )}
                  </div>
                </div>
              </div>
            )}

            {/* Organization Tab */}
            {activeTab === 'organization' && (
              <div>
                <h3 className="text-lg font-bold text-theme-primary mb-2">Feed Organization</h3>
                <p className="text-sm text-theme-secondary mb-6">
                  Choose how to organize your feeds - using free-form tags or structured categories.
                </p>
                <div className="flex gap-4 mb-6">
                  <label
                    className={`flex-1 p-4 rounded-xl border-2 cursor-pointer transition-all duration-200 ${
                      organizationMode === 'tags'
                        ? 'border-theme-accent bg-theme-accent-light shadow-theme-sm'
                        : 'border-theme-secondary bg-theme-primary hover:border-theme-accent/50'
                    }`}
                  >
                    <input
                      type="radio"
                      name="organization-mode"
                      value="tags"
                      checked={organizationMode === 'tags'}
                      onChange={() => onChangeOrganizationMode('tags')}
                      className="sr-only"
                    />
                    <div className="flex items-center gap-2 mb-2">
                      <span className="text-xl">🏷️</span>
                      <span className="font-bold text-theme-primary">Tags</span>
                    </div>
                    <p className="text-xs text-theme-secondary">
                      Flexible, multiple tags per feed. Great for cross-categorization.
                    </p>
                  </label>
                  <label
                    className={`flex-1 p-4 rounded-xl border-2 cursor-pointer transition-all duration-200 ${
                      organizationMode === 'categories'
                        ? 'border-theme-accent bg-theme-accent-light shadow-theme-sm'
                        : 'border-theme-secondary bg-theme-primary hover:border-theme-accent/50'
                    }`}
                  >
                    <input
                      type="radio"
                      name="organization-mode"
                      value="categories"
                      checked={organizationMode === 'categories'}
                      onChange={() => onChangeOrganizationMode('categories')}
                      className="sr-only"
                    />
                    <div className="flex items-center gap-2 mb-2">
                      <span className="text-xl">📁</span>
                      <span className="font-bold text-theme-primary">Categories</span>
                    </div>
                    <p className="text-xs text-theme-secondary">
                      Traditional folders with icons and colors. One category per feed.
                    </p>
                  </label>
                </div>

                {/* Category Management - only show when categories mode is selected */}
                {organizationMode === 'categories' && (
                  <div className="pt-6 border-t border-theme-primary">
                    <h4 className="font-bold text-theme-primary mb-2">Manage Categories</h4>
                    <p className="text-sm text-theme-secondary mb-4">
                      Create and manage categories to organize your feeds.
                    </p>

                    {/* Add New Category */}
                    <div className="mb-6 p-4 bg-theme-tertiary rounded-xl">
                      <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                        {editingCategory ? 'Edit Category' : 'New Category'}
                      </label>
                      <div className="flex gap-2 mb-3">
                        <input
                          type="text"
                          value={editingCategory ? editingCategory.name : newCategoryName}
                          onChange={(e) => editingCategory 
                            ? setEditingCategory({ ...editingCategory, name: e.target.value })
                            : setNewCategoryName(e.target.value)
                          }
                          placeholder="Category name"
                          className="input-theme flex-1"
                        />
                      </div>
                      
                      {/* Icon picker */}
                      <div className="mb-3">
                        <label className="block text-xs text-theme-tertiary mb-1 uppercase tracking-wider">Icon</label>
                        <div className="flex flex-wrap gap-1">
                          {CATEGORY_ICONS.map((icon) => (
                            <button
                              key={icon}
                              onClick={() => editingCategory 
                                ? setEditingCategory({ ...editingCategory, icon })
                                : setNewCategoryIcon(icon)
                              }
                              className={`w-9 h-9 text-lg rounded-lg flex items-center justify-center transition-all ${
                                (editingCategory ? editingCategory.icon : newCategoryIcon) === icon
                                  ? 'bg-theme-accent-light ring-2 ring-theme-accent shadow-theme-sm'
                                  : 'hover:bg-theme-hover'
                              }`}
                            >
                              {icon}
                            </button>
                          ))}
                        </div>
                      </div>

                      {/* Color picker */}
                      <div className="mb-3">
                        <label className="block text-xs text-theme-tertiary mb-1 uppercase tracking-wider">Color</label>
                        <div className="flex gap-2">
                          {CATEGORY_COLORS.map((color) => (
                            <button
                              key={color}
                              onClick={() => editingCategory 
                                ? setEditingCategory({ ...editingCategory, color })
                                : setNewCategoryColor(color)
                              }
                              className={`w-7 h-7 rounded-full transition-all ${
                                (editingCategory ? editingCategory.color : newCategoryColor) === color
                                  ? 'ring-2 ring-offset-2 ring-theme-accent scale-110'
                                  : 'hover:scale-110'
                              }`}
                              style={{ backgroundColor: color }}
                            />
                          ))}
                        </div>
                      </div>

                      {categoryError && (
                        <div className="mb-3 text-sm text-red-600">{categoryError}</div>
                      )}

                      <div className="flex gap-2">
                        {editingCategory ? (
                          <>
                            <button
                              onClick={() => updateCategoryMutation.mutate({
                                id: editingCategory.id,
                                name: editingCategory.name,
                                color: editingCategory.color ?? undefined,
                                icon: editingCategory.icon ?? undefined,
                              })}
                              disabled={updateCategoryMutation.isPending || !editingCategory.name.trim()}
                              className="btn-theme-primary text-sm"
                            >
                              {updateCategoryMutation.isPending ? 'Saving...' : 'Save Changes'}
                            </button>
                            <button
                              onClick={() => {
                                setEditingCategory(null)
                                setCategoryError('')
                              }}
                              className="btn-theme-secondary text-sm"
                            >
                              Cancel
                            </button>
                          </>
                        ) : (
                          <button
                            onClick={() => createCategoryMutation.mutate({
                              name: newCategoryName,
                              color: newCategoryColor,
                              icon: newCategoryIcon,
                            })}
                            disabled={createCategoryMutation.isPending || !newCategoryName.trim()}
                            className="btn-theme-primary text-sm"
                          >
                            {createCategoryMutation.isPending ? 'Creating...' : 'Create Category'}
                          </button>
                        )}
                      </div>
                    </div>

                    {/* Categories List */}
                    <div>
                      <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                        Your Categories ({categories.length})
                      </label>
                      {categories.length === 0 ? (
                        <div className="text-center py-6 text-theme-tertiary text-sm">
                          No categories yet. Create one above to get started!
                        </div>
                      ) : (
                        <div className="space-y-2">
                          {categories.map((cat: Category) => (
                            <div
                              key={cat.id}
                              className="flex items-center justify-between p-3 bg-theme-tertiary rounded-xl"
                            >
                              <div className="flex items-center gap-3">
                                <span
                                  className="w-9 h-9 rounded-lg flex items-center justify-center text-lg shadow-sm"
                                  style={{ backgroundColor: cat.color ?? '#94a3b8' }}
                                >
                                  {cat.icon || '📁'}
                                </span>
                                <div>
                                  <div className="font-medium text-theme-primary">{cat.name}</div>
                                  <div className="text-xs text-theme-tertiary">{cat.feedCount} feeds</div>
                                </div>
                              </div>
                              <div className="flex gap-1">
                                <button
                                  onClick={() => setEditingCategory(cat)}
                                  className="p-2 text-theme-secondary hover:bg-theme-hover rounded-lg transition-colors"
                                  title="Edit category"
                                >
                                  ✏️
                                </button>
                                <button
                                  onClick={() => {
                                    if (confirm(`Delete "${cat.name}"? Feeds in this category will be uncategorized.`)) {
                                      deleteCategoryMutation.mutate({ id: cat.id })
                                    }
                                  }}
                                  className="p-2 text-red-500 hover:bg-red-50 rounded-lg transition-colors"
                                  title="Delete category"
                                >
                                  🗑️
                                </button>
                              </div>
                            </div>
                          ))}
                        </div>
                      )}
                    </div>
                  </div>
                )}
              </div>
            )}

            {/* Reading Tab */}
            {activeTab === 'reading' && (
              <div>
                <h3 className="text-lg font-bold text-theme-primary mb-2">Reading Preferences</h3>
                <p className="text-sm text-theme-secondary mb-6">
                  Choose when articles should be marked as read.
                </p>
                <div className="space-y-3">
                  {MARK_READ_OPTIONS.map((option) => (
                    <label
                      key={option.value}
                      className={`flex items-start gap-3 p-4 rounded-xl border-2 cursor-pointer transition-all duration-200 ${
                        markReadBehavior === option.value
                          ? 'border-theme-accent bg-theme-accent-light shadow-theme-sm'
                          : 'border-theme-secondary bg-theme-primary hover:border-theme-accent/50'
                      }`}
                    >
                      <input
                        type="radio"
                        name="mark-read-behavior"
                        value={option.value}
                        checked={markReadBehavior === option.value}
                        onChange={() => onChangeMarkReadBehavior(option.value)}
                        className="mt-1 accent-[rgb(var(--color-accent))]"
                      />
                      <div>
                        <div className="font-medium text-theme-primary">{option.title}</div>
                        <p className="text-sm text-theme-secondary">{option.description}</p>
                      </div>
                    </label>
                  ))}
                </div>
              </div>
            )}

            {/* Sync Tab */}
            {activeTab === 'sync' && (
              <div>
                <h3 className="text-lg font-bold text-theme-primary mb-2">Subscription Sync</h3>
                <p className="text-sm text-theme-secondary mb-6">
                  Sync your RSS and Nostr subscriptions across devices using Nostr events (kind 30404).
                </p>
                
                {/* Last sync time */}
                <div className="text-sm text-theme-tertiary mb-4">
                  Last synced: {formatLastSync(syncState.lastSync)}
                </div>

                {/* Sync status */}
                {syncState.status === 'syncing' && (
                  <div className="flex items-center gap-2 p-4 bg-theme-accent-light rounded-xl mb-4">
                    <svg className="animate-spin h-5 w-5 text-theme-accent" xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24">
                      <circle className="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="4"></circle>
                      <path className="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path>
                    </svg>
                    <span className="text-theme-accent font-medium">Syncing...</span>
                  </div>
                )}

                {syncState.status === 'success' && !syncState.pendingImport && (
                  <div className="flex items-center gap-2 p-4 bg-green-50 rounded-xl mb-4">
                    <span className="text-green-600">✓</span>
                    <span className="text-green-700 font-medium">Sync successful!</span>
                  </div>
                )}

                {syncState.status === 'error' && (
                  <div className="flex items-center gap-2 p-4 bg-red-50 rounded-xl mb-4">
                    <span className="text-red-600">⚠</span>
                    <span className="text-red-700 font-medium">Error: {syncState.error}</span>
                  </div>
                )}

                {/* Pending import confirmation */}
                {syncState.pendingImport && (
                  <div className="p-4 bg-yellow-50 rounded-xl mb-4">
                    <p className="font-medium text-yellow-800 mb-2">
                      Found {syncState.pendingImport.toAdd.length} new subscription(s) to import:
                    </p>
                    <ul className="text-sm text-yellow-700 mb-3 max-h-32 overflow-y-auto themed-scrollbar">
                      {syncState.pendingImport.toAdd.map((feed, i) => (
                        <li key={i} className="truncate">
                          • [{feed.type}] {feed.url}
                        </li>
                      ))}
                    </ul>
                    <div className="flex gap-2">
                      <button
                        onClick={handleConfirmImport}
                        className="px-4 py-2 bg-green-600 text-white rounded-xl font-medium hover:bg-green-700 text-sm"
                      >
                        Import All
                      </button>
                      <button
                        onClick={handleCancelImport}
                        className="btn-theme-secondary text-sm"
                      >
                        Cancel
                      </button>
                    </div>
                  </div>
                )}

                {/* Sync buttons */}
                <div className="flex flex-wrap gap-3">
                  <button
                    onClick={handleExportToNostr}
                    disabled={syncState.status === 'syncing' || feeds.length === 0}
                    className="flex items-center gap-2 px-4 py-2.5 bg-theme-accent text-white rounded-xl font-medium hover:bg-[rgb(var(--color-accent-hover))] disabled:opacity-50 disabled:cursor-not-allowed transition-all"
                  >
                    <span>⬆</span>
                    Export to Nostr
                  </button>
                  <button
                    onClick={handleImportFromNostr}
                    disabled={syncState.status === 'syncing' || !userPubkey}
                    className="flex items-center gap-2 px-4 py-2.5 border border-theme-accent text-theme-accent bg-transparent rounded-xl font-medium hover:bg-theme-accent-light disabled:opacity-50 disabled:cursor-not-allowed transition-all"
                  >
                    <span>⬇</span>
                    Import from Nostr
                  </button>
                </div>
                <p className="text-xs text-theme-tertiary mt-3">
                  Requires a Nostr browser extension (Alby, nos2x, etc.)
                </p>
              </div>
            )}

            {/* About Tab */}
            {activeTab === 'about' && (
              <div>
                <h3 className="text-lg font-bold text-theme-primary mb-2">About</h3>
                <p className="text-sm text-theme-secondary">
                  Readstr - A sovereign RSS and Nostr feed reader by PrivKey (NIP-23)
                </p>
                <p className="text-xs text-theme-tertiary mt-3">
                  Changes to relays will be applied on next feed refresh
                </p>
              </div>
            )}
          </div>
        </div>

        {/* Footer */}
        <div className="border-t border-theme-primary p-4 bg-theme-tertiary">
          <div className="flex justify-end gap-3">
            <button
              onClick={onClose}
              className="btn-theme-primary"
            >
              Done
            </button>
          </div>
        </div>
      </div>
    </div>
  )
}
