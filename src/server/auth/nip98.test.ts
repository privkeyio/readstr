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

    await verifyNip98Header(token, { url, method: 'GET', userAgent: 'Mozilla/5.0 test' })
    await verifyNip98Header(token, { url, method: 'GET', userAgent: 'Mozilla/5.0 test' })

    const call = warn.mock.calls.find(c => String(c[0]).includes('replayed event id'))
    expect(call?.[1]).toMatchObject({
      method: 'GET',
      path: '/api/trpc/feed.getFeeds',
      userAgent: 'Mozilla/5.0 test',
    })
    const detail = call?.[1] as { ageSeconds: number; sinceFirstMs: number }
    expect(detail.ageSeconds).toBeGreaterThanOrEqual(3)
    expect(detail.sinceFirstMs).toBeGreaterThanOrEqual(0)
    expect(detail.sinceFirstMs).toBeLessThan(3000)
    expect(JSON.stringify(call?.[1])).not.toContain('batch=1')
  })

  it('still logs replays after junk headers use up the general log budget', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    for (let i = 0; i < 40; i++) await verifyNip98Header('Nostr garbage', { url, method: 'GET' })
    const token = header(generateSecretKey(), Math.floor(Date.now() / 1000), [['nonce', 'flood']])
    await verifyNip98Header(token, { url, method: 'GET' })
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
