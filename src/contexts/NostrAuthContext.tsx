'use client'

import React, { createContext, useContext, useState, useEffect, useCallback, ReactNode } from 'react'
import { Event, nip19, UnsignedEvent } from 'nostr-tools'

const debugLog = (...args: unknown[]) => {
  if (process.env.NODE_ENV === 'development') console.log(...args)
}

export type NostrAuthMethod = 'nip07' | 'npub_readonly' | null

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
  connect: (method: NostrAuthMethod, credentials?: { npub?: string }) => Promise<void>
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

  // Check for existing connection on mount
  useEffect(() => {
    const restoreSession = async () => {
      await checkExistingConnection()
    }
    restoreSession()
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

  const connect = async (method: NostrAuthMethod, credentials?: { npub?: string }) => {
    try {
      switch (method) {
        case 'nip07':
          await connectNIP07()
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
        ? 'No Nostr signer detected. Please install Amber from https://github.com/greenart7c3/Amber/releases'
        : 'NIP-07 extension not found. Please install a Nostr browser extension like Alby, nos2x, or Amber.'
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

        case 'npub_readonly':
          throw new Error('Cannot sign events in read-only npub view. Use a NIP-07 extension for signing.')

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

  const value: NostrAuthState = {
    isConnected,
    user,
    authMethod,
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