import type { Feed, FeedItem, Folder } from '../types';

const DB_NAME = 'readstr-cache';
const DB_VERSION = 2;

interface CachedItem extends FeedItem {
  cachedAt: number;
}

class FeedDatabase {
  private db: IDBDatabase | null = null;
  private initPromise: Promise<void> | null = null;

  async init(): Promise<void> {
    if (this.db) return;
    if (this.initPromise) return this.initPromise;

    this.initPromise = new Promise<void>((resolve, reject) => {
      const request = indexedDB.open(DB_NAME, DB_VERSION);

      request.onerror = () => {
        this.initPromise = null;
        reject(request.error);
      };
      request.onsuccess = () => {
        this.db = request.result;
        resolve();
      };

      request.onupgradeneeded = (event) => {
        const db = (event.target as IDBOpenDBRequest).result;

        if (!db.objectStoreNames.contains('feeds')) {
          db.createObjectStore('feeds', { keyPath: 'id' });
        }

        if (!db.objectStoreNames.contains('items')) {
          const itemStore = db.createObjectStore('items', { keyPath: 'id' });
          itemStore.createIndex('publishedAt', 'publishedAt', { unique: false });
          itemStore.createIndex('isRead', 'isRead', { unique: false });
        }

        if (!db.objectStoreNames.contains('readLater')) {
          const readLaterStore = db.createObjectStore('readLater', { keyPath: 'itemId' });
          readLaterStore.createIndex('addedAt', 'addedAt', { unique: false });
        }

        if (!db.objectStoreNames.contains('folders')) {
          db.createObjectStore('folders', { keyPath: 'id' });
        }
      };
    });

    return this.initPromise;
  }

  async getFeeds(): Promise<Feed[]> {
    await this.init();
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction('feeds', 'readonly');
      const store = tx.objectStore('feeds');
      const request = store.getAll();
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
  }

  async saveFeeds(feeds: Feed[]): Promise<void> {
    await this.init();
    const tx = this.db!.transaction('feeds', 'readwrite');
    const store = tx.objectStore('feeds');
    store.clear();
    feeds.forEach((feed) => store.put(feed));
    return new Promise((resolve, reject) => {
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  async getItems(options: { limit?: number; unreadOnly?: boolean } = {}): Promise<FeedItem[]> {
    await this.init();
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction('items', 'readonly');
      const store = tx.objectStore('items');
      const index = store.index('publishedAt');
      const items: CachedItem[] = [];
      const limit = options.limit ?? 100;

      const request = index.openCursor(null, 'prev');
      request.onsuccess = () => {
        const cursor = request.result;
        if (cursor && items.length < limit) {
          const item = cursor.value as CachedItem;
          if (!options.unreadOnly || !item.isRead) {
            items.push(item);
          }
          cursor.continue();
        } else {
          resolve(items);
        }
      };
      request.onerror = () => reject(request.error);
    });
  }

  async saveItems(items: FeedItem[]): Promise<void> {
    await this.init();
    const tx = this.db!.transaction('items', 'readwrite');
    const store = tx.objectStore('items');
    const now = Date.now();
    items.forEach((item) => store.put({ ...item, cachedAt: now }));
    return new Promise((resolve, reject) => {
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  async markItemRead(itemId: string): Promise<void> {
    await this.init();
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction('items', 'readwrite');
      const store = tx.objectStore('items');
      const request = store.get(itemId);
      request.onsuccess = () => {
        if (request.result) {
          store.put({ ...request.result, isRead: true });
        }
        resolve();
      };
      request.onerror = () => reject(request.error);
    });
  }

  async clearOldItems(maxAgeMs: number = 7 * 24 * 60 * 60 * 1000): Promise<void> {
    await this.init();
    const cutoff = Date.now() - maxAgeMs;
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction('items', 'readwrite');
      const store = tx.objectStore('items');
      const request = store.openCursor();

      request.onsuccess = () => {
        const cursor = request.result;
        if (cursor) {
          const item = cursor.value as CachedItem;
          if (item.cachedAt < cutoff && item.isRead) {
            cursor.delete();
          }
          cursor.continue();
        }
      };
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  async searchItems(query: string, limit: number = 50): Promise<FeedItem[]> {
    const items = await this.getItems({ limit: 500 });
    const lowerQuery = query.toLowerCase();
    return items
      .filter(
        (item) =>
          item.title.toLowerCase().includes(lowerQuery) ||
          item.feedTitle.toLowerCase().includes(lowerQuery) ||
          item.content?.toLowerCase().includes(lowerQuery)
      )
      .slice(0, limit);
  }

  async getFolders(): Promise<Folder[]> {
    await this.init();
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction('folders', 'readonly');
      const store = tx.objectStore('folders');
      const request = store.getAll();
      request.onsuccess = () => resolve(request.result ?? []);
      request.onerror = () => reject(request.error);
    });
  }

  async saveFolder(folder: Folder): Promise<void> {
    await this.init();
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction('folders', 'readwrite');
      const store = tx.objectStore('folders');
      store.put(folder);
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  async deleteFolder(folderId: string): Promise<void> {
    await this.init();
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction('folders', 'readwrite');
      const store = tx.objectStore('folders');
      store.delete(folderId);
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  async getReadingStats(): Promise<{ totalRead: number; readToday: number; readThisWeek: number; topFeeds: { feedTitle: string; count: number }[] }> {
    const items = await this.getItems({ limit: 1000 });
    const todayStart = new Date().setHours(0, 0, 0, 0);
    const weekStart = todayStart - 6 * 24 * 60 * 60 * 1000;

    const readItems = items.filter((item) => item.isRead);
    const readToday = readItems.filter((item) => new Date(item.publishedAt).getTime() >= todayStart).length;
    const readThisWeek = readItems.filter((item) => new Date(item.publishedAt).getTime() >= weekStart).length;

    const feedCounts = new Map<string, number>();
    readItems.forEach((item) => {
      feedCounts.set(item.feedTitle, (feedCounts.get(item.feedTitle) ?? 0) + 1);
    });

    const topFeeds = Array.from(feedCounts.entries())
      .sort((a, b) => b[1] - a[1])
      .slice(0, 5)
      .map(([feedTitle, count]) => ({ feedTitle, count }));

    return { totalRead: readItems.length, readToday, readThisWeek, topFeeds };
  }

  async markItemsRead(itemIds: string[]): Promise<void> {
    await this.init();
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction('items', 'readwrite');
      const store = tx.objectStore('items');
      itemIds.forEach((itemId) => {
        const request = store.get(itemId);
        request.onsuccess = () => {
          if (request.result) {
            store.put({ ...request.result, isRead: true });
          }
        };
      });
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }
}

export const feedDatabase = new FeedDatabase();
