'use client'

import { useNostrAuth } from '@/contexts/NostrAuthContext'
import { useState } from 'react'

export function AuthShowcase() {
  const { isConnected, user, authMethod, connect, disconnect } = useNostrAuth()
  const [showLoginForm, setShowLoginForm] = useState(false)
  const [loginMethod, setLoginMethod] = useState<'nip07' | 'npub_readonly' | 'nip46' | null>(null)
  const [npub, setNpub] = useState('')
  const [bunkerUri, setBunkerUri] = useState('')
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState('')

  const handleConnect = async () => {
    if (!loginMethod) return
    
    setLoading(true)
    setError('')
    
    try {
      if (loginMethod === 'nip07') {
        await connect('nip07')
      } else if (loginMethod === 'npub_readonly') {
        await connect('npub_readonly', { npub })
      } else if (loginMethod === 'nip46') {
        const uri = bunkerUri.trim()
        if (!uri) {
          setError('Paste a bunker:// connection string from Amber first.')
          return
        }
        await connect('nip46', { bunkerUri: uri })
      }
      setShowLoginForm(false)
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Connection failed')
    } finally {
      setLoading(false)
    }
  }

  if (isConnected && user) {
    return (
      <div className="flex flex-col items-center justify-center gap-4">
        <div className="text-center">
          <p className="text-2xl text-white mb-2">
            ⚡ Connected to Nostr
          </p>
          <p className="text-sm text-gray-300">
            Method: {authMethod === 'nip07' ? 'Browser Extension' : 'Read-only npub view'}
          </p>
          <div className="mt-4 p-3 bg-gray-800/50 rounded-lg">
            <p className="text-xs text-gray-400 mb-1">Your npub:</p>
            <p className="text-sm text-green-400 font-mono break-all">
              {user.npub}
            </p>
          </div>
        </div>
        <button
          onClick={disconnect}
          className="rounded-full bg-red-600/20 px-6 py-2 text-sm font-semibold text-red-400 hover:bg-red-600/30 transition"
        >
          Disconnect
        </button>
      </div>
    )
  }

  if (!showLoginForm) {
    return (
      <div className="flex flex-col items-center justify-center gap-4">
        <p className="text-center text-2xl text-white">
          🔐 Connect to Nostr
        </p>
        <p className="text-center text-sm text-gray-300 max-w-md">
          Choose your preferred way to connect to the Nostr network
        </p>
        
        <div className="flex flex-col gap-3 w-full max-w-sm">
          <button
            onClick={() => {
              setLoginMethod('nip07')
              setShowLoginForm(true)
            }}
            className="rounded-lg bg-[#27ae60]/20 px-6 py-3 font-semibold text-[#2ecc71] hover:bg-[#27ae60]/30 transition"
          >
            🔌 Browser Extension (NIP-07)
          </button>
          
          <button
            onClick={() => {
              setLoginMethod('nip46')
              setShowLoginForm(true)
            }}
            className="rounded-lg border border-white/20 bg-white/5 px-6 py-3 font-semibold text-gray-200 hover:bg-white/10 transition"
          >
            📱 Connect to Signer (remote)
          </button>

          <button
            onClick={() => {
              setLoginMethod('npub_readonly')
              setShowLoginForm(true)
            }}
            className="rounded-lg border border-white/20 bg-white/5 px-6 py-3 font-semibold text-gray-200 hover:bg-white/10 transition"
          >
            👤 View a public npub (read-only)
          </button>
        </div>
      </div>
    )
  }

  return (
    <div className="flex flex-col items-center justify-center gap-4 w-full max-w-md">
      <p className="text-center text-2xl text-white mb-4">
        {loginMethod === 'nip07'
          ? '🔌 Browser Extension'
          : loginMethod === 'nip46'
          ? '📱 Connect to Signer'
          : '👤 Read-only npub view'}
      </p>

      {error && (
        <div className="w-full p-3 bg-red-600/20 border border-red-600/30 rounded-lg">
          <p className="text-red-400 text-sm">{error}</p>
        </div>
      )}

      {loginMethod === 'nip07' && (
        <div className="w-full p-4 bg-gray-800/50 rounded-lg">
          <p className="text-gray-300 text-sm mb-4">
            This will connect using your Nostr browser extension (like Alby, nos2x, or Amber).
          </p>
          <button
            onClick={handleConnect}
            disabled={loading}
            className="w-full rounded-lg bg-[#27ae60] px-4 py-2 font-semibold text-white hover:bg-[#229954] disabled:opacity-50 transition"
          >
            {loading ? 'Connecting...' : 'Connect Extension'}
          </button>
        </div>
      )}

      {loginMethod === 'nip46' && (
        <div className="w-full p-4 bg-gray-800/50 rounded-lg">
          <p className="text-gray-300 text-sm mb-4">
            In your remote signer app, choose &quot;Connect app&quot; and paste the <code>bunker://</code> string here.
          </p>
          <div className="space-y-3">
            <div>
              <label className="block text-sm font-medium text-gray-300 mb-1">
                bunker:// connection string
              </label>
              <input
                type="text"
                inputMode="url"
                autoCapitalize="off"
                autoCorrect="off"
                spellCheck={false}
                value={bunkerUri}
                onChange={(e) => setBunkerUri(e.target.value)}
                placeholder="bunker://..."
                className="w-full rounded-md bg-gray-700 px-3 py-2 text-white placeholder-gray-400 focus:outline-none focus:ring-2 focus:ring-[#27ae60]"
              />
            </div>
            <button
              onClick={handleConnect}
              disabled={loading || !bunkerUri.trim()}
              className="w-full rounded-lg bg-[#27ae60] px-4 py-2 font-semibold text-white hover:bg-[#229954] disabled:opacity-50 transition"
            >
              {loading ? 'Connecting...' : 'Connect to Signer (NIP-46)'}
            </button>
          </div>
        </div>
      )}

      {loginMethod === 'npub_readonly' && (
        <div className="w-full p-4 bg-gray-800/50 rounded-lg">
          <p className="text-gray-300 text-sm mb-4">
            Enter an npub to view a public profile. This is a read-only view with no authentication (you can&apos;t post or sign events).
          </p>
          <div className="space-y-3">
            <div>
              <label className="block text-sm font-medium text-gray-300 mb-1">
                npub
              </label>
              <input
                type="text"
                value={npub}
                onChange={(e) => setNpub(e.target.value)}
                placeholder="npub1..."
                className="w-full rounded-md bg-gray-700 px-3 py-2 text-white placeholder-gray-400 focus:outline-none focus:ring-2 focus:ring-[#27ae60]"
              />
            </div>
            <button
              onClick={handleConnect}
              disabled={loading || !npub}
              className="w-full rounded-lg bg-[#27ae60] px-4 py-2 font-semibold text-white hover:bg-[#229954] disabled:opacity-50 transition"
            >
              {loading ? 'Connecting...' : 'View profile'}
            </button>
          </div>
        </div>
      )}

      <button
        onClick={() => {
          setShowLoginForm(false)
          setError('')
        }}
        className="text-gray-400 hover:text-gray-300 transition"
      >
        ← Back to options
      </button>
    </div>
  )
}