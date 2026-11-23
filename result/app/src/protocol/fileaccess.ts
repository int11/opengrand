// fn 0x24 FileAccess client (REMOTE_PROTOCOL.md §5 framing, §9.5 OpenGrand contract)
// and the §9.0 Result message.
//
//   F0 40 dev 24 08 02 <v1> <payload> [chk] F7
//   v1 01 TopOfDataLength [L3 L2 L1 L0] (data length in BYTES, 4 x 7 bit — stock FUN_4014F96C / FUN_4014B6E0)
//      02 DataPacket  [counter, 8->7 groups (<= 21 raw bytes), chk]
//      03 FileName    [counter, groups, chk]  — name = '\' + name + NUL, counter restarts per name
//      04 read / 05 write [01, 00] / 06 delete / 07 rename / 08 select
//      09 load [song, type] / 0A save [song, type]   (type index: FILE_TYPE)
//      11 list -> FileName packets ... 1F end of list
//      7B end of data, 7C wait, 7D cancel, 7E nak, 7F ack
//   chk = XOR(bytes 1 .. n-3) & 0x7F
// Every packet in either direction is stop-and-wait: one packet, then wait
// for 24 7F (24 7E = resend the last packet, 24 7D = abort). That includes the
// piano's TopOfData, EndOfData and End-of-list: the stock firmware resends an
// unacknowledged packet 16 times (250 ms each) and then drops the controller
// binding, after which it ignores the controller. The piano also acknowledges each request (04-0B, 11) with
// 24 7F, and a resent packet arrives twice, so duplicates are acked and dropped.
// Success of a file operation arrives as 7E 03 00 00 00 (OpenGrand) or, on the
// stock firmware, as the remote parameter 32 51 7E 7F 00 00.
// Stock request arguments (stock_fn24b.py):
// write must be 24 05 01 00 (any other d1/d2 reports success WITHOUT writing); a byte count that is a
// multiple of 21 needs one empty DataPacket after the last full one; 24 09 / 24 0A carry
// <internal song 0-9> <type index>, and the stock 24 09 loads only KSO files.
import { appFrame, Fn, type KawaiFrame } from "./sysex.ts";
import type { PianoSession } from "./session.ts";

export const FA = {
  Top: 0x01, Data: 0x02, Name: 0x03, Read: 0x04, Write: 0x05, Delete: 0x06, Rename: 0x07, Select: 0x08, Load: 0x09,
  Save: 0x0a, List: 0x11, EndList: 0x1f, EndOfData: 0x7b, Wait: 0x7c, Cancel: 0x7d, Nak: 0x7e, Ack: 0x7f,
} as const;

/** d2 of 24 09 / 24 0A: index into the stock table 0x400FD905 = [WAV, MP3, MID, KSO]. */
export const FILE_TYPE = { WAV: 0, MP3: 1, MID: 2, KSO: 3 } as const;

/** File type index of a name by its extension (KSO = Kawai song; everything else is treated as SMF). */
export function fileTypeOf(name: string): number {
  return /\.kso$/i.test(name) ? FILE_TYPE.KSO : FILE_TYPE.MID;
}

/** Result codes (§9.0; data/app/tables/result_codes.json), value = v2<<14 | v3<<7 | v4. */
export const RESULT: Record<number, string> = {
  0x0000: "OK", 0x0001: "Error", 0x0021: "No media", 0x0022: "Mount error", 0x0031: "Access error", 0x0032: "Disk full",
  0x0033: "Access denied", 0x0041: "Parameter error", 0x0042: "Path error", 0x0055: "Already exists", 0x0056: "Not found",
  0x00e8: "Size over", 0x00e9: "SMF format error", 0x00ea: "Memory over", 0x00ec: "No data", 0x00ed: "Mode error",
  0x00f0: "Not accepted", 0x00f8: "Same name", 0x4000: "Busy",
};

export function resultCode(f: KawaiFrame): number | null {
  if (f.fn === Fn.System && f.body[0] === 0x03 && f.body.length >= 4) return (f.body[1] << 14) | (f.body[2] << 7) | f.body[3];
  // stock: FUN_4014F294(code) reports a finished file operation as 32 51 7E 7F <code hi7> <code lo7>
  if (f.fn === Fn.Remote && f.body[0] === 0x51 && f.body[1] === 0x7e && f.body.length >= 5) return (f.body[3] << 7) | f.body[4];
  return null;
}

export function resultText(code: number): string {
  return RESULT[code] ?? `0x${code.toString(16)}`;
}

/**
 * A fn 0x24 exchange got no answer in time. By then the stock has usually resent its packet 16
 * times and dropped this controller, so the file
 * channel stays silent until a CONNECT. `recovering` = the client has started its `recover` hook
 * (takeControl: CONNECT + re-sync, field 2026-11-16);
 * the next operation waits for it, so a plain retry works.
 */
export class FileTimeoutError extends Error {
  readonly recovering: boolean;
  constructor(what: string, recovering = false) {
    super(`timeout waiting for ${what}${recovering ? " — file channel reset, control re-taken; try again" : ""}`);
    this.name = "FileTimeoutError";
    this.recovering = recovering;
  }
}

/** The piano's file task answers Busy (Result 0x4000 - FUN_4014A8A4: a job still runs, e.g. the NOR write after a
 *  save or an import, or another controller's list) at once, before doing anything: the same request a moment later
 *  goes through (field 2026-11-24: a MID read right after a save, a play right after an import). */
export const BUSY_RETRY = { tries: 6, ms: 700 };
export async function busyRetry<T>(op: () => Promise<T>): Promise<T> {
  for (let i = 1; ; i++) {
    try {
      return await op();
    } catch (e) {
      if (!(e instanceof FileAccessError) || e.code !== 0x4000 || i >= BUSY_RETRY.tries) throw e;
      await new Promise((r) => setTimeout(r, BUSY_RETRY.ms));
    }
  }
}

export class FileAccessError extends Error {
  readonly code: number;
  constructor(code: number) {
    super(resultText(code));
    this.code = code;
  }
}

// ---------------- packing ----------------

/** 8->7: each group of up to 7 bytes gets a header whose bit (6-k) is the MSB of byte k. */
export function pack87(raw: ArrayLike<number>): number[] {
  const out: number[] = [];
  for (let i = 0; i < raw.length; i += 7) {
    const g = Math.min(7, raw.length - i);
    let hdr = 0;
    for (let k = 0; k < g; k++) hdr |= (raw[i + k] & 0x80) >> (k + 1);
    out.push(hdr);
    for (let k = 0; k < g; k++) out.push(raw[i + k] & 0x7f);
  }
  return out;
}

export function unpack78(packed: ArrayLike<number>): number[] {
  const out: number[] = [];
  for (let i = 0; i < packed.length; ) {
    const hdr = packed[i++];
    for (let k = 0; k < 7 && i < packed.length; k++, i++) out.push(packed[i] | ((hdr << (k + 1)) & 0x80));
  }
  return out;
}

export function xorChk(msg: ArrayLike<number>, from: number, to: number): number {
  let x = 0;
  for (let i = from; i < to; i++) x ^= msg[i];
  return x & 0x7f;
}

/** A 02/03 packet: [v1, counter, groups…, chk] with the checksum over bytes 1..n-3. */
export function packet(v1: number, counter: number, raw: ArrayLike<number>, dev: number): Uint8Array {
  const frame = appFrame(Fn.FileDump, [v1, counter & 0x7f, ...pack87(raw), 0], dev);
  frame[frame.length - 2] = xorChk(frame, 1, frame.length - 2);
  return frame;
}

/** Verify and unpack an incoming 02/03 packet frame body; null on a bad checksum. */
export function readPacket(msg: ArrayLike<number>): number[] | null {
  const n = msg.length;
  if (n < 10 || xorChk(msg, 1, n - 2) !== msg[n - 2]) return null;
  return unpack78(Array.prototype.slice.call(msg, 8, n - 2) as number[]);
}

export function nameBytes(name: string): number[] {
  const b = [0x5c]; // '\'
  for (const ch of name.replace(/^\\+/, "")) b.push(ch.charCodeAt(0) & 0xff);
  b.push(0);
  return b;
}

export function namePackets(name: string, dev: number): Uint8Array[] {
  const b = nameBytes(name);
  const out: Uint8Array[] = [];
  for (let i = 0, c = 0; i < b.length; i += 21, c++) out.push(packet(FA.Name, c, b.slice(i, i + 21), dev));
  return out;
}

/**
 * DataPackets of 21 bytes. The stock receiver (FUN_4014FB94) marks the data complete only in a packet
 * one of whose three group offsets (21c, 21c+7, 21c+14) reaches the length, so when the last packet has
 * none (length % 21 = 0 or > 14) one empty packet follows; without it 24 7B is refused (0xED) and nothing
 * is written. The stock also uses the 7-bit counter as
 * the packet index: > 2681 B needs ota_main 213ae247+ (its frc_rx counts past the wrap).
 */
export function dataPackets(data: Uint8Array, dev: number): Uint8Array[] {
  const out: Uint8Array[] = [];
  let c = 0;
  for (let i = 0; i < data.length; i += 21, c++) out.push(packet(FA.Data, c, data.subarray(i, i + 21), dev));
  const r = data.length % 21;
  if (r === 0 || r > 14) out.push(packet(FA.Data, c, [], dev));
  return out;
}

/**
 * Check a DataPacket of a read. `index` = packets taken so far, `left` = bytes still due.
 * -> the bytes of packet `index`; "dup" = a valid resend of packet index - 1; null = damaged or not in sequence (NAK it).
 * The piano fills every packet with 21 bytes but the last (a trailing empty packet included, FUN_4014D7E0), counter
 * = index & 0x7F, so a frame is at most 8 + 3 x 8 + 2 = 34 bytes.
 */
export function readDataPacket(raw: ArrayLike<number>, index: number, left: number): number[] | "dup" | null {
  if (raw.length > 34) return null;
  const bytes = readPacket(raw);
  if (!bytes || bytes.length > 21) return null;
  const ctr = raw[7];
  if (ctr === (index & 0x7f) && bytes.length === Math.min(21, Math.max(0, left))) return bytes;
  if (index > 0 && ctr === ((index - 1) & 0x7f)) return "dup";
  return null;
}

/** The stock's file buffer (0x40B00000) holds < 1 MB: a read of more is refused with Size over (0xE8). */
export const FILE_MAX = 0x100000;

/** TopOfDataLength: the byte count (stock and OpenGrand). */
function topOfData(bytes: number): number[] {
  return [FA.Top, (bytes >> 21) & 0x7f, (bytes >> 14) & 0x7f, (bytes >> 7) & 0x7f, bytes & 0x7f];
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
/** > 2 engine-loop ticks (8 ms): the stock executor must see a write's name before our 7B arrives. */
const WRITE_SETTLE_MS = 30;

// ---------------- client ----------------

type Session = Pick<PianoSession, "onFrame" | "sendRaw" | "sendParam" | "deviceId">;

export interface FileAccessOptions {
  /** Re-bind this controller after a fn 0x24 timeout (the app passes session.takeControl). */
  recover?: () => Promise<unknown> | void;
  /** No second recovery within this many ms of the last one (default 10 s): a dead link must not loop. */
  recoverGapMs?: number;
  now?: () => number;
  /** Silence allowed between the piano's DataPackets of a read (default 1.5 x the timeout = 6 s). It must outlast the
   *  piano's own resends: it repeats a packet every 250 ticks (+ 5) while nothing answers and gives up after 16 such tries
   *  (16 x 255 ms = 4.08 s, stock and ota_main), and ota_main 50a352be+ keeps resending NAKed packets up to 64 times
   *  (long-read retry). Each received frame restarts it. */
  dataTimeoutMs?: number;
}

interface Waiter {
  match: (f: KawaiFrame, raw: Uint8Array) => boolean;
  resolve: (f: KawaiFrame, raw: Uint8Array) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

/**
 * One operation at a time (the piano keeps a single transfer state per port).
 * Every call resolves with the piano's Result or rejects with FileAccessError.
 */
export class FileAccessClient {
  private readonly s: Session;
  private readonly timeoutMs: number;
  private readonly dataTimeoutMs: number;
  private waiters: Waiter[] = [];
  private chain: Promise<unknown> = Promise.resolve();
  private listSink: ((f: KawaiFrame, raw: Uint8Array) => void) | null = null;
  /** frames that arrived before anyone waited for them (bursts are delivered synchronously) */
  private backlog: { f: KawaiFrame; raw: Uint8Array }[] = [];
  private readonly opt: FileAccessOptions;
  private recovering: Promise<unknown> | null = null;
  private lastRecover = -Infinity;
  /** recoveries started (for tests and diagnostics) */
  recoveries = 0;

  constructor(session: Session, timeoutMs = 4000, opt: FileAccessOptions = {}) {
    this.s = session;
    this.timeoutMs = timeoutMs;
    this.dataTimeoutMs = opt.dataTimeoutMs ?? Math.round(timeoutMs * 1.5);
    this.opt = opt;
    session.onFrame((f) => this.onFrame(f));
  }

  /** Drop every wait, the list sink and buffered frames: nothing of a finished operation may answer the next. */
  private reset(why: string): void {
    const ws = this.waiters;
    this.waiters = [];
    this.listSink = null;
    this.backlog = [];
    for (const w of ws) {
      clearTimeout(w.timer);
      w.reject(new Error(why));
    }
  }

  /** Start the recover hook unless one runs or ran within recoverGapMs; returns whether it is running. */
  private recover(): boolean {
    if (!this.opt.recover) return false;
    if (this.recovering) return true;
    const now = (this.opt.now ?? Date.now)();
    if (now - this.lastRecover < (this.opt.recoverGapMs ?? 10000)) return false;
    this.lastRecover = now;
    this.recoveries++;
    const run = Promise.resolve()
      .then(() => this.opt.recover!())
      .catch(() => undefined) // a failed re-take surfaces through the connection state, not here
      .finally(() => {
        if (this.recovering === run) this.recovering = null;
      });
    this.recovering = run;
    return true;
  }

  private raw(f: KawaiFrame): Uint8Array {
    return Uint8Array.from([0xf0, 0x40, f.dev, f.fn, f.group, f.machine, ...f.body, 0xf7]);
  }

  private onFrame(f: KawaiFrame): void {
    const raw = this.raw(f);
    if (this.listSink && f.fn === Fn.FileDump) this.listSink(f, raw);
    for (const w of [...this.waiters]) {
      if (w.match(f, raw)) {
        clearTimeout(w.timer);
        this.waiters = this.waiters.filter((x) => x !== w);
        w.resolve(f, raw);
        return;
      }
    }
    if (!this.listSink && (f.fn === Fn.FileDump || resultCode(f) !== null)) {
      this.backlog.push({ f, raw });
      if (this.backlog.length > 64) this.backlog.shift();
    }
  }

  private expect(match: Waiter["match"], what: string, ms = this.timeoutMs): Promise<{ f: KawaiFrame; raw: Uint8Array }> {
    const i = this.backlog.findIndex((b) => match(b.f, b.raw));
    if (i >= 0) return Promise.resolve(this.backlog.splice(i, 1)[0]);
    return new Promise((resolve, reject) => {
      const w: Waiter = {
        match,
        resolve: (f, raw) => resolve({ f, raw }),
        reject,
        timer: setTimeout(() => {
          this.waiters = this.waiters.filter((x) => x !== w);
          reject(new FileTimeoutError(what));
        }, ms),
      };
      this.waiters.push(w);
    });
  }

  private isFa(f: KawaiFrame, v1?: number): boolean {
    return f.fn === Fn.FileDump && (v1 === undefined || f.body[0] === v1);
  }

  /** Wait for the next Result (7E 03 …); throws FileAccessError unless OK. */
  private async result(): Promise<number> {
    const { f } = await this.expect((x) => resultCode(x) !== null, "result");
    const code = resultCode(f)!;
    if (code !== 0) throw new FileAccessError(code);
    return code;
  }

  /** Send a packet and wait for 24 7F; resend on 24 7E (max 3 tries). */
  private async sendAcked(frame: Uint8Array): Promise<void> {
    for (let tries = 0; tries < 3; tries++) {
      const reply = this.expect((f) => this.isFa(f, FA.Ack) || this.isFa(f, FA.Nak) || this.isFa(f, FA.Cancel) || resultCode(f) !== null, "file ack");
      this.s.sendRaw(frame);
      const { f } = await reply;
      const rc = resultCode(f);
      // an OK here is the late Result of the job before (field 2026-11-24: right after a save) - this request was not
      // taken: retried like Busy (busyRetry)
      if (rc !== null) throw new FileAccessError(rc || 0x4000);
      if (f.body[0] === FA.Ack) return;
      if (f.body[0] === FA.Cancel) throw new Error("cancelled by the piano");
    }
    throw new Error("packet rejected 3 times");
  }

  private fa(v1: number, ...extra: number[]): Uint8Array {
    return appFrame(Fn.FileDump, [v1, ...extra], this.s.deviceId);
  }

  private async sendName(name: string): Promise<void> {
    for (const p of namePackets(name, this.s.deviceId)) await this.sendAcked(p);
  }

  /** Acknowledge a packet from the piano (it waits for this before sending the next one). */
  private ack(): void {
    this.s.sendRaw(this.fa(FA.Ack));
  }

  /** Run one operation after the previous one. `fn24` = it talks fn 0x24, so a timeout means the stock
   *  has probably dropped us: reset and re-take control once, then reject with a FileTimeoutError. */
  private serial<T>(op: () => Promise<T>, fn24 = true): Promise<T> {
    const start = async () => {
      if (this.recovering) await this.recovering; // the next operation starts after the CONNECT
      this.backlog = []; // nothing left over from an earlier operation may answer this one
      try {
        return await op();
      } catch (e) {
        if (!(e instanceof FileTimeoutError) || !fn24) throw e;
        this.reset("file operation timed out");
        const what = e.message.replace(/^timeout waiting for /, "");
        throw new FileTimeoutError(what, this.recover());
      } finally {
        this.reset("file operation ended");
      }
    };
    const run = this.chain.then(start, start);
    this.chain = run.catch(() => undefined);
    return run;
  }

  /** File names in list order; the index is the file-list id used by DeleteFile/RenameFile/SongSelect. */
  list(): Promise<string[]> {
    return this.serial(
      () =>
        new Promise<string[]>((resolve, reject) => {
          const names: string[] = [];
          let cur: number[] = [];
          let prev = "";
          const done = (e?: Error) => {
            this.listSink = null;
            clearTimeout(timer);
            offResult();
            if (e) reject(e);
            else resolve(names);
          };
          let timer = setTimeout(() => done(new FileTimeoutError("file list")), this.timeoutMs);
          const offResult = this.s.onFrame((f) => {
            const rc = resultCode(f);
            if (rc !== null && rc !== 0) done(new FileAccessError(rc));
          });
          this.listSink = (f, raw) => {
            if (f.body[0] === FA.EndList) {
              this.ack(); // the stock resends 24 1F until acked, then unbinds the controller
              return done();
            }
            if (f.body[0] !== FA.Name) return; // 24 7F: the piano acknowledging our request
            clearTimeout(timer);
            timer = setTimeout(() => done(new FileTimeoutError("file list")), this.timeoutMs);
            const bytes = readPacket(raw);
            if (!bytes) return this.s.sendRaw(this.fa(FA.Nak));
            const key = raw.join(",");
            if (key === prev) return this.ack(); // resent because our Ack was late: ack again, keep once
            prev = key;
            for (const b of bytes) {
              if (b === 0) {
                names.push(String.fromCharCode(...cur).replace(/^\\+/, ""));
                cur = [];
              } else cur.push(b);
            }
            this.ack();
          };
          this.s.sendRaw(this.fa(FA.List));
        }),
    );
  }

  /** Read a whole file (24 04 + name -> 01 / 02… / 7B, each packet acknowledged, TopOfData and 7B too). */
  read(name: string): Promise<Uint8Array> {
    return busyRetry(() => this.readOp(name)).then((r) => r.data);
  }

  /** The first `maxBytes` (rounded up to whole packets) and the full length, then Cancel (24 7D): a large file's
   *  header without reading all of it at ~350 B/s (field 2026-11-16). Cancel + Ack: the stock and every ota_main end
   *  the job on the next packet boundary with Result 0xF0. */
  head(name: string, maxBytes: number): Promise<{ length: number; data: Uint8Array }> {
    return busyRetry(() => this.readOp(name, maxBytes));
  }

  private readOp(name: string, maxBytes = Infinity): Promise<{ length: number; data: Uint8Array }> {
    return this.serial(async () => {
      const top = this.expect((f) => this.isFa(f, FA.Top) || (resultCode(f) ?? 0) !== 0, "top of data");
      top.catch(() => undefined);
      await this.sendAcked(this.fa(FA.Read));
      await this.sendName(name);
      const first = await top;
      const rc = resultCode(first.f);
      if (rc) throw new FileAccessError(rc);
      const b = first.f.body;
      const length = (b[1] << 21) | (b[2] << 14) | (b[3] << 7) | b[4]; // bytes
      this.ack(); // the first DataPacket follows the Ack of TopOfData
      const out: number[] = [];
      let got = 0;
      for (;;) {
        const { f, raw } = await this.expect((f) => this.isFa(f, FA.Data) || this.isFa(f, FA.EndOfData), "file data", this.dataTimeoutMs);
        if (f.body[0] === FA.EndOfData) {
          if (f.body.length !== 1) {
            this.s.sendRaw(this.fa(FA.Nak)); // damaged (two frames run together): the piano resends it
            continue;
          }
          this.ack();
          break;
        }
        // A long read meets damaged BLE notifications (field 2026-11-16, 1 MB: two DataPackets run together with
        // `F7 F0 40 7F` gone, resends damaged too): NAK anything that is not exactly the next packet or a resend of
        // the last one. The checksum alone is 7 bit (1 in 128 damaged frames would pass), so the packet must also
        // carry the expected counter and exactly min(21, bytes left) bytes; a resend (counter of the packet before,
        // our Ack was late or lost) is acked again and dropped.
        const v = readDataPacket(raw, got, length - out.length);
        if (v === "dup") {
          this.ack();
          continue;
        }
        if (!v) {
          this.s.sendRaw(this.fa(FA.Nak));
          continue;
        }
        out.push(...v);
        got++;
        if (out.length >= maxBytes && out.length < length) {
          // enough: Cancel, then the Ack. The stock sender's ACK wait (FUN_4014B4E0) never looks at the Cancel flag -
          // it only sees it after an Ack (0x4014D9A8) - so a Cancel alone left it resending 16 x 250 ms and dropping
          // this controller (field 2026-11-16: Busy until the CONNECT). ota_main 213ae247+ also ends on the Cancel
          // alone. Either way the job ends at once with Result 0xF0 (Not accepted = cancelled): wait for it, so the
          // next operation does not meet the job still running (Busy).
          const end = this.expect((f) => resultCode(f) !== null || this.isFa(f, FA.EndOfData), "cancel result");
          end.catch(() => undefined);
          this.s.sendRaw(this.fa(FA.Cancel));
          this.ack();
          const r = await end.catch(() => null);
          if (r && this.isFa(r.f, FA.EndOfData)) this.ack(); // the last packet raced the Cancel: 7B is stop-and-wait too
          return { length, data: Uint8Array.from(out) };
        }
        this.ack();
      }
      if (out.length !== length) throw new Error(`expected ${length} bytes, got ${out.length} in ${got} packets`);
      return { length, data: Uint8Array.from(out) };
    });
  }

  /**
   * Write a file to the USB memory (24 05 01 00 + name, then 01 / 02… / 7B) and wait for the Result.
   * The stock writes only for d1 = 1, d2 = 0 (FUN_4014FA30) and needs its executor to have seen the
   * name (flag 0x413CA3ED, set in the engine loop) by the time 7B arrives, so we pause briefly
   * after the name. The stock writes over an existing file; OpenGrand answers Already exists.
   */
  write(name: string, data: Uint8Array): Promise<number> {
    if (data.length >= FILE_MAX) return Promise.reject(new FileAccessError(0xe8)); // past the 1 MB buffer
    return busyRetry(() => this.serial(async () => {
      await this.sendAcked(this.fa(FA.Write, 1, 0));
      await this.sendName(name);
      await sleep(WRITE_SETTLE_MS);
      return this.upload(data);
    }));
  }

  /** Send an SMF without a name: the piano loads it into its USB slot to play it (§9.5, OpenGrand only;
   *  the stock takes the data and then answers 0xF0 "Not accepted"). */
  playData(data: Uint8Array): Promise<number> {
    return this.serial(() => this.upload(data));
  }

  private async upload(data: Uint8Array): Promise<number> {
    const packets = dataPackets(data, this.s.deviceId);
    await this.sendAcked(this.fa(...(topOfData(data.length) as [number, ...number[]])));
    for (const p of packets) await this.sendAcked(p);
    const res = this.result();
    res.catch(() => undefined);
    await this.sendAcked(this.fa(FA.EndOfData));
    return res;
  }

  private nameOp(v1: number | number[], ...names: string[]): Promise<number> {
    return busyRetry(() => this.nameOnce(v1, ...names));
  }
  /** Once, Busy included: for 24 08 Busy means "that player plays" - ui/usbplay.ts acts on it. */
  private nameOnce(v1: number | number[], ...names: string[]): Promise<number> {
    return this.serial(async () => {
      const res = this.result();
      res.catch(() => undefined);
      await this.sendAcked(this.fa(...((Array.isArray(v1) ? v1 : [v1]) as [number, ...number[]])));
      for (const n of names) await this.sendName(n);
      return res;
    });
  }

  /** Delete by name (24 06): works on the stock firmware and OpenGrand; prefer it over deleteById. */
  delete(name: string): Promise<number> {
    return this.nameOp(FA.Delete, name);
  }
  /** Rename by name (24 07 old new): works on the stock firmware and OpenGrand; prefer it over renameById. */
  rename(from: string, to: string): Promise<number> {
    return this.nameOp(FA.Rename, from, to);
  }
  /**
   * Load a file into the USB player and make it the playback target. On the stock that is
   * MusicMode 3 (USB memory) + Music Play; MID goes to the SMF player, MP3/WAV to the audio player.
   */
  select(name: string): Promise<number> {
    return this.nameOnce(FA.Select, name);
  }
  /**
   * Import a file into internal song `song` (0-9): 24 09 <song> <type>. The stock loads only KSO
   * (type 3) and answers 0xF0 for anything else; OpenGrand imports SMF.
   */
  load(name: string, song: number, type = fileTypeOf(name)): Promise<number> {
    return this.nameOp([FA.Load, song & 0x7f, type], name);
  }
  /** Save internal song `song` (0-9) to the USB memory as `name`: 24 0A <song> <type> (stock and OpenGrand). */
  saveSong(song: number, name: string, type = fileTypeOf(name)): Promise<number> {
    return this.nameOp([FA.Save, song & 0x7f, type], name);
  }

  // ---- remote parameters (fn 0x32, §9.5) ----
  private remote(v1: number, v2: number, data: number[], then?: () => Promise<void>): Promise<number> {
    return this.serial(async () => {
      const res = this.result();
      res.catch(() => undefined);
      await this.s.sendParam({ fn: Fn.Remote, v1, v2, part: 0x7f, data });
      if (then) await then();
      return res;
    }, false); // fn 0x32: the stock simply gives no reply to some of these (32 51 3A / 3B), no unbind
  }
  format(): Promise<number> {
    return this.remote(0x51, 0x3c, [0]);
  }
  /** OpenGrand only: the stock firmware gives no reply to 32 51 3B / 3A (the request times out). */
  deleteById(id: number): Promise<number> {
    return this.remote(0x51, 0x3b, [1, (id >> 7) & 0x7f, id & 0x7f, 0]);
  }
  renameById(id: number, newName: string): Promise<number> {
    return this.remote(0x51, 0x3a, [1, (id >> 7) & 0x7f, id & 0x7f, 0], () => this.sendName(newName));
  }
  /** Save internal song `song` (0-based) as SMF on the USB memory (SONG_nn.MID). OpenGrand only: the stock
   *  ignores 32 52 20 (descriptor 0xCF is a no-op); use saveSong(). */
  saveSongAsSmf(song: number): Promise<number> {
    return this.remote(0x52, 0x20, [1, (song >> 7) & 0x7f, song & 0x7f, 0x7f]);
  }
}
