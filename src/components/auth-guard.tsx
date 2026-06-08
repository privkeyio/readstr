'use client'

import { useNostrAuth } from '@/contexts/NostrAuthContext'
import { useEffect, useState } from 'react'

function BunkerConnect({
  variant,
  bunkerUri,
  setBunkerUri,
  isLoading,
  onConnect,
}: {
  variant: 'primary' | 'compact'
  bunkerUri: string
  setBunkerUri: (value: string) => void
  isLoading: boolean
  onConnect: () => void
}) {
  return (
    <div
      className={
        variant === 'compact'
          ? 'space-y-2 rounded-xl border border-[#27ae60]/25 bg-white/[0.04] p-3'
          : 'space-y-2'
      }
    >
      {variant === 'compact' && (
        <p className="text-sm font-semibold text-white">Connect to Signer (remote)</p>
      )}
      <p className="text-xs text-[#B3B3B3]">
        In your remote signer app, choose &quot;Connect app&quot; and paste the <code>bunker://</code> string here.
      </p>
      <input
        type="text"
        inputMode="url"
        autoCapitalize="off"
        autoCorrect="off"
        spellCheck={false}
        value={bunkerUri}
        onChange={(e) => setBunkerUri(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === 'Enter' && !e.nativeEvent.isComposing) onConnect()
        }}
        placeholder="bunker://..."
        aria-label="Bunker connection string"
        disabled={isLoading}
        className="w-full rounded-lg border border-[#27ae60]/25 bg-black/30 px-3 py-2 text-sm text-white placeholder:text-[#6b7280] focus:border-[#27ae60]/60 focus:outline-none disabled:opacity-50"
      />
      <button
        onClick={onConnect}
        disabled={isLoading}
        className={
          variant === 'compact'
            ? 'flex w-full items-center justify-center gap-2 rounded-lg bg-gradient-to-br from-[#27ae60] to-[#229954] px-4 py-2.5 text-sm font-semibold text-white transition-all duration-300 hover:-translate-y-0.5 disabled:cursor-not-allowed disabled:opacity-50'
            : 'flex w-full items-center justify-center gap-2 rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-6 py-3 font-semibold text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)] transition-all duration-300 hover:-translate-y-0.5 hover:shadow-[0_8px_25px_rgba(39,174,96,0.35)] disabled:cursor-not-allowed disabled:opacity-50'
        }
      >
        {isLoading ? (
          <span className="animate-pulse">Connecting...</span>
        ) : (
          <span>{variant === 'compact' ? 'Connect to Signer (NIP-46)' : 'Connect to Signer'}</span>
        )}
      </button>
    </div>
  )
}

export function AuthGuard({ children }: { children: React.ReactNode }) {
  const { user, isConnected, connect } = useNostrAuth()
  const [error, setError] = useState<string | null>(null)
  const [isLoading, setIsLoading] = useState(false)
  const [isPWA, setIsPWA] = useState(false)
  const [isAndroid, setIsAndroid] = useState(false)
  const [bunkerUri, setBunkerUri] = useState('')

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

  const handleConnect = async (method: 'nip07' | 'npub_readonly' | 'nip46') => {
    setError(null)
    setIsLoading(true)
    try {
      if (method === 'npub_readonly') {
        const npub = window.prompt('Enter an npub to view (read-only):')?.trim()
        if (!npub) {
          setIsLoading(false)
          return
        }
        await connect('npub_readonly', { npub })
      } else if (method === 'nip46') {
        const uri = bunkerUri.trim()
        if (!uri) {
          setError('Paste a bunker:// connection string from Amber first.')
          setIsLoading(false)
          return
        }
        await connect('nip46', { bunkerUri: uri })
      } else {
        await connect(method)
      }
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
            {/* Primary path for PWA on Android - Amber via NIP-46 bunker */}
            {isPWA && isAndroid && (
              <BunkerConnect
                variant="primary"
                bunkerUri={bunkerUri}
                setBunkerUri={setBunkerUri}
                isLoading={isLoading}
                onConnect={() => handleConnect('nip46')}
              />
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
                {!(isPWA && isAndroid) && (
                  <BunkerConnect
                    variant="compact"
                    bunkerUri={bunkerUri}
                    setBunkerUri={setBunkerUri}
                    isLoading={isLoading}
                    onConnect={() => handleConnect('nip46')}
                  />
                )}
                <button
                  onClick={() => handleConnect('npub_readonly')}
                  disabled={isLoading}
                  className="flex w-full items-center justify-center gap-2 rounded-xl border border-[#27ae60]/25 bg-white/10 px-4 py-2.5 text-white transition-all duration-300 hover:-translate-y-0.5 hover:border-[#27ae60]/50 hover:bg-white/20 disabled:cursor-not-allowed disabled:opacity-50"
                >
                  <span className="text-sm">View a public npub (read-only)</span>
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