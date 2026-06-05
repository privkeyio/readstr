'use client'

import { useNostrAuth } from '@/contexts/NostrAuthContext'
import { useEffect, useState } from 'react'

export function AuthGuard({ children }: { children: React.ReactNode }) {
  const { user, isConnected, connect } = useNostrAuth()
  const [error, setError] = useState<string | null>(null)
  const [isLoading, setIsLoading] = useState(false)
  const [isPWA, setIsPWA] = useState(false)
  const [isAndroid, setIsAndroid] = useState(false)

  useEffect(() => {
    // Detect if running as PWA
    const isPWAMode = window.matchMedia('(display-mode: standalone)').matches ||
                      (window.navigator as any).standalone === true ||
                      document.referrer.includes('android-app://')
    setIsPWA(isPWAMode)

    // Detect Android
    setIsAndroid(/Android/i.test(navigator.userAgent))

    console.log('AuthGuard: Auth state =', { isConnected, hasUser: !!user, pubkey: user?.pubkey?.slice(0, 8), isPWA: isPWAMode })
  }, [isConnected, user])

  const handleConnect = async (method: 'nip07' | 'npub_password') => {
    setError(null)
    setIsLoading(true)
    try {
      await connect(method)
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Connection failed')
    } finally {
      setIsLoading(false)
    }
  }

  if (!isConnected || !user) {
    console.log('AuthGuard: Showing login screen')
    return (
      <div className="font-brand flex min-h-screen items-center justify-center bg-gradient-to-br from-[#1a1a1a] via-[#0d1117] to-[#161b22] p-4 text-white">
        <div className="w-full max-w-md space-y-6 rounded-2xl border border-[#27ae60]/20 bg-white/[0.06] p-8 backdrop-blur-xl">
          <div className="text-center">
            <h2 className="text-3xl font-extrabold tracking-tight">
              <span className="bg-gradient-to-br from-[#27ae60] via-[#2ecc71] to-[#58d68d] bg-clip-text text-transparent">
                Readstr
              </span>
            </h2>
            <p className="mt-2 text-[#B3B3B3]">
              Connect with Nostr to access your feeds
            </p>
          </div>

          {error && (
            <div className="rounded-xl border border-red-500/40 bg-red-500/10 p-4">
              <p className="text-sm text-red-300">{error}</p>
            </div>
          )}

          <div className="space-y-3">
            {/* Primary button for PWA on Android - Amber with NIP-07 */}
            {isPWA && isAndroid && (
              <button
                onClick={() => handleConnect('nip07')}
                disabled={isLoading}
                className="flex w-full items-center justify-center gap-2 rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-6 py-3 font-semibold text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)] transition-all duration-300 hover:-translate-y-0.5 hover:shadow-[0_8px_25px_rgba(39,174,96,0.35)] disabled:cursor-not-allowed disabled:opacity-50"
              >
                {isLoading ? (
                  <span className="animate-pulse">Connecting...</span>
                ) : (
                  <span>Connect with Amber</span>
                )}
              </button>
            )}

            {/* Standard NIP-07 for browser extensions */}
            {(!isPWA || !isAndroid) && (
              <button
                onClick={() => handleConnect('nip07')}
                disabled={isLoading}
                className="flex w-full items-center justify-center gap-2 rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-6 py-3 font-semibold text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)] transition-all duration-300 hover:-translate-y-0.5 hover:shadow-[0_8px_25px_rgba(39,174,96,0.35)] disabled:cursor-not-allowed disabled:opacity-50"
              >
                {isLoading ? (
                  <span className="animate-pulse">Connecting...</span>
                ) : (
                  <span>Connect with Browser Extension</span>
                )}
              </button>
            )}

            {/* Alternative methods */}
            <details className="group">
              <summary className="cursor-pointer py-2 text-center text-sm text-[#B3B3B3] transition-colors hover:text-white">
                Other sign-in options
              </summary>
              <div className="mt-3 space-y-3">
                <button
                  onClick={() => handleConnect('npub_password')}
                  disabled={isLoading}
                  className="flex w-full items-center justify-center gap-2 rounded-xl border border-[#27ae60]/25 bg-white/10 px-4 py-2.5 text-white transition-all duration-300 hover:-translate-y-0.5 hover:border-[#27ae60]/50 hover:bg-white/20 disabled:cursor-not-allowed disabled:opacity-50"
                >
                  <span className="text-sm">Connect with Npub (Read-only)</span>
                </button>
              </div>
            </details>
          </div>

          {isPWA && isAndroid && (
            <div className="mt-6 rounded-xl border border-[#27ae60]/20 bg-[#27ae60]/10 p-4">
              <p className="text-center text-xs text-[#B3B3B3]">
                <strong className="text-white">Don&apos;t have Amber?</strong><br />
                Download it from{' '}
                <a
                  href="https://github.com/greenart7c3/Amber/releases"
                  target="_blank"
                  rel="noopener noreferrer"
                  className="text-[#27ae60] underline hover:text-[#2ecc71]"
                >
                  GitHub
                </a>
                {' '}or{' '}
                <a
                  href="https://play.google.com/store/apps/details?id=com.greenart7c3.amber"
                  target="_blank"
                  rel="noopener noreferrer"
                  className="text-[#27ae60] underline hover:text-[#2ecc71]"
                >
                  Google Play
                </a>
              </p>
            </div>
          )}

          <p className="mt-4 text-center text-xs text-[#B3B3B3]">
            Your keys are stored locally and never sent to our servers
          </p>
        </div>
      </div>
    )
  }

  console.log('AuthGuard: User authenticated, showing protected content')
  return <>{children}</>
}