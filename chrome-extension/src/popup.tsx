import { StrictMode, useState, useEffect, useCallback, useRef } from 'react';
import { createRoot } from 'react-dom/client';
import type { Feed, FeedItem, ExtensionSettings, ThemeMode } from './types';
import { ConnectionStatus } from './components/ConnectionStatus';
import { ItemSkeleton } from './components/Skeleton';
import { ErrorBoundary } from './components/ErrorBoundary';
import { VirtualList } from './components/VirtualList';
import { SearchBar } from './components/SearchBar';
import { Statistics } from './components/Statistics';
import { BulkActions } from './components/BulkActions';
import { feedDatabase } from './db/feedDatabase';

const DEFAULT_WEB_APP_URL = 'https://readstr.privkey.io:8444';

interface RecentItem extends FeedItem {
  feedId: string;
}

function formatTimeAgo(dateStr: string): string {
  const date = new Date(dateStr);
  const now = new Date();
  const diffMs = now.getTime() - date.getTime();
  const diffMins = Math.floor(diffMs / 60000);
  if (diffMins < 1) return 'now';
  if (diffMins < 60) return `${diffMins}m`;
  const diffHours = Math.floor(diffMins / 60);
  if (diffHours < 24) return `${diffHours}h`;
  const diffDays = Math.floor(diffHours / 24);
  if (diffDays < 7) return `${diffDays}d`;
  return date.toLocaleDateString();
}

function ItemRow({
  item,
  isSelected,
  isChecked,
  onOpen,
  onMarkRead,
  onToggleSelect,
  onFavorite,
  showCheckbox = false
}: {
  item: RecentItem;
  isSelected: boolean;
  isChecked?: boolean;
  onOpen: () => void;
  onMarkRead: () => void;
  onToggleSelect?: () => void;
  onFavorite?: () => void;
  showCheckbox?: boolean;
}) {
  const typeIcon = item.feedType === 'RSS' ? '📰' : item.feedType === 'NOSTR_VIDEO' ? '🎬' : '📝';

  return (
    <div
      className={`item-row ${isSelected ? 'selected' : ''} ${item.isRead ? 'read' : ''} ${isChecked ? 'checked' : ''}`}
      onClick={onOpen}
      tabIndex={0}
      onKeyDown={(e) => {
        if (e.key === 'Enter') onOpen();
        if (e.key === 'm' || e.key === 'M') onMarkRead();
        if (e.key === 'f' || e.key === 'F') onFavorite?.();
      }}
    >
      {showCheckbox && (
        <input
          type="checkbox"
          className="item-checkbox"
          checked={isChecked}
          onChange={(e) => {
            e.stopPropagation();
            onToggleSelect?.();
          }}
          onClick={(e) => e.stopPropagation()}
        />
      )}
      <span className="item-icon">{typeIcon}</span>
      <div className="item-content">
        <div className="item-title">{item.title}</div>
        <div className="item-meta">
          <span className="item-feed">{item.feedTitle}</span>
          <span className="item-time">{formatTimeAgo(item.publishedAt)}</span>
        </div>
      </div>
      <div className="item-actions">
        {onFavorite && (
          <button
            className={`item-action-btn ${item.isFavorited ? 'active' : ''}`}
            onClick={(e) => {
              e.stopPropagation();
              onFavorite();
            }}
            title={item.isFavorited ? 'Remove from favorites' : 'Add to favorites'}
          >
            {item.isFavorited ? '★' : '☆'}
          </button>
        )}
        {!item.isRead && (
          <span
            className="unread-dot"
            onClick={(e) => {
              e.stopPropagation();
              onMarkRead();
            }}
            title="Mark as read"
          />
        )}
      </div>
    </div>
  );
}

function App() {
  const [feeds, setFeeds] = useState<Feed[]>([]);
  const [recentItems, setRecentItems] = useState<RecentItem[]>([]);
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [syncError, setSyncError] = useState(false);
  const [syncStatus, setSyncStatus] = useState<'idle' | 'syncing' | 'success' | 'error'>('idle');
  const [webAppUrl, setWebAppUrl] = useState(DEFAULT_WEB_APP_URL);
  const [isAuthenticated, setIsAuthenticated] = useState(false);
  const [lastSync, setLastSync] = useState<string | null>(null);
  const [theme, setTheme] = useState<ThemeMode>('system');
  const [showUnreadOnly, setShowUnreadOnly] = useState(false);
  const [selectedIndex, setSelectedIndex] = useState(-1);
  const [view, setView] = useState<'items' | 'feeds' | 'favorites'>('items');
  const [expandedFeeds, setExpandedFeeds] = useState<Set<string>>(new Set());
  const [searchResults, setSearchResults] = useState<RecentItem[] | null>(null);
  const [favoriteItems, setFavoriteItems] = useState<FeedItem[]>([]);
  const [selectedItems, setSelectedItems] = useState<Set<string>>(new Set());
  const [showStats, setShowStats] = useState(false);
  const [isOffline, setIsOffline] = useState(!navigator.onLine);
  const [pollIntervalMinutes, setPollIntervalMinutes] = useState(5);
  const containerRef = useRef<HTMLDivElement>(null);
  const syncStatusTimeoutRef = useRef<number | null>(null);

  const toggleFeedExpanded = (feedId: string) => {
    setExpandedFeeds(prev => {
      const next = new Set(prev);
      if (next.has(feedId)) {
        next.delete(feedId);
      } else {
        next.add(feedId);
      }
      return next;
    });
  };

  const getItemsForFeed = (feedTitle: string) => {
    return recentItems.filter(item => item.feedTitle === feedTitle);
  };

  const applyTheme = useCallback((themeMode: ThemeMode) => {
    let isDark = false;
    if (themeMode === 'dark') {
      isDark = true;
    } else if (themeMode === 'system') {
      isDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
    }
    document.documentElement.setAttribute('data-theme', isDark ? 'dark' : 'light');
  }, []);

  const loadData = useCallback(async () => {
    try {
      const result = await chrome.storage.local.get(['feeds', 'settings', 'authToken', 'nostrAuth', 'recentItems']);
      const feeds = (result['feeds'] as Feed[] | undefined) ?? [];
      const settings = result['settings'] as ExtensionSettings | undefined;
      const authToken = result['authToken'] as string | undefined;
      const nostrAuth = result['nostrAuth'] as { pubkey?: string } | undefined;
      const items = (result['recentItems'] as RecentItem[] | undefined) ?? [];

      setFeeds(feeds);
      setRecentItems(items);
      setWebAppUrl(settings?.webAppUrl ?? DEFAULT_WEB_APP_URL);
      setIsAuthenticated(!!authToken || !!nostrAuth?.pubkey);
      setLastSync(settings?.lastSyncTime ?? null);
      setPollIntervalMinutes(settings?.pollIntervalMinutes ?? 5);
      setTheme(settings?.theme ?? 'system');
      setShowUnreadOnly(settings?.showUnreadOnly ?? false);
      applyTheme(settings?.theme ?? 'system');
    } catch (err) {
      console.error('Failed to load data:', err);
    } finally {
      setLoading(false);
    }
  }, [applyTheme]);

  useEffect(() => {
    void loadData();
  }, [loadData]);

  useEffect(() => {
    chrome.runtime.sendMessage({ type: 'REFRESH_FEEDS', forceSync: false })
      .then(() => loadData())
      .catch(() => {});
  }, [loadData]);

  useEffect(() => {
    const mediaQuery = window.matchMedia('(prefers-color-scheme: dark)');
    const handleChange = () => {
      if (theme === 'system') applyTheme('system');
    };
    mediaQuery.addEventListener('change', handleChange);
    return () => mediaQuery.removeEventListener('change', handleChange);
  }, [applyTheme, theme]);

  useEffect(() => {
    const handleOnline = () => setIsOffline(false);
    const handleOffline = () => setIsOffline(true);
    window.addEventListener('online', handleOnline);
    window.addEventListener('offline', handleOffline);
    return () => {
      window.removeEventListener('online', handleOnline);
      window.removeEventListener('offline', handleOffline);
    };
  }, []);

  useEffect(() => {
    return () => {
      if (syncStatusTimeoutRef.current) {
        clearTimeout(syncStatusTimeoutRef.current);
      }
    };
  }, []);

  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      const items = displayedItems;
      if (!items.length) return;

      switch (e.key) {
        case 'j':
        case 'ArrowDown':
          e.preventDefault();
          setSelectedIndex(prev => Math.min(prev + 1, items.length - 1));
          break;
        case 'k':
        case 'ArrowUp':
          e.preventDefault();
          setSelectedIndex(prev => Math.max(prev - 1, 0));
          break;
        case 'Enter':
        case 'o':
          if (selectedIndex >= 0 && selectedIndex < items.length) {
            handleOpenItem(items[selectedIndex]!);
          }
          break;
        case 'm':
          if (selectedIndex >= 0 && selectedIndex < items.length) {
            void handleMarkRead(items[selectedIndex]!.id);
          }
          break;
        case 'r':
          void handleRefresh();
          break;
        case 'u':
          handleToggleFilter();
          break;
        case 't':
          handleCycleTheme();
          break;
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  });

  const handleRefresh = async () => {
    setRefreshing(true);
    setSyncError(false);
    setSyncStatus('syncing');
    try {
      const response = await chrome.runtime.sendMessage({ type: 'REFRESH_FEEDS' });
      if (response?.error) {
        setSyncError(true);
        setSyncStatus('error');
      } else {
        setSyncStatus('success');
      }
      await loadData();
    } catch (err) {
      console.error('Failed to refresh:', err);
      setSyncError(true);
      setSyncStatus('error');
    } finally {
      setRefreshing(false);
      if (syncStatusTimeoutRef.current) {
        clearTimeout(syncStatusTimeoutRef.current);
      }
      syncStatusTimeoutRef.current = window.setTimeout(() => setSyncStatus('idle'), 2000);
    }
  };

  const handleOpenApp = () => {
    const readerUrl = webAppUrl.replace(/\/$/, '') + '/reader';
    void chrome.tabs.create({ url: readerUrl });
  };

  const handleOpenItem = (item: RecentItem) => {
    const url = item.originalUrl || item.url;
    if (url) {
      void chrome.tabs.create({ url });
      void handleMarkRead(item.id);
    }
  };

  const handleMarkRead = async (itemId: string) => {
    try {
      const item = recentItems.find((i) => i.id === itemId);
      const wasUnread = item && !item.isRead;

      await chrome.runtime.sendMessage({ type: 'MARK_ITEM_READ', itemId });

      setRecentItems((prev) =>
        prev.map((i) => (i.id === itemId ? { ...i, isRead: true } : i))
      );

      if (wasUnread && item?.feedTitle) {
        setFeeds((prev) =>
          prev.map((feed) =>
            feed.title === item.feedTitle && feed.unreadCount > 0
              ? { ...feed, unreadCount: feed.unreadCount - 1 }
              : feed
          )
        );
      }
    } catch (err) {
      console.error('Failed to mark as read:', err);
    }
  };

  const handleToggleFilter = async () => {
    const newValue = !showUnreadOnly;
    setShowUnreadOnly(newValue);
    try {
      const result = await chrome.storage.local.get(['settings']);
      const settings = (result['settings'] as ExtensionSettings | undefined) ?? {} as ExtensionSettings;
      await chrome.storage.local.set({ settings: { ...settings, showUnreadOnly: newValue } });
    } catch (err) {
      console.error('Failed to save filter:', err);
    }
  };

  const handleCycleTheme = async () => {
    const themes: ThemeMode[] = ['light', 'dark', 'system'];
    const currentIndex = themes.indexOf(theme);
    const newTheme = themes[(currentIndex + 1) % themes.length]!;
    setTheme(newTheme);
    applyTheme(newTheme);
    try {
      const result = await chrome.storage.local.get(['settings']);
      const settings = (result['settings'] as ExtensionSettings | undefined) ?? {} as ExtensionSettings;
      await chrome.storage.local.set({ settings: { ...settings, theme: newTheme } });
    } catch (err) {
      console.error('Failed to save theme:', err);
    }
  };

  const handleSearch = useCallback(async (query: string) => {
    try {
      const results = await feedDatabase.searchItems(query);
      setSearchResults(results as RecentItem[]);
    } catch (err) {
      console.error('Search failed:', err);
    }
  }, []);

  const handleClearSearch = useCallback(() => {
    setSearchResults(null);
  }, []);

  const loadFavorites = useCallback(async () => {
    try {
      const response = await chrome.runtime.sendMessage({ type: 'GET_FAVORITES' });
      if (response.success && response.data?.items) {
        setFavoriteItems(response.data.items);
      }
    } catch (err) {
      console.error('Failed to load favorites:', err);
    }
  }, []);

  const handleAddToFavorites = async (item: RecentItem) => {
    try {
      const response = await chrome.runtime.sendMessage({ type: 'ADD_FAVORITE', itemId: item.id });
      if (response.success) {
        setFavoriteItems(prev => [{ ...item, isFavorited: true }, ...prev]);
        setRecentItems(prev => prev.map(i => i.id === item.id ? { ...i, isFavorited: true } : i));
      }
    } catch (err) {
      console.error('Failed to add to favorites:', err);
    }
  };

  const handleRemoveFromFavorites = async (itemId: string) => {
    try {
      const response = await chrome.runtime.sendMessage({ type: 'REMOVE_FAVORITE', itemId });
      if (response.success) {
        setFavoriteItems(prev => prev.filter(i => i.id !== itemId));
        setRecentItems(prev => prev.map(i => i.id === itemId ? { ...i, isFavorited: false } : i));
      }
    } catch (err) {
      console.error('Failed to remove from favorites:', err);
    }
  };

  const handleToggleSelect = (itemId: string) => {
    setSelectedItems(prev => {
      const next = new Set(prev);
      if (next.has(itemId)) {
        next.delete(itemId);
      } else {
        next.add(itemId);
      }
      return next;
    });
  };

  const handleClearSelection = () => {
    setSelectedItems(new Set());
  };

  const handleBulkMarkRead = async () => {
    try {
      const itemIds = Array.from(selectedItems);
      await feedDatabase.markItemsRead(itemIds);
      await Promise.all(itemIds.map(id => chrome.runtime.sendMessage({ type: 'MARK_ITEM_READ', itemId: id })));
      setRecentItems(prev => prev.map(i => selectedItems.has(i.id) ? { ...i, isRead: true } : i));
      setSelectedItems(new Set());
    } catch (err) {
      console.error('Failed to bulk mark as read:', err);
    }
  };

  useEffect(() => {
    if (view === 'favorites') {
      void loadFavorites();
    }
  }, [view, loadFavorites]);

  const totalUnread = feeds.reduce((sum, feed) => sum + feed.unreadCount, 0);
  const displayedItems = showUnreadOnly
    ? recentItems.filter(item => !item.isRead)
    : recentItems;

  const formatLastSync = (timestamp: string | null): string => {
    if (!timestamp) return 'Never';
    const date = new Date(timestamp);
    const now = new Date();
    const diffMs = now.getTime() - date.getTime();
    const diffMins = Math.floor(diffMs / 60000);
    if (diffMins < 1) return 'Just now';
    if (diffMins < 60) return `${diffMins}m ago`;
    const diffHours = Math.floor(diffMins / 60);
    if (diffHours < 24) return `${diffHours}h ago`;
    return date.toLocaleDateString();
  };

  const formatNextSync = (lastSyncTime: string | null, intervalMins: number): string => {
    if (!lastSyncTime) return `in ${intervalMins}m`;
    const lastSync = new Date(lastSyncTime);
    const nextSync = new Date(lastSync.getTime() + intervalMins * 60000);
    const now = new Date();
    const diffMs = nextSync.getTime() - now.getTime();
    if (diffMs <= 0) return 'soon';
    const diffMins = Math.ceil(diffMs / 60000);
    if (diffMins < 60) return `in ${diffMins}m`;
    const diffHours = Math.floor(diffMins / 60);
    return `in ${diffHours}h ${diffMins % 60}m`;
  };

  const themeIcon = theme === 'light' ? '☀️' : theme === 'dark' ? '🌙' : '🖥️';

  if (loading) {
    return (
      <div className="container">
        <header className="header">
          <h1>Readstr</h1>
        </header>
        <div className="content-area">
          <ItemSkeleton />
          <ItemSkeleton />
          <ItemSkeleton />
          <ItemSkeleton />
          <ItemSkeleton />
        </div>
      </div>
    );
  }

  return (
    <div className="container" ref={containerRef}>
      <header className="header">
        <h1>Readstr</h1>
        {totalUnread > 0 && <span className="badge total-badge">{totalUnread}</span>}
        <div className="header-actions">
          <ConnectionStatus isSyncing={refreshing} hasError={syncError} />
          <button
            className="icon-btn"
            onClick={handleCycleTheme}
            title={`Theme: ${theme}`}
            aria-label={`Theme: ${theme}`}
          >
            {themeIcon}
          </button>
        </div>
      </header>

      {!isAuthenticated && (
        <div className="auth-notice">
          <p>Sign in to sync your feeds</p>
        </div>
      )}

      {(isOffline || syncError) && isAuthenticated && (
        <div className="cache-banner">
          <span className="cache-icon">{isOffline ? '📡' : '⚠️'}</span>
          <span className="cache-text">
            {isOffline ? 'Offline - viewing cached data' : 'Sync failed - viewing cached data'}
          </span>
        </div>
      )}

      <div className="toolbar">
        <SearchBar
          onSearch={handleSearch}
          onClear={handleClearSearch}
          placeholder="Search items..."
        />
      </div>

      <div className="toolbar">
        <div className="view-tabs">
          <button
            className={`tab ${view === 'items' ? 'active' : ''}`}
            onClick={() => setView('items')}
          >
            Recent
          </button>
          <button
            className={`tab ${view === 'feeds' ? 'active' : ''}`}
            onClick={() => setView('feeds')}
          >
            Feeds
          </button>
          <button
            className={`tab ${view === 'favorites' ? 'active' : ''}`}
            onClick={() => setView('favorites')}
          >
            Favorites {favoriteItems.length > 0 && <span className="tab-count">{favoriteItems.length}</span>}
          </button>
        </div>
        <button
          className={`filter-btn ${showUnreadOnly ? 'active' : ''}`}
          onClick={handleToggleFilter}
          title="Toggle unread only (u)"
        >
          {showUnreadOnly ? '●' : '○'} Unread
        </button>
      </div>

      {selectedItems.size > 0 && (
        <BulkActions
          selectedCount={selectedItems.size}
          onMarkRead={() => void handleBulkMarkRead()}
          onClearSelection={handleClearSelection}
        />
      )}

      <div className="content-area">
        {!isAuthenticated ? (
          <div className="empty-state">
            <p>Not signed in</p>
            <p className="hint">Open the app to sign in with Nostr</p>
          </div>
        ) : searchResults !== null ? (
          searchResults.length === 0 ? (
            <div className="empty-state">
              <p>No results found</p>
              <p className="hint">Try a different search term</p>
            </div>
          ) : (
            <div className="item-list">
              {searchResults.map((item, index) => (
                <ItemRow
                  key={item.id}
                  item={item}
                  isSelected={selectedIndex === index}
                  isChecked={selectedItems.has(item.id)}
                  showCheckbox={selectedItems.size > 0}
                  onOpen={() => handleOpenItem(item)}
                  onMarkRead={() => void handleMarkRead(item.id)}
                  onToggleSelect={() => handleToggleSelect(item.id)}
                  onFavorite={() => void handleAddToFavorites(item)}
                />
              ))}
            </div>
          )
        ) : view === 'favorites' ? (
          favoriteItems.length === 0 ? (
            <div className="empty-state">
              <p>No favorites</p>
              <p className="hint">Click ★ to add items to favorites</p>
            </div>
          ) : (
            <div className="item-list">
              {favoriteItems.map((favItem, index) => (
                <ItemRow
                  key={favItem.id}
                  item={favItem as RecentItem}
                  isSelected={selectedIndex === index}
                  onOpen={() => handleOpenItem(favItem as RecentItem)}
                  onMarkRead={() => void handleMarkRead(favItem.id)}
                  onFavorite={() => void handleRemoveFromFavorites(favItem.id)}
                />
              ))}
            </div>
          )
        ) : view === 'items' ? (
          displayedItems.length === 0 ? (
            <div className="empty-state">
              <p>{showUnreadOnly ? 'All caught up!' : 'No recent items'}</p>
              <p className="hint">New items will appear here</p>
            </div>
          ) : displayedItems.length > 20 ? (
            <VirtualList
              items={displayedItems}
              itemHeight={58}
              containerHeight={280}
              className="item-list"
              renderItem={(item, index) => (
                <ItemRow
                  key={item.id}
                  item={item}
                  isSelected={selectedIndex === index}
                  isChecked={selectedItems.has(item.id)}
                  showCheckbox={selectedItems.size > 0}
                  onOpen={() => handleOpenItem(item)}
                  onMarkRead={() => void handleMarkRead(item.id)}
                  onToggleSelect={() => handleToggleSelect(item.id)}
                  onFavorite={() => void handleAddToFavorites(item)}
                />
              )}
            />
          ) : (
            <div className="item-list">
              {displayedItems.map((item, index) => (
                <ItemRow
                  key={item.id}
                  item={item}
                  isSelected={selectedIndex === index}
                  isChecked={selectedItems.has(item.id)}
                  showCheckbox={selectedItems.size > 0}
                  onOpen={() => handleOpenItem(item)}
                  onMarkRead={() => void handleMarkRead(item.id)}
                  onToggleSelect={() => handleToggleSelect(item.id)}
                  onFavorite={() => void handleAddToFavorites(item)}
                />
              ))}
            </div>
          )
        ) : (
          feeds.length === 0 ? (
            <div className="empty-state">
              <p>No feeds subscribed</p>
              <p className="hint">Open the app to add feeds</p>
            </div>
          ) : (
            <div className="feed-list">
              {feeds.map((feed) => {
                const isExpanded = expandedFeeds.has(feed.id);
                const feedItems = getItemsForFeed(feed.title);
                const displayItems = showUnreadOnly
                  ? feedItems.filter(item => !item.isRead)
                  : feedItems;
                return (
                  <div key={feed.id} className="feed-folder">
                    <div
                      className={`feed-item ${isExpanded ? 'expanded' : ''}`}
                      onClick={() => toggleFeedExpanded(feed.id)}
                    >
                      <span className="feed-chevron">{isExpanded ? '▼' : '▶'}</span>
                      <span className="feed-type">
                        {feed.type === 'RSS' ? '📰' : feed.type === 'NOSTR_VIDEO' ? '🎬' : '📝'}
                      </span>
                      <span className="feed-title">{feed.title}</span>
                      {feed.unreadCount > 0 && (
                        <span className="badge">{feed.unreadCount}</span>
                      )}
                    </div>
                    {isExpanded && (
                      <div className="feed-items">
                        {displayItems.length === 0 ? (
                          <div className="feed-items-empty">No items</div>
                        ) : (
                          displayItems.slice(0, 10).map(item => (
                            <div
                              key={item.id}
                              className={`feed-subitem ${item.isRead ? 'read' : ''}`}
                              onClick={() => handleOpenItem(item)}
                            >
                              <span className="subitem-title">{item.title}</span>
                              <span className="subitem-time">{formatTimeAgo(item.publishedAt)}</span>
                              {!item.isRead && (
                                <span
                                  className="unread-dot"
                                  onClick={(e) => {
                                    e.stopPropagation();
                                    void handleMarkRead(item.id);
                                  }}
                                  title="Mark as read"
                                />
                              )}
                            </div>
                          ))
                        )}
                      </div>
                    )}
                  </div>
                );
              })}
            </div>
          )
        )}
      </div>

      <div className="actions">
        <button
          className={`btn btn-secondary sync-btn ${syncStatus}`}
          onClick={() => void handleRefresh()}
          disabled={refreshing || !isAuthenticated}
          title="Sync feeds (r)"
        >
          <span className="sync-btn-icon">
            {syncStatus === 'syncing' ? '↻' : syncStatus === 'success' ? '✓' : syncStatus === 'error' ? '!' : '↻'}
          </span>
          <span className="sync-btn-text">
            {syncStatus === 'syncing' ? 'Syncing...' : syncStatus === 'success' ? 'Synced!' : syncStatus === 'error' ? 'Failed' : 'Sync Now'}
          </span>
        </button>
        <button className="btn btn-primary" onClick={handleOpenApp}>
          Open App
        </button>
      </div>

      {isAuthenticated && (
        <footer className="footer">
          <div className="sync-indicator">
            <span className="sync-status">Synced {formatLastSync(lastSync)}</span>
            <span className="sync-next">Next {formatNextSync(lastSync, pollIntervalMinutes)}</span>
          </div>
          <button className="stats-btn" onClick={() => setShowStats(true)} title="View statistics">
            📊
          </button>
        </footer>
      )}

      <Statistics isOpen={showStats} onClose={() => setShowStats(false)} />
    </div>
  );
}

const root = document.getElementById('root');
if (root) {
  createRoot(root).render(
    <StrictMode>
      <ErrorBoundary>
        <App />
      </ErrorBoundary>
    </StrictMode>
  );
}
