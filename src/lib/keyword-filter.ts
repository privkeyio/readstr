export type MatchTarget = 'title' | 'content' | 'author' | 'any'
export type MatchType = 'contains' | 'word'
export type FilterAction = 'hide' | 'highlight'

export interface FilterRule {
  id: string
  enabled: boolean
  order: number
  target: MatchTarget
  type: MatchType
  pattern: string
  caseSensitive: boolean
  action: FilterAction
  color?: string
}

export interface FilterOutcome {
  hidden: boolean
  highlight?: string
}

export interface FilterableItem {
  title?: string | null
  content?: string | null
  author?: string | null
}

const STORAGE_KEY = 'readstr_filters'
const MAX_PATTERN_LENGTH = 200
const MAX_RULES = 200

const MATCH_TARGETS: MatchTarget[] = ['title', 'content', 'author', 'any']
const MATCH_TYPES: MatchType[] = ['contains', 'word']
const FILTER_ACTIONS: FilterAction[] = ['hide', 'highlight']

function stripHtml(value: string): string {
  return value.replace(/<[^>]*>/g, ' ')
}

function pickTarget(item: FilterableItem, target: MatchTarget): string {
  const title = stripHtml(item.title ?? '')
  const content = stripHtml(item.content ?? '')
  const author = item.author ?? ''
  switch (target) {
    case 'title':
      return title
    case 'content':
      return content
    case 'author':
      return author
    case 'any':
      return `${title} ${content} ${author}`
  }
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

function matches(haystack: string, rule: FilterRule): boolean {
  const pattern = rule.pattern
  if (!pattern) return false
  if (rule.type === 'word') {
    try {
      const flags = rule.caseSensitive ? '' : 'i'
      const re = new RegExp(`\\b${escapeRegExp(pattern)}\\b`, flags)
      return re.test(haystack)
    } catch {
      return false
    }
  }
  if (rule.caseSensitive) {
    return haystack.includes(pattern)
  }
  return haystack.toLowerCase().includes(pattern.toLowerCase())
}

export function evaluateRules(item: FilterableItem, rules: FilterRule[]): FilterOutcome {
  const outcome: FilterOutcome = { hidden: false }
  const sorted = [...rules].sort((a, b) => a.order - b.order)
  for (const rule of sorted) {
    if (!rule.enabled) continue
    const haystack = pickTarget(item, rule.target)
    if (!matches(haystack, rule)) continue
    if (rule.action === 'hide') {
      return { hidden: true }
    }
    outcome.highlight = rule.color ?? outcome.highlight
  }
  return outcome
}

function sanitizeRule(raw: unknown): FilterRule | null {
  if (!raw || typeof raw !== 'object') return null
  const r = raw as Record<string, unknown>
  const pattern = typeof r.pattern === 'string' ? r.pattern.slice(0, MAX_PATTERN_LENGTH) : ''
  if (!pattern) return null
  const target = MATCH_TARGETS.includes(r.target as MatchTarget) ? (r.target as MatchTarget) : 'any'
  const type = MATCH_TYPES.includes(r.type as MatchType) ? (r.type as MatchType) : 'contains'
  const action = FILTER_ACTIONS.includes(r.action as FilterAction) ? (r.action as FilterAction) : 'hide'
  const id = typeof r.id === 'string' && r.id ? r.id : `${Date.now()}-${Math.random().toString(36).slice(2)}`
  return {
    id,
    enabled: r.enabled !== false,
    order: typeof r.order === 'number' && Number.isFinite(r.order) ? r.order : 0,
    target,
    type,
    pattern,
    caseSensitive: r.caseSensitive === true,
    action,
    color: action === 'highlight' && typeof r.color === 'string' ? r.color : undefined,
  }
}

export function loadFilterRules(): FilterRule[] {
  if (typeof window === 'undefined') return []
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY)
    if (!raw) return []
    const parsed = JSON.parse(raw)
    if (!Array.isArray(parsed)) return []
    return parsed
      .slice(0, MAX_RULES)
      .map(sanitizeRule)
      .filter((r): r is FilterRule => r !== null)
  } catch {
    return []
  }
}

export function saveFilterRules(rules: FilterRule[]): void {
  if (typeof window === 'undefined') return
  const sanitized = rules
    .slice(0, MAX_RULES)
    .map(sanitizeRule)
    .filter((r): r is FilterRule => r !== null)
  try {
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify(sanitized))
  } catch {
    // ignore write failures (quota, disabled storage)
  }
}
