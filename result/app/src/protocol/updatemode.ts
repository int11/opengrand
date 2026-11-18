// The stock USB firmware updater (BootProgram, power on with E+A+B+C) as the built-in panel sees it.
// Every byte below was read with REA in the
// BootProgram (NOR 0x000000–0x060000, updater at VA 0x40000000 + NOR offset):
//
//   panel -> F0 40 7F 7E 08 02 01 01 F7         CONNECT. FUN_40015420 takes only this exact frame, and only
//                                               while its flag *0x404417C9 is 0: the FIRST CONNECT wins, its
//                                               port is kept (*0x404417CA), every later CONNECT is ignored
//   piano -> F0 40 7F 7E 04 1E 02 01 04 F7      Mode = DevelopperMode / HostUpdate (FUN_40015734), sent ONCE
//                                               when a poll point sees the flag (Sync 0–3: FUN_40015E90 /
//                                               40015E18 / 40015CFC / 40015B6C); nothing is sent before it
//   piano -> F0 40 7F 24 04 1E 03 00 00 <name, 7-bit packed> xor F7     file being written (FUN_40015824)
//   piano -> F0 40 7F 7E 04 1E 04 03 a0 a1 a2 a3 a4 F7                  flash offset, 7 bits per byte LE,
//                                               every 64 KB and once at the file's end (FUN_4001598C)
//   piano -> F0 40 7F 7E 04 1E 03 00 r r F7     file result: 00 00 OK!, 01 01 Write error! (FUN_40015A3C)
//   piano -> F0 40 7F 7E 04 1E 03 00 00 s F7    stop: 21 No USB Memory, 56 File not found / file errors (FUN_40015798)
//   piano -> F0 40 7F 7E 04 1E 03 00 01 7D F7   Boot End: all files written (FUN_40015AC4)
//
// The original Kawai UI shows its update page (appUI/developper/dpupdatemode) when CONNECT is answered with
// Mode DevelopperMode (ui/js/app.js onConnectedPiano). Since the updater answers only the first CONNECT, the
// page that saw the Mode reply must show the update itself: after our app took it, the Kawai main UI would
// never be answered.
import { decodeFrame } from "./sysex.ts";

/** Main program partition in flash offsets (MainProgram at 0x60060000, 0x3A0000 B). */
export const MAIN_START = 0x060000;
export const MAIN_END = 0x400000;

export type UpdaterEvent =
  | { kind: "mode"; sub: number } // 7E 02 01 sub (sub 4 = HostUpdate)
  | { kind: "file"; name: string }
  | { kind: "address"; offset: number }
  | { kind: "result"; ok: boolean }
  | { kind: "stopped"; why: "nousb" | "nofile" | "other"; code: number }
  | { kind: "bootEnd" };

const isHead = (m: ArrayLike<number>, fn: number) => m[0] === 0xf0 && m[1] === 0x40 && m[3] === fn && m[4] === 0x04 && m[5] === 0x1e && m[m.length - 1] === 0xf7;

/** One frame from the updater, or null (anything else, including the running piano's own frames). */
export function decodeUpdaterFrame(m: ArrayLike<number>): UpdaterEvent | null {
  if (m.length < 10) return null;
  if (isHead(m, 0x24) && m.length === 26 && m[6] === 0x03 && m[7] === 0x00 && m[8] === 0x00) {
    let x = 0;
    for (let i = 1; i < 24; i++) x ^= m[i];
    if (x !== m[24]) return null;
    let name = "";
    for (let i = 9; i < 22; i++) if (i !== 16 && m[i] >= 0x20 && m[i] < 0x7f) name += String.fromCharCode(m[i]);
    return { kind: "file", name: name.trim() };
  }
  if (!isHead(m, 0x7e) || !decodeFrame(m)) return null;
  if (m.length === 10 && m[6] === 0x02 && m[7] === 0x01) return { kind: "mode", sub: m[8] };
  if (m.length === 14 && m[6] === 0x04 && m[7] === 0x03) {
    const offset = (m[8] | (m[9] << 7) | (m[10] << 14) | (m[11] << 21)) + m[12] * 0x10000000;
    return { kind: "address", offset };
  }
  if (m.length === 11 && m[6] === 0x03 && m[7] === 0x00) {
    const a = m[8], b = m[9];
    if (a === 0x01 && b === 0x7d) return { kind: "bootEnd" };
    if (a === 0x00 && b === 0x00) return { kind: "result", ok: true };
    if (a === 0x01 && b === 0x01) return { kind: "result", ok: false };
    if (a === 0x00) return { kind: "stopped", why: b === 0x21 ? "nousb" : b === 0x56 ? "nofile" : "other", code: b };
  }
  return null;
}

/** Is this the running piano's own Mode reply (7E 02 00 xx, normal mode)? */
export function isNormalMode(m: ArrayLike<number>): boolean {
  return m.length === 10 && isHead(m, 0x7e) && m[6] === 0x02 && m[7] === 0x00;
}

export interface UpdaterFile { name: string; offset: number | null; first: number | null; result: "ok" | "error" | null }
export type UpdaterPhase = "writing" | "done" | "error" | "nousb" | "nofile";
export interface UpdaterState {
  active: boolean;
  phase: UpdaterPhase;
  files: UpdaterFile[];
  /** 0..1 for a file written into the main program partition, else null. */
  progress: number | null;
  /** How update mode was recognised: the Mode reply, or (that reply went to an earlier page) the progress frames. */
  via: "mode" | "progress" | null;
}

/**
 * Follows the updater's frames on the panel link. Becomes active on the Mode reply DevelopperMode; on the
 * progress frames alone (address, file name, Boot End) only while the piano never answered as the running
 * piano — the Mode reply may have gone to an earlier page that reloaded.
 */
export class UpdaterWatch {
  readonly state: UpdaterState = { active: false, phase: "writing", files: [], progress: null, via: null };
  private normal = false;
  private listeners: ((s: UpdaterState) => void)[] = [];

  onChange(fn: (s: UpdaterState) => void): void {
    this.listeners.push(fn);
  }

  /** The piano answered as the running piano (session ready): progress frames alone no longer count. */
  pianoRunning(): void {
    this.normal = true;
  }

  feed(m: ArrayLike<number>): boolean {
    if (isNormalMode(m)) {
      this.normal = true;
      return false;
    }
    const e = decodeUpdaterFrame(m);
    if (!e) return false;
    const s = this.state;
    if (!s.active) {
      if (e.kind === "mode") {
        if (e.sub !== 0x04 && e.sub !== 0x05) return false; // CheckSum / Destination / Model: factory tools, not ours to show
        s.via = "mode";
      } else if (!this.normal && (e.kind === "address" || e.kind === "file" || e.kind === "bootEnd")) s.via = "progress";
      else return false;
      s.active = true;
    }
    const cur = s.files[s.files.length - 1];
    switch (e.kind) {
      case "mode": break;
      case "file":
        // the updater re-sends a file's name when it syncs late (Sync 2 replays the files so far): same name in a row = same file
        if (!cur || cur.name !== e.name || cur.result) s.files.push({ name: e.name, offset: null, first: null, result: null });
        s.phase = "writing";
        break;
      case "address": {
        let f = cur;
        if (!f || f.result) s.files.push((f = { name: "", offset: null, first: null, result: null }));
        f.offset = e.offset;
        if (f.first === null) f.first = e.offset;
        s.progress = f.first >= MAIN_START && f.first < MAIN_END ? Math.min(1, Math.max(0, (e.offset - MAIN_START) / (MAIN_END - MAIN_START))) : null;
        break;
      }
      case "result":
        if (cur) cur.result = e.ok ? "ok" : "error";
        if (!e.ok) s.phase = "error";
        break;
      case "stopped": s.phase = e.why === "nousb" ? "nousb" : e.why === "nofile" ? "nofile" : "error"; break;
      case "bootEnd":
        if (s.phase === "writing") s.phase = "done";
        if (cur && cur.first !== null && cur.first >= MAIN_START && cur.first < MAIN_END && cur.result === "ok") s.progress = 1;
        break;
    }
    for (const l of this.listeners) l(s);
    return true;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// A model of the updater's panel side, for tests and the panel simulator (scripts/panelsim.ts --updatemode).

const enc = (fn: number, body: number[]) => Uint8Array.from([0xf0, 0x40, 0x7f, fn, 0x04, 0x1e, ...body, 0xf7]);
const CONNECT = [0xf0, 0x40, 0x7f, 0x7e, 0x08, 0x02, 0x01, 0x01, 0xf7];

/** FUN_40015824: the 12 name bytes as the updater lays them out (7 + packing byte + 5), xor of bytes 1..23. */
export function fileFrame(name: string): Uint8Array {
  const b = [...name.slice(0, 12).padEnd(12, " ")].map((c) => c.charCodeAt(0) & 0x7f);
  const m = [0xf0, 0x40, 0x7f, 0x24, 0x04, 0x1e, 0x03, 0x00, 0x00, ...b.slice(0, 7), 0x00, ...b.slice(7, 12), 0x00, 0x00];
  let x = 0;
  for (let i = 1; i < 24; i++) x ^= m[i];
  return Uint8Array.from([...m, x, 0xf7]);
}
export const modeFrame = () => enc(0x7e, [0x02, 0x01, 0x04]);
export const addressFrame = (o: number) => enc(0x7e, [0x04, 0x03, o & 0x7f, (o >> 7) & 0x7f, (o >> 14) & 0x7f, (o >> 21) & 0x7f, (o >>> 28) & 0x7f]);
export const resultFrame = (ok: boolean) => enc(0x7e, ok ? [0x03, 0x00, 0x00, 0x00] : [0x03, 0x00, 0x01, 0x01]);
export const stoppedFrame = (code: 0x21 | 0x56) => enc(0x7e, [0x03, 0x00, 0x00, code]);
export const bootEndFrame = () => enc(0x7e, [0x03, 0x00, 0x01, 0x7d]);

/**
 * The updater writing one main program file, as the panel port sees it: nothing until the first exact CONNECT;
 * that CONNECT is answered with the Mode reply and the replay of the files so far (Sync 2, FUN_40015CFC); later
 * CONNECTs are ignored. `step()` advances the write by 64 KB (an address frame), then the result and Boot End.
 */
export class UpdaterModel {
  private synced = false;
  private pending = false;
  private at: number;
  private finished = false;
  readonly sent: Uint8Array[] = [];
  private out: (m: Uint8Array) => void;
  readonly name: string;
  readonly start: number;
  readonly size: number;
  constructor(out: (m: Uint8Array) => void, name = "CAx9_P06SYS", start = MAIN_START, size = 0x3a0000) {
    this.out = out;
    this.name = name;
    this.start = start;
    this.size = size;
    this.at = start;
  }
  get bound(): boolean { return this.synced; }
  get done(): boolean { return this.finished; }
  receive(m: ArrayLike<number>): void {
    if (this.synced || this.pending || m.length !== CONNECT.length) return;
    for (let i = 0; i < CONNECT.length; i++) if (m[i] !== CONNECT[i]) return;
    this.pending = true; // answered at the next poll point (step)
  }
  private send(m: Uint8Array): void {
    this.sent.push(m);
    if (this.synced) this.out(m); // FUN_400151CC: nothing goes out before the sync
  }
  step(): void {
    if (this.pending && !this.synced) {
      this.pending = false;
      this.synced = true;
      this.send(modeFrame());
      this.send(fileFrame(this.name)); // Sync 2: the file under way
    }
    if (this.finished) return;
    if (this.at === this.start) this.send(fileFrame(this.name));
    if (this.at < this.start + this.size) {
      this.send(addressFrame(this.at));
      this.at += 0x10000;
      return;
    }
    this.send(addressFrame(this.start + this.size - 1));
    this.send(resultFrame(true));
    this.send(bootEndFrame());
    this.finished = true;
  }
}
