import { httpBatchLink, httpLink, splitLink, type HTTPHeaders } from '@trpc/client'
import type { AnyRouter } from '@trpc/server'
import superjson from 'superjson'
import { TRPC_MAX_URL_LENGTH } from './batch-limits'

type FetchLike = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>

/**
 * URL length the batch link would build for this one query on its own,
 * mirroring tRPC 10's getUrl for a batch of one.
 */
export function singleQueryBatchUrlLength(url: string, path: string, input: unknown): number {
  const serialized = JSON.stringify({ 0: superjson.serialize(input) })
  return `${url}/${path}?batch=1&input=`.length + encodeURIComponent(serialized).length
}

/**
 * HTTP links for the tRPC client. The batch link's `maxURLLength` splits large
 * batches so none exceeds the server's batch cap, but tRPC 10 rejects a single
 * query whose own URL is over the limit ("Input is too big for a single
 * dispatch") rather than sending it. Those queries go through the unbatched
 * link instead: it sends exactly one call, so the server counts it as one and
 * the batch cap is unaffected. Both links share the same fetch, so NIP-98
 * signing applies to either path.
 */
export function createTrpcHttpLinks<TRouter extends AnyRouter>(opts: {
  url: string
  headers?: () => HTTPHeaders
  fetch?: FetchLike
}) {
  return splitLink<TRouter>({
    condition: op =>
      op.type === 'query' &&
      singleQueryBatchUrlLength(opts.url, op.path, op.input) > TRPC_MAX_URL_LENGTH,
    true: httpLink({ url: opts.url, headers: opts.headers, fetch: opts.fetch }),
    false: httpBatchLink({
      url: opts.url,
      maxURLLength: TRPC_MAX_URL_LENGTH,
      headers: opts.headers,
      fetch: opts.fetch,
    }),
  })
}
