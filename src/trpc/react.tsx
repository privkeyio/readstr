'use client'

import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { httpBatchLink, loggerLink } from '@trpc/client'
import { createTRPCReact } from '@trpc/react-query'
import { useState, useEffect } from 'react'
import superjson from 'superjson'
import { nip98 } from 'nostr-tools'
import { type AppRouter } from '@/server/api/root'
import { useNostrAuth } from '@/contexts/NostrAuthContext'
import { type UnsignedEvent, type Event as NostrEvent } from 'nostr-tools'

export const api = createTRPCReact<AppRouter>()

async function sha256Hex(input: string): Promise<string> {
  const data = new TextEncoder().encode(input)
  const digest = await crypto.subtle.digest('SHA-256', data)
  return Array.from(new Uint8Array(digest))
    .map(b => b.toString(16).padStart(2, '0'))
    .join('')
}

// Current signer, kept in a module-scoped ref so the per-request fetch wrapper
// (created once when the client is built) always sees the latest auth state.
// `sign` is null when the user can't sign (e.g. npub+password read-only mode),
// in which case no NIP-98 header is sent and the request goes out unauthenticated.
let currentSign:
  | ((event: UnsignedEvent) => Promise<NostrEvent | null>)
  | null = null

export function TRPCReactProvider(props: {
  children: React.ReactNode
  cookies?: string
}) {
  const { user, signEvent } = useNostrAuth()

  const [queryClient] = useState(() => new QueryClient({
    defaultOptions: {
      queries: {
        refetchOnWindowFocus: false,
        retry: 1,
      },
    },
  }))

  // Keep the module-scoped signer in sync with the current user/auth method.
  useEffect(() => {
    if (user) {
      currentSign = (event: UnsignedEvent) => signEvent(event)
    } else {
      currentSign = null
    }
  }, [user, signEvent])

  // Create tRPC client once. We sign a NIP-98 (kind 27235) event for the EXACT
  // URL tRPC is about to fetch by wrapping `fetch`. This guarantees the signed
  // `u` tag matches the real request (including the batched procedure list and
  // serialized `input` query string), so the server's u-tag check passes.
  const [trpcClient] = useState(() => {
    return api.createClient({
      transformer: superjson,
      links: [
        loggerLink({
          enabled: () => process.env.NODE_ENV === 'development',
        }),
        httpBatchLink({
          url: '/api/trpc',
          headers() {
            const headers: Record<string, string> = {}
            if (props.cookies) {
              headers.cookie = props.cookies
            }
            return headers
          },
          async fetch(input, init) {
            // Resolve the absolute URL exactly as the browser will fetch it, so
            // the signed `u` tag matches what the server reconstructs.
            const url =
              typeof input === 'string'
                ? new URL(input, window.location.origin).toString()
                : input instanceof URL
                  ? input.toString()
                  : (input as Request).url

            const method = (init?.method ?? 'GET').toUpperCase()

            const headers = new Headers(init?.headers)
            const sign = currentSign
            if (sign) {
              try {
                // For mutations the body is not in the URL, so bind it into the
                // signature via the NIP-98 `payload` tag (sha256 of the exact
                // bytes we send). We add it inside the sign wrapper so the hash
                // commits to the same string the server hashes server-side.
                const payloadHash =
                  method !== 'GET' && method !== 'HEAD' && typeof init?.body === 'string'
                    ? await sha256Hex(init.body)
                    : null

                // nip98.getToken builds + signs the kind 27235 event and returns
                // the base64 token; the `true` arg prepends the "Nostr " scheme.
                const token = await nip98.getToken(
                  url,
                  method,
                  async e => {
                    const event = e as unknown as UnsignedEvent
                    if (payloadHash) {
                      event.tags = [...event.tags, ['payload', payloadHash]]
                    }
                    const signed = await sign(event)
                    if (!signed) throw new Error('signing returned null')
                    return signed as unknown as NostrEvent
                  },
                  true
                )
                headers.set('Authorization', token)
              } catch (err) {
                // Read-only sessions (npub+password) or rejected signing fall
                // through unauthenticated; protected procedures will then fail.
                console.warn('NIP-98 signing unavailable for request:', err)
              }
            }

            return fetch(input, { ...init, headers })
          },
        }),
      ],
    })
  })

  return (
    <QueryClientProvider client={queryClient}>
      <api.Provider client={trpcClient} queryClient={queryClient}>
        {props.children}
      </api.Provider>
    </QueryClientProvider>
  )
}