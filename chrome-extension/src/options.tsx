import { StrictMode, useState, useEffect, useCallback, useRef } from 'react';
import { createRoot } from 'react-dom/client';
import type { LocalFeed, SyncSettings, ExtensionSettings, Feed, NostrAuthData } from './types';
import { isValidNsec } from './nostr';
import { normalizeWebAppUrl } from './utils/webAppUrl';

const DEFAULT_SYNC_SETTINGS: SyncSettings = {
  webAppUrl: 'https://readstr.privkey.io:8444',
  pollIntervalMinutes: 5,
  notificationsEnabled: true,
  notifyOnNewItems: true,
  maxNotificationsPerRefresh: 3,
};

function generateId(): string {
  return `${Date.now()}-${Math.random().toString(36).slice(2, 9)}`;
}

function isValidUrl(string: string): boolean {
  try {
    new URL(string);
    return true;
  } catch {
    return false;
  }
}

function isValidNpub(string: string): boolean {
  return /^(npub1|nprofile1)[a-z0-9]{58,}$/i.test(string.trim());
}

function parseOPML(xmlString: string): LocalFeed[] {
  const parser = new DOMParser();
  const doc = parser.parseFromString(xmlString, 'text/xml');
  const feeds: LocalFeed[] = [];

  const outlines = doc.querySelectorAll('outline[xmlUrl]');
  outlines.forEach((outline) => {
    const xmlUrl = outline.getAttribute('xmlUrl');
    const title = outline.getAttribute('title') || outline.getAttribute('text') || 'Untitled';

    if (xmlUrl) {
      feeds.push({
        id: generateId(),
        type: 'RSS',
        title,
        url: xmlUrl,
        npub: null,
        addedAt: new Date().toISOString(),
      });
    }
  });

  return feeds;
}

function generateOPML(feeds: LocalFeed[]): string {
  const rssFeeds = feeds.filter((f) => f.type === 'RSS' && f.url);
  const lines = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<opml version="2.0">',
    '  <head>',
    '    <title>Readstr Subscriptions</title>',
    `    <dateCreated>${new Date().toISOString()}</dateCreated>`,
    '  </head>',
    '  <body>',
  ];

  rssFeeds.forEach((feed) => {
    if (!feed.url) return;
    const escapedTitle = feed.title.replace(/&/g, '&amp;').replace(/"/g, '&quot;');
    const escapedUrl = feed.url.replace(/&/g, '&amp;');
    lines.push(`    <outline type="rss" text="${escapedTitle}" title="${escapedTitle}" xmlUrl="${escapedUrl}" />`);
  });

  lines.push('  </body>', '</opml>');
  return lines.join('\n');
}

interface ToastState {
  message: string;
  type: 'success' | 'error';
}

function Toast({ message, type, onClose }: ToastState & { onClose: () => void }) {
  useEffect(() => {
    const timer = setTimeout(onClose, 3000);
    return () => clearTimeout(timer);
  }, [onClose]);

  return (
    <div className={`toast toast-${type}`}>
      {message}
    </div>
  );
}

function FeedItem({ feed, onRemove }: { feed: LocalFeed; onRemove: (id: string) => void }) {
  const icon = feed.type === 'RSS' ? '📰' : '📝';
  const subtitle = feed.type === 'RSS' ? feed.url : feed.npub;

  return (
    <div className="feed-item">
      <span className="feed-icon">{icon}</span>
      <div className="feed-details">
        <div className="feed-title">{feed.title}</div>
        <div className="feed-url">{subtitle}</div>
      </div>
      <div className="feed-actions">
        <button className="btn btn-danger btn-small" onClick={() => onRemove(feed.id)}>
          Remove
        </button>
      </div>
    </div>
  );
}

function App() {
  const [localFeeds, setLocalFeeds] = useState<LocalFeed[]>([]);
  const [syncSettings, setSyncSettings] = useState<SyncSettings>(DEFAULT_SYNC_SETTINGS);
  const [isAuthenticated, setIsAuthenticated] = useState(false);
  const [nostrAuth, setNostrAuth] = useState<NostrAuthData | null>(null);
  const [syncedFeeds, setSyncedFeeds] = useState<Feed[]>([]);
  const [loading, setLoading] = useState(true);
  const [syncing, setSyncing] = useState(false);
  const [loggingIn, setLoggingIn] = useState(false);
  const [toast, setToast] = useState<ToastState | null>(null);

  const [feedInput, setFeedInput] = useState('');
  const [feedType, setFeedType] = useState<'RSS' | 'NOSTR'>('RSS');
  const [feedTitle, setFeedTitle] = useState('');

  const [nsecInput, setNsecInput] = useState('');
  const [showNsecWarning, setShowNsecWarning] = useState(false);

  const fileInputRef = useRef<HTMLInputElement>(null);

  const showToast = useCallback((message: string, type: 'success' | 'error') => {
    setToast({ message, type });
  }, []);

  const loadData = useCallback(async () => {
    try {
      const [syncResult, localResult] = await Promise.all([
        chrome.storage.sync.get(['localFeeds', 'syncSettings']),
        chrome.storage.local.get(['feeds', 'authToken', 'settings', 'nostrAuth']),
      ]);

      const localFeeds = (syncResult['localFeeds'] as LocalFeed[] | undefined) ?? [];
      const syncSettings = (syncResult['syncSettings'] as SyncSettings | undefined) ?? DEFAULT_SYNC_SETTINGS;
      const syncedFeeds = (localResult['feeds'] as Feed[] | undefined) ?? [];
      const authToken = localResult['authToken'] as string | undefined;
      const settings = localResult['settings'] as ExtensionSettings | undefined;
      const nostrAuthData = localResult['nostrAuth'] as NostrAuthData | undefined;

      setLocalFeeds(localFeeds);
      setSyncSettings({
        ...DEFAULT_SYNC_SETTINGS,
        ...syncSettings,
        webAppUrl: settings?.webAppUrl ?? syncSettings.webAppUrl ?? DEFAULT_SYNC_SETTINGS.webAppUrl,
        pollIntervalMinutes: settings?.pollIntervalMinutes ?? syncSettings.pollIntervalMinutes,
        notificationsEnabled: settings?.notificationsEnabled ?? syncSettings.notificationsEnabled,
      });
      setSyncedFeeds(syncedFeeds);
      setNostrAuth(nostrAuthData ?? null);
      setIsAuthenticated(!!authToken || !!(nostrAuthData?.pubkey));
    } catch (err) {
      console.error('Failed to load data:', err);
      showToast('Failed to load settings', 'error');
    } finally {
      setLoading(false);
    }
  }, [showToast]);

  useEffect(() => {
    void loadData();
  }, [loadData]);

  const saveLocalFeeds = async (feeds: LocalFeed[]) => {
    await chrome.storage.sync.set({ localFeeds: feeds });
    setLocalFeeds(feeds);
  };

  const saveSyncSettings = async (settings: SyncSettings): Promise<boolean> => {
    const normalizedUrl = normalizeWebAppUrl(settings.webAppUrl);
    if (!normalizedUrl) {
      return false;
    }
    const normalized = { ...settings, webAppUrl: normalizedUrl };
    await chrome.storage.sync.set({ syncSettings: normalized });
    const existingSettings = await chrome.storage.local.get(['settings']);
    const current = existingSettings['settings'] as ExtensionSettings | undefined;
    await chrome.storage.local.set({
      settings: {
        webAppUrl: normalized.webAppUrl,
        pollIntervalMinutes: normalized.pollIntervalMinutes,
        notificationsEnabled: normalized.notificationsEnabled,
        notifyOnNewItems: normalized.notifyOnNewItems,
        maxNotificationsPerRefresh: normalized.maxNotificationsPerRefresh,
        lastSyncTime: current?.lastSyncTime ?? null,
        theme: current?.theme ?? 'system',
        showUnreadOnly: current?.showUnreadOnly ?? false,
      } satisfies ExtensionSettings,
    });
    setSyncSettings(normalized);
    await chrome.runtime.sendMessage({ type: 'UPDATE_SETTINGS', settings: normalized });
    return true;
  };

  const handleAddFeed = async () => {
    const input = feedInput.trim();
    if (!input) {
      showToast('Please enter a URL or npub', 'error');
      return;
    }

    if (feedType === 'RSS') {
      if (!isValidUrl(input)) {
        showToast('Please enter a valid URL', 'error');
        return;
      }
      const exists = localFeeds.some((f) => f.url === input);
      if (exists) {
        showToast('This feed is already added', 'error');
        return;
      }
    } else {
      if (!isValidNpub(input)) {
        showToast('Please enter a valid npub or nprofile', 'error');
        return;
      }
      const exists = localFeeds.some((f) => f.npub === input);
      if (exists) {
        showToast('This Nostr user is already added', 'error');
        return;
      }
    }

    const newFeed: LocalFeed = {
      id: generateId(),
      type: feedType,
      title: feedTitle.trim() || (feedType === 'RSS' ? new URL(input).hostname : input.slice(0, 16) + '...'),
      url: feedType === 'RSS' ? input : null,
      npub: feedType === 'NOSTR' ? input : null,
      addedAt: new Date().toISOString(),
    };

    await saveLocalFeeds([...localFeeds, newFeed]);
    setFeedInput('');
    setFeedTitle('');
    showToast('Feed added successfully', 'success');
  };

  const handleRemoveFeed = async (id: string) => {
    const filtered = localFeeds.filter((f) => f.id !== id);
    await saveLocalFeeds(filtered);
    showToast('Feed removed', 'success');
  };

  const handleImportOPML = (event: React.ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    if (!file) return;

    const reader = new FileReader();
    reader.onload = async (e) => {
      try {
        const content = e.target?.result as string;
        const imported = parseOPML(content);

        if (imported.length === 0) {
          showToast('No feeds found in OPML file', 'error');
          return;
        }

        const existingUrls = new Set(localFeeds.filter((f) => f.url).map((f) => f.url));
        const newFeeds = imported.filter((f) => !existingUrls.has(f.url));

        if (newFeeds.length === 0) {
          showToast('All feeds already exist', 'error');
          return;
        }

        await saveLocalFeeds([...localFeeds, ...newFeeds]);
        showToast(`Imported ${newFeeds.length} feeds`, 'success');
      } catch (err) {
        console.error('OPML import failed:', err);
        showToast('Failed to parse OPML file', 'error');
      }
    };
    reader.readAsText(file);

    if (fileInputRef.current) {
      fileInputRef.current.value = '';
    }
  };

  const handleExportOPML = () => {
    const opml = generateOPML(localFeeds);
    const blob = new Blob([opml], { type: 'application/xml' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'readstr-subscriptions.opml';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    showToast('OPML exported', 'success');
  };

  const handleSyncWithAccount = async () => {
    if (!isAuthenticated) {
      window.open(syncSettings.webAppUrl, '_blank');
      return;
    }

    setSyncing(true);
    try {
      const response = await chrome.runtime.sendMessage({ type: 'REFRESH_FEEDS' });
      if (response.success) {
        await loadData();
        showToast('Synced with account', 'success');
      } else {
        showToast(response.error || 'Sync failed', 'error');
      }
    } catch (err) {
      console.error('Sync failed:', err);
      showToast('Sync failed', 'error');
    } finally {
      setSyncing(false);
    }
  };

  const handleSettingChange = async (key: keyof SyncSettings, value: string | number | boolean) => {
    const updated = { ...syncSettings, [key]: value };
    if (!(await saveSyncSettings(updated))) {
      showToast('Enter a valid https URL (http allowed only for localhost)', 'error');
      return;
    }
    showToast('Settings saved', 'success');
  };

  const handleWebAppUrlChange = (value: string) => {
    setSyncSettings((prev) => ({ ...prev, webAppUrl: value }));
  };

  const handleWebAppUrlBlur = async () => {
    const url = syncSettings.webAppUrl.trim();
    if (!(await saveSyncSettings({ ...syncSettings, webAppUrl: url }))) {
      showToast('Enter a valid https URL (http allowed only for localhost)', 'error');
      return;
    }
    showToast('Settings saved', 'success');
  };

  const handleNostrLogin = async () => {
    const nsec = nsecInput.trim();
    if (!nsec) {
      showToast('Please enter your nsec key', 'error');
      return;
    }

    if (!isValidNsec(nsec)) {
      showToast('Invalid nsec key format', 'error');
      return;
    }

    setLoggingIn(true);
    try {
      const response = await chrome.runtime.sendMessage({
        type: 'NOSTR_LOGIN',
        method: 'nsec',
        nsec,
      });

      if (response.success) {
        setNsecInput('');
        setShowNsecWarning(false);
        await loadData();
        showToast('Logged in successfully', 'success');
      } else {
        showToast(response.error || 'Login failed', 'error');
      }
    } catch (err) {
      console.error('Login failed:', err);
      showToast('Login failed', 'error');
    } finally {
      setLoggingIn(false);
    }
  };

  const handleNostrLogout = async () => {
    try {
      await chrome.runtime.sendMessage({ type: 'NOSTR_LOGOUT' });
      setNostrAuth(null);
      setIsAuthenticated(false);
      setSyncedFeeds([]);
      showToast('Logged out', 'success');
    } catch (err) {
      console.error('Logout failed:', err);
      showToast('Logout failed', 'error');
    }
  };

  if (loading) {
    return (
      <div className="container">
        <div className="section">
          <p>Loading...</p>
        </div>
      </div>
    );
  }

  return (
    <div className="container">
      <header className="header">
        <h1>Readstr Options</h1>
        <p>Manage your feeds and extension settings</p>
      </header>

      <section className="section">
        <h2 className="section-title">Nostr Account</h2>
        {nostrAuth?.pubkey ? (
          <div className="sync-section">
            <div className="sync-info">
              <span className="status-badge status-connected">● Connected</span>
              <p className="npub-display">{nostrAuth.npub}</p>
              <p>Syncing {syncedFeeds.length} feeds from your account</p>
            </div>
            <div className="btn-group">
              <button
                className="btn btn-primary"
                onClick={() => void handleSyncWithAccount()}
                disabled={syncing}
              >
                {syncing ? 'Syncing...' : 'Sync Now'}
              </button>
              <button
                className="btn btn-danger"
                onClick={() => void handleNostrLogout()}
              >
                Logout
              </button>
            </div>
          </div>
        ) : (
          <div className="login-section">
            <p className="form-hint" style={{ marginBottom: '16px' }}>
              Sign in with your Nostr key to sync feeds and read state
            </p>

            {!showNsecWarning ? (
              <button
                className="btn btn-secondary"
                onClick={() => setShowNsecWarning(true)}
              >
                Login with nsec
              </button>
            ) : (
              <div className="nsec-login">
                <div className="warning-box">
                  <strong>Security Warning:</strong> Your private key will be stored locally in the extension.
                  Only use this on trusted devices. For better security, consider using a NIP-07 signer extension.
                </div>
                <div className="form-group">
                  <label className="form-label">nsec Key</label>
                  <input
                    type="password"
                    value={nsecInput}
                    onChange={(e) => setNsecInput(e.target.value)}
                    placeholder="nsec1..."
                    autoComplete="off"
                  />
                </div>
                <div className="btn-group">
                  <button
                    className="btn btn-primary"
                    onClick={() => void handleNostrLogin()}
                    disabled={loggingIn || !nsecInput.trim()}
                  >
                    {loggingIn ? 'Logging in...' : 'Login'}
                  </button>
                  <button
                    className="btn btn-secondary"
                    onClick={() => {
                      setShowNsecWarning(false);
                      setNsecInput('');
                    }}
                  >
                    Cancel
                  </button>
                </div>
              </div>
            )}

            <div className="divider">
              <span>or</span>
            </div>

            <button
              className="btn btn-secondary"
              onClick={() => window.open(syncSettings.webAppUrl, '_blank')}
            >
              Open Web App to Sign In
            </button>
          </div>
        )}
      </section>

      <section className="section">
        <h2 className="section-title">Local Feeds</h2>
        <p className="form-hint" style={{ marginBottom: '16px' }}>
          Add feeds that are stored locally in your browser (synced via Chrome sync)
        </p>

        <div className="form-row">
          <div className="form-group">
            <label className="form-label">Type</label>
            <select value={feedType} onChange={(e) => setFeedType(e.target.value as 'RSS' | 'NOSTR')}>
              <option value="RSS">RSS Feed</option>
              <option value="NOSTR">Nostr User</option>
            </select>
          </div>
          <div className="form-group" style={{ flex: 2 }}>
            <label className="form-label">{feedType === 'RSS' ? 'Feed URL' : 'npub or nprofile'}</label>
            <input
              type="text"
              value={feedInput}
              onChange={(e) => setFeedInput(e.target.value)}
              placeholder={feedType === 'RSS' ? 'https://example.com/feed.xml' : 'npub1...'}
            />
          </div>
        </div>

        <div className="form-row" style={{ marginTop: '12px' }}>
          <div className="form-group" style={{ flex: 2 }}>
            <label className="form-label">Title (optional)</label>
            <input
              type="text"
              value={feedTitle}
              onChange={(e) => setFeedTitle(e.target.value)}
              placeholder="Custom feed name"
            />
          </div>
          <button className="btn btn-primary" onClick={() => void handleAddFeed()}>
            Add Feed
          </button>
        </div>

        <div className="feed-list">
          {localFeeds.length === 0 ? (
            <div className="empty-state">
              <p>No local feeds added yet</p>
            </div>
          ) : (
            localFeeds.map((feed) => (
              <FeedItem key={feed.id} feed={feed} onRemove={(id) => void handleRemoveFeed(id)} />
            ))
          )}
        </div>

        <div className="btn-group">
          <input
            ref={fileInputRef}
            type="file"
            accept=".opml,.xml"
            className="file-input"
            onChange={handleImportOPML}
          />
          <button className="btn btn-secondary" onClick={() => fileInputRef.current?.click()}>
            Import OPML
          </button>
          <button
            className="btn btn-secondary"
            onClick={handleExportOPML}
            disabled={localFeeds.filter((f) => f.type === 'RSS').length === 0}
          >
            Export OPML
          </button>
        </div>
      </section>

      <section className="section">
        <h2 className="section-title">Refresh Settings</h2>

        <div className="form-group">
          <label className="form-label">Refresh Interval</label>
          <select
            value={syncSettings.pollIntervalMinutes}
            onChange={(e) => void handleSettingChange('pollIntervalMinutes', Number(e.target.value))}
          >
            <option value={1}>Every 1 minute</option>
            <option value={5}>Every 5 minutes</option>
            <option value={10}>Every 10 minutes</option>
            <option value={15}>Every 15 minutes</option>
            <option value={30}>Every 30 minutes</option>
            <option value={60}>Every hour</option>
          </select>
          <p className="form-hint">How often to check for new items</p>
        </div>
      </section>

      <section className="section">
        <h2 className="section-title">Notification Settings</h2>

        <div className="form-group">
          <div className="checkbox-group">
            <input
              type="checkbox"
              id="notificationsEnabled"
              checked={syncSettings.notificationsEnabled}
              onChange={(e) => void handleSettingChange('notificationsEnabled', e.target.checked)}
            />
            <label htmlFor="notificationsEnabled">Enable notifications</label>
          </div>
          <p className="form-hint">Show browser notifications for new items</p>
        </div>

        <div className="form-group">
          <div className="checkbox-group">
            <input
              type="checkbox"
              id="notifyOnNewItems"
              checked={syncSettings.notifyOnNewItems}
              disabled={!syncSettings.notificationsEnabled}
              onChange={(e) => void handleSettingChange('notifyOnNewItems', e.target.checked)}
            />
            <label htmlFor="notifyOnNewItems">Notify on new items</label>
          </div>
          <p className="form-hint">Show a notification when new feed items arrive</p>
        </div>

        <div className="form-group">
          <label className="form-label">Max notifications per refresh</label>
          <select
            value={syncSettings.maxNotificationsPerRefresh}
            disabled={!syncSettings.notificationsEnabled}
            onChange={(e) => void handleSettingChange('maxNotificationsPerRefresh', Number(e.target.value))}
          >
            <option value={1}>1 notification</option>
            <option value={3}>3 notifications</option>
            <option value={5}>5 notifications</option>
            <option value={10}>10 notifications</option>
          </select>
          <p className="form-hint">Limit notifications to prevent spam</p>
        </div>
      </section>

      <section className="section">
        <h2 className="section-title">Web App URL</h2>

        <div className="form-group">
          <label className="form-label">Readstr URL</label>
          <input
            type="url"
            value={syncSettings.webAppUrl}
            onChange={(e) => handleWebAppUrlChange(e.target.value)}
            onBlur={() => void handleWebAppUrlBlur()}
            placeholder="https://readstr.privkey.io:8444"
          />
          <p className="form-hint">URL of your Readstr instance (for self-hosted)</p>
        </div>
      </section>

      {toast && <Toast {...toast} onClose={() => setToast(null)} />}
    </div>
  );
}

const root = document.getElementById('root');
if (root) {
  createRoot(root).render(
    <StrictMode>
      <App />
    </StrictMode>
  );
}
