'use client'

import { useState } from 'react'

interface VideoEmbedProps {
  embedUrl: string
  title: string
  thumbnail?: string
  platform?: 'youtube' | 'rumble'
  className?: string
}

/**
 * Responsive video embed component for YouTube and Rumble
 * Uses 16:9 aspect ratio with lazy loading
 */
export function VideoEmbed({
  embedUrl,
  title,
  thumbnail,
  platform,
  className = '',
}: VideoEmbedProps) {
  const [isLoaded, setIsLoaded] = useState(false)

  // Build iframe with proper parameters
  const getIframeSrc = () => {
    try {
      const url = new URL(embedUrl)
      
      if (platform === 'youtube' || embedUrl.includes('youtube.com')) {
        // Add YouTube parameters for better UX
        url.searchParams.set('modestbranding', '1')
        url.searchParams.set('rel', '0')
        url.searchParams.set('playsinline', '1')
      }
      
      return url.toString()
    } catch {
      return embedUrl
    }
  }

  return (
    <div className={`video-embed-container ${className}`}>
      {/* 16:9 Aspect Ratio Container */}
      <div className="relative w-full pb-[56.25%] bg-slate-900 rounded-lg overflow-hidden shadow-lg">
        {/* Thumbnail placeholder (shown before iframe loads) */}
        {!isLoaded && thumbnail && (
          <div className="absolute inset-0 flex items-center justify-center">
            {/* eslint-disable-next-line @next/next/no-img-element -- remote video thumbnail from arbitrary domains, not suited to next/image */}
            <img
              src={thumbnail}
              alt={title}
              className="absolute inset-0 w-full h-full object-cover"
              loading="lazy"
            />
            {/* Play button overlay */}
            <button
              onClick={() => setIsLoaded(true)}
              className="relative z-10 flex items-center justify-center w-20 h-20 bg-red-600 hover:bg-red-700 rounded-full shadow-xl transition-all transform hover:scale-110"
              aria-label="Play video"
            >
              <svg
                className="w-10 h-10 text-white ml-1"
                fill="currentColor"
                viewBox="0 0 24 24"
              >
                <path d="M8 5v14l11-7z" />
              </svg>
            </button>
          </div>
        )}

        {/* Video iframe (lazy loaded on click or immediately if no thumbnail) */}
        {(isLoaded || !thumbnail) && (
          <iframe
            src={getIframeSrc()}
            title={title}
            className="absolute inset-0 w-full h-full"
            allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share"
            allowFullScreen
            loading="lazy"
            referrerPolicy="no-referrer-when-downgrade"
          />
        )}
      </div>

      {/* Video caption */}
      <p className="mt-2 text-sm text-slate-600 dark:text-slate-400 italic">
        {platform === 'youtube' && '▶️ YouTube'}
        {platform === 'rumble' && '📹 Rumble'}
        {!platform && '🎥 Video'}
      </p>
    </div>
  )
}
