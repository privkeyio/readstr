import './globals.css'
import { Inter, Space_Grotesk, Source_Serif_4, Playfair_Display, Crimson_Pro } from 'next/font/google'
import { TRPCReactProvider } from '@/trpc/react'
import { NostrAuthProvider } from '@/contexts/NostrAuthContext'
import { ThemeProvider } from '@/contexts/ThemeContext'
import { AiConfigProvider } from '@/lib/ai/config'
import { Footer } from '@/components/footer'
import { ServiceWorkerUpdater } from '@/components/service-worker-updater'

const inter = Inter({ subsets: ['latin'], variable: '--font-inter' })
const spaceGrotesk = Space_Grotesk({
  subsets: ['latin'],
  variable: '--font-brand',
  weight: ['400', '500', '600', '700'],
})
const sourceSerif = Source_Serif_4({
  subsets: ['latin'],
  display: 'swap',
  variable: '--font-source-serif',
})
const playfair = Playfair_Display({
  subsets: ['latin'],
  display: 'swap',
  variable: '--font-playfair',
})
const crimson = Crimson_Pro({
  subsets: ['latin'],
  display: 'swap',
  variable: '--font-crimson',
})

export const metadata = {
  title: 'Readstr — by PrivKey',
  description: 'Readstr: a sovereign RSS + Nostr feed reader by PrivKey. Your keys, your stack.',
  appleWebApp: {
    capable: true,
    statusBarStyle: 'default',
    title: 'Readstr',
  },
  openGraph: {
    title: 'Readstr — by PrivKey',
    description: 'A sovereign RSS + Nostr feed reader by PrivKey. Your keys, your stack.',
    siteName: 'Readstr',
    type: 'website',
  },
}

export const viewport = {
  width: 'device-width',
  initialScale: 1,
  maximumScale: 5,
  themeColor: '#27ae60',
}

export default function RootLayout({
  children,
}: {
  children: React.ReactNode
}) {
  return (
    <html lang="en" className={`${spaceGrotesk.variable} ${inter.variable} ${sourceSerif.variable} ${playfair.variable} ${crimson.variable}`}>
      <head>
        <link rel="manifest" href="/manifest.json" />
        <link rel="icon" href="/privkey-favicon.png" type="image/png" />
        <link rel="apple-touch-icon" href="/privkey-favicon.png" />
        <meta name="theme-color" content="#27ae60" />
      </head>
      <body className={`${inter.className} flex flex-col min-h-screen`}>
        <ThemeProvider>
          <AiConfigProvider>
            <ServiceWorkerUpdater />
            <NostrAuthProvider>
              <TRPCReactProvider>
                <div className="flex-1">
                  {children}
                </div>
                <Footer />
              </TRPCReactProvider>
            </NostrAuthProvider>
          </AiConfigProvider>
        </ThemeProvider>
      </body>
    </html>
  )
}