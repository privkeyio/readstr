import { describe, it, expect, beforeEach } from 'vitest'
import {
  evaluateRules,
  loadFilterRules,
  saveFilterRules,
  type FilterRule,
} from './keyword-filter'

function rule(overrides: Partial<FilterRule>): FilterRule {
  return {
    id: overrides.id ?? 'r',
    enabled: overrides.enabled ?? true,
    order: overrides.order ?? 0,
    target: overrides.target ?? 'any',
    type: overrides.type ?? 'contains',
    pattern: overrides.pattern ?? '',
    caseSensitive: overrides.caseSensitive ?? false,
    action: overrides.action ?? 'hide',
    color: overrides.color,
  }
}

describe('evaluateRules', () => {
  it('contains matches substrings', () => {
    const r = rule({ type: 'contains', pattern: 'cat', target: 'title', action: 'hide' })
    expect(evaluateRules({ title: 'category theory' }, [r]).hidden).toBe(true)
  })

  it('word does not match substrings', () => {
    const r = rule({ type: 'word', pattern: 'cat', target: 'title', action: 'hide' })
    expect(evaluateRules({ title: 'category theory' }, [r]).hidden).toBe(false)
    expect(evaluateRules({ title: 'a cat sat' }, [r]).hidden).toBe(true)
  })

  it('respects case sensitivity', () => {
    const sensitive = rule({ pattern: 'Cat', caseSensitive: true, target: 'title' })
    expect(evaluateRules({ title: 'a cat' }, [sensitive]).hidden).toBe(false)
    expect(evaluateRules({ title: 'a Cat' }, [sensitive]).hidden).toBe(true)
    const insensitive = rule({ pattern: 'Cat', caseSensitive: false, target: 'title' })
    expect(evaluateRules({ title: 'a cat' }, [insensitive]).hidden).toBe(true)
  })

  it('selects the correct target', () => {
    const titleRule = rule({ pattern: 'spam', target: 'title' })
    expect(evaluateRules({ title: 'spam', content: 'clean', author: 'bob' }, [titleRule]).hidden).toBe(true)
    const authorRule = rule({ pattern: 'bob', target: 'author' })
    expect(evaluateRules({ title: 'x', content: 'y', author: 'bob' }, [authorRule]).hidden).toBe(true)
    expect(evaluateRules({ title: 'bob', content: 'y', author: 'z' }, [authorRule]).hidden).toBe(false)
    const anyRule = rule({ pattern: 'clean', target: 'any' })
    expect(evaluateRules({ title: 'x', content: 'clean stuff', author: 'z' }, [anyRule]).hidden).toBe(true)
  })

  it('strips HTML when matching content', () => {
    const r = rule({ pattern: 'hello', target: 'content', type: 'word' })
    expect(evaluateRules({ content: '<p>hello</p> world' }, [r]).hidden).toBe(true)
    const tagRule = rule({ pattern: 'p', target: 'content', type: 'word' })
    expect(evaluateRules({ content: '<p>hello</p>' }, [tagRule]).hidden).toBe(false)
  })

  it('first hide rule wins and short-circuits', () => {
    const highlight = rule({ id: 'h', order: 0, pattern: 'x', action: 'highlight', color: '#fff', target: 'title' })
    const hide = rule({ id: 'd', order: 1, pattern: 'x', action: 'hide', target: 'title' })
    const out = evaluateRules({ title: 'x' }, [highlight, hide])
    expect(out.hidden).toBe(true)
    expect(out.highlight).toBeUndefined()
  })

  it('accumulates highlight, last wins', () => {
    const a = rule({ id: 'a', order: 0, pattern: 'x', action: 'highlight', color: '#111', target: 'title' })
    const b = rule({ id: 'b', order: 1, pattern: 'x', action: 'highlight', color: '#222', target: 'title' })
    const out = evaluateRules({ title: 'x' }, [a, b])
    expect(out.hidden).toBe(false)
    expect(out.highlight).toBe('#222')
  })

  it('ignores disabled rules', () => {
    const r = rule({ pattern: 'x', enabled: false, target: 'title' })
    expect(evaluateRules({ title: 'x' }, [r]).hidden).toBe(false)
  })

  it('respects order for hide precedence', () => {
    const later = rule({ id: 'l', order: 5, pattern: 'x', action: 'hide', target: 'title' })
    const earlierHighlight = rule({ id: 'e', order: 1, pattern: 'x', action: 'highlight', color: '#333', target: 'title' })
    const out = evaluateRules({ title: 'x' }, [later, earlierHighlight])
    expect(out.hidden).toBe(true)
  })
})

describe('load/saveFilterRules', () => {
  beforeEach(() => {
    const store = new Map<string, string>()
    globalThis.window = {
      localStorage: {
        getItem: (k: string) => (store.has(k) ? store.get(k)! : null),
        setItem: (k: string, v: string) => void store.set(k, v),
        removeItem: (k: string) => void store.delete(k),
      },
    } as unknown as Window & typeof globalThis
  })

  it('returns [] for missing storage', () => {
    expect(loadFilterRules()).toEqual([])
  })

  it('returns [] for corrupt JSON', () => {
    window.localStorage.setItem('readstr_filters', '{not json')
    expect(loadFilterRules()).toEqual([])
  })

  it('rejects non-array payloads', () => {
    window.localStorage.setItem('readstr_filters', '{"foo":1}')
    expect(loadFilterRules()).toEqual([])
  })

  it('drops rules without a pattern and caps oversized patterns', () => {
    window.localStorage.setItem(
      'readstr_filters',
      JSON.stringify([
        { id: '1', pattern: '' },
        { id: '2', pattern: 'x'.repeat(500), target: 'title', type: 'contains', action: 'hide' },
      ])
    )
    const rules = loadFilterRules()
    expect(rules).toHaveLength(1)
    expect(rules[0]!.pattern.length).toBe(200)
  })

  it('caps rule count at 200', () => {
    const many = Array.from({ length: 300 }, (_, i) => ({ id: String(i), pattern: 'p' }))
    window.localStorage.setItem('readstr_filters', JSON.stringify(many))
    expect(loadFilterRules()).toHaveLength(200)
  })

  it('round-trips valid rules', () => {
    const r = rule({ id: 'x', pattern: 'spam', action: 'highlight', color: '#abc', target: 'title' })
    saveFilterRules([r])
    const loaded = loadFilterRules()
    expect(loaded).toHaveLength(1)
    expect(loaded[0]!.pattern).toBe('spam')
    expect(loaded[0]!.color).toBe('#abc')
  })
})
