export type FeedType = 'RSS' | 'NOSTR' | 'NOSTR_VIDEO';

export interface Feed {
  id: string;
  title: string;
  type: FeedType;
  url: string | null;
  npub: string | null;
  unreadCount: number;
  subscribedAt: string;
  tags: string[];
  categoryId: string | null;
  category: Category | null;
}

export interface FeedItem {
  id: string;
  title: string;
  content: string | null;
  author: string | null;
  publishedAt: string;
  url: string | null;
  originalUrl?: string;
  videoId?: string | null;
  embedUrl?: string | null;
  thumbnail?: string | null;
  isRead: boolean;
  isFavorited: boolean;
  feedTitle: string;
  feedType: FeedType;
}

export interface Category {
  id: string;
  name: string;
  color: string | null;
  icon: string | null;
}

export type ThemeMode = 'light' | 'dark' | 'system';

export interface ExtensionSettings {
  webAppUrl: string;
  pollIntervalMinutes: number;
  notificationsEnabled: boolean;
  notifyOnNewItems: boolean;
  maxNotificationsPerRefresh: number;
  lastSyncTime: string | null;
  theme: ThemeMode;
  showUnreadOnly: boolean;
}

export type NostrAuthMethod = 'nsec' | 'nip07' | 'none';

export interface NostrAuthData {
  method: NostrAuthMethod;
  pubkey: string | null;
  npub: string | null;
}

export interface StorageData {
  feeds: Feed[];
  seenItemIds: string[];
  settings: ExtensionSettings;
  authToken: string | null;
  nostrAuth: NostrAuthData | null;
}

export interface ApiResponse<T> {
  result: {
    data: T;
  };
}

export interface FeedsResponse {
  id: string;
  title: string;
  type: FeedType;
  url: string | null;
  npub: string | null;
  unreadCount: number;
  subscribedAt: string;
  tags: string[];
  categoryId: string | null;
  category: Category | null;
}

export interface FeedItemsResponse {
  items: FeedItem[];
  nextCursor?: string;
}

export type MessageType =
  | { type: 'REFRESH_FEEDS' }
  | { type: 'GET_TAB_INFO' }
  | { type: 'SET_AUTH_TOKEN'; token: string }
  | { type: 'CLEAR_AUTH' }
  | { type: 'GET_UNREAD_COUNT' }
  | { type: 'OPEN_ITEM'; itemId: string };

export interface MessageResponse {
  success: boolean;
  error?: string;
  data?: unknown;
}

export interface LocalFeed {
  id: string;
  type: 'RSS' | 'NOSTR';
  title: string;
  url: string | null;
  npub: string | null;
  addedAt: string;
}

export interface SyncSettings {
  webAppUrl: string;
  pollIntervalMinutes: number;
  notificationsEnabled: boolean;
  notifyOnNewItems: boolean;
  maxNotificationsPerRefresh: number;
}

export interface OPMLOutline {
  title: string;
  xmlUrl?: string;
  htmlUrl?: string;
  type?: string;
  children?: OPMLOutline[];
}

export interface SyncStorageData {
  localFeeds: LocalFeed[];
  syncSettings: SyncSettings;
}

export interface Folder {
  id: string;
  name: string;
  color: string;
  feedIds: string[];
  createdAt: string;
}

export interface ReadingStats {
  totalRead: number;
  readToday: number;
  readThisWeek: number;
  topFeeds: { feedTitle: string; count: number }[];
}
