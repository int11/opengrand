// NgClient: the 0x7D request / reply loop over a PianoSession.
import type { PianoSession } from "../session.ts";
import { u16 } from "./bytes.ts";
import {
  btLogLines, decodeApo, decodeCanary, decodeCoreTest, decodeEcho, decodeIdentity, decodePort, decodeSessionCore, decodeStatus,
  EchoMode, NG_ECHO_MAX, NG_PAYLOAD_MAX, NgState, type NgApoSnapshot, type NgBtLog, type NgCanary, type NgCoreTest, type NgDumpStatus,
  type NgEchoResult, type NgIdentity, type NgLinkSample, type NgPortInfo, type NgStateName, type SessionCore,
} from "./decode.ts";
import { crc32, NgOp, type NgOpCode, ngRequest, parseNgReply, unpack7 } from "./wire.ts";

/** Talks the 0x7D family over an existing session. Every call resolves to null on timeout,
 *  which is the expected outcome against stock firmware. */
export class NgClient {
  private readonly session: PianoSession;
  constructor(session: PianoSession) {
    this.session = session;
  }

  private request(op: NgOpCode, timeoutMs: number, args: number[] = [], accept?: (d: Uint8Array) => boolean): Promise<Uint8Array | null> {
    return new Promise((resolve) => {
      let done = false;
      const finish = (v: Uint8Array | null) => {
        if (done) return;
        done = true;
        off();
        clearTimeout(timer);
        resolve(v);
      };
      const off = this.session.onMidi((m) => {
        const r = parseNgReply(m);
        if (r && r.op === op && (!accept || accept(r.data))) finish(r.data);
      });
      const timer = setTimeout(() => finish(null), timeoutMs);
      this.session.sendRaw(ngRequest(op, args));
    });
  }

  /** Raw op call for helpers in this module (NgPower). */
  call(op: NgOpCode, args: number[], timeoutMs: number, accept?: (d: Uint8Array) => boolean): Promise<Uint8Array | null> {
    return this.request(op, timeoutMs, args, accept);
  }

  /** null = no reply, i.e. stock firmware (or no connection). */
  async identify(timeoutMs = 700): Promise<NgIdentity | null> {
    const d = await this.request(NgOp.Identify, timeoutMs);
    return d ? decodeIdentity(d) : null;
  }

  /** op 0x1B 03: the session core's binding table (feat2 0x02 builds); null = no answer. */
  async sessionCore(timeoutMs = 700): Promise<SessionCore | null> {
    const d = await this.request(NgOp.Front, timeoutMs, [0x03], (x) => x[0] === 0x03);
    return d ? decodeSessionCore(d) : null;
  }

  async status(timeoutMs = 700): Promise<NgDumpStatus | null> {
    const d = await this.request(NgOp.Status, timeoutMs);
    return d ? decodeStatus(d) : null;
  }

  /** Returns the accept code: 0 = started, otherwise an NG_ERRORS key. */
  async startDump(timeoutMs = 1500): Promise<number | null> {
    const d = await this.request(NgOp.DumpStart, timeoutMs);
    return d && d.length >= 1 ? d[0]! : null;
  }

  async cancel(timeoutMs = 1500): Promise<NgStateName | null> {
    const d = await this.request(NgOp.Cancel, timeoutMs);
    return d && d.length >= 1 ? (NgState[d[0]!] ?? "idle") : null;
  }

  // ---- phase2c probe ----

  async port(timeoutMs = 700): Promise<NgPortInfo | null> {
    const d = await this.request(NgOp.Port, timeoutMs);
    return d ? decodePort(d) : null;
  }

  /** Sink (count + CRC only) or echo (also returns the payload). Payload bytes must be 7-bit. */
  async echo(mode: typeof EchoMode.Sink | typeof EchoMode.Echo, payload: Uint8Array, timeoutMs = 4000): Promise<NgEchoResult | null> {
    const d = await this.request(NgOp.Echo, timeoutMs, [mode, ...payload], (x) => x[0] === mode);
    return d ? decodeEcho(d) : null;
  }

  /** The piano sends `n` bytes of (seed + i) & 0x7F (capped at NG_ECHO_MAX). */
  async generate(n: number, seed = 0, timeoutMs = 4000): Promise<NgEchoResult | null> {
    const d = await this.request(NgOp.Echo, timeoutMs, [EchoMode.Generate, (n >> 7) & 0x7f, n & 0x7f, seed], (x) => x[0] === EchoMode.Generate);
    return d ? decodeEcho(d) : null;
  }

  /** Per size: sink (phone -> piano, CRC checked by the piano) then generate (piano -> phone,
   *  CRC checked here). Stops at the first request that gets no reply. */
  async linkTest(sizes = [64, 256, 1024, NG_PAYLOAD_MAX], now: () => number = () => performance.now()): Promise<NgLinkSample[]> {
    const out: NgLinkSample[] = [];
    for (const n of sizes) {
      const payload = Uint8Array.from({ length: n }, (_, i) => (i * 7 + n) & 0x7f);
      let t = now();
      const s = await this.echo(EchoMode.Sink, payload);
      out.push({ mode: "sink", bytes: n, replied: !!s, ok: !!s && s.count === n && s.crc === crc32(payload), ms: now() - t });
      if (!s) break;
      const g = Math.min(n, NG_ECHO_MAX);
      t = now();
      const r = await this.generate(g, n & 0x7f);
      const want = Uint8Array.from({ length: g }, (_, i) => ((n & 0x7f) + i) & 0x7f);
      out.push({ mode: "generate", bytes: g, replied: !!r, ok: !!r && r.count === g && r.crc === crc32(want) && crc32(r.data) === r.crc, ms: now() - t });
      if (!r) break;
    }
    return out;
  }

  async apo(timeoutMs = 700): Promise<NgApoSnapshot | null> {
    const d = await this.request(NgOp.Apo, timeoutMs);
    return d ? decodeApo(d) : null;
  }

  /** Freezes the BT module RX ring on the piano, then reads it in 16 pages of 128 bytes. */
  async btLog(timeoutMs = 1500): Promise<NgBtLog | null> {
    const h = await this.request(NgOp.BtLog, timeoutMs, [0], (x) => x[0] === 0);
    if (!h) return null;
    const hdr = unpack7(h.slice(1));
    if (hdr.length < 6) return null;
    const bytes = new Uint8Array(0x800);
    for (let p = 0; p < 16; p++) {
      const d = await this.request(NgOp.BtLog, timeoutMs, [1, p], (x) => x[0] === 1 && x[1] === p);
      if (!d) return null;
      const page = unpack7(d.slice(2));
      if (page.length < 129 || page[0] !== hdr[5]) return null;
      bytes.set(page.slice(1, 129), p * 128);
    }
    return { wr: u16(hdr, 0), rd: u16(hdr, 2), lineLen: hdr[4]!, seq: hdr[5]!, bytes, lines: btLogLines(bytes) };
  }

  /** Fills the unused cave region R1 with the canary pattern (done by the engine loop). */
  async canaryArm(timeoutMs = 700): Promise<number | null> {
    const d = await this.request(NgOp.Canary, timeoutMs, [1], (x) => x[0] === 1);
    return d && d.length >= 2 ? d[1]! : null;
  }

  async canary(timeoutMs = 1500): Promise<NgCanary | null> {
    const d = await this.request(NgOp.Canary, timeoutMs, [0], (x) => x[0] === 0);
    return d ? decodeCanary(d) : null;
  }

  /** Runs the core self-test; the piano answers from its next engine-loop pass (~8 ms+). */
  async coreTest(timeoutMs = 1500): Promise<NgCoreTest | null> {
    const d = await this.request(NgOp.CoreTest, timeoutMs);
    return d ? decodeCoreTest(d) : null;
  }
}
