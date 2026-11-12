import { createHash } from 'crypto'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { finalizeEvent, generateSecretKey } from 'nostr-tools'
import { __resetNip98LogBudgetsForTests, verifyNip98Header } from './nip98'

const url = 'http://localhost/api/trpc/feed.getFeeds?batch=1'

const header = (sk: Uint8Array, createdAt: number, extraTags: string[][] = []) => {
  const event = finalizeEvent(
    {
      kind: 27235,
      created_at: createdAt,
      tags: [['u', url], ['method', 'GET'], ...extraTags],
      content: '',
    },
    sk
  )
  return `Nostr ${Buffer.from(JSON.stringify(event)).toString('base64')}`
}

describe('verifyNip98Header replay check', () => {
  afterEach(() => {
    vi.restoreAllMocks()
    vi.useRealTimers()
    __resetNip98LogBudgetsForTests()
  })

  it('rejects a second identical event in the same second', async () => {
    const sk = generateSecretKey()
    const now = Math.floor(Date.now() / 1000)

    expect(await verifyNip98Header(header(sk, now), { url, method: 'GET' })).not.toBeNull()
    expect(await verifyNip98Header(header(sk, now), { url, method: 'GET' })).toBeNull()
  })

  it('accepts same-second requests whose events carry distinct nonces', async () => {
    const sk = generateSecretKey()
    const now = Math.floor(Date.now() / 1000)

    const first = header(sk, now, [['nonce', 'a1']])
    const second = header(sk, now, [['nonce', 'b2']])

    expect(await verifyNip98Header(first, { url, method: 'GET' })).not.toBeNull()
    expect(await verifyNip98Header(second, { url, method: 'GET' })).not.toBeNull()
    expect(await verifyNip98Header(first, { url, method: 'GET' })).toBeNull()
  })

  it('logs which call was replayed, without its query input', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    const sk = generateSecretKey()
    const now = Math.floor(Date.now() / 1000)
    const token = header(sk, now - 3)

    vi.useFakeTimers({ toFake: ['Date'] })
    await verifyNip98Header(token, { url, method: 'GET', userAgent: 'Mozilla/5.0 test' })
    vi.setSystemTime(Date.now() + 2_500)
    await verifyNip98Header(token, { url, method: 'GET', userAgent: 'Mozilla/5.0 test' })

    const call = warn.mock.calls.find(c => String(c[0]).includes('replayed event id'))
    expect(call?.[1]).toMatchObject({
      method: 'GET',
      path: '/api/trpc/feed.getFeeds',
      userAgent: 'Mozilla/5.0 test',
    })
    const detail = call?.[1] as { ageSeconds: number; sinceFirstMs: number }
    expect(detail.ageSeconds).toBeGreaterThanOrEqual(3)
    expect(detail.sinceFirstMs).toBeGreaterThanOrEqual(2_500)
    expect(detail.sinceFirstMs).toBeLessThan(3_000)
    expect(JSON.stringify(call?.[1])).not.toContain('batch=1')
  })

  it('still logs replays after junk headers use up the general log budget', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    for (let i = 0; i < 40; i++) await verifyNip98Header('Nostr garbage', { url, method: 'GET' })
    const token = header(generateSecretKey(), Math.floor(Date.now() / 1000), [['nonce', 'flood']])
    vi.useFakeTimers({ toFake: ['Date'] })
    await verifyNip98Header(token, { url, method: 'GET' })
    vi.setSystemTime(Date.now() + 2_500)
    expect(await verifyNip98Header(token, { url, method: 'GET' })).toBeNull()
    expect(warn.mock.calls.some(c => String(c[0]).includes('replayed event id'))).toBe(true)
  })

  it('logs a few fast GET repeats in full, then only counts them', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    const info = vi.spyOn(console, 'info').mockImplementation(() => {})
    vi.useFakeTimers({ toFake: ['Date'] })
    const token = header(generateSecretKey(), Math.floor(Date.now() / 1000), [['nonce', 'retry']])
    expect(await verifyNip98Header(token, { url, method: 'GET' })).not.toBeNull()
    for (let i = 0; i < 6; i++) expect(await verifyNip98Header(token, { url, method: 'GET' })).toBeNull()
    const replays = () => warn.mock.calls.filter(c => String(c[0]).includes('replayed event id')).length
    expect(replays()).toBe(3)

    // The count is written once the window ends, even if the next log is another kind of rejection.
    vi.setSystemTime(Date.now() + 61_000)
    await verifyNip98Header('Nostr garbage', { url, method: 'GET' })
    expect(info.mock.calls.some(c => String(c[0]).includes('rejected 3 more repeated GET/HEAD request(s)'))).toBe(true)
  })

  it('still logs a POST repeated within 2 s in full', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    const body = '{"a":1}'
    const sk = generateSecretKey()
    const event = finalizeEvent(
      {
        kind: 27235,
        created_at: Math.floor(Date.now() / 1000),
        tags: [
          ['u', url],
          ['method', 'POST'],
          ['payload', createHash('sha256').update(body).digest('hex')],
        ],
        content: '',
      },
      sk
    )
    const token = `Nostr ${Buffer.from(JSON.stringify(event)).toString('base64')}`
    expect(await verifyNip98Header(token, { url, method: 'POST', body })).not.toBeNull()
    expect(await verifyNip98Header(token, { url, method: 'POST', body })).toBeNull()
    expect(warn.mock.calls.some(c => String(c[0]).includes('replayed event id'))).toBe(true)
  })

  it('logs a repeat in full when the clock has gone backwards', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    vi.useFakeTimers({ toFake: ['Date'] })
    // Use up this window's full samples so only the clock check can make it warn.
    const t0 = Date.now()
    const filler = header(generateSecretKey(), Math.floor(t0 / 1000), [['nonce', 'filler']])
    await verifyNip98Header(filler, { url, method: 'GET' })
    for (let i = 0; i < 3; i++) await verifyNip98Header(filler, { url, method: 'GET' })
    warn.mockClear()

    // Accept at t0+10 s, then step back to t0+5 s: the replay looks 5 s older
    // than its first copy, but the clock is still inside the current window.
    vi.setSystemTime(t0 + 10_000)
    const token = header(generateSecretKey(), Math.floor(Date.now() / 1000), [['nonce', 'clock']])
    await verifyNip98Header(token, { url, method: 'GET' })
    vi.setSystemTime(t0 + 5_000)
    expect(await verifyNip98Header(token, { url, method: 'GET' })).toBeNull()
    expect(warn.mock.calls.some(c => String(c[0]).includes('replayed event id'))).toBe(true)
  })

  it('logs only paths on a u tag mismatch', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    const token = header(generateSecretKey(), Math.floor(Date.now() / 1000))
    const other = 'http://localhost/api/trpc/feed.getFeeds?batch=1&input=secret'
    expect(await verifyNip98Header(token, { url: other, method: 'GET' })).toBeNull()
    const call = warn.mock.calls.find(c => String(c[0]).includes('u tag mismatch'))
    expect(call).toBeDefined()
    expect(JSON.stringify(call?.[1])).not.toContain('secret')
  })
})
