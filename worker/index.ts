// Bundled by next-pwa into sw.js (customWorkerSrc defaults to "worker").
//
// Builds before #152 cached signed-in tRPC responses under 'apis'. The page
// also deletes it on mount, but that only runs once the app loads, so drop it
// as soon as this worker activates too.
self.addEventListener('activate', (event) => {
  const activate = event as Event & { waitUntil(promise: Promise<unknown>): void }
  activate.waitUntil(caches.delete('apis').catch(() => false))
})

export {}
