import { createHmac, timingSafeEqual } from 'crypto'
import { normalizePubkey, PERIOD_DAYS } from '@/lib/subscription'

/**
 * Verifier and event mapping for Flash subscription webhooks.
 *
 * Flash signs each delivery with a per-endpoint `whsec_` secret:
 *   Flash-Signature: t=<unix seconds>,v1=<hex HMAC-SHA256 of "<t>.<raw body>">
 * The signature covers the whole raw body, so once it verifies every field in
 * the payload (including the subscriber's `externalRef`) is authentic. The
 * timestamp is part of the signed string, so rejecting stale ones blocks
 * replays. See https://flashpay-1.gitbook.io/flash-documentation/subscriptions/webhooks
 */

/** Flash's recommended replay window. Future-dated stamps get the same slack for clock skew. */
export const TOLERANCE_SECONDS = 5 * 60

const DAY_MS = 24 * 60 * 60 * 1000

export const FLASH_EVENTS = [
  'subscription.activated',
  'subscription.renewed',
  'subscription.past_due',
  'subscription.canceled',
  'subscription.expired',
] as const

export type FlashEventName = (typeof FLASH_EVENTS)[number]

export interface FlashEvent {
  event: FlashEventName
  /** When the domain event happened; used to drop out-of-order deliveries. */
  occurredAt: Date
  subscriptionId: string
  /** Hex pubkey from `externalRef`, or null when the subscriber has none we can use. */
  userPubkey: string | null
  data: Record<string, unknown>
}

export type FlashVerifyResult =
  | { ok: true; event: FlashEvent }
  | { ok: true; ignored: string }
  | { ok: false; status: number; error: string }

function parseSignatureHeader(header: string): { t: number; v1: string[] } | null {
  let t: number | undefined
  const v1: string[] = []
  for (const part of header.split(',')) {
    const eq = part.indexOf('=')
    if (eq < 0) continue
    const key = part.slice(0, eq).trim()
    const value = part.slice(eq + 1).trim()
    if (key === 't' && /^\d+$/.test(value)) t = Number(value)
    // Several v1 values can appear while a secret is being rotated.
    if (key === 'v1' && /^[0-9a-f]{64}$/i.test(value)) v1.push(value.toLowerCase())
  }
  return t === undefined || v1.length === 0 ? null : { t, v1 }
}

function signatureMatches(expectedHex: string, candidates: string[]): boolean {
  const expected = Buffer.from(expectedHex, 'hex')
  // Check every candidate rather than returning early, so timing doesn't reveal which matched.
  let matched = false
  for (const candidate of candidates) {
    const given = Buffer.from(candidate, 'hex')
    if (given.length === expected.length && timingSafeEqual(given, expected)) matched = true
  }
  return matched
}

function asString(value: unknown): string | undefined {
  return typeof value === 'string' && value.length > 0 ? value : undefined
}

function asDate(value: unknown): Date | undefined {
  const s = asString(value)
  if (!s) return undefined
  const d = new Date(s)
  return Number.isNaN(d.getTime()) ? undefined : d
}

/**
 * Verify a delivery and parse it into a FlashEvent.
 *
 * `rawBody` must be the exact bytes Flash sent (request.text()), never a
 * re-serialized object, or the HMAC will not match.
 */
export function verifyFlashWebhook(
  signatureHeader: string | null,
  rawBody: string,
  secret: string | undefined,
  nowSeconds: number = Math.floor(Date.now() / 1000)
): FlashVerifyResult {
  if (!secret) {
    console.error('[flash-webhook] FLASH_WEBHOOK_SECRET not configured')
    return { ok: false, status: 500, error: 'Server configuration error' }
  }

  const signature = signatureHeader ? parseSignatureHeader(signatureHeader) : null
  if (!signature) {
    console.warn('[flash-webhook] rejected: missing or malformed Flash-Signature')
    return { ok: false, status: 400, error: 'bad signature' }
  }

  if (Math.abs(nowSeconds - signature.t) > TOLERANCE_SECONDS) {
    console.warn('[flash-webhook] rejected: signature timestamp outside tolerance')
    return { ok: false, status: 400, error: 'stale' }
  }

  const expected = createHmac('sha256', secret).update(`${signature.t}.${rawBody}`).digest('hex')
  if (!signatureMatches(expected, signature.v1)) {
    console.warn('[flash-webhook] rejected: signature mismatch')
    return { ok: false, status: 400, error: 'bad signature' }
  }

  // Authentic from here on: the body is covered by the signature.
  let body: { event?: unknown; timestamp?: unknown; data?: unknown }
  try {
    body = JSON.parse(rawBody)
  } catch {
    return { ok: false, status: 400, error: 'Invalid JSON body' }
  }

  const event = asString(body.event)
  if (!event || !(FLASH_EVENTS as readonly string[]).includes(event)) {
    // A signed event we don't handle (Flash may add more). Acknowledge it so
    // Flash doesn't retry something we will never process.
    return { ok: true, ignored: `unhandled event ${event ?? '(none)'}` }
  }

  const data = (body.data && typeof body.data === 'object' ? body.data : {}) as Record<string, unknown>
  const subscriptionId = asString(data.subscriptionId)
  const occurredAt = asDate(body.timestamp)
  if (!subscriptionId || !occurredAt) {
    return { ok: false, status: 400, error: 'Missing subscriptionId or timestamp' }
  }

  const externalRef = asString(data.externalRef)
  return {
    ok: true,
    event: {
      event: event as FlashEventName,
      occurredAt,
      subscriptionId,
      userPubkey: externalRef ? normalizePubkey(externalRef) : null,
      data,
    },
  }
}

/** Fields to write to UserSubscription for an event. */
export interface SubscriptionChange {
  status: 'ACTIVE' | 'PAST_DUE' | 'CANCELLED' | 'EXPIRED'
  subscriptionEndsAt?: Date
  cancelledAt?: Date | null
}

/**
 * Map a Flash lifecycle event onto our subscription row.
 *
 * Flash is the source of truth for the lifecycle: an end-of-period
 * cancellation keeps the subscription active and later arrives as `expired`,
 * while `canceled` means access ends now (immediate cancel, or dunning
 * exhausted). Webhooks carry no period end, so a paid period is assumed to run
 * PERIOD_DAYS from payment; accessFor adds a grace window on top so a late or
 * dropped `renewed` delivery doesn't lock out a paying user.
 */
export function changeFor(event: FlashEvent): SubscriptionChange {
  const { data, occurredAt } = event
  switch (event.event) {
    case 'subscription.activated': {
      const from = asDate(data.activatedAt) ?? occurredAt
      return { status: 'ACTIVE', subscriptionEndsAt: new Date(from.getTime() + PERIOD_DAYS * DAY_MS), cancelledAt: null }
    }
    case 'subscription.renewed': {
      const from = asDate(data.paidAt) ?? occurredAt
      return { status: 'ACTIVE', subscriptionEndsAt: new Date(from.getTime() + PERIOD_DAYS * DAY_MS), cancelledAt: null }
    }
    case 'subscription.past_due':
      return { status: 'PAST_DUE' }
    case 'subscription.canceled': {
      const at = asDate(data.canceledAt) ?? occurredAt
      return { status: 'CANCELLED', subscriptionEndsAt: at, cancelledAt: at }
    }
    case 'subscription.expired':
      return { status: 'EXPIRED', subscriptionEndsAt: asDate(data.expiredAt) ?? occurredAt }
  }
}
