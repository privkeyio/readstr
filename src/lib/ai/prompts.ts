import type { ChatMessage } from './client'

export type AiFeature = 'summarize' | 'insights' | 'translate'

interface PromptInput {
  title: string
  text: string
  targetLang?: string
}

const MAX_INPUT_CHARS = 12000

function langInstruction(targetLang?: string): string {
  if (!targetLang || targetLang === 'auto') {
    return 'Write in the same language as the article.'
  }
  return `Write your response in ${targetLang}.`
}

function articleBlock({ title, text }: PromptInput): string {
  const clipped = text.slice(0, MAX_INPUT_CHARS)
  return `Title: ${title}\n\nArticle:\n${clipped}`
}

export function buildPrompt(feature: AiFeature, input: PromptInput): ChatMessage[] {
  const lang = langInstruction(input.targetLang)

  if (feature === 'translate') {
    const targetLang =
      input.targetLang && input.targetLang !== 'auto' ? input.targetLang : 'the article language'
    return [
      {
        role: 'system',
        content:
          'You are a precise translator. Translate the article into the requested language, preserving its structure, formatting, and markdown. Output only the translation in markdown, with no summary, no commentary, and no preamble.',
      },
      {
        role: 'user',
        content: `Translate the article into ${targetLang}.\n\n${articleBlock(input)}`,
      },
    ]
  }

  if (feature === 'insights') {
    return [
      {
        role: 'system',
        content:
          'You are a sharp reading assistant. Extract the key insights from an article as concise markdown bullet points. Focus on facts, claims, and takeaways. Do not add preamble or a closing summary. Output plain markdown only.',
      },
      {
        role: 'user',
        content: `${lang}\n\nList the key insights and takeaways as markdown bullet points.\n\n${articleBlock(input)}`,
      },
    ]
  }

  return [
    {
      role: 'system',
      content:
        'You are a concise reading assistant. Summarize articles faithfully in a few short paragraphs. Do not add preamble like "This article". Output plain markdown only.',
    },
    {
      role: 'user',
      content: `${lang}\n\nSummarize the following article.\n\n${articleBlock(input)}`,
    },
  ]
}
