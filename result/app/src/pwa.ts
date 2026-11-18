// Service-worker registration for the installable web app (PWA).
// Registered only where it is useful and allowed:
//   - secure contexts: https, or localhost / 127.0.0.1 / [::1] (the dev server's
//     http://localhost:8079 counts as secure — so do Web MIDI and Web Bluetooth there);
//   - not in the Android shell: it loads the bundle from its own assets
//     (https://appassets.androidplatform.net) and needs no offline cache.

export interface Where {
  protocol: string;
  hostname: string;
}

export function swAllowed(loc: Where, androidShell: boolean): boolean {
  if (androidShell) return false;
  if (loc.protocol === "https:") return true;
  return loc.protocol === "http:" && ["localhost", "127.0.0.1", "[::1]", "::1"].includes(loc.hostname);
}

/** Register dist/sw.js next to index.html (scope = the app's directory). */
export function registerServiceWorker(androidShell: boolean, loc: Where = location, nav: Navigator = navigator): boolean {
  if (!swAllowed(loc, androidShell) || !("serviceWorker" in nav)) return false;
  nav.serviceWorker.register("sw.js").catch((e: unknown) => console.warn("service worker not registered:", e));
  return true;
}
