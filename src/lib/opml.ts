import * as cheerio from 'cheerio'
import type { Element } from 'domhandler'
import { normalizeNpub } from './nostr-sync'

export interface OpmlFeed {
  title?: string
  xmlUrl?: string
  npub?: string
  folder?: string
  tags: string[]
}

export interface OpmlExportFeed {
  type: 'RSS' | 'NOSTR' | 'NOSTR_VIDEO'
  url: string
  title?: string
  tags?: string[]
  category?: { name: string; color?: string | null; icon?: string | null } | null
}

const MAX_CONTENT_BYTES = 8 * 1024 * 1024
const MAX_OUTLINES = 5000

function parseCategory(value?: string): string[] {
  if (!value) return []
  return value.split(',').map(t => t.trim()).filter(Boolean)
}

export function parseOpml(xml: string): OpmlFeed[] {
  if (!xml || xml.length > MAX_CONTENT_BYTES) return []

  let $: cheerio.CheerioAPI
  try {
    $ = cheerio.load(xml, { xmlMode: true })
  } catch {
    return []
  }

  const feeds: OpmlFeed[] = []
  let count = 0
  let capped = false

  const walk = (parent: Element, folder?: string) => {
    if (capped) return
    $(parent).children('outline').each((_, child) => {
      if (capped) return
      count++
      if (count > MAX_OUTLINES) {
        capped = true
        return
      }

      const $child = $(child)
      const attr = (name: string) => {
        const v = $child.attr(name)
        return v && v.trim() ? v.trim() : undefined
      }

      const title = attr('title') ?? attr('text')
      const xmlUrl = attr('xmlUrl')
      const npub = attr('nostr') ?? attr('npub')
      const type = (attr('type') ?? '').toLowerCase()
      const tags = parseCategory(attr('category'))
      const isNostr = type === 'nostr' || !!npub

      if (isNostr) {
        const value = npub ?? xmlUrl
        if (value) feeds.push({ title, npub: value, folder, tags })
        return
      }

      if (xmlUrl) {
        feeds.push({ title, xmlUrl, folder, tags })
        return
      }

      if ($child.children('outline').length > 0) {
        walk(child, title ?? folder)
      }
    })
  }

  const root = $('body').get(0) ?? $('opml').get(0)
  if (root) walk(root)

  return feeds
}

function xmlEscape(value: string): string {
  return value
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
}

export function buildOpml(feeds: OpmlExportFeed[]): string {
  const nostrNpubs = new Set(
    feeds.filter(f => f.type === 'NOSTR' && f.url).map(f => normalizeNpub(f.url))
  )

  const exportable = feeds.filter(f => {
    if (!f.url) return false
    if (f.type === 'NOSTR_VIDEO') return !nostrNpubs.has(normalizeNpub(f.url))
    return true
  })

  const groups = new Map<string, OpmlExportFeed[]>()
  const uncategorized: OpmlExportFeed[] = []
  for (const f of exportable) {
    const name = f.category?.name
    if (name) {
      const arr = groups.get(name) ?? []
      arr.push(f)
      groups.set(name, arr)
    } else {
      uncategorized.push(f)
    }
  }

  const feedLine = (f: OpmlExportFeed, indent: string): string => {
    const isNostr = f.type === 'NOSTR' || f.type === 'NOSTR_VIDEO'
    const text = xmlEscape(f.title || f.url)
    const category = f.tags && f.tags.length > 0
      ? ` category="${xmlEscape(f.tags.join(','))}"`
      : ''
    return isNostr
      ? `${indent}<outline type="nostr" text="${text}" title="${text}" nostr="${xmlEscape(f.url)}"${category}/>`
      : `${indent}<outline type="rss" text="${text}" title="${text}" xmlUrl="${xmlEscape(f.url)}"${category}/>`
  }

  const lines: string[] = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<opml version="2.0">',
    '  <head><title>Readstr Subscriptions</title></head>',
    '  <body>',
  ]

  for (const f of uncategorized) lines.push(feedLine(f, '    '))

  for (const [name, arr] of groups) {
    const folderText = xmlEscape(name)
    lines.push(`    <outline text="${folderText}" title="${folderText}">`)
    for (const f of arr) lines.push(feedLine(f, '      '))
    lines.push('    </outline>')
  }

  lines.push('  </body>', '</opml>')
  return lines.join('\n')
}
