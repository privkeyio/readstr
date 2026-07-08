import Link from 'next/link'
import { BrandHeader } from '@/components/brand-header'

export default function LegalPage() {
  return (
    <main className="font-brand relative flex min-h-screen flex-col bg-gradient-to-br from-[#1a1a1a] via-[#0d1117] to-[#161b22] text-white">
      <BrandHeader cta={{ href: '/', label: 'Back to Home' }} />

      <div className="mx-auto w-full max-w-4xl px-4 py-12">
        <div className="rounded-2xl border border-[#27ae60]/20 bg-white/[0.06] p-8 backdrop-blur-xl md:p-12">
          <h1 className="mb-3 text-4xl font-extrabold tracking-tight">
            <span className="bg-gradient-to-br from-[#27ae60] via-[#2ecc71] to-[#58d68d] bg-clip-text text-transparent">
              Readstr
            </span>{' '}
            Terms of Service
          </h1>
          <p className="mb-12 text-[#B3B3B3]">
            Please read these terms carefully before using the service.
          </p>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">What Readstr Is</h2>
            <div className="space-y-4 leading-relaxed text-[#B3B3B3]">
              <p>
                Readstr is a sovereign RSS and Nostr feed reader by PrivKey. It lets you subscribe
                to RSS feeds and Nostr long-form content (NIP-23) in a single unified reader. Your
                keys, your stack.
              </p>
              <p>
                The Guide and RSS feed generation are provided to help you discover and follow
                long-form content creators on Nostr.
              </p>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">Use At Your Own Risk</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <ul className="space-y-2">
                <li>Service availability may be interrupted for maintenance or unforeseen issues.</li>
                <li>Feeds may occasionally fail to load if upstream sources are unavailable.</li>
                <li>Back up any content that is important to you.</li>
                <li>Features may evolve, change, or be removed as the product develops.</li>
                <li>RSS and Nostr protocols are evolving, and behavior may change accordingly.</li>
              </ul>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">No Warranties</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <p className="mb-3">
                The service is provided &ldquo;as is&rdquo; without warranties of any kind. We make
                no guarantees regarding:
              </p>
              <ul className="space-y-2">
                <li>Uptime or continuous availability</li>
                <li>Data persistence</li>
                <li>Feature stability</li>
                <li>Compatibility with any particular workflow</li>
                <li>Protection from upstream platform rate limits</li>
              </ul>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">Data &amp; Privacy</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <ul className="space-y-2">
                <li>Your Nostr keys are managed by your browser extension and stored locally.</li>
                <li>Keys are never sent to our servers.</li>
                <li>No third-party services are used except the relays and feeds you subscribe to.</li>
                <li>The platform is self-hostable if you prefer to run it on your own infrastructure.</li>
              </ul>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">Platform Changes</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <p className="mb-3">
                RSS feeds and Nostr relays may change their behavior over time. When they do:
              </p>
              <ul className="space-y-2">
                <li>Functionality may be temporarily disrupted until updates are made.</li>
                <li>Some features may change if an upstream platform removes them.</li>
                <li>New restrictions or rate limits may appear without notice.</li>
              </ul>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">Limitation of Liability</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <p>
                To the maximum extent permitted by law, PrivKey LLC is not liable for any loss or
                damage arising from your use of, or inability to use, the service.
              </p>
            </div>
          </section>

          <div className="mt-8 flex justify-center gap-6 text-sm">
            <Link
              href="/legal/privacy"
              className="font-medium text-[#27ae60] transition-colors hover:text-[#2ecc71]"
            >
              Privacy Policy
            </Link>
            <Link
              href="/"
              className="font-medium text-[#27ae60] transition-colors hover:text-[#2ecc71]"
            >
              &larr; Back to Home
            </Link>
          </div>
        </div>
      </div>
    </main>
  )
}
