#!/usr/bin/env node
// Checks a running production build (standalone server.js with the
// server-timeouts.cjs preload) enforces the tRPC request limits:
//   a) an oversize chunked POST is answered 413 while the client is still uploading;
//   b) a superjson referentialEqualities bomb is refused quickly;
//   c) with EXPECT_REQUEST_TIMEOUT_MS set, stalled headers and bodies are cut off.
// Usage: BASE_URL=http://127.0.0.1:3000 node scripts/ci/body-cap-check.mjs
// None of these paths reach the database.

import http from 'node:http'
import net from 'node:net'

const BASE_URL = new URL(process.env.BASE_URL ?? 'http://127.0.0.1:3000')
// MAX_TRPC_BODY_BYTES in src/app/api/trpc/[trpc]/route.ts.
const BODY_CAP_BYTES = 64_000
// A protected mutation: without NIP-98 auth it never touches the database.
const PROCEDURE_PATH = '/api/trpc/feed.markAsRead'

const failures = []
function check(name, ok, detail) {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}: ${detail}`)
  if (!ok) failures.push(name)
}

function post(body, headers = {}) {
  return new Promise((resolve, reject) => {
    const started = performance.now()
    const req = http.request(
      new URL(PROCEDURE_PATH, BASE_URL),
      { method: 'POST', headers: { 'content-type': 'application/json', ...headers } },
      res => {
        let text = ''
        res.setEncoding('utf8')
        res.on('data', chunk => (text += chunk))
        res.on('end', () => resolve({ status: res.statusCode, text, ms: performance.now() - started }))
      }
    )
    req.on('error', reject)
    req.end(body)
  })
}

async function controlRequest() {
  const res = await post(JSON.stringify({ json: { itemId: 'x' } }))
  check(
    'small body is not refused for size',
    res.status !== 413 && res.status < 500,
    `status ${res.status} in ${res.ms.toFixed(0)} ms`
  )
}

// a) Stream well past the cap in slow chunks; the 413 must come back long
// before the upload would have finished.
function oversizeChunkedPost() {
  const CHUNK = Buffer.alloc(8 * 1024, 'a')
  const TOTAL_CHUNKS = 128 // 1 MiB, ~16x the cap
  const INTERVAL_MS = 25 // ~3.2 s to send everything
  return new Promise(resolve => {
    let written = 0
    let finished = false
    let settled = false
    let timer
    const settle = result => {
      if (settled) return
      settled = true
      clearInterval(timer)
      req.destroy()
      resolve(result)
    }
    const req = http.request(new URL(PROCEDURE_PATH, BASE_URL), {
      method: 'POST',
      headers: { 'content-type': 'application/json', 'transfer-encoding': 'chunked' },
    })
    req.on('response', res => {
      res.resume()
      settle({ status: res.statusCode, written, finished })
    })
    req.on('error', err => settle({ error: err.message, written, finished }))
    req.flushHeaders()
    timer = setInterval(() => {
      if (written >= TOTAL_CHUNKS * CHUNK.length) {
        clearInterval(timer)
        finished = true
        req.end()
        return
      }
      req.write(CHUNK)
      written += CHUNK.length
    }, INTERVAL_MS)
    setTimeout(() => settle({ error: 'no response within 15 s', written, finished }), 15_000).unref()
  }).then(result => {
    const ok = result.status === 413 && !result.finished && result.written < TOTAL_CHUNKS * CHUNK.length
    check(
      'oversize chunked POST gets 413 before the upload completes',
      ok,
      result.error
        ? `${result.error} after ${result.written} bytes`
        : `status ${result.status} after ${result.written} of ${TOTAL_CHUNKS * CHUNK.length} bytes (cap ${BODY_CAP_BYTES}), upload ${result.finished ? 'finished' : 'still running'}`
    )
  })
}

// b) Nested {a:{a:...,b:0},b:0} whose every `b` is declared the same object as
// its sibling `a`. Honoured, it expands to 2^depth objects.
function referentialBomb(depth) {
  let json = { a: 0, b: 0 }
  for (let i = 1; i < depth; i++) json = { a: json, b: 0 }
  const refs = {}
  for (let i = depth; i >= 1; i--) {
    const path = Array(i).fill('a')
    refs[path.join('.')] = [[...path.slice(0, -1), 'b'].join('.')]
  }
  return JSON.stringify({ json, meta: { referentialEqualities: refs } })
}

async function referentialBombPost() {
  const body = referentialBomb(30)
  const res = await post(body)
  check(
    'superjson referentialEqualities bomb is refused fast',
    res.status >= 400 && res.status < 500 && res.ms < 2_000,
    `status ${res.status} in ${res.ms.toFixed(0)} ms for a ${body.length}-byte body`
  )
}

// c) Open a raw connection, send part of a request, then stall; the server
// must close it (Node answers 408) within the configured timeout plus slack.
function stalledRequest(name, partial, limitMs) {
  return new Promise(resolve => {
    const started = performance.now()
    const socket = net.connect(Number(BASE_URL.port || 80), BASE_URL.hostname)
    let received = ''
    let settled = false
    const done = outcome => {
      if (settled) return
      settled = true
      clearTimeout(deadline)
      socket.destroy()
      const ms = performance.now() - started
      const status = received.match(/^HTTP\/1\.1 (\d{3})/)?.[1] ?? 'none'
      check(name, outcome === 'closed' && ms <= limitMs, `${outcome} after ${ms.toFixed(0)} ms (limit ${limitMs} ms), status ${status}`)
      resolve()
    }
    const deadline = setTimeout(() => done('still open'), limitMs + 5_000)
    socket.setEncoding('utf8')
    socket.on('connect', () => socket.write(partial))
    socket.on('data', chunk => (received += chunk))
    socket.on('close', () => done('closed'))
    socket.on('error', () => {})
  })
}

async function stalledRequests(requestTimeoutMs) {
  const host = `Host: ${BASE_URL.host}\r\n`
  const slackMs = 3_000
  await stalledRequest(
    'stalled headers are cut off',
    `POST ${PROCEDURE_PATH} HTTP/1.1\r\n${host}content-type: application/json\r\n`,
    requestTimeoutMs + slackMs
  )
  await stalledRequest(
    'stalled body is cut off',
    `POST ${PROCEDURE_PATH} HTTP/1.1\r\n${host}content-type: application/json\r\ncontent-length: 1000\r\n\r\n{"json":`,
    requestTimeoutMs + slackMs
  )
}

async function main() {
  console.log(`Checking ${BASE_URL.origin}`)
  await controlRequest()
  await oversizeChunkedPost()
  await referentialBombPost()
  const expected = process.env.EXPECT_REQUEST_TIMEOUT_MS
  if (expected) {
    if (!/^\d+$/.test(expected)) throw new Error(`EXPECT_REQUEST_TIMEOUT_MS must be a positive integer, got ${expected}`)
    await stalledRequests(Number(expected))
  } else {
    console.log('SKIP stalled-request checks: EXPECT_REQUEST_TIMEOUT_MS is not set')
  }
  if (failures.length) {
    console.error(`\n${failures.length} check(s) failed: ${failures.join('; ')}`)
    process.exit(1)
  }
  console.log('\nAll request-limit checks passed')
}

main().catch(err => {
  console.error(`body-cap-check crashed: ${err.stack ?? err}`)
  process.exit(1)
})
