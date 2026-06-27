import type {
  Feed,
  FeedItem,
  ExtensionSettings,
  StorageData,
  FeedsResponse,
  FeedItemsResponse,
  MessageResponse,
  LocalFeed,
  NostrAuthData,
} from './types';
import {
  decodeNsec,
  getPublicKeyFromPrivate,
  encodeNpub,
  generateAuthHeader,
  buildUnsignedNip98Event,
  encodeNip98AuthHeader,
} from './nostr';
import type { NostrEvent, UnsignedEvent } from './nostr';
import { withRetry } from './utils/retry';
import { validateWebAppUrl, normalizeWebAppUrl, isSameOrigin } from './utils/webAppUrl';
import { feedDatabase } from './db/feedDatabase';

const ALARM_NAME = 'refresh-feeds';
const DEFAULT_POLL_INTERVAL = 5;
const MAX_SEEN_ITEMS = 1000;
const DEFAULT_WEB_APP_URL = 'https://readstr.privkey.io:8444';

const ALLOWED_PROTOCOLS = ['https:', 'http:'];

class AuthUnavailableError extends Error {
  readonly retryable = false;
  constructor() {
    super('Re-authentication required');
    this.name = 'AuthUnavailableError';
  }
}

function sanitizeUrl(urlString: string): string | null {
  try {
    const url = new URL(urlString);
    if (!ALLOWED_PROTOCOLS.includes(url.protocol)) return null;
    return url.href;
  } catch {
    return null;
  }
}

function normalizeBaseUrl(urlString: string): string | null {
  return normalizeWebAppUrl(urlString);
}

const defaultSettings: ExtensionSettings = {
  webAppUrl: DEFAULT_WEB_APP_URL,
  pollIntervalMinutes: DEFAULT_POLL_INTERVAL,
  notificationsEnabled: true,
  notifyOnNewItems: true,
  maxNotificationsPerRefresh: 3,
  lastSyncTime: null,
  theme: 'system',
  showUnreadOnly: false,
};

interface NotificationData {
  itemId: string;
  itemUrl: string | null;
  webAppUrl: string;
}

const notificationDataCache = new Map<string, NotificationData | 'batch'>();

async function getStorageData(): Promise<StorageData> {
  const result = await chrome.storage.local.get([
    'feeds',
    'seenItemIds',
    'settings',
    'authToken',
    'nostrAuth',
  ]);
  return {
    feeds: (result['feeds'] as Feed[] | undefined) ?? [],
    seenItemIds: (result['seenItemIds'] as string[] | undefined) ?? [],
    settings: (result['settings'] as ExtensionSettings | undefined) ?? defaultSettings,
    authToken: (result['authToken'] as string | undefined) ?? null,
    nostrAuth: (result['nostrAuth'] as NostrAuthData | undefined) ?? null,
  };
}

async function saveStorageData(data: Partial<StorageData>): Promise<void> {
  await chrome.storage.local.set(data);
}

// nip07 signing is delegated to a page context with window.nostr (see
// requestNip98FromOpenTab). When no readstr tab is open or the signer declines,
// we cannot produce a signed NIP-98 event, so we surface a one-time warning
// rather than emitting an unsigned request that silently 401s. Reset to false on
// a successful signature so later breakage warns again.
let warnedNip07Unauthenticated = false;

// The raw nsec-derived key is kept only in the service worker's runtime memory
// and is never written to chrome.storage.local. If the worker is torn down the
// user must re-authenticate, which is the correct tradeoff for a root identity key.
let sessionPrivateKeyHex: string | null = null;

// The nsec session key lives only in worker memory; after an MV3 teardown it is
// gone while persisted nostrAuth still says method:'nsec'. Warn once so the user
// re-authenticates instead of silently 401ing. Reset on a fresh nsec login.
let warnedNsecSessionExpired = false;

async function warnNip07Unauthenticated(): Promise<void> {
  if (warnedNip07Unauthenticated) return;
  warnedNip07Unauthenticated = true;
  try {
    await chrome.notifications.create({
      type: 'basic',
      iconUrl: 'icons/icon128.png',
      title: 'Limited functionality',
      message:
        'Open readstr.privkey.io in a tab and approve the signing request to sync. ' +
        'Browser extension signing (NIP-07) needs an open readstr tab with your signer.',
    });
  } catch {
    // Notifications may be unavailable; the warning is best-effort.
  }
}

async function warnNsecSessionExpired(): Promise<void> {
  if (warnedNsecSessionExpired) return;
  warnedNsecSessionExpired = true;
  try {
    await chrome.notifications.create({
      type: 'basic',
      iconUrl: 'icons/icon128.png',
      title: 'Sign in again',
      message:
        'Your readstr session expired. Open the extension and re-enter your nsec to keep syncing.',
    });
  } catch {
    // Notifications may be unavailable; the warning is best-effort.
  }
}

// nip07 signing happens in a page context that owns window.nostr. We relay an
// unsigned NIP-98 event to an open readstr tab, where content.ts hands it to the
// page-world signer and returns the signed event. Returns null when no eligible
// tab/signer is available or the user declines.
// Keep in sync with the page-signer content_scripts match list in manifest.json.
const NOSTR_TAB_URLS = [
  '*://readstr.privkey.io/*',
  '*://*.readstr.privkey.io/*',
  ...__EXTENSION_DEV_MATCHES__,
];

// Serialize nip07 page-context signing. refreshFeeds fires getFeeds and
// getFeedItems concurrently (Promise.all), and each protected request triggers
// its own window.nostr.signEvent round-trip. NIP-07 signers commonly process
// signing requests one at a time and reject/drop a second concurrent request;
// the dropped one yields no auth header -> unsigned fetch -> 401 -> the shared
// Promise.all rejects and the whole refresh falls back to stale cache (badge
// stays correct from cached feed counts while the Recent list shows drifted,
// locally-read items). Chaining signing so only one round-trip is in flight at a
// time lets both fetches authenticate and keeps feeds and items consistent.
let nip07SigningChain: Promise<unknown> = Promise.resolve();

function serializeNip07Signing<T>(fn: () => Promise<T>): Promise<T> {
  const run = nip07SigningChain.then(fn, fn);
  nip07SigningChain = run.then(
    () => undefined,
    () => undefined
  );
  return run;
}

async function requestNip98FromOpenTab(
  unsignedEvent: UnsignedEvent
): Promise<NostrEvent | null> {
  let tabs: chrome.tabs.Tab[];
  try {
    tabs = await chrome.tabs.query({ url: NOSTR_TAB_URLS });
  } catch {
    return null;
  }

  for (const tab of tabs) {
    if (!tab.id) continue;
    try {
      const response = (await chrome.tabs.sendMessage(tab.id, {
        type: 'SIGN_NIP98',
        event: unsignedEvent,
      })) as { signed?: NostrEvent; error?: string } | undefined;
      if (response?.signed) return response.signed;
    } catch {
      continue;
    }
  }
  return null;
}

async function getNostrAuthHeader(
  url: string,
  method: string,
  nostrAuth: NostrAuthData | null,
  body: string | null = null
): Promise<string | null> {
  if (!nostrAuth || !nostrAuth.pubkey) return null;

  if (nostrAuth.method === 'nsec') {
    if (sessionPrivateKeyHex) {
      return generateAuthHeader(url, method, nostrAuth.pubkey, sessionPrivateKeyHex, body);
    }
    // Worker was torn down and the in-memory key is gone; prompt re-auth instead
    // of emitting an unsigned request that silently 401s and burns retries.
    void warnNsecSessionExpired();
    return null;
  }

  if (nostrAuth.method === 'nip07') {
    const unsigned = buildUnsignedNip98Event(url, method, nostrAuth.pubkey, body);
    const signed = await serializeNip07Signing(() => requestNip98FromOpenTab(unsigned));
    if (signed) {
      warnedNip07Unauthenticated = false;
      return encodeNip98AuthHeader(signed);
    }
    // No open readstr tab / signer declined: surface the limited-functionality
    // state instead of sending an unsigned request that silently 401s.
    void warnNip07Unauthenticated();
  }
  return null;
}

// Attach credentials only when the request targets the validated web app
// origin. authToken is a plaintext bearer; if webAppUrl is ever pointed at an
// attacker host (synced/tampered settings, self-XSS) this guard fails closed
// instead of leaking the token. baseUrl is always normalizeBaseUrl(webAppUrl).
async function applyAuthHeaders(
  headers: Record<string, string>,
  url: string,
  baseUrl: string,
  method: string,
  authToken: string | null,
  nostrAuth: NostrAuthData | null,
  body: string | null = null
): Promise<void> {
  if (!isSameOrigin(url, baseUrl)) {
    throw new Error('Refusing to send credentials to an untrusted origin');
  }

  if (authToken) {
    headers['Authorization'] = `Bearer ${authToken}`;
  } else if (nostrAuth && nostrAuth.pubkey) {
    // Authenticate via signed NIP-98 event; the server derives identity from
    // the verified signature, not from any plaintext pubkey header.
    const nostrHeader = await getNostrAuthHeader(url, method, nostrAuth, body);
    if (nostrHeader) {
      headers['Authorization'] = nostrHeader;
    } else {
      // Signing material is gone (worker evicted) or no open tab to sign:
      // skip the request instead of emitting an unsigned one that 401s. For
      // 'nsec'/'nip07' getNostrAuthHeader already fired the re-auth notification;
      // other methods just fail closed here without one.
      throw new AuthUnavailableError();
    }
  }
}

async function fetchWithAuth<T>(
  url: string,
  baseUrl: string,
  authToken: string | null,
  options: RequestInit = {},
  nostrAuth: NostrAuthData | null = null
): Promise<T> {
  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
    ...(options.headers as Record<string, string> | undefined),
  };

  await applyAuthHeaders(headers, url, baseUrl, options.method ?? 'GET', authToken, nostrAuth);

  const response = await fetch(url, {
    ...options,
    headers,
    credentials: 'include',
  });

  if (!response.ok) {
    throw new Error(`API request failed: ${response.status} ${response.statusText}`);
  }

  return response.json() as Promise<T>;
}

async function fetchFeeds(
  settings: ExtensionSettings,
  authToken: string | null,
  nostrAuth: NostrAuthData | null = null,
  forceSync = false
): Promise<Feed[]> {
  const baseUrl = normalizeBaseUrl(settings.webAppUrl);
  if (!baseUrl) {
    throw new Error('Invalid web app URL');
  }
  const input = JSON.stringify({ json: { forceSync } });
  const url = `${baseUrl}/api/trpc/feed.getFeeds?input=${encodeURIComponent(input)}`;

  return withRetry(async () => {
    const response = await fetchWithAuth<{ result: { data: { json: FeedsResponse[] } } }>(
      url,
      baseUrl,
      authToken,
      {},
      nostrAuth
    );
    return response.result.data.json;
  });
}

// Fetch unread items for the Recent list. getFeedItems orders by publishedAt
// desc, so a plain (read+unread) page only surfaces unread items that happen to
// be among the newest N — the badge (server-side total unread from getFeeds)
// then drifts above the visible unread count when older items are still unread.
// Request unreadOnly and page through nextCursor so Recent stays consistent with
// the badge and the reader.
async function fetchNewItems(
  settings: ExtensionSettings,
  authToken: string | null,
  nostrAuth: NostrAuthData | null = null,
  limit = 50
): Promise<FeedItem[]> {
  const baseUrl = normalizeBaseUrl(settings.webAppUrl);
  if (!baseUrl) {
    throw new Error('Invalid web app URL');
  }

  const MAX_PAGES = 5;
  const collected: FeedItem[] = [];
  let cursor: string | undefined;

  for (let page = 0; page < MAX_PAGES; page++) {
    const input = JSON.stringify({ json: { limit, unreadOnly: true, cursor } });
    const url = `${baseUrl}/api/trpc/feed.getFeedItems?input=${encodeURIComponent(input)}`;

    const data = await withRetry(async () => {
      const response = await fetchWithAuth<{ result: { data: { json: FeedItemsResponse } } }>(
        url,
        baseUrl,
        authToken,
        {},
        nostrAuth
      );
      return response.result.data.json;
    });

    collected.push(...data.items);
    if (!data.nextCursor) break;
    cursor = data.nextCursor;

    if (page === MAX_PAGES - 1) {
      console.warn(
        `fetchNewItems: hit MAX_PAGES (${MAX_PAGES}) cap with more unread items remaining; list truncated to ${collected.length}.`
      );
    }
  }

  return collected;
}

async function markItemAsRead(itemId: string): Promise<void> {
  const storage = await getStorageData();
  const { settings, authToken, nostrAuth } = storage;

  const hasAuth = authToken || (nostrAuth?.pubkey && nostrAuth.method !== 'none');
  if (!hasAuth) return;

  const baseUrl = normalizeBaseUrl(settings.webAppUrl);
  if (!baseUrl) return;
  const url = `${baseUrl}/api/trpc/feed.markAsRead`;
  const body = JSON.stringify({ json: { itemId } });

  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
  };
  await applyAuthHeaders(headers, url, baseUrl, 'POST', authToken, nostrAuth, body);

  await fetch(url, {
    method: 'POST',
    headers,
    credentials: 'include',
    body,
  });
}

async function markAllAsRead(): Promise<void> {
  const storage = await getStorageData();
  const { settings, authToken, nostrAuth } = storage;

  const hasAuth = authToken || (nostrAuth?.pubkey && nostrAuth.method !== 'none');
  if (!hasAuth) return;

  const baseUrl = normalizeBaseUrl(settings.webAppUrl);
  if (!baseUrl) return;
  const url = `${baseUrl}/api/trpc/feed.markAllAsRead`;
  const body = JSON.stringify({ json: {} });

  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
  };
  await applyAuthHeaders(headers, url, baseUrl, 'POST', authToken, nostrAuth, body);

  await fetch(url, {
    method: 'POST',
    headers,
    credentials: 'include',
    body,
  });

  updateBadge(0);
  await refreshFeeds();
}

async function addFavorite(itemId: string): Promise<void> {
  const storage = await getStorageData();
  const { settings, authToken, nostrAuth } = storage;

  const hasAuth = authToken || (nostrAuth?.pubkey && nostrAuth.method !== 'none');
  if (!hasAuth) return;

  const baseUrl = normalizeBaseUrl(settings.webAppUrl);
  if (!baseUrl) return;
  const url = `${baseUrl}/api/trpc/feed.addFavorite`;
  const body = JSON.stringify({ json: { itemId } });

  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
  };
  await applyAuthHeaders(headers, url, baseUrl, 'POST', authToken, nostrAuth, body);

  await fetch(url, {
    method: 'POST',
    headers,
    credentials: 'include',
    body,
  });
}

async function removeFavorite(itemId: string): Promise<void> {
  const storage = await getStorageData();
  const { settings, authToken, nostrAuth } = storage;

  const hasAuth = authToken || (nostrAuth?.pubkey && nostrAuth.method !== 'none');
  if (!hasAuth) return;

  const baseUrl = normalizeBaseUrl(settings.webAppUrl);
  if (!baseUrl) return;
  const url = `${baseUrl}/api/trpc/feed.removeFavorite`;
  const body = JSON.stringify({ json: { itemId } });

  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
  };
  await applyAuthHeaders(headers, url, baseUrl, 'POST', authToken, nostrAuth, body);

  await fetch(url, {
    method: 'POST',
    headers,
    credentials: 'include',
    body,
  });
}

async function fetchFavorites(
  settings: ExtensionSettings,
  authToken: string | null,
  nostrAuth: NostrAuthData | null = null
): Promise<FeedItem[]> {
  const baseUrl = normalizeBaseUrl(settings.webAppUrl);
  if (!baseUrl) {
    throw new Error('Invalid web app URL');
  }

  const input = JSON.stringify({ json: { limit: 50 } });
  const url = `${baseUrl}/api/trpc/feed.getFavorites?input=${encodeURIComponent(input)}`;

  return withRetry(async () => {
    const response = await fetchWithAuth<{ result: { data: { json: { items: FeedItem[] } } } }>(
      url,
      baseUrl,
      authToken,
      {},
      nostrAuth
    );
    return response.result.data.json.items;
  });
}

function updateBadge(count: number): void {
  const text = count > 0 ? (count > 99 ? '99+' : String(count)) : '';
  void chrome.action.setBadgeText({ text });
  void chrome.action.setBadgeBackgroundColor({ color: '#ef4444' });
}

async function showNotification(item: FeedItem, webAppUrl: string): Promise<void> {
  const notificationId = `item-${item.id}-${Date.now()}`;

  notificationDataCache.set(notificationId, {
    itemId: item.id,
    itemUrl: item.url ?? item.originalUrl ?? null,
    webAppUrl,
  });

  await chrome.notifications.create(notificationId, {
    type: 'basic',
    iconUrl: 'icons/icon128.png',
    title: item.feedTitle,
    message: item.title,
    contextMessage: item.author ?? undefined,
    buttons: [
      { title: 'Open' },
      { title: 'Mark as Read' },
    ],
    priority: 1,
  });

  setTimeout(() => notificationDataCache.delete(notificationId), 60000);
}

async function showBatchNotification(count: number, feedTitle?: string): Promise<void> {
  const notificationId = `batch-${Date.now()}`;
  const title = feedTitle ? `New items from ${feedTitle}` : 'New items in your feeds';
  const message = `${count} new item${count > 1 ? 's' : ''} available`;

  notificationDataCache.set(notificationId, 'batch');

  await chrome.notifications.create(notificationId, {
    type: 'basic',
    iconUrl: 'icons/icon128.png',
    title,
    message,
    buttons: [
      { title: 'Open App' },
      { title: 'Mark All as Read' },
    ],
    priority: 1,
  });

  setTimeout(() => notificationDataCache.delete(notificationId), 60000);
}

async function loadCachedData(): Promise<{ feeds: Feed[]; items: FeedItem[] }> {
  try {
    const [feeds, items] = await Promise.all([
      feedDatabase.getFeeds(),
      feedDatabase.getItems({ limit: 100 }),
    ]);
    return { feeds, items };
  } catch {
    return { feeds: [], items: [] };
  }
}

async function tryRestoreAuthFromOpenTabs(): Promise<boolean> {
  const storage = await getStorageData();
  if (storage.authToken || (storage.nostrAuth?.pubkey && storage.nostrAuth.method !== 'none')) {
    return true;
  }

  try {
    const tabs = await chrome.tabs.query({ url: NOSTR_TAB_URLS });

    for (const tab of tabs) {
      if (!tab.id) continue;
      try {
        const response = await chrome.tabs.sendMessage(tab.id, { type: 'GET_SESSION' }) as {
          session?: { pubkey: string; npub: string; method: string } | null;
        };
        if (response?.session?.pubkey) {
          const nostrAuth: NostrAuthData = {
            method: response.session.method === 'nip07' ? 'nip07' : 'none',
            pubkey: response.session.pubkey,
            npub: response.session.npub,
          };
          await saveStorageData({ nostrAuth });
          return true;
        }
      } catch {
        continue;
      }
    }
  } catch {
    return false;
  }
  return false;
}

async function refreshFeeds(forceSync = false): Promise<{ newItemCount: number; error?: string }> {
  const storage = await getStorageData();
  const { settings, authToken, seenItemIds, nostrAuth } = storage;

  const hasAuth = authToken || (nostrAuth?.pubkey && nostrAuth.method !== 'none');
  if (!hasAuth) {
    const cached = await loadCachedData();
    if (cached.feeds.length > 0) {
      const totalUnread = cached.feeds.reduce((sum, feed) => sum + feed.unreadCount, 0);
      updateBadge(totalUnread);
      await chrome.storage.local.set({
        feeds: cached.feeds,
        recentItems: cached.items.slice(0, 50).map(item => ({ ...item, feedId: '' })),
      });
    }
    return { newItemCount: 0, error: 'Not authenticated' };
  }

  try {
    const [feeds, items] = await Promise.all([
      fetchFeeds(settings, authToken, nostrAuth, forceSync),
      fetchNewItems(settings, authToken, nostrAuth, 50),
    ]);

    await Promise.all([
      feedDatabase.saveFeeds(feeds),
      feedDatabase.saveItems(items),
      feedDatabase.clearOldItems(),
    ]);

    const seenSet = new Set(seenItemIds);
    const newItems = items.filter((item) => !item.isRead && !seenSet.has(item.id));

    const totalUnread = feeds.reduce((sum, feed) => sum + feed.unreadCount, 0);
    updateBadge(totalUnread);

    if (newItems.length > 0 && settings.notificationsEnabled && settings.notifyOnNewItems) {
      const maxNotifications = settings.maxNotificationsPerRefresh;
      if (newItems.length <= maxNotifications) {
        for (const item of newItems) {
          await showNotification(item, settings.webAppUrl);
        }
      } else {
        await showBatchNotification(newItems.length);
      }
    }

    const newSeenIds = [...seenItemIds, ...newItems.map((item) => item.id)];
    const trimmedSeenIds = newSeenIds.slice(-MAX_SEEN_ITEMS);

    const recentItems = items.slice(0, 50).map(item => ({
      ...item,
      feedId: '',
    }));

    await saveStorageData({
      feeds,
      seenItemIds: trimmedSeenIds,
      settings: {
        ...settings,
        lastSyncTime: new Date().toISOString(),
      },
    });
    await chrome.storage.local.set({ recentItems });

    return { newItemCount: newItems.length };
  } catch (error) {
    const errorMessage = error instanceof Error ? error.message : 'Unknown error';
    if (error instanceof AuthUnavailableError) {
      console.warn('Feed refresh skipped, re-authentication required; using cache');
    } else {
      console.error('Feed refresh failed, using cache:', errorMessage);
    }

    const cached = await loadCachedData();
    if (cached.feeds.length > 0) {
      const totalUnread = cached.feeds.reduce((sum, feed) => sum + feed.unreadCount, 0);
      updateBadge(totalUnread);
      await chrome.storage.local.set({
        feeds: cached.feeds,
        recentItems: cached.items.slice(0, 50).map(item => ({ ...item, feedId: '' })),
      });
    }

    return { newItemCount: 0, error: errorMessage };
  }
}

async function setupAlarm(): Promise<void> {
  const storage = await getStorageData();
  const periodInMinutes = storage.settings.pollIntervalMinutes || DEFAULT_POLL_INTERVAL;

  await chrome.alarms.clear(ALARM_NAME);
  await chrome.alarms.create(ALARM_NAME, {
    delayInMinutes: 1,
    periodInMinutes,
  });
}

const MANUAL_REFRESH_COOLDOWN_MS = 5 * 60 * 1000;

async function getLastManualRefreshTime(): Promise<number> {
  const result = await chrome.storage.session.get('lastManualRefreshTime');
  return (result['lastManualRefreshTime'] as number | undefined) ?? 0;
}

async function setLastManualRefreshTime(time: number): Promise<void> {
  await chrome.storage.session.set({ lastManualRefreshTime: time });
}

async function isSyncOverdue(): Promise<boolean> {
  const storage = await getStorageData();
  const { settings } = storage;
  const lastSyncTime = settings.lastSyncTime;
  if (!lastSyncTime) return true;

  const lastSync = new Date(lastSyncTime).getTime();
  const pollInterval = (settings.pollIntervalMinutes || DEFAULT_POLL_INTERVAL) * 60 * 1000;
  return Date.now() - lastSync > pollInterval;
}

async function handleMessage(
  message: { type: string; [key: string]: unknown },
  sender: chrome.runtime.MessageSender
): Promise<MessageResponse> {
  switch (message.type) {
    case 'REFRESH_FEEDS': {
      const forceSync = message['forceSync'] as boolean | undefined;
      const now = Date.now();
      const lastManualRefresh = await getLastManualRefreshTime();

      if (forceSync !== false && (now - lastManualRefresh < MANUAL_REFRESH_COOLDOWN_MS)) {
        return { success: true, data: { newItemCount: 0, note: 'Cooldown active' } };
      }

      if (forceSync !== false) {
        await setLastManualRefreshTime(now);
      }

      await tryRestoreAuthFromOpenTabs();
      const result = await refreshFeeds(forceSync ?? true);
      const response: MessageResponse = {
        success: !result.error,
        data: { newItemCount: result.newItemCount },
      };
      if (result.error) {
        response.error = result.error;
      }
      return response;
    }

    case 'GET_TAB_INFO': {
      return {
        success: true,
        data: { tabId: sender.tab?.id, url: sender.tab?.url },
      };
    }

    case 'SET_AUTH_TOKEN': {
      const token = message['token'] as string;
      await saveStorageData({ authToken: token });
      void refreshFeeds();
      return { success: true };
    }

    case 'CLEAR_AUTH': {
      sessionPrivateKeyHex = null;
      await saveStorageData({
        authToken: null,
        nostrAuth: null,
        feeds: [],
        seenItemIds: [],
      });
      updateBadge(0);
      return { success: true };
    }

    case 'NOSTR_LOGIN': {
      const nsec = message['nsec'] as string | undefined;
      const pubkeyHex = message['pubkey'] as string | undefined;
      const method = message['method'] as 'nsec' | 'nip07';

      try {
        let pubkey: string;
        let npub: string;

        if (method === 'nsec' && nsec) {
          const decodedKey = decodeNsec(nsec);
          if (!decodedKey) {
            return { success: false, error: 'Invalid nsec key' };
          }
          pubkey = getPublicKeyFromPrivate(decodedKey);
          npub = encodeNpub(pubkey);
          sessionPrivateKeyHex = decodedKey;
          warnedNsecSessionExpired = false;
        } else if (method === 'nip07' && pubkeyHex) {
          pubkey = pubkeyHex;
          npub = encodeNpub(pubkeyHex);
          sessionPrivateKeyHex = null;
        } else {
          return { success: false, error: 'Invalid login parameters' };
        }

        const nostrAuth: NostrAuthData = {
          method,
          pubkey,
          npub,
        };

        await saveStorageData({ nostrAuth });
        void refreshFeeds();
        return { success: true, data: { npub, pubkey } };
      } catch (error) {
        const errorMessage = error instanceof Error ? error.message : 'Login failed';
        return { success: false, error: errorMessage };
      }
    }

    case 'NOSTR_LOGOUT': {
      sessionPrivateKeyHex = null;
      await saveStorageData({
        nostrAuth: null,
        feeds: [],
        seenItemIds: [],
      });
      updateBadge(0);
      return { success: true };
    }

    case 'SYNC_WEB_AUTH': {
      const session = message['session'] as {
        pubkey: string;
        npub: string;
        method: string;
      } | null;

      if (!session || !session.pubkey) {
        return { success: true };
      }

      const existing = await getStorageData();
      const existingPubkey = existing.nostrAuth?.pubkey ?? null;
      const hasActiveAccount =
        !!existing.authToken ||
        (existingPubkey != null && existing.nostrAuth?.method !== 'none');

      // Never silently switch accounts: only adopt when the extension has no
      // active account, or when the pubkeys already match.
      if (hasActiveAccount && existingPubkey !== session.pubkey) {
        return { success: true };
      }

      // Never downgrade an nsec (root-key) session to a weaker/keyless one,
      // even if the in-memory key was lost on a service-worker restart.
      if (existing.nostrAuth?.method === 'nsec') {
        return { success: true };
      }

      const nostrAuth: NostrAuthData = {
        method: session.method === 'nip07' ? 'nip07' : 'none',
        pubkey: session.pubkey,
        npub: session.npub,
      };

      await saveStorageData({ nostrAuth });
      void refreshFeeds();
      return { success: true, data: { npub: session.npub, pubkey: session.pubkey } };
    }

    case 'GET_NOSTR_AUTH': {
      const storage = await getStorageData();
      return {
        success: true,
        data: {
          nostrAuth: storage.nostrAuth,
          isAuthenticated: !!(storage.authToken || storage.nostrAuth?.pubkey),
        },
      };
    }

    case 'GET_UNREAD_COUNT': {
      const storage = await getStorageData();
      const totalUnread = storage.feeds.reduce((sum, feed) => sum + feed.unreadCount, 0);
      return { success: true, data: { unreadCount: totalUnread } };
    }

    case 'MARK_ITEM_READ': {
      const itemId = message['itemId'] as string;
      if (!itemId) {
        return { success: false, error: 'Missing itemId' };
      }
      try {
        await Promise.all([
          markItemAsRead(itemId).catch((error) => {
            if (error instanceof AuthUnavailableError) {
              console.warn('Server mark-as-read skipped, re-authentication required; updating locally');
            } else {
              throw error;
            }
          }),
          feedDatabase.markItemRead(itemId),
        ]);
        const result = await chrome.storage.local.get(['recentItems', 'feeds']);
        const recentItems = (result['recentItems'] as any[] | undefined) ?? [];
        const feeds = (result['feeds'] as Feed[] | undefined) ?? [];

        const item = recentItems.find((i) => i.id === itemId);
        const wasUnread = item && !item.isRead;

        const updatedItems = recentItems.map((i) =>
          i.id === itemId ? { ...i, isRead: true } : i
        );

        if (wasUnread && item?.feedTitle) {
          const updatedFeeds = feeds.map((feed) =>
            feed.title === item.feedTitle && feed.unreadCount > 0
              ? { ...feed, unreadCount: feed.unreadCount - 1 }
              : feed
          );
          await chrome.storage.local.set({ recentItems: updatedItems, feeds: updatedFeeds });
          const totalUnread = updatedFeeds.reduce((sum, feed) => sum + feed.unreadCount, 0);
          updateBadge(totalUnread);
        } else {
          await chrome.storage.local.set({ recentItems: updatedItems });
        }

        return { success: true };
      } catch (error) {
        const errorMessage = error instanceof Error ? error.message : 'Failed to mark as read';
        return { success: false, error: errorMessage };
      }
    }

    case 'ADD_FAVORITE': {
      const itemId = message['itemId'] as string;
      if (!itemId) {
        return { success: false, error: 'Missing itemId' };
      }
      try {
        await addFavorite(itemId);
        return { success: true };
      } catch (error) {
        const errorMessage = error instanceof Error ? error.message : 'Failed to add favorite';
        return { success: false, error: errorMessage };
      }
    }

    case 'REMOVE_FAVORITE': {
      const itemId = message['itemId'] as string;
      if (!itemId) {
        return { success: false, error: 'Missing itemId' };
      }
      try {
        await removeFavorite(itemId);
        return { success: true };
      } catch (error) {
        const errorMessage = error instanceof Error ? error.message : 'Failed to remove favorite';
        return { success: false, error: errorMessage };
      }
    }

    case 'GET_FAVORITES': {
      try {
        const storage = await getStorageData();
        const { settings, authToken, nostrAuth } = storage;
        const hasAuth = authToken || (nostrAuth?.pubkey && nostrAuth.method !== 'none');
        if (!hasAuth) {
          return { success: true, data: { items: [] } };
        }
        const items = await fetchFavorites(settings, authToken, nostrAuth);
        return { success: true, data: { items } };
      } catch (error) {
        const errorMessage = error instanceof Error ? error.message : 'Failed to get favorites';
        return { success: false, error: errorMessage, data: { items: [] } };
      }
    }

    case 'UPDATE_SETTINGS': {
      const newSettings = message['settings'] as Partial<ExtensionSettings>;
      if (newSettings.webAppUrl !== undefined && !validateWebAppUrl(newSettings.webAppUrl)) {
        return { success: false, error: 'Invalid web app URL' };
      }
      const storage = await getStorageData();
      const updatedSettings = { ...storage.settings, ...newSettings };
      await saveStorageData({ settings: updatedSettings });

      if (newSettings.pollIntervalMinutes) {
        await setupAlarm();
      }
      return { success: true };
    }

    case 'SUBSCRIBE_SELECTED_FEEDS': {
      const feeds = (message['feeds'] as { url: string; title: string }[] | undefined) ?? [];
      let added = 0;
      for (const feed of feeds) {
        if (await addFeedToStorage(feed.url, feed.title, false)) {
          added += 1;
        }
      }
      await chrome.notifications.create({
        type: 'basic',
        iconUrl: 'icons/icon128.png',
        title: added > 0 ? 'Feeds Subscribed' : 'No New Feeds',
        message:
          added > 0
            ? `Subscribed to ${added} of ${feeds.length} feed${feeds.length === 1 ? '' : 's'}`
            : `All ${feeds.length} feed${feeds.length === 1 ? '' : 's'} were already added or invalid`,
      });
      return { success: true, data: { added, total: feeds.length } };
    }

    default:
      return { success: false, error: `Unknown message type: ${message.type}` };
  }
}

const MENU_ID_PAGE_FEEDS = 'readstr-page-feeds';
const MENU_ID_SUBSCRIBE_LINK = 'readstr-subscribe-link';
const MENU_ID_PARENT = 'readstr-parent';

// Injected on demand into the active tab via chrome.scripting.executeScript when
// the user invokes the context menu, so feed detection no longer requires a
// standing all-URLs content script. Must be self-contained (runs in the page).
function detectFeedsInPage(): { url: string; title: string; type: string }[] {
  const feeds: { url: string; title: string; type: string }[] = [];
  const seen = new Set<string>();

  const linkSelectors = [
    'link[rel="alternate"][type="application/rss+xml"]',
    'link[rel="alternate"][type="application/atom+xml"]',
    'link[rel="alternate"][type="application/feed+json"]',
    'link[rel="feed"]',
  ];

  linkSelectors.forEach((selector) => {
    document.querySelectorAll<HTMLLinkElement>(selector).forEach((link) => {
      const href = link.href;
      if (!href || seen.has(href)) return;
      seen.add(href);

      const type = link.type?.includes('atom') ? 'atom' : 'rss';
      const title = link.title || document.title || new URL(href).hostname;

      feeds.push({ url: href, title, type });
    });
  });

  const aSelectors = [
    'a[href*="/feed"]',
    'a[href*="/rss"]',
    'a[href*=".rss"]',
    'a[href*=".xml"]',
    'a[href*="atom"]',
  ];

  aSelectors.forEach((selector) => {
    document.querySelectorAll<HTMLAnchorElement>(selector).forEach((link) => {
      const href = link.href;
      if (!href || seen.has(href)) return;

      const url = new URL(href);
      const path = url.pathname.toLowerCase();
      const isLikelyFeed =
        path.includes('/feed') ||
        path.includes('/rss') ||
        path.endsWith('.rss') ||
        path.endsWith('.xml') ||
        path.includes('atom');

      if (!isLikelyFeed) return;
      seen.add(href);

      const type = path.includes('atom') ? 'atom' : 'rss';
      const title = link.textContent?.trim() || document.title || url.hostname;

      feeds.push({ url: href, title, type });
    });
  });

  return feeds;
}

async function subscribeDetectedFeedsForTab(tabId: number, tab?: chrome.tabs.Tab): Promise<void> {
  let detected: { url: string; title: string; type: string }[] = [];
  try {
    const results = await chrome.scripting.executeScript({
      target: { tabId },
      func: detectFeedsInPage,
    });
    detected = (results[0]?.result as typeof detected | undefined) ?? [];
  } catch (err) {
    console.error('Feed detection failed:', err);
    await chrome.notifications.create({
      type: 'basic',
      iconUrl: 'icons/icon128.png',
      title: 'Error',
      message: 'Could not detect feeds on this page',
    });
    return;
  }

  if (detected.length === 0) {
    await chrome.notifications.create({
      type: 'basic',
      iconUrl: 'icons/icon128.png',
      title: 'No feeds detected',
      message: 'No RSS/Atom feeds were found on this page',
    });
    return;
  }

  await chrome.storage.session.set({
    pendingDetectedFeeds: {
      feeds: detected,
      sourceTitle: tab?.title ?? '',
      sourceUrl: tab?.url ?? '',
    },
  });

  try {
    await chrome.action.openPopup();
  } catch {
    await chrome.notifications.create({
      type: 'basic',
      iconUrl: 'icons/icon128.png',
      title: detected.length === 1 ? '1 feed detected' : `${detected.length} feeds detected`,
      message: 'Click the extension icon to choose which feeds to subscribe',
    });
  }
}

function generateId(): string {
  return `${Date.now()}-${Math.random().toString(36).slice(2, 9)}`;
}

function isLikelyFeedUrl(url: string): boolean {
  try {
    const parsed = new URL(url);
    const path = parsed.pathname.toLowerCase();
    const search = parsed.search.toLowerCase();
    return (
      path.includes('/feed') ||
      path.includes('/rss') ||
      path.endsWith('.rss') ||
      path.endsWith('.xml') ||
      path.endsWith('.atom') ||
      path.includes('atom') ||
      search.includes('feed') ||
      search.includes('rss')
    );
  } catch {
    return false;
  }
}

async function addFeedToStorage(feedUrl: string, feedTitle: string, notify = true): Promise<boolean> {
  try {
    const sanitizedFeedUrl = sanitizeUrl(feedUrl);
    if (!sanitizedFeedUrl) {
      if (notify) {
        await chrome.notifications.create({
          type: 'basic',
          iconUrl: 'icons/icon128.png',
          title: 'Invalid URL',
          message: 'The feed URL is not valid',
        });
      }
      return false;
    }

    const result = await chrome.storage.sync.get(['localFeeds']);
    const localFeeds: LocalFeed[] = (result['localFeeds'] as LocalFeed[] | undefined) ?? [];

    const exists = localFeeds.some((f) => f.url === sanitizedFeedUrl);
    if (exists) {
      if (notify) {
        await chrome.notifications.create({
          type: 'basic',
          iconUrl: 'icons/icon128.png',
          title: 'Feed Already Added',
          message: 'This feed is already in your list',
        });
      }
      return false;
    }

    const parsedUrl = new URL(sanitizedFeedUrl);
    const newFeed: LocalFeed = {
      id: generateId(),
      type: 'RSS',
      title: feedTitle || parsedUrl.hostname,
      url: sanitizedFeedUrl,
      npub: null,
      addedAt: new Date().toISOString(),
    };

    localFeeds.push(newFeed);
    await chrome.storage.sync.set({ localFeeds });

    if (notify) {
      await chrome.notifications.create({
        type: 'basic',
        iconUrl: 'icons/icon128.png',
        title: 'Feed Subscribed',
        message: `Subscribed to: ${newFeed.title}`,
      });
    }

    const storage = await getStorageData();
    const baseUrl = normalizeBaseUrl(storage.settings.webAppUrl);
    const hasAuth = storage.authToken || (storage.nostrAuth?.pubkey && storage.nostrAuth.method !== 'none');
    if (hasAuth && baseUrl) {
      try {
        const url = `${baseUrl}/api/trpc/feed.subscribeFeed`;
        const body = JSON.stringify({
          json: {
            type: 'RSS',
            url: sanitizedFeedUrl,
            title: newFeed.title,
          },
        });
        const headers: Record<string, string> = { 'Content-Type': 'application/json' };
        await applyAuthHeaders(headers, url, baseUrl, 'POST', storage.authToken, storage.nostrAuth, body);

        await fetch(url, {
          method: 'POST',
          headers,
          credentials: 'include',
          body,
        });
      } catch (err) {
        console.error('Failed to sync feed with account:', err);
      }
    }

    return true;
  } catch (err) {
    console.error('Failed to add feed:', err);
    if (notify) {
      await chrome.notifications.create({
        type: 'basic',
        iconUrl: 'icons/icon128.png',
        title: 'Error',
        message: 'Failed to subscribe to feed',
      });
    }
    return false;
  }
}

function setupContextMenu(): void {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({
      id: MENU_ID_PARENT,
      title: 'Readstr',
      contexts: ['page', 'link'],
    });

    chrome.contextMenus.create({
      id: MENU_ID_PAGE_FEEDS,
      parentId: MENU_ID_PARENT,
      title: 'Subscribe to detected feeds...',
      contexts: ['page'],
    });

    chrome.contextMenus.create({
      id: MENU_ID_SUBSCRIBE_LINK,
      parentId: MENU_ID_PARENT,
      title: 'Subscribe to this link as feed',
      contexts: ['link'],
    });
  });
}

async function purgePersistedPrivateKey(): Promise<void> {
  const result = await chrome.storage.local.get('nostrAuth');
  const stored = result['nostrAuth'] as (NostrAuthData & { privateKeyHex?: unknown }) | undefined;
  if (stored && 'privateKeyHex' in stored) {
    delete stored.privateKeyHex;
    await chrome.storage.local.set({ nostrAuth: stored });
  }
}

chrome.runtime.onInstalled.addListener((details) => {
  void (async () => {
    if (details.reason === 'install') {
      await saveStorageData({
        feeds: [],
        seenItemIds: [],
        settings: defaultSettings,
        authToken: null,
      });
    } else {
      await purgePersistedPrivateKey();
    }

    setupContextMenu();
    await setupAlarm();
    updateBadge(0);
  })();
});

chrome.runtime.onStartup.addListener(() => {
  void (async () => {
    await setupAlarm();
    await tryRestoreAuthFromOpenTabs();
    void refreshFeeds();
  })();
});

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  handleMessage(message as { type: string; [key: string]: unknown }, sender)
    .then(sendResponse)
    .catch((error: unknown) => {
      const errorMessage = error instanceof Error ? error.message : 'Unknown error';
      sendResponse({ success: false, error: errorMessage });
    });
  return true;
});

chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === ALARM_NAME) {
    void refreshFeeds();
  }
});

chrome.notifications.onClicked.addListener((notificationId) => {
  void (async () => {
    const data = notificationDataCache.get(notificationId);
    const storage = await getStorageData();

    if (data && data !== 'batch') {
      const baseUrl = normalizeBaseUrl(data.webAppUrl);
      const itemUrl = data.itemUrl ? sanitizeUrl(data.itemUrl) : null;
      const targetUrl = itemUrl ?? (baseUrl ? `${baseUrl}/item/${encodeURIComponent(data.itemId)}` : null);
      if (targetUrl) {
        await chrome.tabs.create({ url: targetUrl });
        void markItemAsRead(data.itemId);
      }
    } else {
      const appUrl = normalizeBaseUrl(storage.settings.webAppUrl);
      if (appUrl) {
        await chrome.tabs.create({ url: appUrl });
      }
    }

    notificationDataCache.delete(notificationId);
    await chrome.notifications.clear(notificationId);
  })();
});

chrome.notifications.onButtonClicked.addListener((notificationId, buttonIndex) => {
  void (async () => {
    const data = notificationDataCache.get(notificationId);
    const storage = await getStorageData();

    try {
      if (data === 'batch') {
        if (buttonIndex === 0) {
          const appUrl = normalizeBaseUrl(storage.settings.webAppUrl);
          if (appUrl) {
            await chrome.tabs.create({ url: appUrl });
          }
        } else if (buttonIndex === 1) {
          await markAllAsRead();
        }
      } else if (data) {
        if (buttonIndex === 0) {
          const baseUrl = normalizeBaseUrl(data.webAppUrl);
          const itemUrl = data.itemUrl ? sanitizeUrl(data.itemUrl) : null;
          const targetUrl = itemUrl ?? (baseUrl ? `${baseUrl}/item/${encodeURIComponent(data.itemId)}` : null);
          if (targetUrl) {
            await chrome.tabs.create({ url: targetUrl });
            void markItemAsRead(data.itemId);
          }
        } else if (buttonIndex === 1) {
          await markItemAsRead(data.itemId);
          void refreshFeeds();
        }
      }
    } catch (error) {
      if (error instanceof AuthUnavailableError) {
        console.warn('Notification action skipped, re-authentication required');
      } else {
        console.error('Notification action failed:', error instanceof Error ? error.message : error);
      }
    } finally {
      notificationDataCache.delete(notificationId);
      await chrome.notifications.clear(notificationId);
    }
  })();
});

chrome.notifications.onClosed.addListener((notificationId) => {
  notificationDataCache.delete(notificationId);
});

chrome.storage.onChanged.addListener((changes, areaName) => {
  if (areaName === 'local' && changes['authToken']) {
    const newToken = changes['authToken'].newValue as string | null;
    if (newToken) {
      void refreshFeeds();
    } else {
      updateBadge(0);
    }
  }
});

chrome.contextMenus.onClicked.addListener((info, tab) => {
  if (info.menuItemId === MENU_ID_PAGE_FEEDS && tab?.id) {
    void subscribeDetectedFeedsForTab(tab.id, tab);
  } else if (info.menuItemId === MENU_ID_SUBSCRIBE_LINK) {
    const url = info.linkUrl;
    const title = tab?.title ?? '';
    if (url && isLikelyFeedUrl(url)) {
      void addFeedToStorage(url, title);
    } else if (url) {
      void addFeedToStorage(url, title || new URL(url).hostname);
    }
  }
});

void (async () => {
  const alarm = await chrome.alarms.get(ALARM_NAME);
  if (!alarm) {
    await setupAlarm();
  }
  await tryRestoreAuthFromOpenTabs();
  const storage = await getStorageData();
  const totalUnread = storage.feeds.reduce((sum, feed) => sum + feed.unreadCount, 0);
  updateBadge(totalUnread);
  if (await isSyncOverdue()) {
    void refreshFeeds();
  }
})();
