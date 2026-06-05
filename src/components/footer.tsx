import Link from 'next/link'
import Image from 'next/image'

export function Footer() {
  return (
    <footer className="mt-auto border-t border-[#27ae60]/15 bg-[#0d1117]">
      <div className="mx-auto max-w-7xl px-4 py-8 sm:px-6 lg:px-8">
        <div className="flex flex-col items-center justify-between gap-4 text-sm text-[#B3B3B3] md:flex-row">
          <div className="flex items-center gap-3">
            <Image
              src="/privkey-logo-white.png"
              alt="PrivKey"
              width={234}
              height={60}
              className="h-[60px] w-auto opacity-90"
            />
            <span className="hidden text-white/30 sm:inline">|</span>
            <span className="hidden font-medium text-[#27ae60] sm:inline">
              Your keys, your stack
            </span>
          </div>

          <div className="flex items-center gap-4">
            <a
              href="https://privkey.io"
              target="_blank"
              rel="noopener noreferrer"
              className="font-semibold text-[#27ae60] transition-colors hover:text-[#2ecc71]"
            >
              privkey.io
            </a>
            <span className="text-white/20">•</span>
            <Link
              href="/legal"
              className="transition-colors hover:text-[#27ae60]"
            >
              Terms of Service
            </Link>
            <span className="text-white/20">•</span>
            <span>© {new Date().getFullYear()} PrivKey LLC</span>
          </div>
        </div>
      </div>
    </footer>
  )
}
