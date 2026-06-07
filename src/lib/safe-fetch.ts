import { lookup } from 'dns/promises'
import { lookup as lookupCb } from 'dns'
import { isIP } from 'net'
import { Agent } from 'undici'

const MAX_BODY_BYTES = 5 * 1024 * 1024 // 5 MB
const MAX_REDIRECTS = 5

function ipv4ToBlocked(ip: string): boolean {
  const parts = ip.split('.').map((p) => parseInt(p, 10))
  if (parts.length !== 4 || parts.some((p) => Number.isNaN(p) || p < 0 || p > 255)) {
    return true
  }
  const [a, b, c] = parts
  if (a === 0) return true // 0.0.0.0/8
  if (a === 10) return true // 10.0.0.0/8
  if (a === 127) return true // loopback
  if (a === 100 && b >= 64 && b <= 127) return true // CGNAT 100.64.0.0/10
  if (a === 169 && b === 254) return true // link-local
  if (a === 172 && b >= 16 && b <= 31) return true // 172.16.0.0/12
  if (a === 192 && b === 0 && (c === 0 || c === 2)) return true // 192.0.0.0/24, 192.0.2.0/24 (TEST-NET-1)
  if (a === 192 && b === 168) return true // 192.168.0.0/16
  if (a === 198 && (b === 18 || b === 19)) return true // benchmarking
  if (a >= 224) return true // multicast + reserved 224.0.0.0/3
  return false
}

function ipv6ToBlocked(ip: string): boolean {
  const lower = ip.toLowerCase()
  // IPv4-mapped addresses (::ffff:a.b.c.d or normalized ::ffff:7f00:1)
  const mapped = lower.match(/^::ffff:(.+)$/)
  if (mapped) {
    const rest = mapped[1]
    if (rest.includes('.')) return ipv4ToBlocked(rest)
    const groups = rest.split(':')
    if (groups.length === 2) {
      const hi = parseInt(groups[0], 16)
      const lo = parseInt(groups[1], 16)
      if (!Number.isNaN(hi) && !Number.isNaN(lo)) {
        return ipv4ToBlocked([(hi >> 8) & 255, hi & 255, (lo >> 8) & 255, lo & 255].join('.'))
      }
    }
    return true
  }
  if (lower === '::' || lower === '::1') return true // unspecified / loopback
  if (lower.startsWith('fc') || lower.startsWith('fd')) return true // fc00::/7 ULA
  if (lower.startsWith('fe8') || lower.startsWith('fe9') || lower.startsWith('fea') || lower.startsWith('feb')) {
    return true // fe80::/10 link-local
  }
  if (lower.startsWith('ff')) return true // multicast
  return false
}

function isBlockedAddress(ip: string): boolean {
  const family = isIP(ip)
  if (family === 4) return ipv4ToBlocked(ip)
  if (family === 6) return ipv6ToBlocked(ip)
  return true
}

// Validate at the resolution undici actually connects to, closing the
// TOCTOU/DNS-rebinding gap where a separate pre-flight lookup could differ
// from the address the request ultimately connects to.
const safeDispatcher = new Agent({
  connect: {
    lookup(hostname, options, callback) {
      lookupCb(hostname, { ...options, all: true }, (err, addresses) => {
        if (err) {
          callback(err, '', 0)
          return
        }
        const list = addresses as unknown as { address: string; family: number }[]
        if (list.length === 0 || list.some((a) => isBlockedAddress(a.address))) {
          callback(new Error('URL resolves to a disallowed address'), '', 0)
          return
        }
        if (options && (options as { all?: boolean }).all) {
          callback(null, list as never)
        } else {
          callback(null, list[0].address, list[0].family)
        }
      })
    },
  },
})

export async function assertSafeUrl(rawUrl: string): Promise<URL> {
  let parsed: URL
  try {
    parsed = new URL(rawUrl)
  } catch {
    throw new Error('Invalid URL')
  }

  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
    throw new Error('Only http and https URLs are allowed')
  }

  const hostname = parsed.hostname.replace(/^\[|\]$/g, '')

  if (isIP(hostname)) {
    if (isBlockedAddress(hostname)) {
      throw new Error('URL resolves to a disallowed address')
    }
    return parsed
  }

  let addresses: { address: string }[]
  try {
    addresses = await lookup(hostname, { all: true })
  } catch {
    throw new Error('Unable to resolve host')
  }

  if (addresses.length === 0 || addresses.some((a) => isBlockedAddress(a.address))) {
    throw new Error('URL resolves to a disallowed address')
  }

  return parsed
}

async function readCappedText(response: Response): Promise<string> {
  const reader = response.body?.getReader()
  if (!reader) return await response.text()

  const chunks: Uint8Array[] = []
  let total = 0
  try {
    while (true) {
      const { done, value } = await reader.read()
      if (done) break
      if (value) {
        total += value.length
        if (total > MAX_BODY_BYTES) {
          throw new Error('Response body too large')
        }
        chunks.push(value)
      }
    }
  } catch (err) {
    await reader.cancel().catch(() => {})
    throw err
  }

  const merged = new Uint8Array(total)
  let offset = 0
  for (const chunk of chunks) {
    merged.set(chunk, offset)
    offset += chunk.length
  }
  return new TextDecoder().decode(merged)
}

export interface SafeResponse {
  ok: boolean
  status: number
  statusText: string
  headers: Headers
  text: () => Promise<string>
  cancel: () => Promise<void>
}

export async function safeFetch(rawUrl: string, init: RequestInit = {}): Promise<SafeResponse> {
  let currentUrl = rawUrl

  for (let i = 0; i <= MAX_REDIRECTS; i++) {
    await assertSafeUrl(currentUrl)

    const response = await fetch(currentUrl, { ...init, redirect: 'manual', dispatcher: safeDispatcher } as RequestInit)

    if (response.status >= 300 && response.status < 400 && response.headers.has('location')) {
      const location = response.headers.get('location') as string
      await response.body?.cancel().catch(() => {})
      currentUrl = new URL(location, currentUrl).href
      continue
    }

    if (!response.ok) {
      await response.body?.cancel().catch(() => {})
      return {
        ok: response.ok,
        status: response.status,
        statusText: response.statusText,
        headers: response.headers,
        text: async () => '',
        cancel: async () => {},
      }
    }

    return {
      ok: response.ok,
      status: response.status,
      statusText: response.statusText,
      headers: response.headers,
      text: () => readCappedText(response),
      cancel: async () => {
        await response.body?.cancel().catch(() => {})
      },
    }
  }

  throw new Error('Too many redirects')
}
