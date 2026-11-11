/**
 * Count the procedure calls a tRPC request asks for, using only its URL, so the
 * route can refuse oversize batches and charge the rate limit before reading
 * the body or running any procedure.
 *
 * tRPC takes the path after the endpoint, percent-decodes it, and splits it on
 * commas, so this does the same: otherwise `%2C` would hide calls from the
 * count. Every request is counted this way even without `?batch=1`, which can
 * only overcount, since tRPC treats a non-batched comma path as one unknown
 * procedure. Returns null when the path is not valid percent-encoding, which
 * tRPC would reject anyway.
 */
export function countTrpcCalls(url: string, endpoint: string): number | null {
  const path = new URL(url).pathname.slice(endpoint.length)
  let decoded: string
  try {
    decoded = decodeURIComponent(path)
  } catch {
    return null
  }
  return decoded.split(',').length
}
