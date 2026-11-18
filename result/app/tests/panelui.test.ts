// Remote panel UI update (REMOTE_PROTOCOL.md §10.7):
// the ES5 loader's crypto / inflate against Node's, package format TS ↔ ES5, the receiver
// behind a fake piano relay (window, losses, resume across a page reload, storage quota),
// boot order and fall-backs, and the BLE time estimate for the real bundle.
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { createHash, randomBytes, sign } from "node:crypto";
import { runInNewContext } from "node:vm";
import { deflateRawSync } from "node:zlib";
import { JSDOM, VirtualConsole } from "jsdom";

import { keyFromPem, loaderSource, makePackage, newKeyPem, panelCheck, uiConfig, type PanelKey, type UiConfig } from "../scripts/panelpkg.ts";
import { buildApp, chooserHtml, probeFiles } from "../scripts/bundlecore.ts";
import { BleMidiDecoder, encodeBlePackets } from "../src/protocol/blemidi.ts";
import {
  dataFrame, decodeHeader, decodePanelStatus, estimateTransfer, PanelUiClient, PanelUiOp, parsePackage, PuiControl, PUI_CHUNK, puiRequest, type PuiPort, type UploadProgress,
} from "../src/protocol/panelui.ts";

const KEY: PanelKey = keyFromPem(newKeyPem());
const OTHER: PanelKey = keyFromPem(newKeyPem());
const CFG: UiConfig = uiConfig(KEY, { version: "0.2.0", seq: 1000 });

type Loader = {
  _: {
    sha512(b: Uint8Array): Uint8Array;
    edVerify(sig: Uint8Array, msg: Uint8Array, pub: Uint8Array): boolean;
    inflateRaw(b: Uint8Array, n: number): Uint8Array;
    b64enc(b: Uint8Array): string;
    b64dec(s: string): Uint8Array;
    utf8dec(b: Uint8Array): string;
    crc32(b: Uint8Array): number;
    openPackage(p: Uint8Array): { rc: number; h?: { seq: number; version: string }; files?: { js: string; css: string } };
  };
  hookKwm(k: Record<string, unknown>): unknown;
  handle(msg: number[]): number[] | null;
  boot(b: { js: string; css: string }): string;
  confirm(): void;
  fail(r: string): void;
  control(sub: number): number;
  state(): { refused?: number; running: string; rx: number; got: number; total: number; maxSeq: number; slots: Record<string, { seq: number; state: string; tries: number; why?: string; version: string }> };
  on(f: (kind: string, d: Record<string, unknown>) => void): void;
  setPage(p: number): void;
  probeQuota(max?: number): number;
  navigate: (url: string) => void;
  bootTimeoutMs: number;
};

/** localStorage double with Chromium's quota rule: 10 MiB of UTF-16 (keys + values). */
class FakeStorage {
  readonly m = new Map<string, string>();
  readonly quotaChars: number;
  constructor(quotaChars = 5 * 1024 * 1024) {
    this.quotaChars = quotaChars;
  }
  private size(): number {
    let n = 0;
    for (const [k, v] of this.m) n += k.length + v.length;
    return n;
  }
  getItem(k: string) {
    return this.m.has(k) ? this.m.get(k)! : null;
  }
  setItem(k: string, v: string) {
    const old = this.m.get(k);
    if (this.size() - (old !== undefined ? k.length + old.length : 0) + k.length + String(v).length > this.quotaChars) throw Object.assign(new Error("QuotaExceededError"), { name: "QuotaExceededError" });
    this.m.set(k, String(v));
  }
  removeItem(k: string) {
    this.m.delete(k);
  }
}

/** The loader in a fresh VM context — one "page load" of the panel. */
function panelPage(storage: FakeStorage, cfg: UiConfig | null = CFG, page = 1) {
  const sent: number[][] = [];
  const nav: string[] = [];
  const shell: string[] = []; // every shell call other than MIDI: "cmd {json}"
  const ctx: Record<string, unknown> = {
    localStorage: storage,
    // unref: the loader's 15 s boot guard must not keep the test process alive
    setTimeout: (f: () => void, ms: number) => setTimeout(f, ms).unref(),
    clearTimeout,
    location: { search: "?embedded=1", replace: (u: string) => nav.push(u) },
    kwmcore: {
      postMessage(url: string) {
        const cmd = url.substring(10, url.indexOf("?") - 1);
        if (cmd !== "sendMidiMessage") shell.push(`${cmd} ${url.slice(url.indexOf("?") + 1)}`);
        if (cmd === "sendMidiMessage") sent.push(JSON.parse(url.slice(url.indexOf("?") + 1)).msg);
      },
    },
    addEventListener() {},
  };
  runInNewContext(loaderSource(cfg), ctx, { filename: "uiloader.js" });
  const ui = ctx.CA79UI as Loader;
  ui.navigate = (u) => nav.push(u);
  ui.setPage(page);
  const KWM: Record<string, unknown> = { onMidiMessage: () => {} };
  ui.hookKwm(KWM);
  ctx.KWM = KWM;
  return { ui, ctx, sent, nav, KWM, shell };
}

const appFiles = (tag: string) => ({ js: `window.__app = "${tag}"; window.__ca79ngBooted = true;`, css: `body{--tag:"${tag}"}` });
const pack = (seq: number, tag = "v" + seq, key = KEY, version = "0.3." + (seq % 1000)) => makePackage(appFiles(tag), { version, seq, key });

// ---- primitives against Node ------------------------------------------------------

test("loader SHA-512 equals node:crypto for lengths across block edges", () => {
  const { ui } = panelPage(new FakeStorage());
  for (const n of [0, 1, 3, 55, 111, 112, 113, 127, 128, 129, 239, 240, 255, 256, 1000, 70001]) {
    const b = new Uint8Array(randomBytes(n));
    assert.equal(Buffer.from(ui._.sha512(b)).toString("hex"), createHash("sha512").update(b).digest("hex"), `n=${n}`);
  }
});

test("loader Ed25519 verify accepts node:crypto signatures and rejects any change", () => {
  const { ui } = panelPage(new FakeStorage());
  for (const n of [0, 1, 80, 1000]) {
    const msg = new Uint8Array(randomBytes(n));
    const sig = new Uint8Array(sign(null, msg, KEY.priv!));
    assert.equal(ui._.edVerify(sig, msg, KEY.pub), true, `valid n=${n}`);
    assert.equal(ui._.edVerify(sig, msg, OTHER.pub), false, "other key");
    const s2 = sig.slice();
    s2[n % 64] ^= 0x10;
    assert.equal(ui._.edVerify(s2, msg, KEY.pub), false, "flipped signature bit");
    if (n) {
      const m2 = msg.slice();
      m2[0] ^= 1;
      assert.equal(ui._.edVerify(sig, m2, KEY.pub), false, "flipped message bit");
    }
  }
  // RFC 8032 §7.1 test 1 (empty message)
  const pub = Buffer.from("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "hex");
  const sig = Buffer.from("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", "hex");
  assert.equal(ui._.edVerify(new Uint8Array(sig), new Uint8Array(0), new Uint8Array(pub)), true, "RFC 8032 test 1");
  // non-canonical S (S + L) is refused
  const big = new Uint8Array(sig);
  const L = Buffer.from("edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010", "hex");
  let c = 0;
  for (let i = 0; i < 32; i++) {
    const v = big[32 + i]! + L[i]! + c;
    big[32 + i] = v & 255;
    c = v >> 8;
  }
  assert.equal(ui._.edVerify(big, new Uint8Array(0), new Uint8Array(pub)), false, "S >= L");
});

test("loader inflate equals zlib (stored, fixed, dynamic blocks) and base64 / UTF-8 round trip", () => {
  const { ui } = panelPage(new FakeStorage());
  const text = Buffer.from("한글 ✓ 𝄞 CA79 ".repeat(3000) + JSON.stringify({ a: [1, 2, 3] }));
  const samples = [Buffer.alloc(0), Buffer.from("a"), randomBytes(70000), text, Buffer.alloc(100000, 7)];
  for (const s of samples) {
    for (const level of [0, 1, 9]) {
      const d = deflateRawSync(s, { level });
      const out = ui._.inflateRaw(new Uint8Array(d), s.length);
      assert.ok(Buffer.from(out).equals(s), `len ${s.length} level ${level}`);
    }
  }
  assert.equal(ui._.utf8dec(new Uint8Array(text)), text.toString("utf8"));
  for (const n of [0, 1, 2, 3, 4, 1000]) {
    const b = new Uint8Array(randomBytes(n));
    assert.equal(ui._.b64enc(b), Buffer.from(b).toString("base64"));
    assert.deepEqual(Array.from(ui._.b64dec(ui._.b64enc(b))), Array.from(b));
  }
  assert.throws(() => ui._.inflateRaw(new Uint8Array([0xff, 0xff, 0xff]), 10));
});

// ---- package format ----------------------------------------------------------------

test("package: TS header ↔ ES5 parse, signature, key id, hash, unpack", () => {
  const file = pack(2000, "x");
  const p = parsePackage(new Uint8Array(file))!;
  assert.equal(p.header.seq, 2000);
  assert.equal(p.header.keyId, KEY.keyId);
  assert.equal(p.header.flags & 1, 1);
  assert.deepEqual(panelCheck(file, CFG), { rc: 0, version: "0.3.0", seq: 2000, jsBytes: appFiles("x").js.length });
  assert.equal(panelCheck(file, uiConfig(OTHER, { version: "", seq: 0 })).rc, 11, "other key → header (key id)");
  assert.equal(panelCheck(file, uiConfig(null, { version: "", seq: 0 })).rc, 17, "bundle without key");
  // signature over the header: change the version text → signature mismatch
  const f2 = Buffer.from(file);
  f2[30] ^= 1;
  assert.equal(panelCheck(f2, CFG).rc, 12);
  // payload bit flip → hash mismatch (header still signed)
  const f3 = Buffer.from(file);
  f3[f3.length - 3] ^= 1;
  assert.equal(panelCheck(f3, CFG).rc, 10);
  // forged key id with another key's signature → signature mismatch
  const forged = makePackage(appFiles("evil"), { version: "9.9.9", seq: 9999, key: { ...OTHER, keyId: KEY.keyId } });
  assert.equal(panelCheck(forged, CFG).rc, 12);
  assert.equal(decodeHeader(new Uint8Array(10)), null);
});

// ---- fake piano relay ------------------------------------------------------------------

interface RelayOpts {
  dropToPanel?: (frame: Uint8Array, i: number) => boolean;
  dropToPhone?: (frame: number[], i: number) => boolean;
  /** what reaches the panel instead of the phone's frame (bytes lost / added on BLE or a UART), null = lost */
  mangleToPanel?: (frame: Uint8Array, i: number) => Uint8Array | null;
  delayMs?: number;
}

/**
 * Phone ↔ piano relay ↔ panel, as the cave must do it: ops 0x30-0x3F from the
 * phone go verbatim to the panel port, replies 0x70-0x7F from the panel back to the phone.
 * Everything else would be the piano's (not modelled). Checks the frame size limits.
 */
class FakeRelay implements PuiPort {
  private listeners: ((m: Uint8Array) => void)[] = [];
  toPanel = 0;
  toPhone = 0;
  maxFrame = 0;
  page: ReturnType<typeof panelPage>;
  readonly o: RelayOpts;
  constructor(page: ReturnType<typeof panelPage>, o: RelayOpts = {}) {
    this.page = page;
    this.o = o;
  }
  sendRaw(msg: Uint8Array): void {
    assert.ok(msg.length <= 245, `phone frame ${msg.length} B > 245 B (BLE path ≤ 256 B)`);
    this.maxFrame = Math.max(this.maxFrame, msg.length);
    const isUi = msg[0] === 0xf0 && msg[1] === 0x7d && msg[6]! >= 0x30 && msg[6]! <= 0x3f;
    if (!isUi) return;
    const i = this.toPanel++;
    if (this.o.dropToPanel?.(msg, i)) return;
    if (this.o.mangleToPanel) {
      const m = this.o.mangleToPanel(msg, i);
      if (!m) return;
      msg = m;
    }
    setTimeout(() => {
      const before = this.page.sent.length;
      (this.page.KWM.onMidiMessage as (m: number[], id: string, t: string) => void)(Array.from(msg), "USB", "0");
      for (const r of this.page.sent.splice(before)) {
        assert.ok(r.length <= 511, "panel → piano SysEx fits the stock buffers");
        assert.ok(r[6]! >= 0x70 && r[6]! <= 0x7f, "only replies come back");
        const j = this.toPhone++;
        if (this.o.dropToPhone?.(r, j)) continue;
        setTimeout(() => this.listeners.forEach((l) => l(Uint8Array.from(r))), this.o.delayMs ?? 0);
      }
    }, this.o.delayMs ?? 0);
  }
  onMidi(fn: (m: Uint8Array) => void): () => void {
    this.listeners.push(fn);
    return () => {
      this.listeners = this.listeners.filter((l) => l !== fn);
    };
  }
}

test("upload over the fake relay: window 2, commit stores slot A, status and boot pick it", async () => {
  const store = new FakeStorage();
  const panel = panelPage(store);
  const relay = new FakeRelay(panel);
  const cli = new PanelUiClient(relay);
  const st0 = await cli.status();
  assert.ok(st0);
  assert.equal(st0.hasKey, true);
  assert.equal(st0.bundledSeq, 1000);
  assert.equal(st0.bundledVersion, "0.2.0");
  assert.equal(st0.chunk, PUI_CHUNK);
  const file = pack(2000, "big" + randomBytes(3000).toString("hex"), KEY, "0.3.0");
  const events: string[] = [];
  panel.ui.on((k) => events.push(k));
  const fin = await cli.upload(new Uint8Array(file), { window: 2 });
  assert.equal(fin.phase, "done", fin.message);
  assert.equal(fin.message, "A");
  assert.equal(relay.maxFrame, 245, "Data frames with their CRC-32");
  assert.ok(events.includes("begin") && events.includes("stored"));
  const st = (await cli.status())!;
  assert.equal(st.slots[0].seq, 2000);
  assert.equal(st.slots[0].state, "new");
  assert.equal(st.slots[0].version, "0.3.0");
  assert.equal(st.maxSeq, 2000);
  assert.equal(st.rxState, 4);
  // same package again → already installed; older seq → refused
  assert.equal((await cli.upload(new Uint8Array(file))).rc, 14);
  assert.equal((await cli.upload(new Uint8Array(pack(1500)))).rc, 6);
  assert.equal((await cli.upload(new Uint8Array(pack(999)))).rc, 6, "older than the bundled app");
  // wrong key → header refused before any data
  const bad = await cli.upload(new Uint8Array(pack(3000, "k", OTHER)));
  assert.equal(bad.rc, 11);
  // a non-UI frame still reaches the app's MIDI handler behind the filter
  let appGot = 0;
  Object.assign(panel.KWM, { onMidiMessage: () => appGot++ }); // the app's installKwm() does this
  (panel.KWM.onMidiMessage as (m: number[]) => void)([0x90, 60, 64]);
  (panel.KWM.onMidiMessage as (m: number[]) => void)(Array.from(puiRequest(PanelUiOp.Status)));
  assert.equal(appGot, 1);
});

test("losses both ways and a panel page reload mid-transfer: go-back + resume finish the same bytes", async () => {
  const store = new FakeStorage();
  let panel = panelPage(store);
  const relay = new FakeRelay(panel, {
    dropToPanel: (f, i) => f[6] === PanelUiOp.Data && i % 17 === 5,
    dropToPhone: (r, i) => r[6] === 0x72 && i % 23 === 7,
  });
  const cli = new PanelUiClient(relay);
  const file = new Uint8Array(pack(5000, "lossy" + "z".repeat(30000)));
  // reload the panel page (new JS context, same storage) once 40 % has arrived
  let reloaded = false;
  const fin = await cli.upload(file, {
    window: 3,
    ackTimeoutMs: 60,
    maxRetries: 50,
    onProgress: (p) => {
      if (!reloaded && p.phase === "upload" && p.sent > p.total * 0.4) {
        reloaded = true;
        panel = panelPage(store);
        relay.page = panel;
      }
    },
  });
  assert.equal(fin.phase, "done", fin.message);
  assert.ok(reloaded);
  assert.ok(fin.retries >= 0);
  const pkgA = panel.ui.state().slots.A!;
  assert.equal(pkgA.seq, 5000);
  // what is stored opens with the panel's own check
  const b64 = store.getItem("ca79ng.ui.slot.A")!;
  assert.ok(Buffer.from(b64, "base64").equals(Buffer.from(file)), "stored bytes == sent file");
  assert.equal(store.getItem("ca79ng.ui.rx"), null, "staging freed after commit");
});

// ---- frames damaged in transit (field 2026-11-15: 0.2.7, 156 KB, rc 10 "hash mismatch" after 121 s) ----------
// A Data frame that lost (or gained) bytes between the phone and
// the panel still ends in F7; the relay forwards it whole (R1, no content check). The field loader stored whatever
// it decoded at `off` and answered received = off + (shorter length); go-back-N then resent from there, so the
// total came out right and only the Commit hash saw the damage.

/** one BLE continuation packet of the frame lost in the air / in the module (the module reassembles the rest) */
function bleLose(frame: Uint8Array, k: number): Uint8Array {
  const pk = encodeBlePackets([frame], 0, 23);
  const out: Uint8Array[] = [];
  const dec = new BleMidiDecoder((m) => out.push(m));
  pk.forEach((p, i) => i !== k && dec.feed(p));
  assert.equal(out.length, 1, "still one whole SysEx");
  return out[0]!;
}
const cut = (f: Uint8Array, at: number, n = 1) => Uint8Array.from([...f.subarray(0, at), ...f.subarray(at + n)]);
const ins = (f: Uint8Array, at: number, b = 0x00) => Uint8Array.from([...f.subarray(0, at), b, ...f.subarray(at)]);
const flip = (f: Uint8Array, at: number) => {
  const g = Uint8Array.from(f);
  g[at] = g[at]! ^ 0x01; // stays a 7-bit data byte
  return g;
};
/** the receiver as it is on the panel now: uiloader.js of the field USB bundle (record 04 §6.1: dd2a5e1; its
 *  Begin / Data / Commit are unchanged from 404f0d3 up to this fix) — from git, skipped without it */
function fieldLoaderSource(): string | null {
  try {
    return execFileSync("git", ["show", "dd2a5e1:result/app/panelui/uiloader.js"], { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] });
  } catch {
    return null;
  }
}
function fieldPage(storage: FakeStorage, src: string): ReturnType<typeof panelPage> {
  const p = panelPage(storage);
  // the same fake shell, the old receiver in place of the new one
  const ctx: Record<string, unknown> = { ...p.ctx, CA79UI: undefined, KWM: undefined };
  runInNewContext(src.replace(/\/\*UI_CONFIG\*\/[\s\S]*?\/\*END\*\//, `/*UI_CONFIG*/${JSON.stringify(CFG)}/*END*/`), ctx, { filename: "uiloader-field.js" });
  const ui = ctx.CA79UI as Loader;
  ui.navigate = (u) => p.nav.push(u);
  ui.setPage(1);
  const KWM: Record<string, unknown> = { onMidiMessage: () => {} };
  ui.hookKwm(KWM);
  ctx.KWM = KWM;
  return { ...p, ui, ctx, KWM };
}
const isData = (f: Uint8Array) => f[6] === PanelUiOp.Data;
/** damage the n-th Data frame the phone sends (every Data frame counts, resends too) with `fn`, once */
function damageNth(n: number, fn: (f: Uint8Array) => Uint8Array) {
  let seen = 0;
  return (f: Uint8Array) => (isData(f) && seen++ === n ? fn(f) : f);
}
const stored = (store: FakeStorage, slot: string) => Buffer.from(store.getItem(`ca79ng.ui.slot.${slot}`) ?? "", "base64");

test("damaged Data frames (byte lost / BLE packet lost / byte added / byte changed) never reach the stored package", async () => {
  const file = new Uint8Array(pack(6000, "dmg" + randomBytes(4000).toString("hex")));
  // frame 6 lost and frame 7's offset bytes changed into frame 6's: it arrives exactly at `received`, 200 B long
  const offsetInto6 = () => {
    let seen = 0;
    return (f: Uint8Array) => {
      if (!isData(f)) return f;
      const i = seen++;
      if (i === 6) return null;
      if (i !== 7) return f;
      const g = Uint8Array.from(f);
      const off = ((g[7]! << 14) | (g[8]! << 7) | g[9]!) - PUI_CHUNK;
      g.set([(off >> 14) & 0x7f, (off >> 7) & 0x7f, off & 0x7f], 7);
      return g;
    };
  };
  const cases: [string, (f: Uint8Array) => Uint8Array | null][] = [
    ["one byte lost mid-frame", damageNth(6, (f) => cut(f, 120))],
    ["one BLE continuation packet lost", damageNth(6, (f) => bleLose(f, 4))],
    ["one byte added mid-frame", damageNth(6, (f) => ins(f, 77))],
    ["one data byte changed", damageNth(6, (f) => flip(f, 150))],
    ["a frame lost, the next one's offset changed into it", offsetInto6()],
  ];
  for (const [name, mangle] of cases) {
    const store = new FakeStorage();
    const panel = panelPage(store);
    const relay = new FakeRelay(panel, { mangleToPanel: mangle });
    const fin = await new PanelUiClient(relay).upload(file, { window: 2, ackTimeoutMs: 80 });
    assert.equal(fin.phase, "done", `${name}: ${fin.message}`);
    assert.ok(stored(store, "A").equals(Buffer.from(file)), `${name}: stored bytes == sent file`);
    assert.equal(fin.restarts ?? 0, 0, `${name}: the loader refused the frame, no resend from 0`);
    assert.equal(panel.ui.state().refused, 1, `${name}: one frame refused`);
  }
});

test("frame check: CRC frames only when the loader offers them in its Begin reply; plain frames are length-checked", async () => {
  const file = new Uint8Array(pack(6100, "crc" + randomBytes(1500).toString("hex")));
  // new loader + plain frames (the app before this fix): a short frame is refused, not stored
  let store = new FakeStorage();
  let relay = new FakeRelay(panelPage(store), { mangleToPanel: damageNth(3, (f) => cut(f, 60)) });
  let fin = await new PanelUiClient(relay).upload(file, { crc: false, ackTimeoutMs: 80 });
  assert.equal(fin.phase, "done", fin.message);
  assert.equal(relay.maxFrame, 240, "plain frames");
  assert.ok(stored(store, "A").equals(Buffer.from(file)));
  // new loader, new sender: 204 raw bytes per frame (data 200 + CRC-32) = 245 B on the wire
  store = new FakeStorage();
  relay = new FakeRelay(panelPage(store));
  fin = await new PanelUiClient(relay).upload(file);
  assert.equal(fin.phase, "done", fin.message);
  assert.equal(relay.maxFrame, 245);
  assert.ok(stored(store, "A").equals(Buffer.from(file)));
  const pl = parsePackage(file)!.payload;
  assert.equal(dataFrame(200, pl.subarray(200, 400), true).length, 245);
  assert.equal(dataFrame(200, pl.subarray(200, 400)).length, 240);
});

test("field loader (as on the panel now) + this sender: a short / long frame is caught by the sender and resent from 0; rc 10 is resent", async (t) => {
  const src = fieldLoaderSource();
  if (!src) return t.skip("git history of uiloader.js not available");
  const file = new Uint8Array(pack(6200, "fld" + randomBytes(4000).toString("hex")));
  // the field failure itself (the sender's new checks off): everything received, hash mismatch at Commit
  {
    const store = new FakeStorage();
    const relay = new FakeRelay(fieldPage(store, src), { mangleToPanel: damageNth(6, (f) => cut(f, 120)) });
    const fin = await new PanelUiClient(relay).upload(file, { maxRestarts: 0, ackTimeoutMs: 80 });
    assert.equal(fin.rc, 10, `the field failure: ${fin.message}`);
  }
  // a frame that came out shorter / longer: the panel's `received` falls off the frame grid → Control 4 → from 0
  const shapes: [string, (f: Uint8Array) => Uint8Array][] = [
    ["byte lost", (f) => cut(f, 120)],
    ["BLE packet lost", (f) => bleLose(f, 5)],
    ["byte added", (f) => ins(f, 50)],
  ];
  for (const [name, fn] of shapes) {
    const store = new FakeStorage();
    const relay: FakeRelay = new FakeRelay(fieldPage(store, src), { mangleToPanel: damageNth(6, fn) });
    const msgs: string[] = [];
    const fin: UploadProgress = await new PanelUiClient(relay).upload(file, { ackTimeoutMs: 80, onProgress: (p) => void (p.message && msgs.push(p.message)) });
    assert.equal(fin.phase, "done", `${name}: ${fin.message}`);
    assert.equal(fin.restarts, 1, name);
    assert.equal(relay.maxFrame, 240, `${name}: no CRC frames to the old loader`);
    assert.ok(stored(store, "A").equals(Buffer.from(file)), `${name}: stored bytes == sent file`);
    assert.ok(msgs.some((m) => /damaged/.test(m)), `${name}: ${msgs.join(" | ")}`);
  }
  // a changed byte keeps the length: only the Commit hash sees it → the whole package once more
  {
    const store = new FakeStorage();
    const relay = new FakeRelay(fieldPage(store, src), { mangleToPanel: damageNth(6, (f) => flip(f, 150)) });
    const fin = await new PanelUiClient(relay).upload(file, { ackTimeoutMs: 80 });
    assert.equal(fin.phase, "done", fin.message);
    assert.equal(fin.restarts, 1);
    assert.ok(stored(store, "A").equals(Buffer.from(file)));
  }
  // damaged every time: gives up after maxRestarts with the panel's rc
  {
    const store = new FakeStorage();
    const at1200 = (f: Uint8Array) => isData(f) && ((f[7]! << 14) | (f[8]! << 7) | f[9]!) === 1200; // the frame at byte 1200, every time
    const relay = new FakeRelay(fieldPage(store, src), { mangleToPanel: (f) => (at1200(f) ? flip(f, 150) : f) });
    const fin = await new PanelUiClient(relay).upload(file, { ackTimeoutMs: 80, maxRestarts: 1 });
    assert.equal(fin.phase, "error");
    assert.equal(fin.rc, 10);
    assert.equal(fin.restarts, 1);
  }
});

test("slots: the newest confirmed version is never overwritten; rollback / forget / clear via control ops", async () => {
  const store = new FakeStorage();
  const panel = panelPage(store);
  const cli = new PanelUiClient(new FakeRelay(panel));
  assert.equal((await cli.upload(new Uint8Array(pack(2001)))).message, "A");
  // boot A once and confirm it (it reached the piano)
  const p2 = panelPage(store);
  bootIn(p2, "A");
  p2.ui.confirm();
  assert.equal(p2.ui.state().slots.A!.state, "ok");
  assert.equal((await cli.upload(new Uint8Array(pack(2002)))).message, "B");
  assert.equal((await cli.upload(new Uint8Array(pack(2003)))).message, "B", "A (confirmed) kept as the fallback");
  assert.equal(await cli.control(PuiControl.Rollback), 0);
  assert.equal(panel.ui.state().slots.B!.state, "bad");
  assert.equal(await cli.control(PuiControl.ClearBad), 0);
  assert.equal(panel.ui.state().slots.B!.state, "new");
  assert.equal(await cli.control(PuiControl.Forget), 0);
  assert.equal(Object.keys(panel.ui.state().slots).length, 0);
  assert.equal(panel.ui.state().maxSeq, 2003, "anti-rollback survives forget");
  assert.equal(await cli.control(99), 1);
  // activate: reply first, then the page reloads into ng/index.html
  assert.equal(await cli.control(PuiControl.Activate), 0);
  await new Promise((r) => setTimeout(r, 350));
  assert.deepEqual(panel.nav, ["index.html?embedded=1"]);
});

test("storage quota: a package that does not fit is refused at commit, the old slots stay", async () => {
  const store = new FakeStorage(40_000);
  const panel = panelPage(store);
  const cli = new PanelUiClient(new FakeRelay(panel));
  assert.equal((await cli.upload(new Uint8Array(pack(2100)))).phase, "done");
  const big = makePackage({ js: randomBytes(40000).toString("base64"), css: "" }, { version: "0.9.0", seq: 2101, key: KEY });
  const r = await cli.upload(new Uint8Array(big));
  assert.equal(r.rc, 13, r.message);
  assert.equal(panel.ui.state().slots.A!.seq, 2100);
  assert.ok(panel.ui.probeQuota(200_000) > 0);
});

// ---- boot ---------------------------------------------------------------------------------

/** Boot the loader in jsdom (ng/index.html shape) and say what ran. */
function bootIn(p: ReturnType<typeof panelPage>, expect: string) {
  // the vm page has no DOM: give boot() a minimal document
  const appended: { tag: string; text?: string; src?: string }[] = [];
  const mk = (tag: string) => ({ tag, set text(v: string) { this._t = v; }, _t: "", textContent: "", src: "", href: "", rel: "", onerror: null });
  (p.ctx as Record<string, unknown>).document = {
    head: { appendChild: (e: { tag: string; _t: string; src: string }) => appended.push({ tag: e.tag, text: e._t, src: e.src }) },
    body: { appendChild: (e: { tag: string; _t: string; src: string }) => appended.push({ tag: e.tag, text: e._t, src: e.src }) },
    createElement: mk,
  };
  const r = p.ui.boot({ js: "app-1.js", css: "styles-1.css" });
  assert.equal(r, expect);
  return appended;
}

test("boot order: newest stored by seq, bad ones skipped, three unconfirmed boots → bad, then bundled", async () => {
  const store = new FakeStorage();
  const cli = new PanelUiClient(new FakeRelay(panelPage(store)));
  await cli.upload(new Uint8Array(pack(3001, "A1")));
  await cli.upload(new Uint8Array(pack(3002, "B1")));
  let p = panelPage(store);
  let els = bootIn(p, "B");
  assert.match(els.find((e) => e.tag === "script")!.text!, /B1/, "stored JS injected inline");
  assert.equal(p.ui.state().slots.B!.tries, 1);
  // a script error before boot marks B bad and reloads the host page → A
  p.ui.fail("error");
  assert.deepEqual(p.nav, ["index.html?embedded=1"]);
  assert.equal(p.ui.state().slots.B!.state, "bad");
  p = panelPage(store);
  bootIn(p, "A");
  // A never confirms: boots 2 and 3 still try it, the 4th gives up
  bootIn(panelPage(store), "A");
  bootIn(panelPage(store), "A");
  els = bootIn(panelPage(store), "bundled");
  assert.deepEqual(els.map((e) => (e.tag === "script" ? e.src : e.tag)), ["link", "app-1.js"]);
  assert.equal(panelPage(store).ui.state().slots.A!.why, "tries");
  // the bundled app failing goes back to the chooser (→ Kawai UI)
  const pb = panelPage(store);
  bootIn(pb, "bundled");
  pb.ui.fail("timeout");
  assert.deepEqual(pb.nav.map((u) => u.split("&detail=")[0]), ["../index.html?reason=timeout"]);
});

test("boot: a corrupted stored slot fails its check and falls through; noconnect spares a confirmed version", async () => {
  const store = new FakeStorage();
  const cli = new PanelUiClient(new FakeRelay(panelPage(store)));
  await cli.upload(new Uint8Array(pack(4001)));
  await cli.upload(new Uint8Array(pack(4002)));
  // corrupt B's stored bytes
  const k = "ca79ng.ui.slot.B";
  const raw = Buffer.from(store.getItem(k)!, "base64");
  raw[raw.length - 1] ^= 1;
  store.setItem(k, raw.toString("base64"));
  let p = panelPage(store);
  bootIn(p, "A");
  assert.match(p.ui.state().slots.B!.why!, /verify 10/);
  p.ui.confirm();
  // confirmed A loses the piano later → chooser, A stays ok
  (p.ctx as { __ca79ngBooted?: boolean }).__ca79ngBooted = true;
  p.ui.fail("noconnect");
  assert.deepEqual(p.nav.map((u) => u.split("&detail=")[0]), ["../index.html?reason=noconnect"]); // + detail · ua for the chooser
  assert.equal(p.ui.state().slots.A!.state, "ok");
  // errors after the app booted are not the loader's business
  p = panelPage(store);
  bootIn(p, "A");
  (p.ctx as { __ca79ngBooted?: boolean }).__ca79ngBooted = true;
  p.ui.fail("error");
  assert.deepEqual(p.nav, []);
  // src=bundled forces the bundled app
  p = panelPage(store);
  (p.ctx.location as { search: string }).search = "?embedded=1&src=bundled";
  bootIn(p, "bundled");
  // a newer USB bundle (higher seq) wins over older stored slots
  const p5 = panelPage(store, uiConfig(KEY, { version: "0.5.0", seq: 5000 }));
  bootIn(p5, "bundled");
});

test("boot drops taps queued while the page was loading; activate covers the old page with a notice", async () => {
  // field 2026-11-02 (record 04 §5): the panel keeps the old frame on screen while the next page loads and
  // starts (seconds); taps made then are queued and reach the NEW page before its first frame
  const store = new FakeStorage();
  await new PanelUiClient(new FakeRelay(panelPage(store))).upload(new Uint8Array(pack(7001, "G1")));
  const dom = new JSDOM("<!doctype html><body><button id=b>b</button></body>", {
    url: "file:///data/data/x/files/appUI/ng/index.html?embedded=1", runScripts: "outside-only", pretendToBeVisual: true,
  });
  const w = dom.window as unknown as Window & { CA79UI: Loader & { inputGuardMs: number; _guard: { active(): boolean } }; eval(s: string): unknown };
  try {
    Object.defineProperty(w, "localStorage", { value: store, configurable: true });
    w.eval(loaderSource(CFG));
    const ui = w.CA79UI;
    const nav: string[] = [];
    ui.navigate = (u) => nav.push(u);
    ui.inputGuardMs = 50;
    let taps = 0;
    w.document.getElementById("b")!.addEventListener("click", () => taps++);
    assert.equal(ui.boot({ js: "app-1.js", css: "styles-1.css" }), "A");
    assert.match(w.document.body.textContent!, /G1/, "stored app injected");
    (w.document.getElementById("b") as HTMLButtonElement).click(); // queued during the boot freeze
    assert.equal(taps, 0, "a tap before the first frame is swallowed");
    assert.equal(ui._guard.active(), true);
    await new Promise((r) => setTimeout(r, 150)); // first frame (jsdom rAF ~16 ms) + guard
    assert.equal(ui._guard.active(), false);
    (w.document.getElementById("b") as HTMLButtonElement).click();
    assert.equal(taps, 1, "later taps pass");
    // Control 1: the reply goes out, the old page shows what is happening, then the page reloads
    const K: Record<string, unknown> = { onMidiMessage: () => {} };
    ui.hookKwm(K);
    (K.onMidiMessage as (m: number[], id: string) => void)(Array.from(puiRequest(PanelUiOp.Control, [PuiControl.Activate])), "USB");
    assert.match(w.document.getElementById("ca79ui-notice")!.textContent!, /starting the new UI/);
    await new Promise((r) => setTimeout(r, 350));
    assert.deepEqual(nav, ["index.html?embedded=1"]);
  } finally {
    dom.window.close();
  }
});

test("activate that does not reload (field 11-15: Android Settings on top): the notice comes down, the reload is retried when the page is shown", async () => {
  // record 04_panel_ui_remote, last lines: stored with "start right away" while the shell's Settings activity was on
  // top → the page kept 「새 UI 시작 중」 for 43 min and op 0x37 taps changed nothing
  const store = new FakeStorage();
  await new PanelUiClient(new FakeRelay(panelPage(store))).upload(new Uint8Array(pack(7201, "H1")));
  const dom = new JSDOM("<!doctype html><body><button id=b>b</button></body>", {
    url: "file:///data/data/x/files/appUI/ng/index.html?embedded=1", runScripts: "outside-only", pretendToBeVisual: true,
  });
  const w = dom.window as unknown as Window & { CA79UI: Loader & { inputGuardMs: number; activateTimeoutMs: number; activateWatch?: number }; eval(s: string): unknown };
  let vis: "visible" | "hidden" = "visible";
  Object.defineProperty(w.document, "visibilityState", { get: () => vis, configurable: true });
  const show = (v: "visible" | "hidden") => { vis = v; w.document.dispatchEvent(new dom.window.Event("visibilitychange")); };
  const noticeOn = () => !!w.document.getElementById("ca79ui-notice");
  try {
    Object.defineProperty(w, "localStorage", { value: store, configurable: true });
    w.eval(loaderSource(CFG));
    const ui = w.CA79UI;
    assert.equal(ui.activateWatch, 1, "the app's own watch (src/ui/activatewatch.ts) stands down for this loader");
    const nav: string[] = [];
    const events: string[] = [];
    ui.navigate = (u) => nav.push(u); // the navigation never takes effect: the page stays
    ui.on((k) => events.push(k));
    ui.inputGuardMs = 10;
    ui.activateTimeoutMs = 120;
    assert.equal(ui.boot({ js: "app-1.js", css: "styles-1.css" }), "A");
    await new Promise((r) => setTimeout(r, 60));
    let taps = 0;
    w.document.getElementById("b")!.addEventListener("click", () => taps++);
    // shown page: notice + navigate, then the watch takes the notice down
    assert.equal(ui.control(PuiControl.Activate), 0);
    assert.equal(noticeOn(), true);
    await new Promise((r) => setTimeout(r, 350));
    assert.deepEqual(nav, ["index.html?embedded=1"]);
    assert.equal(noticeOn(), false, "no reload within activateTimeoutMs: the notice is gone");
    assert.ok(events.includes("activate-stalled"));
    (w.document.getElementById("b") as HTMLButtonElement).click();
    assert.equal(taps, 1, "the old UI is usable again");
    // shown again later: the reload is tried again
    show("hidden");
    assert.equal(nav.length, 1);
    show("visible");
    assert.equal(noticeOn(), true);
    await new Promise((r) => setTimeout(r, 350));
    assert.deepEqual(nav, ["index.html?embedded=1", "index.html?embedded=1"]);
    await new Promise((r) => setTimeout(r, 100));
    assert.equal(noticeOn(), false);
    // a page that is not shown (another activity on top) when Control 1 comes: no notice, no navigate until shown
    show("hidden");
    nav.length = 0;
    events.length = 0;
    assert.equal(ui.control(PuiControl.Activate), 0, "the reply still goes out");
    await new Promise((r) => setTimeout(r, 400));
    assert.deepEqual(nav, []);
    assert.equal(noticeOn(), false);
    assert.ok(events.includes("activate-deferred"));
    show("visible");
    assert.equal(noticeOn(), true);
    await new Promise((r) => setTimeout(r, 350));
    assert.deepEqual(nav, ["index.html?embedded=1"]);
    // that one stalled too (third stall): one more try at the next show, then it is left for the next boot
    await new Promise((r) => setTimeout(r, 100));
    show("hidden");
    show("visible");
    await new Promise((r) => setTimeout(r, 450));
    assert.equal(nav.length, 2);
    assert.equal(noticeOn(), false);
    show("hidden");
    show("visible");
    await new Promise((r) => setTimeout(r, 450));
    assert.equal(nav.length, 2, "after activateRetries stalls: no more tries (the next boot starts the new version)");
    assert.equal(noticeOn(), false);
  } finally {
    dom.window.close();
  }
});

test("backlight: boot, activate and the chooser turn it on at the app's brightness (field 11-03 dark probe)", async () => {
  // record 04 §6: the app's auto display off had set the backlight to 0; the next version (the probe) never set it
  const store = new FakeStorage();
  await new PanelUiClient(new FakeRelay(panelPage(store))).upload(new Uint8Array(pack(7101)));
  let p = panelPage(store);
  bootIn(p, "A");
  assert.deepEqual(p.shell, ['setLcdBacklight {"brightness":80}'], "default brightness 8 → 80 %");
  store.setItem("ca79ng.settings", JSON.stringify({ brightness: 3 }));
  p = panelPage(store);
  bootIn(p, "A");
  assert.deepEqual(p.shell, ['setLcdBacklight {"brightness":30}'], "the app's own setting");
  p.shell.length = 0;
  p.ui.control(PuiControl.Activate);
  assert.deepEqual(p.shell, ['setLcdBacklight {"brightness":30}'], "activate lights the page being left too");
  const shell: string[] = [];
  const dom = new JSDOM(chooserHtml({ version: "0.2.0", built: "t", kawai: "1.3.10" }, CFG).toString(), {
    url: "file:///data/data/x/files/appUI/index.html?stay=1&reason=error",
    runScripts: "dangerously",
    beforeParse(w) {
      (w as unknown as Record<string, unknown>).kwmcore = { postMessage: (u: string) => shell.push(u.slice(10, u.indexOf("?") - 1)) };
    },
  });
  try {
    assert.equal(shell[0], "setLcdBacklight", "the chooser (fallback screen) lights the panel first");
  } finally {
    dom.window.close();
  }
});

test("probe package under the real loader: draws, lights the panel, answers Check, follows a remote roll back", async () => {
  const store = new FakeStorage();
  await new PanelUiClient(new FakeRelay(panelPage(store))).upload(new Uint8Array(makePackage(probeFiles("0.2.2"), { version: "0.2.2", seq: 7201, key: KEY })));
  const shell: string[] = [];
  const reloads: string[] = [];
  const vc = new VirtualConsole();
  vc.on("jsdomError", (e) => reloads.push(String(e.message))); // location.reload(): "Not implemented: navigation"
  const dom = new JSDOM("<!doctype html><head></head><body></body>", {
    url: "file:///data/data/x/files/appUI/ng/index.html?embedded=1", runScripts: "dangerously", pretendToBeVisual: true, virtualConsole: vc,
  });
  const w = dom.window as unknown as Window & { CA79UI: Loader & { inputGuardMs: number }; eval(s: string): unknown; __ca79ngBooted?: boolean };
  try {
    Object.defineProperty(w, "localStorage", { value: store, configurable: true });
    (w as unknown as Record<string, unknown>).kwmcore = {
      postMessage(u: string) {
        const cmd = u.substring(10, u.indexOf("?") - 1);
        if (cmd !== "sendMidiMessage") shell.push(`${cmd} ${u.slice(u.indexOf("?") + 1)}`);
      },
    };
    w.eval(loaderSource(CFG));
    const ui = w.CA79UI;
    ui.navigate = () => {};
    const K: Record<string, unknown> = { onMidiMessage: () => {} };
    ui.hookKwm(K);
    assert.equal(ui.boot({ js: "app-1.js", css: "styles-1.css" }), "A");
    assert.equal(w.__ca79ngBooted, true, "booted after it drew");
    assert.match(w.document.getElementById("probe")!.textContent!, /panel probe 0\.2\.2[\s\S]*"running":"A"/);
    assert.ok(w.document.getElementById("probe-back"));
    assert.deepEqual(shell, ['setLcdBacklight {"brightness":80}', 'setLcdBacklight {"brightness":80}'], "loader and probe both light the panel");
    // the app's Check: the loader under the probe answers (running = slot A)
    const sent: number[][] = [];
    (w as unknown as Record<string, unknown>).kwmcore = { postMessage: (u: string) => { if (u.startsWith("kwmcore://sendMidiMessage/")) sent.push(JSON.parse(u.slice(u.indexOf("?") + 1)).msg); } };
    (K.onMidiMessage as (m: number[], id: string) => void)(Array.from(puiRequest(PanelUiOp.Status)), "USB");
    const st = decodePanelStatus(Uint8Array.from(sent[0]!.slice(7, -1)))!;
    assert.equal(st.running, 2, "slot A");
    assert.equal(st.slots[0].version, "0.2.2");
    // the app's Roll back (Control 2): the probe sees its slot bad and reloads → the loader boots the previous one
    (K.onMidiMessage as (m: number[], id: string) => void)(Array.from(puiRequest(PanelUiOp.Control, [PuiControl.Rollback])), "USB");
    assert.equal(ui.state().slots.A!.state, "bad");
    await new Promise((r) => setTimeout(r, 1200));
    assert.ok(reloads.some((m) => /navigation/i.test(m)), `reloaded: ${reloads.join(" | ")}`);
  } finally {
    dom.window.close();
  }
});

test("probe package: a throw before it drew is not swallowed (no boot mark → the loader falls back) and is shown", () => {
  const { js } = probeFiles("0.2.2");
  const vc = new VirtualConsole();
  const w = new JSDOM("<!doctype html><body></body>", { runScripts: "outside-only", virtualConsole: vc }).window as unknown as Window & { eval(s: string): unknown; __ca79ngBooted?: boolean };
  try {
    const real = w.document.body.appendChild.bind(w.document.body);
    let n = 0;
    w.document.body.appendChild = (<T extends Node>(e: T): T => { if (n++ === 1) throw new Error("boom"); return real(e); }) as typeof w.document.body.appendChild;
    assert.throws(() => w.eval(js), /boom/);
    assert.equal(w.__ca79ngBooted, undefined, "not booted: the loader's window error handler marks the slot bad");
    assert.match(w.document.body.textContent!, /probe error: Error: boom/);
  } finally {
    w.close();
  }
});

test("built pages: chooser and ng/index.html carry the loader; the chooser answers relayed ops", async () => {
  const files = await buildApp("0.2.0", CFG);
  const html = files.find((f) => f.path === "ng/index.html")!.data.toString();
  assert.match(html, /CA79UI\.boot\(\{ js: "app-[A-Z0-9]+\.js", css: "styles-[0-9A-F]+\.css" \}\)/);
  assert.match(html, new RegExp(`"pub":"${Buffer.from(KEY.pub).toString("hex")}"`));
  const chooser = chooserHtml({ version: "0.2.0", built: "t", kawai: "1.3.10" }, CFG).toString();
  const sent: number[][] = [];
  const dom = new JSDOM(chooser, {
    url: "file:///data/data/x/files/appUI/index.html?stay=1",
    runScripts: "dangerously",
    beforeParse(w) {
      (w as unknown as Record<string, unknown>).kwmcore = {
        postMessage(url: string) {
          if (url.startsWith("kwmcore://sendMidiMessage/")) sent.push(JSON.parse(url.slice(url.indexOf("?") + 1)).msg);
        },
      };
    },
  });
  try {
    const w = dom.window as unknown as { KWM: { onMidiMessage(m: number[], id: string): void }; CA79UI: Loader };
    assert.ok(w.CA79UI);
    w.KWM.onMidiMessage(Array.from(puiRequest(PanelUiOp.Status)), "USB");
    assert.equal(sent.length, 1);
    const st = decodePanelStatus(Uint8Array.from(sent[0]!.slice(7, -1)))!;
    assert.equal(st.page, 0, "chooser");
    assert.equal(st.hasKey, true);
    assert.match(dom.window.document.getElementById("rstate")!.textContent!, /없음 · none/);
    // a Begin stops nothing on stay=1 but opens the recovery block and shows progress
    const file = parsePackage(new Uint8Array(pack(6000)))!;
    const { pack7 } = await import("../src/protocol/ngext.ts");
    w.KWM.onMidiMessage(Array.from(puiRequest(PanelUiOp.Begin, pack7(file.head))), "USB");
    assert.equal((dom.window.document.getElementById("rec") as HTMLDetailsElement).open, true);
    assert.match(dom.window.document.getElementById("msg")!.textContent!, /receiving 0\//);
    const n0 = Math.min(PUI_CHUNK, file.payload.length); // a Data frame carries exactly min(200, rest) bytes
    w.KWM.onMidiMessage(Array.from(dataFrame(0, file.payload.subarray(0, n0))), "USB");
    assert.match(dom.window.document.getElementById("msg")!.textContent!, new RegExp(`receiving ${n0}/`));
    await new Promise((r) => setTimeout(r, 20)); // let the details toggle event run
  } finally {
    dom.window.close();
  }
});

test("BLE transfer estimate for the real app bundle (MTU 23, paced writer)", async () => {
  const files = await buildApp("0.2.0");
  const js = files.find((f) => f.path.endsWith(".js"))!.data.toString();
  const css = files.find((f) => f.path.endsWith(".css"))!.data.toString();
  const file = makePackage({ js, css }, { version: "0.2.0", seq: 7000, key: KEY });
  const e = estimateTransfer(file.length);
  // every frame ≤ 240 B; ~19 B of SysEx per 20 B BLE packet
  assert.ok(e.frameBytes / e.frames <= 240);
  assert.ok(e.blePacketBytes / e.frameBytes < 1.12 && e.blePacketBytes / e.frameBytes > 1.0);
  assert.ok(e.windowedS < e.stopAndWaitS);
  // 150 000 -> 151 000 (2026-11-09, 1D 0B 「피아노 BT 입력」 line: HEAD was 149 824 B, the line adds ~470 B deflated)
  // 151 000 -> 153 000 (2026-11-10, 1D 0C 「PC 연결(AVRCP 브라우징)」 card with its confirmation in en/ja/ko: 152 328 B)
  // 153 000 -> 156 000 (2026-11-11, 1D 0D 「소리 경로 사건(모듈 오디오 설정)」 card, three actions + confirmation in en/ja/ko: 155 393 B)
  // 156 000 -> 158 000 (2026-11-11, late-stop toggle 「PC가 소리를 늦게 놓으면 피아노가 멈춤(실험)」 + its line in en/ja/ko: 156 530 B)
  // 158 000 -> 161 000 (2026-11-15, 「GPIO 읽기」 developer view (op 0x17) + its pin names
  // and lines in en/ja/ko: 160 608 B)
  // 161 000 -> 162 000 (2026-11-15, panel 「Android 설정」 + 「USB 메모리 열기」 · probe-APK buttons (openURL) and their lines in en/ja/ko: 161 146 B)
  // 162 000 -> 163 000 (2026-11-15, panel 「이 주소 열기」 URI picker + 「Android 탐색」 file reader
  // and their lines in en/ja/ko: 162 311 B)
  // 163 000 -> 164 000 (2026-11-15, op 0x37 Act in the screen reporter, KSO song backup button, probe-row CSS: 163 075 B)
  // 164 000 -> 165 000 (2026-11-15, in-page two-step confirm instead of window.confirm (ui/confirm2.ts) + the loader
  // activate watch for older loaders (ui/activatewatch.ts): 164 093 B)
  // 165 000 -> 166 000 (2026-11-16, window.__H test helpers (src/testhooks.ts) + fileaccess head() Cancel + Ack, the
  // empty-packet rule and the 1 MB write guard: 165 260 B)
  // 166 000 -> 167 000 (2026-11-16, ngver.gen.ts feature table + ngFeature: HEAD was
  // 165 944 B, this 166 185 B)
  // 167 000 -> 173 500 (2026-11-17, OTA loader v4: status layout 3, slot info / loader confirm / reselect, the loader
  // rows and the rc 22-25 texts in en/ja/ko (non-ASCII strings are \u-escaped in the bundle): HEAD was 165 992 B,
  // this 172 766 B)
  // 173 500 -> 175 500 (2026-11-17, fast OTA upload: v2 frames, LZ encoder / decoder, windowed sender in ota.ts:
  // 174 626 B)
  // 175 500 -> 176 500 (2026-11-19, styles.css detail pass: header outline controls, segmented parts, chip line,
  // list rows: 175 654 B)
  // 176 500 -> 177 500 (2026-11-19, ui/picker.ts: the desktop app's Bluetooth search inside the connection dialog -
  // the picker is shared with the panel, which never runs that branch: 176 596 B)
  assert.ok(file.length < 177_500, `package ${file.length} B`);
  console.log(`# real bundle: package ${file.length} B, ${e.frames} frames, ${e.blePackets} BLE packets; window ≈ ${e.windowedS.toFixed(0)} s, stop-and-wait ≈ ${e.stopAndWaitS.toFixed(0)} s`);
});
