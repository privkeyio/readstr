import { AI_CONFIG_KEY } from './ai/config'
import { clearSummaryCache } from './ai/summary-cache'
import { clearHistory } from './reading-history'

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
] as const

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

/**
 * Clear everything that belongs to the signed-in user on this device. Each
 * step is best effort, so one failure (private mode, a blocked database)
 * doesn't leave the rest behind; resolves once every step has finished.
 */
export async function clearUserClientState(): Promise<void> {
  const steps: Promise<unknown>[] = [clearHistory(), clearSummaryCache()]
  try {
    for (const key of USER_STORAGE_KEYS) localStorage.removeItem(key)
  } catch {
    // localStorage can throw when storage is disabled.
  }
  if (typeof caches !== 'undefined') {
    for (const name of USER_CACHE_NAMES) steps.push(caches.delete(name))
  }
  await Promise.allSettled(steps)
}
