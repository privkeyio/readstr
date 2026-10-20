import { describe, expect, it } from 'vitest'
import { finalizeEvent, generateSecretKey } from 'nostr-tools'
import { verifyNip98Header } from './nip98'

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
})
