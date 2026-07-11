import { buildHaystacks, type FilterableItem } from './keyword-filter'

export type ViewReadState = 'all' | 'unread' | 'read'
export type ViewSort = 'newest' | 'oldest'

export type ViewSource =
  | { kind: 'all' }
  | { kind: 'feed'; feedId: string }
  | { kind: 'tags'; tags: string[] }
  | { kind: 'category'; categoryId: string }
  | { kind: 'favorites' }

export interface ViewKeywords {
  include?: string[]
  exclude?: string[]
}

export type ViewOrganizationMode = 'tags' | 'categories'

export interface SavedView {
  id: string
  name: string
  icon?: string
  order: number
  source: ViewSource
  readState: ViewReadState
  sort: ViewSort
  keywords?: ViewKeywords
  organizationMode?: ViewOrganizationMode
}

const STORAGE_KEY = 'readstr_views'
const ACTIVE_KEY = 'readstr_active_view'
const MAX_VIEWS = 50
const NAME_MAX = 60
const KEYWORD_MAX = 200
const MAX_KEYWORDS = 50

const READ_STATES: ViewReadState[] = ['all', 'unread', 'read']
const SORTS: ViewSort[] = ['newest', 'oldest']
const ORGANIZATION_MODES: ViewOrganizationMode[] = ['tags', 'categories']

export function newViewId(): string {
  return `${Date.now()}-${Math.random().toString(36).slice(2)}`
}

function sanitizeStrings(raw: unknown): string[] | undefined {
  if (!Array.isArray(raw)) return undefined
  const out = raw
    .filter((v): v is string => typeof v === 'string')
    .map((v) => v.slice(0, KEYWORD_MAX))
    .filter((v) => v.length > 0)
    .slice(0, MAX_KEYWORDS)
  return out.length > 0 ? out : undefined
}

function sanitizeSource(raw: unknown): ViewSource {
  if (!raw || typeof raw !== 'object') return { kind: 'all' }
  const s = raw as Record<string, unknown>
  switch (s.kind) {
    case 'feed':
      return typeof s.feedId === 'string' && s.feedId ? { kind: 'feed', feedId: s.feedId } : { kind: 'all' }
    case 'tags': {
      const tags = sanitizeStrings(s.tags)
      return tags ? { kind: 'tags', tags } : { kind: 'all' }
    }
    case 'category':
      return typeof s.categoryId === 'string' && s.categoryId
        ? { kind: 'category', categoryId: s.categoryId }
        : { kind: 'all' }
    case 'favorites':
      return { kind: 'favorites' }
    default:
      return { kind: 'all' }
  }
}

function sanitizeKeywords(raw: unknown): ViewKeywords | undefined {
  if (!raw || typeof raw !== 'object') return undefined
  const k = raw as Record<string, unknown>
  const include = sanitizeStrings(k.include)
  const exclude = sanitizeStrings(k.exclude)
  if (!include && !exclude) return undefined
  const out: ViewKeywords = {}
  if (include) out.include = include
  if (exclude) out.exclude = exclude
  return out
}

export function sanitizeView(raw: unknown): SavedView | null {
  if (!raw || typeof raw !== 'object') return null
  const v = raw as Record<string, unknown>
  const name = typeof v.name === 'string' ? v.name.slice(0, NAME_MAX).trim() : ''
  if (!name) return null
  const id = typeof v.id === 'string' && v.id ? v.id : newViewId()
  const readState = READ_STATES.includes(v.readState as ViewReadState)
    ? (v.readState as ViewReadState)
    : 'all'
  const sort = SORTS.includes(v.sort as ViewSort) ? (v.sort as ViewSort) : 'newest'
  const view: SavedView = {
    id,
    name,
    order: typeof v.order === 'number' && Number.isFinite(v.order) ? v.order : 0,
    source: sanitizeSource(v.source),
    readState,
    sort,
  }
  if (typeof v.icon === 'string' && v.icon) view.icon = v.icon.slice(0, 8)
  const keywords = sanitizeKeywords(v.keywords)
  if (keywords) view.keywords = keywords
  if (ORGANIZATION_MODES.includes(v.organizationMode as ViewOrganizationMode)) {
    view.organizationMode = v.organizationMode as ViewOrganizationMode
  }
  return view
}

export function normalizeViews(raw: unknown): SavedView[] {
  if (!Array.isArray(raw)) return []
  return raw
    .slice(0, MAX_VIEWS)
    .map(sanitizeView)
    .filter((v): v is SavedView => v !== null)
    .sort((a, b) => a.order - b.order)
    .map((v, i) => ({ ...v, order: i }))
}

function reattachExclude(view: SavedView, exclude?: string[]): SavedView {
  // Strip any exclude the input carries and re-attach the caller-supplied local
  // exclude (mute words are never carried over the wire, only locally).
  const keywords: ViewKeywords = { ...view.keywords }
  delete keywords.exclude
  if (exclude && exclude.length > 0) keywords.exclude = exclude
  const out = { ...view }
  if (keywords.include || keywords.exclude) out.keywords = keywords
  else delete out.keywords
  return out
}

export function mergeViewLists(local: SavedView[], remote: SavedView[]): SavedView[] {
  const localById = new Map(local.map((v) => [v.id, v]))
  const seen = new Set<string>()
  const merged: SavedView[] = []

  // Remote is authoritative for shared fields, but keywords.exclude is local-only:
  // strip whatever the remote carries and re-attach the local mute words by id.
  for (const r of remote) {
    seen.add(r.id)
    merged.push(reattachExclude(r, localById.get(r.id)?.keywords?.exclude))
  }

  // Additive merge: keep local-only views (no deletion-sync).
  for (const l of local) {
    if (!seen.has(l.id)) merged.push(l)
  }

  return normalizeViews(merged)
}

export function reorderViews(views: SavedView[], index: number, direction: -1 | 1): SavedView[] {
  const target = index + direction
  if (index < 0 || index >= views.length || target < 0 || target >= views.length) {
    return views
  }
  const next = [...views]
  const [moved] = next.splice(index, 1)
  next.splice(target, 0, moved!)
  return next.map((v, i) => ({ ...v, order: i }))
}

export function loadViews(): SavedView[] {
  if (typeof window === 'undefined') return []
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY)
    if (!raw) return []
    return normalizeViews(JSON.parse(raw))
  } catch {
    return []
  }
}

export function saveViews(views: SavedView[]): void {
  if (typeof window === 'undefined') return
  try {
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify(normalizeViews(views)))
  } catch {
    // ignore write failures (quota, disabled storage)
  }
}

export function loadActiveViewId(): string | null {
  if (typeof window === 'undefined') return null
  try {
    return window.localStorage.getItem(ACTIVE_KEY)
  } catch {
    return null
  }
}

export function saveActiveViewId(id: string | null): void {
  if (typeof window === 'undefined') return
  try {
    if (id) window.localStorage.setItem(ACTIVE_KEY, id)
    else window.localStorage.removeItem(ACTIVE_KEY)
  } catch {
    // ignore write failures
  }
}

export function matchesView(item: FilterableItem, view: SavedView): boolean {
  const kw = view.keywords
  if (!kw || (!kw.include?.length && !kw.exclude?.length)) return true
  const haystack = buildHaystacks(item).any.toLowerCase()
  if (kw.include?.length && !kw.include.some((k) => haystack.includes(k.toLowerCase()))) {
    return false
  }
  if (kw.exclude?.length && kw.exclude.some((k) => haystack.includes(k.toLowerCase()))) {
    return false
  }
  return true
}
