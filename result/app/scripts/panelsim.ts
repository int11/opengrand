// Front-end simulator: screenshots of every screen at the built-in panel's size (and a phone and a desktop),
// fed by the firmware simulator so the screens show real data. For automated checks and people who cannot look at the
// piano: `npm run panelsim` → PNGs under .panelsim/<profile>/ (git-ignored), then open them.
//
//   npm run panelsim                         all profiles (panel = Korean, phone, desktop)
//   npm run panelsim -- --only panel         one profile (panel | panel-en | phone | desktop)
//   npm run panelsim -- --out <dir>
//   npm run panelsim -- --no-audit           screenshots only (no layout checks, see audit() below)
//   npm run panelsim -- --cobound            the piano with two controllers bound (session core, op 0x1B 03):
//                                            the header's "Panel also connected" / "App also connected"
//   npm run panelsim -- --btsrc              the 「블루투스」 page with three BT audio sources (op 0x1D 04 layout 2 /
//                                            1D 09 answered here: TV selected and playing, PC refused, phone without a
//                                            sound link; AUTOCALL on)
//   npm run panelsim -- --updatemode         the piano in its USB firmware updater (E+A+B+C at power on): a model of
//                                            the BootProgram's panel side (src/protocol/updatemode.ts UpdaterModel)
//                                            instead of the firmware simulator. The panel page must show 「전원을 끄지
//                                            마세요」 and hand over to the Kawai update page within a few seconds, not
//                                            wait 30 s for noconnect
//
// panel: the OpenGrand bundle page (ng/index.html, built like `npm run panelui` but unsigned) opened from
//   file:// in headless Chromium at the panel's CSS viewport 480×800, devicePixelRatio 1
//   (the shell leaves useWideViewPort off, the photo of the
//   panel measures 480×800 CSS px), behind a fake KWMCore shell (window.kwmcore → this script) that forwards
//   sendMidiMessage to the firmware simulator's PANEL port and calls KWM.onMidiMessage with its answers,
//   like KWMCoreManager does. The chooser page (appUI/index.html) is shot too.
// phone / desktop: the web build (dist/) through scripts/dev-server.ts (?sim → ws /midi → simulator).
//
// The engine is current Chromium, not the panel's Chrome 71: what Chrome 71 lacks is kept out by
// tests/css71.test.ts (CSS) and tests/bundlecore.test.ts (JS built-ins), not by this script.
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { createServer } from "node:net";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { chromium, type Browser, type BrowserContext, type Locator, type Page } from "playwright-core";

import { buildApp, chooserHtml } from "./bundlecore.ts";
import { SimProcessLink, SIM_BIN } from "./sim-link.ts";
import { estimateScreenTransfer, PanelScreenClient } from "../src/protocol/panelscreen.ts";
import type { PuiPort } from "../src/protocol/panelui.ts";
import { fromHex, toHex } from "../src/protocol/sysex.ts";
import { UpdaterModel } from "../src/protocol/updatemode.ts";

const APP = resolve(dirname(fileURLToPath(import.meta.url)), "..");

/** The built-in panel and two reference sizes. */
export const PROFILES = {
  panel: { width: 480, height: 800, dpr: 1, lang: "ko", kind: "panel" },
  "panel-en": { width: 480, height: 800, dpr: 1, lang: "en", kind: "panel" },
  phone: { width: 390, height: 844, dpr: 3, lang: "ko", kind: "web" },
  desktop: { width: 1280, height: 800, dpr: 1, lang: "ko", kind: "web" },
} as const;
type ProfileId = keyof typeof PROFILES;

// UA of the panel WebView (field 2026-11-01, chooser page)
const PANEL_UA = "Mozilla/5.0 (Linux; Android 6.0.1; rk312x Build/MXC89K; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/71.0.3578.99 Safari/537.36";
const TAB_IDS = ["sound", "music", "metronome", "technician", "chords", "files", "bluetooth", "settings", "info"];

function args(argv: string[]): Record<string, string> {
  const o: Record<string, string> = {};
  for (let i = 0; i < argv.length; i++) if (argv[i].startsWith("--")) o[argv[i].slice(2)] = argv[i + 1] && !argv[i + 1].startsWith("--") ? argv[++i] : "1";
  return o;
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/**
 * `--cobound`: the piano as with the session-core payload ota_main and two controllers bound (the app on BLE
 * port 2 and the panel on port 3) - the firmware simulator has
 * no session core, so this script answers Identify (feat2 0x03) and op 0x1B 03 (table 0x0C, stock binding 4,
 * byte 45 = the asking port) itself; everything else goes to the simulator. Shows the header's "also connected".
 */
let coBound = false;
function coBoundAnswer(m: Uint8Array, port: number): Uint8Array | null {
  if (!coBound || m.length < 8 || m[0] !== 0xf0 || m[1] !== 0x7d || m[2] !== 0x43 || m[3] !== 0x41 || m[4] !== 0x37 || m[5] !== 0x39) return null;
  const head = [0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39];
  if (m[6] === 0x01) return Uint8Array.from([...head, 0x41, 1, 2, 19, 0x3b, 0x77, 0x03, 0xf7]);
  if (m[6] !== 0x1b || m[7] !== 0x03) return null;
  const raw = new Array<number>(48).fill(0);
  raw[0] = 3; raw[1] = 0x07; raw[2] = 0x0c; raw[3] = 4; raw[45] = 0x80 | port;
  const packed: number[] = [];
  for (let i = 0; i < raw.length; i += 7) {
    const g = raw.slice(i, i + 7);
    packed.push(g.reduce((hi, b, k) => hi | (((b >> 7) & 1) << k), 0), ...g.map((b) => b & 0x7f));
  }
  return Uint8Array.from([...head, 0x5b, 0x03, 0x00, ...packed, 0xf7]);
}

/** `--btsrc`: op 0x1D 04 (layout 2), 1D 09 (AUTOCALL), 1D 0C (AVRCP features, read 1f) and 1D 0A (class of device, read 001f00 - the field value of
 *  2026-11-09) answered here - the firmware simulator has no BT module. */
let btSrc = false;
function pack7(raw: number[]): number[] {
  const out: number[] = [];
  for (let i = 0; i < raw.length; i += 7) {
    const g = raw.slice(i, i + 7);
    out.push(g.reduce((hi, b, k) => hi | (((b >> 7) & 1) << k), 0), ...g.map((b) => b & 0x7f));
  }
  return out;
}
function btSrcAnswer(m: Uint8Array): Uint8Array | null {
  if (!btSrc || m.length < 9 || m[0] !== 0xf0 || m[1] !== 0x7d || m[6] !== 0x1d) return null;
  const head = [0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, 0x5d];
  if (m[7] === 0x09) {
    const line = "SET CONTROL AUTOCALL 19 5000 A2DP";
    return Uint8Array.from([...head, 9, 0, m[8] === 0xf7 ? 0 : m[8]!, 1, 0, 0, 1, line.length, ...[...line].map((c) => c.charCodeAt(0)), 0xf7]);
  }
  if (m[7] === 0x0a) {
    const c7 = (c: number) => [(c >> 21) & 0x7f, (c >> 14) & 0x7f, (c >> 7) & 0x7f, c & 0x7f];
    return Uint8Array.from([...head, 0x0a, 0, m[8] === 0xf7 ? 0 : m[8]!, 1, 0, 0, 1, ...c7(0x001f00), ...c7(0x001f00), ...c7(0x240414), 0xf7]);
  }
  if (m[7] === 0x0c) {   // AVRCP controller features, read `1f` (the field line of 2026-11-09)
    return Uint8Array.from([...head, 0x0c, 0, m[8] === 0xf7 ? 0 : m[8]!, 1, 0, 0, 1, 0, 0x1f, 0, 0x1f, 0, 0x0f, 0xf7]);
  }
  if (m[7] !== 0x04) return null;
  const raw = new Array<number>(102).fill(0);
  const TV = [0x12, 0x84, 0x04, 0x04, 0x14, 0x24], PC = [0x02, 0x1a, 0x7d, 0xda, 0x13, 0x23], PH = [0x62, 0x81, 0x81, 0x81, 0x11, 0xa1];
  raw[0] = 2; raw[1] = 2; raw[2] = 0x04; raw[3] = 15; raw[4] = 5; raw[5] = 3; raw[6] = 1;
  raw[18] = 1; raw.splice(19, 6, ...TV);
  const dev = (i: number, a: number[], st: number, links: number, a2dp: number, st2: number, ref: number) => {
    const o = 28 + 14 * i;
    raw[o] = 0x80 | st; raw.splice(o + 1, 6, ...a); raw[o + 7] = links & 0xff; raw[o + 8] = links >> 8; raw[o + 9] = a2dp & 0xff; raw[o + 10] = a2dp >> 8;
    raw[o + 11] = st & 1 ? 42 : 0; raw[o + 12] = st2; raw[o + 13] = ref;
  };
  dev(0, TV, 0x01 | 0x08 | 0x10 | 0x20, 0x0f, 0x03, 0x01 | 0x04, 0);
  dev(1, PC, 0x20 | 0x02, 0x70, 0x30, 0x04 | 0x02, 1);
  dev(2, PH, 0x20 | 0x02, 0x380, 0x000, 0x04, 0);
  raw[84] = 0; raw[85] = 0; raw[86] = 1; raw[87] = 19; raw[88] = 1; raw[95] = 1; raw.splice(96, 6, ...TV);
  return Uint8Array.from([...head, 4, 0, ...pack7(raw), 0xf7]);
}
/**
 * The 「정보」 tab after 「확인」 on a piano with the full slot payload (identify as ota_main: feat 0x3B 0x77 0x0F) and
 * the OTA loader v2 (op 0x10 status layout 2: slot B #13 unconfirmed running, slot A #12 confirmed, boot
 * diagnostics) - the firmware simulator has neither, so this script answers them while `ngFull` is set (the
 * `info-ng` views in shootTabs). Everything else goes to the simulator.
 */
let ngFull = false;
function ngFullAnswer(m: Uint8Array): Uint8Array | null {
  if (!ngFull || m.length < 8 || m[0] !== 0xf0 || m[1] !== 0x7d || m[2] !== 0x43 || m[3] !== 0x41 || m[4] !== 0x37 || m[5] !== 0x39) return null;
  const head = [0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39];
  if (m[6] === 0x01) return Uint8Array.from([...head, 0x41, 1, 2, 19, 0x3b, 0x77, 0x0f, 0xf7]);
  if (m[6] !== 0x10) return null;
  const raw = new Array<number>(72).fill(0);
  const le32 = (o: number, v: number) => raw.splice(o, 4, v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff);
  raw[0] = 2; raw[3] = 1; raw[4] = 1; raw[5] = 1;           // layout 2 · booted · slot B · newest valid slot
  le32(16, 0x1234567); le32(20, 0x3c5e9a01);              // challenge · key id
  le32(24, 12); raw[28] = 0x01 | 0x02 | 0x08;               // slot A #12 valid, confirmed, booted
  le32(30, 13); raw[34] = 0x01 | 0x08;                      // slot B #13 valid, booted (unconfirmed)
  le32(40, 0x1fe00); raw[44] = 1; raw[45] = 200;
  raw[48] = 0x07;                                           // caps: apply now · boot diagnostics · key escape
  le32(54, 0x55aaa55a); le32(58, 0xffffffff); le32(62, 0xffffffff);
  return Uint8Array.from([...head, 0x50, ...pack7(raw), 0xf7]);
}
const fakeAnswer = (m: Uint8Array, port: number) => ngFullAnswer(m) ?? coBoundAnswer(m, port) ?? btSrcAnswer(m);

async function freePort(): Promise<number> {
  return new Promise((ok, no) => {
    const s = createServer();
    s.once("error", no);
    s.listen(0, "127.0.0.1", () => {
      const p = (s.address() as { port: number }).port;
      s.close(() => ok(p));
    });
  });
}

/** Unsigned panel bundle pages (ours only; the stock Kawai UI is not needed to look at ours). */
async function stagePanel(dir: string): Promise<string> {
  rmSync(dir, { recursive: true, force: true });
  const files = [{ path: "index.html", data: chooserHtml({ version: "sim", built: "panelsim", kawai: "1.3.10", seq: 0, keyId: "" }) }, ...(await buildApp("0.0.0-sim"))];
  for (const f of files) {
    mkdirSync(dirname(join(dir, f.path)), { recursive: true });
    writeFileSync(join(dir, f.path), f.data);
  }
  return dir;
}

/** The Java side of the original shell, as far as our pages use it (tests/kwmcore.test.ts fakeShell). */
interface PianoPort { onMessage(fn: (m: Uint8Array) => void): void; send(m: Uint8Array): void }
async function fakeShell(ctx: BrowserContext, page: Page, sim: PianoPort, log: string[]): Promise<void> {
  let live = false;
  const js = (s: string) => page.evaluate(s).catch(() => undefined);
  const esc = (s: string) => s.replace(/\\/g, "\\\\").replace(/'/g, "\\'");
  const resolve = (cmd: string, p: string) => js(`KWM.resolvePromise('${cmd}','${esc(p)}')`);
  const reject = (cmd: string, p = "{}") => js(`KWM.rejectPromise('${cmd}','${esc(p)}')`);
  const devices = JSON.stringify([{ id: "USB", name: "CA79", driver: "USB" }]);
  let uicache = JSON.stringify({ lcdContrast: 80, displayOff: 0 });
  sim.onMessage((m) => {
    if (live) void js(`KWM.onMidiMessage([${[...m].join(",")}],'USB','0')`);
  });
  await ctx.exposeBinding("__kwmPost", (_src, url: string) => {
    const i = url.indexOf("?");
    if (!url.startsWith("kwmcore://") || i < 0) return;
    const cmd = url.substring(10, i - 1);
    let obj: Record<string, unknown> = {};
    try { obj = JSON.parse(url.substring(i + 1)) as Record<string, unknown>; } catch { /* as the shell: ignore */ }
    if (cmd !== "sendMidiMessage") log.push(`shell ${cmd}`);
    switch (cmd) {
      case "sendMidiMessage": {
        const m = Uint8Array.from(obj.msg as number[]);
        const a = fakeAnswer(m, 3);
        if (a) void js(`KWM.onMidiMessage([${[...a].join(",")}],'USB','0')`);
        else sim.send(m);
        return;
      }
      case "getMidiDevices": void js(`KWM.devices=${devices}`).then(() => resolve(cmd, devices)); return;
      case "getEmbeddedMode": void resolve(cmd, "true"); return;
      case "getLcdBacklight": void resolve(cmd, '{"brightness":"204"}'); return;
      case "getLcdInfo": void resolve(cmd, '{"width":480,"height":800,"dpi":160}'); return;
      case "loadJson": void resolve(cmd, uicache); return;
      case "saveJson": uicache = JSON.stringify(obj.json ?? {}); void resolve(cmd, "{}"); return;
      case "getVersions": void js("KWM.localUiVersion='0.0.0-sim';KWM.latestUiVersion='';KWM.localAppVersion='3.4.12';KWM.latestAppVersion='';KWM.latestBleMidiVersion=''").then(() => resolve(cmd, "{}")); return;
      case "getUsbPath": void resolve(cmd, '{"path":""}'); return;
      case "setLcdBacklight": case "disableAutoLock": return;
      default: void reject(cmd);
    }
  });
  await ctx.addInitScript(() => {
    (window as unknown as { kwmcore: unknown }).kwmcore = { postMessage: (u: string) => void (window as unknown as { __kwmPost: (u: string) => void }).__kwmPost(u) };
  });
  // KWMWebViewClient.onPageFinished → detectKWM → devices + deviceready (after every page load)
  page.on("load", () => {
    void (async () => {
      if ((await page.evaluate("typeof KWM").catch(() => "")) !== "object") return;
      live = true;
      await js(`KWM.devices=${devices}`);
      await js(`KWM.dispatchEvent('deviceready',{"midi":true,"ble":false,"strageAccess":true})`);
    })();
  });
}

async function waitReady(page: Page, ms = 25000): Promise<boolean> {
  try {
    // the header carries the phase on every surface (the panel shell has no #connect button)
    await page.waitForSelector('header[data-phase="ready"]', { timeout: ms });
    return true;
  } catch {
    return false;
  }
}

/**
 * Layout checks on the page as drawn (2026-11-01 GUI rework):
 * - overflow: the page scrolls sideways, or an element (not position:fixed, not inside a sideways scroller)
 *   reaches past the viewport's sides
 * - small: a primary control (header / tab bar / main buttons, selects, sliders, switches, file pickers, sheet
 *   buttons) whose tap box is under 44×44 CSS px — for sliders and switches the box of the enclosing
 *   `label` (a row or an inline label) counts — tapping it works the control
 * - clipped: text cut by an ellipsis (report only — long names may be cut on purpose)
 * - offscreen: an open dialog that does not fit the viewport
 * - scrollers: elements that scroll sideways on purpose (overflow-x auto/scroll and wider content — the narrow tab
 *   row #tabbar, 2026-11-02): reported as `selector client/scroll`. Their content may pass the screen edge (not
 *   overflow), but the scroller's own box must stay inside the viewport (else overflow) — the page itself never
 *   scrolls sideways (the page scrollWidth check above)
 * - tabbar: the tab row as drawn — how many tabs, its height, its visible / scrolled width (null when absent)
 * - card overflow (in overflow): an element inside a `.card` that reaches past the card's own sides (not inside a
 *   sideways scroller within the card) — a long value or a button pushed out of its card into the next column
 *   (desktop 「정보」, user 2026-11-02), which the viewport check above does not see
 */
export interface Audit { overflow: string[]; small: string[]; clipped: string[]; offscreen: string[]; scrollers: string[]; tabbar: { tabs: number; height: number; client: number; scroll: number } | null }
const MIN_TAP = 44;
async function audit(page: Page): Promise<Audit> {
  return page.evaluate((min: number) => {
    const vw = document.documentElement.clientWidth;
    const vh = window.innerHeight;
    const desc = (e: Element) => {
      const id = e.id ? `#${e.id}` : "";
      const cls = typeof e.className === "string" && e.className.trim() ? `.${e.className.trim().split(/\s+/).join(".")}` : "";
      const txt = (e.textContent || "").trim().replace(/\s+/g, " ").slice(0, 24);
      return `${e.tagName.toLowerCase()}${id}${cls}${txt ? ` "${txt}"` : ""}`;
    };
    const shown = (e: Element) => {
      const r = e.getBoundingClientRect();
      return r.width > 0 && r.height > 0 && getComputedStyle(e).visibility !== "hidden";
    };
    const fixedUp = (e: Element | null): boolean => !!e && (getComputedStyle(e).position === "fixed" || fixedUp(e.parentElement));
    const a = { overflow: [] as string[], small: [] as string[], clipped: [] as string[], offscreen: [] as string[], scrollers: [] as string[], tabbar: null as Audit["tabbar"] };
    if (document.documentElement.scrollWidth > vw + 1) a.overflow.push(`page scrollWidth ${document.documentElement.scrollWidth} > ${vw}`);
    for (const e of Array.from(document.querySelectorAll("body *"))) {
      if (!shown(e) || e.closest("dialog") || e.closest("#midi-log")) continue;
      const r = e.getBoundingClientRect();
      if ((r.right > vw + 1 || r.left < -1) && !fixedUp(e)) {
        let p = e.parentElement;
        let scroller = false;
        while (p && p !== document.body) {
          const o = getComputedStyle(p).overflowX;
          if (o === "auto" || o === "scroll" || o === "hidden") { scroller = true; break; }
          p = p.parentElement;
        }
        if (!scroller) a.overflow.push(`${desc(e)} ${Math.round(r.left)}..${Math.round(r.right)}`);
      }
      const card = e.parentElement?.closest(".card");
      if (card) {
        const c = card.getBoundingClientRect();
        if (r.right > c.right + 1 || r.left < c.left - 1) {
          let p = e.parentElement;
          let scroller = false;
          while (p && p !== card) {
            const o = getComputedStyle(p).overflowX;
            if (o === "auto" || o === "scroll" || o === "hidden") { scroller = true; break; }
            p = p.parentElement;
          }
          if (!scroller) a.overflow.push(`card ${desc(card).slice(0, 28)}: ${desc(e)} ${Math.round(r.left)}..${Math.round(r.right)} (card ${Math.round(c.left)}..${Math.round(c.right)})`);
        }
      }
      if (getComputedStyle(e).textOverflow === "ellipsis" && e.scrollWidth > e.clientWidth + 1) a.clipped.push(desc(e));
      const ox = getComputedStyle(e).overflowX;
      if ((ox === "auto" || ox === "scroll") && e.scrollWidth > e.clientWidth + 1) {
        a.scrollers.push(`${desc(e).slice(0, 30)} ${e.clientWidth}/${e.scrollWidth}`);
        if (r.left < -1 || r.right > vw + 1) a.overflow.push(`scroller ${desc(e).slice(0, 30)} box ${Math.round(r.left)}..${Math.round(r.right)}`);
      }
    }
    const bar = document.getElementById("tabbar");
    if (bar && shown(bar)) a.tabbar = { tabs: bar.querySelectorAll("button[data-tab]").length, height: Math.round(bar.getBoundingClientRect().height), client: bar.clientWidth, scroll: bar.scrollWidth };
    const primary = "header button, nav button, main button, main select, main input[type=range], main input[type=checkbox], main input[type=file], .banner button";
    for (const e of Array.from(document.querySelectorAll(primary))) {
      if (!shown(e) || e.closest("dialog")) continue;
      const box = ((e.matches("input") && e.closest("label")) || e).getBoundingClientRect();
      if (box.height < min - 0.5 || box.width < min - 0.5) a.small.push(`${desc(e)} ${Math.round(box.width)}×${Math.round(box.height)}`);
    }
    for (const d of Array.from(document.querySelectorAll("dialog[open]"))) {
      const r = d.getBoundingClientRect();
      if (r.left < -1 || r.top < -1 || r.right > vw + 1 || r.bottom > vh + 1) a.offscreen.push(`${desc(d).slice(0, 40)} ${Math.round(r.left)},${Math.round(r.top)}..${Math.round(r.right)},${Math.round(r.bottom)}`);
      for (const b of Array.from(d.querySelectorAll("button, select"))) {
        if (!shown(b)) continue;
        const br = b.getBoundingClientRect();
        if (br.height < min - 0.5 || br.width < min - 0.5) a.small.push(`dialog#${d.id} ${desc(b)} ${Math.round(br.width)}×${Math.round(br.height)}`);
      }
    }
    for (const k of ["overflow", "small", "clipped", "offscreen", "scrollers"] as const) a[k] = Array.from(new Set(a[k]));
    return a;
  }, MIN_TAP);
}

/**
 * A user's tap on `loc`: scrolled into view (a tab in the bottom row that scrolls sideways, as a swipe does; a button
 * in <main> vertically), then a real click.
 *
 * The click needs the control on the screen. When the page is wider than the screen, the emulated phone (isMobile)
 * widens its layout viewport to the page and grows it in proportion (390×844 with a 534 px page → 534×1156) without
 * zooming out like a phone's overview mode: the fixed bottom tab row then sits below the visible 844 px, the click
 * lands on whatever <main> shows there and Playwright retried for 30 s ("… from <main> subtree intercepts pointer
 * events" - a sound row or a song row depending on the screen) and the run died before its audit. That is a layout
 * fault (the page scrolls sideways - audit overflow), not a control fault: it is logged as TAP BLOCKED with the
 * geometry (fails the run), the control gets its own click() and the run goes on, so the screenshots and the audit
 * show what is too wide.
 */
async function tap(loc: Locator, what: string, log: string[]): Promise<void> {
  const blocked = await loc.evaluate((e) => {
    e.scrollIntoView({ block: "nearest", inline: "nearest" });
    const vv = window.visualViewport;
    const vw = vv ? vv.width : innerWidth, vh = vv ? vv.height : innerHeight;
    const r = e.getBoundingClientRect();
    const x = r.left + r.width / 2, y = r.top + r.height / 2;
    const hit = x >= 0 && x < vw && y >= 0 && y < vh ? document.elementFromPoint(x, y) : null;
    if (hit && e.contains(hit)) return "";
    const d = document.documentElement;
    const how = hit ? `${hit.tagName.toLowerCase()} "${(hit.textContent || "").trim().replace(/\s+/g, " ").slice(0, 24)}" covers it` : "off the screen";
    return `centre ${Math.round(x)},${Math.round(y)} on a ${Math.round(vw)}×${Math.round(vh)} screen: ${how}; ` +
      `page ${d.scrollWidth} px wide, layout viewport ${innerWidth}×${innerHeight}`;
  });
  if (!blocked) return loc.click();
  log.push(`TAP BLOCKED ${what}: ${blocked}`);
  await loc.evaluate((e) => (e as HTMLElement).click());
}

/** Open a tab the way a user does: its button in the tab list (wide) or the bottom tab row (narrow — every tab is
 *  there since 2026-11-02). */
function openTab(page: Page, id: string, log: string[]): Promise<void> {
  return tap(page.locator(`nav button[data-tab="${id}"]`), `tab ${id}`, log);
}

async function shootTabs(page: Page, out: string, shots: string[], audits: Record<string, Audit>, log: string[]): Promise<void> {
  for (let k = 0; k < TAB_IDS.length; k++) {
    await openTab(page, TAB_IDS[k], log);
    await sleep(700);
    await page.evaluate(() => window.scrollTo(0, 0));
    const name = `${String(k + 1).padStart(2, "0")}-${TAB_IDS[k]}`;
    await page.screenshot({ path: join(out, `${name}.png`), scale: "css" });
    await page.screenshot({ path: join(out, `${name}-full.png`), fullPage: true, scale: "css" });
    shots.push(`${name}.png`, `${name}-full.png`);
    audits[TAB_IDS[k]] = await audit(page);
  }
  // 「정보」 after 「확인」 on a piano with the full slot payload (ngFullAnswer): every card the firmware turns on,
  // first as opened (collapsible sections as they start), then with every collapsible section open
  await openTab(page, "info", log);
  await sleep(300);
  const check = page.locator("#ng-check");
  if ((await check.count()) && (await check.isEnabled())) {
    ngFull = true;
    try {
      await tap(check, "#ng-check", log);
      await sleep(2500);
      for (const [name, open] of [["11-info-ng", false], ["12-info-ng-open", true]] as const) {
        if (open) await page.evaluate(() => document.querySelectorAll("main details").forEach((d) => d.setAttribute("open", "")));
        await sleep(300);
        await page.evaluate(() => window.scrollTo(0, 0));
        await page.screenshot({ path: join(out, `${name}.png`), scale: "css" });
        await page.screenshot({ path: join(out, `${name}-full.png`), fullPage: true, scale: "css" });
        shots.push(`${name}.png`, `${name}-full.png`);
        audits[name] = await audit(page);
      }
    } finally {
      ngFull = false;
    }
  }
  // the connection menu sheet (not in the panel shell: always connected, no connect button)
  await openTab(page, TAB_IDS[0], log);
  await sleep(300);
  const menus: [string[], string][] = [[["#connect"], "90-connect-menu"]];
  for (const [sels, name] of menus) {
    for (const sel of sels) {
      const b = page.locator(sel);
      if (!(await b.count()) || !(await b.isVisible())) continue;
      await tap(b, sel, log);
      await sleep(400);
      await page.screenshot({ path: join(out, `${name}.png`), scale: "css" });
      shots.push(`${name}.png`);
      audits[name] = await audit(page);
      await page.keyboard.press("Escape");
      await sleep(250);
      break;
    }
  }
}

/** Audit lines that fail the run: sideways overflow on every profile, small tap targets on the touch profiles. */
function auditErrors(id: ProfileId, audits: Record<string, Audit>): string[] {
  const touch = PROFILES[id].width < 900;
  const out: string[] = [];
  for (const [view, a] of Object.entries(audits)) {
    for (const o of [...a.overflow, ...a.offscreen]) out.push(`AUDIT ${view}: overflow ${o}`);
    if (touch) for (const s of a.small) out.push(`AUDIT ${view}: small tap target ${s}`);
    // narrow: every tab in the one bottom row, at least a tap high
    if (touch && a.tabbar && (a.tabbar.tabs !== TAB_IDS.length || a.tabbar.height < MIN_TAP)) out.push(`AUDIT ${view}: tab row ${a.tabbar.tabs}/${TAB_IDS.length} tabs, ${a.tabbar.height} px high`);
  }
  return out;
}

function watch(page: Page, log: string[]): void {
  page.on("pageerror", (e) => log.push(`PAGEERROR ${e.message}`));
  page.on("console", (m) => {
    if (m.type() === "error" || m.type() === "warning") log.push(`console.${m.type()} ${m.text()}`);
  });
}

/** The simulator's BLE port as the phone: what the app's 「패널 화면 보기」 talks through (the simulator relays ops
 *  0x30-0x3F / 0x70-0x7F between it and the panel port like the piano's cave, firmware ports/sim/main.c). */
function simBlePort(sim: SimProcessLink): PuiPort {
  const ls = new Set<(m: Uint8Array) => void>();
  sim.onLine((line) => {
    const m = /^tx ble (.*)$/.exec(line);
    if (m) for (const f of ls) f(fromHex(m[1]));
  });
  return { sendRaw: (m) => sim.command(`midi ble ${toHex(m)}`), onMidi: (fn) => (ls.add(fn), () => void ls.delete(fn)) };
}

/**
 * The screen reporter end to end: from the simulator's BLE port
 * through its relay to the page shown, the page's own DOM → canvas painter (here in Chromium; on the panel Chrome 71).
 * Writes <name>-report.json · <name>-snapshot.jpg next to a real screenshot <name>-reference.png.
 */
async function screenCheck(page: Page, port: PuiPort, out: string, name: string, shots: string[], log: string[]): Promise<void> {
  await page.screenshot({ path: join(out, `${name}-reference.png`), scale: "css" });
  const cli = new PanelScreenClient(port);
  const rep = await cli.report();
  if (!rep.ok || !rep.report) return void log.push(`SCREEN FAIL ${name} report: ${rep.error}`);
  writeFileSync(join(out, `${name}-report.json`), JSON.stringify(rep.report, null, 1) + "\n");
  const shot = await cli.snapshot();
  if (!shot.ok || !shot.bytes || !shot.info) return void log.push(`SCREEN FAIL ${name} snapshot: ${shot.error}`);
  writeFileSync(join(out, `${name}-snapshot.jpg`), shot.bytes);
  shots.push(`${name}-reference.png`, `${name}-snapshot.jpg`);
  const est = estimateScreenTransfer(shot.bytes.length);
  log.push(`screen ${name}: report ${rep.bytes!.length} B (${rep.report.pg}${rep.report.tab ? ` · ${rep.report.tab}` : ""}, ${rep.report.t?.length ?? 0} lines, cut ${rep.report.cut ?? 0}) in ${rep.ms} ms · ` +
    `snapshot ${shot.info.width}x${shot.info.height} ${shot.bytes.length} B drawn in ${shot.info.ms} ms, ${shot.frames} reads ${shot.readMs} ms here (field rate ≈ ${est.seconds.toFixed(1)} s)`);
  if (shot.bytes.length > 40 * 1024 || shot.bytes.length < 2000) log.push(`SCREEN FAIL ${name} snapshot size ${shot.bytes.length} B`);
}

async function runPanel(browser: Browser, id: ProfileId, out: string, stage: string): Promise<{ shots: string[]; log: string[]; ready: boolean; audits: Record<string, Audit> }> {
  const p = PROFILES[id];
  const ctx = await browser.newContext({ viewport: { width: p.width, height: p.height }, deviceScaleFactor: p.dpr, isMobile: true, hasTouch: true, userAgent: PANEL_UA, locale: "en-US" });
  const page = await ctx.newPage();
  const log: string[] = [];
  const shots: string[] = [];
  const audits: Record<string, Audit> = {};
  watch(page, log);
  const sim = new SimProcessLink({ port: "panel" });
  await sim.ready;
  try {
    await fakeShell(ctx, page, sim, log);
    const base = pathToFileURL(stage).href;
    await page.goto(`${base}/index.html?stay=1`);
    await sleep(800);
    await page.screenshot({ path: join(out, "00-chooser.png"), scale: "css" });
    const phone = simBlePort(sim);
    await screenCheck(page, phone, out, "80-screen-chooser", shots, log);
    const rec = page.locator("#rec summary");
    if (await rec.count()) {
      await rec.click();
      await sleep(400);
      await page.screenshot({ path: join(out, "00-chooser-recovery.png"), scale: "css", fullPage: true });
      shots.push("00-chooser-recovery.png");
    }
    shots.push("00-chooser.png");
    // pick the OpenGrand UI language on the chooser, as the user does (kept in localStorage ca79ng.settings)
    const lb = page.locator(`#langrow button[data-lang="${p.lang}"]`);
    if (await lb.count()) await lb.click();
    else log.push("chooser: no language button");
    await page.goto(`${base}/ng/index.html?embedded=1`);
    const ready = await waitReady(page);
    if (!ready) log.push("NOT CONNECTED within 25 s");
    await sleep(800);
    await shootTabs(page, out, shots, audits, log);
    for (const tab of ["sound", "settings"]) {
      await openTab(page, tab, log);
      await sleep(500);
      await page.evaluate(() => window.scrollTo(0, 0));
      await screenCheck(page, phone, out, `81-screen-${tab}`, shots, log);
    }
    return { shots, log, ready, audits };
  } finally {
    sim.close();
    await ctx.close();
  }
}

/**
 * `--updatemode`: the panel page against the USB updater model. Passes when the update
 * screen shows and the page goes to the Kawai update page (appUI/developper/dpupdatemode — not staged here, so
 * only the URL is checked) well before the 30 s noconnect.
 */
async function runUpdateMode(browser: Browser, out: string, stage: string): Promise<{ shots: string[]; log: string[]; ready: boolean; audits: Record<string, Audit> }> {
  const p = PROFILES.panel;
  const ctx = await browser.newContext({ viewport: { width: p.width, height: p.height }, deviceScaleFactor: p.dpr, isMobile: true, hasTouch: true, userAgent: PANEL_UA, locale: "en-US" });
  const page = await ctx.newPage();
  const log: string[] = [];
  const shots: string[] = [];
  const audits: Record<string, Audit> = {};
  watch(page, log);
  let toPage: (m: Uint8Array) => void = () => {};
  const model = new UpdaterModel((m) => toPage(m));
  let connects = 0;
  const port: PianoPort = {
    onMessage: (fn) => { toPage = fn; },
    send: (m) => { if (m[3] === 0x7e && m[6] === 0x01 && m[7] === 0x01) connects++; model.receive(m); },
  };
  const timer = setInterval(() => model.step(), 100); // one 64 KB block per 100 ms
  try {
    await fakeShell(ctx, page, port, log);
    // a stand-in for the stock Kawai update page (the stock UI is not staged)
    const stub = join(stage, "developper/dpupdatemode/html/index.html");
    mkdirSync(dirname(stub), { recursive: true });
    writeFileSync(stub, "<!doctype html><title>Kawai update page (stand-in)</title><p>Firmware Update</p>");
    const base = pathToFileURL(stage).href;
    await page.goto(`${base}/index.html?stay=1`);
    await page.locator('#langrow button[data-lang="ko"]').click().catch(() => log.push("chooser: no language button"));
    const t0 = Date.now();
    await page.goto(`${base}/ng/index.html?embedded=1`);
    let shown = false;
    try {
      await page.waitForSelector("#update-mode:not([hidden])", { timeout: 10000 });
      shown = true;
    } catch {
      log.push("UPDATE FAIL: no update screen within 10 s");
    }
    if (shown) {
      await sleep(300);
      await page.screenshot({ path: join(out, "70-update-mode.png"), scale: "css" }).catch(() => undefined);
      shots.push("70-update-mode.png");
      const text = (await page.locator("#update-mode").textContent().catch(() => "")) ?? "";
      if (!/전원을 끄지 마세요/.test(text)) log.push(`UPDATE FAIL: no warning on the update screen: ${text.slice(0, 120)}`);
    }
    try {
      await page.waitForURL(/developper\/dpupdatemode\/html\/index\.html/, { timeout: 8000 });
      await page.screenshot({ path: join(out, "71-kawai-update-page.png"), scale: "css" });
      shots.push("71-kawai-update-page.png");
      log.push(`update mode: Kawai update page after ${((Date.now() - t0) / 1000).toFixed(1)} s, ${connects} CONNECT, model bound ${model.bound}`);
    } catch {
      log.push("UPDATE FAIL: no hand-over to the Kawai update page within 8 s");
    }
    return { shots, log, ready: shown, audits };
  } finally {
    clearInterval(timer);
    await ctx.close();
  }
}

async function runWeb(browser: Browser, id: ProfileId, out: string, origin: string): Promise<{ shots: string[]; log: string[]; ready: boolean; audits: Record<string, Audit> }> {
  const p = PROFILES[id];
  const mobile = p.width < 600;
  const ctx = await browser.newContext({ viewport: { width: p.width, height: p.height }, deviceScaleFactor: p.dpr, isMobile: mobile, hasTouch: mobile, locale: p.lang === "ko" ? "ko-KR" : "en-US", serviceWorkers: "block" });
  const page = await ctx.newPage();
  const log: string[] = [];
  const shots: string[] = [];
  const audits: Record<string, Audit> = {};
  watch(page, log);
  // the app is the BLE controller (port 2) next to the panel; fakeAnswer passes everything through unless
  // --cobound / --btsrc or the info-ng views (ngFull) answer it here
  await page.routeWebSocket(/\/midi$/, (ws) => {
    const server = ws.connectToServer();
    ws.onMessage((msg) => {
      const m = typeof msg === "string" ? Uint8Array.from(Buffer.from(msg)) : Uint8Array.from(msg);
      const a = fakeAnswer(m, 2);
      if (a) ws.send(Buffer.from(a));
      else server.send(msg);
    });
  });
  try {
    await page.goto(`${origin}/?sim&tab=sound`);
    const ready = await waitReady(page);
    if (!ready) log.push("NOT CONNECTED within 25 s");
    await sleep(800);
    await shootTabs(page, out, shots, audits, log);
    return { shots, log, ready, audits };
  } finally {
    await ctx.close();
  }
}

/**
 * WSL / minimal Linux without root: the headless shell needs a few system libraries (nspr, nss, alsa).
 * Missing ones are fetched as .deb (apt-get download, no root) and unpacked under node_modules/.cache.
 */
const LIBS = join(APP, "node_modules/.cache/panelsim-libs");
const LIB_PKGS: Record<string, string[]> = { "libnspr4.so": ["libnspr4"], "libnss3.so": ["libnss3"], "libnssutil3.so": ["libnss3"], "libasound.so.2": ["libasound2t64", "libasound2"] };
export function libEnv(): Record<string, string> {
  // headless: true runs chrome-headless-shell (not chromium.executablePath()): ldd the shells that are there
  const reg = process.env.PLAYWRIGHT_BROWSERS_PATH || join(process.env.HOME ?? "", ".cache/ms-playwright");
  const exe = existsSync(reg) ? readdirSync(reg).filter((d) => d.startsWith("chromium_headless_shell-")).sort().map((d) => join(reg, d, "chrome-headless-shell-linux64/chrome-headless-shell")).filter(existsSync).pop() ?? "" : "";
  const dir = join(LIBS, "root/usr/lib/x86_64-linux-gnu");
  const env = { ...process.env, LD_LIBRARY_PATH: [dir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(":") } as Record<string, string>;
  if (process.platform !== "linux" || !exe) return env;
  const missing = (e: Record<string, string>) => {
    const r = spawnSync("ldd", [exe], { env: e, encoding: "utf8" });
    return [...(r.stdout ?? "").matchAll(/^\s*(\S+) => not found/gm)].map((m) => m[1]);
  };
  const miss = missing(env);
  if (!miss.length) return env;
  const pkgs = [...new Set(miss.flatMap((l) => LIB_PKGS[l] ?? []))];
  console.log(`headless Chromium needs ${miss.join(", ")} → apt-get download ${pkgs.join(" ")} into ${LIBS}`);
  mkdirSync(join(LIBS, "deb"), { recursive: true });
  for (const p of pkgs) spawnSync("apt-get", ["download", p], { cwd: join(LIBS, "deb"), stdio: "ignore" });
  for (const f of readdirSync(join(LIBS, "deb"))) if (f.endsWith(".deb")) spawnSync("dpkg-deb", ["-x", join(LIBS, "deb", f), join(LIBS, "root")]);
  const still = missing(env);
  if (still.length) console.error(`still missing: ${still.join(", ")} (install them: sudo apt-get install ${pkgs.join(" ")})`);
  return env;
}

async function main(): Promise<number> {
  const a = args(process.argv.slice(2));
  coBound = !!a.cobound;
  btSrc = !!a.btsrc;
  const updateMode = !!a.updatemode;
  const outRoot = resolve(APP, a.out ?? (coBound ? ".panelsim/cobound" : btSrc ? ".panelsim/btsrc" : updateMode ? ".panelsim/updatemode" : ".panelsim"));
  const only = updateMode ? (["panel"] as ProfileId[]) : a.only ? (a.only.split(",") as ProfileId[]) : (["panel", "phone", "desktop"] as ProfileId[]);
  for (const id of only) if (!(id in PROFILES)) throw new Error(`--only ${id}: one of ${Object.keys(PROFILES).join(", ")}`);
  if (!existsSync(SIM_BIN)) throw new Error(`firmware simulator not built: ${SIM_BIN} (make -C result/firmware sim)`);
  let browser: Browser;
  try {
    browser = await chromium.launch({ headless: true, env: libEnv() });
  } catch (e) {
    console.error(`${(e as Error).message.split("\n")[0]}\nheadless Chromium missing: npx playwright-core install chromium-headless-shell`);
    return 2;
  }
  let server: ReturnType<typeof spawn> | null = null;
  let bad = 0;
  try {
    const stage = only.some((id) => PROFILES[id].kind === "panel") ? await stagePanel(join(outRoot, "stage", "appUI")) : "";
    let origin = "";
    if (only.some((id) => PROFILES[id].kind === "web")) {
      const b = spawn(process.execPath, ["scripts/build.mjs"], { cwd: APP, stdio: "ignore" });
      if ((await new Promise<number | null>((r) => b.on("exit", r))) !== 0) throw new Error("web build failed (node scripts/build.mjs)");
      const port = await freePort();
      server = spawn(process.execPath, ["scripts/dev-server.ts"], { cwd: APP, env: { ...process.env, PORT: String(port) }, stdio: "ignore" });
      origin = `http://localhost:${port}`;
      for (let i = 0; i < 50; i++) {
        if (await fetch(origin).then((r) => r.ok, () => false)) break;
        await sleep(100);
      }
    }
    for (const id of only) {
      const out = join(outRoot, id);
      rmSync(out, { recursive: true, force: true });
      mkdirSync(out, { recursive: true });
      const r = updateMode ? await runUpdateMode(browser, out, stage) : PROFILES[id].kind === "panel" ? await runPanel(browser, id, out, stage) : await runWeb(browser, id, out, origin);
      const errors = [...r.log.filter((l) => /^PAGEERROR|^NOT CONNECTED|^SCREEN FAIL|^UPDATE FAIL|^TAP BLOCKED/.test(l)), ...(a["no-audit"] ? [] : auditErrors(id, r.audits))];
      writeFileSync(join(out, "log.txt"), r.log.join("\n") + "\n");
      writeFileSync(join(out, "audit.json"), JSON.stringify(r.audits, null, 1) + "\n");
      const p = PROFILES[id];
      console.log(`${id}: ${p.width}x${p.height}@${p.dpr} ${p.lang} — ${r.shots.length} PNG in ${out}${r.ready ? "" : " (NOT connected)"}${errors.length ? `\n  ${errors.join("\n  ")}` : ""}`);
      if (errors.length) bad++;
    }
  } finally {
    server?.kill();
    await browser.close();
  }
  return bad ? 1 : 0;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().then((c) => process.exit(c), (e: Error) => {
    console.error(e.stack ?? e.message);
    process.exit(1);
  });
}
