'use client'

import { FormattedContent } from '../formatted-content'
import { AiSummaryPanel } from '../ai-summary-panel'
import { AuthorByline } from './author-byline'
import { safeExternalUrl } from '@/lib/safe-url'
import type { HistoryRecord } from '@/lib/reading-history'
import type { AiConfig } from '@/lib/ai/config'
import type { FeedItem } from './types'

interface ArticlePaneProps {
  mobileView: 'list' | 'content'
  selectedItem: string | null
  selectedHistoryItem: HistoryRecord | null
  selectedItemData: FeedItem | null
  selectedItemOriginalUrl: string | null | undefined
  setMobileView: (view: 'list' | 'content') => void
  onToggleReadStatus: (item: FeedItem | null) => void
  onShareToNostr: (item: FeedItem, originalUrl: string | null | undefined) => void
  isSharing: boolean
  shareSuccess: boolean
  onToggleFavorite: (itemId: string, isFavorited: boolean) => void
  aiEnabled: boolean
  showAiPanel: boolean
  setShowAiPanel: React.Dispatch<React.SetStateAction<boolean>>
  aiConfig: AiConfig
}

export function ArticlePane({
  mobileView,
  selectedItem,
  selectedHistoryItem,
  selectedItemData,
  selectedItemOriginalUrl,
  setMobileView,
  onToggleReadStatus,
  onShareToNostr,
  isSharing,
  shareSuccess,
  onToggleFavorite,
  aiEnabled,
  showAiPanel,
  setShowAiPanel,
  aiConfig,
}: ArticlePaneProps) {
  return (
      <div className={`
        ${mobileView === 'list' && (selectedItem || selectedHistoryItem) ? 'hidden md:flex' : 'flex'}
        flex-1 bg-theme-secondary flex-col max-h-screen
        pt-32 md:pt-0
      `}>
        {/* Mobile Back Button */}
        {(selectedItem || selectedHistoryItem) && (
          <button
            onClick={() => setMobileView('list')}
            className="md:hidden fixed top-20 left-4 z-50 p-2.5 bg-theme-surface rounded-full shadow-theme-lg border border-theme-primary"
          >
            <svg className="w-5 h-5 text-theme-primary" fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M15 19l-7-7 7-7" />
            </svg>
          </button>
        )}

        {selectedHistoryItem ? (
          <>
            <div className="bg-theme-surface-raised p-6 md:p-8 border-b border-theme-primary flex-shrink-0 shadow-theme-sm">
              <div className="mx-auto" style={{ maxWidth: 'var(--reading-measure)' }}>
                <h1 className="text-2xl md:text-3xl font-bold text-theme-primary leading-tight mb-4" style={{ fontFamily: 'var(--heading-font)' }}>
                  {selectedHistoryItem.title}
                </h1>
                <div className="flex flex-wrap items-center gap-x-3 gap-y-1 text-sm text-theme-secondary">
                  <span className="font-medium">{selectedHistoryItem.author || selectedHistoryItem.feedTitle}</span>
                  <span className="text-theme-muted">•</span>
                  <span className="tag-badge">{selectedHistoryItem.feedTitle}</span>
                  <span className="text-theme-muted">•</span>
                  <span>Read {new Date(selectedHistoryItem.readAt).toLocaleString()}</span>
                  {selectedHistoryItem.url && (
                    <>
                      <span className="text-theme-muted">•</span>
                      <a
                        href={safeExternalUrl(selectedHistoryItem.url) ?? '#'}
                        target="_blank"
                        rel="noopener noreferrer"
                        className="inline-flex items-center gap-1 text-theme-accent hover:underline"
                      >
                        View Original
                        <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M10 6H6a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2v-4M14 4h6m0 0v6m0-6L10 14" />
                        </svg>
                      </a>
                    </>
                  )}
                </div>
              </div>
            </div>
            <div className="flex-1 overflow-y-auto themed-scrollbar">
              <div className="mx-auto p-6 md:p-8" style={{ maxWidth: 'var(--reading-measure)' }}>
                <article className="article-content-inner p-6 md:p-10 rounded-xl">
                  <p className="prose-theme whitespace-pre-wrap">{selectedHistoryItem.content}</p>
                </article>
              </div>
            </div>
          </>
        ) : selectedItemData ? (
          <>
            <div className="bg-theme-surface-raised p-6 md:p-8 border-b border-theme-primary flex-shrink-0 shadow-theme-sm">
              <div className="mx-auto" style={{ maxWidth: 'var(--reading-measure)' }}>
                <div className="flex items-start justify-between gap-4 mb-4">
                  <h1 className="text-2xl md:text-3xl font-bold text-theme-primary leading-tight" style={{ fontFamily: 'var(--heading-font)' }}>
                    {selectedItemData.title}
                  </h1>
                  <div className="flex items-center gap-1 flex-shrink-0">
                    <button
                      onClick={() => onToggleReadStatus(selectedItemData)}
                      className="btn-theme-secondary text-sm hidden sm:flex items-center gap-2"
                    >
                      {selectedItemData.isRead ? (
                        <>
                          <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M3 19v-8.93a2 2 0 01.89-1.664l7-4.666a2 2 0 012.22 0l7 4.666A2 2 0 0121 10.07V19M3 19a2 2 0 002 2h14a2 2 0 002-2M3 19l6.75-4.5M21 19l-6.75-4.5M3 10l6.75 4.5M21 10l-6.75 4.5m0 0l-1.14.76a2 2 0 01-2.22 0l-1.14-.76" />
                          </svg>
                          <span>Mark Unread</span>
                        </>
                      ) : (
                        <>
                          <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M5 13l4 4L19 7" />
                          </svg>
                          <span>Mark Read</span>
                        </>
                      )}
                    </button>
                    <button
                      onClick={() => onShareToNostr(selectedItemData, selectedItemOriginalUrl)}
                      disabled={isSharing}
                      className="p-2.5 hover:bg-theme-hover rounded-lg transition-colors"
                      title="Share to Nostr"
                    >
                      {isSharing ? (
                        <svg className="w-5 h-5 text-theme-tertiary animate-spin" fill="none" viewBox="0 0 24 24">
                          <circle className="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="4" />
                          <path className="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z" />
                        </svg>
                      ) : shareSuccess ? (
                        <svg className="w-5 h-5 text-theme-accent" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M5 13l4 4L19 7" />
                        </svg>
                      ) : (
                        <svg className="w-5 h-5 text-theme-secondary" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M8.684 13.342C8.886 12.938 9 12.482 9 12c0-.482-.114-.938-.316-1.342m0 2.684a3 3 0 110-2.684m0 2.684l6.632 3.316m-6.632-6l6.632-3.316m0 0a3 3 0 105.367-2.684 3 3 0 00-5.367 2.684zm0 9.316a3 3 0 105.368 2.684 3 3 0 00-5.368-2.684z" />
                        </svg>
                      )}
                    </button>
                    <button
                      onClick={() => onToggleFavorite(selectedItemData.id, selectedItemData.isFavorited || false)}
                      className="p-2.5 hover:bg-theme-hover rounded-lg transition-colors"
                      title={selectedItemData.isFavorited ? 'Remove from favorites' : 'Add to favorites'}
                    >
                      <span className="text-2xl">
                        {selectedItemData.isFavorited ? '⭐' : '☆'}
                      </span>
                    </button>
                    {aiEnabled && (
                      <button
                        onClick={() => setShowAiPanel((v) => !v)}
                        className={`p-2.5 rounded-lg transition-colors ${showAiPanel ? 'bg-theme-accent-light text-theme-accent' : 'hover:bg-theme-hover text-theme-secondary'}`}
                        title={showAiPanel ? 'Hide AI summary' : 'AI summary'}
                      >
                        <span className="text-xl">✨</span>
                      </button>
                    )}
                  </div>
                </div>
                <div className="flex flex-wrap items-center gap-x-3 gap-y-1 text-sm text-theme-secondary">
                  <AuthorByline
                    author={selectedItemData.author}
                    feedType={selectedItemData.feedType}
                    feedTitle={selectedItemData.feedTitle}
                    className="font-medium"
                  />
                  <span className="text-theme-muted">•</span>
                  <span>{selectedItemData.publishedAt.toLocaleDateString('en-US', {
                    weekday: 'long',
                    year: 'numeric',
                    month: 'long',
                    day: 'numeric'
                  })}</span>
                  <span className="text-theme-muted">•</span>
                  <span className="tag-badge">{selectedItemData.feedTitle}</span>
                  {selectedItemOriginalUrl && (
                    <>
                      <span className="text-theme-muted">•</span>
                      <a
                        href={safeExternalUrl(selectedItemOriginalUrl) ?? '#'}
                        target="_blank"
                        rel="noopener noreferrer"
                        className="inline-flex items-center gap-1 text-theme-accent hover:underline"
                      >
                        {selectedItemData.feedType === 'NOSTR' || selectedItemData.feedType === 'NOSTR_VIDEO' ? 'View on Nostr' : 'View Original'}
                        <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M10 6H6a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2v-4M14 4h6m0 0v6m0-6L10 14" />
                        </svg>
                      </a>
                    </>
                  )}
                </div>
              </div>
            </div>
            <div className="flex-1 overflow-y-auto themed-scrollbar">
              <div className="mx-auto p-6 md:p-8" style={{ maxWidth: 'var(--reading-measure)' }}>
                {aiEnabled && showAiPanel && (
                  <AiSummaryPanel
                    key={selectedItemData.guid ?? selectedItemData.id}
                    articleKey={selectedItemData.guid ?? selectedItemData.id}
                    title={selectedItemData.title}
                    text={selectedItemData.content.replace(/<[^>]*>/g, '')}
                    feedTitle={selectedItemData.feedTitle}
                    config={aiConfig}
                  />
                )}
                <article className="article-content-inner p-6 md:p-10 rounded-xl">
                  <FormattedContent
                    content={selectedItemData.content}
                    embedUrl={selectedItemData.embedUrl ?? undefined}
                    thumbnail={selectedItemData.thumbnail ?? undefined}
                    title={selectedItemData.title}
                    className="prose-theme"
                  />
                </article>
              </div>
            </div>
          </>
        ) : (
          <div className="flex-1 flex items-center justify-center">
            <div className="text-center">
              <div className="text-6xl mb-4 opacity-50">📖</div>
              <p className="text-xl font-medium text-theme-secondary mb-2">Select an article to read</p>
              <p className="text-sm text-theme-tertiary">Choose from the list on the left</p>
            </div>
          </div>
        )}
      </div>
  )
}
