const DB_NAME = 'readstr-history'
const STORE_NAME = 'history'
const DB_VERSION = 1

const MAX_ENTRIES = 500
const CONTENT_CAP = 8192
const DEFAULT_LIMIT = 100

export interface HistoryRecord {
  id: string
  title: string
  content: string
  author: string | null
  feedTitle: string
  url?: string | null
  feedType: string
  readAt: number
}

export interface HistoryInput {
  id: string
  title: string
  content: string
  author: string | null
  feedTitle: string
  url?: string | null
  feedType: string
}

const memoryStore = new Map<string, HistoryRecord>()

function hasIndexedDb(): boolean {
  return typeof window !== 'undefined' && typeof window.indexedDB !== 'undefined'
}

function stripHtml(html: string): string {
  return html
    .replace(/<[^>]*>/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, CONTENT_CAP)
}

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = window.indexedDB.open(DB_NAME, DB_VERSION)
    request.onupgradeneeded = () => {
      const db = request.result
      if (!db.objectStoreNames.contains(STORE_NAME)) {
        db.createObjectStore(STORE_NAME, { keyPath: 'id' })
      }
    }
    request.onsuccess = () => resolve(request.result)
    request.onerror = () => reject(request.error)
  })
}

function getAllRecords(): Promise<HistoryRecord[]> {
  if (!hasIndexedDb()) {
    return Promise.resolve([...memoryStore.values()])
  }
  return openDb()
    .then(
      (db) =>
        new Promise<HistoryRecord[]>((resolve, reject) => {
          const tx = db.transaction(STORE_NAME, 'readonly')
          const req = tx.objectStore(STORE_NAME).getAll()
          req.onsuccess = () => resolve((req.result as HistoryRecord[]) ?? [])
          req.onerror = () => reject(req.error)
          tx.oncomplete = () => db.close()
        })
    )
    .catch(() => [...memoryStore.values()])
}

function byReadAtDesc(a: HistoryRecord, b: HistoryRecord): number {
  return b.readAt - a.readAt
}

export async function recordRead(item: HistoryInput): Promise<void> {
  const record: HistoryRecord = {
    id: item.id,
    title: item.title,
    content: stripHtml(item.content ?? ''),
    author: item.author,
    feedTitle: item.feedTitle,
    url: item.url ?? null,
    feedType: item.feedType,
    readAt: Date.now(),
  }
  memoryStore.set(record.id, record)
  if (memoryStore.size > MAX_ENTRIES) {
    ;[...memoryStore.values()]
      .sort(byReadAtDesc)
      .slice(MAX_ENTRIES)
      .forEach((r) => memoryStore.delete(r.id))
  }
  if (!hasIndexedDb()) return
  try {
    const db = await openDb()
    await new Promise<void>((resolve, reject) => {
      const tx = db.transaction(STORE_NAME, 'readwrite')
      const store = tx.objectStore(STORE_NAME)
      store.put(record)
      const allReq = store.getAll()
      allReq.onsuccess = () => {
        const all = (allReq.result as HistoryRecord[]) ?? []
        if (all.length > MAX_ENTRIES) {
          all
            .sort(byReadAtDesc)
            .slice(MAX_ENTRIES)
            .forEach((r) => store.delete(r.id))
        }
      }
      tx.oncomplete = () => {
        db.close()
        resolve()
      }
      tx.onerror = () => reject(tx.error)
    })
  } catch {
    // Record is already mirrored in memoryStore above.
  }
}

export async function clearHistory(): Promise<void> {
  memoryStore.clear()
  if (!hasIndexedDb()) return
  const db = await openDb()
  await new Promise<void>((resolve, reject) => {
    const tx = db.transaction(STORE_NAME, 'readwrite')
    tx.objectStore(STORE_NAME).clear()
    tx.oncomplete = () => {
      db.close()
      resolve()
    }
    tx.onerror = () => reject(tx.error)
  })
}

export async function getHistory(limit: number = DEFAULT_LIMIT): Promise<HistoryRecord[]> {
  const all = await getAllRecords()
  return all.sort(byReadAtDesc).slice(0, limit)
}

export async function searchHistory(
  query: string,
  limit: number = DEFAULT_LIMIT
): Promise<HistoryRecord[]> {
  const tokens = query.toLowerCase().trim().split(/\s+/).filter(Boolean)
  if (tokens.length === 0) {
    return getHistory(limit)
  }
  const all = await getAllRecords()
  return all
    .filter((r) => {
      const haystack = `${r.title} ${r.content}`.toLowerCase()
      return tokens.every((t) => haystack.includes(t))
    })
    .sort(byReadAtDesc)
    .slice(0, limit)
}
