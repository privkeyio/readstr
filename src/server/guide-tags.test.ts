import { beforeEach, describe, expect, it, vi } from 'vitest'
import { getGuideTagsCached, GUIDE_TAGS_TTL_MS, invalidateGuideTags } from './guide-tags'

function fakeDb(rows: { tags: string[] }[] | Error) {
  const findMany = vi.fn(async () => {
    if (rows instanceof Error) throw rows
    return rows
  })
  return { db: { guideFeed: { findMany } } as never, findMany }
}

describe('getGuideTagsCached', () => {
  beforeEach(() => invalidateGuideTags())

  it('counts tags across feeds, most used first', async () => {
    const { db } = fakeDb([{ tags: ['tech', 'btc'] }, { tags: ['btc'] }])
    expect(await getGuideTagsCached(db, 0)).toEqual([
      { tag: 'btc', count: 2 },
      { tag: 'tech', count: 1 },
    ])
  })

  it('queries once per TTL, sharing the in-flight query', async () => {
    const { db, findMany } = fakeDb([{ tags: ['a'] }])
    await Promise.all(Array.from({ length: 200 }, () => getGuideTagsCached(db, 0)))
    await getGuideTagsCached(db, GUIDE_TAGS_TTL_MS - 1)
    expect(findMany).toHaveBeenCalledTimes(1)
    await getGuideTagsCached(db, GUIDE_TAGS_TTL_MS)
    expect(findMany).toHaveBeenCalledTimes(2)
  })

  it('recomputes after invalidation', async () => {
    const { db, findMany } = fakeDb([{ tags: ['a'] }])
    await getGuideTagsCached(db, 0)
    invalidateGuideTags()
    await getGuideTagsCached(db, 1)
    expect(findMany).toHaveBeenCalledTimes(2)
  })

  it('does not cache a failed query', async () => {
    const failing = fakeDb(new Error('db down'))
    await expect(getGuideTagsCached(failing.db, 0)).rejects.toThrow('db down')
    const { db, findMany } = fakeDb([{ tags: ['a'] }])
    expect(await getGuideTagsCached(db, 1)).toEqual([{ tag: 'a', count: 1 }])
    expect(findMany).toHaveBeenCalledTimes(1)
  })
})
