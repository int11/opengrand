// PWA: registration guard, emitted manifest / icons, and the service worker's
// caching strategy (run in a vm sandbox with fake caches and fetch).
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { runInNewContext } from "node:vm";
import { spawnSync } from "node:child_process";

import { emitPwa, serviceWorker } from "../scripts/pwa.ts";
import { registerServiceWorker, swAllowed } from "../src/pwa.ts";

test("service worker only on https / localhost, never in the Android shell", () => {
  assert.equal(swAllowed({ protocol: "https:", hostname: "example.org" }, false), true);
  assert.equal(swAllowed({ protocol: "http:", hostname: "localhost" }, false), true);
  assert.equal(swAllowed({ protocol: "http:", hostname: "127.0.0.1" }, false), true);
  assert.equal(swAllowed({ protocol: "http:", hostname: "198.51.100.10" }, false), false);
  assert.equal(swAllowed({ protocol: "file:", hostname: "" }, false), false);
  assert.equal(swAllowed({ protocol: "https:", hostname: "appassets.androidplatform.net" }, true), false);

  const registered: string[] = [];
  const nav = { serviceWorker: { register: (u: string) => (registered.push(u), Promise.resolve()) } } as unknown as Navigator;
  assert.equal(registerServiceWorker(false, { protocol: "http:", hostname: "localhost" }, nav), true);
  assert.equal(registerServiceWorker(true, { protocol: "https:", hostname: "x" }, nav), false);
  assert.equal(registerServiceWorker(false, { protocol: "https:", hostname: "x" }, {} as Navigator), false); // old WebView
  assert.deepEqual(registered, ["sw.js"]);
});

test("build emits an installable manifest, PNG icons and a service worker", () => {
  const dir = mkdtempSync(join(tmpdir(), "pwa-"));
  try {
    const files = emitPwa(dir, { version: "9.9.9", assets: ["index.html", "app-ABC.js", "styles-DEF.css"] });
    assert.deepEqual(files.sort(), ["icon-192.png", "icon-512.png", "icon-maskable-512.png", "manifest.webmanifest", "sw.js"]);
    const m = JSON.parse(readFileSync(join(dir, "manifest.webmanifest"), "utf8"));
    assert.equal(m.display, "standalone");
    assert.equal(m.start_url, "./");
    assert.match(m.theme_color, /^#[0-9a-f]{6}$/);
    assert.ok(m.icons.some((i: { sizes: string }) => i.sizes === "192x192"));
    assert.ok(m.icons.some((i: { sizes: string; purpose: string }) => i.sizes === "512x512" && i.purpose === "maskable"));
    for (const [f, size] of [["icon-192.png", 192], ["icon-512.png", 512], ["icon-maskable-512.png", 512]] as const) {
      const png = readFileSync(join(dir, f));
      assert.deepEqual([...png.subarray(0, 8)], [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
      assert.equal(png.readUInt32BE(16), size);
      assert.equal(png.readUInt32BE(20), size);
    }
    const sw = readFileSync(join(dir, "sw.js"), "utf8");
    assert.ok(!sw.includes("__PRECACHE__") && !sw.includes("__VERSION__"));
    assert.match(sw, /"app-ABC\.js"/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

// ---- the service worker itself -------------------------------------------

const SCOPE = "https://pwa.test/app/";

type Res = { ok: boolean; status: number; body: string; clone(): Res };
const res = (body: string, status = 200): Res => ({ ok: status < 300, status, body, clone() { return this; } });

class FakeCache {
  m = new Map<string, string>();
  load: (url: string) => Promise<Res>;
  constructor(load: (url: string) => Promise<Res> = async (u) => res(`net:${u}`)) {
    this.load = load;
  }
  async addAll(urls: string[]) {
    for (const u of urls) {
      const r = await this.load(u);
      if (!r.ok) throw new TypeError(`addAll: ${u} ${r.status}`); // Cache.addAll fails as a whole
      this.m.set(u, r.body);
    }
  }
  async put(req: string | { url: string }, res: { body: string }) {
    this.m.set(typeof req === "string" ? req : req.url, res.body);
  }
  async match(req: string | { url: string }) {
    const b = this.m.get(typeof req === "string" ? req : req.url);
    return b === undefined ? undefined : { ok: true, body: b, clone() { return this; } };
  }
}

/**
 * Run a service worker source in a sandbox. `net.mode`: "online" (the server answers through
 * `serve`), "offline" (fetch rejects, as with the dev server stopped), "hang" (never answers),
 * "error" (the server answers 404).
 */
function loadSw(src: string, serve: (url: string) => Res = (u) => res(`net:${u}`)) {
  const stores = new Map<string, FakeCache>();
  const handlers: Record<string, (e: unknown) => void> = {};
  const net = { online: true, mode: "online" as "online" | "offline" | "hang" | "error", calls: [] as string[] };
  const fakeFetch = (req: { url: string } | string): Promise<Res> => {
    const url = typeof req === "string" ? req : req.url;
    net.calls.push(url);
    const mode = net.online ? net.mode : "offline";
    if (mode === "offline") return Promise.reject(new TypeError("Failed to fetch"));
    if (mode === "hang") return new Promise(() => {});
    if (mode === "error") return Promise.resolve(res("not found", 404));
    return Promise.resolve(serve(url));
  };
  const self = {
    registration: { scope: SCOPE },
    location: { origin: new URL(SCOPE).origin },
    skipWaiting: async () => undefined,
    clients: { claim: async () => undefined },
    addEventListener: (t: string, h: (e: unknown) => void) => (handlers[t] = h),
  };
  const caches = {
    open: async (n: string) => (stores.has(n) ? stores.get(n)! : (stores.set(n, new FakeCache(fakeFetch)), stores.get(n)!)),
    keys: async () => [...stores.keys()],
    delete: async (n: string) => stores.delete(n),
  };
  // timers run fast: the page timeout (4 s) is exercised without waiting for it
  const fastTimer = (f: () => void, ms: number) => setTimeout(f, Math.min(ms, 20));
  runInNewContext(src, { self, caches, fetch: fakeFetch, URL, setTimeout: fastTimer, clearTimeout, Response: { error: () => ({ ok: false, body: "error" }) } });
  const run = async (type: string, extra: object = {}) => {
    let p: Promise<unknown> | undefined;
    let responded: Promise<{ body: string }> | undefined;
    handlers[type]({ ...extra, waitUntil: (x: Promise<unknown>) => (p = x), respondWith: (x: Promise<{ body: string }>) => (responded = x) });
    await p;
    return responded ? await responded : undefined;
  };
  const get = (path: string, mode = "no-cors") => run("fetch", { request: { method: "GET", url: new URL(path, SCOPE).href, mode } });
  return { stores, net, run, get };
}

test("service worker: precache, network-first page, cache-first assets, old caches dropped", async () => {
  const sw = loadSw(serviceWorker("1.0.0-a", ["./", "index.html", "app-ABC.js"]));
  sw.stores.set("ca79ng-old", new FakeCache());
  sw.stores.set("other-app", new FakeCache());
  await sw.run("install");
  await sw.run("activate");
  assert.deepEqual([...sw.stores.keys()].sort(), ["ca79ng-1.0.0-a", "other-app"]);
  const cache = sw.stores.get("ca79ng-1.0.0-a")!;
  assert.ok(cache.m.has(SCOPE + "app-ABC.js") && cache.m.has(SCOPE + "index.html"));

  // hashed asset: served from the cache without touching the network
  sw.net.calls.length = 0;
  assert.equal((await sw.get("app-ABC.js"))?.body, `net:${SCOPE}app-ABC.js`);
  assert.deepEqual(sw.net.calls, []);

  // page: network first, the fresh copy replaces the cached index.html
  const page = await sw.get("./?sim", "navigate");
  assert.equal(page?.body, `net:${SCOPE}?sim`);
  assert.equal(cache.m.get(SCOPE + "index.html"), `net:${SCOPE}?sim`);

  // offline start: the cached page comes back
  sw.net.online = false;
  assert.equal((await sw.get("index.html", "navigate"))?.body, `net:${SCOPE}?sim`);

  // other origins and non-GET requests are left to the browser
  const r = await sw.run("fetch", { request: { method: "GET", url: "https://cdn.example/x.js", mode: "cors" } });
  assert.equal(r, undefined);
  const post = await sw.run("fetch", { request: { method: "POST", url: SCOPE + "x", mode: "cors" } });
  assert.equal(post, undefined);
});

test("service worker: the page falls back to the last cached copy on a 404 and on a server that does not answer", async () => {
  const sw = loadSw(serviceWorker("1.0.0-b", ["./", "index.html", "app-ABC.js"]));
  await sw.run("install");
  await sw.run("activate");
  const cached = `net:${SCOPE}index.html`;
  sw.net.mode = "error"; // dist being rebuilt, or another server on the port
  assert.equal((await sw.get("./", "navigate"))?.body, cached);
  sw.net.mode = "hang"; // half-open network: the cached page after PAGE_TIMEOUT_MS (shortened here)
  assert.equal((await sw.get("./", "navigate"))?.body, cached);
  sw.net.mode = "online";
  assert.equal((await sw.get("./", "navigate"))?.body, `net:${SCOPE}`);
});

test("service worker: with nothing cached yet, a server error is passed through (not an empty response)", async () => {
  const sw = loadSw(serviceWorker("1.0.0-c", []));
  await sw.run("install");
  sw.net.mode = "error";
  const r = (await sw.get("./", "navigate")) as unknown as Res;
  assert.equal(r.status, 404);
});

test("the real build opens offline: every file index.html and the manifest load comes from the precache", () => {
  // npm run build into a temporary folder, then: first visit online (install), dev server stopped, start again
  const dir = mkdtempSync(join(tmpdir(), "pwa-build-"));
  const run = async () => {
    const appRoot = new URL("..", import.meta.url).pathname;
    const b = spawnSync(process.execPath, ["scripts/build.mjs"], { cwd: appRoot, env: { ...process.env, CA79_DIST: dir }, encoding: "utf8" });
    assert.equal(b.status, 0, b.stderr);
    const file = (rel: string) => readFileSync(join(dir, rel), "latin1");
    const serve = (url: string): Res => {
      const rel = url.slice(SCOPE.length).split("?")[0] || "index.html";
      try { return res(file(rel)); } catch { return res("not found", 404); }
    };
    const sw = loadSw(file("sw.js"), serve);
    await sw.run("install");
    await sw.run("activate");

    sw.net.online = false;
    sw.net.calls.length = 0;
    const html = (await sw.get("./", "navigate"))?.body ?? "";
    assert.equal(html, file("index.html"), "the start_url opens the last build's page");
    const refs = [...html.matchAll(/(?:href|src)="([^"]+)"/g)].map((m) => m[1]!).filter((u) => !/^[a-z]+:/i.test(u));
    const manifest = JSON.parse(file("manifest.webmanifest")) as { start_url: string; icons: { src: string }[] };
    assert.equal(manifest.start_url, "./");
    const need = [...new Set([...refs, ...manifest.icons.map((i) => i.src)])];
    assert.ok(need.some((u) => /^app-\w+\.js$/.test(u)) && need.some((u) => /^styles-\w+\.css$/.test(u)), need.join(" "));
    for (const u of need) assert.equal((await sw.get(u))?.body, file(u), `${u} offline`);
    assert.deepEqual(sw.net.calls, [SCOPE], "only the page tried the network (and fell back)");
  };
  return run().finally(() => rmSync(dir, { recursive: true, force: true }));
});
