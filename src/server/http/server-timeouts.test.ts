import { createRequire } from 'node:module'
import { describe, expect, it, vi } from 'vitest'

const require = createRequire(import.meta.url)
vi.spyOn(console, 'info').mockImplementation(() => {})
const { resolveTimeouts, parseTimeoutMs } = require('../../../server-timeouts.cjs') as {
  resolveTimeouts: (env: Record<string, string | undefined>) => { requestTimeout: number; headersTimeout: number }
  parseTimeoutMs: (raw: string | undefined, fallback: number, name: string) => number
}

describe('server-timeouts preload', () => {
  it('defaults to 30 s per request and 15 s for headers', () => {
    expect(resolveTimeouts({})).toEqual({ requestTimeout: 30_000, headersTimeout: 15_000 })
  })

  it('reads positive integer overrides from the environment', () => {
    expect(resolveTimeouts({ REQUEST_TIMEOUT_MS: '3000', HEADERS_TIMEOUT_MS: ' 2000 ' })).toEqual({
      requestTimeout: 3000,
      headersTimeout: 2000,
    })
  })

  it('falls back to the default for anything but an integer from 1 s to 1 h', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    const rejected = [
      '0',
      '1',
      '999',
      '-5',
      '1.5',
      '1e4',
      'abc',
      '0x10',
      '3600001',
      '4294967296',
      '4294970296',
      '99999999999999999999',
    ]
    for (const raw of rejected) expect(parseTimeoutMs(raw, 123, 'X')).toBe(123)
    expect(parseTimeoutMs('', 123, 'X')).toBe(123)
    expect(parseTimeoutMs(undefined, 123, 'X')).toBe(123)
    expect(warn).toHaveBeenCalledTimes(rejected.length)
    expect(parseTimeoutMs('1000', 123, 'X')).toBe(1000)
    expect(parseTimeoutMs('3600000', 123, 'X')).toBe(3_600_000)
    warn.mockRestore()
  })

  it('keeps headersTimeout at or below requestTimeout', () => {
    expect(resolveTimeouts({ REQUEST_TIMEOUT_MS: '5000' })).toEqual({ requestTimeout: 5000, headersTimeout: 5000 })
    expect(resolveTimeouts({ HEADERS_TIMEOUT_MS: '60000' })).toEqual({ requestTimeout: 30_000, headersTimeout: 30_000 })
  })

  it('applies the timeouts to servers created after it loads', async () => {
    const http = await import('node:http')
    const server = http.createServer()
    expect(server.requestTimeout).toBe(30_000)
    expect(server.headersTimeout).toBe(15_000)
    expect((server as { connectionsCheckingInterval?: number }).connectionsCheckingInterval).toBe(1_000)
  })
})
