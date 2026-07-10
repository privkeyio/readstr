'use client'

import { createContext, useContext, useEffect, useRef, useState, useCallback } from 'react'

export type Theme = 'light' | 'dark' | 'newspaper' | 'parchment'

export type FontKey =
  | 'default'
  | 'system'
  | 'inter'
  | 'georgia'
  | 'charter'
  | 'source-serif'
  | 'playfair'

// TODO(readstr-h39): add self-hosted woff2 for Charter / Source Serif / Playfair
// via next/font/local so these options render their true faces instead of the
// Georgia/Times fallbacks below. No CDN — files must be same-origin (CSP).
export const FONT_STACKS: Record<FontKey, string> = {
  default: '',
  system: 'system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif',
  inter: 'var(--font-inter), system-ui, sans-serif',
  georgia: 'Georgia, Cambria, "Times New Roman", Times, serif',
  charter: 'Charter, "Bitstream Charter", Georgia, Cambria, serif',
  'source-serif': '"Source Serif 4", "Source Serif Pro", Georgia, serif',
  playfair: '"Playfair Display", Georgia, "Times New Roman", serif',
}

export const FONT_OPTIONS: { key: FontKey; label: string }[] = [
  { key: 'default', label: 'Theme default' },
  { key: 'system', label: 'System' },
  { key: 'inter', label: 'Inter' },
  { key: 'georgia', label: 'Georgia' },
  { key: 'charter', label: 'Charter' },
  { key: 'source-serif', label: 'Source Serif' },
  { key: 'playfair', label: 'Playfair' },
]

export const MEASURES = ['40rem', '48rem', '60rem', 'none'] as const
export const PARA_GAPS = ['0.75em', '1.25em', '1.75em'] as const

export interface ReadingPrefs {
  scale: number
  contentFont: FontKey
  headingFont: FontKey
  lineHeight: number
  measure: string
  paraGap: string
}

export const READING_BOUNDS = {
  scale: [0.85, 1.4],
  lineHeight: [1.4, 2.0],
} as const

export const DEFAULT_READING: ReadingPrefs = {
  scale: 1,
  contentFont: 'default',
  headingFont: 'default',
  lineHeight: 1.75,
  measure: '48rem',
  paraGap: '1.25em',
}

const clamp = (n: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, n))

export function normalizeReading(p: Partial<ReadingPrefs>): ReadingPrefs {
  const measure =
    typeof p.measure === 'string' && (MEASURES as readonly string[]).includes(p.measure)
      ? p.measure
      : DEFAULT_READING.measure
  const paraGap =
    typeof p.paraGap === 'string' && (PARA_GAPS as readonly string[]).includes(p.paraGap)
      ? p.paraGap
      : DEFAULT_READING.paraGap
  const contentFont =
    typeof p.contentFont === 'string' && Object.prototype.hasOwnProperty.call(FONT_STACKS, p.contentFont)
      ? (p.contentFont as FontKey)
      : DEFAULT_READING.contentFont
  const headingFont =
    typeof p.headingFont === 'string' && Object.prototype.hasOwnProperty.call(FONT_STACKS, p.headingFont)
      ? (p.headingFont as FontKey)
      : DEFAULT_READING.headingFont
  return {
    scale: clamp(
      typeof p.scale === 'number' && Number.isFinite(p.scale) ? p.scale : 1,
      READING_BOUNDS.scale[0],
      READING_BOUNDS.scale[1]
    ),
    lineHeight: clamp(
      typeof p.lineHeight === 'number' && Number.isFinite(p.lineHeight) ? p.lineHeight : 1.75,
      READING_BOUNDS.lineHeight[0],
      READING_BOUNDS.lineHeight[1]
    ),
    measure,
    paraGap,
    contentFont,
    headingFont,
  }
}

interface ThemeContextType {
  theme: Theme
  setTheme: (theme: Theme) => void
  cycleTheme: () => void
  readingPrefs: ReadingPrefs
  setReadingPref: (partial: Partial<ReadingPrefs>) => void
  resetReading: () => void
}

const themeOrder: Theme[] = ['light', 'dark', 'newspaper', 'parchment']

export const themeConfig: Record<Theme, { name: string; icon: string; description: string }> = {
  light: {
    name: 'Light',
    icon: '☀️',
    description: 'Clean and bright',
  },
  dark: {
    name: 'Dark',
    icon: '🌙',
    description: 'Easy on the eyes',
  },
  newspaper: {
    name: 'Newspaper',
    icon: '📰',
    description: 'Classic print style',
  },
  parchment: {
    name: 'Parchment',
    icon: '📜',
    description: 'Warm and vintage',
  },
}

const ThemeContext = createContext<ThemeContextType | undefined>(undefined)

export function ThemeProvider({ children }: { children: React.ReactNode }) {
  const [theme, setThemeState] = useState<Theme>('light')
  const [readingPrefs, setReadingPrefs] = useState<ReadingPrefs>(DEFAULT_READING)
  const readingPrefsRef = useRef<ReadingPrefs>(DEFAULT_READING)
  const [mounted, setMounted] = useState(false)

  const applyTheme = useCallback((newTheme: Theme) => {
    const root = document.documentElement
    // Remove all theme classes
    themeOrder.forEach(t => root.classList.remove(`theme-${t}`))
    // Add new theme class
    root.classList.add(`theme-${newTheme}`)
    // Handle dark mode class for Tailwind
    root.classList.toggle('dark', newTheme === 'dark')
  }, [])

  const applyReading = useCallback((p: ReadingPrefs) => {
    const root = document.documentElement
    root.classList.add('theme-transitioning')
    const setOrClear = (name: string, value: string | number, def: string | number) => {
      if (value === def) root.style.removeProperty(name)
      else root.style.setProperty(name, String(value))
    }
    setOrClear('--reading-scale', p.scale, DEFAULT_READING.scale)
    setOrClear('--reading-line-height', p.lineHeight, DEFAULT_READING.lineHeight)
    setOrClear('--reading-measure', p.measure, DEFAULT_READING.measure)
    setOrClear('--reading-para-gap', p.paraGap, DEFAULT_READING.paraGap)
    if (p.contentFont === 'default') root.style.removeProperty('--content-font')
    else root.style.setProperty('--content-font', FONT_STACKS[p.contentFont])
    if (p.headingFont === 'default') root.style.removeProperty('--heading-font')
    else root.style.setProperty('--heading-font', FONT_STACKS[p.headingFont])
    requestAnimationFrame(() => root.classList.remove('theme-transitioning'))
  }, [])

  /* eslint-disable react-hooks/set-state-in-effect --
     One-time mount initialization that must read browser-only APIs (localStorage,
     matchMedia) unavailable during SSR, so it cannot be hoisted into render. */
  useEffect(() => {
    setMounted(true)
    const savedTheme = localStorage.getItem('theme') as Theme | null
    if (savedTheme && themeOrder.includes(savedTheme)) {
      setThemeState(savedTheme)
      applyTheme(savedTheme)
    } else {
      // Check system preference for dark mode
      const systemTheme = window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
      setThemeState(systemTheme)
      applyTheme(systemTheme)
    }
    try {
      const savedReading = localStorage.getItem('readstr_reading')
      if (savedReading) {
        const parsed = normalizeReading(JSON.parse(savedReading))
        readingPrefsRef.current = parsed
        setReadingPrefs(parsed)
        applyReading(parsed)
      }
    } catch {
      // Corrupt/unavailable localStorage — fall back to defaults.
    }
  }, [applyTheme, applyReading])
  /* eslint-enable react-hooks/set-state-in-effect */

  const setTheme = useCallback((newTheme: Theme) => {
    setThemeState(newTheme)
    localStorage.setItem('theme', newTheme)
    applyTheme(newTheme)
  }, [applyTheme])

  const setReadingPref = useCallback((partial: Partial<ReadingPrefs>) => {
    const next = normalizeReading({ ...readingPrefsRef.current, ...partial })
    readingPrefsRef.current = next
    setReadingPrefs(next)
    try {
      localStorage.setItem('readstr_reading', JSON.stringify(next))
    } catch {
      // Ignore persistence failures (private mode, quota).
    }
    applyReading(next)
  }, [applyReading])

  const resetReading = useCallback(() => {
    readingPrefsRef.current = DEFAULT_READING
    setReadingPrefs(DEFAULT_READING)
    try {
      localStorage.removeItem('readstr_reading')
    } catch {
      // Ignore persistence failures.
    }
    applyReading(DEFAULT_READING)
  }, [applyReading])

  const cycleTheme = useCallback(() => {
    const currentIndex = themeOrder.indexOf(theme)
    const nextIndex = (currentIndex + 1) % themeOrder.length
    setTheme(themeOrder[nextIndex])
  }, [theme, setTheme])

  // Prevent flash of unstyled content
  if (!mounted) {
    return <>{children}</>
  }

  return (
    <ThemeContext.Provider value={{ theme, setTheme, cycleTheme, readingPrefs, setReadingPref, resetReading }}>
      {children}
    </ThemeContext.Provider>
  )
}

export function useTheme() {
  const context = useContext(ThemeContext)
  if (context === undefined) {
    throw new Error('useTheme must be used within a ThemeProvider')
  }
  return context
}
