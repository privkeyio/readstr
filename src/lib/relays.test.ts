import { afterEach, describe, expect, it, vi } from 'vitest'

import { FALLBACK_RELAYS, parseRelayList, resolveDefaultRelays } from './relays'

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
