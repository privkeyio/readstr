import { AI_CONFIG_KEY } from './ai/config'
import { clearSummaryCache } from './ai/summary-cache'
import { APPLIED_CREATEDAT_PREFIX } from './nostr-sync'
import { clearHistory } from './reading-history'
import { USER_STATE_CLEARED_EVENT } from './user-state-events'

// Pubkey whose per-user data is currently on this device.
export const OWNER_PUBKEY_KEY = 'readstr_owner_pubkey'

// Per-user data a shared browser must not hand to the next person who signs
// in. Device preferences (theme, reading and layout settings, mark-read
// behaviour, relay list) are kept.
export const USER_STORAGE_KEYS = [
  AI_CONFIG_KEY, // holds the AI endpoint's API key
  'readstr_views',
  'readstr_active_view',
  'readstr_filters',
  'readstr_profiles',
  'readstr_last_sync',
  'readstr_prefs_sync',
  'last_feed_refresh',
  OWNER_PUBKEY_KEY,
] as const

// Prefixes of per-user keys with a dynamic suffix (sync watermarks per d-tag,
// which would make the next user's synced data look stale).
export const USER_STORAGE_PREFIXES = [APPLIED_CREATEDAT_PREFIX] as const

// Service worker caches that can hold signed-in API responses (older builds)
// or media from the user's feeds, which reveals what they follow.
export const USER_CACHE_NAMES = [
  'apis',
  'cross-origin',
  'next-image',
  'static-image-assets',
  'static-audio-assets',
  'static-video-assets',
] as const

const SIGN_OUT_CLEANUP_TIMEOUT_MS = 2000

function clearUserStorage(): void {
  try {
    const prefixed: string[] = []
    for (let i = 0; i < localStorage.length; i++) {
      const key = localStorage.key(i)
      if (key && USER_STORAGE_PREFIXES.some((p) => key.startsWith(p))) prefixed.push(key)
    }
    // Collect first: removing while indexing shifts the keys.
    for (const key of [...USER_STORAGE_KEYS, ...prefixed]) localStorage.removeItem(key)
  } catch {
    // localStorage can throw when storage is disabled.
  }
}

// Workbox keeps {url, cacheName, timestamp} rows for expiring caches, so the
// URLs outlive the caches themselves. Delete our rows rather than the database,
// which the service worker holds open.
function clearCacheExpirationEntries(): Promise<void> {
  if (typeof indexedDB === 'undefined') return Promise.resolve()
  return new Promise<void>((resolve, reject) => {
    const request = indexedDB.open('workbox-expiration')
    // Not created yet: abort so we don't leave an empty v1 database that would
    // stop workbox's own upgrade from creating its store.
    request.onupgradeneeded = () => request.transaction?.abort()
    request.onerror = () => reject(request.error)
    request.onsuccess = () => {
      const db = request.result
      if (!db.objectStoreNames.contains('cache-entries')) {
        db.close()
        resolve()
        return
      }
      const tx = db.transaction('cache-entries', 'readwrite')
      const index = tx.objectStore('cache-entries').index('cacheName')
      for (const name of USER_CACHE_NAMES) {
        const cursorReq = index.openKeyCursor(IDBKeyRange.only(name))
        cursorReq.onsuccess = () => {
          const cursor = cursorReq.result
          if (!cursor) return
          tx.objectStore('cache-entries').delete(cursor.primaryKey)
          cursor.continue()
        }
      }
      tx.oncomplete = () => {
        db.close()
        resolve()
      }
      tx.onerror = () => {
        db.close()
        reject(tx.error)
      }
      tx.onabort = () => {
        db.close()
        reject(tx.error)
      }
    }
  })
}

/**
 * Clear everything that belongs to the signed-in user on this device. Each
 * step is best effort, so one failure (private mode, a blocked database)
 * doesn't leave the rest behind; resolves once every step has finished.
 */
export async function clearUserClientState(): Promise<void> {
  const steps: Promise<unknown>[] = [clearHistory(), clearSummaryCache(), clearCacheExpirationEntries()]
  clearUserStorage()
  if (typeof caches !== 'undefined') {
    for (const name of USER_CACHE_NAMES) steps.push(caches.delete(name))
  }
  await Promise.allSettled(steps)
}

/**
 * Sign out and wipe this user's data. Disconnects first so the session (and a
 * NIP-46 client secret) is gone even if a database clear hangs, then bounds
 * the cleanup and clears storage again in case an in-flight writer re-created
 * a key meanwhile. Reloads so in-memory per-user state goes too.
 */
export async function signOut(disconnect: () => void): Promise<void> {
  try {
    disconnect()
    await Promise.race([
      clearUserClientState(),
      new Promise((resolve) => setTimeout(resolve, SIGN_OUT_CLEANUP_TIMEOUT_MS)),
    ])
  } finally {
    clearUserStorage()
    window.location.replace('/')
  }
}

/**
 * Record `pubkey` as this device's owner. If data from a different account is
 * still here (e.g. it never signed out), clear it first: the localStorage part
 * synchronously, so it's gone before the new user's UI reads it.
 */
export function claimDeviceForUser(pubkey: string): Promise<void> {
  let previous: string | null = null
  try {
    previous = localStorage.getItem(OWNER_PUBKEY_KEY)
  } catch {
    return Promise.resolve()
  }
  const switching = Boolean(previous && previous !== pubkey)
  const cleared = switching ? clearUserClientState() : Promise.resolve()
  if (switching && typeof window !== 'undefined' && typeof window.dispatchEvent === 'function') {
    window.dispatchEvent(new Event(USER_STATE_CLEARED_EVENT))
  }
  try {
    localStorage.setItem(OWNER_PUBKEY_KEY, pubkey)
  } catch {
    // Storage disabled: nothing persisted to guard.
  }
  return cleared
}

/**
 * Whether cached query results must be dropped for this account change. Not on
 * null -> pubkey (session restore), which would cancel the first queries.
 */
export function shouldClearOnAccountChange(prev: string | null, next: string | null): boolean {
  return prev !== null && prev !== next
}
