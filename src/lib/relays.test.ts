import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { afterEach, describe, expect, it, vi } from 'vitest'

import {
  clampRelaysForRequest,
  FALLBACK_RELAYS,
  MAX_RELAY_URL_LENGTH,
  MAX_CLIENT_RELAYS,
  parseRelayList,
  pickRelays,
  resolveDefaultRelays,
} from './relays'

describe('parseRelayList', () => {
  it('returns [] when unset or empty', () => {
    expect(parseRelayList(undefined)).toEqual([])
    expect(parseRelayList(null)).toEqual([])
    expect(parseRelayList('')).toEqual([])
    expect(parseRelayList(' , ,  ')).toEqual([])
  })

  it('parses a comma- or whitespace-separated list, trimming and deduping', () => {
    expect(parseRelayList(' wss://a.example , wss://b.example\nws://c.example,wss://a.example ')).toEqual([
      'wss://a.example',
      'wss://b.example',
      'ws://c.example',
    ])
  })

  it('drops entries that are not ws:// or wss:// URLs', () => {
    expect(parseRelayList('https://a.example,not a url,wss://ok.example,relay.example')).toEqual([
      'wss://ok.example',
    ])
  })

  it('drops entries that carry credentials', () => {
    expect(
      parseRelayList('wss://user:pass@a.example,wss://user@b.example,wss://:secret@c.example,wss://ok.example'),
    ).toEqual(['wss://ok.example'])
  })

  it('drops entries containing control characters', () => {
    expect(
      parseRelayList('wss://a\u0000.example,wss://b.example\u0007,wss://c\u007f.example,wss://d\u0085.example,wss://ok.example'),
    ).toEqual(['wss://ok.example'])
  })

  it('keeps the raw entry instead of the normalized href', () => {
    expect(parseRelayList('WSS://relay.damus.io')).toEqual(['WSS://relay.damus.io'])
    expect(parseRelayList('wss://relay.damus.io')).toEqual(['wss://relay.damus.io'])
  })
})

describe('pickRelays', () => {
  const defaults = ['wss://default.example']

  it('prefers the saved list', () => {
    expect(pickRelays('["wss://saved.example"]', defaults)).toEqual(['wss://saved.example'])
  })

  it('falls back to defaults when nothing is saved', () => {
    expect(pickRelays(null, defaults)).toEqual(defaults)
  })

  it('falls back to defaults on bad JSON', () => {
    expect(pickRelays('{not json', defaults)).toEqual(defaults)
  })

  it('falls back to defaults on an empty array or a non-array', () => {
    expect(pickRelays('[]', defaults)).toEqual(defaults)
    expect(pickRelays('{"a":1}', defaults)).toEqual(defaults)
  })

  it('falls back to defaults when entries are not strings', () => {
    expect(pickRelays('["wss://ok.example", 42]', defaults)).toEqual(defaults)
    expect(pickRelays('[null]', defaults)).toEqual(defaults)
  })

  it('returns a copy of the defaults', () => {
    const result = pickRelays(null, defaults)
    result.push('wss://mutated.example')
    expect(defaults).toEqual(['wss://default.example'])
  })
})

describe('resolveDefaultRelays', () => {
  it('falls back to the built-in list when unset', () => {
    expect(resolveDefaultRelays(undefined)).toEqual(FALLBACK_RELAYS)
  })

  it('falls back to the built-in list when empty or all entries are invalid', () => {
    expect(resolveDefaultRelays('')).toEqual(FALLBACK_RELAYS)
    expect(resolveDefaultRelays('https://nope.example')).toEqual(FALLBACK_RELAYS)
  })

  it('uses the operator list when it has valid entries', () => {
    expect(resolveDefaultRelays('wss://one.example,wss://two.example')).toEqual([
      'wss://one.example',
      'wss://two.example',
    ])
  })

  it('returns a copy so callers cannot mutate the fallback', () => {
    const relays = resolveDefaultRelays(undefined)
    relays.push('wss://mutated.example')
    expect(FALLBACK_RELAYS).not.toContain('wss://mutated.example')
  })
})

describe('dead relays', () => {
  const DEAD = ['wss://relay.nostr.band', 'wss://nostr-pub.wellorder.net']

  it('are not in the fallback list', () => {
    expect(FALLBACK_RELAYS).toEqual([
      'wss://relay.damus.io',
      'wss://relay.primal.net',
      'wss://nos.lol',
      'wss://relay.snort.social',
    ])
  })

  it('are dropped from a list saved before they were removed', () => {
    const saved = JSON.stringify(['wss://relay.nostr.band', 'wss://ok.example', 'WSS://nostr-pub.wellorder.net/'])
    expect(pickRelays(saved, ['wss://default.example'])).toEqual(['wss://ok.example'])
    expect(pickRelays(JSON.stringify(DEAD), ['wss://default.example'])).toEqual(['wss://default.example'])
  })

  it('are not suggested in settings or used for sharing', () => {
    for (const file of ['../components/settings-dialog.tsx', '../components/feed-reader.tsx']) {
      const source = readFileSync(fileURLToPath(new URL(file, import.meta.url)), 'utf8')
      for (const relay of DEAD) expect(source, file).not.toContain(relay)
    }
  })
})

describe('getDefaultRelays', () => {
  afterEach(() => {
    vi.unstubAllEnvs()
    vi.unstubAllGlobals()
    vi.resetModules()
  })

  it('reads DEFAULT_RELAYS from the server environment', async () => {
    vi.stubEnv('DEFAULT_RELAYS', 'wss://operator.example')
    vi.resetModules()
    const { getDefaultRelays } = await import('./relays')
    expect(getDefaultRelays()).toEqual(['wss://operator.example'])
  })

  it('uses the fallback on the server when DEFAULT_RELAYS is unset', async () => {
    vi.stubEnv('DEFAULT_RELAYS', undefined)
    vi.resetModules()
    const { getDefaultRelays } = await import('./relays')
    expect(getDefaultRelays()).toEqual(FALLBACK_RELAYS)
  })

  it('uses the runtime-provided list in the browser, else the fallback', async () => {
    vi.resetModules()
    const { getDefaultRelays, setRuntimeDefaultRelays } = await import('./relays')
    vi.stubGlobal('window', {})
    expect(getDefaultRelays()).toEqual(FALLBACK_RELAYS)
    setRuntimeDefaultRelays(['wss://operator.example'])
    expect(getDefaultRelays()).toEqual(['wss://operator.example'])
    setRuntimeDefaultRelays([])
    expect(getDefaultRelays()).toEqual(FALLBACK_RELAYS)
  })
})

describe('clampRelaysForRequest', () => {
  it('drops over-long entries before applying the count cap', () => {
    const long = 'wss://' + 'a'.repeat(MAX_RELAY_URL_LENGTH)
    const exact = 'wss://' + 'b'.repeat(MAX_RELAY_URL_LENGTH - 6)
    const valid = Array.from({ length: MAX_CLIENT_RELAYS + 5 }, (_, i) => `wss://r${i}.example`)
    const result = clampRelaysForRequest([long, exact, ...valid])
    expect(result).toHaveLength(MAX_CLIENT_RELAYS)
    expect(result[0]).toBe(exact)
    expect(result).not.toContain(long)
    expect(result.slice(1)).toEqual(valid.slice(0, MAX_CLIENT_RELAYS - 1))
  })

  it('returns [] when every entry is too long', () => {
    expect(clampRelaysForRequest(['wss://' + 'x'.repeat(MAX_RELAY_URL_LENGTH)])).toEqual([])
  })
})
