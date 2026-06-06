'use client'

import { useState, useEffect } from 'react'
import Link from 'next/link'
import { api } from '@/trpc/react'
import type { OrganizationMode } from './settings-dialog'

interface NostrProfile {
  npub: string
  name?: string
  displayName?: string
  about?: string
  picture?: string
  nip05?: string
  verified?: boolean
}

interface Category {
  id: string
  name: string
  color: string | null
  icon: string | null
}

interface AddFeedModalProps {
  isOpen: boolean
  onClose: () => void
  onAddFeed: (type: 'RSS' | 'NOSTR', url?: string, npub?: string, title?: string, tags?: string[], categoryId?: string) => void
  isLoading?: boolean
  error?: string
  organizationMode: OrganizationMode
  categories?: Category[]
}

export function AddFeedModal({ isOpen, onClose, onAddFeed, isLoading = false, error: externalError, organizationMode, categories = [] }: AddFeedModalProps) {
  const [feedType, setFeedType] = useState<'RSS' | 'NOSTR'>('RSS')
  const [rssUrl, setRssUrl] = useState('')
  const [nostrSearch, setNostrSearch] = useState('')
  const [manualNpub, setManualNpub] = useState('')
  const [showManualInput, setShowManualInput] = useState(false)
  const [searchResults, setSearchResults] = useState<NostrProfile[]>([])
  const [popularUsers, setPopularUsers] = useState<NostrProfile[]>([])
  const [internalError, setInternalError] = useState('')
  const [tagInput, setTagInput] = useState('')
  const [tags, setTags] = useState<string[]>([])
  const [selectedCategoryId, setSelectedCategoryId] = useState<string | null>(null)
  
  const error = externalError || internalError
  
  // Helper to get relays from localStorage
  const getRelays = (): string[] | undefined => {
    if (typeof window === 'undefined') return undefined
    const saved = localStorage.getItem('nostr_relays')
    if (saved) {
      try {
        const relays = JSON.parse(saved)
        return Array.isArray(relays) && relays.length > 0 ? relays : undefined
      } catch {
        return undefined
      }
    }
    return undefined
  }

  // tRPC mutations
  const searchProfilesMutation = api.feed.searchProfiles.useMutation({
    onSuccess: (data) => {
      // Deduplicate by npub
      const uniqueProfiles = data.profiles.reduce((acc, profile) => {
        if (!acc.find(p => p.npub === profile.npub)) {
          acc.push(profile)
        }
        return acc
      }, [] as NostrProfile[])
      setSearchResults(uniqueProfiles)
    },
    onError: (error) => {
      console.error('Profile search failed:', error)
      setSearchResults([])
    },
  })

  const getPopularUsersMutation = api.feed.getPopularUsers.useMutation({
    onSuccess: (data) => {
      // Deduplicate by npub
      const uniqueProfiles = data.profiles.reduce((acc, profile) => {
        if (!acc.find(p => p.npub === profile.npub)) {
          acc.push(profile)
        }
        return acc
      }, [] as NostrProfile[])
      setPopularUsers(uniqueProfiles)
    },
  })

  // Load popular users when modal opens and Nostr is selected
  useEffect(() => {
    if (isOpen && feedType === 'NOSTR' && popularUsers.length === 0) {
      getPopularUsersMutation.mutate({
        limit: 15,
        relays: getRelays()
      })
    }
    // tRPC mutation object is unstable across renders; intentionally fetch only when
    // the modal opens or feed type changes, not on every render.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [isOpen, feedType])

  // Debounced search
  useEffect(() => {
    if (!nostrSearch.trim() || nostrSearch.length < 2) {
      setSearchResults([])
      return
    }

    const timeoutId = setTimeout(() => {
      searchProfilesMutation.mutate({
        query: nostrSearch,
        limit: 10,
        relays: getRelays()
      })
    }, 500)

    return () => clearTimeout(timeoutId)
    // tRPC mutation object is unstable across renders; the search is intentionally
    // debounced on the query string only.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [nostrSearch])

  const handleAddFeed = () => {
    setInternalError('')
    if (feedType === 'RSS') {
      if (!rssUrl.trim()) return
      
      // Clear any previous errors and let tRPC mutation handle validation
      onAddFeed('RSS', rssUrl, undefined, undefined, tags, selectedCategoryId ?? undefined)
    } else {
      const npub = manualNpub.trim()
      if (!npub) {
        setInternalError('Please enter an npub or select a user')
        return
      }
      if (!npub.startsWith('npub1')) {
        setInternalError('Invalid npub format. Must start with npub1')
        return
      }
      onAddFeed('NOSTR', undefined, npub, undefined, tags, selectedCategoryId ?? undefined)
    }
    handleClose()
  }

  const handleSelectProfile = (profile: NostrProfile) => {
    console.log('🟢 Profile selected:', profile.name, 'npub:', profile.npub)
    setManualNpub(profile.npub)
    setShowManualInput(true)
    setNostrSearch('')
    setSearchResults([])
    console.log('🟢 After selection - showManualInput should be true, manualNpub:', profile.npub)
  }

  const handleAddTag = () => {
    const trimmed = tagInput.trim()
    if (trimmed && !tags.includes(trimmed)) {
      setTags([...tags, trimmed])
      setTagInput('')
    }
  }

  const handleRemoveTag = (tagToRemove: string) => {
    setTags(tags.filter(t => t !== tagToRemove))
  }

  const handleTagInputKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter') {
      e.preventDefault()
      handleAddTag()
    }
  }

  const handleClose = () => {
    onClose()
    setRssUrl('')
    setNostrSearch('')
    setManualNpub('')
    setShowManualInput(false)
    setSearchResults([])
    setTagInput('')
    setTags([])
    setSelectedCategoryId(null)
  }

  if (!isOpen) return null

  return (
    <div className="modal-overlay">
      <div className="modal-content w-full max-w-2xl mx-4 max-h-[90vh] overflow-hidden animate-slide-in">
        <div className="p-6 border-b border-theme-primary">
          <h3 className="text-xl font-bold text-theme-primary">Add New Feed</h3>
        </div>

        <div className="p-6 overflow-y-auto themed-scrollbar max-h-[calc(90vh-140px)]">
          {/* Feed Type Selection */}
          <div className="mb-6">
            <label className="block text-sm font-semibold text-theme-secondary mb-3 uppercase tracking-wider">
              Feed Type
            </label>
            <div className="flex gap-3">
              <button
                onClick={() => setFeedType('RSS')}
                className={`flex-1 px-4 py-3 rounded-xl text-sm font-medium transition-all duration-200 border-2 ${
                  feedType === 'RSS'
                    ? 'bg-theme-accent-light border-theme-accent text-theme-accent shadow-theme-sm'
                    : 'bg-theme-tertiary border-theme-secondary text-theme-secondary hover:border-theme-accent/50'
                }`}
              >
                <span className="text-lg mr-2">📰</span> RSS/YouTube Feed
              </button>
              <button
                onClick={() => setFeedType('NOSTR')}
                className={`flex-1 px-4 py-3 rounded-xl text-sm font-medium transition-all duration-200 border-2 ${
                  feedType === 'NOSTR'
                    ? 'bg-theme-accent-light border-theme-accent text-theme-accent shadow-theme-sm'
                    : 'bg-theme-tertiary border-theme-secondary text-theme-secondary hover:border-theme-accent/50'
                }`}
              >
                <span className="text-lg mr-2">⚡</span> Nostr (Articles & Videos)
              </button>
            </div>
          </div>

          {/* RSS Input */}
          {feedType === 'RSS' && (
            <div>
              <label className="block text-sm font-medium text-theme-secondary mb-2">
                RSS Feed URL
              </label>
              {error && (
                <div className="mb-3 rounded-lg bg-red-50 p-4 border border-red-200">
                  <p className="text-sm text-red-800 flex items-center gap-2">
                    <svg className="w-4 h-4" fill="currentColor" viewBox="0 0 20 20">
                      <path fillRule="evenodd" d="M10 18a8 8 0 100-16 8 8 0 000 16zM8.707 7.293a1 1 0 00-1.414 1.414L8.586 10l-1.293 1.293a1 1 0 101.414 1.414L10 11.414l1.293 1.293a1 1 0 001.414-1.414L11.414 10l1.293-1.293a1 1 0 00-1.414-1.414L10 8.586 8.707 7.293z" clipRule="evenodd" />
                    </svg>
                    {error}
                  </p>
                </div>
              )}
              <input
                type="url"
                value={rssUrl}
                onChange={(e) => {
                  setRssUrl(e.target.value)
                  setInternalError('')
                }}
                placeholder="https://example.com/feed.xml"
                className="input-theme"
              />
              <p className="mt-2 text-xs text-theme-tertiary">
                Enter a feed URL or website homepage - we&apos;ll find the feed for you
              </p>
            </div>
          )}

          {/* Nostr Profile Search */}
          {feedType === 'NOSTR' && (
            <div className="space-y-4">
              {error && (
                <div className="rounded-lg bg-red-50 p-4 border border-red-200">
                  <p className="text-sm text-red-800 flex items-center gap-2">
                    <svg className="w-4 h-4" fill="currentColor" viewBox="0 0 20 20">
                      <path fillRule="evenodd" d="M10 18a8 8 0 100-16 8 8 0 000 16zM8.707 7.293a1 1 0 00-1.414 1.414L8.586 10l-1.293 1.293a1 1 0 101.414 1.414L10 11.414l1.293 1.293a1 1 0 001.414-1.414L11.414 10l1.293-1.293a1 1 0 00-1.414-1.414L10 8.586 8.707 7.293z" clipRule="evenodd" />
                    </svg>
                    {error}
                  </p>
                </div>
              )}
              <div className="text-center p-4 bg-theme-tertiary rounded-xl">
                <p className="text-sm text-theme-secondary">
                  Looking for new content creators?{' '}
                  <Link
                    href="/guide"
                    onClick={handleClose}
                    className="font-semibold text-theme-accent hover:underline"
                  >
                    Check out the Readstr Guide
                  </Link>
                  {' '}to generate RSS feeds from any npub.
                </p>
              </div>
              {!showManualInput ? (
                <>
                  {/* Search Input */}
                  <div>
                    <label className="block text-sm font-medium text-theme-secondary mb-2">
                      Search for User to Subscribe
                    </label>
                    <input
                      type="text"
                      value={nostrSearch}
                      onChange={(e) => setNostrSearch(e.target.value)}
                      placeholder="Search by name, NIP-05, or npub..."
                      className="input-theme"
                    />
                  </div>

                  {/* Search Results */}
                  {searchResults.length > 0 && (
                    <div>
                      <h4 className="text-sm font-medium text-theme-secondary mb-2">Search Results</h4>
                      <div className="space-y-2 max-h-48 overflow-y-auto themed-scrollbar">
                        {searchResults.map((profile, index) => (
                          <button
                            key={`search-${profile.npub}-${index}`}
                            onClick={() => handleSelectProfile(profile)}
                            className="w-full p-3 text-left border border-theme-primary rounded-xl hover:bg-theme-hover transition-colors"
                          >
                            <div className="flex items-center gap-3">
                              {profile.picture && (
                                // eslint-disable-next-line @next/next/no-img-element -- remote profile avatar from arbitrary domains, not suited to next/image
                                <img
                                  src={profile.picture}
                                  alt={profile.name || 'User'}
                                  className="w-10 h-10 rounded-full object-cover"
                                />
                              )}
                              <div className="flex-1 min-w-0">
                                <div className="flex items-center gap-2">
                                  <p className="font-medium text-theme-primary truncate">
                                    {profile.displayName || profile.name || 'Unknown'}
                                  </p>
                                  {profile.nip05 && (
                                    <span className="text-xs bg-green-100 text-green-700 px-2 py-0.5 rounded-full">
                                      {profile.nip05}
                                    </span>
                                  )}
                                </div>
                                <p className="text-sm text-theme-tertiary truncate">
                                  {profile.about || profile.npub.slice(0, 20) + '...'}
                                </p>
                              </div>
                            </div>
                          </button>
                        ))}
                      </div>
                    </div>
                  )}

                  {/* Popular Users */}
                  {nostrSearch.length === 0 && popularUsers.length > 0 && (
                    <div>
                      <h4 className="text-sm font-medium text-theme-secondary mb-2">Discover Users</h4>
                      <div className="grid grid-cols-1 gap-2 max-h-64 overflow-y-auto themed-scrollbar">
                        {popularUsers.map((profile, index) => (
                          <button
                            key={`popular-${profile.npub}-${index}`}
                            onClick={() => handleSelectProfile(profile)}
                            className="p-3 text-left border border-theme-primary rounded-xl hover:bg-theme-hover transition-colors"
                          >
                            <div className="flex items-center gap-3">
                              {profile.picture && (
                                // eslint-disable-next-line @next/next/no-img-element -- remote profile avatar from arbitrary domains, not suited to next/image
                                <img
                                  src={profile.picture}
                                  alt={profile.name || 'User'}
                                  className="w-8 h-8 rounded-full object-cover"
                                />
                              )}
                              <div className="flex-1 min-w-0">
                                <div className="flex items-center gap-2">
                                  <p className="font-medium text-theme-primary truncate text-sm">
                                    {profile.displayName || profile.name || 'Unknown'}
                                  </p>
                                  {profile.nip05 && (
                                    <span className="text-xs bg-green-100 text-green-700 px-1.5 py-0.5 rounded-full">
                                      ✓
                                    </span>
                                  )}
                                </div>
                                {profile.about && (
                                  <p className="text-xs text-theme-tertiary truncate">
                                    {profile.about.slice(0, 60)}...
                                  </p>
                                )}
                              </div>
                            </div>
                          </button>
                        ))}
                      </div>
                    </div>
                  )}

                  {/* Manual Entry Option */}
                  <div className="pt-4 border-t border-theme-primary">
                    <button
                      onClick={() => setShowManualInput(true)}
                      aria-label="Enter npub manually"
                      className="w-full flex items-center justify-center gap-2 px-4 py-3 bg-theme-accent-light border-2 border-theme-accent/30 text-sm font-medium text-theme-accent rounded-xl hover:border-theme-accent transition-all"
                    >
                      <svg xmlns="http://www.w3.org/2000/svg" className="h-4 w-4" viewBox="0 0 20 20" fill="currentColor" aria-hidden="true">
                        <path d="M2.003 5.884l8 4.8a1 1 0 0 0 .994 0l8-4.8A1 1 0 0 0 18 4H2a1 1 0 0 0 .003 1.884z" />
                        <path d="M18 8.118l-8 4.8a3 3 0 0 1-2.994 0l-8-4.8V14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V8.118z" />
                      </svg>
                      <span>Enter npub manually</span>
                    </button>
                    <p className="mt-2 text-xs text-theme-tertiary text-center">Paste an npub (npub1...) to add a Nostr user directly</p>
                  </div>
                </>
              ) : (
                <>
                  {/* Manual NPub Input */}
                  <div>
                    <label className="block text-sm font-medium text-theme-secondary mb-2">
                      Nostr npub
                    </label>
                    <input
                      type="text"
                      value={manualNpub}
                      onChange={(e) => {
                        const value = e.target.value
                        console.log('Npub input changed:', value)
                        console.log('Starts with npub1?', value.startsWith('npub1'))
                        setManualNpub(value)
                        setInternalError('') // Clear errors when typing
                      }}
                      placeholder="npub1..."
                      className="input-theme"
                    />
                    <p className="mt-2 text-xs text-theme-tertiary">
                      Enter the full npub of the Nostr user you want to follow
                    </p>
                  </div>
                  <button
                    onClick={() => {
                      setShowManualInput(false)
                      setManualNpub('')
                    }}
                    className="text-sm font-medium text-theme-accent hover:underline"
                  >
                    ← Back to search
                  </button>
                </>
              )}
            </div>
          )}

          {/* Category Selection - shown when using categories mode */}
          {organizationMode === 'categories' && (
            <div className="mt-6">
              <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                Category (optional)
              </label>
              <select
                value={selectedCategoryId ?? ''}
                onChange={(e) => setSelectedCategoryId(e.target.value || null)}
                className="input-theme cursor-pointer"
              >
                <option value="">No category</option>
                {categories.map((cat) => (
                  <option key={cat.id} value={cat.id}>
                    {cat.icon || '📁'} {cat.name}
                  </option>
                ))}
              </select>
              {categories.length === 0 && (
                <p className="mt-2 text-xs text-theme-tertiary">
                  No categories yet. Create them in Settings.
                </p>
              )}
            </div>
          )}

          {/* Tags Input - shown when using tags mode */}
          {organizationMode === 'tags' && (
            <div className="mt-6">
              <label className="block text-sm font-semibold text-theme-secondary mb-2 uppercase tracking-wider">
                Tags (optional)
              </label>
              <div className="flex gap-2">
                <input
                  type="text"
                  value={tagInput}
                  onChange={(e) => setTagInput(e.target.value)}
                  onKeyDown={handleTagInputKeyDown}
                  placeholder="Add a tag..."
                  className="input-theme flex-1"
                />
                <button
                  type="button"
                  onClick={handleAddTag}
                  className="btn-theme-secondary px-4"
                >
                  Add
                </button>
              </div>
              {tags.length > 0 && (
                <div className="flex flex-wrap gap-2 mt-3">
                  {tags.map((tag) => (
                    <span
                      key={tag}
                      className="inline-flex items-center px-3 py-1.5 bg-theme-accent-light text-theme-accent rounded-full text-sm font-medium"
                    >
                      {tag}
                      <button
                        type="button"
                        onClick={() => handleRemoveTag(tag)}
                        className="ml-2 hover:opacity-70 transition-opacity"
                      >
                        ×
                      </button>
                    </span>
                  ))}
                </div>
              )}
            </div>
          )}
        </div>

        {/* Footer */}
        <div className="flex justify-end gap-3 p-6 border-t border-theme-primary bg-theme-tertiary">
          <button
            onClick={handleClose}
            className="btn-theme-secondary"
          >
            Cancel
          </button>
          <button
            onClick={() => {
              console.log('🔵 Add Feed clicked - feedType:', feedType)
              console.log('🔵 manualNpub:', manualNpub)
              console.log('🔵 manualNpub.trim():', manualNpub.trim())
              console.log('🔵 Starts with npub1?', manualNpub.startsWith('npub1'))
              console.log('🔵 isLoading:', isLoading)
              handleAddFeed()
            }}
            disabled={
              isLoading || 
              (feedType === 'RSS' && !rssUrl.trim()) ||
              (feedType === 'NOSTR' && (!manualNpub.trim() || !manualNpub.startsWith('npub1')))
            }
            className="btn-theme-primary"
            title={
              feedType === 'NOSTR' && !manualNpub.trim() ? 'Please enter an npub' :
              feedType === 'NOSTR' && !manualNpub.startsWith('npub1') ? 'npub must start with npub1' :
              feedType === 'RSS' && !rssUrl.trim() ? 'Please enter an RSS feed URL' :
              undefined
            }
          >
            {isLoading ? 'Adding...' : 'Add Feed'}
          </button>
        </div>
      </div>
    </div>
  )
}