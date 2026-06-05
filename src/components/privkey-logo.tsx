import Image from 'next/image'

export function PrivKeyLogo({
  priority,
  className = 'h-[60px] w-auto',
}: {
  priority?: boolean
  className?: string
}) {
  return (
    <Image
      src="/privkey-logo-white.png"
      alt="PrivKey"
      width={480}
      height={229}
      priority={priority}
      className={className}
    />
  )
}
