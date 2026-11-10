'use client'

import { setRuntimeDefaultRelays } from '@/lib/relays'

// Rendered at the top of the root layout so the operator's DEFAULT_RELAYS are
// registered during render, before any descendant effect reads relays.
export function DefaultRelays({ relays }: { relays: string[] }) {
  if (typeof window !== 'undefined') setRuntimeDefaultRelays(relays)
  return null
}
