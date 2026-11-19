'use client'

import { api } from '@/trpc/react'
import { useRouter } from 'next/navigation'
import { useState } from 'react'
import { getLastSigningError, useNostrAuth } from '@/contexts/NostrAuthContext'
import { dismissTrialBanner, gateScreen, isTrialBannerDismissed } from '@/lib/paywall'
import { MONTHLY_PRICE_SATS, TRIAL_DAYS } from '@/lib/subscription'

interface SubscriptionGateProps {
  children: React.ReactNode
}

const FEATURES = [
  'Unlimited feed subscriptions',
  'RSS and Nostr (NIP-23) support',
  'Tag organization',
  'Read/unread tracking',
  'Cancel anytime',
]

const primaryButton =
  'w-full bg-[#27ae60] hover:bg-[#229954] disabled:bg-[#27ae60]/50 text-white font-bold py-4 px-6 rounded-lg text-lg transition-colors shadow-lg'

function useCheckout() {
  const createCheckout = api.subscription.createCheckoutSession.useMutation()
  const [isRedirecting, setIsRedirecting] = useState(false)
  const [checkoutError, setCheckoutError] = useState<string | null>(null)

  const subscribe = async () => {
    setIsRedirecting(true)
    setCheckoutError(null)
    try {
      const result = await createCheckout.mutateAsync()
      window.location.href = result.checkoutUrl
    } catch (error) {
      // Don't fall back to the bare signup URL: without our `ref` the payment
      // can't be linked to this account. Let the user retry instead.
      console.error('Failed to create checkout session:', error)
      setCheckoutError('Could not start checkout. Check your signer and try again.')
      setIsRedirecting(false)
    }
  }

  return { subscribe, isRedirecting, checkoutError }
}

function GateCard({ title, body, children }: { title: string; body: string; children: React.ReactNode }) {
  const router = useRouter()
  return (
    <div className="flex min-h-screen items-center justify-center bg-gradient-to-br from-[#1a1a1a] via-[#0d1117] to-[#161b22] p-4">
      <div className="max-w-2xl w-full bg-white dark:bg-slate-800 rounded-lg shadow-2xl p-8">
        <div className="text-center mb-6">
          <h1 className="text-3xl font-bold text-slate-900 dark:text-white mb-2">{title}</h1>
          <p className="text-lg text-slate-600 dark:text-slate-400">{body}</p>
        </div>
        {children}
        <div className="text-center">
          <p className="text-sm text-slate-500 dark:text-slate-400 mb-4">
            The Guide and RSS feed generation are always free!
          </p>
          <button onClick={() => router.push('/')} className="text-[#27ae60] dark:text-[#2ecc71] hover:underline">
            Back to Home
          </button>
        </div>
      </div>
    </div>
  )
}

function PlanCard({ price, children }: { price: number; children: React.ReactNode }) {
  return (
    <div className="bg-gradient-to-r from-[#27ae60]/10 to-[#2ecc71]/10 rounded-lg p-6 mb-6 border border-[#27ae60]/40 dark:border-[#2ecc71]/40">
      <div className="text-center mb-4">
        <div className="text-5xl font-bold text-[#27ae60] dark:text-[#2ecc71] mb-2">
          <span className="block">{price}</span>
          <span className="text-2xl text-slate-600 dark:text-slate-400">sats/month</span>
        </div>
        <p className="text-slate-600 dark:text-slate-400">Unlimited RSS and Nostr feeds</p>
        <p className="text-sm text-[#27ae60] dark:text-[#2ecc71] mt-1 font-medium">
          {TRIAL_DAYS} day free trial • Pay with Bitcoin ₿
        </p>
      </div>
      <ul className="space-y-3 mb-6">
        {FEATURES.map((feature) => (
          <li key={feature} className="flex items-center text-slate-700 dark:text-slate-300">
            <svg className="w-5 h-5 text-green-500 mr-3" fill="currentColor" viewBox="0 0 20 20">
              <path fillRule="evenodd" d="M16.707 5.293a1 1 0 010 1.414l-8 8a1 1 0 01-1.414 0l-4-4a1 1 0 011.414-1.414L8 12.586l7.293-7.293a1 1 0 011.414 0z" clipRule="evenodd" />
            </svg>
            {feature}
          </li>
        ))}
      </ul>
      {children}
    </div>
  )
}

export function SubscriptionGate({ children }: SubscriptionGateProps) {
  const { canSign, disconnect } = useNostrAuth()
  const { data: subscription, isLoading, isFetching, refetch } = api.subscription.getStatus.useQuery()
  const { subscribe, isRedirecting, checkoutError } = useCheckout()
  // Read once on mount. The server render can't reach the banner (the status
  // query is still loading there), so this can't cause a hydration mismatch.
  const [bannerDismissed, setBannerDismissed] = useState(
    () => typeof window === 'undefined' || isTrialBannerDismissed(new Date())
  )

  const screen = gateScreen(subscription, { isLoading, canSign })
  const price = subscription?.price ?? MONTHLY_PRICE_SATS

  if (screen === 'loading') {
    return (
      <div className="flex min-h-screen items-center justify-center bg-slate-100 dark:bg-slate-900">
        <div className="text-center">
          <div className="animate-spin rounded-full h-12 w-12 border-b-2 border-[#27ae60] mx-auto mb-4"></div>
          <p className="text-slate-600 dark:text-slate-400">Checking subscription...</p>
        </div>
      </div>
    )
  }

  if (screen === 'signer-required') {
    return (
      <GateCard
        title="Sign in with a signer"
        body="Sign in with a signer to start your free trial or subscribe. A read-only npub view can't, because checkout has to be signed by your key."
      >
        <PlanCard price={price}>
          {/* Ending the read-only session brings back the sign-in screen with its signer options. */}
          <button onClick={disconnect} className={primaryButton}>
            Sign in with a signer
          </button>
        </PlanCard>
      </GateCard>
    )
  }

  if (screen === 'signer-failed') {
    const signingError = getLastSigningError()
    return (
      <GateCard
        title="Could not check your subscription"
        body={`Your signer didn't sign the request, so Readstr can't tell which account this is.${
          signingError ? ` Signing error: ${signingError}.` : ''
        } Check your signer app, then retry.`}
      >
        <div className="mb-6">
          <button onClick={() => void refetch()} disabled={isFetching} className={primaryButton}>
            {isFetching ? 'Retrying...' : 'Retry'}
          </button>
        </div>
      </GateCard>
    )
  }

  if (screen === 'trial-expired' || screen === 'subscription-expired') {
    const trial = screen === 'trial-expired'
    return (
      <GateCard
        title={trial ? 'Trial Expired' : 'Subscription Ended'}
        body={
          trial
            ? 'Your free trial has ended. Subscribe to continue using Readstr.'
            : 'Your subscription has ended. Subscribe again to continue using Readstr.'
        }
      >
        <PlanCard price={price}>
          <button onClick={subscribe} disabled={isRedirecting} className={primaryButton}>
            {isRedirecting ? 'Redirecting...' : `Subscribe — ${price} sats/month`}
          </button>
          {checkoutError && <p className="mt-3 text-center text-sm text-red-600 dark:text-red-400">{checkoutError}</p>}
        </PlanCard>
      </GateCard>
    )
  }

  const days = subscription?.daysRemaining ?? 0
  return (
    <>
      {children}
      {/* Fixed so it doesn't push the full-height reader layout or sit under its fixed header. */}
      {screen === 'trial' && !bannerDismissed && (
        <div className="fixed bottom-0 left-0 right-0 z-[70] bg-[#27ae60] text-white pl-4 pr-10 py-2 text-center text-sm shadow-lg">
          Trial: {days} day{days !== 1 ? 's' : ''} remaining.
          <button
            onClick={subscribe}
            disabled={isRedirecting}
            className="ml-2 underline font-semibold hover:text-white/80 disabled:text-white/60"
          >
            {isRedirecting ? 'Redirecting...' : `Subscribe for ${price} sats/month`}
          </button>
          {checkoutError && <span className="ml-2">{checkoutError}</span>}
          <button
            onClick={() => {
              dismissTrialBanner(new Date())
              setBannerDismissed(true)
            }}
            aria-label="Dismiss for today"
            title="Dismiss for today"
            className="absolute right-3 top-1/2 -translate-y-1/2 px-1 text-lg leading-none font-bold hover:text-white/80"
          >
            ×
          </button>
        </div>
      )}
    </>
  )
}
