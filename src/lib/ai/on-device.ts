import { AiClientError, type ChatMessage } from './client'
import type { MLCEngineInterface } from '@mlc-ai/web-llm'

export function isWebGpuAvailable(): boolean {
  return typeof navigator !== 'undefined' && 'gpu' in navigator
}

interface OnDeviceOptions {
  model: string
  messages: ChatMessage[]
  signal?: AbortSignal
  onProgress?: (p: { progress: number; text: string }) => void
}

let cached: { model: string; engine: MLCEngineInterface } | null = null
let inflight: { model: string; promise: Promise<MLCEngineInterface> } | null = null

async function getEngine(
  model: string,
  signal?: AbortSignal,
  onProgress?: (p: { progress: number; text: string }) => void
): Promise<MLCEngineInterface> {
  if (cached && cached.model === model) return cached.engine
  if (inflight && inflight.model === model) return inflight.promise

  if (signal?.aborted) throw new AiClientError('Aborted.')

  if (!isWebGpuAvailable()) {
    throw new AiClientError(
      'On-device AI requires a browser with WebGPU (Chrome, Edge, or recent Safari/Firefox).'
    )
  }

  const prior = inflight

  const build = (async () => {
    // Let any in-flight build for a different model settle first so two
    // CreateMLCEngine calls never run concurrently and orphan an engine.
    if (prior) {
      try {
        await prior.promise
      } catch {
        // prior build failed and already surfaced to its own caller; proceed
      }
    }

    // Release the previously loaded model before building a different one so we
    // don't leak its WebGPU device + weights.
    if (cached && cached.model !== model) {
      const stale = cached.engine
      cached = null
      try {
        await stale.unload()
      } catch {
        // ignore unload failures; proceed with the new build
      }
    }

    try {
      const { CreateMLCEngine } = await import('@mlc-ai/web-llm')
      // web-llm 0.2.84 has no AbortSignal for the weight download, so the CDN
      // fetch itself can't be interrupted; abort is enforced around it instead.
      const engine = await CreateMLCEngine(model, {
        initProgressCallback: (r) => onProgress?.({ progress: r.progress, text: r.text }),
      })
      cached = { model, engine }
      return engine
    } catch (err) {
      cached = null
      throw new AiClientError(
        `Could not load the on-device model "${model}". ${
          err instanceof Error ? err.message : 'Model load failed.'
        }`
      )
    }
  })()

  const entry = { model, promise: build }
  inflight = entry
  try {
    return await build
  } finally {
    if (inflight === entry) inflight = null
  }
}

export async function* streamOnDevice(opts: OnDeviceOptions): AsyncGenerator<string, void, unknown> {
  const { model, messages, signal, onProgress } = opts

  if (signal?.aborted) return

  const engine = await getEngine(model, signal, onProgress)

  if (signal?.aborted) return

  const completion = await engine.chat.completions.create({
    messages,
    stream: true,
  })

  try {
    for await (const chunk of completion) {
      if (signal?.aborted) {
        engine.interruptGenerate()
        return
      }
      const token = chunk.choices[0]?.delta?.content
      if (token) yield token
    }
  } catch (err) {
    if (signal?.aborted || (err instanceof DOMException && err.name === 'AbortError')) return
    throw new AiClientError(
      err instanceof Error ? err.message : 'On-device generation failed.'
    )
  }
}
