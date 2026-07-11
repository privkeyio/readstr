import type { inferRouterOutputs } from '@trpc/server'
import type { AppRouter } from '@/server/api/root'

export interface Category {
  id: string
  name: string
  color: string | null
  icon: string | null
}

export interface Feed {
  id: string
  title: string
  type: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'
  unreadCount: number
  url?: string | null
  npub?: string | null
  tags?: string[]
  categoryId?: string | null
  category?: Category | null
}

type RouterOutputs = inferRouterOutputs<AppRouter>
export type FeedItemsResponse = RouterOutputs['feed']['getFeedItems']
export type FeedItem = FeedItemsResponse['items'][number]
export type FavoritesResponse = RouterOutputs['feed']['getFavorites']
export type FavoriteItem = FavoritesResponse['items'][number]
