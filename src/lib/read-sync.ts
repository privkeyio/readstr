import type { Event, UnsignedEvent } from 'nostr-tools'
import {
  advanceSyncWatermarkIfFresh,
  fetchReadStatus,
  getLastAppliedSyncCreatedAt,
  getReadSyncEnabled,
  isAcceptableListCreatedAt,
  isSyncEventFresh,
  MAX_GUID_LEN,
  MAX_READ_ITEMS,
  publishReadStatus,
  readStatusWatermarkTag,
} from './nostr-sync'

export const READ_STATUS_D_TAG = 'readstr-read-status'

// Read changes come in bursts (auto-mark), and each publish re-sends the whole
// list, so wait for a quiet spell.
export const READ_SYNC_PUBLISH_DELAY_MS = 30_000

// The /api/trpc route refuses bodies over 64,000 bytes. Applying a pulled list
// is split into calls whose guids take at most this many UTF-8 bytes, which
// leaves room for the tRPC envelope and small mutations batched alongside.
export const MARK_READ_REQUEST_BYTES = 48_000

// Most feed ids one getReadSyncUncoveredFeeds answer, scoped markReadByGuids
// call or markReadListApplied call carries. At the schema's 64 characters per
// id that is about 33 KB, so a scoped call still fits MARK_READ_REQUEST_BYTES
// once its guids are chunked to the bytes left (see applyReadListChunks).
export const READ_SYNC_FEED_IDS_MAX = 500

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
  // Whether coverNewFeedsWithReadList is running, and whether it was asked to
  // run again meanwhile, so two runs never overlap.
  covering: boolean
  coverAgain: boolean
  // How long to wait before retrying the next failed cover run. Doubled on
  // each failure and reset by a run that finishes, so a 429 is not hammered.
  coverRetryDelay: number
  // The greatest created_at this session has sent to the server as applied, or
  // null, so a pull that finds an event this browser already applied tells the
  // server at most once per session and event.
  serverRecordedAt: number | null
}

export function newReadSyncSession(npub: string | null = null): ReadSyncSession {
  return {
    npub,
    pulled: false,
    remote: null,
    remoteVersion: 0,
    latestSeenAt: null,
    queue: Promise.resolve(),
    covering: false,
    coverAgain: false,
    coverRetryDelay: READ_SYNC_RETRY_MIN_MS,
    serverRecordedAt: null,
  }
}

// Retry delays for a pull that failed or that not every relay answered.
export const READ_SYNC_RETRY_MIN_MS = 30_000
export const READ_SYNC_RETRY_MAX_MS = 10 * 60_000

/** The delay after `delay` when a retried pull fails again: doubled, capped. */
export function nextReadSyncRetryDelay(delay: number): number {
  return Math.min(delay * 2, READ_SYNC_RETRY_MAX_MS)
}

type SignEvent = (event: UnsignedEvent) => Promise<Event>

/** What getReadSyncUncoveredFeeds answers. */
export interface ReadSyncUncoveredFeeds {
  feedIds: string[]
  more: boolean
}

/**
 * The server calls the pull and the cover run share. markRead without feedIds
 * applies guids to every active subscription, with them only to those feeds.
 */
export interface ReadListServer {
  markRead: (guids: string[], feedIds?: string[]) => Promise<unknown>
  getUncovered: () => Promise<ReadSyncUncoveredFeeds>
  markApplied: (feedIds: string[]) => Promise<unknown>
  onApplied: () => void
}

/**
 * The created_at of the newest kind 30405 event applied or published by any of
 * the account's browsers, kept on the server (null when none is recorded).
 * Read state lives on the server, shared by those browsers, while the
 * localStorage watermark is per browser, so a new browser would otherwise
 * apply an event the others applied long ago and re-read items the user has
 * marked unread since.
 */
export interface ReadListAppliedServer {
  getAppliedCreatedAt: () => Promise<number | null>
  advanceAppliedCreatedAt: (createdAt: number) => Promise<unknown>
}

function raiseServerRecordedAt(session: ReadSyncSession, createdAt: number): void {
  if (session.serverRecordedAt == null || createdAt > session.serverRecordedAt) session.serverRecordedAt = createdAt
}

// Record on the server that an event with this created_at is applied, best
// effort: if it fails, this browser's own watermark still keeps it from
// applying the event again, and another browser applies it once more, which
// only marks read again. Not for another account, since the call is made as
// whoever is signed in now, and not for a created_at the server refuses.
async function recordAppliedOnServer(
  opts: ReadListAppliedServer & { npub: () => string | null | undefined },
  session: ReadSyncSession,
  createdAt: number | undefined
): Promise<void> {
  try {
    if (!isAcceptableListCreatedAt(createdAt) || opts.npub() !== session.npub) return
    await opts.advanceAppliedCreatedAt(createdAt)
    // Noted only once the server has it, so a failed record is tried again on
    // the next pull rather than skipped for the rest of the session.
    raiseServerRecordedAt(session, createdAt)
  } catch (error) {
    console.warn('Read status sync could not record the applied list on the server:', error)
  }
}

/**
 * Send `guids` in request-sized chunks, re-checking `live` before each one.
 * `reservedBytes` is what the rest of each call's input (a feedIds scope)
 * takes from the request budget. Returns false when it stopped early, true
 * when every chunk was sent and the run is still live.
 */
async function applyReadListChunks(
  guids: string[],
  send: (part: string[]) => Promise<unknown>,
  live: () => boolean,
  reservedBytes = 0
): Promise<boolean> {
  for (const part of chunkGuidsForRequest(guids, MARK_READ_REQUEST_BYTES - reservedBytes)) {
    if (!live()) return false
    await send(part)
  }
  return live()
}

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
 * next pull. A fresh event is applied to every feed, and the feeds it had
 * never been applied to when the pull started are then marked applied, so the
 * cover run does not send it to them again. A pull that not every connected
 * relay answered still applies what it found, but leaves publishing locked.
 * An event is fresh only when it is newer than both this browser's watermark
 * for the account and the one the server keeps for it (see
 * ReadListAppliedServer); the local one also covers an apply whose server
 * record failed, and is sent to the server when it holds less. The unkeyed
 * watermark older builds kept is not consulted: a browser that recorded no
 * owner before the owner key existed may hold another account's there, and
 * trusting it would leave this account's list unapplied while its
 * publishes, recorded on the server, told the account's other browsers it was
 * applied. So a browser upgraded before the server kept a record asks the
 * server instead, and the first such browser of an account applies the newest
 * list once more. Returns whether publishing is now
 * safe (see ReadSyncSession.pulled), so false means "retry the pull". Never
 * throws.
 */
export async function pullReadStatus(opts: ReadListServer & ReadListAppliedServer & {
  canSign: () => boolean
  npub: () => string | null | undefined
  isCancelled?: () => boolean
  session: ReadSyncSession
  fetchRemote?: typeof fetchReadStatus
}): Promise<boolean> {
  const { session } = opts
  const live = () => !opts.isCancelled?.() && sessionAllowed(opts)
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
    if (!live()) return false
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
    const watermark = readStatusWatermarkTag(session.npub!)
    if (
      !result.fresh ||
      !isSyncEventFresh(watermark, result.createdAt) ||
      !isAcceptableListCreatedAt(result.createdAt)
    ) {
      // Applied before, or published by this device: nothing to apply, but it
      // is what the relays hold, so publishes build on it. One dated too far
      // in the future is not applied either, as for subscription lists: it
      // could not be recorded as applied, so every pull would apply it again.
      // When this account's own watermark here covers it, the server is told,
      // so a browser that applied it before the server kept a record (or whose
      // record failed) stops the account's other browsers applying it again.
      const own = getLastAppliedSyncCreatedAt(watermark)
      if (
        own != null &&
        result.createdAt <= own &&
        (session.serverRecordedAt == null || result.createdAt > session.serverRecordedAt)
      ) {
        await recordAppliedOnServer(opts, session, result.createdAt)
      }
      return settle(guids, result.complete)
    }
    // Without the server's answer the event may already be applied, and
    // applying it again could re-read items marked unread since, so it is not
    // applied. Publishing stays locked and the pull is retried, as for a relay
    // that did not answer.
    let appliedCreatedAt: number | null
    try {
      appliedCreatedAt = await opts.getAppliedCreatedAt()
    } catch (error) {
      console.warn('Read status sync could not read the applied list from the server:', error)
      return settle(guids, false)
    }
    if (!live()) return false
    if (appliedCreatedAt != null && result.createdAt <= appliedCreatedAt) {
      // Another browser of this account applied or published it. It is still
      // what the relays hold, so the cover run gives it to feeds it never
      // reached. The local watermark catches up so later pulls skip the call,
      // and the session knows the server needs no record of it.
      advanceSyncWatermarkIfFresh(watermark, result.createdAt)
      raiseServerRecordedAt(session, appliedCreatedAt)
      return settle(guids, result.complete)
    }
    // The feeds the list was never applied to, taken before the first chunk:
    // they get every chunk, so they can be marked applied once it is done. A
    // feed subscribed during the pull missed the earlier chunks and is not in
    // this snapshot, so it stays uncovered and the cover run applies the list
    // to it. Best effort: without the snapshot they stay uncovered too, and the
    // cover run applies the list to them once more. The snapshot is one page,
    // so past READ_SYNC_FEED_IDS_MAX uncovered feeds the rest get the list a
    // second time from the cover run, which only marks read again.
    let uncovered: string[] = []
    try {
      uncovered = (await opts.getUncovered()).feedIds
    } catch (error) {
      console.warn('Read status sync could not list the feeds still to cover:', error)
    }
    if (!(await applyReadListChunks(guids, (part) => opts.markRead(part), live))) return false
    if (uncovered.length > 0) {
      try {
        await opts.markApplied(uncovered)
      } catch (error) {
        console.warn('Read status sync could not record the covered feeds:', error)
      }
      if (!live()) return false
    }
    opts.onApplied()
    advanceSyncWatermarkIfFresh(watermark, result.createdAt)
    await recordAppliedOnServer(opts, session, result.createdAt)
    return settle(guids, result.complete)
  } catch (error) {
    console.error('Read status sync pull failed:', error)
    return false
  }
}

/**
 * Apply the known remote list to the account's feeds it was never applied to,
 * which the server tracks per subscription (readListAppliedAt), so a key whose
 * feed was not subscribed at pull time still gets marked read once it is. The
 * server picks the feeds, since the client's feed list may be filtered by
 * category or tag, and since another browser may already have covered them:
 * a covered feed never gets the list again, so items the user marked unread
 * there stay unread. Each round snapshots the uncovered feeds, applies the
 * list scoped to them, then marks them applied. A feed subscribed during the
 * round is not in the snapshot and is picked up by the next run. With no
 * remote list (none published, or an empty one) the feeds are still marked
 * applied: there is nothing to give them, and a key this browser publishes
 * later must not be applied to a feed where the user since marked it unread.
 *
 * Runs only after the session's pull. A run asked for while one is going runs
 * again after it. Stops when cancelled, turned off or the account changes.
 * Returns null when no retry is needed, else the delay before retrying, which
 * doubles on each failure. A failure leaves the snapshot uncovered, so a retry
 * applies again the chunks sent before it failed; within that retry window an
 * item the user marked unread in a feed subscribed moments ago can be marked
 * read again, which is accepted. Never throws.
 */
export async function coverNewFeedsWithReadList(opts: ReadListServer & {
  canSign: () => boolean
  npub: () => string | null | undefined
  isCancelled?: () => boolean
  session: ReadSyncSession
}): Promise<number | null> {
  const { session } = opts
  if (session.covering) {
    session.coverAgain = true
    return null
  }
  session.covering = true
  try {
    const live = () => !opts.isCancelled?.() && sessionAllowed(opts) && session.pulled
    do {
      session.coverAgain = false
      if (!live()) return null
      const { feedIds, more } = await opts.getUncovered()
      if (feedIds.length === 0) continue
      const scope = feedIds.slice(0, READ_SYNC_FEED_IDS_MAX)
      const remote = session.remote ?? []
      const send = (part: string[]) => opts.markRead(part, scope)
      const scopeBytes = utf8.encode(JSON.stringify(scope)).length
      if (!(await applyReadListChunks(remote, send, live, scopeBytes))) return null
      await opts.markApplied(scope)
      if (!live()) return null
      if (remote.length > 0) opts.onApplied()
      if (more || feedIds.length > scope.length) session.coverAgain = true
    } while (session.coverAgain)
    session.coverRetryDelay = READ_SYNC_RETRY_MIN_MS
    return null
  } catch (error) {
    console.error('Read status sync for new feeds failed:', error)
    const delay = session.coverRetryDelay
    session.coverRetryDelay = nextReadSyncRetryDelay(delay)
    return delay
  } finally {
    session.covering = false
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
  if (npub && npub === session.npub) advanceSyncWatermarkIfFresh(readStatusWatermarkTag(npub), createdAt)
}

/**
 * Publish the union of the server's read guids and the last list seen on the
 * relays, unless the relays already hold every guid in it. Waits for a
 * successful pull. Publishes are queued so two never race, and a failed one
 * does not block the next. A published list counts as applied on the server:
 * it was built from the server's read state, so another browser of the
 * account must not apply it again. Never throws.
 */
export function publishReadStatusIfChanged(opts: ReadListAppliedServer & {
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
      if (result.success) {
        recordPublished(session, guids, result.createdAt, opts.npub())
        await recordAppliedOnServer(opts, session, result.createdAt)
      } else {
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
 * or the account changed by the time it runs. The empty list is recorded on
 * the server like any publish: there is nothing in it to apply. Never throws.
 */
export function withdrawReadStatus(opts: ReadListAppliedServer & {
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
      if (result.success) {
        recordPublished(session, [], result.createdAt, opts.npub())
        await recordAppliedOnServer(opts, session, result.createdAt)
      } else {
        console.warn('Read status withdraw failed:', result.error)
      }
    } catch (error) {
      console.error('Read status withdraw failed:', error)
    }
  }
  session.queue = session.queue.then(run, run)
  return session.queue
}
