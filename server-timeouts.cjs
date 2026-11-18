'use strict'

/**
 * Preloaded with `node --require ./server-timeouts.cjs server.js` so every
 * http.Server gets tight request timeouts. Next's standalone server.js has no
 * option for them, and Node's defaults let a client hold a request open for
 * 300 s (requestTimeout) by trickling headers or a body.
 *
 * REQUEST_TIMEOUT_MS bounds receiving a whole request (headers and body);
 * HEADERS_TIMEOUT_MS bounds the headers alone and is clamped to it, since Node
 * refuses a headersTimeout above requestTimeout. Both are enforced to within
 * about a second. Responses are not affected.
 */

const http = require('node:http')

const DEFAULT_REQUEST_TIMEOUT_MS = 30_000
const DEFAULT_HEADERS_TIMEOUT_MS = 15_000
const CONNECTIONS_CHECKING_INTERVAL_MS = 1_000

function parseTimeoutMs(raw, fallback, name) {
  if (raw === undefined || raw.trim() === '') return fallback
  const value = Number(raw.trim())
  if (/^\d+$/.test(raw.trim()) && Number.isSafeInteger(value) && value > 0) return value
  console.warn(`[server-timeouts] ignoring ${name}=${JSON.stringify(raw)}: not a positive integer; using ${fallback}`)
  return fallback
}

function resolveTimeouts(env) {
  const requestTimeout = parseTimeoutMs(env.REQUEST_TIMEOUT_MS, DEFAULT_REQUEST_TIMEOUT_MS, 'REQUEST_TIMEOUT_MS')
  const headersTimeout = Math.min(
    parseTimeoutMs(env.HEADERS_TIMEOUT_MS, DEFAULT_HEADERS_TIMEOUT_MS, 'HEADERS_TIMEOUT_MS'),
    requestTimeout
  )
  return { requestTimeout, headersTimeout }
}

function install(env) {
  const timeouts = resolveTimeouts(env)
  const createServer = http.createServer
  http.createServer = function createServerWithTimeouts(...args) {
    const server = createServer.apply(this, args)
    server.requestTimeout = timeouts.requestTimeout
    server.headersTimeout = timeouts.headersTimeout
    // Node only checks for expired requests every connectionsCheckingInterval
    // (30 s by default), which would let a request outlive its timeout by that much.
    server.connectionsCheckingInterval = CONNECTIONS_CHECKING_INTERVAL_MS
    return server
  }
  return timeouts
}

const installed = install(process.env)
console.info(
  `[server-timeouts] requestTimeout=${installed.requestTimeout}ms headersTimeout=${installed.headersTimeout}ms`
)

module.exports = { parseTimeoutMs, resolveTimeouts, DEFAULT_REQUEST_TIMEOUT_MS, DEFAULT_HEADERS_TIMEOUT_MS }
