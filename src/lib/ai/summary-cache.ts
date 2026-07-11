import type { AiFeature } from './prompts'

const DB_NAME = 'readstr-ai'
const STORE_NAME = 'summaries'
const DB_VERSION = 1

const memoryStore = new Map<string, string>()

export function cacheKey(
  guidOrId: string,
  feature: AiFeature,
  lang: string,
  model: string,
  baseUrl: string
): string {
  return `${guidOrId}:${feature}:${lang}:${model}:${baseUrl}`
}

function hasIndexedDb(): boolean {
  return typeof window !== 'undefined' && typeof window.indexedDB !== 'undefined'
}

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = window.indexedDB.open(DB_NAME, DB_VERSION)
    request.onupgradeneeded = () => {
      const db = request.result
      if (!db.objectStoreNames.contains(STORE_NAME)) {
        db.createObjectStore(STORE_NAME)
      }
    }
    request.onsuccess = () => resolve(request.result)
    request.onerror = () => reject(request.error)
  })
}

export async function getCachedSummary(key: string): Promise<string | null> {
  if (!hasIndexedDb()) {
    return memoryStore.get(key) ?? null
  }
  try {
    const db = await openDb()
    const result = await new Promise<string | null>((resolve, reject) => {
      const tx = db.transaction(STORE_NAME, 'readonly')
      const req = tx.objectStore(STORE_NAME).get(key)
      req.onsuccess = () => resolve((req.result as string | undefined) ?? null)
      req.onerror = () => reject(req.error)
      tx.oncomplete = () => db.close()
    })
    return result ?? memoryStore.get(key) ?? null
  } catch {
    return memoryStore.get(key) ?? null
  }
}

export async function setCachedSummary(key: string, value: string): Promise<void> {
  memoryStore.set(key, value)
  if (!hasIndexedDb()) return
  try {
    const db = await openDb()
    await new Promise<void>((resolve, reject) => {
      const tx = db.transaction(STORE_NAME, 'readwrite')
      tx.objectStore(STORE_NAME).put(value, key)
      tx.oncomplete = () => {
        db.close()
        resolve()
      }
      tx.onerror = () => reject(tx.error)
    })
  } catch {
    // Value is already mirrored in memoryStore above.
  }
}
