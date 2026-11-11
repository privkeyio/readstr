/**
 * Shared by the browser client and the /api/trpc route so the two caps stay
 * consistent and normal use never hits the server's 400.
 *
 * tRPC 10's httpBatchLink has no `maxItems` option, only `maxURLLength`, so
 * the client bounds a batch by URL length. Every batched call adds its
 * procedure path plus a comma to the URL, and the shortest path in the router
 * is `feed.getFeeds` (13 characters). A 2,000 character URL therefore holds at
 * most ~141 calls, and fewer for queries, whose inputs are also in the URL.
 * route.test.ts recomputes this bound from the live router so adding a shorter
 * procedure name cannot silently push the client past the server cap.
 */
export const TRPC_MAX_URL_LENGTH = 2_000

/**
 * Most procedure calls the server accepts in one batched request. The largest
 * batch the app builds on its own is the auto-mark-read filter action, which
 * fires one markAsRead per unread item on a 50-item page and then up to four
 * refetches per settled mutation (~200 queries in one tick). The client's URL
 * cap splits those into requests of ~141 calls or fewer, so 200 leaves
 * headroom while stopping a single 16 KB URL from fanning out to ~900 calls.
 */
export const MAX_TRPC_BATCH_SIZE = 200
