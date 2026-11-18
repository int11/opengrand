// OpenGrand service worker (template: scripts/pwa.mjs fills in VERSION and PRECACHE).
//   - index.html / navigations: network first, cached copy when offline
//   - everything else of this origin (content-hashed app-*.js, styles-*.css, icons): cache first
// Not registered in the Android shell (assets are local there) nor on plain http
// other than localhost (src/pwa.ts).
const VERSION = "__VERSION__";
const PRECACHE = __PRECACHE__;
const PREFIX = "ca79ng-";
const CACHE = PREFIX + VERSION;
const scopeUrl = (p) => new URL(p, self.registration.scope).href;

self.addEventListener("install", (e) => {
  e.waitUntil(
    caches
      .open(CACHE)
      .then((c) => c.addAll(PRECACHE.map(scopeUrl)))
      .then(() => self.skipWaiting()),
  );
});

self.addEventListener("activate", (e) => {
  e.waitUntil(
    caches
      .keys()
      .then((keys) => Promise.all(keys.filter((k) => k.startsWith(PREFIX) && k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim()),
  );
});

function isPage(req, url) {
  return req.mode === "navigate" || url.href === scopeUrl("./") || url.href === scopeUrl("index.html");
}

// The installed app must open with the last build when the server is gone (dev server stopped:
// fetch rejects), answers with an error (404 / 5xx: dist being rebuilt, another server on the port)
// or does not answer (half-open network): then the cached page, if there is one.
const PAGE_TIMEOUT_MS = 4000;

async function networkFirst(req) {
  const cache = await caches.open(CACHE);
  const key = scopeUrl("index.html");
  const cached = await cache.match(key);
  let timer;
  const net = fetch(req).then(async (res) => {
    if (res.ok) await cache.put(key, res.clone());
    return res;
  });
  const late = cached ? new Promise((resolve) => (timer = setTimeout(() => resolve(null), PAGE_TIMEOUT_MS))) : null;
  try {
    const res = await (late ? Promise.race([net, late]) : net);
    if (res && (res.ok || !cached)) return res;
    return cached;
  } catch {
    return cached || Response.error();
  } finally {
    clearTimeout(timer);
    net.catch(() => {}); // a late failure after the timeout is not an unhandled rejection
  }
}

async function cacheFirst(req) {
  const cache = await caches.open(CACHE);
  const hit = await cache.match(req);
  if (hit) return hit;
  const res = await fetch(req);
  if (res.ok && !new URL(req.url).pathname.endsWith(".map")) await cache.put(req, res.clone());
  return res;
}

self.addEventListener("fetch", (e) => {
  const req = e.request;
  if (req.method !== "GET") return;
  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;
  // strip ?sim / ?tab=… for the page key; the app reads them from location itself
  e.respondWith(isPage(req, new URL(url.pathname, url.origin)) ? networkFirst(req) : cacheFirst(req));
});
