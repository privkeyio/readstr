'use client'

import React, { createContext, useContext, useState, useEffect, useRef, useCallback, ReactNode } from 'react'
import { Event, nip19, UnsignedEvent } from 'nostr-tools'
import { BunkerSigner, parseBunkerInput, type BunkerPointer } from 'nostr-tools/nip46'
import { SimplePool } from 'nostr-tools/pool'
import { generateSecretKey } from 'nostr-tools/pure'
import { bytesToHex, hexToBytes } from '@noble/hashes/utils'

const debugLog = (...args: unknown[]) => {
  if (process.env.NODE_ENV === 'development') console.log(...args)
}

// Remote signer round-trips through Amber, which may wait on manual user
// approval, so the timeout is generous enough to cover a human tapping approve.
const NIP46_TIMEOUT_MS = 60_000

const withTimeout = <T,>(promise: Promise<T>, ms: number, label: string): Promise<T> => {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${label} timed out`)), ms)
    promise.then(
      (value) => {
        clearTimeout(timer)
        resolve(value)
      },
      (error) => {
        clearTimeout(timer)
        reject(error)
      },
    )
  })
}

export type NostrAuthMethod = 'nip07' | 'npub_readonly' | 'nip46' | null

interface StoredNip46Session {
  method: 'nip46'
  pubkey: string
  npub: string
  clientSecretKey: string
  bunker: BunkerPointer
  timestamp: number
}

// The live BunkerSigner is not serializable, so it lives in module scope
// alongside its pool. signEvent/disconnect/restore reach it from here.
let activeBunkerSigner: BunkerSigner | null = null
let activeBunkerPool: SimplePool | null = null
let activeBunkerRelays: string[] = []
// All signer construction is serialized behind this single in-flight promise so
// concurrent callers (lazy-init, restore, connect) never build competing pools.
let bunkerInitPromise: Promise<BunkerSigner> | null = null
// Bumped by disconnect so an in-flight init can detect it was torn down and
// dispose the signer it built instead of installing a dead session.
let bunkerGeneration = 0

// Closes a specific signer/pool, clearing the module refs only if they still
// point at it, so cleaning up a superseded build never tears down a newer one.
const disposeSigner = async (
  signer: BunkerSigner | null,
  pool: SimplePool | null,
  relays: string[],
) => {
  if (activeBunkerSigner === signer) activeBunkerSigner = null
  if (activeBunkerPool === pool) {
    activeBunkerPool = null
    activeBunkerRelays = []
  }
  if (signer) {
    try {
      await signer.close()
    } catch (error) {
      debugLog('Error closing bunker signer:', error)
    }
  }
  if (pool) {
    try {
      pool.close(relays)
    } catch {
      // pool cleanup is best-effort
    }
  }
}

const closeActiveBunker = async () => {
  // Claim the refs synchronously before the first await so a signer built by a
  // concurrent reconnect can't be clobbered by this close finishing late.
  const signer = activeBunkerSigner
  const pool = activeBunkerPool
  const relays = activeBunkerRelays
  activeBunkerSigner = null
  activeBunkerPool = null
  activeBunkerRelays = []
  await disposeSigner(signer, pool, relays)
}

const buildBunkerSigner = (
  session: StoredNip46Session,
): { signer: BunkerSigner; pool: SimplePool; relays: string[] } => {
  const pool = new SimplePool()
  const csk = hexToBytes(session.clientSecretKey)
  const signer = BunkerSigner.fromBunker(csk, session.bunker, {
    pool,
    onauth: (url: string) => {
      try {
        const u = new URL(url)
        if (u.protocol === 'https:' || u.protocol === 'http:') {
          window.open(u.href, '_blank', 'noopener,noreferrer')
        }
      } catch {
        // ignore non-URL or unsafe (javascript:, data:, …) auth_url values
      }
    },
  })
  activeBunkerSigner = signer
  activeBunkerPool = pool
  activeBunkerRelays = session.bunker.relays
  return { signer, pool, relays: session.bunker.relays }
}

// The single owner of signer construction. Closes any existing signer before
// building a replacement, connects with a timeout, and dedupes concurrent calls.
// Disposes its own build on connect failure or if disconnect superseded it.
const ensureBunkerSigner = (session: StoredNip46Session): Promise<BunkerSigner> => {
  if (!bunkerInitPromise) {
    const generation = bunkerGeneration
    bunkerInitPromise = (async () => {
      await closeActiveBunker()
      const built = buildBunkerSigner(session)
      try {
        await withTimeout(built.signer.connect(), NIP46_TIMEOUT_MS, 'NIP-46 connect')
      } catch (error) {
        await disposeSigner(built.signer, built.pool, built.relays)
        throw error
      }
      if (bunkerGeneration !== generation) {
        await disposeSigner(built.signer, built.pool, built.relays)
        throw new Error('NIP-46 connection superseded')
      }
      return built.signer
    })()
  }
  const inFlight = bunkerInitPromise
  return inFlight.finally(() => {
    if (bunkerInitPromise === inFlight) bunkerInitPromise = null
  })
}

export interface NostrUser {
  pubkey: string
  npub: string
  profile?: {
    name?: string
    display_name?: string
    about?: string
    picture?: string
    nip05?: string
  }
}

export interface NostrAuthState {
  isConnected: boolean
  user: NostrUser | null
  authMethod: NostrAuthMethod
  canSign: boolean
  connect: (method: NostrAuthMethod, credentials?: { npub?: string; bunkerUri?: string }) => Promise<void>
  disconnect: () => void
  signEvent: (event: UnsignedEvent) => Promise<Event | null>
  signEventOrThrow: (event: UnsignedEvent) => Promise<Event>
  getPublicKey: () => string | null
}

const NostrAuthContext = createContext<NostrAuthState | null>(null)

interface NostrAuthProviderProps {
  children: ReactNode
}

export function NostrAuthProvider({ children }: NostrAuthProviderProps) {
  const [isConnected, setIsConnected] = useState(false)
  const [user, setUser] = useState<NostrUser | null>(null)
  const [authMethod, setAuthMethod] = useState<NostrAuthMethod>(null)

  const restoreStartedRef = useRef(false)
  const connectingNip46Ref = useRef<Promise<void> | null>(null)

  // Check for existing connection on mount. Guarded so a StrictMode double-mount
  // doesn't start a duplicate restore; the serialized init covers the rest.
  useEffect(() => {
    if (restoreStartedRef.current) return
    restoreStartedRef.current = true
    void checkExistingConnection()
  }, [])

  const checkExistingConnection = async () => {
    // Check for stored session first
    const storedSession = localStorage.getItem('nostr_session')
    if (storedSession) {
      try {
        const sessionData = JSON.parse(storedSession)
        
        if (sessionData.method === 'nip07') {
          // For NIP-07, verify the extension is still available
          if (typeof window !== 'undefined' && window.nostr) {
            try {
              const pubkey = await window.nostr.getPublicKey()
              const npub = nip19.npubEncode(pubkey)
              setUser({ pubkey, npub })
              setAuthMethod('nip07')
              setIsConnected(true)
              debugLog('Restored NIP-07 session')
              return
            } catch (error) {
              debugLog('NIP-07 extension not available, clearing session')
              localStorage.removeItem('nostr_session')
            }
          } else {
            debugLog('NIP-07 extension not found, clearing session')
            localStorage.removeItem('nostr_session')
          }
        } else if (sessionData.method === 'nip46') {
          const stored = sessionData as StoredNip46Session
          try {
            const signer = await ensureBunkerSigner(stored)
            const pubkey = await withTimeout(signer.getPublicKey(), NIP46_TIMEOUT_MS, 'NIP-46 getPublicKey')
            if (pubkey !== stored.pubkey) {
              throw new Error('Remote signer pubkey mismatch')
            }
            setUser({ pubkey, npub: nip19.npubEncode(pubkey) })
            setAuthMethod('nip46')
            setIsConnected(true)
            debugLog('Restored NIP-46 session')
            return
          } catch (error) {
            debugLog('NIP-46 session restore failed, clearing session:', error)
            // ensureBunkerSigner already disposed its own build. Only clear the
            // stored session if it's still the one we tried to restore — a fresh
            // connect/disconnect may have replaced it while we were connecting.
            const current = localStorage.getItem('nostr_session')
            if (current) {
              try {
                const parsed = JSON.parse(current) as StoredNip46Session
                if (parsed.method === 'nip46' && parsed.clientSecretKey === stored.clientSecretKey) {
                  localStorage.removeItem('nostr_session')
                }
              } catch {
                localStorage.removeItem('nostr_session')
              }
            }
          }
        } else if (sessionData.method === 'npub_readonly' && sessionData.pubkey) {
          // Restore read-only npub session
          setUser({
            pubkey: sessionData.pubkey,
            npub: sessionData.npub
          })
          setAuthMethod('npub_readonly')
          setIsConnected(true)
          debugLog('Restored read-only npub session')
          return
        } else {
          // Unknown or legacy session method, clear it
          localStorage.removeItem('nostr_session')
        }
      } catch (error) {
        console.error('Invalid stored session:', error)
        localStorage.removeItem('nostr_session')
      }
    }
    
    // If no stored session, try to detect NIP-07 extension
    if (typeof window !== 'undefined' && window.nostr) {
      debugLog('NIP-07 extension detected but no stored session')
    }
  }

  const connect = async (method: NostrAuthMethod, credentials?: { npub?: string; bunkerUri?: string }) => {
    try {
      switch (method) {
        case 'nip07':
          await connectNIP07()
          break
        case 'nip46':
          if (!credentials?.bunkerUri) {
            throw new Error('bunker URL required for remote signer')
          }
          await connectNIP46(credentials.bunkerUri)
          break
        case 'npub_readonly':
          if (!credentials?.npub) {
            throw new Error('npub required for read-only view')
          }
          await connectNpubReadonly(credentials.npub)
          break
        default:
          throw new Error('Invalid auth method')
      }
    } catch (error) {
      console.error('Connection failed:', error)
      throw error
    }
  }

  const connectNIP07 = async () => {
    if (typeof window === 'undefined' || !window.nostr) {
      // Check if we're on Android to suggest Amber
      const isAndroid = /Android/i.test(navigator.userAgent)
      const errorMessage = isAndroid
        ? "No browser extension found. On Android, use 'Connect with Amber' (remote signer) below."
        : 'NIP-07 extension not found. Please install a Nostr browser extension like Alby or nos2x, or use the remote signer (NIP-46) option.'
      throw new Error(errorMessage)
    }

    try {
      const pubkey = await window.nostr.getPublicKey()
      const npub = nip19.npubEncode(pubkey)
      
      // Store the session
      const sessionData = {
        pubkey,
        npub,
        method: 'nip07',
        timestamp: Date.now()
      }
      localStorage.setItem('nostr_session', JSON.stringify(sessionData))
      
      setUser({ pubkey, npub })
      setAuthMethod('nip07')
      setIsConnected(true)
    } catch (error) {
      console.error('NIP-07 connection error:', error)
      throw new Error('Failed to connect to Nostr signer. Please approve the connection request.')
    }
  }

  const connectNIP46 = async (bunkerUri: string) => {
    if (connectingNip46Ref.current) {
      return connectingNip46Ref.current
    }

    const run = async () => {
      const generation = bunkerGeneration
      const bp = await parseBunkerInput(bunkerUri)
      if (!bp) {
        throw new Error('Invalid bunker URL')
      }

      const clientSecretKey = generateSecretKey()
      const session: StoredNip46Session = {
        method: 'nip46',
        pubkey: '',
        npub: '',
        clientSecretKey: bytesToHex(clientSecretKey),
        bunker: bp,
        timestamp: Date.now(),
      }

      let builtSigner: BunkerSigner | null = null
      try {
        builtSigner = await ensureBunkerSigner(session)
        const pubkey = await withTimeout(builtSigner.getPublicKey(), NIP46_TIMEOUT_MS, 'NIP-46 getPublicKey')
        if (bunkerGeneration !== generation) {
          throw new Error('NIP-46 connection superseded')
        }
        const npub = nip19.npubEncode(pubkey)

        session.pubkey = pubkey
        session.npub = npub
        localStorage.setItem('nostr_session', JSON.stringify(session))

        setUser({ pubkey, npub })
        setAuthMethod('nip46')
        setIsConnected(true)
        debugLog('Connected with NIP-46 remote signer')
      } catch (error) {
        // Only tear down the signer this call built; a newer connection stays up.
        if (builtSigner && activeBunkerSigner === builtSigner) {
          await closeActiveBunker()
          localStorage.removeItem('nostr_session')
        }
        console.error('NIP-46 connection error:', error)
        throw new Error('Failed to connect to remote signer. Approve the request in your signer app and try again.')
      }
    }

    const promise = run().finally(() => {
      connectingNip46Ref.current = null
    })
    connectingNip46Ref.current = promise
    return promise
  }

  const connectNpubReadonly = async (npub: string) => {
    try {
      // Validate npub format
      const decoded = nip19.decode(npub)
      if (decoded.type !== 'npub') {
        throw new Error('Invalid npub format')
      }

      const pubkey = decoded.data as string

      const sessionData = {
        pubkey,
        npub,
        method: 'npub_readonly',
        timestamp: Date.now()
      }
      localStorage.setItem('nostr_session', JSON.stringify(sessionData))

      setUser({ pubkey, npub })
      setAuthMethod('npub_readonly')
      setIsConnected(true)

      debugLog('Connected with read-only npub view')
    } catch (error) {
      throw new Error('Invalid npub provided or connection failed')
    }
  }

  const disconnect = () => {
    // Supersede any in-flight init (so it disposes its own build instead of
    // installing a dead session), abandon its promise, and close the active
    // signer so an immediate reconnect can't interleave with this teardown.
    bunkerGeneration++
    bunkerInitPromise = null
    void closeActiveBunker()
    setIsConnected(false)
    setUser(null)
    setAuthMethod(null)
    localStorage.removeItem('nostr_session')
  }

  const signEvent = useCallback(async (unsignedEvent: UnsignedEvent): Promise<Event | null> => {
    if (!isConnected || !user) return null

    try {
      switch (authMethod) {
        case 'nip07':
          if (window.nostr?.signEvent) {
            return await window.nostr.signEvent({ ...unsignedEvent, pubkey: user.pubkey })
          }
          throw new Error('NIP-07 signing not available')

        case 'nip46': {
          let signer = activeBunkerSigner
          if (!signer) {
            const storedSession = localStorage.getItem('nostr_session')
            if (!storedSession) throw new Error('NIP-46 session not available')
            const parsed = JSON.parse(storedSession) as StoredNip46Session
            if (parsed.method !== 'nip46') throw new Error('NIP-46 session not available')
            signer = await ensureBunkerSigner(parsed)
          }
          return await withTimeout(
            signer.signEvent({
              kind: unsignedEvent.kind,
              content: unsignedEvent.content,
              tags: unsignedEvent.tags,
              created_at: unsignedEvent.created_at,
            }),
            NIP46_TIMEOUT_MS,
            'NIP-46 sign',
          )
        }

        case 'npub_readonly':
          throw new Error('Cannot sign events in read-only npub view. Use a NIP-07 extension or remote signer for signing.')

        default:
          throw new Error('No signing method available')
      }
    } catch (error) {
      console.error('Signing failed:', error)
      return null
    }
  }, [isConnected, user, authMethod])

  const signEventOrThrow = useCallback(async (unsignedEvent: UnsignedEvent): Promise<Event> => {
    const signedEvent = await signEvent(unsignedEvent)
    if (!signedEvent) throw new Error('Failed to sign event')
    return signedEvent
  }, [signEvent])

  const getPublicKeyMethod = (): string | null => {
    return user?.pubkey || null
  }

  const canSign = authMethod === 'nip07' || authMethod === 'nip46'

  const value: NostrAuthState = {
    isConnected,
    user,
    authMethod,
    canSign,
    connect,
    disconnect,
    signEvent,
    signEventOrThrow,
    getPublicKey: getPublicKeyMethod,
  }

  return (
    <NostrAuthContext.Provider value={value}>
      {children}
    </NostrAuthContext.Provider>
  )
}

export function useNostrAuth(): NostrAuthState {
  const context = useContext(NostrAuthContext)
  if (!context) {
    throw new Error('useNostrAuth must be used within a NostrAuthProvider')
  }
  return context
}

// Extend window type for NIP-07
declare global {
  interface Window {
    nostr?: {
      getPublicKey(): Promise<string>
      signEvent(event: UnsignedEvent): Promise<Event>
      getRelays?(): Promise<Record<string, { read: boolean; write: boolean }>>
      nip04?: {
        encrypt(pubkey: string, plaintext: string): Promise<string>
        decrypt(pubkey: string, ciphertext: string): Promise<string>
      }
    }
  }
}