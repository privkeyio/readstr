'use client'

import { AuthShowcase } from '@/components/auth-showcase'
import { BrandHeader } from '@/components/brand-header'
import { useNostrAuth } from '@/contexts/NostrAuthContext'
import { useRouter } from 'next/navigation'
import { useEffect } from 'react'
import Link from 'next/link'

export default function HomePage() {
  const { isConnected } = useNostrAuth()
  const router = useRouter()

  useEffect(() => {
    if (isConnected) {
      router.push('/reader')
    }
  }, [isConnected, router])

  return (
    <main className="font-brand relative flex min-h-screen flex-col items-center bg-gradient-to-br from-[#1a1a1a] via-[#0d1117] to-[#161b22] text-white">
      {/* Top nav / brand bar */}
      <BrandHeader cta={{ href: '/guide', label: 'Explore the Guide' }} />

      <div className="mx-auto flex w-full max-w-6xl flex-col items-center justify-center gap-16 px-4 py-20">
        {/* Hero */}
        <section className="flex flex-col items-center text-center">
          <h1 className="text-5xl font-extrabold tracking-tight sm:text-7xl">
            <span className="bg-gradient-to-br from-[#27ae60] via-[#2ecc71] to-[#58d68d] bg-clip-text text-transparent">
              Read
            </span>
            str
          </h1>
          <p className="mt-6 max-w-2xl text-xl font-medium text-white/90 sm:text-2xl">
            Your RSS + Nostr feed reader. Sovereign reading, your keys, your stack.
          </p>
          <p className="mt-4 max-w-2xl text-base leading-relaxed text-[#B3B3B3]">
            Subscribe to RSS feeds and Nostr long-form content (NIP-23) in one unified reader.
            Experience the best of traditional blogging and decentralized publishing.
          </p>
        </section>

        {/* Feature cards */}
        <section className="grid w-full max-w-3xl gap-6 md:grid-cols-2">
          <div className="group rounded-2xl border border-[#27ae60]/15 bg-white/[0.08] p-6 backdrop-blur-xl transition-all duration-300 hover:-translate-y-1 hover:border-[#27ae60]/30 hover:bg-[#27ae60]/[0.08]">
            <h3 className="mb-2 text-lg font-semibold text-white">RSS Feeds</h3>
            <p className="text-sm leading-relaxed text-[#B3B3B3]">
              Subscribe to your favorite blogs, news sites, and traditional RSS feeds in one place.
            </p>
          </div>
          <div className="group rounded-2xl border border-[#27ae60]/15 bg-white/[0.08] p-6 backdrop-blur-xl transition-all duration-300 hover:-translate-y-1 hover:border-[#27ae60]/30 hover:bg-[#27ae60]/[0.08]">
            <h3 className="mb-2 text-lg font-semibold text-white">Nostr Content</h3>
            <p className="text-sm leading-relaxed text-[#B3B3B3]">
              Follow Nostr npubs for their long-form articles and blog posts (NIP-23).
            </p>
          </div>
        </section>

        {/* Auth */}
        <section className="w-full max-w-md rounded-2xl border border-[#27ae60]/15 bg-white/[0.05] p-8 backdrop-blur-xl">
          <AuthShowcase />
        </section>

        {/* Guide section */}
        <section className="w-full max-w-3xl">
          <div className="rounded-2xl border border-[#27ae60]/20 bg-white/[0.06] p-8 backdrop-blur-xl">
            <div className="mb-6 text-center">
              <h2 className="mb-3 text-3xl font-bold">
                <span className="bg-gradient-to-br from-[#27ae60] to-[#2ecc71] bg-clip-text text-transparent">
                  Discover
                </span>{' '}
                the Readstr Guide
              </h2>
              <p className="text-base leading-relaxed text-[#B3B3B3]">
                Explore a curated directory of long-form content creators on Nostr. Find writers,
                bloggers, and thinkers publishing decentralized content.
              </p>
            </div>

            <div className="mb-8 grid gap-4 text-sm md:grid-cols-3">
              <div className="text-center text-[#B3B3B3]">
                <p className="font-medium text-white">Browse by topic</p>
                <p className="mt-1">Discover creators by tag</p>
              </div>
              <div className="text-center text-[#B3B3B3]">
                <p className="font-medium text-white">Get RSS feeds</p>
                <p className="mt-1">For any creator</p>
              </div>
              <div className="text-center text-[#B3B3B3]">
                <p className="font-medium text-white">Subscribe instantly</p>
                <p className="mt-1">Directly in-app</p>
              </div>
            </div>

            <div className="text-center">
              <Link
                href="/guide"
                className="inline-block rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-8 py-3 text-base font-semibold text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)] transition-all duration-300 hover:-translate-y-0.5 hover:shadow-[0_8px_25px_rgba(39,174,96,0.35)]"
              >
                Explore the Guide
              </Link>
              <p className="mt-3 text-sm text-[#B3B3B3]">
                Submit your own feed and help grow the Nostr ecosystem
              </p>
            </div>
          </div>
        </section>
      </div>
    </main>
  )
}
