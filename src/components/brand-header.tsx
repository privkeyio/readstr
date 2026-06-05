import Link from 'next/link'
import Image from 'next/image'

export function BrandHeader({
  cta,
}: {
  cta?: { href: string; label: string }
}) {
  return (
    <header className="w-full border-b border-[#27ae60]/15">
      <div className="mx-auto flex w-full max-w-6xl items-center justify-between px-4 py-5">
        <Link href="/" className="flex items-center gap-3">
          <Image
            src="/privkey-logo-white.png"
            alt="PrivKey"
            width={234}
            height={60}
            priority
            className="h-[60px] w-auto"
          />
          <span className="hidden text-sm font-medium tracking-wide text-white/40 sm:inline">
            /
          </span>
          <span className="hidden text-lg font-semibold tracking-tight text-white sm:inline">
            Readstr
          </span>
        </Link>
        {cta && (
          <Link
            href={cta.href}
            className="rounded-xl border border-white/20 bg-white/10 px-5 py-2 text-sm font-semibold text-white backdrop-blur-sm transition-all duration-300 hover:-translate-y-0.5 hover:border-white/40 hover:bg-white/20"
          >
            {cta.label}
          </Link>
        )}
      </div>
    </header>
  )
}
