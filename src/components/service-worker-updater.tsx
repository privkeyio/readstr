'use client'

import { useEffect } from 'react'

// next-pwa registers the service worker with skipWaiting + clientsClaim, so a new
// service worker activates and claims clients on its own. What's missing is the
// page actually reloading onto the new bundles, plus a proactive update check —
// browsers only re-fetch sw.js on navigation or every ~24h, so a long-lived
// standalone PWA can keep serving stale code for a long time after a deploy.

// Guard against reload loops: if a deploy is mid-rollout or a CDN serves
// inconsistent sw.js bytes, controllerchange can fire repeatedly. We reload at
// most once per settle window and break the loop if it hasn't settled.
const RELOAD_GUARD_KEY = 'sw-auto-reloaded'
const RELOAD_SETTLE_MS = 20 * 1000
const UPDATE_CHECK_MIN_INTERVAL_MS = 5 * 60 * 1000
const UPDATE_CHECK_INTERVAL_MS = 60 * 60 * 1000

export function ServiceWorkerUpdater() {
  useEffect(() => {
    if (!('serviceWorker' in navigator)) return

    const abort = new AbortController()
    const { signal } = abort

    let updateActivated = false
    let reloaded = false

    // If the previous load reloaded us, let the new version settle before clearing
    // the guard so genuine future updates can reload again. A flapping sw.js would
    // reload again before this fires, leaving the guard set and breaking the loop.
    const settleTimer = window.setTimeout(() => {
      try {
        sessionStorage.removeItem(RELOAD_GUARD_KEY)
      } catch {}
    }, RELOAD_SETTLE_MS)

    // Reload only after a real update activates. The initial registration also
    // claims this (previously uncontrolled) page and fires controllerchange, but
    // updateActivated is only armed for a worker that replaces an existing
    // controller, so first load never reloads.
    const onControllerChange = () => {
      if (!updateActivated || reloaded) return
      try {
        if (sessionStorage.getItem(RELOAD_GUARD_KEY)) return
        sessionStorage.setItem(RELOAD_GUARD_KEY, '1')
      } catch {}
      reloaded = true
      window.location.reload()
    }
    navigator.serviceWorker.addEventListener('controllerchange', onControllerChange, { signal })

    // Arm the reload when an updated worker (one replacing an existing controller)
    // reaches installed-or-later. We read state live and re-check on statechange so
    // the skipWaiting race — where the worker is already past 'installed' by the
    // time we look — still arms the reload.
    const armIfUpdate = (worker: ServiceWorker | null) => {
      if (!worker || !navigator.serviceWorker.controller) return
      const evaluate = () => {
        if (
          worker.state === 'installed' ||
          worker.state === 'activating' ||
          worker.state === 'activated'
        ) {
          updateActivated = true
        }
      }
      evaluate()
      worker.addEventListener('statechange', evaluate, { signal })
    }

    let registration: ServiceWorkerRegistration | undefined
    let lastCheckAt = 0
    const checkForUpdate = () => {
      const now = Date.now()
      if (now - lastCheckAt < UPDATE_CHECK_MIN_INTERVAL_MS) return
      lastCheckAt = now
      registration?.update().catch(() => {})
    }

    navigator.serviceWorker.ready
      .then((reg) => {
        registration = reg
        // An update may already be in flight before this resolves (its updatefound
        // already fired), so wire up any worker that's already installing/waiting.
        armIfUpdate(reg.installing ?? reg.waiting)
        reg.addEventListener('updatefound', () => armIfUpdate(reg.installing), { signal })
      })
      .catch(() => {})

    const onVisibility = () => {
      if (document.visibilityState === 'visible') checkForUpdate()
    }
    document.addEventListener('visibilitychange', onVisibility, { signal })
    const intervalId = window.setInterval(checkForUpdate, UPDATE_CHECK_INTERVAL_MS)

    return () => {
      abort.abort()
      window.clearInterval(intervalId)
      window.clearTimeout(settleTimer)
    }
  }, [])

  return null
}
