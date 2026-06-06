import type { NostrEvent, UnsignedEvent } from '../nostr';

export const NIP98_SIGN_REQUEST = 'READSTR_NIP98_SIGN_REQUEST';
export const NIP98_SIGN_RESPONSE = 'READSTR_NIP98_SIGN_RESPONSE';

export interface Nip98SignRequest {
  type: typeof NIP98_SIGN_REQUEST;
  id: string;
  event: UnsignedEvent;
}

export interface Nip98SignResponse {
  type: typeof NIP98_SIGN_RESPONSE;
  id: string;
  signed?: NostrEvent;
  error?: string;
}
