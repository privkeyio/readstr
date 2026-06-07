import { NextRequest, NextResponse } from 'next/server';
import { SimplePool, nip19, Event, Filter } from 'nostr-tools';
import RSS from 'rss';
import { unified } from 'unified';
import rehypeParse from 'rehype-parse';
import rehypeSanitize, { defaultSchema, type Options as SanitizeSchema } from 'rehype-sanitize';
import rehypeStringify from 'rehype-stringify';

// Sanitize generated HTML before it lands in content:encoded. readstr's own UI
// renders feeds through rehype-sanitize, but third-party RSS readers consume this
// route's raw markup directly — unsanitized attacker content would be stored XSS.
const sanitizeSchema: SanitizeSchema = {
  ...defaultSchema,
  attributes: {
    ...defaultSchema.attributes,
    code: [...(defaultSchema.attributes?.code || []), ['className', /^language-./]],
  },
};

async function sanitizeHtml(html: string): Promise<string> {
  return String(
    await unified()
      .use(rehypeParse, { fragment: true })
      .use(rehypeSanitize, sanitizeSchema)
      .use(rehypeStringify)
      .process(html)
  );
}

// Plain-text RSS fields (titles, descriptions, author) carry untrusted Nostr data.
// Strip all HTML so a lenient reader that renders them as markup can't be XSS'd.
// These land in CDATA (no XML escaping), so we extract the decoded text and drop
// any remaining angle brackets — a lenient reader then can't form a tag from them.
const stripSchema: SanitizeSchema = {
  ...defaultSchema,
  tagNames: [],
  attributes: {},
};

function hastToText(node: any): string {
  if (node.type === 'text') return node.value;
  if (Array.isArray(node.children)) return node.children.map(hastToText).join('');
  return '';
}

async function stripHtml(input: string): Promise<string> {
  const processor = unified().use(rehypeParse, { fragment: true }).use(rehypeSanitize, stripSchema);
  const tree = await processor.run(processor.parse(String(input)));
  return hastToText(tree).replace(/[<>]/g, '');
}

const DEFAULT_RELAYS = [
  'wss://relay.damus.io',
  'wss://relay.primal.net',
  'wss://relay.snort.social',
  'wss://nos.lol',
];

// Habla.news is a dedicated long-form content viewer that handles naddr links well
const NOSTR_ARTICLE_VIEWER_URL = 'https://habla.news';

/**
 * Convert Markdown content to HTML for RSS readers
 * Handles common Markdown syntax used in Nostr long-form content
 */
function convertMarkdownToHtml(markdown: string, featuredImage?: string): string {
  let html = markdown;
  
  // Escape HTML entities first (but preserve intentional HTML)
  html = html
    .replace(/&(?!amp;|lt;|gt;|quot;|#)/g, '&amp;')
    
  // Code blocks (fenced) - must be done before other transformations
  html = html.replace(/```(\w*)\n([\s\S]*?)```/g, (_, lang, code) => {
    const escapedCode = code
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;');
    return `<pre><code class="language-${lang || 'text'}">${escapedCode}</code></pre>`;
  });
  
  // Inline code
  html = html.replace(/`([^`]+)`/g, '<code>$1</code>');
  
  // Headers (h1-h6)
  html = html.replace(/^######\s+(.+)$/gm, '<h6>$1</h6>');
  html = html.replace(/^#####\s+(.+)$/gm, '<h5>$1</h5>');
  html = html.replace(/^####\s+(.+)$/gm, '<h4>$1</h4>');
  html = html.replace(/^###\s+(.+)$/gm, '<h3>$1</h3>');
  html = html.replace(/^##\s+(.+)$/gm, '<h2>$1</h2>');
  html = html.replace(/^#\s+(.+)$/gm, '<h1>$1</h1>');
  
  // Images - ![alt](url)
  html = html.replace(/!\[([^\]]*)\]\(([^)]+)\)/g, '<img src="$2" alt="$1" style="max-width: 100%; height: auto;" />');
  
  // Links - [text](url)
  html = html.replace(/\[([^\]]+)\]\(([^)]+)\)/g, '<a href="$2">$1</a>');
  
  // Bold - **text** or __text__
  html = html.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
  html = html.replace(/__([^_]+)__/g, '<strong>$1</strong>');
  
  // Italic - *text* or _text_
  html = html.replace(/\*([^*]+)\*/g, '<em>$1</em>');
  html = html.replace(/_([^_]+)_/g, '<em>$1</em>');
  
  // Strikethrough - ~~text~~
  html = html.replace(/~~([^~]+)~~/g, '<del>$1</del>');
  
  // Blockquotes
  html = html.replace(/^>\s+(.+)$/gm, '<blockquote>$1</blockquote>');
  // Merge consecutive blockquotes
  html = html.replace(/<\/blockquote>\n<blockquote>/g, '\n');
  
  // Horizontal rules
  html = html.replace(/^[-*_]{3,}$/gm, '<hr />');
  
  // Unordered lists
  html = html.replace(/^[\*\-]\s+(.+)$/gm, '<li>$1</li>');
  html = html.replace(/(<li>.*<\/li>\n?)+/g, '<ul>$&</ul>');
  
  // Ordered lists
  html = html.replace(/^\d+\.\s+(.+)$/gm, '<li>$1</li>');
  
  // Paragraphs - wrap remaining text blocks
  // Split by double newlines and wrap non-HTML blocks
  const blocks = html.split(/\n\n+/);
  html = blocks.map(block => {
    const trimmed = block.trim();
    if (!trimmed) return '';
    // Don't wrap if already an HTML block element
    if (/^<(h[1-6]|p|div|ul|ol|li|blockquote|pre|hr|img)/i.test(trimmed)) {
      return trimmed;
    }
    // Wrap in paragraph, converting single newlines to <br>
    return `<p>${trimmed.replace(/\n/g, '<br />')}</p>`;
  }).join('\n\n');
  
  // Add featured image at the top if provided and not already in content
  if (featuredImage && !html.includes(featuredImage)) {
    html = `<img src="${featuredImage}" alt="Featured image" style="max-width: 100%; height: auto; margin-bottom: 1em;" />\n\n${html}`;
  }
  
  return html;
}

export async function GET(request: NextRequest) {
  const { searchParams } = new URL(request.url);
  const npub = searchParams.get('npub');
  const tags = searchParams.get('tags')?.split(',').map(t => t.trim()).filter(Boolean);

  if (!npub || !npub.startsWith('npub1')) {
    return new NextResponse('Invalid or missing npub parameter.', { status: 400 });
  }

  let pubkey: string;
  try {
    const { type, data } = nip19.decode(npub);
    if (type !== 'npub') {
      return new NextResponse('Invalid npub.', { status: 400 });
    }
    pubkey = data as string;
  } catch (error) {
    return new NextResponse('Error decoding npub.', { status: 400 });
  }

  const pool = new SimplePool();
  const filter: Filter = {
    authors: [pubkey],
    kinds: [30023], // Long-form content
    limit: 20,
  };

  if (tags && tags.length > 0) {
    filter['#t'] = tags;
  }

  try {
    const events = await pool.querySync(DEFAULT_RELAYS, filter);

    if (!events || events.length === 0) {
      return new NextResponse('No long-form posts found for this user with the specified tags.', { 
        status: 404,
        headers: { 'Content-Type': 'text/plain' }
      });
    }
    
    // Fetch user profile (kind 0) to get metadata
    const profileEvents = await pool.querySync(DEFAULT_RELAYS, {
      authors: [pubkey],
      kinds: [0],
      limit: 1,
    });

    let authorName = npub;
    let authorAbout = 'A Nostr user.';
    let authorPicture = '';
    if (profileEvents && profileEvents.length > 0) {
        try {
            const metadata = JSON.parse(profileEvents[0].content);
            authorName = metadata.displayName || metadata.name || authorName;
            authorAbout = metadata.about || authorAbout;
            authorPicture = /^https?:\/\//i.test(metadata.picture) ? metadata.picture : '';
        } catch (e) {
            console.error("Error parsing profile metadata:", e);
        }
    }

    authorName = await stripHtml(authorName);
    authorAbout = await stripHtml(authorAbout);

    const feed = new RSS({
      title: `${authorName}'s Nostr Feed`,
      description: `Long-form articles from ${authorName} on Nostr. ${authorAbout}`,
      feed_url: request.url,
      site_url: `https://njump.me/${npub}`,
      image_url: authorPicture,
      managingEditor: authorName,
      webMaster: authorName,
      copyright: `2025 ${authorName}`,
      language: 'en',
      pubDate: new Date().toUTCString(),
      ttl: 60,
      custom_namespaces: {
        'content': 'http://purl.org/rss/1.0/modules/content/',
      },
    });

    for (const event of events) {
      const rawTitle = event.tags.find((t: string[]) => t[0] === 'title')?.[1] || 'Untitled';
      const published = event.tags.find((t: string[]) => t[0] === 'published_at')?.[1];
      const summary = event.tags.find((t: string[]) => t[0] === 'summary')?.[1];
      const dTag = event.tags.find((t: string[]) => t[0] === 'd')?.[1];
      const image = event.tags.find((t: string[]) => t[0] === 'image')?.[1];
      
      // The full article content is in event.content (usually Markdown)
      const fullContent = event.content || '';
      
      // Convert Markdown to simple HTML for RSS readers
      // Basic conversion: paragraphs, headers, links, bold, italic, code, images
      const htmlContent = await sanitizeHtml(convertMarkdownToHtml(fullContent, image));
      
      // Build a proper naddr URL for long-form articles using Habla.news
      // naddr includes: kind, pubkey, d-tag identifier
      let articleUrl: string;
      if (dTag) {
        const naddr = nip19.naddrEncode({
          kind: 30023,
          pubkey: pubkey,
          identifier: dTag,
        });
        articleUrl = `${NOSTR_ARTICLE_VIEWER_URL}/a/${naddr}`;
      } else {
        // Fallback to njump.me with nevent if no d-tag
        const nevent = nip19.neventEncode({ id: event.id, relays: DEFAULT_RELAYS.slice(0, 2) });
        articleUrl = `https://njump.me/${nevent}`;
      }

      const title = await stripHtml(rawTitle);
      const description = summary
        ? await stripHtml(summary)
        : (await stripHtml(fullContent.slice(0, 1000))).slice(0, 300) + '...';

      feed.item({
        title: title,
        description: description,
        url: articleUrl,
        guid: event.id,
        author: authorName,
        date: published ? new Date(parseInt(published) * 1000) : new Date(event.created_at * 1000),
        custom_elements: [
          { 'content:encoded': { _cdata: htmlContent } },
        ],
      });
    }

    pool.close(DEFAULT_RELAYS);

    const xml = feed.xml({ indent: true });
    return new NextResponse(xml, {
      status: 200,
      headers: { 'Content-Type': 'application/xml' },
    });

  } catch (error) {
    console.error('Failed to fetch Nostr events or generate RSS feed:', error);
    pool.close(DEFAULT_RELAYS);
    return new NextResponse('Error generating RSS feed.', { status: 500 });
  }
}
