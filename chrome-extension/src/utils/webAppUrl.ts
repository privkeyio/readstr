const LOOPBACK_HOSTS = new Set(['localhost', '127.0.0.1', '[::1]']);

export function validateWebAppUrl(urlString: string): string | null {
  try {
    const url = new URL(urlString);
    if (url.protocol === 'https:') return url.href;
    if (url.protocol === 'http:' && LOOPBACK_HOSTS.has(url.hostname)) return url.href;
    return null;
  } catch {
    return null;
  }
}

export function normalizeWebAppUrl(urlString: string): string | null {
  const validated = validateWebAppUrl(urlString);
  return validated ? validated.replace(/\/+$/, '') : null;
}

export function isSameOrigin(url: string, baseUrl: string): boolean {
  try {
    return new URL(url).origin === new URL(baseUrl).origin;
  } catch {
    return false;
  }
}
