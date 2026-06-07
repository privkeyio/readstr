# PayWithFlash Integration

Readstr uses [PayWithFlash](https://paywithflash.com) for Bitcoin Lightning subscription payments.

## Setup

### 1. Create a Subscription Plan on Flash

1. Go to [Flash Dashboard](https://app.paywithflash.com)
2. Navigate to **New Subs > Create a Subscription Plan**
3. Configure your plan:
   - **Name**: Readstr Reader
   - **Price**: 1750 sats
   - **Billing Period**: Monthly
   - **Trial Period**: 7 days

### 2. Configure Webhook

1. Click **"Use Advanced Webhook Features"** checkbox
2. Set **Webhook URL**: `https://nostrfeedz.com/api/webhooks/flash`
3. Save and copy your **Subscription Key** (you'll need this for JWT verification)
4. Copy your **Checkout Page URL**

### 3. Set Environment Variables

Add to your `.env.production` file:

```bash
# Flash Configuration
NEXT_PUBLIC_FLASH_CHECKOUT_URL="https://app.paywithflash.com/subscription-page?flashId=3238"
FLASH_SUBSCRIPTION_KEY="97ee08713791860b5c849941dd596d0208928ab7ae9a468cec993504b06daca4"
```

**Note**: These values are already configured in the production environment.

## How It Works

### Payment Flow

1. **User subscribes**: User clicks "Subscribe" button → tRPC creates checkout URL with pre-filled npub
2. **Pre-filled form**: Flash checkout page auto-fills user's Nostr npub (no manual entry needed)
3. **Payment completed**: Flash sends webhook to `/api/webhooks/flash`
4. **Webhook verified**: JWT token is verified using `FLASH_SUBSCRIPTION_KEY`
5. **Subscription activated**: User record created/updated in database with 30-day access

### Pre-Filled User Details

When authenticated users click subscribe, their Nostr npub is automatically passed to Flash using Base64-encoded JSON:

```javascript
{
  "npub": "npub1...",
  "external_uuid": "npub1...",  // Same as npub for user mapping
  "is_verified": true            // Skip verification (already logged in)
}
```

This saves users from manually entering their npub and ensures the webhook links to the correct account.

### Webhook Events

Flash sends the following events:

- `user_signed_up`: New subscription created → Status: ACTIVE
- `renewal_successful`: Monthly renewal succeeded → Extend 30 days
- `renewal_failed`: Payment failed → Status: PAST_DUE
- `user_paused_subscription`: User paused → Status: CANCELLED
- `user_cancelled_subscription`: User cancelled → Status: CANCELLED

### Security

- All webhooks include a JWT token in the `Authorization` header
- Token is verified using HS256 algorithm with your subscription key
- Tokens expire after 1 hour
- Invalid tokens are rejected with 401 status

## Testing

### Test Webhook Locally

1. Use ngrok to expose your local server:
   ```bash
   ngrok http 3000
   ```

2. Update Flash webhook URL to: `https://your-ngrok-url.ngrok.io/api/webhooks/flash`

3. Make a test payment on Flash checkout page

4. Check your server logs for webhook processing

### Manual Webhook Testing

```bash
# Get a valid JWT token from Flash (check their test tools)
curl -X POST https://nostrfeedz.com/api/webhooks/flash \
  -H "Authorization: Bearer YOUR_JWT_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "eventType": {"id": "1", "name": "user_signed_up"},
    "data": {
      "npub": "npub1test...",
      "email": "test@example.com"
    }
  }'
```

## Database Schema

Subscriptions are stored in the `UserSubscription` table:

```prisma
model UserSubscription {
  userPubkey         String   @unique // Nostr npub or external_uuid
  status             SubscriptionStatus
  trialEndsAt        DateTime
  subscriptionEndsAt DateTime?
  cancelledAt        DateTime?
  ...
}
```

### Subscription Statuses

- `TRIAL`: In 7-day free trial (not used with Flash, goes directly to ACTIVE)
- `ACTIVE`: Paid and active subscription
- `PAST_DUE`: Payment failed, grace period
- `CANCELLED`: User cancelled, valid until end date
- `EXPIRED`: Subscription expired

## Troubleshooting

### Webhooks not received

1. Check Flash dashboard for webhook delivery logs
2. Verify webhook URL is accessible publicly (use curl)
3. Check server logs for errors
4. Ensure HTTPS is enabled (Flash requires HTTPS)

### JWT verification fails

1. Verify `FLASH_SUBSCRIPTION_KEY` matches the key in Flash dashboard
2. Check token hasn't expired (1 hour validity)
3. Ensure secret key has no extra whitespace

### User not getting access

1. Check if webhook was received (server logs)
2. Verify user identifier (npub/external_uuid) is correct
3. Check database for UserSubscription record
4. Verify subscription status is ACTIVE

## Documentation

- [Flash Documentation](https://docs.paywithflash.com/)
- [Subscription API](https://docs.paywithflash.com/products/editor)
- [Webhooks](https://docs.paywithflash.com/products/editor/webhooks)
