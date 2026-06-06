import type { LocalFeed, ExtensionSettings } from './types';
import {
  NIP98_SIGN_REQUEST,
  NIP98_SIGN_RESPONSE,
  type Nip98SignResponse,
} from './utils/nip98Bridge';
import type { UnsignedEvent, NostrEvent } from './nostr';

const SIGN_TIMEOUT_MS = 20000;

interface DetectedFeed {
  url: string;
  title: string;
  type: 'rss' | 'atom';
}

const ALLOWED_PROTOCOLS = ['https:', 'http:'];

function isTrustedHost(hostname: string): boolean {
  return (
    hostname === 'readstr.privkey.io' ||
    hostname.endsWith('.readstr.privkey.io') ||
    hostname === 'localhost'
  );
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
  const sanitized = sanitizeUrl(urlString);
  return sanitized ? sanitized.replace(/\/+$/, '') : null;
}

const CONTAINER_ID = 'nostr-feedz-container';
const BUTTON_ID = 'nostr-feedz-fab';
const MENU_ID = 'nostr-feedz-menu';
const TOAST_ID = 'nostr-feedz-toast';

function generateId(): string {
  return `${Date.now()}-${Math.random().toString(36).slice(2, 9)}`;
}

function detectFeeds(): DetectedFeed[] {
  const feeds: DetectedFeed[] = [];
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

function createContainer(): HTMLDivElement {
  let container = document.getElementById(CONTAINER_ID) as HTMLDivElement | null;
  if (container) return container;

  container = document.createElement('div');
  container.id = CONTAINER_ID;
  document.body.appendChild(container);
  return container;
}

function showToast(message: string, type: 'success' | 'error' = 'success'): void {
  const existing = document.getElementById(TOAST_ID);
  if (existing) existing.remove();

  const container = createContainer();
  const toast = document.createElement('div');
  toast.id = TOAST_ID;
  toast.className = `nf-toast nf-toast-${type}`;
  toast.textContent = message;
  container.appendChild(toast);

  setTimeout(() => {
    toast.classList.add('nf-toast-hide');
    setTimeout(() => toast.remove(), 300);
  }, 3000);
}

function createFeedMenu(feeds: DetectedFeed[], onAdd: (feed: DetectedFeed) => void): HTMLDivElement {
  const menu = document.createElement('div');
  menu.id = MENU_ID;
  menu.className = 'nf-menu';

  const header = document.createElement('div');
  header.className = 'nf-menu-header';
  header.textContent = `${feeds.length} feed${feeds.length > 1 ? 's' : ''} detected`;
  menu.appendChild(header);

  const list = document.createElement('div');
  list.className = 'nf-menu-list';

  feeds.forEach((feed) => {
    const item = document.createElement('div');
    item.className = 'nf-menu-item';

    const icon = document.createElement('span');
    icon.className = 'nf-menu-icon';
    icon.textContent = feed.type === 'atom' ? '⚛️' : '📰';

    const info = document.createElement('div');
    info.className = 'nf-menu-info';

    const title = document.createElement('div');
    title.className = 'nf-menu-title';
    title.textContent = feed.title;

    const url = document.createElement('div');
    url.className = 'nf-menu-url';
    url.textContent = new URL(feed.url).pathname;

    info.appendChild(title);
    info.appendChild(url);

    const addBtn = document.createElement('button');
    addBtn.className = 'nf-menu-add';
    addBtn.textContent = 'Add';
    addBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      onAdd(feed);
      addBtn.textContent = '✓';
      addBtn.disabled = true;
      addBtn.classList.add('nf-menu-added');
    });

    item.appendChild(icon);
    item.appendChild(info);
    item.appendChild(addBtn);
    list.appendChild(item);
  });

  menu.appendChild(list);

  const addAllBtn = document.createElement('button');
  addAllBtn.className = 'nf-menu-addall';
  addAllBtn.textContent = 'Add All Feeds';
  addAllBtn.addEventListener('click', () => {
    feeds.forEach((feed) => onAdd(feed));
    addAllBtn.textContent = 'All Added ✓';
    addAllBtn.disabled = true;
    list.querySelectorAll<HTMLButtonElement>('.nf-menu-add').forEach((btn) => {
      btn.textContent = '✓';
      btn.disabled = true;
      btn.classList.add('nf-menu-added');
    });
  });
  menu.appendChild(addAllBtn);

  return menu;
}

function createFloatingButton(feeds: DetectedFeed[]): void {
  const existing = document.getElementById(BUTTON_ID);
  if (existing) existing.remove();

  const existingMenu = document.getElementById(MENU_ID);
  if (existingMenu) existingMenu.remove();

  if (feeds.length === 0) return;

  const container = createContainer();

  const fab = document.createElement('button');
  fab.id = BUTTON_ID;
  fab.className = 'nf-fab';
  fab.title = `${feeds.length} feed${feeds.length > 1 ? 's' : ''} found - Add to Readstr`;

  const icon = document.createElement('span');
  icon.className = 'nf-fab-icon';
  icon.textContent = '📡';

  const badge = document.createElement('span');
  badge.className = 'nf-fab-badge';
  badge.textContent = String(feeds.length);

  fab.appendChild(icon);
  fab.appendChild(badge);

  let menuVisible = false;
  let menu: HTMLDivElement | null = null;

  const closeMenu = () => {
    if (menu) {
      menu.classList.remove('nf-menu-visible');
      setTimeout(() => menu?.remove(), 200);
      menu = null;
      menuVisible = false;
    }
  };

  const handleClickOutside = (e: MouseEvent) => {
    if (menuVisible && menu && !menu.contains(e.target as Node) && e.target !== fab) {
      closeMenu();
    }
  };

  const toggleMenu = () => {
    if (menuVisible && menu) {
      closeMenu();
    } else {
      menu = createFeedMenu(feeds, (feed) => void addFeedToStorage(feed));
      container.appendChild(menu);
      requestAnimationFrame(() => {
        menu?.classList.add('nf-menu-visible');
      });
      menuVisible = true;
    }
  };

  fab.addEventListener('click', toggleMenu);
  document.addEventListener('click', handleClickOutside);

  container.appendChild(fab);
}

async function addFeedToStorage(feed: DetectedFeed): Promise<void> {
  try {
    const sanitizedFeedUrl = sanitizeUrl(feed.url);
    if (!sanitizedFeedUrl) {
      showToast('Invalid feed URL', 'error');
      return;
    }

    const result = await chrome.storage.sync.get(['localFeeds']);
    const localFeeds: LocalFeed[] = (result['localFeeds'] as LocalFeed[] | undefined) ?? [];

    const exists = localFeeds.some((f) => f.url === sanitizedFeedUrl);
    if (exists) {
      showToast('Feed already added', 'error');
      return;
    }

    const newFeed: LocalFeed = {
      id: generateId(),
      type: 'RSS',
      title: feed.title,
      url: sanitizedFeedUrl,
      npub: null,
      addedAt: new Date().toISOString(),
    };

    localFeeds.push(newFeed);
    await chrome.storage.sync.set({ localFeeds });

    showToast(`Added: ${feed.title}`, 'success');

    void syncWithAccount(newFeed);
  } catch (err) {
    console.error('Failed to add feed:', err);
    showToast('Failed to add feed', 'error');
  }
}

async function syncWithAccount(feed: LocalFeed): Promise<void> {
  try {
    const result = await chrome.storage.local.get(['authToken', 'nostrAuth', 'settings']);
    const authToken = result['authToken'] as string | undefined;
    const nostrAuth = result['nostrAuth'] as { pubkey?: string } | undefined;
    const settings = result['settings'] as ExtensionSettings | undefined;

    const hasAuth = authToken || nostrAuth?.pubkey;
    if (!hasAuth || !settings?.webAppUrl) return;

    const baseUrl = normalizeBaseUrl(settings.webAppUrl);
    if (!baseUrl || !feed.url) return;

    const url = `${baseUrl}/api/trpc/feed.subscribeFeed`;
    const headers: Record<string, string> = { 'Content-Type': 'application/json' };

    if (authToken) {
      headers['Authorization'] = `Bearer ${authToken}`;
    }

    await fetch(url, {
      method: 'POST',
      headers,
      credentials: 'include',
      body: JSON.stringify({
        json: {
          type: 'RSS',
          url: feed.url,
          title: feed.title,
        },
      }),
    });
  } catch (err) {
    console.error('Failed to sync with account:', err);
  }
}

// Check if we're on readstr.privkey.io and sync auth with extension
async function syncAuthFromWebApp(): Promise<void> {
  const hostname = window.location.hostname;
  if (!isTrustedHost(hostname)) {
    return;
  }

  try {
    if (!chrome.runtime?.id) return;

    const sessionStr = localStorage.getItem('nostr_session');
    if (!sessionStr) {
      return;
    }

    const session = JSON.parse(sessionStr) as {
      pubkey: string;
      npub: string;
      method: string;
      timestamp: number;
    };

    if (session.pubkey) {
      await chrome.runtime.sendMessage({ type: 'SYNC_WEB_AUTH', session });
    }
  } catch {
    // Extension context invalidated - ignore silently
  }
}

// Watch for auth changes in localStorage
function watchAuthChanges(): void {
  const hostname = window.location.hostname;
  if (!isTrustedHost(hostname)) {
    return;
  }

  window.addEventListener('storage', (event) => {
    if (event.key === 'nostr_session') {
      void syncAuthFromWebApp();
    }
  });

  // Also check periodically for same-tab changes
  setInterval(() => void syncAuthFromWebApp(), 5000);
}

function init(): void {
  if (document.getElementById(CONTAINER_ID)) return;

  // Sync auth from web app if on readstr.privkey.io
  void syncAuthFromWebApp();
  watchAuthChanges();

  const feeds = detectFeeds();

  if (feeds.length > 0) {
    createFloatingButton(feeds);
  }
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', init);
} else {
  init();
}

// Relay a NIP-98 signing request from the background service worker to the
// page-world signer (window.nostr) and back. The background has no window.nostr,
// so it delegates here. Origin-checked and timed out so a stalled signer cannot
// hang the caller.
function requestPageSignature(unsignedEvent: UnsignedEvent): Promise<NostrEvent> {
  return new Promise((resolve, reject) => {
    const id = generateId();
    const origin = window.location.origin;

    const handler = (event: MessageEvent): void => {
      if (event.source !== window || event.origin !== origin) return;
      const data = event.data as Partial<Nip98SignResponse> | null;
      if (!data || data.type !== NIP98_SIGN_RESPONSE || data.id !== id) return;
      cleanup();
      if (data.signed) {
        resolve(data.signed);
      } else {
        reject(new Error(data.error ?? 'Signing failed'));
      }
    };

    const timeout = setTimeout(() => {
      cleanup();
      reject(new Error('Signing timed out'));
    }, SIGN_TIMEOUT_MS);

    const cleanup = (): void => {
      clearTimeout(timeout);
      window.removeEventListener('message', handler);
    };

    window.addEventListener('message', handler);
    window.postMessage({ type: NIP98_SIGN_REQUEST, id, event: unsignedEvent }, origin);
  });
}

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message.type === 'GET_DETECTED_FEEDS') {
    const feeds = detectFeeds();
    sendResponse({ feeds });
  } else if (message.type === 'SIGN_NIP98') {
    if (!isTrustedHost(window.location.hostname)) {
      sendResponse({ error: 'Untrusted host' });
      return true;
    }
    requestPageSignature(message.event as UnsignedEvent)
      .then((signed) => sendResponse({ signed }))
      .catch((err: unknown) => {
        sendResponse({ error: err instanceof Error ? err.message : 'Signing failed' });
      });
    return true;
  } else if (message.type === 'GET_SESSION') {
    const hostname = window.location.hostname;
    if (!isTrustedHost(hostname)) {
      sendResponse({ session: null });
      return true;
    }
    try {
      const sessionStr = localStorage.getItem('nostr_session');
      if (sessionStr) {
        const session = JSON.parse(sessionStr) as {
          pubkey: string;
          npub: string;
          method: string;
        };
        sendResponse({ session });
      } else {
        sendResponse({ session: null });
      }
    } catch {
      sendResponse({ session: null });
    }
  }
  return true;
});
