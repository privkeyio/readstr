import { NextRequest, NextResponse } from 'next/server'
import { db } from '@/server/db'
import { changeFor, verifyFlashWebhook } from '@/server/auth/flash-webhook'
import { readBodyWithLimit } from '@/server/http/read-body'

/** Flash's subscription events are well under 10 KB; anything bigger isn't one. */
const MAX_WEBHOOK_BODY_BYTES = 64_000

/**
 * Flash subscription webhooks.
 *
 * Flash retries a failed delivery only 3 times within seconds and then drops
 * it, and deliveries can repeat or arrive out of order. So every event is
 * applied as an absolute state (never "extend by 30 days"), and a delivery
 * older than the last one applied to the row is acknowledged and skipped.
 */
export async function POST(request: NextRequest) {
  try {
    const read = await readBodyWithLimit(request, MAX_WEBHOOK_BODY_BYTES)
    if (!read.ok) {
      return NextResponse.json({ error: 'Invalid body' }, { status: read.status })
    }
    const rawBody = read.body
    const result = verifyFlashWebhook(
      request.headers.get('flash-signature'),
      rawBody,
      process.env.FLASH_WEBHOOK_SECRET
    )

    if (!result.ok) {
      return NextResponse.json({ error: result.error }, { status: result.status })
    }
    if ('ignored' in result) {
      console.log('[flash-webhook] ignored:', result.ignored)
      return NextResponse.json({ ok: true })
    }

    const { event } = result
    if (!event.userPubkey) {
      // Signed up without our `ref` (e.g. from a bare signup link), so there is
      // no account to apply it to. Acknowledge so Flash stops retrying, and log
      // the id so it can be linked by hand.
      console.warn('[flash-webhook] no usable externalRef; subscription needs manual linking:', {
        event: event.event,
        subscriptionId: event.subscriptionId,
      })
      return NextResponse.json({ ok: true })
    }

    const { userPubkey, occurredAt, subscriptionId } = event
    const existing = await db.userSubscription.findUnique({ where: { userPubkey } })
    if (existing?.flashEventAt && existing.flashEventAt > occurredAt) {
      console.log('[flash-webhook] skipped out-of-order delivery:', event.event, subscriptionId)
      return NextResponse.json({ ok: true })
    }

    const change = changeFor(event)
    const tracking = { flashSubscriptionId: subscriptionId, flashEventAt: occurredAt }
    await db.userSubscription.upsert({
      where: { userPubkey },
      create: { userPubkey, trialEndsAt: occurredAt, ...change, ...tracking },
      update: { ...change, ...tracking },
    })

    console.log('[flash-webhook] applied:', event.event, userPubkey.substring(0, 10) + '...')
    return NextResponse.json({ ok: true })
  } catch (error) {
    console.error('[flash-webhook] error:', error)
    return NextResponse.json({ error: 'Internal server error' }, { status: 500 })
  }
}
