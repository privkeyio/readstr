import { describe, expect, it } from 'vitest'
import { countTrpcCalls } from './trpc-batch'

const count = (path: string) => countTrpcCalls(`http://localhost/api/trpc/${path}`, '/api/trpc')

describe('countTrpcCalls', () => {
  it('counts a single call', () => {
    expect(count('guide.getGuideTags')).toBe(1)
  })

  it('counts each call in a batch', () => {
    expect(count('guide.getGuideTags,feed.getFeeds,feed.getFeeds?batch=1')).toBe(3)
  })

  it('counts percent-encoded commas the way tRPC decodes them', () => {
    expect(count('guide.getGuideTags%2Cguide.getGuideTags%2cguide.getGuideTags')).toBe(3)
  })

  it('returns null for invalid percent-encoding', () => {
    expect(count('guide.getGuideTags%E0%A4%A')).toBeNull()
  })
})
