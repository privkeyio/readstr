'use client'

import { useEffect, useRef, useState } from 'react'
import { FormattedContent } from './formatted-content'
import { streamChat } from '@/lib/ai/client'
import { buildPrompt, type AiFeature } from '@/lib/ai/prompts'
import { cacheKey, getCachedSummary, setCachedSummary } from '@/lib/ai/summary-cache'
import { AI_LANG_OPTIONS, type AiConfig } from '@/lib/ai/config'

interface AiSummaryPanelProps {
  articleKey: string
  title: string
  text: string
  feedTitle: string
  config: AiConfig
}

const FEATURE_LABELS: Record<AiFeature, string> = {
  summarize: 'Summary',
  insights: 'Insights',
}

export function AiSummaryPanel({ articleKey, title, text, feedTitle, config }: AiSummaryPanelProps) {
  const availableFeatures = (['summarize', 'insights'] as AiFeature[]).filter(
    (f) => config.features[f]
  )
  const [feature, setFeature] = useState<AiFeature>(availableFeatures[0] ?? 'summarize')
  const [lang, setLang] = useState(config.targetLang)
  const [output, setOutput] = useState('')
  const [status, setStatus] = useState<'idle' | 'loading' | 'streaming' | 'done' | 'error'>('idle')
  const [error, setError] = useState('')
  const [fromCache, setFromCache] = useState(false)
  const abortRef = useRef<AbortController | null>(null)

  useEffect(() => {
    return () => abortRef.current?.abort()
  }, [])

  const run = async () => {
    abortRef.current?.abort()
    const controller = new AbortController()
    abortRef.current = controller

    setStatus('loading')
    setOutput('')
    setError('')
    setFromCache(false)

    const key = cacheKey(articleKey, feature, lang, config.model, config.baseUrl)

    const cached = await getCachedSummary(key)
    if (controller.signal.aborted) return
    if (cached) {
      setOutput(cached)
      setStatus('done')
      setFromCache(true)
      return
    }

    try {
      const messages = buildPrompt(feature, { title, text, targetLang: lang })
      let acc = ''
      setStatus('streaming')
      for await (const token of streamChat({
        baseUrl: config.baseUrl,
        apiKey: config.apiKey || undefined,
        model: config.model,
        messages,
        signal: controller.signal,
      })) {
        acc += token
        setOutput(acc)
      }
      if (controller.signal.aborted) return
      setStatus('done')
      if (acc.trim()) void setCachedSummary(key, acc)
    } catch (err) {
      if (controller.signal.aborted || (err instanceof DOMException && err.name === 'AbortError')) {
        return
      }
      setError(err instanceof Error ? err.message : 'AI request failed.')
      setStatus('error')
    }
  }

  const busy = status === 'loading' || status === 'streaming'

  return (
    <div className="mb-6 rounded-xl border border-theme-primary bg-theme-surface-raised p-4 shadow-theme-sm">
      <div className="flex flex-wrap items-center gap-2 mb-3">
        {availableFeatures.length > 1 && (
          <div className="flex gap-1">
            {availableFeatures.map((f) => (
              <button
                key={f}
                onClick={() => setFeature(f)}
                className={`px-3 py-1.5 rounded-lg text-sm font-medium border-2 transition-all ${
                  feature === f
                    ? 'border-theme-accent bg-theme-accent-light text-theme-primary'
                    : 'border-theme-secondary bg-theme-primary text-theme-secondary hover:border-theme-accent/50'
                }`}
              >
                {FEATURE_LABELS[f]}
              </button>
            ))}
          </div>
        )}
        <select
          value={lang}
          onChange={(e) => setLang(e.target.value)}
          className="input-theme text-sm py-1.5"
          title="Output language"
        >
          {AI_LANG_OPTIONS.map((o) => (
            <option key={o.value} value={o.value}>
              {o.label}
            </option>
          ))}
        </select>
        <button
          onClick={run}
          disabled={busy || availableFeatures.length === 0}
          className="btn-theme-primary text-sm flex items-center gap-2 disabled:opacity-50"
        >
          {busy ? (
            <>
              <svg className="w-4 h-4 animate-spin" fill="none" viewBox="0 0 24 24">
                <circle className="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="4" />
                <path className="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z" />
              </svg>
              {status === 'loading' ? 'Loading…' : 'Generating…'}
            </>
          ) : (
            <>✨ {output ? 'Regenerate' : `Generate ${FEATURE_LABELS[feature]}`}</>
          )}
        </button>
        <span className="ml-auto text-xs text-theme-tertiary">{feedTitle}</span>
      </div>

      {error && (
        <div className="text-sm text-red-600 flex items-start gap-2">
          <span>⚠</span>
          <span>{error}</span>
        </div>
      )}

      {output && (
        <div className="prose-theme">
          <FormattedContent content={output} />
          {fromCache && status === 'done' && (
            <p className="text-xs text-theme-tertiary mt-2">Cached</p>
          )}
        </div>
      )}
    </div>
  )
}
