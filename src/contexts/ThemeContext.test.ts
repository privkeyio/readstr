import { describe, expect, it } from 'vitest'
import { normalizeReading, DEFAULT_READING, READING_BOUNDS } from './ThemeContext'

describe('normalizeReading', () => {
  it('returns defaults for an empty object', () => {
    expect(normalizeReading({})).toEqual(DEFAULT_READING)
  })

  it('passes a fully valid object through unchanged', () => {
    const valid = {
      scale: 1.2,
      contentFont: 'georgia' as const,
      headingFont: 'playfair' as const,
      lineHeight: 1.6,
      measure: '60rem',
      paraGap: '1.75em',
    }
    expect(normalizeReading(valid)).toEqual(valid)
  })

  it('falls back to default scale for NaN', () => {
    expect(normalizeReading({ scale: NaN }).scale).toBe(DEFAULT_READING.scale)
  })

  it('clamps out-of-range scale to bounds', () => {
    expect(normalizeReading({ scale: 5 }).scale).toBe(READING_BOUNDS.scale[1])
    expect(normalizeReading({ scale: 0.1 }).scale).toBe(READING_BOUNDS.scale[0])
  })

  it('clamps out-of-range lineHeight to bounds', () => {
    expect(normalizeReading({ lineHeight: 9 }).lineHeight).toBe(READING_BOUNDS.lineHeight[1])
    expect(normalizeReading({ lineHeight: 0.5 }).lineHeight).toBe(READING_BOUNDS.lineHeight[0])
  })

  it('rejects unknown measure and paraGap', () => {
    expect(normalizeReading({ measure: 'bogus' }).measure).toBe(DEFAULT_READING.measure)
    expect(normalizeReading({ paraGap: 'bogus' }).paraGap).toBe(DEFAULT_READING.paraGap)
  })

  it('rejects unknown font keys', () => {
    expect(normalizeReading({ contentFont: 'notafont' as never }).contentFont).toBe(
      DEFAULT_READING.contentFont
    )
    expect(normalizeReading({ headingFont: 'notafont' as never }).headingFont).toBe(
      DEFAULT_READING.headingFont
    )
  })

  it('rejects prototype-chain keys as font values', () => {
    expect(normalizeReading({ contentFont: 'constructor' as never }).contentFont).toBe(
      DEFAULT_READING.contentFont
    )
    expect(normalizeReading({ headingFont: 'toString' as never }).headingFont).toBe(
      DEFAULT_READING.headingFont
    )
  })
})
