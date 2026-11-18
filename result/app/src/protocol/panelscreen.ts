// What the built-in panel shows, read from a phone / PC (REMOTE_PROTOCOL.md §10.7 ops 0x35 Capture / 0x36 Read). The panel side is panelui/panelscreen.js (ES5, in the USB
// bundle pages and in every remote package's js); the piano only relays (ops 0x30-0x3F, cave relay_cave.c).
//
// Capture asks the panel page to make a report (kind 1, UTF-8 JSON ≤ 1 KiB by default) or a snapshot (kind 2, its
// DOM painted on a canvas, JPEG ≈ 24 KiB at 240 px wide); the answer is its length + CRC-32 (rc 3 = still drawing:
// ask again with the same tag). Read pulls it in 200 B pieces, a window of 2 in flight, go-back-N on a timeout —
// the UI-package flow (PanelUiClient.upload) in reverse.
import { crc32, pack7, unpack7 } from "./ngext.ts";
import { parsePuiReply, puiRequest, type PuiPort } from "./panelui.ts";

export const PanelScreenOp = { Capture: 0x35, Read: 0x36, Act: 0x37 } as const;
export const PS_KIND = { Report: 1, Snapshot: 2 } as const;
export const PS_FMT: Record<number, string> = { 1: "application/json", 2: "image/jpeg", 3: "image/png" };
export const PS_RC: Record<number, string> = {
  0: "ok", 1: "bad arguments", 3: "busy (still drawing)", 5: "offset past the end", 8: "no such capture (the panel page reloaded?)",
  13: "not supported on this page (no canvas)", 0x7f: "panel error",
};
/** raw bytes per Read reply: F0 + tag 5 + op + pack7(6 + 200) = 236 + F7 → 244 B, under the relay's 256 B */
export const PS_CHUNK = 200;
/** the field rate of the relay (11-02: 118,155 B package in 82.8 s, window 2) */
export const RELAY_FIELD_BPS = 118155 / 82.8;

const u32 = (b: Uint8Array, o: number) => (b[o]! | (b[o + 1]! << 8) | (b[o + 2]! << 16) | (b[o + 3]! << 24)) >>> 0;
const u16 = (b: Uint8Array, o: number) => b[o]! | (b[o + 1]! << 8);

export interface CaptureInfo {
  rc: number;
  tag: number;
  kind: number;
  fmt: number;
  length: number;
  crc: number;
  width: number;
  height: number;
  /** panel time to make it (report: build, snapshot: paint + encode), ms */
  ms: number;
}
export function decodeCapture(data: Uint8Array): CaptureInfo | null {
  const r = unpack7(data);
  if (r.length < 4) return null;
  return {
    rc: r[0]!, tag: r[1]!, kind: r[2]!, fmt: r[3]!, length: r.length >= 8 ? u32(r, 4) : 0, crc: r.length >= 12 ? u32(r, 8) : 0,
    width: r.length >= 14 ? u16(r, 12) : 0, height: r.length >= 16 ? u16(r, 14) : 0, ms: r.length >= 18 ? u16(r, 16) : 0,
  };
}
export interface ReadReply {
  rc: number;
  tag: number;
  off: number;
  data: Uint8Array;
}
export function decodeRead(data: Uint8Array): ReadReply | null {
  const r = unpack7(data);
  if (r.length < 6) return null;
  return { rc: r[0]!, tag: r[1]!, off: u32(r, 2), data: r.slice(6) };
}
/** Capture: kind, tag 1..127, p1 (report: max size / 64 B · snapshot: width / 8 px), p2 (snapshot: target KiB) */
export function captureRequest(kind: number, tag: number, p1 = 0, p2 = 0): Uint8Array {
  return puiRequest(PanelScreenOp.Capture, [kind, tag, p1, p2]);
}
/** Act (panelscreen.js v2): click (1) or set the value (2) of the element with this id on the panel page */
export function actRequest(tag: number, action: 1 | 2, id: string, value = ""): Uint8Array {
  const ascii = (t: string) => [...t].map((c) => c.charCodeAt(0) & 0x7f);
  return puiRequest(PanelScreenOp.Act, [tag, action, ...ascii(id), ...(action === 2 ? [0, ...ascii(value)] : [])]);
}
/** Read: tag, 21-bit offset, at most n bytes (1..200, 0 = 200) */
export function readRequest(tag: number, off: number, n = PS_CHUNK): Uint8Array {
  return puiRequest(PanelScreenOp.Read, [tag, (off >> 14) & 0x7f, (off >> 7) & 0x7f, off & 0x7f, n >= PS_CHUNK ? 0 : n]);
}

export interface ScreenProgress {
  phase: "capture" | "read" | "done" | "error";
  got: number;
  total: number;
  retries: number;
  message?: string;
}
export interface ScreenOptions {
  /** Read requests in flight (default 2, like a UI package) */
  window?: number;
  /** bytes per Read reply asked for (default 200); halved down to 50 after two timeouts in a row */
  chunk?: number;
  ackTimeoutMs?: number;
  /** timeouts in a row before giving up */
  maxRetries?: number;
  /** how long the panel may draw (rc 3 polling) */
  captureTimeoutMs?: number;
  onProgress?: (p: ScreenProgress) => void;
}
export interface ScreenResult {
  ok: boolean;
  error?: string;
  info?: CaptureInfo;
  bytes?: Uint8Array;
  /** wall time of capture + transfer, ms */
  ms: number;
  /** transfer only (Read), ms */
  readMs: number;
  frames: number;
  retries: number;
  chunk: number;
}
export interface PanelReport {
  v: number;
  pg: string;
  ver?: string;
  tab?: string;
  con?: string;
  bl?: number | null;
  dim?: boolean;
  off?: number | null;
  ld?: string;
  ua?: string;
  vp?: string;
  err?: string[];
  js?: string[];
  t?: string[];
  cut?: number;
  [k: string]: unknown;
}

export function toBase64(b: Uint8Array): string {
  let s = "";
  for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode(...b.subarray(i, i + 0x8000));
  return btoa(s);
}
export function dataUrl(bytes: Uint8Array, fmt: number): string {
  return `data:${PS_FMT[fmt] ?? "application/octet-stream"};base64,${toBase64(bytes)}`;
}

/** Report text as lines, for the screen and logs. */
export function reportLines(r: PanelReport): string[] {
  const head = [
    `${r.pg}${r.ver ? ` ${r.ver}` : ""}${r.tab ? ` · tab ${r.tab}` : ""}${r.con ? ` · ${r.con}` : ""}`,
    `backlight ${r.bl ?? "?"}%${r.dim ? " (display off)" : ""} · auto off ${r.off ? `${r.off} min` : r.off === 0 ? "never" : "?"} · ${r.vp ?? ""} · up ${r.up ?? "?"} s`,
    `${r.ua ?? ""}`,
    `loader: ${r.ld ?? "?"}`,
  ];
  const extra = Object.keys(r).filter((k) => !["v", "pg", "ver", "tab", "con", "bl", "dim", "off", "ld", "ua", "vp", "up", "err", "js", "t", "cut", "title"].includes(k));
  if (extra.length) head.push(extra.map((k) => `${k}: ${typeof r[k] === "string" ? r[k] : JSON.stringify(r[k])}`).join(" · "));
  for (const e of r.err ?? []) head.push(`! ${e}`);
  for (const e of r.js ?? []) head.push(`JS ${e}`);
  head.push("—", ...(r.t ?? []));
  if (r.cut) head.push(`… ${r.cut} more lines`);
  return head;
}

/** Wire time of `length` bytes panel → phone: Read frames and the field relay rate (bytes per second). */
export function estimateScreenTransfer(length: number, o: { chunk?: number; bytesPerSec?: number } = {}): { frames: number; replyBytes: number; requestBytes: number; seconds: number } {
  const chunk = o.chunk ?? PS_CHUNK;
  const frames = Math.ceil(length / chunk);
  let replyBytes = 0;
  for (let off = 0; off < length; off += chunk) replyBytes += 8 + pack7(new Uint8Array(6 + Math.min(chunk, length - off))).length;
  // the field rate counts package bytes carried in 240 B Data frames (200 B each): as frame bytes per second it is
  // 1.2 times that; the reverse direction is taken to move frame bytes at the same pace [inference, unmeasured]
  const frameRate = (o.bytesPerSec ?? RELAY_FIELD_BPS) * (240 / 200);
  return { frames, replyBytes, requestBytes: frames * readRequest(0, 0).length, seconds: replyBytes / frameRate };
}

/** Reader over any PuiPort (a PianoSession, a test double). */
export class PanelScreenClient {
  private readonly port: PuiPort;
  private static tag = Math.floor(Math.random() * 100);
  constructor(port: PuiPort) {
    this.port = port;
  }

  private nextTag(): number {
    PanelScreenClient.tag = (PanelScreenClient.tag % 127) + 1;
    return PanelScreenClient.tag;
  }

  private ask(msg: Uint8Array, op: number, match: (d: Uint8Array) => boolean, timeoutMs: number): Promise<Uint8Array | null> {
    return new Promise((resolve) => {
      let done = false;
      const finish = (v: Uint8Array | null) => {
        if (done) return;
        done = true;
        off();
        clearTimeout(timer);
        resolve(v);
      };
      const off = this.port.onMidi((m) => {
        const r = parsePuiReply(m);
        if (r && r.op === op && match(r.data)) finish(r.data);
      });
      const timer = setTimeout(() => finish(null), timeoutMs);
      this.port.sendRaw(msg);
    });
  }

  /** Press (click) or fill (set) an element on the panel page by id; rc 0 done · 1 bad args · 4 no such element,
   *  null = no answer (older panel page without op 0x37, or not our page). */
  async act(id: string, value?: string, timeoutMs = 2500): Promise<number | null> {
    const tag = this.nextTag();
    const action = value === undefined ? 1 : 2;
    const d = await this.ask(actRequest(tag, action, id, value ?? ""), PanelScreenOp.Act, (x) => x[1] === tag, timeoutMs);
    return d ? d[0]! : null;
  }

  /** Capture: polls while the panel draws (rc 3). null = no reply at all. */
  async capture(kind: number, p1 = 0, p2 = 0, opt: { timeoutMs?: number; replyMs?: number } = {}): Promise<CaptureInfo | null> {
    const tag = this.nextTag();
    const until = Date.now() + (opt.timeoutMs ?? 20000);
    let silent = 0;
    for (;;) {
      const d = await this.ask(captureRequest(kind, tag, p1, p2), PanelScreenOp.Capture, (x) => decodeCapture(x)?.tag === tag, opt.replyMs ?? 2000);
      const c = d ? decodeCapture(d) : null;
      if (!c) {
        if (++silent >= 3) return null;
        continue;
      }
      silent = 0;
      if (c.rc !== 3 || Date.now() > until) return c;
      await new Promise((r) => setTimeout(r, 250));
    }
  }

  /** Read the capture's bytes: window W, cumulative (in-order) acceptance, go-back-N after a timeout. */
  async read(info: CaptureInfo, opt: ScreenOptions = {}): Promise<ScreenResult> {
    const t0 = Date.now();
    const W = Math.max(1, opt.window ?? 2);
    const ackMs = opt.ackTimeoutMs ?? 3000;
    const maxRetries = opt.maxRetries ?? 8;
    let chunk = Math.max(16, Math.min(PS_CHUNK, opt.chunk ?? PS_CHUNK));
    const total = info.length;
    const buf = new Uint8Array(total);
    let base = 0;
    let next = 0;
    let retries = 0;
    let inRow = 0;
    let frames = 0;
    let fail: string | null = null;
    const replies: ReadReply[] = [];
    let wake: (() => void) | null = null;
    const off = this.port.onMidi((m) => {
      const r = parsePuiReply(m);
      if (!r || r.op !== PanelScreenOp.Read) return;
      const d = decodeRead(r.data);
      if (!d || d.tag !== info.tag) return;
      replies.push(d);
      wake?.();
    });
    const waitReply = () =>
      new Promise<boolean>((resolve) => {
        if (replies.length) return resolve(true);
        const t = setTimeout(() => {
          wake = null;
          resolve(false);
        }, ackMs);
        wake = () => {
          clearTimeout(t);
          wake = null;
          resolve(true);
        };
      });
    const progress = (phase: ScreenProgress["phase"], message?: string) => opt.onProgress?.({ phase, got: base, total, retries, message });
    try {
      while (base < total && !fail) {
        while (next < total && next - base < W * chunk) {
          this.port.sendRaw(readRequest(info.tag, next, chunk));
          frames++;
          next += Math.min(chunk, total - next);
        }
        if (!(await waitReply())) {
          retries++;
          if (++inRow > maxRetries) fail = "too many timeouts";
          if (inRow >= 2 && chunk > 50) chunk = Math.max(50, chunk >> 1); // smaller replies if long ones get lost
          next = base;
          progress("read", `timeout, again from ${base}`);
          continue;
        }
        const r = replies.shift()!;
        if (r.rc !== 0) {
          fail = PS_RC[r.rc] ?? `rc ${r.rc}`;
          break;
        }
        if (r.off !== base || !r.data.length) continue; // a duplicate or one behind a lost reply: the go-back resends it
        buf.set(r.data.subarray(0, total - base), base);
        base += r.data.length;
        inRow = 0;
        progress("read");
      }
    } finally {
      off();
    }
    const readMs = Date.now() - t0;
    if (fail) return { ok: false, error: fail, info, ms: readMs, readMs, frames, retries, chunk };
    if (crc32(buf) !== info.crc) return { ok: false, error: "CRC mismatch", info, ms: readMs, readMs, frames, retries, chunk };
    return { ok: true, info, bytes: buf, ms: readMs, readMs, frames, retries, chunk };
  }

  private async fetch(kind: number, p1: number, p2: number, opt: ScreenOptions): Promise<ScreenResult> {
    const t0 = Date.now();
    opt.onProgress?.({ phase: "capture", got: 0, total: 0, retries: 0 });
    const info = await this.capture(kind, p1, p2, { timeoutMs: opt.captureTimeoutMs });
    const failed = (error: string, i?: CaptureInfo): ScreenResult => {
      opt.onProgress?.({ phase: "error", got: 0, total: 0, retries: 0, message: error });
      return { ok: false, error, info: i, ms: Date.now() - t0, readMs: 0, frames: 0, retries: 0, chunk: 0 };
    };
    if (!info) return failed("no reply — the piano needs the panel relay, the panel a OpenGrand page with the screen reporter (bundle or remote UI 0.2.3+)");
    if (info.rc !== 0) return failed(PS_RC[info.rc] ?? `rc ${info.rc}`, info);
    const r = await this.read(info, opt);
    r.ms = Date.now() - t0;
    opt.onProgress?.(r.ok ? { phase: "done", got: info.length, total: info.length, retries: r.retries } : { phase: "error", got: 0, total: info.length, retries: r.retries, message: r.error });
    return r;
  }

  /** The panel's text report (≤ maxBytes of JSON). */
  async report(opt: ScreenOptions & { maxBytes?: number } = {}): Promise<ScreenResult & { report?: PanelReport }> {
    const r = await this.fetch(PS_KIND.Report, Math.max(4, Math.min(127, Math.round((opt.maxBytes ?? 1024) / 64))), 0, opt);
    if (!r.ok || !r.bytes) return r;
    try {
      return { ...r, report: JSON.parse(new TextDecoder().decode(r.bytes)) as PanelReport };
    } catch (e) {
      return { ...r, ok: false, error: `report is not JSON: ${(e as Error).message}` };
    }
  }

  /** The panel's picture: `width` px wide (≤ 480, the panel is 480 CSS px), JPEG fitted to about `kb` KiB. */
  async snapshot(opt: ScreenOptions & { width?: number; kb?: number } = {}): Promise<ScreenResult & { dataUrl?: string; mime?: string }> {
    const w = Math.max(48, Math.min(480, opt.width ?? 240));
    const r = await this.fetch(PS_KIND.Snapshot, Math.round(w / 8), Math.max(4, Math.min(127, opt.kb ?? 24)), { captureTimeoutMs: 30000, ...opt });
    if (!r.ok || !r.bytes || !r.info) return r;
    return { ...r, mime: PS_FMT[r.info.fmt] ?? "application/octet-stream", dataUrl: dataUrl(r.bytes, r.info.fmt) };
  }
}
