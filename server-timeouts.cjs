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
 * about a second. The clock stops once the request has been fully received, so
 * a slow response is not cut off; but a response the app starts before the
 * client has finished uploading is cut off with the connection at about
 * requestTimeout.
 *
 * Values must be 1000..3600000 ms: Node's connection checker reads them as
 * uint32, so larger values would wrap (2^32 disables the timeout), and tiny
 * ones would 408 every request.
 */

const http = require('node:http')

const DEFAULT_REQUEST_TIMEOUT_MS = 30_000
const DEFAULT_HEADERS_TIMEOUT_MS = 15_000
const CONNECTIONS_CHECKING_INTERVAL_MS = 1_000
const MIN_TIMEOUT_MS = 1_000
const MAX_TIMEOUT_MS = 3_600_000

function parseTimeoutMs(raw, fallback, name) {
  const trimmed = raw?.trim()
  if (!trimmed) return fallback
  const value = Number(trimmed)
  if (/^\d+$/.test(trimmed) && value >= MIN_TIMEOUT_MS && value <= MAX_TIMEOUT_MS) return value
  console.warn(
    `[server-timeouts] ignoring ${name}=${JSON.stringify(raw)}: not an integer from ${MIN_TIMEOUT_MS} to ${MAX_TIMEOUT_MS}; using ${fallback}`
  )
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

module.exports = {
  parseTimeoutMs,
  resolveTimeouts,
  DEFAULT_REQUEST_TIMEOUT_MS,
  DEFAULT_HEADERS_TIMEOUT_MS,
  MIN_TIMEOUT_MS,
  MAX_TIMEOUT_MS,
}
