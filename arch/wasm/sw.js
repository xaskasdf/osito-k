/* OsitoK Service Worker — minimal offline support.
 *
 * Cache-first for static kernel artifacts (osito.html/js/wasm and the
 * default GGUF model). Network-first for everything else (R2 fetches,
 * fetch() bridges, etc.) so live data isn't trapped in stale cache.
 *
 * To install: registered automatically on first visit; PWA install
 * prompt appears once registration completes.
 */

const CACHE = 'osito-k-v1';
const PRECACHE = [
    './osito.html',
    './osito.js',
    './osito.wasm',
    './manifest.json',
];

self.addEventListener('install', (event) => {
    event.waitUntil(
        caches.open(CACHE)
            .then((c) => Promise.allSettled(PRECACHE.map((u) => c.add(u))))
            .then(() => self.skipWaiting())
    );
});

self.addEventListener('activate', (event) => {
    event.waitUntil(
        caches.keys().then((keys) =>
            Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k)))
        ).then(() => self.clients.claim())
    );
});

self.addEventListener('fetch', (event) => {
    const url = new URL(event.request.url);

    /* Same-origin static artifacts: cache-first */
    if (url.origin === self.location.origin) {
        event.respondWith(
            caches.match(event.request).then((cached) => {
                if (cached) return cached;
                return fetch(event.request)
                    .then((resp) => {
                        if (resp.ok) {
                            const clone = resp.clone();
                            caches.open(CACHE).then((c) => c.put(event.request, clone));
                        }
                        return resp;
                    })
                    .catch(() => cached);
            })
        );
        return;
    }

    /* Cross-origin (R2 GGUF, FS images, fetch bridge): network-first
     * with cache fallback so first-load is fresh, offline still works. */
    event.respondWith(
        fetch(event.request)
            .then((resp) => {
                if (resp.ok && (event.request.url.endsWith('.gguf') ||
                                event.request.url.endsWith('.img'))) {
                    const clone = resp.clone();
                    caches.open(CACHE).then((c) => c.put(event.request, clone));
                }
                return resp;
            })
            .catch(() => caches.match(event.request))
    );
});
