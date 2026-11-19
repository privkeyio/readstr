import { describe, expect, it, vi } from 'vitest'

// Stand in for a nostr-tools that accepted any signature, so this checks the
// pubkey guard on its own.
vi.mock('nostr-tools', async importOriginal => ({
  ...(await importOriginal<typeof import('nostr-tools')>()),
  verifyEvent: () => true,
}))

const { verifyNip98Header } = await import('./nip98')

const url = 'http://localhost/api/trpc/feed.getFeeds?batch=1'

function header(pubkey: unknown, id: string) {
  const event = {
    id,
    pubkey,
    kind: 27235,
    created_at: Math.floor(Date.now() / 1000),
    tags: [['u', url], ['method', 'GET']],
    content: '',
    sig: '00'.repeat(64),
  }
  return `Nostr ${Buffer.from(JSON.stringify(event)).toString('base64')}`
}

describe('verifyNip98Header pubkey guard', () => {
  it('returns only a lowercase 64-char hex pubkey', async () => {
    vi.spyOn(console, 'warn').mockImplementation(() => {})
    const good = 'ab'.repeat(32)
    expect(await verifyNip98Header(header(good, '01'.repeat(32)), { url, method: 'GET' })).toBe(good)
    const bad = ['AB'.repeat(32), 'ab'.repeat(31), 'ab'.repeat(33), 'zz'.repeat(32), ` ${'ab'.repeat(32)}`, 42, null]
    for (const [i, pubkey] of bad.entries()) {
      expect(await verifyNip98Header(header(pubkey, String(i).padStart(64, '2')), { url, method: 'GET' }), String(pubkey)).toBeNull()
    }
  })
})
