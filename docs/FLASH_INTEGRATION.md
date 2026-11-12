# Flash Integration

Readstr uses [Flash](https://flashpay-1.gitbook.io/flash-documentation/subscriptions) for
Bitcoin Lightning subscriptions. The paywall (`SubscriptionGate`) is currently **disabled**,
so the web app is free. This describes how payments work when it is switched on.

## Setup

### 1. Service and plan

In the Flash dashboard, open **Subscriptions** and add a service ("Readstr Reader") with
these settings:

- **Dunning:** 3 attempts, every 3 days, 7-day grace, cancel after the final failure.
  The server's `GRACE_DAYS` matches this.
- **Cancellation:** at the end of the current period.

Then add a plan to the service:

- **Price:** 1750 sats per month.
- **Trial days:** 0. Readstr gives its own 7-day trial before checkout.
- **Payment method:** the Lightning wallet.

Copy the plan's **signup URL**.

### 2. Webhook endpoint

Under **Settings → API & Webhooks → Webhooks**, add an endpoint:

- **URL:** `https://readstr.privkey.io/api/webhooks/flash`
- **Events:** Subscription Activated, Renewed, Canceled, Expired, Past Due

Copy the endpoint's **signing secret** (`whsec_...`).

### 3. API key (optional, recommended)

Under **Settings → API & Webhooks → API Keys**, create a key with only the
**View Subscriptions** scope. With it, the server checks Flash's own records instead of
trusting each webhook on its own (see *Reconciliation* below).

### 4. Environment (`.env.production`, read at runtime)

```bash
FLASH_CHECKOUT_URL="https://.../subscriptions/signup/<serviceId>/<planId>"
FLASH_WEBHOOK_SECRET="<signing secret from the Flash dashboard>"
FLASH_API_KEY="<API key with View Subscriptions>"   # optional
```

`FLASH_CHECKOUT_URL` is public. `FLASH_WEBHOOK_SECRET` and `FLASH_API_KEY` are secrets:
the first lets anyone forge webhooks, the second lets anyone read your subscribers.
Keep them only in the server's environment, never in docs, compose files or git, and
roll them in Flash if they are ever exposed. The API is called on the checkout URL's
host, so `FLASH_API_KEY` does nothing without `FLASH_CHECKOUT_URL`.

> **Security notice**: the legacy `FLASH_SUBSCRIPTION_KEY` (the old JWT scheme) was
> published in this repo and remains in git history, so treat it as compromised.
> The server no longer reads it. Remove it from every environment's
> `.env.production` and secrets, and if you control the old Flash account that
> issued it, revoke it there.

## How it works

1. **Checkout.** `subscription.createCheckoutSession` returns the signup URL with
   `?ref=<hex pubkey>` for the NIP-98-authenticated user. Flash stores `ref` as the
   subscription's `externalRef` and echoes it in every webhook. If the call fails, the
   paywall shows an error rather than falling back to the bare URL, because a payment
   without `ref` can't be linked to an account.
2. **Webhook verification** (`src/server/auth/flash-webhook.ts`). The verifier checks
   `Flash-Signature: t=<unix>,v1=<hex>` as an HMAC-SHA256 of `"<t>.<raw body>"` with
   `FLASH_WEBHOOK_SECRET`, using a constant-time comparison. It rejects timestamps more
   than 5 minutes off (replay). The signature covers the whole body, so `externalRef`
   is trusted once it verifies.
3. **Applying events** (`src/app/api/webhooks/flash/route.ts`). Each event sets an
   absolute state, so duplicate deliveries are harmless. A delivery older than the
   row's `flashEventAt` is skipped, which handles out-of-order arrival.

| Flash event | Row change | Access |
|---|---|---|
| `subscription.activated` | `ACTIVE`, paid until `activatedAt` + 30 days | Paid period + 7-day grace |
| `subscription.renewed` | `ACTIVE`, paid until `paidAt` + 30 days | Paid period + 7-day grace |
| `subscription.past_due` | `PAST_DUE`, period unchanged | Paid period + 7-day grace (Flash retries) |
| `subscription.canceled` | `CANCELLED`, `cancelledAt` set, period unchanged | Until the paid period ends, no grace. Flash sends this as soon as any cancellation is requested, including one scheduled for the period end |
| `subscription.expired` | `EXPIRED` | None (the scheduled end date was reached) |

Other signed events are acknowledged and ignored. Events without a usable `externalRef`
are acknowledged and logged with their `subscriptionId` for manual linking.

4. **Reconciliation** (`src/server/flash-reconcile.ts`, only with `FLASH_API_KEY`). Each
   webhook instead triggers a call to `GET /api/v1/external/subscriptions?ref=<pubkey>`, and
   the row is rebuilt from every subscription Flash lists for that pubkey: the one granting
   the latest access (grace included, `ACTIVE` on ties) wins, using Flash's real
   `currentPeriodEnd` (or `cancelEffectiveDate`). So an old or foreign subscription can't
   end a paid one, and event order doesn't matter. Nothing is written, and the webhook is
   applied as in the table above, when Flash can't be reached, lists nothing usable, doesn't
   yet show the event's own subscription as started (its list can lag the webhook).
   Separately, when Flash lists nothing that grants access while the row still grants it and
   its linked subscription isn't in the list (a local trial, or a subscription linked by
   hand), the row is kept and the webhook is acknowledged without being applied. Records
   that fail validation (non-ISO dates, years outside 1970-9999) are skipped one by one, and responses over 1 MB
   are refused. `getStatus` makes the same check when a row denies access or ends within a
   day, at most every 2 minutes per user and 30 times a minute overall, which repairs rows
   whose webhook was lost.
5. **Access** (`accessFor` in `src/lib/subscription.ts`). The first time `getStatus`
   sees a user, it saves a `TRIAL` row ending 7 days later. Access is then decided only
   from the stored row.

## Delivery caveats

Flash retries a failed delivery 3 times (1 s, 2 s, 4 s apart) and then drops it. There
is no replay. The grace window keeps a paying user's access if a `renewed` delivery is
lost, and with `FLASH_API_KEY` set, reconciliation repairs any lost event the next time
the user's row lapses.

The rest of this section describes the webhook-only fallback, used without an API key.

A row follows one Flash subscription at a time. While the row still grants access, only
`activated` can link it to a new one (or `renewed`, when the linked subscription is
cancelled and so can't renew), and other events from a different subscription are
logged and skipped, so an old subscription's `expired` cannot end a newer paid one. Once
the row no longer grants access, any event can relink it.

A `CANCELLED` row has no grace: access stops at the paid-through date. If Flash sends
only `canceled` for an immediate cancel or refund, the user keeps access until that
date; end it by hand if a refund should end access sooner.

## Testing

`npx vitest run src/server/auth/flash-webhook.test.ts src/server/flash-reconcile.test.ts src/lib/subscription.test.ts`
covers signatures, the replay window, secret rotation, event mapping, reconciliation and
access rules.

To send a signed delivery by hand (substitute your own secret and pubkey):

```bash
BODY='{"event":"subscription.activated","timestamp":"2026-11-10T12:00:00.000Z","data":{"accountId":"a","subscriptionId":"s1","externalRef":"<hex pubkey>"}}'
T=$(date +%s)
SIG=$(printf '%s.%s' "$T" "$BODY" | openssl dgst -sha256 -hmac "$FLASH_WEBHOOK_SECRET" -hex | sed 's/^.* //')
curl -X POST http://localhost:3000/api/webhooks/flash \
  -H 'Content-Type: application/json' \
  -H "Flash-Signature: t=$T,v1=$SIG" \
  --data "$BODY"
```
