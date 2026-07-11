'use client'

import { createContext, useCallback, useContext, useEffect, useState } from 'react'

export interface AiFeatureToggles {
  summarize: boolean
  insights: boolean
  translate: boolean
}

export type AiProvider = 'endpoint' | 'on-device'

export interface AiConfig {
  enabled: boolean
  provider: AiProvider
  baseUrl: string
  apiKey: string
  model: string
  deviceModel: string
  features: AiFeatureToggles
  targetLang: string
}

export const AI_CONFIG_KEY = 'readstr_ai_config'

export const ON_DEVICE_MODELS: { id: string; label: string; sizeLabel: string }[] = [
  { id: 'Qwen2.5-0.5B-Instruct-q4f16_1-MLC', label: 'Qwen2.5 0.5B', sizeLabel: '~0.5 GB' },
  { id: 'Llama-3.2-1B-Instruct-q4f16_1-MLC', label: 'Llama 3.2 1B', sizeLabel: '~0.9 GB' },
  { id: 'Llama-3.2-3B-Instruct-q4f16_1-MLC', label: 'Llama 3.2 3B', sizeLabel: '~1.9 GB' },
]

const ON_DEVICE_MODEL_IDS = new Set(ON_DEVICE_MODELS.map((m) => m.id))
const DEFAULT_DEVICE_MODEL = ON_DEVICE_MODELS[0].id

export const DEFAULT_AI_CONFIG: AiConfig = {
  enabled: false,
  provider: 'endpoint',
  baseUrl: 'http://localhost:11434/v1',
  apiKey: '',
  model: 'llama3.2',
  deviceModel: DEFAULT_DEVICE_MODEL,
  features: {
    summarize: true,
    insights: true,
    translate: true,
  },
  targetLang: 'auto',
}

export const AI_LANG_OPTIONS: { label: string; value: string }[] = [
  { label: 'Original', value: 'auto' },
  { label: 'English', value: 'English' },
  { label: 'Spanish', value: 'Spanish' },
  { label: 'French', value: 'French' },
  { label: 'German', value: 'German' },
  { label: 'Portuguese', value: 'Portuguese' },
  { label: 'Japanese', value: 'Japanese' },
  { label: 'Chinese', value: 'Chinese' },
]

export function normalizeAiConfig(input: Partial<AiConfig> | null | undefined): AiConfig {
  const p = input ?? {}
  const features = (p.features ?? {}) as Partial<AiFeatureToggles>
  return {
    enabled: p.enabled === true,
    provider: p.provider === 'on-device' ? 'on-device' : 'endpoint',
    baseUrl:
      typeof p.baseUrl === 'string' && p.baseUrl.trim()
        ? p.baseUrl.trim().replace(/\/+$/, '')
        : DEFAULT_AI_CONFIG.baseUrl,
    apiKey: typeof p.apiKey === 'string' ? p.apiKey : DEFAULT_AI_CONFIG.apiKey,
    model:
      typeof p.model === 'string' && p.model.trim()
        ? p.model.trim()
        : DEFAULT_AI_CONFIG.model,
    deviceModel:
      typeof p.deviceModel === 'string' && ON_DEVICE_MODEL_IDS.has(p.deviceModel)
        ? p.deviceModel
        : DEFAULT_DEVICE_MODEL,
    features: {
      summarize: features.summarize !== false,
      insights: features.insights !== false,
      translate: features.translate !== false,
    },
    targetLang:
      typeof p.targetLang === 'string' && p.targetLang.trim()
        ? p.targetLang.trim()
        : DEFAULT_AI_CONFIG.targetLang,
  }
}

export function loadAiConfig(): AiConfig {
  if (typeof window === 'undefined') return DEFAULT_AI_CONFIG
  try {
    const raw = localStorage.getItem(AI_CONFIG_KEY)
    if (!raw) return DEFAULT_AI_CONFIG
    return normalizeAiConfig(JSON.parse(raw))
  } catch {
    return DEFAULT_AI_CONFIG
  }
}

export function saveAiConfig(config: AiConfig): void {
  if (typeof window === 'undefined') return
  try {
    localStorage.setItem(AI_CONFIG_KEY, JSON.stringify(normalizeAiConfig(config)))
  } catch {
    // Ignore persistence failures (private mode, quota).
  }
}

interface AiConfigContextType {
  config: AiConfig
  setConfig: (partial: Partial<AiConfig>) => void
}

const AiConfigContext = createContext<AiConfigContextType | undefined>(undefined)

export function AiConfigProvider({ children }: { children: React.ReactNode }) {
  const [config, setConfigState] = useState<AiConfig>(DEFAULT_AI_CONFIG)

  useEffect(() => {
    // localStorage is unavailable during SSR, so read it in an effect.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    setConfigState(loadAiConfig())
  }, [])

  const setConfig = useCallback((partial: Partial<AiConfig>) => {
    setConfigState((prev) => {
      const next = normalizeAiConfig({ ...prev, ...partial })
      saveAiConfig(next)
      return next
    })
  }, [])

  return (
    <AiConfigContext.Provider value={{ config, setConfig }}>
      {children}
    </AiConfigContext.Provider>
  )
}

export function useAiConfig() {
  const context = useContext(AiConfigContext)
  if (context === undefined) {
    throw new Error('useAiConfig must be used within an AiConfigProvider')
  }
  return context
}
