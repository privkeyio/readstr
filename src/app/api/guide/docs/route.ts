import { NextResponse } from 'next/server'

const corsHeaders = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type',
}

export async function OPTIONS() {
  return new NextResponse(null, { status: 204, headers: corsHeaders })
}

export async function GET() {
  const docs = {
    name: 'Readstr Guide API',
    version: '1.0',
    description: 'REST API for accessing the Readstr guide - a curated directory of Nostr long-form content creators.',
    baseUrl: 'https://readstr.privkey.io:8444/api/guide',
    
    endpoints: [
      {
        path: '/api/guide',
        method: 'GET',
        description: 'Get list of guide feeds with optional filtering and sorting',
        parameters: [
          {
            name: 'tags',
            type: 'string',
            required: false,
            description: 'Comma-separated list of tags to filter by',
            example: 'bitcoin,nostr,tech',
          },
          {
            name: 'orderBy',
            type: 'string',
            required: false,
            default: 'popular',
            options: ['popular', 'newest', 'recent_posts'],
            description: 'Sort order for feeds',
          },
          {
            name: 'limit',
            type: 'number',
            required: false,
            default: 50,
            min: 1,
            max: 100,
            description: 'Number of feeds to return',
          },
          {
            name: 'format',
            type: 'string',
            required: false,
            default: 'full',
            options: ['full', 'minimal'],
            description: 'Response format - minimal excludes about and timestamps',
          },
        ],
        exampleRequest: '/api/guide?tags=bitcoin&orderBy=popular&limit=20',
        exampleResponse: {
          success: true,
          data: {
            feeds: [
              {
                npub: 'npub1...',
                displayName: 'Satoshi',
                picture: 'https://...',
                tags: ['bitcoin', 'tech'],
                subscriberCount: 42,
                postCount: 15,
                about: 'Writing about Bitcoin...',
                subscribeUrl: 'https://readstr.privkey.io:8444/subscribe?npub=npub1...',
                webUrl: 'https://readstr.privkey.io:8444/guide#npub1...',
                rssUrl: 'https://readstr.privkey.io:8444/api/nostr-rss?npub=npub1...',
              },
            ],
            totalCount: 1,
            availableTags: [
              { tag: 'bitcoin', count: 10 },
              { tag: 'nostr', count: 8 },
            ],
          },
        },
      },
      {
        path: '/api/guide/{npub}',
        method: 'GET',
        description: 'Get a single guide feed by npub with optional recent posts',
        parameters: [
          {
            name: 'npub',
            type: 'string',
            required: true,
            description: 'The npub of the feed author',
            example: 'npub1cj8znuztfqkvq89pl8hceph0svvvqk0qay6nydgk9uyq7fhpfsgsqwrz4u',
          },
          {
            name: 'includePosts',
            type: 'boolean',
            required: false,
            default: false,
            description: 'Include recent long-form posts from Nostr relays',
          },
          {
            name: 'postLimit',
            type: 'number',
            required: false,
            default: 5,
            min: 1,
            max: 20,
            description: 'Number of posts to include (if includePosts=true)',
          },
        ],
        exampleRequest: '/api/guide/npub1abc...?includePosts=true&postLimit=5',
        exampleResponse: {
          success: true,
          data: {
            npub: 'npub1...',
            displayName: 'Author Name',
            about: 'Bio text...',
            picture: 'https://...',
            tags: ['bitcoin', 'tech'],
            subscriberCount: 42,
            postCount: 15,
            lastPublishedAt: '2025-11-26T12:00:00.000Z',
            subscribeUrl: 'https://readstr.privkey.io:8444/subscribe?npub=npub1...',
            recentPosts: [
              {
                id: 'abc123...',
                title: 'My Latest Article',
                summary: 'Article preview...',
                publishedAt: '2025-11-26T12:00:00.000Z',
                tags: ['bitcoin'],
              },
            ],
          },
        },
      },
    ],
    
    oneClickSubscription: {
      description: 'Enable one-click subscription from native apps using deep links',
      url: 'https://readstr.privkey.io:8444/subscribe',
      parameters: [
        {
          name: 'npub',
          type: 'string',
          required: true,
          description: 'The npub to subscribe to',
        },
        {
          name: 'tags',
          type: 'string',
          required: false,
          description: 'Comma-separated tags to apply to the subscription',
        },
        {
          name: 'return',
          type: 'string',
          required: false,
          description: 'URL to redirect to after successful subscription',
        },
      ],
      example: 'https://readstr.privkey.io:8444/subscribe?npub=npub1abc...&tags=bitcoin,nostr&return=https://yourapp.com/success',
      behavior: [
        'If user is already logged in, subscription happens automatically',
        'If not logged in, prompts user to connect with Nostr extension',
        'After success, redirects to return URL or /reader',
      ],
    },
    
    rssFeeds: {
      description: 'Get RSS feeds for individual Nostr authors',
      url: 'https://readstr.privkey.io:8444/api/nostr-rss',
      parameters: [
        {
          name: 'npub',
          type: 'string',
          required: true,
          description: 'The npub of the author',
        },
        {
          name: 'tags',
          type: 'string',
          required: false,
          description: 'Tags to include in the RSS metadata',
        },
      ],
      example: 'https://readstr.privkey.io:8444/api/nostr-rss?npub=npub1abc...',
    },
    
    rateLimits: {
      description: 'API rate limits',
      limits: [
        'No authentication required',
        'General limit: 100 requests per minute per IP',
        'Burst limit: 10 requests per second',
      ],
    },
    
    cors: {
      description: 'CORS is enabled for all origins',
      allowedOrigins: '*',
      allowedMethods: ['GET', 'POST', 'OPTIONS'],
    },
  }
  
  return NextResponse.json(docs, { headers: corsHeaders })
}
