import { NextResponse } from 'next/server'
import { createHash, timingSafeEqual } from 'node:crypto'

function sha256(value: string): Buffer {
  return createHash('sha256').update(value).digest()
}

/**
 * Fail-closed CRON_SECRET auth shared by the cron and seed routes. Returns a
 * 500/401 response to short-circuit on misconfiguration or bad credentials, or
 * null when the request is authorized. Compares SHA-256 digests with
 * timingSafeEqual so neither the secret's value nor its length leaks via timing.
 */
export function requireCronSecret(request: Request): NextResponse | null {
  const cronSecret = process.env.CRON_SECRET
  if (!cronSecret) {
    return new NextResponse('Server misconfigured: CRON_SECRET not set', { status: 500 })
  }

  const authHeader = request.headers.get('authorization') ?? ''
  const expected = sha256(`Bearer ${cronSecret}`)
  const provided = sha256(authHeader)

  if (!timingSafeEqual(expected, provided)) {
    return new NextResponse('Unauthorized', { status: 401 })
  }

  return null
}
