// Panel UI bundle core: our web app staged
// for the ORIGINAL panel shell, and the signed remote UI packages (.c79u) the UI loader accepts over BLE.
//
//   node scripts/bundlecore.ts build  [--out dist-kwm/ng] [--version X.Y.Z] [--seq N] [--stock-ui <dir>] [--stock-version V] [--kwm-key <file>]
//        Stages our files (<out>/stage/appUI/), writes the plain bundle zip (<out>/appUI.zip) and a size report.
//        USB first install (advanced): only with a user-supplied key file (CA79_KWM_KEY=<path> or --kwm-key),
//        then also <out>/usb/{version.xxx,appUI.xxx} — see scripts/usbbundle.ts. This repo ships no key,
//        no key derivation, no decryptor and no Kawai files.
//        --stock-ui <dir>: YOUR OWN locally extracted stock UI (a folder holding appUI/…, or the appUI folder
//        itself), included as the chooser's "Kawai" fallback. Without it the bundle carries our files only.
//   node scripts/bundlecore.ts key                       # panel UI signing key: show (create if missing)
//   node scripts/bundlecore.ts pack [--version X.Y.Z] [--seq N] [--out dist-kwm/remote] [--probe]   # remote UI package .c79u
//        --probe: a small test page instead of the app (~2 KB + the screen reporter, ~11 KB packed) (first field transfer): shows the panel's
//        WebView, viewport, loader state; buttons measure the localStorage quota (CA79UI.probeQuota) / roll back
//        (two taps) / restart. Never confirms itself.
//   node scripts/bundlecore.ts inspect <file.c79u>
//
// Zip layout the shell expects: every entry under "appUI/", unzipped into files/; the WebView opens
// file://…/files/appUI/html/loading.html if present, else appUI/index.html. Our layout:
//   appUI/index.html            chooser (OpenGrand / Kawai, recovery) + UI loader
//   appUI/ng/…                  OpenGrand app (classic IIFE script — no ES modules on file://);
//                               ng/index.html carries the UI loader (panelui/uiloader.js), which
//                               receives remote UI packages and boots the newest verified one
//   appUI/ca79ng-bundle.json    build info
//   appUI/ui, lib, developper   only with --stock-ui: the user's own stock UI copy (its appUI/index.html,
//                               a redirect, is the only stock file replaced)
import { build } from "esbuild";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { loaderSource, loadKey, makePackage, panelCheck, screenSource, seqNow, uiConfig, type UiConfig } from "./panelpkg.ts";
import { encryptUsbBundle, loadUsbKey, usbKeyPath, VERSION_RE } from "./usbbundle.ts";
import { readZip, writeZip, type NewEntry } from "./zip.ts";
import { decodeHeader, estimateTransfer, keyIdHex } from "../src/protocol/panelui.ts";

const APP = resolve(dirname(fileURLToPath(import.meta.url)), "..");
/** The stock UI's [UPDATE] button compares the stick's version against this one: never build with it. */
export const STOCK_UI_VERSION = "1.3.10";

const sha256 = (b: Buffer) => createHash("sha256").update(b).digest("hex");

// ---- our app, built for the original shell ----------------------------------------

export interface StagedFile {
  path: string; // relative to appUI/
  data: Buffer;
}

// The UI loader is inlined at the UILOADER marker (an empty script element) of panelui/chooser.html and ng.html,
// the screen reporter (panelui/panelscreen.js) at the PANELSCREEN marker right after it.
export function inlineLoader(html: string, cfg: UiConfig | null, where: string): string {
  if (!html.includes("/*UILOADER*/")) throw new Error(`${where}: UILOADER marker missing`);
  if (!html.includes("/*PANELSCREEN*/")) throw new Error(`${where}: PANELSCREEN marker missing`);
  const src = loaderSource(cfg);
  if (/<\/script/i.test(src)) throw new Error("uiloader.js must not contain a closing script tag");
  const scr = screenSource();
  return html.replace("/*UILOADER*/", () => src).replace("/*PANELSCREEN*/", () => scr);
}

/** The app js as it rides in the bundle and in remote packages: the screen reporter first (a remote package then
 *  brings it to a panel whose USB bundle predates it — the field loader 0.2.0 swallows ops 0x35-0x3F itself). */
export function withScreen(js: string): string {
  return `${screenSource()}\n${js}`;
}

/** esbuild → classic IIFE (file:// pages cannot load ES modules in the WebView), chrome60
 *  (the stock UI itself needs object spread + async/await, i.e. Chrome ≥ 60 on the panel). */
export async function buildApp(version: string, ui: UiConfig | null = null): Promise<StagedFile[]> {
  const res = await build({
    absWorkingDir: APP,
    entryPoints: ["src/main.ts"],
    bundle: true,
    format: "iife",
    target: ["es2017", "chrome60"],
    minify: true,
    write: false,
    outdir: "out",
    entryNames: "app-[hash]",
    define: { __APP_VERSION__: JSON.stringify(version) },
    logLevel: "warning",
  });
  const js = res.outputFiles.find((f) => f.path.endsWith(".js"));
  if (!js) throw new Error("esbuild produced no js");
  const jsName = js.path.split("/").pop()!;
  const css = readFileSync(join(APP, "styles.css"));
  const cssName = `styles-${sha256(css).slice(0, 8).toUpperCase()}.css`;
  let html = readFileSync(join(APP, "panelui/ng.html"), "utf8");
  for (const [from, to] of [['"styles.css"', `"${cssName}"`], ['"app.js"', `"${jsName}"`]]) {
    if (!html.includes(from)) throw new Error(`panelui/ng.html: ${from} not found`);
    html = html.split(from).join(to);
  }
  html = inlineLoader(html, ui, "panelui/ng.html");
  // The bundled app also rides INLINE (as text the loader injects): a script loaded from another file://
  // URL is a different origin to Chrome, so its errors reach window.onerror as a bare "Script error."
  // (field 2026-11-01). The separate file stays in the bundle (desktop debugging, no-loader fallback).
  if (!html.includes("</body>")) throw new Error("panelui/ng.html: </body> not found");
  const jsText = withScreen(Buffer.from(js.contents).toString("utf8"));
  const inline = jsText.replace(/<\/script/gi, "<\\/script");
  html = html.replace("</body>", () => `<script type="text/plain" id="ca79ng-app-js">${inline}</script>\n</body>`);
  return [
    { path: "ng/index.html", data: Buffer.from(html) },
    { path: `ng/${jsName}`, data: Buffer.from(jsText) },
    { path: `ng/${cssName}`, data: css },
  ];
}

export function chooserHtml(info: Record<string, unknown>, ui: UiConfig | null = null): Buffer {
  const src = inlineLoader(readFileSync(join(APP, "panelui/chooser.html"), "utf8"), ui, "panelui/chooser.html");
  const re = /\/\*BUNDLE_INFO\*\/[\s\S]*?\/\*END\*\//;
  if (!re.test(src)) throw new Error("panelui/chooser.html: BUNDLE_INFO marker missing");
  return Buffer.from(src.replace(re, () => `/*BUNDLE_INFO*/${JSON.stringify(info).replace(/</g, "\\u003c")}/*END*/`));
}

// ---- assemble ------------------------------------------------------------------------

export interface AssembleOptions {
  version: string;
  app: StagedFile[];
  /** the user's own locally extracted stock UI (paths relative to appUI/), or null: not included */
  stock: { files: StagedFile[]; version: string } | null;
  built?: string;
  /** loader config (signing key, bundled seq); null = no remote updates (tests) */
  ui?: UiConfig | null;
}

export function assemble(o: AssembleOptions): { zip: Buffer; staged: StagedFile[]; info: Record<string, unknown> } {
  const info = { version: o.version, built: o.built ?? new Date().toISOString().slice(0, 16).replace("T", " "), kawai: o.stock?.version ?? null, seq: o.ui?.bundled.seq ?? 0, keyId: o.ui?.keyId ?? "" };
  const staged: StagedFile[] = [
    { path: "index.html", data: chooserHtml(info, o.ui ?? null) },
    ...o.app,
    { path: "ca79ng-bundle.json", data: Buffer.from(JSON.stringify({ ...info, files: o.app.map((f) => ({ path: f.path, sha256: sha256(f.data) })) }, null, 2) + "\n") },
  ];
  const ours = new Set(staged.map((f) => f.path));
  const items: NewEntry[] = [];
  const dirs = new Set<string>();
  const addDir = (d: string) => {
    if (dirs.has(d)) return;
    dirs.add(d);
    items.push({ name: d, data: Buffer.alloc(0) });
  };
  addDir("appUI/");
  // the user's own stock copy first (only its appUI/index.html, a redirect, is replaced), then ours
  for (const f of [...(o.stock?.files ?? []).filter((s) => !ours.has(s.path)), ...staged]) {
    const parts = ("appUI/" + f.path).split("/");
    for (let i = 2; i < parts.length; i++) addDir(parts.slice(0, i).join("/") + "/");
    items.push({ name: "appUI/" + f.path, data: f.data });
  }
  return { zip: writeZip(items), staged, info };
}


// ---- the user's own stock UI copy (optional) -------------------------------------------

/** Files of a locally extracted stock UI: `<dir>/appUI/…` or `<dir>` itself being the appUI folder. */
export function readStockUi(dir: string): StagedFile[] {
  const root = existsSync(join(dir, "appUI")) && statSync(join(dir, "appUI")).isDirectory() ? join(dir, "appUI") : dir;
  if (!existsSync(root)) throw new Error(`--stock-ui ${dir}: not found`);
  const files = readTree(root);
  if (!files.some((f) => f.path === "ui/html/index.html")) throw new Error(`--stock-ui ${dir}: no ui/html/index.html (expected the stock appUI folder)`);
  return files;
}

// ---- verify (the plain zip, before any container) ---------------------------------------

export interface VerifyResult {
  entries: number;
  bytes: number;
  problems: string[];
  warnings: string[];
  checked: { stage: number; stock: number };
}

/** Check entry points, paths, CRCs, and diff against the staged files (and the stock copy, if included). */
export function verifyZip(zip: Buffer, opt: { stage?: StagedFile[]; stock?: StagedFile[] | null } = {}): VerifyResult {
  const problems: string[] = [];
  const z = readZip(zip);
  let bytes = 0;
  for (const e of z.entries) {
    if (!e.name.startsWith("appUI/")) problems.push(`entry outside appUI/: ${e.name}`);
    if (e.name.includes("..") || e.name.startsWith("/")) problems.push(`unsafe path: ${e.name}`);
    if (!e.name.endsWith("/")) {
      try {
        bytes += z.read(e).length; // CRC check
      } catch (err) {
        problems.push((err as Error).message);
      }
    }
  }
  if (z.get("appUI/html/loading.html")) problems.push("appUI/html/loading.html present: the shell would open it instead of appUI/index.html");
  const entry = z.get("appUI/index.html");
  if (!entry) problems.push("missing entry point appUI/index.html");
  const ngHtml = z.get("appUI/ng/index.html");
  const warnings: string[] = [];
  if (ngHtml) {
    const full = z.read(ngHtml).toString("utf8");
    const app = full.indexOf('<script type="text/plain" id="ca79ng-app-js">');
    const html = app > 0 ? full.slice(0, app) : full; // the inline app's own strings are not page references
    for (const m of new Set([...html.matchAll(/"([\w./-]+\.(?:js|css))"/g)].map((x) => x[1]!))) {
      if (!z.get("appUI/ng/" + m)) problems.push(`ng/index.html references missing ${m}`);
    }
    if (/type="module"/.test(html)) problems.push("ng/index.html uses an ES module (blocked on file://)");
    const cfg = /\/\*UI_CONFIG\*\/(\{.*?\})\/\*END\*\//.exec(html);
    if (!cfg) warnings.push("ng/index.html has no UI loader: no remote UI updates");
    else if (!(JSON.parse(cfg[1]!) as UiConfig).pub) warnings.push("UI loader without a signing key: the panel refuses every remote UI package");
  }
  if (entry && !z.get("appUI/ui/html/index.html") && /ui\/html\/index\.html/.test(z.read(entry).toString("utf8"))) {
    warnings.push("stock UI not in the bundle (no --stock-ui): the chooser's Kawai button has no target");
  }
  const checked = { stage: 0, stock: 0 };
  for (const f of opt.stage ?? []) {
    const e = z.get("appUI/" + f.path);
    if (!e) problems.push(`stage file missing in bundle: ${f.path}`);
    else if (!z.read(e).equals(f.data)) problems.push(`stage file differs: ${f.path}`);
    else checked.stage++;
  }
  for (const f of opt.stock ?? []) {
    if (f.path === "index.html" && z.get("appUI/ng/index.html")) continue; // replaced by the chooser
    const e = z.get("appUI/" + f.path);
    if (!e) problems.push(`stock file missing: ${f.path}`);
    else if (!z.read(e).equals(f.data)) problems.push(`stock file differs: ${f.path}`);
    else checked.stock++;
  }
  return { entries: z.entries.length, bytes, problems, warnings, checked };
}

// ---- size report ----------------------------------------------------------------------

export function sizeReport(zip: Buffer, extra: Record<string, unknown> = {}): Record<string, unknown> {
  const z = readZip(zip);
  const groups = new Map<string, { files: number; size: number; comp: number }>();
  for (const e of z.entries) {
    if (e.name.endsWith("/")) continue;
    const top = e.name.split("/").slice(0, 2).join("/");
    const key = top.includes(".") ? "appUI/(root files)" : top + "/";
    const g = groups.get(key) ?? { files: 0, size: 0, comp: 0 };
    g.files++;
    g.size += e.size;
    g.comp += e.compSize;
    groups.set(key, g);
  }
  return {
    ...extra,
    zip: { bytes: zip.length, sha256: sha256(zip), entries: z.entries.length, unpacked: [...groups.values()].reduce((a, g) => a + g.size, 0) },
    groups: Object.fromEntries([...groups.entries()].sort()),
  };
}

// ---- CLI -------------------------------------------------------------------------------

function args(argv: string[]): { pos: string[]; opt: Record<string, string | true> } {
  const pos: string[] = [];
  const opt: Record<string, string | true> = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith("--")) {
      const k = a.slice(2);
      const v = argv[i + 1];
      if (v !== undefined && !v.startsWith("--") && !["probe"].includes(k)) {
        opt[k] = v;
        i++;
      } else opt[k] = true;
    } else pos.push(a);
  }
  return { pos, opt };
}

function readTree(dir: string): StagedFile[] {
  const out: StagedFile[] = [];
  const walk = (d: string) => {
    for (const n of readdirSync(d).sort()) {
      const p = join(d, n);
      if (statSync(p).isDirectory()) walk(p);
      else out.push({ path: relative(dir, p).split("\\").join("/"), data: readFileSync(p) });
    }
  };
  if (existsSync(dir)) walk(dir);
  return out;
}

function printVerify(r: VerifyResult): boolean {
  console.log(`verify: ${r.entries} entries · ${r.bytes} B unpacked · stage ${r.checked.stage} same · stock ${r.checked.stock} same`);
  for (const w of r.warnings) console.log("  WARNING " + w);
  for (const p of r.problems) console.log("  PROBLEM " + p);
  return r.problems.length === 0;
}

/** Header, the panel's own verdict (uiloader.js in a VM) and the BLE transfer estimate. */
function inspect(file: Uint8Array, key: ReturnType<typeof loadKey>): boolean {
  const h = decodeHeader(file);
  if (!h) {
    console.log("  PROBLEM not a .c79u package");
    return false;
  }
  const chk = panelCheck(file, uiConfig(key, { version: "", seq: 0 }));
  console.log(`  header: version ${h.version}, seq ${h.seq}, key ${keyIdHex(h.keyId)}, payload ${h.length} B (deflate) → ${h.unpacked} B`);
  console.log(`  panel loader check with key ${key.keyIdHex}: ${chk.rc === 0 ? "accepted" : "PROBLEM rc " + chk.rc}`);
  const fmt = (s: number) => `${s.toFixed(0)} s`;
  for (const [label, o] of [
    ["panel link 31.25 kbaud", { panelBytesPerSec: 3125 }],
    ["panel link 115.2 kbaud", { panelBytesPerSec: 11520 }],
  ] as const) {
    const e = estimateTransfer(file.length, o);
    console.log(`  BLE estimate (MTU 23, 2500 B/s paced, ${label}): ${e.frames} frames / ${e.frameBytes} B → ${e.blePackets} packets / ${e.blePacketBytes} B; window 2 ≈ ${fmt(e.windowedS)}, stop-and-wait ≈ ${fmt(e.stopAndWaitS)}`);
  }
  return chk.rc === 0;
}

async function main(argv: string[]): Promise<number> {
  const { pos, opt } = args(argv);
  const cmd = pos[0];
  const str = (k: string, d: string) => (typeof opt[k] === "string" ? (opt[k] as string) : d);
  if (cmd === "build") {
    const pkg = JSON.parse(readFileSync(join(APP, "package.json"), "utf8"));
    const version = str("version", pkg.version);
    if (!VERSION_RE.test(version)) throw new Error(`--version ${version}: must be X.Y.Z`);
    if (version === STOCK_UI_VERSION) throw new Error(`--version ${STOCK_UI_VERSION} would hide the stock UI's [UPDATE] button (it compares against '${STOCK_UI_VERSION}')`);
    const out = resolve(APP, str("out", "dist-kwm/ng"));
    const stockDir = typeof opt["stock-ui"] === "string" ? resolve(opt["stock-ui"]) : null;
    const stock = stockDir ? { files: readStockUi(stockDir), version: str("stock-version", "local") } : null;
    const keyPath = usbKeyPath(typeof opt["kwm-key"] === "string" ? opt["kwm-key"] : undefined);
    const usbKey = keyPath ? loadUsbKey(keyPath) : null; // fail early on a bad key file
    // the bundle's trust anchor for remote UI packages (created on first use, git-ignored)
    const key = loadKey(undefined, { create: true });
    if (key.created) console.log(`NEW panel UI signing key ${key.keyIdHex} at ${key.path} — keep a copy outside this machine`);
    const seq = Number(str("seq", String(seqNow())));
    const ui = uiConfig(key, { version, seq });
    const app = await buildApp(version, ui);
    const { zip, staged } = assemble({ version, app, stock, ui });
    console.log(`UI loader: signing key ${key.keyIdHex}, bundled seq ${seq} (remote packages must be newer)`);
    mkdirSync(out, { recursive: true });
    const stageDir = join(out, "stage", "appUI");
    rmSync(join(out, "stage"), { recursive: true, force: true });
    for (const f of staged) {
      mkdirSync(dirname(join(stageDir, f.path)), { recursive: true });
      writeFileSync(join(stageDir, f.path), f.data);
    }
    writeFileSync(join(out, "appUI.zip"), zip);
    const usb = join(out, "usb");
    rmSync(usb, { recursive: true, force: true });
    let md5: string | null = null;
    if (usbKey) {
      const enc = encryptUsbBundle(zip, version, usbKey);
      mkdirSync(usb, { recursive: true });
      writeFileSync(join(usb, "version.xxx"), enc.versionXxx);
      writeFileSync(join(usb, "appUI.xxx"), enc.appUiXxx);
      md5 = enc.md5;
      console.log(`USB container ${usb} (key file ${keyPath}): copy version.xxx + appUI.xxx to the stick root`);
    } else {
      console.log("no CA79_KWM_KEY / --kwm-key: USB container not written (first install is advanced — see scripts/usbbundle.ts)");
    }
    console.log(`built ${out}: version ${version}, stock UI ${stock ? stock.version + " included (from " + stockDir + ")" : "NOT included"}`);
    const ok = printVerify(verifyZip(zip, { stage: staged, stock: stock?.files ?? null }));
    const rep = sizeReport(zip, { version, md5, usb: usbKey !== null });
    writeFileSync(join(out, "report.json"), JSON.stringify(rep, null, 2) + "\n");
    console.log(JSON.stringify(rep, null, 2));
    return ok ? 0 : 1;
  }
  if (cmd === "key") {
    const key = loadKey(undefined, { create: true });
    console.log(`${key.created ? "created" : "found"} ${key.path}\n  key id ${key.keyIdHex}\n  public ${Buffer.from(key.pub).toString("hex")}`);
    if (key.created) console.log("  keep a copy outside this machine; a bundle on the panel only accepts packages signed with this key");
    return 0;
  }
  if (cmd === "pack") {
    const pkgJson = JSON.parse(readFileSync(join(APP, "package.json"), "utf8"));
    const version = str("version", pkgJson.version);
    if (version.length > 16) throw new Error(`--version ${version}: at most 16 characters`);
    const seq = Number(str("seq", String(seqNow())));
    const key = loadKey();
    let js: string;
    let css: string;
    if (opt.probe === true) ({ js, css } = probeFiles(version));
    else {
      const app = await buildApp(version);
      js = app.find((f) => f.path.endsWith(".js"))!.data.toString("utf8");
      css = app.find((f) => f.path.endsWith(".css"))!.data.toString("utf8");
    }
    const built = new Date().toISOString().slice(0, 16).replace("T", " ");
    const file = makePackage({ js, css }, { version, seq, key, built });
    const out = resolve(APP, str("out", "dist-kwm/remote"));
    mkdirSync(out, { recursive: true });
    const name = join(out, `ca79ng-ui-${opt.probe === true ? "probe-" : ""}${version}-${seq}.c79u`);
    writeFileSync(name, file);
    console.log(`packed ${name}: version ${version}, seq ${seq}, key ${key.keyIdHex}, ${file.length} B (app js ${js.length} B + css ${css.length} B)`);
    return inspect(file, key) ? 0 : 1;
  }
  if (cmd === "inspect") {
    const file = readFileSync(resolve(pos[1] ?? ""));
    return inspect(file, loadKey()) ? 0 : 1;
  }
  console.error("usage: node scripts/bundlecore.ts build|key|pack|inspect … (see header)");
  return 2;
}

/**
 * `pack --probe`: a tiny remote UI (ES5, Chrome 71) for the first field transfer through the piano relay:
 * WebView, viewport, loader state.
 * - Backlight first: the page it replaces (our app) may have turned the backlight off (auto display off) while the
 *   phone was sending, and then the panel is black whatever this page draws (field 2026-11-03). It sets
 *   the backlight to the app's brightness (ca79ng.settings, else 80 %) at once and again on a touch.
 * - Draws as soon as there is a body (waits for DOMContentLoaded if there is none yet) and only then marks itself
 *   booted: a throw before that reaches the loader (window error → this slot bad → the previous version); the error
 *   text is put on the screen too.
 * - The localStorage quota (CA79UI.probeQuota, writes up to ~10 MB synchronously, freezes for seconds) is measured
 *   only on a tap; taps queued during that freeze are dropped. "Roll back" takes two taps.
 * - Remote roll back: the loader under this page answers the app's Check / Roll back (ops 0x30 / 0x34) itself; the
 *   page watches the loader state and reloads once its own slot is marked bad (Control 2) — the loader then boots the
 *   previous version. It never calls CA79UI.confirm(), so three more boots also fall back by themselves.
 */
export function probeFiles(version: string): { js: string; css: string } {
  const js = `(function () {
  var W = window, D = document, U = W.CA79UI, p = null, q = "not measured (tap Measure)", st = "", started = false;
  var quiet = 0; // taps dispatched before this time were made during a freeze (queued by the WebView): ignored
  function now() { return new Date().getTime(); }
  function host() {
    var k = W.kwmcore, w = W.webkit;
    if (k && typeof k === "object" && k.postMessage) return k;
    return w && w.messageHandlers && w.messageHandlers.kwmcore ? w.messageHandlers.kwmcore : null;
  }
  // the Kawai shell's setLcdBacklight {brightness 0..100} (Settings.System screen_brightness; 0 = backlight off)
  var lit = 0;
  function light() {
    var b = 8, h;
    try { var s = JSON.parse(W.localStorage.getItem("ca79ng.settings") || "{}"); if (s && s.brightness >= 1 && s.brightness <= 10) b = Math.round(s.brightness); } catch (e) { /* default */ }
    try { h = host(); if (h) { h.postMessage("kwmcore://setLcdBacklight/?" + JSON.stringify({ brightness: b * 10 })); lit = now(); } } catch (e) { /* no shell */ }
  }
  light();
  function wake() { if (now() - lit > 2000) light(); }
  if (W.addEventListener) { W.addEventListener("touchstart", wake, true); W.addEventListener("mousedown", wake, true); }
  // the screen reporter (prepended to this script): the phone reads what this page shows (ops 0x35 / 0x36)
  var S = W.CA79Screen;
  if (S) {
    S.install(W.KWM);
    S.provide(function () { return { pg: "probe", ver: "${version}", quota: q, lit: lit ? Math.round((now() - lit) / 1000) + "s ago" : "never" }; });
  }
  function state() { try { return U && U.state ? U.state() : null; } catch (e) { return null; } }
  function paint() {
    var s = state();
    try { st = s ? JSON.stringify(s) : "no loader"; } catch (e) { st = "?"; }
    p.textContent = "OpenGrand panel probe ${version}\\nlocalStorage quota (UTF-16 chars): " + q +
      "\\nUA: " + navigator.userAgent + "\\nviewport: " + W.innerWidth + "x" + W.innerHeight + " dpr " + W.devicePixelRatio +
      "\\nlang: " + navigator.language + "\\nloader: " + st +
      "\\nleave: app Roll back · Roll back here (2 taps) · power off/on 3x";
  }
  function btn(b, id, t, f) {
    var e = D.createElement("button");
    e.id = id; e.textContent = t;
    e.onclick = function () { if (now() >= quiet) f(e); };
    b.appendChild(e);
  }
  function draw() {
    var b = D.body || D.documentElement, armed = 0;
    p = D.createElement("pre");
    p.id = "probe";
    b.appendChild(p);
    paint();
    btn(b, "probe-measure", "Measure storage / 저장 한도 재기", function (e) {
      q = "measuring... (the panel freezes a few seconds)"; paint(); e.disabled = true;
      setTimeout(function () { // after that line has been painted
        var t = now();
        try { q = U && U.probeQuota ? String(U.probeQuota()) : "no loader"; } catch (x) { q = "error " + x; }
        q += " (" + (now() - t) + " ms)";
        quiet = now() + 500; paint(); e.disabled = false;
      }, 100);
    });
    btn(b, "probe-back", "Roll back / 되돌리기", function (e) {
      if (now() > armed) { armed = now() + 5000; e.textContent = "Tap again to roll back / 한 번 더 누르면 되돌립니다"; return; }
      if (U && U.control) U.control(2);
      W.location.reload();
    });
    btn(b, "probe-restart", "Restart UI / 다시 시작", function () { W.location.reload(); });
    // the app's Roll back (Control 2 through the relay) marks this slot bad: go to the previous version now
    setInterval(function () {
      var s = state(), e = s && s.slots && s.slots[s.running];
      if (e && e.state === "bad") { W.location.reload(); return; }
      paint();
    }, 1000);
  }
  function start() {
    if (started) return;
    started = true;
    try { draw(); } catch (x) {
      try { var b = D.body || D.documentElement, e = D.createElement("pre"); e.textContent = "probe error: " + x; b.appendChild(e); } catch (y) { /* nothing to show it on */ }
      throw x; // not booted yet: the loader marks this slot bad and boots the previous version
    }
    W.__ca79ngBooted = true;
  }
  if (D.body || D.readyState !== "loading") start();
  else D.addEventListener("DOMContentLoaded", start);
})();`;
  const css = "html,body{margin:0;background:#000;color:#fff;font:18px sans-serif}" +
    "#probe{white-space:pre-wrap;word-break:break-all;padding:16px;margin:0;font:16px monospace;color:#fff}" +
    "button{display:block;font-size:22px;min-height:64px;margin:12px 16px;padding:0 20px}";
  return { js: withScreen(js), css };
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2)).then(
    (c) => process.exit(c),
    (e) => {
      console.error(e instanceof Error ? e.message : e);
      process.exit(1);
    },
  );
}
