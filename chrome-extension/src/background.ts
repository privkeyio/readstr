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
} from './nostr';
import { withRetry } from './utils/retry';
import { feedDatabase } from './db/feedDatabase';

const ALARM_NAME = 'refresh-feeds';
const DEFAULT_POLL_INTERVAL = 5;
const MAX_SEEN_ITEMS = 1000;
const DEFAULT_WEB_APP_URL = 'https://nostrfeedz.com';

const ALLOWED_PROTOCOLS = ['https:', 'http:'];

function isValidHttpUrl(urlString: string): boolean {
  try {
    const url = new URL(urlString);
    return ALLOWED_PROTOCOLS.includes(url.protocol);
  } catch {
    return false;
  }
}

function sanitizeUrl(urlString: string): string | null {
  if (!isValidHttpUrl(urlString)) return null;
  try {
    const url = new URL(urlString);
    if (!ALLOWED_PROTOCOLS.includes(url.protocol)) return null;
    return url.href;
  } catch {
    return null;
  }
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

async function getNostrAuthHeader(
  url: string,
  method: string,
  nostrAuth: NostrAuthData | null
): Promise<string | null> {
  if (!nostrAuth || !nostrAuth.pubkey) return null;

  if (nostrAuth.method === 'nsec' && nostrAuth.privateKeyHex) {
    return generateAuthHeader(url, method, nostrAuth.pubkey, nostrAuth.privateKeyHex);
  }

  // TODO: nip07 signing must happen in a content/page context. There is no
  // window.nostr in the background service worker, so we cannot produce a
  // NIP-98 event here for the nip07 method. Requests for nip07 users will be
  // unauthenticated until signing is delegated to a page/content script.
  return null;
}

async function fetchWithAuth<T>(
  url: string,
  authToken: string | null,
  options: RequestInit = {},
  nostrAuth: NostrAuthData | null = null
): Promise<T> {
  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
    ...(options.headers as Record<string, string> | undefined),
  };

  if (authToken) {
    headers['Authorization'] = `Bearer ${authToken}`;
  } else if (nostrAuth && nostrAuth.pubkey) {
    // Authenticate via signed NIP-98 event; the server derives identity from
    // the verified signature, not from any plaintext pubkey header.
    const nostrHeader = await getNostrAuthHeader(url, options.method ?? 'GET', nostrAuth);
    if (nostrHeader) {
      headers['Authorization'] = nostrHeader;
    }
  }

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
  const baseUrl = sanitizeUrl(settings.webAppUrl);
  if (!baseUrl) {
    throw new Error('Invalid web app URL');
  }
  const input = JSON.stringify({ json: { forceSync } });
  const url = `${baseUrl}/api/trpc/feed.getFeeds?input=${encodeURIComponent(input)}`;

  return withRetry(async () => {
    const response = await fetchWithAuth<{ result: { data: { json: FeedsResponse[] } } }>(
      url,
      authToken,
      {},
      nostrAuth
    );
    return response.result.data.json;
  });
}

async function fetchNewItems(
  settings: ExtensionSettings,
  authToken: string | null,
  nostrAuth: NostrAuthData | null = null,
  limit = 20
): Promise<FeedItem[]> {
  const baseUrl = sanitizeUrl(settings.webAppUrl);
  if (!baseUrl) {
    throw new Error('Invalid web app URL');
  }
  const input = JSON.stringify({ json: { limit } });
  const url = `${baseUrl}/api/trpc/feed.getFeedItems?input=${encodeURIComponent(input)}`;

  return withRetry(async () => {
    const response = await fetchWithAuth<{ result: { data: { json: FeedItemsResponse } } }>(
      url,
      authToken,
      {},
      nostrAuth
    );
    return response.result.data.json.items;
  });
}

async function markItemAsRead(itemId: string): Promise<void> {
  const storage = await getStorageData();
  const { settings, authToken, nostrAuth } = storage;

  const hasAuth = authToken || (nostrAuth?.pubkey && nostrAuth.method !== 'none');
  if (!hasAuth) return;

  const baseUrl = sanitizeUrl(settings.webAppUrl);
  if (!baseUrl) return;
  const url = `${baseUrl}/api/trpc/feed.markAsRead`;

  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
  };

  if (authToken) {
    headers['Authorization'] = `Bearer ${authToken}`;
  } else if (nostrAuth && nostrAuth.pubkey) {
    const nostrHeader = await getNostrAuthHeader(url, 'POST', nostrAuth);
    if (nostrHeader) {
      headers['Authorization'] = nostrHeader;
    }
  }

  await fetch(url, {
    method: 'POST',
    headers,
    credentials: 'include',
    body: JSON.stringify({ json: { itemId } }),
  });
}

async function markAllAsRead(): Promise<void> {
  const storage = await getStorageData();
  const { settings, authToken, nostrAuth } = storage;

  const hasAuth = authToken || (nostrAuth?.pubkey && nostrAuth.method !== 'none');
  if (!hasAuth) return;

  const baseUrl = sanitizeUrl(settings.webAppUrl);
  if (!baseUrl) return;
  const url = `${baseUrl}/api/trpc/feed.markAllAsRead`;

  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
  };

  if (authToken) {
    headers['Authorization'] = `Bearer ${authToken}`;
  } else if (nostrAuth && nostrAuth.pubkey) {
    const nostrHeader = await getNostrAuthHeader(url, 'POST', nostrAuth);
    if (nostrHeader) {
      headers['Authorization'] = nostrHeader;
    }
  }

  await fetch(url, {
    method: 'POST',
    headers,
    credentials: 'include',
    body: JSON.stringify({ json: {} }),
  });

  updateBadge(0);
  await refreshFeeds();
}

async function addFavorite(itemId: string): Promise<void> {
  const storage = await getStorageData();
  const { settings, authToken, nostrAuth } = storage;

  const hasAuth = authToken || (nostrAuth?.pubkey && nostrAuth.method !== 'none');
  if (!hasAuth) return;

  const baseUrl = sanitizeUrl(settings.webAppUrl);
  if (!baseUrl) return;
  const url = `${baseUrl}/api/trpc/feed.addFavorite`;

  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
  };

  if (authToken) {
    headers['Authorization'] = `Bearer ${authToken}`;
  } else if (nostrAuth && nostrAuth.pubkey) {
    const nostrHeader = await getNostrAuthHeader(url, 'POST', nostrAuth);
    if (nostrHeader) {
      headers['Authorization'] = nostrHeader;
    }
  }

  await fetch(url, {
    method: 'POST',
    headers,
    credentials: 'include',
    body: JSON.stringify({ json: { itemId } }),
  });
}

async function removeFavorite(itemId: string): Promise<void> {
  const storage = await getStorageData();
  const { settings, authToken, nostrAuth } = storage;

  const hasAuth = authToken || (nostrAuth?.pubkey && nostrAuth.method !== 'none');
  if (!hasAuth) return;

  const baseUrl = sanitizeUrl(settings.webAppUrl);
  if (!baseUrl) return;
  const url = `${baseUrl}/api/trpc/feed.removeFavorite`;

  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
  };

  if (authToken) {
    headers['Authorization'] = `Bearer ${authToken}`;
  } else if (nostrAuth && nostrAuth.pubkey) {
    const nostrHeader = await getNostrAuthHeader(url, 'POST', nostrAuth);
    if (nostrHeader) {
      headers['Authorization'] = nostrHeader;
    }
  }

  await fetch(url, {
    method: 'POST',
    headers,
    credentials: 'include',
    body: JSON.stringify({ json: { itemId } }),
  });
}

async function fetchFavorites(
  settings: ExtensionSettings,
  authToken: string | null,
  nostrAuth: NostrAuthData | null = null
): Promise<FeedItem[]> {
  const baseUrl = sanitizeUrl(settings.webAppUrl);
  if (!baseUrl) {
    throw new Error('Invalid web app URL');
  }

  const input = JSON.stringify({ json: { limit: 50 } });
  const url = `${baseUrl}/api/trpc/feed.getFavorites?input=${encodeURIComponent(input)}`;

  return withRetry(async () => {
    const response = await fetchWithAuth<{ result: { data: { json: { items: FeedItem[] } } } }>(
      url,
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
    const tabs = await chrome.tabs.query({
      url: ['*://*.nostrfeedz.com/*', '*://nostrfeedz.com/*', '*://localhost:*'],
    });

    for (const tab of tabs) {
      if (!tab.id) continue;
      try {
        const response = await chrome.tabs.sendMessage(tab.id, { type: 'GET_SESSION' }) as {
          session?: { pubkey: string; npub: string; method: string } | null;
        };
        if (response?.session?.pubkey) {
          const nostrAuth: NostrAuthData = {
            method: response.session.method === 'nip07' ? 'nip07' : 'nsec',
            pubkey: response.session.pubkey,
            npub: response.session.npub,
            privateKeyHex: null,
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
    console.error('Feed refresh failed, using cache:', errorMessage);

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
        let privateKeyHex: string | null = null;

        if (method === 'nsec' && nsec) {
          const decodedKey = decodeNsec(nsec);
          if (!decodedKey) {
            return { success: false, error: 'Invalid nsec key' };
          }
          pubkey = getPublicKeyFromPrivate(decodedKey);
          npub = encodeNpub(pubkey);
          privateKeyHex = decodedKey;
        } else if (method === 'nip07' && pubkeyHex) {
          pubkey = pubkeyHex;
          npub = encodeNpub(pubkeyHex);
        } else {
          return { success: false, error: 'Invalid login parameters' };
        }

        const nostrAuth: NostrAuthData = {
          method,
          pubkey,
          npub,
          privateKeyHex,
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

      const nostrAuth: NostrAuthData = {
        method: session.method === 'nip07' ? 'nip07' : 'nsec',
        pubkey: session.pubkey,
        npub: session.npub,
        privateKeyHex: null,
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
          markItemAsRead(itemId),
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
      const storage = await getStorageData();
      const updatedSettings = { ...storage.settings, ...newSettings };
      await saveStorageData({ settings: updatedSettings });

      if (newSettings.pollIntervalMinutes) {
        await setupAlarm();
      }
      return { success: true };
    }

    case 'DETECTED_FEEDS': {
      const feeds = message['feeds'] as DetectedFeed[];
      const tabId = sender.tab?.id;
      if (tabId && feeds) {
        updateContextMenuForTab(tabId, feeds);
      }
      return { success: true };
    }

    default:
      return { success: false, error: `Unknown message type: ${message.type}` };
  }
}

const MENU_ID_PAGE_FEEDS = 'nostr-feedz-page-feeds';
const MENU_ID_SUBSCRIBE_LINK = 'nostr-feedz-subscribe-link';
const MENU_ID_PARENT = 'nostr-feedz-parent';

interface DetectedFeed {
  url: string;
  title: string;
  type: string;
}

const tabFeeds = new Map<number, DetectedFeed[]>();

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

async function addFeedToStorage(feedUrl: string, feedTitle: string): Promise<boolean> {
  try {
    const sanitizedFeedUrl = sanitizeUrl(feedUrl);
    if (!sanitizedFeedUrl) {
      await chrome.notifications.create({
        type: 'basic',
        iconUrl: 'icons/icon128.png',
        title: 'Invalid URL',
        message: 'The feed URL is not valid',
      });
      return false;
    }

    const result = await chrome.storage.sync.get(['localFeeds']);
    const localFeeds: LocalFeed[] = (result['localFeeds'] as LocalFeed[] | undefined) ?? [];

    const exists = localFeeds.some((f) => f.url === sanitizedFeedUrl);
    if (exists) {
      await chrome.notifications.create({
        type: 'basic',
        iconUrl: 'icons/icon128.png',
        title: 'Feed Already Added',
        message: 'This feed is already in your list',
      });
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

    await chrome.notifications.create({
      type: 'basic',
      iconUrl: 'icons/icon128.png',
      title: 'Feed Subscribed',
      message: `Subscribed to: ${newFeed.title}`,
    });

    const storage = await getStorageData();
    const baseUrl = sanitizeUrl(storage.settings.webAppUrl);
    const hasAuth = storage.authToken || (storage.nostrAuth?.pubkey && storage.nostrAuth.method !== 'none');
    if (hasAuth && baseUrl) {
      try {
        const url = `${baseUrl}/api/trpc/feed.subscribeFeed`;
        const headers: Record<string, string> = { 'Content-Type': 'application/json' };

        if (storage.authToken) {
          headers['Authorization'] = `Bearer ${storage.authToken}`;
        } else if (storage.nostrAuth?.pubkey) {
          const nostrHeader = await getNostrAuthHeader(url, 'POST', storage.nostrAuth);
          if (nostrHeader) {
            headers['Authorization'] = nostrHeader;
          }
        }

        await fetch(url, {
          method: 'POST',
          headers,
          credentials: 'include',
          body: JSON.stringify({
            json: {
              type: 'RSS',
              url: sanitizedFeedUrl,
              title: newFeed.title,
            },
          }),
        });
      } catch (err) {
        console.error('Failed to sync feed with account:', err);
      }
    }

    return true;
  } catch (err) {
    console.error('Failed to add feed:', err);
    await chrome.notifications.create({
      type: 'basic',
      iconUrl: 'icons/icon128.png',
      title: 'Error',
      message: 'Failed to subscribe to feed',
    });
    return false;
  }
}

function setupContextMenu(): void {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({
      id: MENU_ID_PARENT,
      title: 'Nostr Feedz',
      contexts: ['page', 'link'],
    });

    chrome.contextMenus.create({
      id: MENU_ID_PAGE_FEEDS,
      parentId: MENU_ID_PARENT,
      title: 'Subscribe to detected feeds...',
      contexts: ['page'],
      enabled: false,
    });

    chrome.contextMenus.create({
      id: MENU_ID_SUBSCRIBE_LINK,
      parentId: MENU_ID_PARENT,
      title: 'Subscribe to this link as feed',
      contexts: ['link'],
    });
  });
}

function updateContextMenuForTab(tabId: number, feeds: DetectedFeed[]): void {
  tabFeeds.set(tabId, feeds);

  const hasFeeds = feeds.length > 0;
  const title = hasFeeds
    ? `Subscribe to this feed (${feeds.length} found)`
    : 'No feeds detected on this page';

  chrome.contextMenus.update(MENU_ID_PAGE_FEEDS, {
    title,
    enabled: hasFeeds,
  });
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
      const baseUrl = sanitizeUrl(data.webAppUrl);
      const itemUrl = data.itemUrl ? sanitizeUrl(data.itemUrl) : null;
      const targetUrl = itemUrl ?? (baseUrl ? `${baseUrl}/item/${encodeURIComponent(data.itemId)}` : null);
      if (targetUrl) {
        await chrome.tabs.create({ url: targetUrl });
        void markItemAsRead(data.itemId);
      }
    } else {
      const appUrl = sanitizeUrl(storage.settings.webAppUrl);
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

    if (data === 'batch') {
      if (buttonIndex === 0) {
        const appUrl = sanitizeUrl(storage.settings.webAppUrl);
        if (appUrl) {
          await chrome.tabs.create({ url: appUrl });
        }
      } else if (buttonIndex === 1) {
        await markAllAsRead();
      }
    } else if (data) {
      if (buttonIndex === 0) {
        const baseUrl = sanitizeUrl(data.webAppUrl);
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

    notificationDataCache.delete(notificationId);
    await chrome.notifications.clear(notificationId);
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

chrome.tabs.onActivated.addListener((activeInfo) => {
  const feeds = tabFeeds.get(activeInfo.tabId);
  if (feeds) {
    updateContextMenuForTab(activeInfo.tabId, feeds);
  } else {
    // Query the content script for feeds on this tab
    void chrome.tabs.sendMessage(activeInfo.tabId, { type: 'GET_DETECTED_FEEDS' })
      .then((response: { feeds?: DetectedFeed[] } | undefined) => {
        if (response?.feeds) {
          updateContextMenuForTab(activeInfo.tabId, response.feeds);
        }
      })
      .catch(() => {
        // Tab might not have content script loaded, reset menu
        updateContextMenuForTab(activeInfo.tabId, []);
      });
  }
});

chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
  if (changeInfo.status === 'complete' && tab.url) {
    // Query the content script for feeds when page loads
    void chrome.tabs.sendMessage(tabId, { type: 'GET_DETECTED_FEEDS' })
      .then((response: { feeds?: DetectedFeed[] } | undefined) => {
        if (response?.feeds) {
          updateContextMenuForTab(tabId, response.feeds);
        }
      })
      .catch(() => {
        // Content script not loaded yet
      });
  }
});

chrome.tabs.onRemoved.addListener((tabId) => {
  tabFeeds.delete(tabId);
});

chrome.contextMenus.onClicked.addListener((info, tab) => {
  if (info.menuItemId === MENU_ID_PAGE_FEEDS && tab?.id) {
    const feeds = tabFeeds.get(tab.id);
    if (feeds && feeds.length > 0) {
      const feed = feeds[0];
      if (feed) {
        void addFeedToStorage(feed.url, feed.title);
      }
    }
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
