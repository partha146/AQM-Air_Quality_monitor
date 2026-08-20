/**
 * AQM v2.0 Service Worker
 * Caches app shell for instant load. Firebase data always from network.
 */

const CACHE_NAME = 'aqm-v5';

const APP_SHELL = [
  './',
  './index.html',
  './app.js',
  './styles.css',
  'https://cdn.jsdelivr.net/npm/chart.js',
  'https://unpkg.com/lucide@0.460.0'
];

// Install: cache app shell
self.addEventListener('install', event => {
  event.waitUntil(
    caches.open(CACHE_NAME).then(cache => {
      console.log('[SW] Caching app shell...');
      return Promise.allSettled(APP_SHELL.map(url => cache.add(url)));
    }).then(() => self.skipWaiting())
  );
});

// Activate: clean old caches
self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys.filter(k => k !== CACHE_NAME).map(k => caches.delete(k)))
    ).then(() => self.clients.claim())
  );
});

// Fetch: network-first for Firebase, cache-first for shell
self.addEventListener('fetch', event => {
  const url = event.request.url;

  // Firebase: always network
  if (url.includes('firebasedatabase.app') || url.includes('firebase')) {
    event.respondWith(fetch(event.request));
    return;
  }

  // App shell: cache-first, fallback to network
  event.respondWith(
    caches.match(event.request).then(cached => {
      if (cached) return cached;
      return fetch(event.request).then(response => {
        if (response && response.status === 200) {
          const clone = response.clone();
          caches.open(CACHE_NAME).then(cache => cache.put(event.request, clone));
        }
        return response;
      }).catch(() => {
        if (event.request.mode === 'navigate') {
          return caches.match('./index.html');
        }
      });
    })
  );
});
