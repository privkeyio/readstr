import './src/env.mjs'
import nextPWA from 'next-pwa'

const withPWA = nextPWA({
  dest: 'public',
  register: true,
  skipWaiting: true,
  disable: process.env.NODE_ENV === 'development',
})

// Conservative CSP. img-src/media-src/connect-src stay permissive because the
// app renders remote feed images and connects to arbitrary Nostr relays over
// websockets. script-src keeps 'unsafe-inline'/'unsafe-eval' since Next.js
// injects inline bootstrap scripts. frame-src mirrors the embed allow-list from
// the (host-managed) Caddyfile for YouTube and Rumble.
const contentSecurityPolicy = [
  "default-src 'self'",
  "script-src 'self' 'unsafe-inline' 'unsafe-eval'",
  "style-src 'self' 'unsafe-inline'",
  'img-src * data: blob:',
  'media-src *',
  'connect-src * wss:',
  "frame-src 'self' https://www.youtube.com https://youtube.com https://www.youtube-nocookie.com https://rumble.com https://*.rumble.com",
  "frame-ancestors 'none'",
  "base-uri 'self'",
  "form-action 'self'",
].join('; ')

const securityHeaders = [
  {
    key: 'Strict-Transport-Security',
    value: 'max-age=31536000; includeSubDomains; preload',
  },
  { key: 'X-Content-Type-Options', value: 'nosniff' },
  { key: 'Referrer-Policy', value: 'strict-origin-when-cross-origin' },
  { key: 'Content-Security-Policy', value: contentSecurityPolicy },
]

/** @type {import('next').NextConfig} */
const nextConfig = {
  serverExternalPackages: ['@prisma/client'],
  output: 'standalone',
  images: {
    remotePatterns: [
      {
        protocol: 'https',
        hostname: 'images.unsplash.com',
      },
    ],
  },
  async headers() {
    return [
      {
        source: '/:path*',
        headers: securityHeaders,
      },
    ]
  },
  // Empty turbopack config to suppress the warning
  turbopack: {},
}

export default withPWA(nextConfig)
