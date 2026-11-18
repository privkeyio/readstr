import { describe, expect, it } from 'vitest'
import { isBlockedAddress } from './safe-fetch'

describe('isBlockedAddress', () => {
  it.each(['fec0::1', 'FEC0::1', 'fed0::1', 'feff:ffff::1', '2001::1', '2001:0:4136:e378:8000:63bf:3fff:fdd2'])(
    'blocks %s',
    (ip) => {
      expect(isBlockedAddress(ip)).toBe(true)
    },
  )

  it.each(['2001:4860:4860::8888', '2606:4700:4700::1111', '2001:1::1', '93.184.216.34'])(
    'allows %s',
    (ip) => {
      expect(isBlockedAddress(ip)).toBe(false)
    },
  )
})
