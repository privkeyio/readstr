import { NextRequest, NextResponse } from 'next/server'
import { db } from '@/server/db'
import { getNostrFetcher } from '@/lib/nostr-fetcher'
import { requireCronSecret } from '@/server/auth/cron'

interface SeedFeed {
  npub: string
  tags: string[]
}

const seedFeeds: SeedFeed[] = [
  {
    npub: 'npub1sg6plzptd64u62a878hep2kev88swjh3tw00gjsfl8f237lmu63q0uf63m', // Derek Ross
    tags: ['nostr', 'bitcoin', 'technology', 'tutorials'],
  },
  {
    npub: 'npub1az9xj85cmxv8e9j9y80lvqp97crsqdu2fpu3srwthd99qfu9qsgstam8y8', // Max DeMarco
    tags: ['bitcoin', 'economics', 'freedom', 'nostr'],
  },
  {
    npub: 'npub1g53mukxnjkcmr94fhryzkqutdz2ukq4ks0gvy5af25rgmwsl4ngq43drvk', // Gigi
    tags: ['bitcoin', 'philosophy', 'freedom', 'technology'],
  },
  {
    npub: 'npub1qny3tkh0acurzla8x3zy4nhrjz5zd8l9sy9jys09umwng00manysew95gx', // Jeff Booth
    tags: ['bitcoin', 'economics', 'technology', 'deflation'],
  },
  {
    npub: 'npub1rtlqca8r6auyaw5n5h3l5422dm4sry5dzfee4696fqe8s6qgudks7djtfs', // ODELL
    tags: ['bitcoin', 'privacy', 'freedom', 'nostr'],
  },
  {
    npub: 'npub1gcxzte5zlkncx26j68ez60fzkvtkm9e0vrwdcvsjakxf9mu9qewqlfnj5z', // Mike Dilger
    tags: ['nostr', 'development', 'technology', 'privacy'],
  },
  {
    npub: 'npub1acg6thl5psv62405rljzkj8spesceyfz2c32udakc2ak0dmvfeyse9p35c', // Lyn Alden
    tags: ['bitcoin', 'economics', 'finance', 'macro'],
  },
  {
    npub: 'npub1hu3hdctm5nkzd8gslnyedfr5ddz3z547jqcl5j88g4fame2jd08qep6kvr', // Preston Pysh
    tags: ['bitcoin', 'investing', 'economics', 'finance'],
  },
  {
    npub: 'npub1cn4t4cd78nm900qc2hhqte5aa8c9njm6qkfzw95tszufwcwtcnsq7g3vle', // ZEUS
    tags: ['bitcoin', 'lightning', 'technology', 'wallets'],
  },
  {
    npub: 'npub1xnf02f60r9v0e5kty33a404dm79zr7z2eepyrk5gsq3m7pwvsz2sazlpr5', // Marty Bent
    tags: ['bitcoin', 'freedom', 'energy', 'finance'],
  },
]

export async function GET(request: NextRequest) {
  const authFailure = requireCronSecret(request)
  if (authFailure) return authFailure

  const results = {
    total: seedFeeds.length,
    success: 0,
    failed: 0,
    details: [] as any[],
  }

  const nostrFetcher = getNostrFetcher()

  for (const seedFeed of seedFeeds) {
    try {
      // Check if already exists
      const existing = await db.guideFeed.findUnique({
        where: { npub: seedFeed.npub },
      })

      if (existing) {
        results.details.push({
          npub: seedFeed.npub.slice(0, 20) + '...',
          status: 'skipped',
          reason: 'Already exists',
          name: existing.displayName,
        })
        continue
      }

      // Fetch profile
      const profile = await nostrFetcher.getProfile(seedFeed.npub)
      
      if (!profile) {
        results.failed++
        results.details.push({
          npub: seedFeed.npub.slice(0, 20) + '...',
          status: 'failed',
          reason: 'Could not fetch profile',
        })
        continue
      }

      // Fetch posts
      const posts = await nostrFetcher.fetchLongFormPosts(seedFeed.npub, 20)
      
      if (posts.length === 0) {
        results.failed++
        results.details.push({
          npub: seedFeed.npub.slice(0, 20) + '...',
          status: 'failed',
          reason: 'No long-form posts found',
          name: profile.name,
        })
        continue
      }

      // Find most recent post
      const lastPublishedAt = posts.reduce((latest, post) => 
        post.publishedAt > latest ? post.publishedAt : latest, 
        posts[0].publishedAt
      )

      // Create guide entry
      await db.guideFeed.create({
        data: {
          npub: seedFeed.npub,
          displayName: profile.name || seedFeed.npub.slice(0, 16) + '...',
          about: profile.about,
          picture: profile.picture,
          tags: seedFeed.tags,
          submittedBy: null,
          lastPublishedAt,
          postCount: posts.length,
          subscriberCount: 0,
        },
      })

      results.success++
      results.details.push({
        npub: seedFeed.npub.slice(0, 20) + '...',
        status: 'success',
        name: profile.name,
        posts: posts.length,
        tags: seedFeed.tags,
      })

      // Small delay
      await new Promise(resolve => setTimeout(resolve, 500))

    } catch (error) {
      results.failed++
      results.details.push({
        npub: seedFeed.npub.slice(0, 20) + '...',
        status: 'error',
        error: error instanceof Error ? error.message : 'Unknown error',
      })
    }
  }

  nostrFetcher.close()

  return NextResponse.json(results)
}
