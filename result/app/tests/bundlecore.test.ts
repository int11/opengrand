// Panel UI bundle core: the zip layout the original shell expects, the chooser page, the app build for the
// panel WebView, the optional stock UI fallback (a user-supplied local folder; the files here are synthetic),
// the USB container (encrypt only, with a RANDOM test key — no real key material is in this repo) and the
// size report.
import { test } from "node:test";
import assert from "node:assert/strict";
import { createCipheriv, createHash, randomBytes } from "node:crypto";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { JSDOM, VirtualConsole } from "jsdom";

import { assemble, buildApp, chooserHtml, readStockUi, sizeReport, verifyZip } from "../scripts/bundlecore.ts";
import { encryptUsbBundle, parseUsbKey } from "../scripts/usbbundle.ts";
import { readZip, writeZip } from "../scripts/zip.ts";

// a synthetic stand-in for a user's locally extracted stock UI folder (paths relative to appUI/)
const fakeStockFiles = () => [
  { path: "index.html", data: Buffer.from("<script>requestAnimationFrame(()=>{location.href='ui/html/index.html'})</script>") },
  { path: "ui/html/index.html", data: Buffer.from("<html>stock " + "x".repeat(5000) + "</html>") },
  { path: "lib/kwm.js", data: Buffer.from("const KWM = {}") },
];
const fakeStock = () =>
  writeZip([
    { name: "appUI/", data: Buffer.alloc(0) },
    { name: "appUI/index.html", data: fakeStockFiles()[0]!.data },
    { name: "appUI/ui/", data: Buffer.alloc(0) },
    { name: "appUI/ui/html/", data: Buffer.alloc(0) },
    { name: "appUI/ui/html/index.html", data: fakeStockFiles()[1]!.data },
    { name: "appUI/lib/kwm.js", data: fakeStockFiles()[2]!.data, store: true },
  ]);
const fakeApp = () => [
  { path: "ng/index.html", data: Buffer.from('<link rel="stylesheet" href="styles-1.css"><script defer src="app-1.js"></script>') },
  { path: "ng/app-1.js", data: Buffer.from("(()=>{window.__ca79ngBooted=true})();") },
  { path: "ng/styles-1.css", data: Buffer.from("body{}") },
];

test("USB container: encrypt only, with the user's key file (random test key)", () => {
  const hex = (n: number) => randomBytes(n).toString("hex");
  const json = JSON.stringify({ versionKey: hex(32), versionIv: hex(16), c1: "0123456789".repeat(4), key2: hex(32), iv2: hex(16), key3: hex(32), iv3: hex(16) });
  const k = parseUsbKey(json);
  const { zip } = assemble({ version: "0.2.0", app: fakeApp(), stock: null, built: "test" });
  const enc = encryptUsbBundle(zip, "0.2.0", k);
  const aes = (d: Buffer, key: Buffer, iv: Buffer) => {
    const c = createCipheriv("aes-256-cbc", key, iv);
    return Buffer.concat([c.update(d), c.final()]);
  };
  const md5 = createHash("md5").update(zip).digest("hex");
  assert.equal(enc.md5, md5);
  assert.ok(enc.appUiXxx.equals(aes(zip, k.key3, k.iv3)));
  const sc2 = aes(Buffer.from(md5), k.key2, k.iv2).toString("base64");
  assert.ok(enc.versionXxx.equals(aes(Buffer.from(`0.2.0\n${k.c1}\n${sc2}`), k.versionKey, k.versionIv)), "3 lines, no trailing newline");
  // the shell rejects other version strings
  assert.throws(() => encryptUsbBundle(zip, "1.3", k));
  assert.throws(() => encryptUsbBundle(zip, "v1.0.0", k));
  // malformed key files are refused
  assert.throws(() => parseUsbKey(JSON.stringify({ ...JSON.parse(json), key3: "00" })), /key3/);
  assert.throws(() => parseUsbKey(JSON.stringify({ ...JSON.parse(json), c1: "12ab" })), /c1/);
});

test("zip writer/reader: deflate, stored, dirs, raw copy keeps bytes and CRC", () => {
  const src = readZip(fakeStock());
  assert.equal(src.read("appUI/lib/kwm.js").toString(), "const KWM = {}");
  assert.equal(src.get("appUI/lib/kwm.js")!.method, 0);
  assert.equal(src.get("appUI/ui/html/index.html")!.method, 8);
  const copy = readZip(writeZip(src.entries.map((e) => ({ name: e.name, copy: e, from: src }))));
  for (const e of src.entries) {
    const c = copy.get(e.name)!;
    assert.equal(c.crc, e.crc);
    assert.ok(copy.raw(c).equals(src.raw(e)), e.name);
  }
  assert.throws(() => writeZip([{ name: "a", data: Buffer.from("1") }, { name: "a", data: Buffer.from("2") }]), /duplicate/);
});


test("assemble: our files only by default; verify catches what would break on the panel", () => {
  const { zip, staged, info } = assemble({ version: "0.2.0", app: fakeApp(), stock: null, built: "test" });
  const z = readZip(zip);
  assert.ok(z.entries.every((e) => e.name.startsWith("appUI/")));
  assert.equal(info.kawai, null);
  assert.equal(z.get("appUI/ui/html/index.html"), undefined, "nothing from the stock UI");
  assert.ok(z.get("appUI/ng/") && z.get("appUI/ca79ng-bundle.json"));
  const r = verifyZip(zip, { stage: staged });
  assert.deepEqual(r.problems, []);
  assert.equal(r.checked.stage, staged.length);
  assert.match(r.warnings.join("\n"), /no --stock-ui/);

  const broken = writeZip([
    { name: "appUI/html/loading.html", data: Buffer.from("x") },
    { name: "appUI/ng/index.html", data: Buffer.from('<script type="module" src="app.js"></script>') },
    { name: "x/outside.txt", data: Buffer.from("x") },
  ]);
  const p = verifyZip(broken, { stage: fakeApp() }).problems.join("\n");
  for (const want of [/loading\.html/, /ES module/, /missing app\.js/, /outside appUI/, /missing entry point appUI\/index\.html/, /stage file missing/]) assert.match(p, want);
});

test("--stock-ui: a local stock folder is included; the chooser replaces only its entry page", () => {
  const dir = mkdtempSync(join(tmpdir(), "stockui-"));
  try {
    for (const f of fakeStockFiles()) {
      mkdirSync(dirname(join(dir, "appUI", f.path)), { recursive: true });
      writeFileSync(join(dir, "appUI", f.path), f.data);
    }
    const files = readStockUi(dir);
    assert.deepEqual(files.map((f) => f.path).sort(), ["index.html", "lib/kwm.js", "ui/html/index.html"]);
    assert.deepEqual(readStockUi(join(dir, "appUI")).map((f) => f.path).sort(), files.map((f) => f.path).sort(), "the appUI folder itself works too");
    assert.throws(() => readStockUi(join(dir, "appUI", "lib")), /ui\/html\/index\.html/);
    const { zip, staged } = assemble({ version: "0.2.0", app: fakeApp(), stock: { files, version: "local" }, built: "test" });
    const z = readZip(zip);
    assert.match(z.read("appUI/index.html").toString(), /OpenGrand UI/);
    assert.match(z.read("appUI/index.html").toString(), /"kawai":"local"/);
    assert.equal(z.read("appUI/ui/html/index.html").toString(), fakeStockFiles()[1]!.data.toString());
    const r = verifyZip(zip, { stage: staged, stock: files });
    assert.deepEqual(r.problems, []);
    assert.deepEqual(r.warnings.filter((w) => /stock/.test(w)), []);
    assert.equal(r.checked.stock, files.length - 1);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test("chooser page: no countdown, default Kawai, failure reason, recovery installs via the bridge", async () => {
  const html = chooserHtml({ version: "0.2.0", built: "t", kawai: "1.3.10" }).toString();
  const calls: string[] = [];
  const ls = new Map<string, string>(); // jsdom has no localStorage on file:// (the shell's WebView has)
  const dom = new JSDOM(html, {
    url: "file:///data/data/x/files/appUI/index.html?reason=noconnect",
    runScripts: "dangerously",
    beforeParse(w) {
      Object.defineProperty(w, "localStorage", { value: { getItem: (k: string) => ls.get(k) ?? null, setItem: (k: string, v: string) => void ls.set(k, String(v)) } });
      (w as unknown as Record<string, unknown>).kwmcore = {
        postMessage(url: string) {
          const cmd = url.substring(10, url.indexOf("?") - 1);
          calls.push(cmd);
          const k = (w as unknown as { KWM: Record<string, (a: string, b: string) => void> }).KWM;
          setTimeout(() => {
            if (cmd === "getVersions") (w as unknown as { KWM: Record<string, unknown> }).KWM.latestUiVersion = "1.3.10";
            k.resolvePromise(cmd, cmd === "getEmbeddedMode" ? "true" : "{}");
          }, 1);
        },
      };
    },
  });
  const d = dom.window.document;
  try {
    assert.match(d.getElementById("why")!.textContent!, /noconnect/);
    assert.equal(d.getElementById("count")!.textContent, "", "no countdown (owner 2026-11-15)");
    assert.equal(d.getElementById("kawai")!.className, "def");
    assert.match(d.getElementById("info")!.textContent!, /OpenGrand 0\.2\.0/);
    // shell's onPageFinished → deviceready
    (dom.window as unknown as { KWM: { dispatchEvent(id: string, a: unknown): void } }).KWM.dispatchEvent("deviceready", {});
    await new Promise((r) => setTimeout(r, 20));
    // the backlight first, at load (the UI that failed may have left it off)
    assert.deepEqual(calls, ["setLcdBacklight", "getEmbeddedMode", "getVersions", "getLcdInfo"]);
    assert.match(d.getElementById("ver")!.textContent!, /USB: 1\.3\.10/);
    // screen readout for the panel simulator
    assert.match(d.getElementById("screen")!.textContent!, /CSS px · DPR /);
    // OpenGrand UI language, kept in the app's settings
    (d.querySelector('#langrow button[data-lang="ko"]') as HTMLButtonElement).click();
    assert.equal(JSON.parse(ls.get("ca79ng.settings")!).lang, "ko");
    assert.equal(d.querySelector("#langrow button.on")!.getAttribute("data-lang"), "ko");
    // recovery: two taps, then downloadUiData → updateUiData
    const btn = d.getElementById("usbui") as HTMLButtonElement;
    btn.click();

    assert.deepEqual(calls.slice(4), [], "first tap only arms");
    btn.click();
    await new Promise((r) => setTimeout(r, 20));
    assert.deepEqual(calls.slice(4), ["downloadUiData", "updateUiData"]);
    assert.match(d.getElementById("msg")!.textContent!, /완료: UI 1\.3\.10/);
  } finally {
    dom.window.close();
  }
});

test("chooser page: default OpenGrand goes straight in; ?stay=1 and a failure reason stay and wait", async () => {
  const html = chooserHtml({ version: "0.2.0", built: "t", kawai: "1.3.10" }).toString();
  const open = (query: string) => {
    const navs: string[] = [];
    const vc = new VirtualConsole();
    vc.on("jsdomError", (e: Error) => { if (/navigation/i.test(e.message)) navs.push(e.message); });
    const ls = new Map<string, string>([["ca79ng.uiDefault", "ng"]]);
    const dom = new JSDOM(html, {
      url: "file:///data/data/x/files/appUI/index.html" + query, runScripts: "dangerously", virtualConsole: vc,
      beforeParse(w) {
        Object.defineProperty(w, "localStorage", { value: { getItem: (k: string) => ls.get(k) ?? null, setItem: (k: string, v: string) => void ls.set(k, String(v)) } });
      },
    });
    return { dom, navs, count: dom.window.document.getElementById("count")!.textContent ?? "" };
  };
  const a = open("");
  try {
    assert.equal(a.navs.length, 1, "navigated to the OpenGrand UI at once");
    assert.equal(a.count, "", "no countdown shown");
  } finally { a.dom.window.close(); }
  const b = open("?stay=1");
  try { assert.equal(b.navs.length, 0, "Settings → UI chooser stays on the page"); } finally { b.dom.window.close(); }
  const c = open("?reason=timeout");
  try {
    assert.equal(c.navs.length, 0);
    assert.equal(c.count, "", "after a failure: no countdown, the page waits for a tap");
  } finally { c.dom.window.close(); }
});

test("app build for the shell: classic script, no ES module, hashed names resolve", async () => {
  const files = await buildApp("0.2.0");
  const html = files.find((f) => f.path === "ng/index.html")!.data.toString();
  assert.doesNotMatch(html, /type="module"/);
  const markup = html.slice(0, html.lastIndexOf("<script>")); // the inline app's own strings are not page links
  for (const m of markup.matchAll(/(?:src|href)="([^"]+)"/g)) assert.ok(files.some((f) => f.path === "ng/" + m[1]), m[1]);
  const js = files.find((f) => f.path.endsWith(".js"))!.data.toString();
  assert.doesNotMatch(js, /^\s*(import|export)\s/m);
  assert.doesNotMatch(js, /\?\?|\?\.[a-zA-Z_$]/, "chrome60 target: no ?? / ?. left");
  assert.match(js, /__ca79ngBooted/);
});


test("size report of the bundle zip", () => {
  const { zip } = assemble({ version: "0.2.0", app: fakeApp(), stock: { files: fakeStockFiles(), version: "local" } });
  const r = sizeReport(zip, { version: "0.2.0" }) as { version: string; zip: { entries: number }; groups: Record<string, { files: number }> };
  assert.equal(r.version, "0.2.0");
  assert.equal(r.groups["appUI/ng/"].files, 3);
  assert.equal(r.groups["appUI/ui/"].files, 1);
  assert.ok(r.zip.entries > 8);
});

// Field 2026-11-01: the panel WebView (Android 6.0.1 rk312x, Chrome 71) died at start: nav.replaceChildren
// (Chrome 86) at boot, reported only as "Script error." because the app came from another file:// URL.
// The app now runs inline after a boot guard that supplies newer built-ins and reports real errors.
const CHROME71_MISSING: [RegExp, string | null][] = [
  // [use in the bundle, polyfill marker in the guard (null = must not appear at all)]
  [/\.replaceChildren\(/, "C.prototype.replaceChildren ="],
  [/\.arrayBuffer\(\)/, "Blob.prototype.arrayBuffer ="],
  [/\bglobalThis\b/, "window.globalThis = window"],
  [/Object\.fromEntries\(/, null], [/Promise\.allSettled\(/, null], [/Promise\.any\(/, null],
  [/\.matchAll\(/, null], [/\.replaceAll\(/, null], [/Object\.hasOwn\(/, null], [/\.at\(-?\d/, null],
  [/structuredClone\(/, null], [/\.trimStart\(|\.trimEnd\(/, null], [/AbortSignal\.timeout/, null],
  [/crypto\.randomUUID/, null], [/\bWeakRef\b|FinalizationRegistry/, null], [/\.toggleAttribute\(/, null],
  [/\.requestSubmit\(/, null], [/\.flat\(|\.flatMap\(/, null],
];

test("panel bundle runs on Chrome 71: newer built-ins are polyfilled before loader and app, errors carry detail", async () => {
  const files = await buildApp("0.0.0-test");
  const html = files.find((f) => f.path === "ng/index.html")!.data.toString();
  const js = files.find((f) => f.path.endsWith(".js"))!.data.toString();
  const appAt = html.indexOf('<script type="text/plain" id="ca79ng-app-js">');
  assert.ok(appAt > 0 && html.slice(appAt).includes(js.slice(0, 200)), "app rides inline (file:// scripts only report 'Script error.')");
  const loaderAt = html.indexOf("CA79UI");
  assert.ok(loaderAt > 0);
  for (const [use, marker] of CHROME71_MISSING) {
    if (!use.test(js)) continue;
    assert.ok(marker, `bundle uses ${use} (not in Chrome 71) and nothing supplies it`);
    const at = html.indexOf(marker!);
    assert.ok(at > 0 && at < loaderAt && at < appAt, `${marker} must run before the loader and the app`);
  }
  const replaced: string[] = [];
  // jsdom's location cannot be stubbed: route navigation to a recorder; keep the app text out
  const page = html.slice(0, appAt).replace(/location\.replace\(/g, "window.__go(") + "</body></html>";
  const dom = new JSDOM(page, {
    runScripts: "dangerously",
    url: "file:///appUI/ng/index.html",
    beforeParse(w) {
      (w as unknown as { __go(u: string): void }).__go = (u) => void replaced.push(u);
    },
  });
  const w = dom.window as unknown as { document: Document; Element: typeof Element; __ca79ngFail(r: string, d?: string): void };
  const d = w.document.createElement("div");
  delete (w.Element.prototype as { replaceChildren?: unknown }).replaceChildren; // jsdom has it: test ours
  dom.window.eval((html.match(/\[window\.Element, window\.Document[\s\S]*?\}\);\n/) ?? [""])[0]);
  d.replaceChildren(w.document.createElement("b"), "t");
  assert.equal(d.innerHTML, "<b></b>t");
  w.__ca79ngFail("error", "TypeError: nav.replaceChildren is not a function @index.html:9:1");
  assert.equal(replaced.length, 1);
  const q = new URL(replaced[0], "file:///appUI/ng/").searchParams;
  assert.equal(q.get("reason"), "error");
  assert.match(q.get("detail")!, /replaceChildren/);
  assert.ok(q.get("ua"));
  dom.window.close();
});
