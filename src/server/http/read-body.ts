/**
 * Read a request body as text, refusing anything over `maxBytes`.
 *
 * Request bodies feed JSON.parse and then zod. zod v3 keeps parsing every
 * array element even after `.max()` fails, so a ~1 MB array can cost ~150 MB
 * of heap (GHSA-mrhx-c7xc-m3w5). Capping the bytes before parsing is the only
 * mitigation that holds until the zod 4 upgrade, and it also bounds HMAC and
 * NIP-98 payload hashing.
 *
 * Content-Length is checked first so honest oversize requests are refused
 * without reading. The stream is still counted as it is read, because the
 * header can be missing (chunked) or wrong, and reading stops as soon as the
 * limit is passed rather than buffering the rest.
 *
 * Invalid UTF-8 is refused rather than replaced: each bad byte would become a
 * U+FFFD that re-encodes to 3 bytes, so the body handed on to tRPC could be up
 * to 3x the cap, and no legitimate client sends malformed UTF-8.
 */

export type ReadBodyResult = { ok: true; body: string } | { ok: false; status: 400 | 413 }

export async function readBodyWithLimit(req: Request, maxBytes: number): Promise<ReadBodyResult> {
  const declared = req.headers.get('content-length')
  if (declared !== null) {
    if (!/^\d+$/.test(declared.trim())) return { ok: false, status: 400 }
    if (Number(declared) > maxBytes) return { ok: false, status: 413 }
  }

  if (!req.body) return { ok: true, body: '' }

  const reader = req.body.getReader()
  const decoder = new TextDecoder('utf-8', { fatal: true })
  let received = 0
  let body = ''
  for (;;) {
    const { done, value } = await reader.read()
    if (done) break
    received += value.byteLength
    if (received > maxBytes) {
      await reader.cancel().catch(() => {})
      return { ok: false, status: 413 }
    }
    try {
      body += decoder.decode(value, { stream: true })
    } catch {
      await reader.cancel().catch(() => {})
      return { ok: false, status: 400 }
    }
  }
  try {
    return { ok: true, body: body + decoder.decode() }
  } catch {
    return { ok: false, status: 400 }
  }
}
