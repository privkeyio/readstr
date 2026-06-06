'use client'

import { useEffect, useState, Suspense } from 'react'
import { useSearchParams } from 'next/navigation'
import { useNostrAuth } from '@/contexts/NostrAuthContext'
import { api } from '@/trpc/react'
import Link from 'next/link'
import { BrandHeader } from '@/components/brand-header'

function safeReturnPath(value: string | null): string {
  if (!value || value[0] !== '/' || value[1] === '/' || value[1] === '\\') {
    return '/reader'
  }
  try {
    const url = new URL(value, window.location.origin)
    if (url.origin !== window.location.origin) {
      return '/reader'
    }
    const path = url.pathname + url.search + url.hash
    if (path[0] !== '/' || path[1] === '/' || path[1] === '\\') {
      return '/reader'
    }
    return path
  } catch {
    return '/reader'
  }
}

function SubscribeContent() {
  const searchParams = useSearchParams()
  const { user, isConnected, connect } = useNostrAuth()

  const npub = searchParams.get('npub')
  const tags = searchParams.get('tags')?.split(',').filter(Boolean) || []
  const returnUrl = searchParams.get('return')

  const [status, setStatus] = useState<'loading' | 'ready' | 'subscribing' | 'success' | 'error'>('loading')
  const [errorMessage, setErrorMessage] = useState('')

  // Fetch feed info from guide
  const { data: guideFeed, isLoading: feedLoading } = api.guide.getGuideFeed.useQuery(
    { npub: npub || '' },
    { enabled: !!npub }
  )

  const subscribeMutation = api.feed.subscribeFeed.useMutation()
  const incrementSubscriberMutation = api.guide.incrementSubscriberCount.useMutation()

  useEffect(() => {
    if (!npub) {
      setStatus('error')
      setErrorMessage('Missing npub parameter')
      return
    }

    if (!feedLoading) {
      setStatus('ready')
    }
  }, [npub, feedLoading])

  const handleSubscribe = async () => {
    if (!npub) return

    // If not connected, prompt to connect via NIP-07
    if (!isConnected) {
      try {
        await connect('nip07')
      } catch (error) {
        setStatus('error')
        setErrorMessage('Please install a Nostr browser extension (like Alby) to subscribe')
        return
      }
    }

    setStatus('subscribing')

    try {
      await subscribeMutation.mutateAsync({
        type: 'NOSTR',
        npub,
        title: guideFeed?.displayName,
        tags: tags.length > 0 ? tags : (guideFeed?.tags || []),
      })

      // Increment subscriber count
      if (guideFeed) {
        await incrementSubscriberMutation.mutateAsync({ npub })
      }

      setStatus('success')

      // Redirect after success
      setTimeout(() => {
        window.location.href = safeReturnPath(returnUrl)
      }, 2000)

    } catch (error: any) {
      setStatus('error')
      setErrorMessage(error.message || 'Failed to subscribe')
    }
  }

  // Auto-subscribe if user is already connected
  useEffect(() => {
    if (isConnected && status === 'ready' && npub) {
      // Small delay to let UI render
      const timer = setTimeout(() => {
        handleSubscribe()
      }, 500)
      return () => clearTimeout(timer)
    }
  }, [isConnected, status, npub])

  if (!npub) {
    return (
      <main className="font-brand relative flex min-h-screen flex-col bg-gradient-to-br from-[#1a1a1a] via-[#0d1117] to-[#161b22] text-white">
        <BrandHeader cta={{ href: '/guide', label: 'Browse the Guide' }} />
        <div className="flex flex-1 items-center justify-center p-4">
          <div className="w-full max-w-md rounded-2xl border border-[#27ae60]/20 bg-white/[0.06] p-8 text-center backdrop-blur-xl">
            <h1 className="mb-2 text-xl font-bold text-white">Invalid Link</h1>
            <p className="mb-6 text-[#B3B3B3]">
              This subscription link is missing the npub parameter.
            </p>
            <Link
              href="/guide"
              className="inline-block rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-6 py-3 font-semibold text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)] transition-all duration-300 hover:-translate-y-0.5 hover:shadow-[0_8px_25px_rgba(39,174,96,0.35)]"
            >
              Browse the Guide
            </Link>
          </div>
        </div>
      </main>
    )
  }

  return (
    <main className="font-brand relative flex min-h-screen flex-col bg-gradient-to-br from-[#1a1a1a] via-[#0d1117] to-[#161b22] text-white">
      <BrandHeader cta={{ href: '/guide', label: 'Browse the Guide' }} />
      <div className="flex flex-1 items-center justify-center p-4">
        <div className="w-full max-w-md rounded-2xl border border-[#27ae60]/20 bg-white/[0.06] p-8 backdrop-blur-xl">
          {/* Header */}
          <div className="mb-6 text-center">
            <h1 className="text-2xl font-extrabold tracking-tight text-white">
              Subscribe to Feed
            </h1>
          </div>

          {/* Feed Info */}
          {feedLoading ? (
            <div className="flex justify-center py-8">
              <div className="h-8 w-8 animate-spin rounded-full border-b-2 border-[#27ae60]"></div>
            </div>
          ) : guideFeed ? (
            <div className="mb-6 rounded-xl border border-[#27ae60]/15 bg-white/[0.05] p-4">
              <div className="flex items-center gap-4">
                {guideFeed.picture ? (
                  <img
                    src={guideFeed.picture}
                    alt={guideFeed.displayName}
                    className="h-16 w-16 rounded-full border-2 border-[#27ae60]/40 object-cover"
                  />
                ) : (
                  <div className="flex h-16 w-16 items-center justify-center rounded-full border border-[#27ae60]/30 bg-[#27ae60]/10 text-lg font-bold text-[#27ae60]">
                    {guideFeed.displayName?.charAt(0)?.toUpperCase() || '?'}
                  </div>
                )}
                <div className="min-w-0 flex-1">
                  <h2 className="truncate font-semibold text-white">
                    {guideFeed.displayName}
                  </h2>
                  {guideFeed.about && (
                    <p className="line-clamp-2 text-sm text-[#B3B3B3]">
                      {guideFeed.about}
                    </p>
                  )}
                  <div className="mt-2 flex flex-wrap gap-2">
                    {guideFeed.tags.slice(0, 4).map((tag: string) => (
                      <span
                        key={tag}
                        className="rounded border border-[#27ae60]/30 bg-[#27ae60]/10 px-2 py-0.5 text-xs text-[#B3B3B3]"
                      >
                        {tag}
                      </span>
                    ))}
                  </div>
                </div>
              </div>
              <div className="mt-3 flex justify-between border-t border-[#27ae60]/15 pt-3 text-sm text-[#B3B3B3]">
                <span>{guideFeed.postCount} posts</span>
                <span>{guideFeed.subscriberCount} subscribers</span>
              </div>
            </div>
          ) : (
            <div className="mb-6 rounded-xl border border-[#27ae60]/15 bg-white/[0.05] p-4">
              <p className="text-center text-[#B3B3B3]">
                <span className="break-all font-mono text-sm">{npub}</span>
              </p>
            </div>
          )}

          {/* Status Messages */}
          {status === 'subscribing' && (
            <div className="flex items-center justify-center gap-3 py-4 text-[#27ae60]">
              <div className="h-5 w-5 animate-spin rounded-full border-b-2 border-current"></div>
              <span>Subscribing...</span>
            </div>
          )}

          {status === 'success' && (
            <div className="py-4 text-center">
              <p className="font-medium text-[#58d68d]">
                Successfully subscribed!
              </p>
              <p className="mt-1 text-sm text-[#B3B3B3]">
                Redirecting...
              </p>
            </div>
          )}

          {status === 'error' && (
            <div className="py-4 text-center">
              <p className="font-medium text-red-400">
                {errorMessage}
              </p>
            </div>
          )}

          {/* Action Buttons */}
          {(status === 'ready' || status === 'loading') && !isConnected && (
            <div className="space-y-3">
              <button
                onClick={handleSubscribe}
                disabled={status === 'loading'}
                className="w-full rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-6 py-3 font-semibold text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)] transition-all duration-300 hover:-translate-y-0.5 hover:shadow-[0_8px_25px_rgba(39,174,96,0.35)] disabled:opacity-50"
              >
                {status === 'loading' ? 'Loading...' : 'Connect & Subscribe'}
              </button>
              <p className="text-center text-xs text-[#B3B3B3]">
                Requires a Nostr browser extension (Alby, nos2x, etc.)
              </p>
            </div>
          )}

          {status === 'ready' && isConnected && (
            <div className="py-2 text-center text-[#B3B3B3]">
              <div className="animate-pulse">Subscribing automatically...</div>
            </div>
          )}

          {/* Footer Links */}
          <div className="mt-6 flex justify-center gap-4 border-t border-[#27ae60]/15 pt-4 text-sm">
            <Link
              href="/guide"
              className="font-medium text-[#27ae60] transition-colors hover:text-[#2ecc71]"
            >
              Browse Guide
            </Link>
            <Link
              href="/reader"
              className="font-medium text-[#27ae60] transition-colors hover:text-[#2ecc71]"
            >
              Go to Reader
            </Link>
          </div>
        </div>
      </div>
    </main>
  )
}

export default function SubscribePage() {
  return (
    <Suspense fallback={
      <div className="flex min-h-screen items-center justify-center bg-gradient-to-br from-[#1a1a1a] via-[#0d1117] to-[#161b22]">
        <div className="h-8 w-8 animate-spin rounded-full border-b-2 border-[#27ae60]"></div>
      </div>
    }>
      <SubscribeContent />
    </Suspense>
  )
}
