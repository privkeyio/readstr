'use client'

import { useNostrAuth } from '@/contexts/NostrAuthContext'
import { useEffect } from 'react'
import { notFound } from 'next/navigation'
import { BrandHeader } from '@/components/brand-header'

export default function TestAuthPage() {
  if (process.env.NODE_ENV !== 'development') {
    notFound()
  }

  const { user, isConnected, connect, disconnect } = useNostrAuth()

  useEffect(() => {
    console.log('TestAuthPage: Current auth state:', {
      isConnected,
      hasUser: !!user,
      pubkey: user?.pubkey,
      npub: user?.npub,
    })
  }, [isConnected, user])

  return (
    <main className="font-brand relative flex min-h-screen flex-col bg-gradient-to-br from-[#1a1a1a] via-[#0d1117] to-[#161b22] text-white">
      <BrandHeader cta={{ href: '/reader', label: 'Go to Reader' }} />

      <div className="mx-auto w-full max-w-2xl px-4 py-12">
        <div className="rounded-2xl border border-[#27ae60]/20 bg-white/[0.06] p-6 backdrop-blur-xl md:p-8">
          <h1 className="mb-4 text-2xl font-extrabold tracking-tight">Authentication Test Page</h1>

          <div className="space-y-4">
            <div className="border-b border-[#27ae60]/15 pb-4">
              <h2 className="mb-2 font-semibold text-white">Current State:</h2>
              <ul className="list-inside list-disc space-y-1 text-[#B3B3B3]">
                <li>
                  Connected:{' '}
                  <span className={isConnected ? 'text-[#58d68d]' : 'text-red-400'}>
                    {isConnected ? 'Yes' : 'No'}
                  </span>
                </li>
                <li>
                  Has User:{' '}
                  <span className={user ? 'text-[#58d68d]' : 'text-red-400'}>
                    {user ? 'Yes' : 'No'}
                  </span>
                </li>
                {user && (
                  <>
                    <li>Pubkey: <code className="rounded bg-white/10 px-1 text-xs text-white">{user.pubkey}</code></li>
                    <li>Npub: <code className="rounded bg-white/10 px-1 text-xs text-white">{user.npub}</code></li>
                  </>
                )}
              </ul>
            </div>

            <div className="border-b border-[#27ae60]/15 pb-4">
              <h2 className="mb-2 font-semibold text-white">LocalStorage:</h2>
              <pre className="overflow-x-auto rounded-xl border border-[#27ae60]/15 bg-white/[0.05] p-2 text-xs text-[#B3B3B3]">
                {typeof window !== 'undefined' ? localStorage.getItem('nostr_session') || 'No session found' : 'Loading...'}
              </pre>
            </div>

            <div className="space-y-2">
              <h2 className="font-semibold text-white">Actions:</h2>
              {!isConnected ? (
                <>
                  <button
                    onClick={() => connect('nip07')}
                    className="block w-full rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-4 py-2 font-semibold text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)] transition-all duration-300 hover:-translate-y-0.5 hover:shadow-[0_8px_25px_rgba(39,174,96,0.35)]"
                  >
                    Connect with Browser Extension (NIP-07)
                  </button>
                  <button
                    onClick={() => {
                      const npub = prompt('Enter an npub to view (read-only):')
                      if (npub) {
                        connect('npub_readonly', { npub })
                      }
                    }}
                    className="block w-full rounded-xl border border-[#27ae60]/25 bg-white/10 px-4 py-2 font-semibold text-white transition-all duration-300 hover:-translate-y-0.5 hover:border-[#27ae60]/50 hover:bg-white/20"
                  >
                    View a public npub (read-only)
                  </button>
                </>
              ) : (
                <button
                  onClick={disconnect}
                  className="block w-full rounded-xl border border-red-500/40 bg-red-500/10 px-4 py-2 font-semibold text-red-300 transition-all duration-300 hover:bg-red-500/20"
                >
                  Disconnect
                </button>
              )}
            </div>

            <div className="border-t border-[#27ae60]/15 pt-4">
              <a href="/reader" className="text-sm font-medium text-[#27ae60] transition-colors hover:text-[#2ecc71]">
                Go to Feed Reader &rarr;
              </a>
            </div>
          </div>
        </div>
      </div>
    </main>
  )
}
