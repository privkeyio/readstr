import {
  NIP98_SIGN_REQUEST,
  NIP98_SIGN_RESPONSE,
  type Nip98SignResponse,
} from './utils/nip98Bridge';
import type { UnsignedEvent, NostrEvent } from './nostr';

const SIGN_TIMEOUT_MS = 20000;

function isTrustedHost(hostname: string): boolean {
  return (
    hostname === 'readstr.privkey.io' ||
    hostname.endsWith('.readstr.privkey.io') ||
    hostname === 'localhost'
  );
}

function generateId(): string {
  return `${Date.now()}-${Math.random().toString(36).slice(2, 9)}`;
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
  // Sync auth from web app if on readstr.privkey.io
  void syncAuthFromWebApp();
  watchAuthChanges();
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
  if (message.type === 'SIGN_NIP98') {
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
