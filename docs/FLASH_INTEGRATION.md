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

### 3. Environment (`.env.production`, read at runtime)

```bash
FLASH_CHECKOUT_URL="https://.../subscriptions/signup/<serviceId>/<planId>"
FLASH_WEBHOOK_SECRET="<signing secret from the Flash dashboard>"
```

`FLASH_CHECKOUT_URL` is public. `FLASH_WEBHOOK_SECRET` is a secret: anyone holding it
can forge webhooks. Keep it only in the server's environment, never in docs, compose
files or git, and roll it in Flash if it is ever exposed.

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
| `subscription.canceled` | `CANCELLED`, ends at `canceledAt` | Ends immediately (immediate cancel or dunning exhausted) |
| `subscription.expired` | `EXPIRED` | None (end-of-period cancellation reached its date) |

Other signed events are acknowledged and ignored. Events without a usable `externalRef`
are acknowledged and logged with their `subscriptionId` for manual linking.

4. **Access** (`accessFor` in `src/lib/subscription.ts`). The first time `getStatus`
   sees a user, it saves a `TRIAL` row ending 7 days later. Access is then decided only
   from the stored row.

## Delivery caveats

Flash retries a failed delivery 3 times (1 s, 2 s, 4 s apart) and then drops it. There
is no replay. The grace window keeps a paying user's access if a `renewed` delivery is
lost. `flashSubscriptionId` is stored so rows can be reconciled against Flash's API
(`GET /api/v1/external/subscriptions/{id}/verify`). That reconciliation is not built yet.

## Testing

`npx vitest run src/server/auth/flash-webhook.test.ts src/lib/subscription.test.ts` covers
signatures, the replay window, secret rotation, event mapping and access rules.

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
