import {
  NIP98_SIGN_REQUEST,
  NIP98_SIGN_RESPONSE,
  type Nip98SignRequest,
} from './utils/nip98Bridge';
import type { NostrEvent } from './nostr';

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
