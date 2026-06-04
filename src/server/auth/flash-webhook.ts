import * as jwt from 'jsonwebtoken'

/**
 * Hardened, fail-closed verifier for Flash subscription webhooks.
 *
 * Security model: the signed JWT is the single source of truth for *who* the
 * webhook is about and *what* event fired. The unsigned request body is only
 * trusted insofar as it agrees with the token. This prevents the body-retarget
 * attack where a valid token for one account is replayed with an attacker
 * chosen `npub`/`external_uuid` in the body to activate/modify another account.
 *
 * Clock tolerance: jsonwebtoken enforces `exp` automatically when present; we
 * keep that and only add a small tolerance for clock skew. Expiry is never
 * disabled.
 */

const KNOWN_EVENTS = new Set([
  'user_signed_up',
  'renewal_successful',
  'renewal_failed',
  'user_paused_subscription',
  'user_cancelled_subscription',
])

const CLOCK_TOLERANCE_SECONDS = 30

export type FlashVerifyResult =
  | { ok: true; userPubkey: string; eventName: string }
  | { ok: false; status: number; error: string }

type DecodedToken = {
  version?: string
  eventType?: { id?: string; name?: string }
  user_public_key?: string
  exp?: number
}

type WebhookBody = {
  eventType?: { id?: string; name?: string }
  data?: {
    public_key?: string
    name?: string
    email?: string
    npub?: string
    external_uuid?: string
  }
}

function asString(value: unknown): string | undefined {
  return typeof value === 'string' && value.length > 0 ? value : undefined
}

/**
 * Plain case-insensitive string equality, used both for identity binding and
 * event-name matching. For identities this intentionally does NOT attempt to
 * reconcile an npub-encoded value against its hex equivalent — if the token
 * carries hex and the body carries npub (or vice versa), they are treated as a
 * mismatch and rejected. This is the conservative, fail-closed choice; loosen
 * it only once Flash's exact token schema is confirmed.
 */
function equalsIgnoreCase(a: string, b: string): boolean {
  return a.toLowerCase() === b.toLowerCase()
}

export function verifyFlashWebhook(
  authHeader: string | undefined,
  rawBody: unknown
): FlashVerifyResult {
  // 1. Extract bearer token.
  const token = authHeader?.split(' ')[1]
  if (!token) {
    console.warn('[flash-webhook] rejected: missing bearer token')
    return { ok: false, status: 401, error: 'No token provided' }
  }

  // 2. Read signing key.
  const subscriptionKey = process.env.FLASH_SUBSCRIPTION_KEY
  if (!subscriptionKey) {
    console.error('[flash-webhook] FLASH_SUBSCRIPTION_KEY not configured')
    return { ok: false, status: 500, error: 'Server configuration error' }
  }

  // 3. Verify the JWT (HS256 only; exp enforced with small clock tolerance).
  let decoded: DecodedToken
  try {
    const verified = jwt.verify(token, subscriptionKey, {
      algorithms: ['HS256'],
      clockTolerance: CLOCK_TOLERANCE_SECONDS,
    })
    if (typeof verified === 'string') {
      console.warn('[flash-webhook] rejected: token payload is a string, expected object')
      return { ok: false, status: 401, error: 'Invalid token' }
    }
    decoded = verified as DecodedToken
  } catch (error) {
    console.warn(
      '[flash-webhook] rejected: JWT verification failed:',
      error instanceof Error ? error.message : 'unknown error'
    )
    return { ok: false, status: 401, error: 'Invalid token' }
  }

  const body = (rawBody ?? {}) as WebhookBody

  // 4. Identity binding — the token is authoritative.
  const tokenIdentity = asString(decoded.user_public_key)
  const bodyIdentity = asString(body.data?.npub) ?? asString(body.data?.external_uuid)

  let userPubkey: string
  if (tokenIdentity) {
    // Token carries an identity: it wins. If the body also provides one, it
    // MUST match — otherwise this is a body-retarget attempt.
    if (bodyIdentity && !equalsIgnoreCase(tokenIdentity, bodyIdentity)) {
      console.warn(
        '[flash-webhook] rejected: body identity does not match signed token identity ' +
          '(possible replay/retarget attack)'
      )
      return { ok: false, status: 403, error: 'Identity mismatch' }
    }
    userPubkey = tokenIdentity
  } else {
    // Token has no user_public_key. Schema uncertainty: do NOT silently trust
    // the body. Fail closed unless the operator has explicitly opted into the
    // temporary fallback while confirming Flash's token schema.
    if (process.env.ALLOW_FLASH_BODY_IDENTITY === 'true' && bodyIdentity) {
      console.warn(
        '⚠️ INSECURE: trusting unsigned body identity for Flash webhook because ' +
          'ALLOW_FLASH_BODY_IDENTITY=true and the verified token carried no ' +
          'user_public_key. This bypasses the signed-identity binding and MUST be ' +
          'unset once Flash\'s token schema is confirmed.'
      )
      userPubkey = bodyIdentity
    } else {
      console.warn(
        '[flash-webhook] rejected: verified token has no user_public_key and ' +
          'ALLOW_FLASH_BODY_IDENTITY is not enabled'
      )
      return { ok: false, status: 401, error: 'No verified user identifier' }
    }
  }

  // Event type — the token is authoritative. The signed event name MUST be
  // present; we never fall back to the unsigned body event name. If the body
  // also carries one it must agree with the token's.
  const tokenEvent = asString(decoded.eventType?.name)
  const bodyEvent = asString(body.eventType?.name)

  if (!tokenEvent) {
    console.warn('[flash-webhook] rejected: signed token has no event name')
    return { ok: false, status: 403, error: 'Missing event in token' }
  }

  if (bodyEvent && !equalsIgnoreCase(tokenEvent, bodyEvent)) {
    console.warn('[flash-webhook] rejected: body event name does not match signed token event name')
    return { ok: false, status: 403, error: 'Event mismatch' }
  }

  const eventName = tokenEvent

  // 5. Validate the resolved event is known.
  if (!KNOWN_EVENTS.has(eventName)) {
    console.warn(`[flash-webhook] rejected: unknown event type "${eventName}"`)
    return { ok: false, status: 400, error: 'Unknown event type' }
  }

  return { ok: true, userPubkey, eventName }
}
