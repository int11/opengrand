// Remote panel UI update (REMOTE_PROTOCOL.md section 10.7).
//
// The built-in panel has no network: its only link is USB-MIDI to the piano. After the OpenGrand
// UI bundle is installed once from USB, a phone / PC sends a signed UI package (.c79u) over
// BLE-MIDI to the piano, the piano's cave relays ops 0x30-0x3F to the panel port (3) and the
// replies 0x70-0x7F back, and the loader in the panel bundle (panelui/uiloader.js) verifies,
// stores and boots it. This module is the sender plus the package format shared with the
// packer (scripts/panelpkg.ts); the panel side is the ES5 loader, cross-checked in tests.
import { encodeBlePackets } from "./blemidi.ts";
import { crc32, NG_ID, NG_TAG, pack7, unpack7 } from "./ngext.ts";
import { withXferLock, type XferJob, xferJob } from "./xferlock.ts";

export const PanelUiOp = { Status: 0x30, Begin: 0x31, Data: 0x32, Commit: 0x33, Control: 0x34 } as const;
/** Ops 0x30-0x3F (replies 0x70-0x7F) are the panel-UI family the piano relays to port 3. */
export const PANEL_UI_OPS = { first: 0x30, last: 0x3f } as const;
export const PUI_MAGIC = [0x43, 0x37, 0x39, 0x55] as const; // "C79U"
export const PUI_HDR = 80;
export const PUI_SIG = 64;
/** Raw payload bytes per data op: 11 B framing + pack7(200) = 240 B, the OTA frame size (BLE path ≤ 256 B). */
export const PUI_CHUNK = 200;
export const PUI_MAX_PKG = 512 * 1024;
export const PUI_ABI = 1;
export const PUI_FLAG_DEFLATE = 0x01;
/** Begin reply byte 5, after rc · received (loaders from 2026-11-15 on; older replies end after `received`). */
export const PUI_CAP_CRC = 0x01; // Data frames may carry CRC-32(off LE u32 · data) after the data

export const PUI_RC: Record<number, string> = {
  0: "ok", 1: "bad arguments", 3: "busy", 5: "bad length", 6: "not newer than the installed UI", 7: "out of order",
  8: "not receiving", 9: "incomplete", 10: "hash mismatch", 11: "header invalid (other signing key?)", 12: "signature mismatch",
  13: "panel storage full / write failed", 14: "already installed", 15: "unpack failed", 16: "no such slot", 17: "panel bundle has no signing key",
  0x7f: "panel loader error",
};
export const PuiControl = { Activate: 1, Rollback: 2, Forget: 3, Abort: 4, ClearBad: 5 } as const;
export const PUI_RX_STATE = ["idle", "receiving", "complete", "", "stored", "error"] as const;
export const PUI_SLOT_STATE = ["empty", "new", "ok", "bad"] as const;
export const PUI_RUNNING = ["chooser / none", "bundled", "slot A", "slot B"] as const;

export interface PanelUiHeader {
  format: number;
  flags: number;
  abi: number;
  seq: number;
  length: number;
  unpacked: number;
  crc: number;
  keyId: number;
  version: string;
  hash: Uint8Array; // first 32 B of SHA-512(payload)
}

const u32 = (b: Uint8Array, o: number) => (b[o]! | (b[o + 1]! << 8) | (b[o + 2]! << 16) | (b[o + 3]! << 24)) >>> 0;
const le32 = (v: number) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
const ascii = (b: Uint8Array) => {
  let s = "";
  for (const c of b) {
    if (!c) break;
    s += String.fromCharCode(c & 0x7f);
  }
  return s;
};

/** Header layout (80 B, LE) — the one place besides uiloader.js parseHeader(). */
export function encodeHeader(h: PanelUiHeader): Uint8Array {
  const b = new Uint8Array(PUI_HDR);
  b.set(PUI_MAGIC, 0);
  b[4] = h.format;
  b[5] = h.flags;
  b[6] = h.abi;
  b.set(le32(h.seq), 8);
  b.set(le32(h.length), 12);
  b.set(le32(h.unpacked), 16);
  b.set(le32(h.crc), 20);
  b.set(le32(h.keyId), 24);
  const v = Array.from(h.version, (c) => c.charCodeAt(0) & 0x7f);
  if (v.length > 16) throw new Error(`version ${h.version}: at most 16 characters`);
  b.set(v, 28);
  b.set(h.hash.subarray(0, 32), 48);
  return b;
}

export function decodeHeader(b: Uint8Array): PanelUiHeader | null {
  if (b.length < PUI_HDR || PUI_MAGIC.some((m, i) => b[i] !== m)) return null;
  return {
    format: b[4]!, flags: b[5]!, abi: b[6]!, seq: u32(b, 8), length: u32(b, 12), unpacked: u32(b, 16), crc: u32(b, 20), keyId: u32(b, 24),
    version: ascii(b.subarray(28, 44)), hash: b.slice(48, 80),
  };
}

export interface PanelUiPackage {
  header: PanelUiHeader;
  /** header 80 B + signature 64 B: the Begin argument */
  head: Uint8Array;
  payload: Uint8Array;
}

/** Split a .c79u file (no signature check here: the panel does it). */
export function parsePackage(file: Uint8Array): PanelUiPackage | null {
  const header = decodeHeader(file);
  if (!header || header.format !== 1 || file.length !== PUI_HDR + PUI_SIG + header.length) return null;
  return { header, head: file.slice(0, PUI_HDR + PUI_SIG), payload: file.slice(PUI_HDR + PUI_SIG) };
}

export const keyIdHex = (id: number) => le32(id).map((b) => b.toString(16).padStart(2, "0")).join("");

export interface PanelUiSlot {
  seq: number;
  state: (typeof PUI_SLOT_STATE)[number];
  tries: number;
  version: string;
}
export interface PanelUiStatus {
  layout: number;
  abi: number;
  page: number;
  rxState: number;
  lastRc: number;
  storage: boolean;
  hasKey: boolean;
  rxSeq: number;
  received: number;
  total: number;
  maxSeq: number;
  slots: [PanelUiSlot, PanelUiSlot];
  bundledSeq: number;
  chunk: number;
  running: number;
  keyId: number;
  bundledVersion: string;
  /** Data frames the loader refused by length / CRC since its page loaded; null from loaders before 2026-11-15 */
  refused: number | null;
}

export function decodePanelStatus(data: Uint8Array): PanelUiStatus | null {
  const r = unpack7(data);
  if (r.length < 93 || r[0] !== 1) return null;
  const str = (o: number) => ascii(r.subarray(o, o + 16));
  const slot = (o: number, vo: number): PanelUiSlot => ({ seq: u32(r, o), state: PUI_SLOT_STATE[r[o + 4]!] ?? "empty", tries: r[o + 5]!, version: str(vo) });
  return {
    layout: r[0]!, abi: r[1]!, page: r[2]!, rxState: r[3]!, lastRc: r[4]!, storage: !!(r[5]! & 1), hasKey: !!(r[5]! & 2),
    rxSeq: u32(r, 6), received: u32(r, 10), total: u32(r, 14), maxSeq: u32(r, 18),
    slots: [slot(22, 45), slot(28, 61)], bundledSeq: u32(r, 34), chunk: r[38]! | (r[39]! << 8), running: r[40]!, keyId: u32(r, 41), bundledVersion: str(77),
    refused: r.length >= 97 ? u32(r, 93) : null,
  };
}

export function puiRequest(op: number, args: number[] = []): Uint8Array {
  return Uint8Array.from([0xf0, NG_ID, ...NG_TAG, op, ...args.map((b) => b & 0x7f), 0xf7]);
}

/** Data op frame for `chunk` at byte offset `off` (21-bit offset); `crc` appends CRC-32(off LE u32 · chunk), which only
 *  a loader that offered PUI_CAP_CRC in its Begin reply understands (an older one would store the 4 bytes as data). */
export function dataFrame(off: number, chunk: Uint8Array, crc = false): Uint8Array {
  const raw = crc ? Uint8Array.from([...chunk, ...le32(crc32([...le32(off), ...chunk]))]) : chunk;
  return puiRequest(PanelUiOp.Data, [(off >> 14) & 0x7f, (off >> 7) & 0x7f, off & 0x7f, ...pack7(raw)]);
}

/** A panel-UI reply (op 0x70-0x7F) → its op (0x30-0x3F) and data, else null. */
export function parsePuiReply(msg: Uint8Array): { op: number; data: Uint8Array } | null {
  if (msg.length < 8 || msg[0] !== 0xf0 || msg[1] !== NG_ID || msg[msg.length - 1] !== 0xf7) return null;
  for (let i = 0; i < NG_TAG.length; i++) if (msg[2 + i] !== NG_TAG[i]) return null;
  const op = msg[6]!;
  if (op < 0x70 || op > 0x7f) return null;
  return { op: op & 0x3f, data: msg.slice(7, msg.length - 1) };
}

/** What the sender needs from a PianoSession (or a test double). */
export interface PuiPort {
  sendRaw(msg: Uint8Array): void;
  onMidi(fn: (m: Uint8Array) => void): () => void;
}

export interface UploadProgress {
  phase: "begin" | "upload" | "commit" | "activate" | "done" | "error";
  sent: number;
  total: number;
  retries: number;
  /** times the whole package was sent again from byte 0 (the panel's copy was found damaged) */
  restarts?: number;
  message?: string;
  rc?: number;
  /** done with `activate`: true = the panel took Activate (it reloads into the new version), false = it refused or did
   *  not answer (`activateRc`: its rc, null = no reply) - the package is stored, the old UI still runs */
  activated?: boolean;
  activateRc?: number | null;
}
export interface UploadOptions {
  /** Data frames in flight (1 = stop-and-wait). Default 2: hides the relay round trip, keeps the
   *  BLE module and the relay's panel ring to ≤ 2 frames. */
  window?: number;
  ackTimeoutMs?: number;
  commitTimeoutMs?: number;
  /** Timeouts tolerated in a row before giving up (each one re-syncs from the panel status). */
  maxRetries?: number;
  /** Whole-package resends after the panel's copy was found damaged (a frame stored at a wrong length, or rc 10
   *  "hash mismatch" at Commit). Default 2. */
  maxRestarts?: number;
  /** Per-frame CRC-32 when the loader offers it (default); false = plain frames as before 2026-11-15. */
  crc?: boolean;
  /** Reload the panel into the new version right after it is stored. */
  activate?: boolean;
  onProgress?: (p: UploadProgress) => void;
}

/** Sender over any PuiPort. One reply listener; requests are matched by op. */
/** A panel UI job as a screen sees it (PanelUiClient.running): "upload" reports UploadProgress and ends with the final
 *  one; "rollback" ends with the text its screen showed. */
export type PanelUiJob = XferJob<unknown, unknown>;

export class PanelUiClient {
  private readonly port: PuiPort;
  constructor(port: PuiPort) {
    this.port = port;
  }

  /** The panel UI job running on this link (started by any screen), to follow: progress, then the result. */
  static running(port: PuiPort): PanelUiJob | null {
    return xferJob(port, "panelui");
  }

  private request(op: number, args: number[], timeoutMs: number): Promise<Uint8Array | null> {
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
        if (r && r.op === op) finish(r.data);
      });
      const timer = setTimeout(() => finish(null), timeoutMs);
      this.port.sendRaw(puiRequest(op, args));
    });
  }

  /** null = no reply: no relay in the piano, or the panel is not on the OpenGrand chooser / UI. */
  async status(timeoutMs = 1500): Promise<PanelUiStatus | null> {
    const d = await this.request(PanelUiOp.Status, [], timeoutMs);
    return d ? decodePanelStatus(d) : null;
  }

  async control(sub: number, timeoutMs = 1500): Promise<number | null> {
    const d = await this.request(PanelUiOp.Control, [sub], timeoutMs);
    return d && d.length >= 2 && d[0] === sub ? d[1]! : null;
  }

  /** Begin: rc, the bytes the panel already holds (resume point) and what else the loader accepts (PUI_CAP_*; 0 for
   *  loaders before 2026-11-15, whose reply ends after `received`). */
  async begin(pkg: PanelUiPackage, timeoutMs = 4000): Promise<{ rc: number; received: number; caps: number } | null> {
    const d = await this.request(PanelUiOp.Begin, pack7(pkg.head), timeoutMs);
    if (!d) return null;
    const r = unpack7(d);
    return { rc: r[0] ?? 1, received: r.length >= 5 ? u32(r, 1) : 0, caps: r.length >= 6 ? r[5]! : 0 };
  }

  async commit(timeoutMs = 30000): Promise<{ rc: number; slot: number } | null> {
    const d = await this.request(PanelUiOp.Commit, [], timeoutMs);
    if (!d) return null;
    const r = unpack7(d);
    return { rc: r[0] ?? 1, slot: r[1] ?? 0 };
  }

  /**
   * Begin → data (go-back-N with `window` frames in flight, cumulative acks) → commit
   * [→ activate]. A lost frame or ack costs one ack timeout, then the panel status gives the
   * resume point; the panel keeps a partial package across page reloads, so calling upload()
   * again with the same file continues where it stopped.
   *
   * Damaged frames: a Data frame that lost or gained bytes on
   * the way still ends in F7, and loaders before 2026-11-15 store whatever it decodes to at `off`. Every frame sent
   * here ends on a grid (resume point + k·200, or the end), so a `received` off that grid means the panel stored a
   * frame of the wrong length: abort the reception (Control 4) and send the package again from 0. A changed byte keeps
   * the length and shows only as rc 10 at Commit (the loader then drops its copy), which also sends it again. Newer
   * loaders check each frame themselves (exact length; CRC-32 when they offer PUI_CAP_CRC) and answer rc 7 = resend.
   */
  upload(file: Uint8Array, opt: UploadOptions = {}): Promise<UploadProgress> {
    // one panel UI job per link (src/protocol/xferlock.ts): Data acks carry no tag
    return withXferLock<UploadProgress, unknown>(this.port, "panelui", "upload", (job) => this.uploadNow(file, {
      ...opt,
      onProgress: (p) => {
        job.report(p);
        opt.onProgress?.(p);
      },
    }), () => {
      const p: UploadProgress = { phase: "error", sent: 0, total: 0, retries: 0, rc: 3, message: PUI_RC[3] };
      opt.onProgress?.(p);
      return p;
    });
  }

  private async uploadNow(file: Uint8Array, opt: UploadOptions): Promise<UploadProgress> {
    const W = Math.max(1, opt.window ?? 2);
    const ackMs = opt.ackTimeoutMs ?? 3000;
    const maxRetries = opt.maxRetries ?? 8;
    const maxRestarts = opt.maxRestarts ?? 2;
    const pkg = parsePackage(file);
    let retries = 0;
    let restarts = 0;
    const report = (p: Omit<UploadProgress, "retries" | "restarts">): UploadProgress => {
      const full = { ...p, retries, restarts };
      opt.onProgress?.(full);
      return full;
    };
    if (!pkg) return report({ phase: "error", sent: 0, total: 0, message: "not a OpenGrand panel UI package (.c79u)" });
    const total = pkg.header.length;
    report({ phase: "begin", sent: 0, total });
    for (;;) {
      let b: { rc: number; received: number; caps: number } | null = null;
      for (let i = 0; i < 3 && !b; i++) b = await this.begin(pkg);
      if (!b) return report({ phase: "error", sent: 0, total, message: "no reply from the panel (relay missing, or the panel is not on the OpenGrand UI)" });
      if (b.rc === 14) return report({ phase: "done", sent: total, total, rc: 14, message: PUI_RC[14] });
      if (b.rc !== 0) return report({ phase: "error", sent: 0, total, rc: b.rc, message: PUI_RC[b.rc] ?? `rc ${b.rc}` });

      const d = await this.sendData(pkg, b.received, {
        W, ackMs, crc: opt.crc !== false && !!(b.caps & PUI_CAP_CRC),
        timeout: () => ++retries > maxRetries,
        progressed: () => void (retries = 0),
        report,
      });
      if (d.kind === "error") return d.p;
      let why: string;
      if (d.kind === "damaged") why = `damaged frame: the panel holds ${d.received} bytes, off the ${PUI_CHUNK}-byte frame grid`;
      else {
        report({ phase: "commit", sent: total, total });
        const c = await this.commit(opt.commitTimeoutMs ?? 30000);
        if (!c) return report({ phase: "error", sent: total, total, message: "no reply to commit" });
        if (c.rc === 0) {
          const slot = c.slot ? "B" : "A";
          if (opt.activate) {
            report({ phase: "activate", sent: total, total, message: slot });
            // the loader answers 0 before it reloads (panelui/uiloader.js onControl 1); no reply / another rc = not started
            const a = await this.control(PuiControl.Activate);
            if (a !== 0) return report({ phase: "done", sent: total, total, message: slot, activated: false, activateRc: a });
            return report({ phase: "done", sent: total, total, message: slot, activated: true });
          }
          return report({ phase: "done", sent: total, total, message: slot });
        }
        // rc 10: every byte arrived, one of them damaged on the way; the loader has dropped its copy (idle again)
        if (c.rc !== 10 || restarts >= maxRestarts) {
          const more = c.rc === 10 && restarts ? ` (after ${restarts} resend${restarts > 1 ? "s" : ""})` : "";
          return report({ phase: "error", sent: total, total, rc: c.rc, message: (PUI_RC[c.rc] ?? `rc ${c.rc}`) + more });
        }
        why = "hash mismatch: damaged on the way";
      }
      if (d.kind === "damaged") {
        if (restarts >= maxRestarts) return report({ phase: "error", sent: d.received, total, rc: 10, message: `${why}; gave up after ${restarts} resends` });
        // Data cannot go back over what the panel holds: drop its copy, then Begin starts at 0
        let a: number | null = null;
        for (let i = 0; i < 3 && a !== 0; i++) a = await this.control(PuiControl.Abort);
        if (a !== 0) return report({ phase: "error", sent: d.received, total, message: `${why}; the panel did not drop its copy` });
      }
      restarts++;
      retries = 0;
      report({ phase: "begin", sent: 0, total, message: `${why} — sending it again from the start` });
    }
  }

  /** upload()'s data phase: until the panel holds every byte, a damaged frame shows, or an error. */
  private async sendData(
    pkg: PanelUiPackage,
    start: number,
    o: { W: number; ackMs: number; crc: boolean; timeout(): boolean; progressed(): void; report(p: Omit<UploadProgress, "retries" | "restarts">): UploadProgress },
  ): Promise<{ kind: "complete" } | { kind: "damaged"; received: number } | { kind: "error"; p: UploadProgress }> {
    const total = pkg.header.length;
    // acks are cumulative ("received"), a gap answers rc 7 with the resume point
    let base = start; // panel holds [0, base)
    let next = base;
    let rewoundAt = -1;
    // every `received` the panel can rightly report: the resume point, the end of any frame sent from it, the end
    const onGrid = (x: number) => x === total || (x >= start && (x - start) % PUI_CHUNK === 0);
    const acks: { rc: number; received: number }[] = [];
    let wake: (() => void) | null = null;
    const off = this.port.onMidi((m) => {
      const r = parsePuiReply(m);
      if (!r || r.op !== PanelUiOp.Data) return;
      const raw = unpack7(r.data);
      acks.push({ rc: raw[0] ?? 1, received: raw.length >= 5 ? u32(raw, 1) : 0 });
      wake?.();
    });
    const waitAck = () =>
      new Promise<boolean>((resolve) => {
        if (acks.length) return resolve(true);
        const t = setTimeout(() => {
          wake = null;
          resolve(false);
        }, o.ackMs);
        wake = () => {
          clearTimeout(t);
          wake = null;
          resolve(true);
        };
      });
    try {
      while (base < total) {
        while (next < total && next - base < o.W * PUI_CHUNK) {
          const n = Math.min(PUI_CHUNK, total - next);
          this.port.sendRaw(dataFrame(next, pkg.payload.subarray(next, next + n), o.crc));
          next += n;
        }
        if (!(await waitAck())) {
          // lost frame or ack: ask where the panel is and continue from there
          if (o.timeout()) return { kind: "error", p: o.report({ phase: "error", sent: base, total, message: "too many timeouts" }) };
          acks.length = 0;
          const st = await this.status();
          let at: number;
          if (st && st.rxState >= 1 && st.rxSeq === pkg.header.seq) at = st.received;
          else {
            const again = await this.begin(pkg); // panel page reloaded / reception aborted: resume
            if (!again || again.rc !== 0) continue;
            at = again.received;
          }
          if (!onGrid(at)) return { kind: "damaged", received: at };
          base = at;
          next = base;
          rewoundAt = -1;
          o.report({ phase: "upload", sent: base, total });
          continue;
        }
        const a = acks.shift()!;
        if ((a.rc === 0 || a.rc === 5 || a.rc === 7) && !onGrid(a.received)) return { kind: "damaged", received: a.received };
        if (a.rc === 0) {
          if (a.received > base) {
            base = a.received;
            o.progressed();
            o.report({ phase: "upload", sent: base, total });
          }
        } else if (a.rc === 7 || a.rc === 5) {
          // 7: a gap, or (loaders from 2026-11-15) a frame refused by its length / CRC; 5: a frame that came out longer
          // than the package (bytes added on the way). Nothing was stored: go back to `received`.
          if (a.received !== rewoundAt) {
            rewoundAt = a.received;
            base = Math.max(base, a.received);
            next = a.received;
          }
        } else return { kind: "error", p: o.report({ phase: "error", sent: base, total, rc: a.rc, message: PUI_RC[a.rc] ?? `rc ${a.rc}` }) };
      }
    } finally {
      off();
    }
    return { kind: "complete" };
  }
}

// ---- transfer time over BLE -------------------------------------------------------

export interface TransferEstimate {
  frames: number;
  frameBytes: number;
  blePackets: number;
  blePacketBytes: number;
  /** pacing-limited time (GattWriter average rate) */
  pacedS: number;
  /** BLE-MIDI module → piano UART (31.25 kbaud ≈ 3125 B/s) time for the SysEx bytes */
  uartS: number;
  /** stop-and-wait: every frame waits one relay round trip */
  stopAndWaitS: number;
  /** windowed: the slowest stage sets the pace */
  windowedS: number;
}

/**
 * Wire time of a package: frames → BLE-MIDI packets (encodeBlePackets, `mtu`), paced at
 * `bytesPerSec` (GattWriter default 2500 B/s). The relay hop (module UART 3125 B/s, then
 * the panel link at `panelBytesPerSec`) and the ack trip add `rttMs` per frame when stop-and-wait.
 */
export function estimateTransfer(pkgLength: number, o: { mtu?: number; bytesPerSec?: number; panelBytesPerSec?: number; ackMs?: number } = {}): TransferEstimate {
  const mtu = o.mtu ?? 23;
  const rate = o.bytesPerSec ?? 2500;
  const panelRate = o.panelBytesPerSec ?? 3125;
  const ackMs = o.ackMs ?? 40; // ack SysEx back over UART + BLE notification (connection interval)
  let frames = 0;
  let frameBytes = 0;
  let packets = 0;
  let packetBytes = 0;
  const body = pkgLength - PUI_HDR - PUI_SIG;
  const all: Uint8Array[] = [puiRequest(PanelUiOp.Begin, pack7(new Uint8Array(PUI_HDR + PUI_SIG).fill(0x55)))];
  for (let off = 0; off < body; off += PUI_CHUNK) all.push(dataFrame(off, new Uint8Array(Math.min(PUI_CHUNK, body - off)).fill(0x55)));
  all.push(puiRequest(PanelUiOp.Commit));
  for (const f of all) {
    frames++;
    frameBytes += f.length;
    const p = encodeBlePackets([f], 0, mtu);
    packets += p.length;
    packetBytes += p.reduce((a, x) => a + x.length, 0);
  }
  const pacedS = packetBytes / rate;
  const uartS = frameBytes / 3125;
  const panelS = frameBytes / panelRate;
  const perFrame = frameBytes / frames;
  // stop-and-wait: BLE send + module UART + panel link + ack trip, per frame, nothing overlaps
  const stopAndWaitS = pacedS + frames * ((perFrame / 3125 + perFrame / panelRate) + ackMs / 1000);
  const windowedS = Math.max(pacedS, uartS, panelS) + (perFrame / 3125 + perFrame / panelRate + ackMs / 1000);
  return { frames, frameBytes, blePackets: packets, blePacketBytes: packetBytes, pacedS, uartS, stopAndWaitS, windowedS };
}
