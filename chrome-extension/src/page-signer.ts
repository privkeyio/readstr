import {
  NIP98_SIGN_REQUEST,
  NIP98_SIGN_RESPONSE,
  type Nip98SignRequest,
} from './utils/nip98Bridge';
import type { NostrEvent, UnsignedEvent } from './nostr';

const NIP98_KIND = 27235;
const HTTP_METHODS = new Set(['GET', 'HEAD', 'POST', 'PUT', 'PATCH', 'DELETE']);

function findTag(tags: string[][], name: string): string | undefined {
  const tag = tags.find((t) => Array.isArray(t) && t[0] === name);
  return tag?.[1];
}

// Only ever sign genuine NIP-98 (kind 27235) HTTP auth events. The bridge is
// reachable by any script on the matched origins, so anything that is not a
// well-formed NIP-98 request (a kind-1 note, a deletion, a DM, ...) must be
// rejected before it reaches the signer.
function isValidNip98Event(event: UnsignedEvent): boolean {
  if (!event || typeof event !== 'object') return false;
  if (event.kind !== NIP98_KIND) return false;
  if (event.content !== '') return false;
  if (!Array.isArray(event.tags)) return false;

  const method = findTag(event.tags, 'method');
  if (!method || !HTTP_METHODS.has(method.toUpperCase())) return false;

  const url = findTag(event.tags, 'u');
  if (!url) return false;
  try {
    const parsed = new URL(url);
    if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
      return false;
    }
  } catch {
    return false;
  }

  return true;
}

// Runs in the page's MAIN world (see manifest) so it can reach window.nostr,
// which the isolated content script cannot. It only acts on same-window
// messages and only ever returns a freshly signed event; nothing is persisted.
window.addEventListener('message', (event: MessageEvent) => {
  if (event.source !== window) return;
  if (event.origin !== window.location.origin) return;

  const data = event.data as Partial<Nip98SignRequest> | null;
  if (
    !data ||
    data.type !== NIP98_SIGN_REQUEST ||
    typeof data.id !== 'string' ||
    !data.event
  ) {
    return;
  }

  const id = data.id;
  const unsigned = data.event;
  const origin = window.location.origin;
  const respond = (payload: { signed?: NostrEvent; error?: string }): void => {
    window.postMessage({ type: NIP98_SIGN_RESPONSE, id, ...payload }, origin);
  };

  if (!isValidNip98Event(unsigned)) {
    respond({ error: 'NOT_NIP98_EVENT' });
    return;
  }

  const signer = window.nostr;
  if (!signer || typeof signer.signEvent !== 'function') {
    respond({ error: 'NO_SIGNER' });
    return;
  }

  Promise.resolve()
    .then(() => signer.signEvent(unsigned))
    .then((signed) => respond({ signed }))
    .catch((err: unknown) => {
      respond({ error: err instanceof Error ? err.message : 'SIGN_REJECTED' });
    });
});
