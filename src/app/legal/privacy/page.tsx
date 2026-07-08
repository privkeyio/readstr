import Link from 'next/link'
import { BrandHeader } from '@/components/brand-header'

export const metadata = {
  title: 'Privacy Policy — Readstr',
  description:
    'How Readstr and the Readstr browser extension handle your data. No tracking, no data sales, keys stay on your device.',
}

export default function PrivacyPage() {
  return (
    <main className="font-brand relative flex min-h-screen flex-col bg-gradient-to-br from-[#1a1a1a] via-[#0d1117] to-[#161b22] text-white">
      <BrandHeader cta={{ href: '/', label: 'Back to Home' }} />

      <div className="mx-auto w-full max-w-4xl px-4 py-12">
        <div className="rounded-2xl border border-[#27ae60]/20 bg-white/[0.06] p-8 backdrop-blur-xl md:p-12">
          <h1 className="mb-3 text-4xl font-extrabold tracking-tight">
            <span className="bg-gradient-to-br from-[#27ae60] via-[#2ecc71] to-[#58d68d] bg-clip-text text-transparent">
              Readstr
            </span>{' '}
            Privacy Policy
          </h1>
          <p className="mb-12 text-[#B3B3B3]">Last updated: July 8, 2026</p>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">Summary</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <ul className="space-y-2">
                <li>We do not run analytics, trackers, or advertising.</li>
                <li>We do not sell or rent your data, and we do not share it with third parties for their own use.</li>
                <li>Your Nostr private key never leaves your device and is never sent to our servers.</li>
                <li>We store only what is needed to sync your subscriptions and read state across your devices.</li>
                <li>Readstr is open source and self-hostable, so you can run the entire stack yourself.</li>
              </ul>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">Who This Covers</h2>
            <div className="space-y-4 leading-relaxed text-[#B3B3B3]">
              <p>
                This policy applies to the Readstr web app at readstr.privkey.io and the Readstr
                browser extension, both operated by PrivKey LLC. If you self-host Readstr, this
                policy describes the software&rsquo;s behavior, but you become the operator of your
                own instance and its data.
              </p>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">Data We Handle</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <p className="mb-3">
                Readstr is designed to hold as little as possible. Depending on how you use it, the
                following data is involved:
              </p>
              <ul className="space-y-2">
                <li>
                  <strong className="text-white">Nostr public key (npub / pubkey).</strong> Used as
                  your account identifier so your subscriptions and read state can be synced. This is
                  public information by design.
                </li>
                <li>
                  <strong className="text-white">Feed subscriptions.</strong> The RSS/Atom and Nostr
                  feeds you add, stored so they are available across your devices.
                </li>
                <li>
                  <strong className="text-white">Read state.</strong> Which items you have marked as
                  read, so unread counts stay consistent between the web app and the extension.
                </li>
                <li>
                  <strong className="text-white">Extension settings.</strong> Preferences such as
                  your configured Readstr URL, poll interval, and notification options. These are
                  stored locally in your browser.
                </li>
                <li>
                  <strong className="text-white">Nostr private key (nsec), if you choose to enter
                  one.</strong> Stored locally in your browser and used only to sign requests on your
                  device. It is never transmitted to our servers. Where a NIP-07 signer is available,
                  the extension uses it and does not handle your key at all.
                </li>
              </ul>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">How We Use It</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <ul className="space-y-2">
                <li>To detect feeds on pages you visit and let you subscribe to them.</li>
                <li>To synchronize your subscriptions and read state across your devices.</li>
                <li>To fetch new feed items and, if enabled, notify you when they arrive.</li>
                <li>To authenticate your requests to the Readstr server using NIP-98 HTTP auth.</li>
              </ul>
              <p className="mt-3">
                We use your data only for these operational purposes. We do not use it for
                advertising, profiling, or any purpose unrelated to running the feed reader.
              </p>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">Who It&rsquo;s Shared With</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <ul className="space-y-2">
                <li>
                  <strong className="text-white">Feed sources you subscribe to.</strong> Fetching a
                  feed contacts that source&rsquo;s server, which will see the request as any feed
                  reader would.
                </li>
                <li>
                  <strong className="text-white">Nostr relays you use.</strong> Nostr content is
                  fetched from and published to the relays you configure.
                </li>
                <li>
                  <strong className="text-white">No one else.</strong> We do not sell, rent, or
                  transfer your data to third parties, and we do not use third-party analytics or
                  advertising networks.
                </li>
              </ul>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">Storage &amp; Security</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <ul className="space-y-2">
                <li>Sync data (public key, subscriptions, read state) is stored on the Readstr server you connect to.</li>
                <li>Local data (settings and any private key) is stored in your browser&rsquo;s extension storage and never leaves your device except to sign your own requests.</li>
                <li>All communication with the Readstr server uses HTTPS.</li>
                <li>Requests are authenticated with NIP-98, so your private key is used only to sign and is not transmitted.</li>
              </ul>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">Your Choices</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <ul className="space-y-2">
                <li>You can use the extension without a Nostr account; feeds can be kept locally in your browser.</li>
                <li>You can remove any locally stored key from the extension&rsquo;s settings at any time.</li>
                <li>You can delete your subscriptions and read state from your account.</li>
                <li>You can self-host Readstr so that all data stays on infrastructure you control.</li>
              </ul>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">Changes to This Policy</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <p>
                If our data practices change, we will update this page and revise the &ldquo;Last
                updated&rdquo; date above. Material changes will be reflected here before they take
                effect.
              </p>
            </div>
          </section>

          <section className="mb-10">
            <h2 className="mb-4 text-2xl font-bold text-white">Contact</h2>
            <div className="leading-relaxed text-[#B3B3B3]">
              <p>
                Questions about this policy or your data can be sent to{' '}
                <a
                  href="mailto:kyle@privkey.io"
                  className="font-medium text-[#27ae60] transition-colors hover:text-[#2ecc71]"
                >
                  kyle@privkey.io
                </a>
                . Readstr is open source at{' '}
                <a
                  href="https://github.com/privkeyio/readstr"
                  target="_blank"
                  rel="noopener noreferrer"
                  className="font-medium text-[#27ae60] transition-colors hover:text-[#2ecc71]"
                >
                  github.com/privkeyio/readstr
                </a>
                .
              </p>
            </div>
          </section>

          <div className="mt-8 flex justify-center gap-6 text-sm">
            <Link
              href="/legal"
              className="font-medium text-[#27ae60] transition-colors hover:text-[#2ecc71]"
            >
              Terms of Service
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
