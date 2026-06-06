'use client';

import ReactMarkdown from 'react-markdown';
import remarkGfm from 'remark-gfm';
import rehypeRaw from 'rehype-raw';
import rehypeSanitize from 'rehype-sanitize';
import { VideoEmbed } from './video-embed';
import { detectVideoPlatform } from '@/lib/video-parser';

interface FormattedContentProps {
  content: string;
  className?: string;
  embedUrl?: string;
  thumbnail?: string;
  title?: string;
}

/**
 * Detects content type and formats it appropriately
 * Supports: Markdown, HTML, plain text, and video embeds
 */
export function FormattedContent({
  content,
  className = '',
  embedUrl,
  thumbnail,
  title = 'Video',
}: FormattedContentProps) {
  // If we have a video embed URL, show the video player first
  const showVideoEmbed = embedUrl && embedUrl.trim().length > 0;
  const videoPlatform = embedUrl ? detectVideoPlatform(embedUrl) : 'unknown';

  // Detect if content is likely HTML
  const isHtml = /<[^>]+>/.test(content);

  // Detect if content is likely Markdown
  const hasMarkdownSyntax = /[#*_`[\]]/g.test(content);

  return (
    <div className={`formatted-content-wrapper ${className}`}>
      {/* Video Embed Section */}
      {showVideoEmbed && (
        <div className="mb-8 rounded-xl overflow-hidden shadow-theme-md">
          <VideoEmbed
            embedUrl={embedUrl}
            title={title}
            thumbnail={thumbnail}
            platform={videoPlatform !== 'unknown' ? videoPlatform : undefined}
          />
        </div>
      )}

      {/* Text Content Section */}
      {isHtml || hasMarkdownSyntax ? (
        // Use react-markdown which handles both Markdown and HTML
        <div className="formatted-content">
          <ReactMarkdown
            remarkPlugins={[remarkGfm]} // GitHub Flavored Markdown (tables, strikethrough, etc.)
            rehypePlugins={[rehypeRaw, rehypeSanitize]} // Allow HTML but sanitize it
            components={{
              // Custom component overrides for better styling
              a: ({ node, ...props }) => (
                <a
                  className="text-[rgb(var(--color-accent))] hover:text-[rgb(var(--color-accent-hover))] underline underline-offset-2 transition-colors"
                  target="_blank"
                  rel="noopener noreferrer"
                  {...props}
                />
              ),
              img: ({ node, ...props }) => (
                // eslint-disable-next-line @next/next/no-img-element -- remote image from arbitrary feed content, not suited to next/image
                <img
                  alt=""
                  className="max-w-full h-auto rounded-xl shadow-theme-md my-6"
                  loading="lazy"
                  {...props}
                />
              ),
              table: ({ node, ...props }) => (
                <div className="overflow-x-auto my-6 rounded-lg border border-[rgb(var(--color-border-primary))]">
                  <table
                    className="min-w-full divide-y divide-[rgb(var(--color-border-primary))]"
                    {...props}
                  />
                </div>
              ),
              thead: ({ node, ...props }) => (
                <thead
                  className="bg-[rgb(var(--color-bg-tertiary))]"
                  {...props}
                />
              ),
              th: ({ node, ...props }) => (
                <th
                  className="px-4 py-3 text-left text-sm font-semibold text-[rgb(var(--color-text-primary))]"
                  {...props}
                />
              ),
              td: ({ node, ...props }) => (
                <td
                  className="px-4 py-3 text-sm text-[rgb(var(--color-text-secondary))] border-t border-[rgb(var(--color-border-secondary))]"
                  {...props}
                />
              ),
              blockquote: ({ node, ...props }) => (
                <blockquote
                  className="border-l-4 border-[rgb(var(--color-border-accent))] pl-6 my-6 italic text-[rgb(var(--color-text-secondary))]"
                  {...props}
                />
              ),
              code: ({ node, inline, ...props }: any) => 
                inline ? (
                  <code
                    className="px-1.5 py-0.5 rounded-md bg-[rgb(var(--color-bg-tertiary))] text-[rgb(var(--color-text-primary))] font-mono text-sm"
                    {...props}
                  />
                ) : (
                  <code {...props} />
                ),
              pre: ({ node, ...props }) => (
                <pre
                  className="p-4 rounded-xl bg-[rgb(var(--color-bg-tertiary))] overflow-x-auto my-6 font-mono text-sm"
                  {...props}
                />
              ),
              h1: ({ node, ...props }) => (
                <h1 className="text-3xl font-bold mt-8 mb-4 text-[rgb(var(--color-text-primary))]" style={{ fontFamily: 'var(--heading-font)' }} {...props} />
              ),
              h2: ({ node, ...props }) => (
                <h2 className="text-2xl font-bold mt-8 mb-3 text-[rgb(var(--color-text-primary))]" style={{ fontFamily: 'var(--heading-font)' }} {...props} />
              ),
              h3: ({ node, ...props }) => (
                <h3 className="text-xl font-bold mt-6 mb-3 text-[rgb(var(--color-text-primary))]" style={{ fontFamily: 'var(--heading-font)' }} {...props} />
              ),
              h4: ({ node, ...props }) => (
                <h4 className="text-lg font-bold mt-6 mb-2 text-[rgb(var(--color-text-primary))]" style={{ fontFamily: 'var(--heading-font)' }} {...props} />
              ),
              p: ({ node, ...props }) => (
                <p className="mb-4 leading-relaxed text-[rgb(var(--color-text-primary))]" style={{ fontFamily: 'var(--content-font)' }} {...props} />
              ),
              ul: ({ node, ...props }) => (
                <ul className="list-disc pl-6 mb-4 space-y-1 text-[rgb(var(--color-text-primary))]" {...props} />
              ),
              ol: ({ node, ...props }) => (
                <ol className="list-decimal pl-6 mb-4 space-y-1 text-[rgb(var(--color-text-primary))]" {...props} />
              ),
              li: ({ node, ...props }) => (
                <li className="leading-relaxed" {...props} />
              ),
              hr: ({ node, ...props }) => (
                <hr className="my-8 border-t border-[rgb(var(--color-border-primary))]" {...props} />
              ),
            }}
          >
            {content}
          </ReactMarkdown>
        </div>
      ) : (
        // Plain text - split into paragraphs
        <div className="formatted-content">
          {content.split('\n\n').map((paragraph, index) => {
            if (paragraph.trim()) {
              return (
                <p
                  key={index}
                  className="mb-4 leading-relaxed text-[rgb(var(--color-text-primary))] whitespace-pre-wrap"
                  style={{ fontFamily: 'var(--content-font)' }}
                >
                  {paragraph}
                </p>
              );
            }
            return null;
          })}
        </div>
      )}
    </div>
  );
}
