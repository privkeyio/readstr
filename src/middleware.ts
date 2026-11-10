import { NextResponse } from 'next/server'
import type { NextRequest } from 'next/server'

// Pass-through middleware; route protection is handled in-app via Nostr auth
export function middleware(request: NextRequest) {
  return NextResponse.next()
}

// /api/* routes are excluded: matching them makes Next buffer and clone every
// request body (up to 10 MB) before the route runs, defeating the routes' own
// 64 KB body caps. Page routes are still matched; they read no request bodies.
export const config = {
  matcher: [
    '/((?!_next|api/|[^?]*\\.(?:html?|css|js(?!on)|jpe?g|webp|png|gif|svg|ttf|woff2?|ico|csv|docx?|xlsx?|zip|webmanifest)).*)',
  ],
}