'use client'

import { useEffect } from 'react'

// next-pwa registers the service worker with skipWaiting + clientsClaim, so a new
// service worker activates and claims clients on its own. What's missing is the
// page actually reloading onto the new bundles, plus a proactive update check —
// browsers only re-fetch sw.js on navigation or every ~24h, so a long-lived
// standalone PWA can keep serving stale code for a long time after a deploy.
export function ServiceWorkerUpdater() {
  useEffect(() => {
    if (typeof navigator === 'undefined' || !('serviceWorker' in navigator)) return

    let updateActivated = false
    let reloaded = false

    // Reload only after a real update activates. The initial registration also
    // claims this (previously uncontrolled) page and fires controllerchange, but
    // updateActivated is still false then, so first load never reloads.
    const onControllerChange = () => {
      if (!updateActivated || reloaded) return
      reloaded = true
      window.location.reload()
    }
    navigator.serviceWorker.addEventListener('controllerchange', onControllerChange)

    let registration: ServiceWorkerRegistration | undefined

    const trackUpdates = (reg: ServiceWorkerRegistration) => {
      registration = reg
      reg.addEventListener('updatefound', () => {
        const newWorker = reg.installing
        if (!newWorker) return
        newWorker.addEventListener('statechange', () => {
          // A new version finished installing while a controller already exists →
          // this is an update, not a first install. skipWaiting + clientsClaim
          // then activate and claim, firing controllerchange (handled above).
          if (newWorker.state === 'installed' && navigator.serviceWorker.controller) {
            updateActivated = true
          }
        })
      })
    }

    navigator.serviceWorker.ready.then(trackUpdates).catch(() => {})

    const checkForUpdate = () => {
      registration?.update().catch(() => {})
    }
    const onVisibility = () => {
      if (document.visibilityState === 'visible') checkForUpdate()
    }
    document.addEventListener('visibilitychange', onVisibility)
    const intervalId = window.setInterval(checkForUpdate, 60 * 60 * 1000)

    return () => {
      navigator.serviceWorker.removeEventListener('controllerchange', onControllerChange)
      document.removeEventListener('visibilitychange', onVisibility)
      window.clearInterval(intervalId)
    }
  }, [])

  return null
}
