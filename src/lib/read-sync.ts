import type { Event, UnsignedEvent } from 'nostr-tools'
import {
  advanceSyncWatermarkIfFresh,
  fetchReadStatus,
  getReadSyncEnabled,
  isSyncEventFresh,
  MAX_GUID_LEN,
  MAX_READ_ITEMS,
  publishReadStatus,
} from './nostr-sync'

export const READ_STATUS_D_TAG = 'readstr-read-status'

// Read changes come in bursts (auto-mark), and each publish re-sends the whole
// list, so wait for a quiet spell.
export const READ_SYNC_PUBLISH_DELAY_MS = 30_000

// The /api/trpc route refuses bodies over 64,000 bytes. Applying a pulled list
// is split into calls whose guids take at most this many UTF-8 bytes, which
// leaves room for the tRPC envelope and small mutations batched alongside.
export const MARK_READ_REQUEST_BYTES = 48_000

// Common strfry relays (damus, nos.lol) refuse events over 64 KiB of JSON, so a
// bigger list would fail to publish without any error. The web publishes only
// the newest reads that fit this budget. Pulls still accept lists up to
// MAX_CONTENT_BYTES, since other clients may publish bigger ones.
export const MAX_PUBLISHED_READ_CONTENT = 60_000

// Room for the {"itemGuids":[],"lastUpdated":...} wrapper and the event's own
// fields (id, pubkey, sig, tags, created_at), so the whole event stays under
// the relays' limit.
const CONTENT_ENVELOPE_RESERVE = 1024

const utf8 = new TextEncoder()

// What one key adds to the published event: its JSON string and a comma in
// content, escaped a second time when content is embedded in the event JSON,
// in UTF-8 bytes. Relays limit the event's JSON in bytes, and this is never
// less than the key's UTF-16 length in content, so the content cap holds too.
function publishedKeyCost(key: string): number {
  return utf8.encode(JSON.stringify(JSON.stringify(key))).length - 1
}

/**
 * Collects read-status keys in the order given (newest read first), skipping
 * duplicates and keys over MAX_GUID_LEN UTF-16 units, until the list would
 * break readStatusListSchema or the published event would pass
 * MAX_PUBLISHED_READ_CONTENT. It stops at the first key that does not fit, so
 * the result is always a prefix of the newest reads.
 */
export class ReadSyncGuidCollector {
  readonly guids: string[] = []
  private readonly seen = new Set<string>()
  private size = CONTENT_ENVELOPE_RESERVE
  private done = false

  constructor(
    private readonly maxItems = MAX_READ_ITEMS,
    private readonly maxSize = MAX_PUBLISHED_READ_CONTENT
  ) {}

  get full(): boolean {
    return this.done || this.guids.length >= this.maxItems
  }

  add(key: string | null): void {
    if (this.full || !key || key.length > MAX_GUID_LEN || this.seen.has(key)) return
    const cost = publishedKeyCost(key)
    if (this.size + cost > this.maxSize) {
      this.done = true
      return
    }
    this.size += cost
    this.seen.add(key)
    this.guids.push(key)
  }
}

/**
 * The list to publish: this server's keys first (newest read first), then the
 * keys of the last list seen on the relays that this server lacks, so a key
 * another device published is not dropped just because no feed here has it.
 * Everything is under the publish cap.
 */
export function buildPublishedReadList(local: string[], remote: string[]): string[] {
  const collector = new ReadSyncGuidCollector()
  for (const key of local) collector.add(key)
  for (const key of remote) collector.add(key)
  return collector.guids
}

export function readSyncAllowed(canSign: boolean, npub: string | null | undefined): boolean {
  return canSign && !!npub && getReadSyncEnabled()
}

export function chunkGuidsForRequest(guids: string[], maxBytes = MARK_READ_REQUEST_BYTES): string[][] {
  const chunks: string[][] = []
  let current: string[] = []
  let bytes = 0
  for (const guid of guids) {
    const cost = utf8.encode(JSON.stringify(guid)).length + 1
    if (current.length > 0 && bytes + cost > maxBytes) {
      chunks.push(current)
      current = []
      bytes = 0
    }
    current.push(guid)
    bytes += cost
  }
  if (current.length > 0) chunks.push(current)
  return chunks
}

/** Pulled guids without empty or whitespace-only entries and duplicates, in order. */
export function cleanPulledGuids(guids: string[]): string[] {
  const seen = new Set<string>()
  const out: string[] = []
  for (const guid of guids) {
    if (!guid.trim() || seen.has(guid)) continue
    seen.add(guid)
    out.push(guid)
  }
  return out
}

/** Per-account session state for read-status sync. */
export interface ReadSyncSession {
  // The account this state belongs to. Work queued for another account stops.
  npub: string | null
  // True once a pull that every connected relay answered applied the newest
  // event, or found that it was already applied here, or that none exists.
  // Publishing waits for it: a publish replaces the event, so one made after a
  // failed or partial pull could drop reads another device published.
  pulled: boolean
  // The guids of the newest event known to be on the relays (pulled or
  // published here), or null when unknown. Publishes add to it rather than
  // drop its keys, and a list it already covers is not published again.
  remote: string[] | null
  // Bumped each time a publish or withdrawal records what the relays hold. A
  // pull compares it with the value it saw at the start, so a result fetched
  // before that publish does not overwrite the newer `remote`.
  remoteVersion: number
  // The greatest created_at this session has published or seen in a pulled
  // event, or null. The next publish goes strictly after it, so it replaces
  // that event even when made within the same second or under a clock that
  // runs behind another device's.
  latestSeenAt: number | null
  queue: Promise<void>
}

export function newReadSyncSession(npub: string | null = null): ReadSyncSession {
  return { npub, pulled: false, remote: null, remoteVersion: 0, latestSeenAt: null, queue: Promise.resolve() }
}

// Retry delays for a pull that failed or that not every relay answered.
export const READ_SYNC_RETRY_MIN_MS = 30_000
export const READ_SYNC_RETRY_MAX_MS = 10 * 60_000

/** The delay after `delay` when a retried pull fails again: doubled, capped. */
export function nextReadSyncRetryDelay(delay: number): number {
  return Math.min(delay * 2, READ_SYNC_RETRY_MAX_MS)
}

type SignEvent = (event: UnsignedEvent) => Promise<Event>

// Sync is allowed and the signed-in account is still the session's, so work
// queued for an earlier account stops.
function sessionAllowed(opts: {
  canSign: () => boolean
  npub: () => string | null | undefined
  session: ReadSyncSession
}): boolean {
  const npub = opts.npub()
  return readSyncAllowed(opts.canSign(), npub) && npub === opts.session.npub
}

/**
 * Pull the newest kind 30405 event and mark its guids read on the server.
 * Additive only. The pull stops between chunks once it is cancelled, the
 * setting is turned off or the account changes. The watermark advances only
 * once every chunk is applied, so a failed or stopped apply is retried on the
 * next pull. A pull that not every connected relay answered still applies
 * what it found, but leaves publishing locked. Returns whether publishing is
 * now safe (see ReadSyncSession.pulled), so false means "retry the pull".
 * Never throws.
 */
export async function pullReadStatus(opts: {
  canSign: () => boolean
  npub: () => string | null | undefined
  isCancelled?: () => boolean
  session: ReadSyncSession
  markRead: (guids: string[]) => Promise<unknown>
  onApplied: () => void
  fetchRemote?: typeof fetchReadStatus
}): Promise<boolean> {
  const { session } = opts
  const live = () => !opts.isCancelled?.() && sessionAllowed(opts)
  if (!live()) return false
  const remoteVersion = session.remoteVersion
  // A publish or withdrawal that finished during the pull already set what the
  // relays hold, after the pulled event, so the pulled list must not replace
  // it. Publishing is then safe too, since that publish set the relay state.
  const settle = (guids: string[], complete: boolean): boolean => {
    if (session.remoteVersion !== remoteVersion) {
      session.pulled = true
      return true
    }
    session.remote = guids
    session.pulled = complete
    return complete
  }
  try {
    const result = await (opts.fetchRemote ?? fetchReadStatus)(session.npub!)
    // Raised even when the pull then stops, so a withdrawal made right after
    // still goes after the event the relays hold.
    if (result.success && result.found) raiseLatestSeenAt(session, result.createdAt)
    if (!live()) return false
    if (!result.success) {
      console.warn('Read status sync fetch failed:', result.error)
      return false
    }
    if (!result.found) {
      session.pulled = true
      return true
    }
    const guids = cleanPulledGuids(result.data.itemGuids)
    if (!result.fresh || !isSyncEventFresh(READ_STATUS_D_TAG, result.createdAt)) {
      // Applied before, or published by this device: nothing to apply, but it
      // is what the relays hold, so publishes build on it.
      return settle(guids, result.complete)
    }
    for (const part of chunkGuidsForRequest(guids)) {
      if (!live()) return false
      await opts.markRead(part)
    }
    if (!live()) return false
    opts.onApplied()
    advanceSyncWatermarkIfFresh(READ_STATUS_D_TAG, result.createdAt)
    return settle(guids, result.complete)
  } catch (error) {
    console.error('Read status sync pull failed:', error)
    return false
  }
}

const turnedOff = () => new Error('Read status sync was turned off')

function raiseLatestSeenAt(session: ReadSyncSession, createdAt: number): void {
  if (session.latestSeenAt == null || createdAt > session.latestSeenAt) session.latestSeenAt = createdAt
}

// The lowest created_at the session's next event may have.
function nextCreatedAt(session: ReadSyncSession): number {
  return session.latestSeenAt == null ? 0 : session.latestSeenAt + 1
}

// The watermark is shared by every account on this device, so it only moves
// while the session's account is still the signed-in one; otherwise one
// account's publish could make the next account skip its own newer event.
function recordPublished(
  session: ReadSyncSession,
  guids: string[],
  createdAt: number | undefined,
  npub: string | null | undefined
): void {
  session.remote = guids
  session.remoteVersion++
  if (createdAt != null) raiseLatestSeenAt(session, createdAt)
  if (npub === session.npub) advanceSyncWatermarkIfFresh(READ_STATUS_D_TAG, createdAt)
}

/**
 * Publish the union of the server's read guids and the last list seen on the
 * relays, unless the relays already hold every guid in it. Waits for a
 * successful pull. Publishes are queued so two never race, and a failed one
 * does not block the next. Never throws.
 */
export function publishReadStatusIfChanged(opts: {
  canSign: () => boolean
  npub: () => string | null | undefined
  session: ReadSyncSession
  fetchLocal: () => Promise<string[]>
  signEvent: SignEvent
  publish?: typeof publishReadStatus
}): Promise<void> {
  const { session } = opts
  const allowed = () => sessionAllowed(opts)
  const run = async () => {
    try {
      // Re-checked when the queued run starts: the setting or signer may have
      // changed during the debounce.
      if (!allowed() || !session.pulled) return
      const local = await opts.fetchLocal()
      const guids = buildPublishedReadList(local, session.remote ?? [])
      const remote = new Set(session.remote ?? [])
      if (guids.every((guid) => remote.has(guid))) return
      // Checked again around signing, so turning the setting off while this
      // run waits on the server or the signer stops it before it publishes.
      const result = await (opts.publish ?? publishReadStatus)(
        guids,
        async (event) => {
          if (!allowed()) throw turnedOff()
          const signed = await opts.signEvent(event)
          if (!allowed()) throw turnedOff()
          return signed
        },
        nextCreatedAt(session)
      )
      if (result.success) recordPublished(session, guids, result.createdAt, opts.npub())
      else {
        console.warn('Read status auto-export failed:', result.error)
      }
    } catch (error) {
      console.error('Read status auto-export failed:', error)
    }
  }
  session.queue = session.queue.then(run, run)
  return session.queue
}

/**
 * After the setting is turned off, replace the published list with an empty
 * one, best effort. Relays and other clients may keep older copies. Queued
 * behind any publish in flight, and skipped if the setting was turned back on
 * or the account changed by the time it runs. Never throws.
 */
export function withdrawReadStatus(opts: {
  canSign: () => boolean
  npub: () => string | null | undefined
  session: ReadSyncSession
  signEvent: SignEvent
  publish?: typeof publishReadStatus
}): Promise<void> {
  const { session } = opts
  const run = async () => {
    try {
      const npub = opts.npub()
      if (getReadSyncEnabled() || !opts.canSign() || !npub || npub !== session.npub) return
      const result = await (opts.publish ?? publishReadStatus)([], opts.signEvent, nextCreatedAt(session))
      // npub is read again: the account may have changed while signing.
      if (result.success) recordPublished(session, [], result.createdAt, opts.npub())
      else {
        console.warn('Read status withdraw failed:', result.error)
      }
    } catch (error) {
      console.error('Read status withdraw failed:', error)
    }
  }
  session.queue = session.queue.then(run, run)
  return session.queue
}
