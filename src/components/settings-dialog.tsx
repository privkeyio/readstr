'use client'

import { useState, useEffect, useRef } from 'react'
import { api } from '@/trpc/react'
import { useNostrAuth } from '@/contexts/NostrAuthContext'
import { useTheme, FONT_OPTIONS, FONT_STACKS, type FontKey } from '@/contexts/ThemeContext'
import {
  publishSubscriptionList,
  fetchSubscriptionList,
  buildSubscriptionListFromFeeds,
  mergeSubscriptionLists,
  getLastSyncTime,
  setLastSyncTime,
  advanceSyncWatermarkIfFresh,
  getPrefsSyncEnabled,
  setPrefsSyncEnabled,
  type SubscriptionList,
} from '@/lib/nostr-sync'
import { parseOpml, buildOpml, planOpmlImport, MAX_CONTENT_BYTES } from '@/lib/opml'
import { useAiConfig, AI_LANG_OPTIONS, ON_DEVICE_MODELS } from '@/lib/ai/config'
import {
  loadFilterRules,
  saveFilterRules,
  newRuleId,
  isValidRegex,
  type FilterRule,
  type MatchTarget,
  type MatchType,
  type FilterAction,
} from '@/lib/keyword-filter'
import type { SavedView } from '@/lib/saved-views'

export type MarkReadBehavior = 'on-open' | 'after-10s' | 'never'
export type LayoutMode = 'split' | 'single' | 'grid'
export type OrganizationMode = 'tags' | 'categories'

// Settings tabs
type SettingsTab = 'relays' | 'organization' | 'reading' | 'filters' | 'views' | 'ai' | 'sync' | 'about'

const SETTINGS_TABS: { id: SettingsTab; label: string; icon: string }[] = [
  { id: 'relays', label: 'Nostr Relays', icon: '🔌' },
  { id: 'organization', label: 'Feed Organization', icon: '📁' },
  { id: 'reading', label: 'Reading', icon: '📖' },
  { id: 'filters', label: 'Filters', icon: '🧹' },
  { id: 'views', label: 'Views', icon: '🗂️' },
  { id: 'ai', label: 'AI', icon: '✨' },
  { id: 'sync', label: 'Sync', icon: '🔄' },
  { id: 'about', label: 'About', icon: 'ℹ️' },
]

const SETTINGS_LANG_OPTIONS = AI_LANG_OPTIONS.map((o) =>
  o.value === 'auto' ? { label: 'Original (article language)', value: o.value } : o
)

function isInsecureAiUrl(baseUrl: string): boolean {
  try {
    const u = new URL(baseUrl)
    if (u.protocol === 'https:') return false
    if (u.hostname === 'localhost' || u.hostname === '127.0.0.1' || u.hostname === '[::1]') return false
    return true
  } catch {
    return false
  }
}

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

const LAYOUT_OPTIONS: { label: string; value: LayoutMode }[] = [
  { label: 'Split', value: 'split' },
  { label: 'Single', value: 'single' },
  { label: 'Grid', value: 'grid' },
]

const LINE_HEIGHT_OPTIONS: { label: string; value: number }[] = [
  { label: 'Compact', value: 1.5 },
  { label: 'Normal', value: 1.75 },
  { label: 'Relaxed', value: 2.0 },
]

const MEASURE_OPTIONS: { label: string; value: string }[] = [
  { label: 'Narrow', value: '40rem' },
  { label: 'Normal', value: '48rem' },
  { label: 'Wide', value: '60rem' },
  { label: 'Full', value: 'none' },
]

const PARA_GAP_OPTIONS: { label: string; value: string }[] = [
  { label: 'Compact', value: '0.75em' },
  { label: 'Normal', value: '1.25em' },
  { label: 'Relaxed', value: '1.75em' },
]

function Segmented<T extends string | number>({
  options,
  value,
  onChange,
}: {
  options: { label: string; value: T }[]
  value: T
  onChange: (value: T) => void
}) {
  return (
    <div className="flex gap-2">
      {options.map((option) => (
        <button
          key={String(option.value)}
          onClick={() => onChange(option.value)}
          className={`flex-1 px-3 py-2 rounded-lg text-sm font-medium border-2 transition-all duration-200 ${
            value === option.value
              ? 'border-theme-accent bg-theme-accent-light text-theme-primary shadow-theme-sm'
              : 'border-theme-secondary bg-theme-primary text-theme-secondary hover:border-theme-accent/50'
          }`}
        >
          {option.label}
        </button>
      ))}
    </div>
  )
}

function FontPicker({
  label,
  value,
  onChange,
}: {
  label: string
  value: FontKey
  onChange: (key: FontKey) => void
}) {
  return (
    <div className="mb-6">
      <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
        {label}
      </label>
      <div className="grid grid-cols-2 sm:grid-cols-3 gap-2">
        {FONT_OPTIONS.map((option) => (
          <button
            key={option.key}
            onClick={() => onChange(option.key)}
            style={option.key !== 'default' ? { fontFamily: FONT_STACKS[option.key] } : undefined}
            className={`px-3 py-2 rounded-lg text-sm font-medium border-2 transition-all duration-200 ${
              value === option.key
                ? 'border-theme-accent bg-theme-accent-light text-theme-primary shadow-theme-sm'
                : 'border-theme-secondary bg-theme-primary text-theme-secondary hover:border-theme-accent/50'
            }`}
          >
            {option.label}
          </button>
        ))}
      </div>
    </div>
  )
}

interface SettingsDialogProps {
  isOpen: boolean
  onClose: () => void
  markReadBehavior: MarkReadBehavior
  onChangeMarkReadBehavior: (behavior: MarkReadBehavior) => void
  layoutMode: LayoutMode
  onChangeLayoutMode: (mode: LayoutMode) => void
  organizationMode: OrganizationMode
  onChangeOrganizationMode: (mode: OrganizationMode) => void
  feeds?: Array<{ type: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'; url: string; title?: string; tags?: string[]; category?: { name: string; color?: string | null; icon?: string | null } | null }>
  userPubkey?: string
  onImportFeeds?: (feeds: Array<{ type: 'RSS' | 'NOSTR'; url: string; tags?: string[]; category?: { name: string; color?: string; icon?: string } }>) => Promise<void>
  onFilterRulesChange?: () => void
  views?: SavedView[]
  onRenameView?: (id: string, name: string) => void
  onDeleteView?: (id: string) => void
  onMoveView?: (index: number, direction: -1 | 1) => void
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

export function SettingsDialog({ isOpen, onClose, markReadBehavior, onChangeMarkReadBehavior, layoutMode, onChangeLayoutMode, organizationMode, onChangeOrganizationMode, feeds = [], userPubkey, onImportFeeds, onFilterRulesChange, views = [], onRenameView, onDeleteView, onMoveView }: SettingsDialogProps) {
  const { user, canSign, signEventOrThrow } = useNostrAuth()
  const { readingPrefs, setReadingPref, resetReading } = useTheme()
  const { config: aiConfig, setConfig: setAiConfig } = useAiConfig()
  const [activeTab, setActiveTab] = useState<SettingsTab>('relays')
  const [relays, setRelays] = useState<Relay[]>([])
  const [newRelayUrl, setNewRelayUrl] = useState('')
  const [error, setError] = useState('')
  const [syncState, setSyncState] = useState<SyncState>({
    status: 'idle',
    lastSync: null,
  })
  const [prefsSyncEnabled, setPrefsSyncEnabledState] = useState(false)

  // Category management state
  const [newCategoryName, setNewCategoryName] = useState('')
  const [newCategoryColor, setNewCategoryColor] = useState(CATEGORY_COLORS[0])
  const [newCategoryIcon, setNewCategoryIcon] = useState('📁')
  const [editingCategory, setEditingCategory] = useState<Category | null>(null)
  const [categoryError, setCategoryError] = useState('')

  // Saved views management state
  const [editingViewId, setEditingViewId] = useState<string | null>(null)
  const [editViewName, setEditViewName] = useState('')

  const sourceLabel = (source: SavedView['source']): string => {
    switch (source.kind) {
      case 'all':
        return 'All items'
      case 'feed':
        return 'Single feed'
      case 'tags':
        return `Tags: ${source.tags.join(', ')}`
      case 'category':
        return 'Category'
      case 'favorites':
        return 'Saved'
    }
  }

  // Local keyword filter state
  const [filterRules, setFilterRulesState] = useState<FilterRule[]>([])
  const [editingRuleId, setEditingRuleId] = useState<string | null>(null)
  const emptyRuleDraft = {
    target: 'any' as MatchTarget,
    type: 'contains' as MatchType,
    pattern: '',
    caseSensitive: false,
    action: 'hide' as FilterAction,
    color: CATEGORY_COLORS[0]!,
  }
  const [ruleDraft, setRuleDraft] = useState(emptyRuleDraft)
  const [dragRuleIndex, setDragRuleIndex] = useState<number | null>(null)
  const draftRegexInvalid =
    ruleDraft.type === 'regex' && !isValidRegex(ruleDraft.pattern.trim(), ruleDraft.caseSensitive)

  useEffect(() => {
    if (isOpen) setFilterRulesState(loadFilterRules())
  }, [isOpen])

  const persistFilterRules = (rules: FilterRule[]) => {
    const reordered = rules.map((r, i) => ({ ...r, order: i }))
    saveFilterRules(reordered)
    setFilterRulesState(reordered)
    onFilterRulesChange?.()
  }

  const resetRuleDraft = () => {
    setRuleDraft(emptyRuleDraft)
    setEditingRuleId(null)
  }

  const handleSaveRule = () => {
    const pattern = ruleDraft.pattern.trim()
    if (!pattern) return
    if (ruleDraft.type === 'regex' && !isValidRegex(pattern, ruleDraft.caseSensitive)) return
    const color = ruleDraft.action === 'highlight' ? ruleDraft.color : undefined
    if (editingRuleId) {
      persistFilterRules(
        filterRules.map((r) =>
          r.id === editingRuleId ? { ...r, ...ruleDraft, pattern, color } : r
        )
      )
    } else {
      const newRule: FilterRule = {
        id: newRuleId(),
        enabled: true,
        order: filterRules.length,
        target: ruleDraft.target,
        type: ruleDraft.type,
        pattern,
        caseSensitive: ruleDraft.caseSensitive,
        action: ruleDraft.action,
        color,
      }
      persistFilterRules([...filterRules, newRule])
    }
    resetRuleDraft()
  }

  const handleEditRule = (rule: FilterRule) => {
    setEditingRuleId(rule.id)
    setRuleDraft({
      target: rule.target,
      type: rule.type,
      pattern: rule.pattern,
      caseSensitive: rule.caseSensitive,
      action: rule.action,
      color: rule.color ?? CATEGORY_COLORS[0]!,
    })
  }

  const handleToggleRule = (id: string) => {
    persistFilterRules(filterRules.map((r) => (r.id === id ? { ...r, enabled: !r.enabled } : r)))
  }

  const handleDeleteRule = (id: string) => {
    persistFilterRules(filterRules.filter((r) => r.id !== id))
    if (editingRuleId === id) resetRuleDraft()
  }

  const handleMoveRule = (index: number, direction: -1 | 1) => {
    const target = index + direction
    if (target < 0 || target >= filterRules.length) return
    const next = [...filterRules]
    const [moved] = next.splice(index, 1)
    next.splice(target, 0, moved!)
    persistFilterRules(next)
  }

  const handleDropRule = (targetIndex: number) => {
    const from = dragRuleIndex
    setDragRuleIndex(null)
    if (from === null || from === targetIndex) return
    const next = [...filterRules]
    const [moved] = next.splice(from, 1)
    next.splice(from < targetIndex ? targetIndex - 1 : targetIndex, 0, moved!)
    persistFilterRules(next)
  }

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

    setPrefsSyncEnabledState(getPrefsSyncEnabled())
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

  // OPML import/export
  const opmlInputRef = useRef<HTMLInputElement>(null)
  const [opmlResult, setOpmlResult] = useState<{ imported: number; skipped: number; failed: number } | null>(null)
  const [opmlBusy, setOpmlBusy] = useState(false)

  const handleExportOpml = () => {
    const xml = buildOpml(feeds)
    const blob = new Blob([xml], { type: 'text/x-opml' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = 'readstr-subscriptions.opml'
    document.body.appendChild(a)
    a.click()
    a.remove()
    URL.revokeObjectURL(url)
  }

  const handleImportOpml = async (event: React.ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0]
    event.target.value = ''
    if (!file || !onImportFeeds) return

    setOpmlResult(null)

    if (file.size > MAX_CONTENT_BYTES) {
      setOpmlResult({ imported: 0, skipped: 0, failed: 0 })
      alert('That OPML file is too large to import (max 8 MB).')
      return
    }

    setOpmlBusy(true)
    try {
      const parsed = parseOpml(await file.text())
      const { toAdd, skipped } = planOpmlImport(parsed, feeds, organizationMode)

      if (toAdd.length === 0) {
        setOpmlResult({ imported: 0, skipped, failed: 0 })
        return
      }

      try {
        await onImportFeeds(toAdd)
        setOpmlResult({ imported: toAdd.length, skipped, failed: 0 })
      } catch (error) {
        const failure = (error as { importFailure?: { failed: number; total: number } })?.importFailure
        const failed = failure?.failed ?? toAdd.length
        setOpmlResult({ imported: toAdd.length - failed, skipped, failed })
      }
    } finally {
      setOpmlBusy(false)
    }
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
                <div className="flex items-center justify-between mb-2">
                  <h3 className="text-lg font-bold text-theme-primary">Typography</h3>
                  <button
                    onClick={() => {
                      if (confirm('Reset typography to defaults? This will discard your reading preferences.')) {
                        resetReading()
                      }
                    }}
                    className="text-sm font-medium text-theme-accent hover:underline"
                  >
                    Reset to Defaults
                  </button>
                </div>
                <p className="text-sm text-theme-secondary mb-4">
                  Adjust how articles read on this device. Saved locally to your browser.
                </p>

                {/* Live preview */}
                <div className="prose-theme mb-6 p-4 rounded-xl border-2 border-theme-secondary bg-theme-primary overflow-hidden">
                  <h3 className="!mt-0">The quick brown fox</h3>
                  <p className="!mb-0">
                    Jumps over the lazy dog. This sample paragraph reflects your font, size,
                    line height, and spacing choices as you make them.
                  </p>
                </div>

                {/* Font scale */}
                <div className="mb-6">
                  <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                    Font Size
                  </label>
                  <div className="flex items-center gap-3">
                    <span className="text-theme-tertiary" style={{ fontSize: '0.85rem' }}>A</span>
                    <input
                      type="range"
                      min={0.85}
                      max={1.4}
                      step={0.05}
                      value={readingPrefs.scale}
                      onChange={(e) => setReadingPref({ scale: parseFloat(e.target.value) })}
                      className="flex-1 accent-[rgb(var(--color-accent))]"
                    />
                    <span className="text-theme-primary" style={{ fontSize: '1.4rem' }}>A</span>
                    <span className="text-sm text-theme-tertiary w-10 text-right tabular-nums">
                      {Math.round(readingPrefs.scale * 100)}%
                    </span>
                  </div>
                </div>

                {/* Content font */}
                <FontPicker
                  label="Body Font"
                  value={readingPrefs.contentFont}
                  onChange={(key) => setReadingPref({ contentFont: key })}
                />

                {/* Heading font */}
                <FontPicker
                  label="Heading Font"
                  value={readingPrefs.headingFont}
                  onChange={(key) => setReadingPref({ headingFont: key })}
                />

                {/* Line height */}
                <div className="mb-6">
                  <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                    Line Height
                  </label>
                  <Segmented
                    options={LINE_HEIGHT_OPTIONS}
                    value={readingPrefs.lineHeight}
                    onChange={(value) => setReadingPref({ lineHeight: value })}
                  />
                </div>

                {/* Measure */}
                <div className="mb-6">
                  <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                    Content Width
                  </label>
                  <Segmented
                    options={MEASURE_OPTIONS}
                    value={readingPrefs.measure}
                    onChange={(value) => setReadingPref({ measure: value })}
                  />
                </div>

                {/* Paragraph spacing */}
                <div className="mb-8">
                  <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                    Paragraph Spacing
                  </label>
                  <Segmented
                    options={PARA_GAP_OPTIONS}
                    value={readingPrefs.paraGap}
                    onChange={(value) => setReadingPref({ paraGap: value })}
                  />
                </div>

                <h3 className="text-lg font-bold text-theme-primary mb-2">Layout</h3>
                <p className="text-sm text-theme-secondary mb-3">
                  How the item list and article panes arrange on larger screens.
                </p>
                <div className="mb-8">
                  <Segmented
                    options={LAYOUT_OPTIONS}
                    value={layoutMode}
                    onChange={onChangeLayoutMode}
                  />
                </div>

                <h3 className="text-lg font-bold text-theme-primary mb-2">Mark as Read</h3>
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

            {/* Filters Tab */}
            {activeTab === 'filters' && (
              <div>
                <h3 className="text-lg font-bold text-theme-primary mb-2">Keyword Filters</h3>
                <p className="text-sm text-theme-secondary mb-2">
                  Hide or highlight items by keyword. Rules run entirely in your browser and are never sent to any server or relay.
                </p>
                <p className="text-xs text-theme-tertiary mb-6">
                  Author matching uses the raw author identifier, which is a pubkey for Nostr feeds — not a display name.
                </p>

                {/* Add / Edit Rule */}
                <div className="mb-6 p-4 bg-theme-tertiary rounded-xl">
                  <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                    {editingRuleId ? 'Edit Rule' : 'New Rule'}
                  </label>

                  <input
                    type="text"
                    value={ruleDraft.pattern}
                    onChange={(e) => setRuleDraft({ ...ruleDraft, pattern: e.target.value })}
                    placeholder="Keyword or phrase"
                    maxLength={200}
                    className="input-theme w-full mb-3"
                  />

                  <div className="grid grid-cols-2 gap-2 mb-3">
                    <div>
                      <label className="block text-xs text-theme-tertiary mb-1 uppercase tracking-wider">Match in</label>
                      <select
                        value={ruleDraft.target}
                        onChange={(e) => setRuleDraft({ ...ruleDraft, target: e.target.value as MatchTarget })}
                        className="input-theme w-full"
                      >
                        <option value="any">Anywhere</option>
                        <option value="title">Title</option>
                        <option value="content">Content</option>
                        <option value="author">Author</option>
                      </select>
                    </div>
                    <div>
                      <label className="block text-xs text-theme-tertiary mb-1 uppercase tracking-wider">Match type</label>
                      <select
                        value={ruleDraft.type}
                        onChange={(e) => setRuleDraft({ ...ruleDraft, type: e.target.value as MatchType })}
                        className="input-theme w-full"
                      >
                        <option value="contains">Contains</option>
                        <option value="word">Whole word</option>
                        <option value="regex">Regex</option>
                      </select>
                    </div>
                    <div>
                      <label className="block text-xs text-theme-tertiary mb-1 uppercase tracking-wider">Action</label>
                      <select
                        value={ruleDraft.action}
                        onChange={(e) => setRuleDraft({ ...ruleDraft, action: e.target.value as FilterAction })}
                        className="input-theme w-full"
                      >
                        <option value="hide">Hide</option>
                        <option value="highlight">Highlight</option>
                        <option value="mark-read">Auto-mark read</option>
                      </select>
                    </div>
                    <label className="flex items-end gap-2 pb-2 text-sm text-theme-secondary cursor-pointer">
                      <input
                        type="checkbox"
                        checked={ruleDraft.caseSensitive}
                        onChange={(e) => setRuleDraft({ ...ruleDraft, caseSensitive: e.target.checked })}
                      />
                      Case sensitive
                    </label>
                  </div>

                  {ruleDraft.type === 'regex' && ruleDraft.pattern.trim() && draftRegexInvalid && (
                    <p className="mb-3 text-sm text-red-500">
                      Invalid or unsafe regex — this pattern will never match. Avoid nested quantifiers like (a+)+.
                    </p>
                  )}

                  {ruleDraft.action === 'mark-read' && (
                    <p className="mb-3 text-xs text-amber-600 dark:text-amber-400 bg-amber-500/10 rounded-lg p-2">
                      ⚠️ Auto-mark read marks matching items as read. Unlike hide and highlight (local-only),
                      read status SYNCS to a PUBLIC Nostr event (kind 30405) across your devices.
                    </p>
                  )}

                  {ruleDraft.action === 'highlight' && (
                    <div className="mb-3">
                      <label className="block text-xs text-theme-tertiary mb-1 uppercase tracking-wider">Color</label>
                      <div className="flex gap-2">
                        {CATEGORY_COLORS.map((color) => (
                          <button
                            key={color}
                            onClick={() => setRuleDraft({ ...ruleDraft, color })}
                            className={`w-7 h-7 rounded-full transition-all ${
                              ruleDraft.color === color ? 'ring-2 ring-offset-2 ring-theme-accent scale-110' : 'hover:scale-110'
                            }`}
                            style={{ backgroundColor: color }}
                          />
                        ))}
                      </div>
                    </div>
                  )}

                  <div className="flex gap-2">
                    <button
                      onClick={handleSaveRule}
                      disabled={!ruleDraft.pattern.trim() || draftRegexInvalid}
                      className="btn-theme-primary text-sm disabled:opacity-50"
                    >
                      {editingRuleId ? 'Save Changes' : 'Add Rule'}
                    </button>
                    {editingRuleId && (
                      <button onClick={resetRuleDraft} className="btn-theme-secondary text-sm">
                        Cancel
                      </button>
                    )}
                  </div>
                </div>

                {/* Rules List */}
                <div>
                  <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                    Your Rules ({filterRules.length})
                  </label>
                  {filterRules.length === 0 ? (
                    <div className="text-center py-6 text-theme-tertiary text-sm">
                      No filter rules yet. Add one above to hide or highlight items.
                    </div>
                  ) : (
                    <div className="space-y-2">
                      {filterRules.map((rule, index) => (
                        <div
                          key={rule.id}
                          draggable
                          onDragStart={() => setDragRuleIndex(index)}
                          onDragOver={(e) => e.preventDefault()}
                          onDrop={() => handleDropRule(index)}
                          onDragEnd={() => setDragRuleIndex(null)}
                          className={`flex items-center justify-between gap-3 p-3 bg-theme-tertiary rounded-xl ${
                            dragRuleIndex === index ? 'opacity-50' : ''
                          }`}
                        >
                          <div className="flex items-center gap-3 min-w-0">
                            <span className="text-theme-tertiary cursor-grab select-none flex-shrink-0" title="Drag to reorder">⋮⋮</span>
                            <input
                              type="checkbox"
                              checked={rule.enabled}
                              onChange={() => handleToggleRule(rule.id)}
                              title={rule.enabled ? 'Enabled' : 'Disabled'}
                            />
                            {rule.action === 'highlight' && (
                              <span
                                className="w-4 h-4 rounded-full flex-shrink-0"
                                style={{ backgroundColor: rule.color ?? '#94a3b8' }}
                              />
                            )}
                            <div className="min-w-0">
                              <div className="font-medium text-theme-primary truncate">
                                {rule.pattern}
                                {rule.type === 'regex' && !isValidRegex(rule.pattern, rule.caseSensitive) && (
                                  <span className="ml-2 text-xs text-red-500">(invalid)</span>
                                )}
                              </div>
                              <div className="text-xs text-theme-tertiary">
                                {rule.action === 'hide' ? 'Hide' : rule.action === 'mark-read' ? 'Auto-mark read' : 'Highlight'} · {rule.type === 'word' ? 'whole word' : rule.type === 'regex' ? 'regex' : 'contains'} · {rule.target}
                                {rule.caseSensitive ? ' · case' : ''}
                              </div>
                            </div>
                          </div>
                          <div className="flex gap-1 flex-shrink-0">
                            <button
                              onClick={() => handleMoveRule(index, -1)}
                              disabled={index === 0}
                              className="p-2 text-theme-secondary hover:bg-theme-hover rounded-lg transition-colors disabled:opacity-30"
                              title="Move up"
                            >
                              ↑
                            </button>
                            <button
                              onClick={() => handleMoveRule(index, 1)}
                              disabled={index === filterRules.length - 1}
                              className="p-2 text-theme-secondary hover:bg-theme-hover rounded-lg transition-colors disabled:opacity-30"
                              title="Move down"
                            >
                              ↓
                            </button>
                            <button
                              onClick={() => handleEditRule(rule)}
                              className="p-2 text-theme-secondary hover:bg-theme-hover rounded-lg transition-colors"
                              title="Edit rule"
                            >
                              ✏️
                            </button>
                            <button
                              onClick={() => handleDeleteRule(rule.id)}
                              className="p-2 text-red-500 hover:bg-red-50 rounded-lg transition-colors"
                              title="Delete rule"
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

            {activeTab === 'views' && (
              <div>
                <h3 className="text-lg font-bold text-theme-primary mb-2">Smart Views</h3>
                <p className="text-sm text-theme-secondary mb-6">
                  Saved filters shown as chips above your item list. Views are stored only in this browser and are never sent to any server or relay.
                </p>

                <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                  Your Views ({views.length})
                </label>
                {views.length === 0 ? (
                  <div className="text-center py-6 text-theme-tertiary text-sm">
                    No views yet. Use the “+” chip above your item list to save your current filter.
                  </div>
                ) : (
                  <div className="space-y-2">
                    {views.map((view, index) => (
                      <div
                        key={view.id}
                        className="flex items-center justify-between gap-3 p-3 bg-theme-tertiary rounded-xl"
                      >
                        <div className="flex items-center gap-3 min-w-0">
                          {view.icon && <span className="flex-shrink-0">{view.icon}</span>}
                          <div className="min-w-0">
                            {editingViewId === view.id ? (
                              <input
                                type="text"
                                value={editViewName}
                                autoFocus
                                maxLength={60}
                                onChange={(e) => setEditViewName(e.target.value)}
                                onKeyDown={(e) => {
                                  if (e.key === 'Enter' && editViewName.trim()) {
                                    onRenameView?.(view.id, editViewName.trim())
                                    setEditingViewId(null)
                                  }
                                  if (e.key === 'Escape') setEditingViewId(null)
                                }}
                                className="input-theme w-full"
                              />
                            ) : (
                              <>
                                <div className="font-medium text-theme-primary truncate">{view.name}</div>
                                <div className="text-xs text-theme-tertiary truncate">
                                  {sourceLabel(view.source)} · {view.readState} · {view.sort}
                                </div>
                              </>
                            )}
                          </div>
                        </div>
                        <div className="flex gap-1 flex-shrink-0">
                          {editingViewId === view.id ? (
                            <>
                              <button
                                onClick={() => {
                                  if (editViewName.trim()) onRenameView?.(view.id, editViewName.trim())
                                  setEditingViewId(null)
                                }}
                                className="p-2 text-theme-secondary hover:bg-theme-hover rounded-lg transition-colors"
                                title="Save name"
                              >
                                ✓
                              </button>
                              <button
                                onClick={() => setEditingViewId(null)}
                                className="p-2 text-theme-secondary hover:bg-theme-hover rounded-lg transition-colors"
                                title="Cancel"
                              >
                                ✕
                              </button>
                            </>
                          ) : (
                            <>
                              <button
                                onClick={() => onMoveView?.(index, -1)}
                                disabled={index === 0}
                                className="p-2 text-theme-secondary hover:bg-theme-hover rounded-lg transition-colors disabled:opacity-30"
                                title="Move up"
                              >
                                ↑
                              </button>
                              <button
                                onClick={() => onMoveView?.(index, 1)}
                                disabled={index === views.length - 1}
                                className="p-2 text-theme-secondary hover:bg-theme-hover rounded-lg transition-colors disabled:opacity-30"
                                title="Move down"
                              >
                                ↓
                              </button>
                              <button
                                onClick={() => {
                                  setEditingViewId(view.id)
                                  setEditViewName(view.name)
                                }}
                                className="p-2 text-theme-secondary hover:bg-theme-hover rounded-lg transition-colors"
                                title="Rename view"
                              >
                                ✏️
                              </button>
                              <button
                                onClick={() => onDeleteView?.(view.id)}
                                className="p-2 text-red-500 hover:bg-red-50 rounded-lg transition-colors"
                                title="Delete view"
                              >
                                🗑️
                              </button>
                            </>
                          )}
                        </div>
                      </div>
                    ))}
                  </div>
                )}
              </div>
            )}

            {/* AI Tab */}
            {activeTab === 'ai' && (
              <div>
                <h3 className="text-lg font-bold text-theme-primary mb-2">AI Summaries & Insights</h3>
                {aiConfig.provider === 'on-device' ? (
                  <>
                    <p className="text-sm text-theme-secondary mb-2">
                      Generate article summaries and insights with a model that runs entirely in your
                      browser, fully offline after the first download.
                    </p>
                    <p className="text-xs text-theme-tertiary mb-6">
                      Nothing is sent anywhere. The Readstr server never sees your prompt or the output,
                      and your article text never leaves your device.
                    </p>
                  </>
                ) : (
                  <>
                    <p className="text-sm text-theme-secondary mb-2">
                      Generate article summaries and insights using your own OpenAI-compatible endpoint
                      (Ollama, LM Studio, etc.).
                    </p>
                    <p className="text-xs text-theme-tertiary mb-6">
                      Your browser calls the endpoint directly. The Readstr server never sees your prompt,
                      API key, or the output. The API key is stored only in this browser and is never sent
                      to any relay or server.
                    </p>
                  </>
                )}

                <label className="flex items-start gap-3 p-4 mb-6 rounded-xl border-2 border-theme-secondary bg-theme-primary cursor-pointer">
                  <input
                    type="checkbox"
                    checked={aiConfig.enabled}
                    onChange={(e) => setAiConfig({ enabled: e.target.checked })}
                    className="mt-1 accent-[rgb(var(--color-accent))]"
                  />
                  <div>
                    <div className="font-medium text-theme-primary">Enable AI features</div>
                    <p className="text-sm text-theme-secondary">
                      Off by default. When disabled, no AI controls appear and no requests are made.
                    </p>
                  </div>
                </label>

                <div className="mb-4">
                  <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                    Provider
                  </label>
                  <div className="flex gap-2">
                    <button
                      type="button"
                      onClick={() => setAiConfig({ provider: 'endpoint' })}
                      className={`flex-1 px-3 py-2 rounded-lg text-sm font-medium border-2 transition-all ${
                        aiConfig.provider !== 'on-device'
                          ? 'border-theme-accent bg-theme-accent-light text-theme-primary'
                          : 'border-theme-secondary bg-theme-primary text-theme-secondary hover:border-theme-accent/50'
                      }`}
                    >
                      Custom endpoint
                    </button>
                    <button
                      type="button"
                      onClick={() => setAiConfig({ provider: 'on-device' })}
                      className={`flex-1 px-3 py-2 rounded-lg text-sm font-medium border-2 transition-all ${
                        aiConfig.provider === 'on-device'
                          ? 'border-theme-accent bg-theme-accent-light text-theme-primary'
                          : 'border-theme-secondary bg-theme-primary text-theme-secondary hover:border-theme-accent/50'
                      }`}
                    >
                      On-device (offline)
                    </button>
                  </div>
                </div>

                {aiConfig.provider === 'on-device' ? (
                  <div className="mb-4">
                    <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                      Model
                    </label>
                    <select
                      value={aiConfig.deviceModel}
                      onChange={(e) => setAiConfig({ deviceModel: e.target.value })}
                      className="input-theme w-full"
                    >
                      {ON_DEVICE_MODELS.map((m) => (
                        <option key={m.id} value={m.id}>
                          {m.label} ({m.sizeLabel})
                        </option>
                      ))}
                    </select>
                    <p className="mt-2 text-xs text-theme-tertiary">
                      The first run downloads the model from a CDN, then it works fully offline. The
                      model runs entirely in your browser and no article text leaves your device.
                    </p>
                    <p className="mt-1 text-xs text-theme-tertiary">
                      Requires a browser with WebGPU (Chrome, Edge, or recent Safari/Firefox).
                    </p>
                  </div>
                ) : (
                  <>
                    <div className="mb-4">
                      <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                        Base URL
                      </label>
                      <input
                        type="text"
                        value={aiConfig.baseUrl}
                        onChange={(e) => setAiConfig({ baseUrl: e.target.value })}
                        placeholder="http://localhost:11434/v1"
                        className="input-theme w-full"
                      />
                      {isInsecureAiUrl(aiConfig.baseUrl) && (
                        <div className="mt-2 flex items-start gap-2 text-sm text-yellow-700">
                          <span>⚠</span>
                          <span>
                            This is a non-localhost, non-HTTPS endpoint. Browsers block mixed content, so
                            requests from this page will fail. Use HTTPS or a localhost endpoint.
                          </span>
                        </div>
                      )}
                    </div>

                    <div className="mb-4">
                      <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                        API Key
                      </label>
                      <input
                        type="password"
                        value={aiConfig.apiKey}
                        onChange={(e) => setAiConfig({ apiKey: e.target.value })}
                        placeholder="Leave empty for local endpoints"
                        autoComplete="off"
                        className="input-theme w-full"
                      />
                    </div>

                    <div className="mb-4">
                      <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                        Model
                      </label>
                      <input
                        type="text"
                        value={aiConfig.model}
                        onChange={(e) => setAiConfig({ model: e.target.value })}
                        placeholder="llama3.2"
                        className="input-theme w-full"
                      />
                    </div>
                  </>
                )}

                <div className="mb-6">
                  <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                    Features
                  </label>
                  <div className="space-y-2">
                    <label className="flex items-center gap-3 text-sm text-theme-secondary cursor-pointer">
                      <input
                        type="checkbox"
                        checked={aiConfig.features.summarize}
                        onChange={(e) => setAiConfig({ features: { ...aiConfig.features, summarize: e.target.checked } })}
                        className="accent-[rgb(var(--color-accent))]"
                      />
                      Summaries
                    </label>
                    <label className="flex items-center gap-3 text-sm text-theme-secondary cursor-pointer">
                      <input
                        type="checkbox"
                        checked={aiConfig.features.insights}
                        onChange={(e) => setAiConfig({ features: { ...aiConfig.features, insights: e.target.checked } })}
                        className="accent-[rgb(var(--color-accent))]"
                      />
                      Insights
                    </label>
                    <label className="flex items-center gap-3 text-sm text-theme-secondary cursor-pointer">
                      <input
                        type="checkbox"
                        checked={aiConfig.features.translate}
                        onChange={(e) => setAiConfig({ features: { ...aiConfig.features, translate: e.target.checked } })}
                        className="accent-[rgb(var(--color-accent))]"
                      />
                      Translate
                    </label>
                  </div>
                </div>

                <div className="mb-4">
                  <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                    Output Language
                  </label>
                  <select
                    value={aiConfig.targetLang}
                    onChange={(e) => setAiConfig({ targetLang: e.target.value })}
                    className="input-theme w-full"
                  >
                    {SETTINGS_LANG_OPTIONS.map((o) => (
                      <option key={o.value} value={o.value}>
                        {o.label}
                      </option>
                    ))}
                  </select>
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

                {/* OPML import result */}
                {opmlResult && (
                  <div className="p-4 bg-yellow-50 rounded-xl mb-4">
                    <p className="font-medium text-yellow-800">
                      Imported {opmlResult.imported} · Skipped {opmlResult.skipped} (already subscribed) · Failed {opmlResult.failed}
                    </p>
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
                  <button
                    onClick={handleExportOpml}
                    disabled={feeds.length === 0}
                    className="flex items-center gap-2 px-4 py-2.5 border border-theme-accent text-theme-accent bg-transparent rounded-xl font-medium hover:bg-theme-accent-light disabled:opacity-50 disabled:cursor-not-allowed transition-all"
                  >
                    <span>📄</span>
                    Export OPML
                  </button>
                  <button
                    onClick={() => opmlInputRef.current?.click()}
                    disabled={opmlBusy || !userPubkey}
                    className="flex items-center gap-2 px-4 py-2.5 border border-theme-accent text-theme-accent bg-transparent rounded-xl font-medium hover:bg-theme-accent-light disabled:opacity-50 disabled:cursor-not-allowed transition-all"
                  >
                    <span>📥</span>
                    Import OPML
                  </button>
                  <input
                    ref={opmlInputRef}
                    type="file"
                    accept=".opml,.xml,text/x-opml,text/xml,application/xml"
                    onChange={handleImportOpml}
                    className="hidden"
                  />
                </div>
                <p className="text-xs text-theme-tertiary mt-3">
                  Requires a Nostr browser extension (Alby, nos2x, etc.)
                </p>

                <div className="mt-8 pt-6 border-t border-theme-primary">
                  <label className="flex items-center gap-3 text-sm font-medium text-theme-secondary cursor-pointer">
                    <input
                      type="checkbox"
                      checked={prefsSyncEnabled}
                      onChange={(e) => {
                        const next = e.target.checked
                        setPrefsSyncEnabled(next)
                        setPrefsSyncEnabledState(next)
                      }}
                      className="accent-[rgb(var(--color-accent))]"
                    />
                    Sync reading &amp; theme preferences across devices
                  </label>
                  <p className="text-xs text-theme-tertiary mt-2">
                    When on, your reading and theme preferences are published to your
                    Nostr relays (public, kind 30407) and applied on your other devices.
                    Local-only when off.
                  </p>
                </div>
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
