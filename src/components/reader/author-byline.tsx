'use client'

import { useNostrProfile } from '@/lib/nostr-profile'

export function AuthorByline({
  author,
  feedType,
  feedTitle,
  className,
}: {
  author: string | null
  feedType: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'
  feedTitle: string
  className: string
}) {
  const isNostr = feedType === 'NOSTR' || feedType === 'NOSTR_VIDEO'
  const { profile } = useNostrProfile(isNostr ? author : null)

  if (!isNostr) {
    return <span className={className}>{author}</span>
  }

  const label = profile?.name || feedTitle || author

  return <span className={className}>{label}</span>
}
