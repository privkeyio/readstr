import { describe, it, expect } from 'vitest'
import {
  reorderViews,
  normalizeViews,
  sanitizeView,
  matchesView,
  type SavedView,
} from './saved-views'

function view(overrides: Partial<SavedView>): SavedView {
  return {
    id: overrides.id ?? 'v',
    name: overrides.name ?? 'View',
    order: overrides.order ?? 0,
    source: overrides.source ?? { kind: 'all' },
    readState: overrides.readState ?? 'all',
    sort: overrides.sort ?? 'newest',
    icon: overrides.icon,
    keywords: overrides.keywords,
    organizationMode: overrides.organizationMode,
  }
}

describe('reorderViews', () => {
  const views = [
    view({ id: 'a', order: 0 }),
    view({ id: 'b', order: 1 }),
    view({ id: 'c', order: 2 }),
  ]

  it('moves an element up and reindexes order', () => {
    const next = reorderViews(views, 2, -1)
    expect(next.map((v) => v.id)).toEqual(['a', 'c', 'b'])
    expect(next.map((v) => v.order)).toEqual([0, 1, 2])
  })

  it('moves an element down and reindexes order', () => {
    const next = reorderViews(views, 0, 1)
    expect(next.map((v) => v.id)).toEqual(['b', 'a', 'c'])
    expect(next.map((v) => v.order)).toEqual([0, 1, 2])
  })

  it('is a no-op at the top boundary', () => {
    expect(reorderViews(views, 0, -1)).toBe(views)
  })

  it('is a no-op at the bottom boundary', () => {
    expect(reorderViews(views, 2, 1)).toBe(views)
  })

  it('is a no-op for out-of-range index', () => {
    expect(reorderViews(views, 5, -1)).toBe(views)
    expect(reorderViews(views, -1, 1)).toBe(views)
  })

  it('survives a normalizeViews round-trip', () => {
    const moved = reorderViews(views, 0, 1)
    const normalized = normalizeViews(moved)
    expect(normalized.map((v) => v.id)).toEqual(['b', 'a', 'c'])
    expect(normalized.map((v) => v.order)).toEqual([0, 1, 2])
  })
})

describe('sanitizeView', () => {
  it('rejects non-object input', () => {
    expect(sanitizeView(null)).toBeNull()
    expect(sanitizeView(42)).toBeNull()
    expect(sanitizeView('nope')).toBeNull()
  })

  it('rejects an empty or whitespace name', () => {
    expect(sanitizeView({ name: '' })).toBeNull()
    expect(sanitizeView({ name: '   ' })).toBeNull()
  })

  it('caps the name length', () => {
    const v = sanitizeView({ name: 'x'.repeat(200) })
    expect(v!.name.length).toBe(60)
  })

  it('caps the icon length', () => {
    const v = sanitizeView({ name: 'V', icon: 'y'.repeat(20) })
    expect(v!.icon!.length).toBe(8)
  })

  it('defaults invalid readState and sort', () => {
    const v = sanitizeView({ name: 'V', readState: 'bogus', sort: 'sideways' })
    expect(v!.readState).toBe('all')
    expect(v!.sort).toBe('newest')
  })

  it('coerces a bad source kind to all', () => {
    expect(sanitizeView({ name: 'V', source: { kind: 'nope' } })!.source).toEqual({ kind: 'all' })
    expect(sanitizeView({ name: 'V', source: 'x' })!.source).toEqual({ kind: 'all' })
  })

  it('coerces a feed source with a missing id to all', () => {
    expect(sanitizeView({ name: 'V', source: { kind: 'feed' } })!.source).toEqual({ kind: 'all' })
    expect(sanitizeView({ name: 'V', source: { kind: 'feed', feedId: 'f1' } })!.source).toEqual({
      kind: 'feed',
      feedId: 'f1',
    })
  })

  it('coerces a category source with a missing id to all', () => {
    expect(sanitizeView({ name: 'V', source: { kind: 'category' } })!.source).toEqual({ kind: 'all' })
  })

  it('drops non-string tags and empties to all', () => {
    expect(sanitizeView({ name: 'V', source: { kind: 'tags', tags: [1, 2] } })!.source).toEqual({
      kind: 'all',
    })
    expect(sanitizeView({ name: 'V', source: { kind: 'tags', tags: ['a', 3, ''] } })!.source).toEqual({
      kind: 'tags',
      tags: ['a'],
    })
  })

  it('caps keyword length and count', () => {
    const v = sanitizeView({
      name: 'V',
      keywords: {
        include: ['k'.repeat(500), ...Array.from({ length: 80 }, (_, i) => `w${i}`)],
      },
    })
    expect(v!.keywords!.include!.length).toBe(50)
    expect(v!.keywords!.include![0]!.length).toBe(200)
  })

  it('validates organizationMode', () => {
    expect(sanitizeView({ name: 'V', organizationMode: 'tags' })!.organizationMode).toBe('tags')
    expect(sanitizeView({ name: 'V', organizationMode: 'categories' })!.organizationMode).toBe(
      'categories'
    )
    expect(sanitizeView({ name: 'V', organizationMode: 'bogus' })!.organizationMode).toBeUndefined()
    expect(sanitizeView({ name: 'V' })!.organizationMode).toBeUndefined()
  })
})

describe('normalizeViews', () => {
  it('returns [] for non-array input', () => {
    expect(normalizeViews(null)).toEqual([])
    expect(normalizeViews({ foo: 1 })).toEqual([])
  })

  it('drops malformed entries and sorts by order then reindexes', () => {
    const out = normalizeViews([
      { name: 'B', order: 5 },
      null,
      { name: '', order: 0 },
      { name: 'A', order: 1 },
    ])
    expect(out.map((v) => v.name)).toEqual(['A', 'B'])
    expect(out.map((v) => v.order)).toEqual([0, 1])
  })

  it('enforces MAX_VIEWS', () => {
    const many = Array.from({ length: 80 }, (_, i) => ({ name: `V${i}` }))
    expect(normalizeViews(many)).toHaveLength(50)
  })
})

describe('matchesView', () => {
  const item = { title: 'The quick brown fox', content: 'lazy dog', author: 'alice' }

  it('passes through with no keywords', () => {
    expect(matchesView(item, view({}))).toBe(true)
    expect(matchesView(item, view({ keywords: {} }))).toBe(true)
  })

  it('requires at least one include match', () => {
    expect(matchesView(item, view({ keywords: { include: ['fox'] } }))).toBe(true)
    expect(matchesView(item, view({ keywords: { include: ['cat', 'dog'] } }))).toBe(true)
    expect(matchesView(item, view({ keywords: { include: ['zebra'] } }))).toBe(false)
  })

  it('rejects when any exclude matches', () => {
    expect(matchesView(item, view({ keywords: { exclude: ['zebra'] } }))).toBe(true)
    expect(matchesView(item, view({ keywords: { exclude: ['dog'] } }))).toBe(false)
  })

  it('applies exclude precedence over include', () => {
    expect(
      matchesView(item, view({ keywords: { include: ['fox'], exclude: ['dog'] } }))
    ).toBe(false)
    expect(
      matchesView(item, view({ keywords: { include: ['fox'], exclude: ['zebra'] } }))
    ).toBe(true)
  })

  it('is case-insensitive', () => {
    expect(matchesView(item, view({ keywords: { include: ['FOX'] } }))).toBe(true)
  })
})
