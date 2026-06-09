import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { requireCronSecret } from './cron'

const makeRequest = (authorization?: string) =>
  new Request('https://example.test/api/cron', {
    headers: authorization ? { authorization } : {},
  })

let savedSecret: string | undefined

beforeEach(() => {
  savedSecret = process.env.CRON_SECRET
})

afterEach(() => {
  if (savedSecret === undefined) {
    delete process.env.CRON_SECRET
  } else {
    process.env.CRON_SECRET = savedSecret
  }
})

describe('requireCronSecret', () => {
  it('returns a 500 response when CRON_SECRET is unset (fail closed)', () => {
    delete process.env.CRON_SECRET
    const res = requireCronSecret(makeRequest('Bearer anything'))
    expect(res).not.toBeNull()
    expect(res?.status).toBe(500)
  })

  it('returns a 401 response when the Authorization header is missing', () => {
    process.env.CRON_SECRET = 's3cret'
    const res = requireCronSecret(makeRequest())
    expect(res?.status).toBe(401)
  })

  it('returns a 401 response when the bearer token is wrong', () => {
    process.env.CRON_SECRET = 's3cret'
    const res = requireCronSecret(makeRequest('Bearer wrong'))
    expect(res?.status).toBe(401)
  })

  it('returns a 401 response when the scheme is missing', () => {
    process.env.CRON_SECRET = 's3cret'
    const res = requireCronSecret(makeRequest('s3cret'))
    expect(res?.status).toBe(401)
  })

  it('returns null when the correct Bearer secret is provided', () => {
    process.env.CRON_SECRET = 's3cret'
    const res = requireCronSecret(makeRequest('Bearer s3cret'))
    expect(res).toBeNull()
  })
})
