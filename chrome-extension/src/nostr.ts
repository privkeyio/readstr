import { sha256 } from '@noble/hashes/sha256';
import { bytesToHex, hexToBytes } from '@noble/hashes/utils';
import * as secp256k1 from '@noble/secp256k1';
import { bech32 } from '@scure/base';

export interface NostrEvent {
  id: string;
  pubkey: string;
  created_at: number;
  kind: number;
  tags: string[][];
  content: string;
  sig: string;
}

export interface UnsignedEvent {
  pubkey: string;
  created_at: number;
  kind: number;
  tags: string[][];
  content: string;
}

export interface Nip07Signer {
  getPublicKey(): Promise<string>;
  signEvent(event: UnsignedEvent): Promise<NostrEvent>;
}

declare global {
  interface Window {
    nostr?: Nip07Signer;
  }
}

export function decodeNsec(nsec: string): string | null {
  try {
    if (!nsec.startsWith('nsec1')) return null;
    const decoded = bech32.decode(nsec as `nsec1${string}`, 1500);
    const bytes = bech32.fromWords(decoded.words);
    return bytesToHex(new Uint8Array(bytes));
  } catch {
    return null;
  }
}

export function decodeNpub(npub: string): string | null {
  try {
    if (!npub.startsWith('npub1')) return null;
    const decoded = bech32.decode(npub as `npub1${string}`, 1500);
    const bytes = bech32.fromWords(decoded.words);
    return bytesToHex(new Uint8Array(bytes));
  } catch {
    return null;
  }
}

export function encodeNpub(pubkeyHex: string): string {
  const bytes = hexToBytes(pubkeyHex);
  const words = bech32.toWords(bytes);
  return bech32.encode('npub', words, 1500);
}

export function getPublicKeyFromPrivate(privateKeyHex: string): string {
  const pubkeyBytes = secp256k1.getPublicKey(privateKeyHex, true);
  return bytesToHex(pubkeyBytes.slice(1));
}

export function serializeEvent(event: UnsignedEvent): string {
  return JSON.stringify([
    0,
    event.pubkey,
    event.created_at,
    event.kind,
    event.tags,
    event.content,
  ]);
}

export function getEventHash(event: UnsignedEvent): string {
  const serialized = serializeEvent(event);
  const hash = sha256(new TextEncoder().encode(serialized));
  return bytesToHex(hash);
}

export async function signEvent(
  event: UnsignedEvent,
  privateKeyHex: string
): Promise<NostrEvent> {
  const id = getEventHash(event);
  const sigBytes = await secp256k1.signAsync(id, privateKeyHex);
  const sig = sigBytes.toCompactHex();

  return {
    ...event,
    id,
    sig,
  };
}

export async function createNip98AuthEvent(
  url: string,
  method: string,
  pubkey: string,
  privateKeyHex?: string,
  body?: string | null
): Promise<NostrEvent> {
  const tags: string[][] = [
    ['u', url],
    ['method', method.toUpperCase()],
  ];

  // Bind the request body into the signature via the NIP-98 `payload` tag so a
  // captured header cannot be replayed against a different body.
  if (body) {
    const digest = bytesToHex(sha256(new TextEncoder().encode(body)));
    tags.push(['payload', digest]);
  }

  const unsignedEvent: UnsignedEvent = {
    pubkey,
    created_at: Math.floor(Date.now() / 1000),
    kind: 27235,
    tags,
    content: '',
  };

  if (privateKeyHex) {
    return signEvent(unsignedEvent, privateKeyHex);
  }

  if (typeof window !== 'undefined' && window.nostr) {
    return window.nostr.signEvent(unsignedEvent);
  }

  throw new Error('No signing method available');
}

export function encodeNip98AuthHeader(event: NostrEvent): string {
  const eventJson = JSON.stringify(event);
  const base64 = btoa(eventJson);
  return `Nostr ${base64}`;
}

export async function generateAuthHeader(
  url: string,
  method: string,
  pubkey: string,
  privateKeyHex?: string,
  body?: string | null
): Promise<string> {
  const event = await createNip98AuthEvent(url, method, pubkey, privateKeyHex, body);
  return encodeNip98AuthHeader(event);
}

export function isValidNsec(nsec: string): boolean {
  return decodeNsec(nsec) !== null;
}

export function isValidNpub(npub: string): boolean {
  return decodeNpub(npub) !== null;
}

export function isValidHexKey(hex: string): boolean {
  return /^[0-9a-f]{64}$/i.test(hex);
}
