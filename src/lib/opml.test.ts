import { describe, it, expect } from 'vitest'
import { parseOpml, buildOpml, planOpmlImport, type OpmlExportFeed, type OpmlFeed } from './opml'

describe('parseOpml', () => {
  it('assigns folder from nested outline', () => {
    const xml = `<opml version="2.0"><body>
      <outline text="News" title="News">
        <outline type="rss" text="Example" title="Example" xmlUrl="https://example.com/feed"/>
      </outline>
    </body></opml>`
    const feeds = parseOpml(xml)
    expect(feeds).toHaveLength(1)
    expect(feeds[0]).toMatchObject({ folder: 'News', xmlUrl: 'https://example.com/feed', title: 'Example' })
  })

  it('distinguishes nostr via type attr', () => {
    const xml = `<opml><body>
      <outline type="nostr" text="Author" nostr="npub1abc"/>
    </body></opml>`
    const feeds = parseOpml(xml)
    expect(feeds[0]!.npub).toBe('npub1abc')
    expect(feeds[0]!.xmlUrl).toBeUndefined()
  })

  it('distinguishes nostr via npub attr without type', () => {
    const xml = `<opml><body>
      <outline text="Author" npub="npub1xyz"/>
    </body></opml>`
    const feeds = parseOpml(xml)
    expect(feeds[0]!.npub).toBe('npub1xyz')
  })

  it('falls back from title to text', () => {
    const xml = `<opml><body>
      <outline type="rss" text="Text Title" xmlUrl="https://a.com/feed"/>
    </body></opml>`
    expect(parseOpml(xml)[0]!.title).toBe('Text Title')
  })

  it('parses comma-separated category into tags', () => {
    const xml = `<opml><body>
      <outline type="rss" text="A" xmlUrl="https://a.com/feed" category="tech, news ,"/>
    </body></opml>`
    expect(parseOpml(xml)[0]!.tags).toEqual(['tech', 'news'])
  })

  it('does not throw on malformed or empty xml', () => {
    expect(parseOpml('')).toEqual([])
    expect(parseOpml('<opml><body><outline></outline></body></opml>')).toEqual([])
    expect(() => parseOpml('<opml><body><outline text=')).not.toThrow()
  })

  it('caps the number of outlines', () => {
    const rows = Array.from({ length: 5100 }, (_, i) => `<outline type="rss" xmlUrl="https://x.com/${i}"/>`).join('')
    const feeds = parseOpml(`<opml><body>${rows}</body></opml>`)
    expect(feeds.length).toBeLessThanOrEqual(5000)
  })
})

describe('buildOpml', () => {
  it('round-trips url, npub, title, folder, and tags', () => {
    const input: OpmlExportFeed[] = [
      { type: 'RSS', url: 'https://a.com/feed', title: 'Feed A', tags: ['tech'], category: { name: 'News' } },
      { type: 'NOSTR', url: 'npub1abc', title: 'Author', tags: ['nostr'] },
    ]
    const parsed = parseOpml(buildOpml(input))

    const rss = parsed.find(f => f.xmlUrl)
    expect(rss).toMatchObject({ xmlUrl: 'https://a.com/feed', title: 'Feed A', folder: 'News', tags: ['tech'] })

    const nostr = parsed.find(f => f.npub)
    expect(nostr).toMatchObject({ npub: 'npub1abc', title: 'Author', tags: ['nostr'] })
  })

  it('escapes special characters in titles', () => {
    const xml = buildOpml([{ type: 'RSS', url: 'https://a.com/feed', title: 'Tom & Jerry <"quote">' }])
    expect(xml).toContain('&amp;')
    expect(xml).toContain('&lt;')
    expect(xml).toContain('&quot;')
    expect(parseOpml(xml)[0]!.title).toBe('Tom & Jerry <"quote">')
  })

  it('skips NOSTR_VIDEO duplicating a NOSTR npub', () => {
    const xml = buildOpml([
      { type: 'NOSTR', url: 'npub1dup', title: 'A' },
      { type: 'NOSTR_VIDEO', url: 'npub1dup', title: 'A video' },
    ])
    expect(parseOpml(xml).filter(f => f.npub === 'npub1dup')).toHaveLength(1)
  })
})

describe('planOpmlImport', () => {
  const feed = (f: Partial<OpmlFeed>): OpmlFeed => ({ tags: [], ...f })

  it('dedupes against existing RSS and npub subscriptions', () => {
    const parsed: OpmlFeed[] = [
      feed({ xmlUrl: 'https://a.com/feed/' }),
      feed({ xmlUrl: 'https://new.com/feed' }),
      feed({ npub: 'npub1abc' }),
      feed({ npub: 'npub1new' }),
    ]
    const existing = [
      { type: 'RSS' as const, url: 'https://a.com/feed' },
      { type: 'NOSTR_VIDEO' as const, url: 'npub1abc' },
    ]
    const { toAdd, skipped } = planOpmlImport(parsed, existing, 'tags')
    expect(skipped).toBe(2)
    expect(toAdd.map(f => f.url).sort()).toEqual(['https://new.com/feed', 'npub1new'])
  })

  it('collapses intra-file duplicates', () => {
    const parsed: OpmlFeed[] = [
      feed({ xmlUrl: 'https://a.com/feed' }),
      feed({ xmlUrl: 'https://a.com/feed/' }),
      feed({ npub: 'npub1abc' }),
      feed({ npub: 'npub1abc' }),
    ]
    const { toAdd, skipped } = planOpmlImport(parsed, [], 'tags')
    expect(toAdd).toHaveLength(2)
    expect(skipped).toBe(2)
  })

  it('maps folder to category in categories mode', () => {
    const parsed: OpmlFeed[] = [feed({ xmlUrl: 'https://a.com/feed', folder: 'News' })]
    const { toAdd } = planOpmlImport(parsed, [], 'categories')
    expect(toAdd[0]!.category).toEqual({ name: 'News' })
    expect(toAdd[0]!.tags).toBeUndefined()
  })

  it('maps folder to tag in tags mode', () => {
    const parsed: OpmlFeed[] = [feed({ xmlUrl: 'https://a.com/feed', folder: 'News' })]
    const { toAdd } = planOpmlImport(parsed, [], 'tags')
    expect(toAdd[0]!.category).toBeUndefined()
    expect(toAdd[0]!.tags).toEqual(['News'])
  })

  it('carries category-attr tags through to the plan', () => {
    const parsed: OpmlFeed[] = [feed({ xmlUrl: 'https://a.com/feed', tags: ['tech', 'news'] })]
    const { toAdd } = planOpmlImport(parsed, [], 'tags')
    expect(toAdd[0]!.tags).toEqual(['tech', 'news'])
  })
})
