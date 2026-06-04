import { NextRequest, NextResponse } from 'next/server'
import { db } from '@/server/db'
import { verifyFlashWebhook } from '@/server/auth/flash-webhook'

const TRIAL_DAYS = 7

export async function POST(request: NextRequest) {
  try {
    const authHeader = request.headers.get('authorization') ?? undefined

    let rawBody: unknown
    try {
      rawBody = await request.json()
    } catch {
      return NextResponse.json({ error: 'Invalid JSON body' }, { status: 400 })
    }

    const result = verifyFlashWebhook(authHeader, rawBody)
    if (!result.ok) {
      return NextResponse.json({ error: result.error }, { status: result.status })
    }

    const { userPubkey, eventName } = result

    console.log('Flash webhook received:', {
      event: eventName,
      userPubkey: userPubkey.substring(0, 10) + '...',
    })

    // Handle different event types
    switch (eventName) {
      case 'user_signed_up':
        // Create or update subscription to ACTIVE
        await db.userSubscription.upsert({
          where: { userPubkey },
          create: {
            userPubkey,
            status: 'ACTIVE',
            trialEndsAt: new Date(Date.now() + TRIAL_DAYS * 24 * 60 * 60 * 1000),
            subscriptionEndsAt: new Date(Date.now() + 30 * 24 * 60 * 60 * 1000), // 30 days
          },
          update: {
            status: 'ACTIVE',
            subscriptionEndsAt: new Date(Date.now() + 30 * 24 * 60 * 60 * 1000),
            cancelledAt: null,
          },
        })
        console.log('User subscription activated:', userPubkey.substring(0, 10) + '...')
        break

      case 'renewal_successful':
        // Extend subscription by 30 days
        await db.userSubscription.update({
          where: { userPubkey },
          data: {
            status: 'ACTIVE',
            subscriptionEndsAt: new Date(Date.now() + 30 * 24 * 60 * 60 * 1000),
          },
        })
        console.log('Subscription renewed:', userPubkey.substring(0, 10) + '...')
        break

      case 'renewal_failed':
        // Mark as PAST_DUE
        await db.userSubscription.update({
          where: { userPubkey },
          data: {
            status: 'PAST_DUE',
          },
        })
        console.log('Renewal failed:', userPubkey.substring(0, 10) + '...')
        break

      case 'user_paused_subscription':
      case 'user_cancelled_subscription':
        // Mark as cancelled but keep until end date
        await db.userSubscription.update({
          where: { userPubkey },
          data: {
            status: 'CANCELLED',
            cancelledAt: new Date(),
          },
        })
        console.log('Subscription cancelled:', userPubkey.substring(0, 10) + '...')
        break
    }

    return NextResponse.json({ ok: true })
  } catch (error) {
    console.error('Flash webhook error:', error)
    return NextResponse.json(
      { error: 'Internal server error' },
      { status: 500 }
    )
  }
}
