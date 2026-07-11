export interface ChatMessage {
  role: 'system' | 'user' | 'assistant'
  content: string
}

export interface StreamChatOptions {
  baseUrl: string
  apiKey?: string
  model: string
  messages: ChatMessage[]
  signal?: AbortSignal
}

export class AiClientError extends Error {}

function parseSseData(line: string): string | null {
  const trimmed = line.trim()
  if (!trimmed.startsWith('data:')) return null
  const data = trimmed.slice(5).trim()
  if (data === '[DONE]') return null
  try {
    const parsed = JSON.parse(data)
    const token = parsed?.choices?.[0]?.delta?.content
    return typeof token === 'string' && token ? token : null
  } catch {
    return null
  }
}

export async function* streamChat(
  options: StreamChatOptions
): AsyncGenerator<string, void, unknown> {
  const { baseUrl, apiKey, model, messages, signal } = options

  let response: Response
  try {
    response = await fetch(`${baseUrl}/chat/completions`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        ...(apiKey ? { Authorization: `Bearer ${apiKey}` } : {}),
      },
      body: JSON.stringify({ model, messages, stream: true }),
      signal,
    })
  } catch (err) {
    if (err instanceof DOMException && err.name === 'AbortError') throw err
    throw new AiClientError(
      `Could not reach the AI endpoint at ${baseUrl}. Check the base URL and that the server is running.`
    )
  }

  if (!response.ok) {
    let detail = ''
    try {
      detail = (await response.text()).slice(0, 200)
    } catch {
      // ignore body read failures
    }
    throw new AiClientError(
      `AI endpoint returned ${response.status} ${response.statusText}${detail ? `: ${detail}` : ''}`
    )
  }

  if (!response.body) {
    throw new AiClientError('AI endpoint returned an empty response body.')
  }

  const reader = response.body.getReader()
  const decoder = new TextDecoder()
  let buffer = ''

  try {
    while (true) {
      const { done, value } = await reader.read()
      if (done) break
      buffer += decoder.decode(value, { stream: true })

      let boundary: number
      while ((boundary = buffer.indexOf('\n')) !== -1) {
        const line = buffer.slice(0, boundary)
        buffer = buffer.slice(boundary + 1)
        const token = parseSseData(line)
        if (token !== null) yield token
      }
    }

    buffer += decoder.decode()
    const token = parseSseData(buffer)
    if (token !== null) yield token
  } finally {
    reader.cancel().catch(() => {})
  }
}
