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

function groupsToIpv4(hi: number, lo: number): string {
  return [(hi >> 8) & 255, hi & 255, (lo >> 8) & 255, lo & 255].join('.')
}

function groupsAreZero(groups: number[], start: number, end: number): boolean {
  for (let i = start; i < end; i++) {
    if (groups[i] !== 0) return false
  }
  return true
}

function expandIpv6(ip: string): number[] | null {
  const halves = ip.split('::')
  if (halves.length > 2) return null

  function parseGroups(segment: string): number[] | null {
    if (segment === '') return []
    const out: number[] = []
    const parts = segment.split(':')
    for (let i = 0; i < parts.length; i++) {
      const part = parts[i]
      if (part.includes('.')) {
        if (i !== parts.length - 1) return null
        const octets = part.split('.').map((o) => parseInt(o, 10))
        if (octets.length !== 4 || octets.some((o) => Number.isNaN(o) || o < 0 || o > 255)) return null
        out.push((octets[0] << 8) | octets[1], (octets[2] << 8) | octets[3])
      } else {
        const value = parseInt(part, 16)
        if (Number.isNaN(value) || value < 0 || value > 0xffff) return null
        out.push(value)
      }
    }
    return out
  }

  if (halves.length === 2) {
    const head = parseGroups(halves[0])
    const tail = parseGroups(halves[1])
    if (head === null || tail === null) return null
    const fill = 8 - head.length - tail.length
    if (fill < 0) return null
    const expanded = [...head, ...new Array(fill).fill(0), ...tail]
    if (expanded.length !== 8) return null
    return expanded
  }

  const all = parseGroups(ip)
  if (all === null || all.length !== 8) return null
  return all
}

function ipv6ToBlocked(ip: string): boolean {
  const lower = ip.toLowerCase()
  if (lower === '::' || lower === '::1') return true // unspecified / loopback
  if (lower.startsWith('fc') || lower.startsWith('fd')) return true // fc00::/7 ULA
  if (lower.startsWith('fe8') || lower.startsWith('fe9') || lower.startsWith('fea') || lower.startsWith('feb')) {
    return true // fe80::/10 link-local
  }
  if (lower.startsWith('ff')) return true // multicast

  const groups = expandIpv6(lower)
  if (!groups) return true // fail closed: unparseable IPv6

  // IPv4-mapped ::ffff:a.b.c.d (and all alternate spellings)
  if (groupsAreZero(groups, 0, 5) && groups[5] === 0xffff) {
    return ipv4ToBlocked(groupsToIpv4(groups[6], groups[7]))
  }
  // NAT64 64:ff9b::/96 — embedded IPv4 in the final two groups
  if (groups[0] === 0x0064 && groups[1] === 0xff9b && groupsAreZero(groups, 2, 6)) {
    return ipv4ToBlocked(groupsToIpv4(groups[6], groups[7]))
  }
  // 6to4 2002::/16 — embedded IPv4 in the two groups after the 2002 prefix
  if (groups[0] === 0x2002) {
    return ipv4ToBlocked(groupsToIpv4(groups[1], groups[2]))
  }
  // IPv4-compatible ::/96 (high 96 bits zero; :: and ::1 handled above)
  if (groupsAreZero(groups, 0, 6)) {
    return ipv4ToBlocked(groupsToIpv4(groups[6], groups[7]))
  }
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
