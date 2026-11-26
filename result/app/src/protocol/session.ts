// Remote-control session with the piano.
//
// Observed connect sequence (original app and firmware handler 0x4014a2c8):
//   1. app  -> F0 40 7F 7E 08 02 01 01 F7               (Sync / CONNECT)
//   2. piano-> F0 40 dd 7E 04 1E 02 .. F7               (Mode/connect status; dd = device id)
//   3. app  -> RequestModel 05 40, RequestDestination 05 41, RequestLanguage 05 42, RequestVersion 04 41
//      (each sent after the previous reply, as the original app does)
//   4. app  -> F0 40 7F 02 08 02 5F 7F 7F F7            (request AllParameter)
//      piano-> fn 0x10 parameter messages ..., then fn 0x03 5F 7F 7F (end of data)
// ACK rules (original app's handshake):
//   - outgoing fn 0x03/0x05/0x09/0x10 are queued; the next is sent only after
//     an ACK (fn 0x07) echoing bytes 6..8 — 1 s timeout, up to 16 sends;
//   - fn 0x31 (mode) is never ACKed, fn 0x32 (remote) is not awaited;
//   - every fn 0x10 received from the piano is ACKed by the controller.
import { APP_GROUP, APP_MACHINE, DEV_BROADCAST, Fn, ackKey, appFrame, decodeFrame, type KawaiFrame } from "./sysex.ts";
import type { MidiLink } from "../transport/link.ts";
import { NgOp, ngNotBound, parseNgReply } from "./ngext/wire.ts";

/**
 * Why the piano may have forgotten this controller's binding while the session looks fine (field 2026-11-09): `apply` = an OTA apply now finished (the new payload's session core
 * starts with an empty binding table), `identity` = the OpenGrand Identify reply changed (another payload runs),
 * `notBound` = a bound-only OpenGrand op answered "not bound".
 */
export type RebindReason = "apply" | "identity" | "notBound";

export const SysCmd = {
  PowerOff: [0x00, 0x00],
  Async: [0x01, 0x00],
  Sync: [0x01, 0x01],
  Mode: [0x02],
  Result: [0x03],
  RequestVersion: [0x04, 0x41],
  Model: [0x05, 0x00],
  Destination: [0x05, 0x01],
  Language: [0x05, 0x02],
  RequestModel: [0x05, 0x40],
  RequestDestination: [0x05, 0x41],
  RequestLanguage: [0x05, 0x42],
} as const;

const HANDSHAKE_FNS = new Set<number>([Fn.EndOfData, Fn.Cancel, Fn.SaveStart, Fn.Param]);

export interface PianoIdentity {
  group: number;
  machine: number;
  sub1: number;
  sub2: number;
  model: string; // resolved name, e.g. "CA79"
  destination?: number;
  language?: number;
  version?: string;
}

/** Known identities (model.json subset relevant to this design). */
export const MODELS: { group: number; machine: number; sub1: number; sub2: number; name: string }[] = [
  { group: 0x04, machine: 0x1e, sub1: 0x00, sub2: 0x02, name: "CA79" },
  { group: 0x04, machine: 0x1e, sub1: 0x00, sub2: 0x01, name: "CA99" },
  { group: 0x04, machine: 0x1e, sub1: 0x00, sub2: 0x04, name: "CA9900GP" },
];

export interface ParamMessage {
  fn: number;
  v1: number;
  v2: number;
  part: number;
  data: number[];
}

export interface SessionOptions {
  ackTimeoutMs?: number; // default 1000
  maxRetries?: number; // default 16
  handshake?: boolean; // wait for ACK per message (default true)
  connectTries?: number; // sends of CONNECT / RequestModel within the connect budget (default 4)
  now?: () => number;
  /** Diagnostics: one line per session event (state change, dropped send, given-up message). */
  trace?: (line: string) => void;
  setTimer?: (fn: () => void, ms: number) => unknown;
  clearTimer?: (h: unknown) => void;
}

/**
 * "displaced": the piano handed the controller binding to another port (`7E 01 00`, stock: a
 * CONNECT from the panel or another app). The link is fine, but the stock answers nothing
 * from an unbound port, so queued requests are dropped and new ones fail at once instead of
 * being resent 16 times each. takeControl() sends CONNECT again and re-syncs.
 */
export type SessionState = "idle" | "connecting" | "syncing" | "ready" | "displaced" | "closed";
type State = SessionState;

/** Thrown by helpers that stop a multi-step job when the session is displaced. */
export class DisplacedError extends Error {
  constructor() {
    super("another device took control");
    this.name = "DisplacedError";
  }
}

interface Pending {
  msg: Uint8Array;
  key: string;
  tries: number;
  resolve: (ok: boolean) => void;
  timer?: unknown;
}

export class PianoSession {
  state: State = "idle";
  deviceId = DEV_BROADCAST;
  identity: PianoIdentity | null = null;
  private readonly link: MidiLink;
  private readonly opt: Required<Omit<SessionOptions, "now" | "trace">>;
  private readonly trace: (line: string) => void;
  private queue: Pending[] = [];
  private inflight: Pending | null = null;
  private waiters: { match: (f: KawaiFrame) => boolean; resolve: (f: KawaiFrame) => void; reject: (e: Error) => void; timer: unknown }[] = [];
  private paramListeners: ((p: ParamMessage, fromPiano: boolean) => void)[] = [];
  private midiListeners: ((m: Uint8Array) => void)[] = [];
  private stateListeners: ((s: State) => void)[] = [];
  private frameListeners: ((f: KawaiFrame) => void)[] = [];
  private inviteListeners: (() => void)[] = [];
  private rebindListeners: ((why: RebindReason) => void)[] = [];
  /** the last OpenGrand Identify reply seen on this session (hex), null before the first / after apply now */
  private ngIdentify: string | null = null;
  private dumpDone: (() => void) | null = null;
  private dumpFail: ((e: Error) => void) | null = null;
  private dumpTouch: (() => void) | null = null;
  stats = { sent: 0, acked: 0, retried: 0, failed: 0, received: 0, dropped: 0, displaced: 0, invited: 0, connectRetries: 0, dumpFrames: 0 };

  constructor(link: MidiLink, opt: SessionOptions = {}) {
    this.link = link;
    this.opt = {
      ackTimeoutMs: opt.ackTimeoutMs ?? 1000,
      maxRetries: opt.maxRetries ?? 16,
      handshake: opt.handshake ?? true,
      connectTries: opt.connectTries ?? 4,
      setTimer: opt.setTimer ?? ((fn, ms) => setTimeout(fn, ms)),
      clearTimer: opt.clearTimer ?? ((h) => clearTimeout(h as ReturnType<typeof setTimeout>)),
    };
    this.trace = opt.trace ?? (() => {});
    link.onMessage((m) => this.receive(m));
    link.onClose(() => this.setState("closed"));
  }

  /**
   * One-line queue state for logs: the session state, the message waiting for its ACK (key and send count)
   * and how many wait behind it, e.g. `ready inflight=55 08 00#2 queued=1`.
   */
  describe(): string {
    const hex = (k: string) => k.match(/../g)?.join(" ") ?? k;
    const p = this.inflight;
    return `${this.state}${p ? ` inflight=${hex(p.key)}#${p.tries}` : ""}${this.queue.length ? ` queued=${this.queue.length}` : ""}`;
  }

  onParam(fn: (p: ParamMessage, fromPiano: boolean) => void): void {
    this.paramListeners.push(fn);
  }
  /** Non-Kawai MIDI (notes, BLE module SysEx …). Returns an unsubscribe function. */
  onMidi(fn: (m: Uint8Array) => void): () => void {
    this.midiListeners.push(fn);
    return () => {
      this.midiListeners = this.midiListeners.filter((x) => x !== fn);
    };
  }
  /** Every Kawai frame from the piano (fn 0x24 file access, 7E 03 results …). Returns an unsubscribe function. */
  onFrame(fn: (f: KawaiFrame) => void): () => void {
    this.frameListeners.push(fn);
    return () => {
      this.frameListeners = this.frameListeners.filter((x) => x !== fn);
    };
  }

  /**
   * `7E 01 01` from the piano: "CONNECT again". The stock sends it to the panel's port only, when the
   * controller that held the binding went away (FUN_4014C080); OpenGrand also sends it after a live
   * payload swap (apply now), whose new session core no longer knows the panel was bound
   * The original panel app answers with CONNECT +
   * a full parameter request; the connection manager does the same (takeControl). Returns an
   * unsubscribe function.
   */
  onInvite(fn: () => void): () => void {
    this.inviteListeners.push(fn);
    return () => {
      this.inviteListeners = this.inviteListeners.filter((x) => x !== fn);
    };
  }

  /**
   * The piano may have dropped this controller's binding without a `7E 01 00` / `7E 01 01` (see RebindReason):
   * the connection manager answers with CONNECT + a full re-sync (takeControl). Returns an unsubscribe function.
   */
  onRebind(fn: (why: RebindReason) => void): () => void {
    this.rebindListeners.push(fn);
    return () => {
      this.rebindListeners = this.rebindListeners.filter((x) => x !== fn);
    };
  }

  /** An OTA apply now finished: a new payload runs, and its session core knows no binding but the stock one. */
  noteSwapped(): void {
    this.ngIdentify = null; // the new payload's Identify is the new baseline, not a second change
    this.rebind("apply");
  }

  private rebind(why: RebindReason): void {
    if (this.released || this.state === "closed" || this.state === "idle") return;
    this.trace(`the piano may have dropped our binding (${why})`);
    for (const l of [...this.rebindListeners]) l(why);
  }

  /** Non-Kawai MIDI from the piano: a OpenGrand Identify that changed, a "not bound" answer. */
  private watchNg(msg: Uint8Array): void {
    if (msg[0] !== 0xf0 || msg[1] !== 0x7d) return;
    if (ngNotBound(msg)) return this.rebind("notBound");
    const r = parseNgReply(msg);
    if (r?.op !== NgOp.Identify) return;
    const id = Array.from(r.data, (b) => b.toString(16).padStart(2, "0")).join("");
    const was = this.ngIdentify;
    this.ngIdentify = id;
    if (was !== null && was !== id) this.rebind("identity");
  }

  /** Returns an unsubscribe function. */
  onState(fn: (s: State) => void): () => void {
    this.stateListeners.push(fn);
    return () => {
      this.stateListeners = this.stateListeners.filter((x) => x !== fn);
    };
  }

  /**
   * Full connect: Sync, identity queries, AllParameter dump.
   *
   * `timeoutMs` is the budget for each reply. Within it the request is sent up to
   * `connectTries` times (one every timeoutMs / 3, at least 250 ms): the first CONNECT after a
   * fresh BLE link or a piano boot is often lost, and
   * every request here is idempotent on the stock — a CONNECT from the port that is already bound
   * only re-sends the `7E 02` reply (stock 0x4014A75C–0x4014A7A4, no `7E 01 00`, no queue reset).
   */
  connect(timeoutMs = 3000): Promise<PianoIdentity> {
    // one CONNECT + re-sync at a time: a second caller (the file client's recover, a re-bind) joins the one under way —
    // two would share the waiters and the dump handlers, and the second dump request ends the first one's wait
    if (this.connecting) return this.connecting;
    const run: Promise<PianoIdentity> = this.connect1(timeoutMs).finally(() => {
      if (this.connecting === run) this.connecting = null;
    });
    this.connecting = run;
    return run;
  }

  private connecting: Promise<PianoIdentity> | null = null;

  private async connect1(timeoutMs: number): Promise<PianoIdentity> {
    this.setState("connecting");
    const step = Math.max(250, Math.floor(timeoutMs / 3));
    const tries = this.opt.connectTries;
    const sys = (match: (f: KawaiFrame) => boolean, body: number[], n: number, dev = this.deviceId) =>
      this.ask(() => appFrame(Fn.System, body, dev), (f) => f.fn === Fn.System && match(f), step, n);

    const mode = await sys((f) => f.body[0] === 0x02, [...SysCmd.Sync], tries, DEV_BROADCAST);
    this.deviceId = mode.dev;

    const m = await sys((f) => f.body[0] === 0x05 && f.body[1] === 0x00, [...SysCmd.RequestModel], tries);
    const sub1 = m.body[2] ?? 0;
    const sub2 = m.body[3] ?? 0;
    const known = MODELS.find((x) => x.group === m.group && x.machine === m.machine && x.sub1 === sub1 && x.sub2 === sub2);
    this.identity = { group: m.group, machine: m.machine, sub1, sub2, model: known?.name ?? `unknown ${m.group}/${m.machine}` };

    // Destination and language, in the original app's order (golden trace
    // 261031_phone_pianoremote_1.1.5: 05 40 -> 05 41 -> 05 42 -> 04 41). Optional: two tries each,
    // but a displacement still aborts the whole connect.
    const optional = (p: Promise<KawaiFrame>) =>
      p.catch((e) => {
        if (e instanceof DisplacedError) throw e;
        return null;
      });
    const d = await optional(sys((f) => f.body[0] === 0x05 && f.body[1] === 0x01, [...SysCmd.RequestDestination], 2));
    if (d) this.identity.destination = d.body[d.body.length - 1];
    const l = await optional(sys((f) => f.body[0] === 0x05 && f.body[1] === 0x02, [...SysCmd.RequestLanguage], 2));
    if (l) this.identity.language = l.body[2];
    const v = await optional(sys((f) => f.body[0] === 0x04 && f.body[1] === 0x01, [...SysCmd.RequestVersion], 2));
    if (v) this.identity.version = String.fromCharCode(...v.body.slice(2).filter((b) => b >= 0x20));

    this.setState("syncing");
    try {
      await this.requestAll(timeoutMs * 2);
    } catch (e) {
      if (e instanceof DisplacedError || this.state !== "syncing") throw e;
      // The stock drops a controller without a word when one dump value goes unACKed through its 16
      // sends (FUN_4014BD1C). Its wait has a 16-bit deadline (FUN_4014BCAC), so for 0.5 s in every 131 s
      // each wait times out at once and values are resent every ~5 ms — a sync can lose its binding even
      // though we ACK everything (field 2026-10-31). CONNECT
      // again and ask once more.
      const again = this.waitFor((f) => f.fn === Fn.System && f.body[0] === 0x02, timeoutMs);
      this.link.send(appFrame(Fn.System, [...SysCmd.Sync]));
      await again;
      await this.requestAll(timeoutMs * 2);
    }
    this.setState("ready");
    return this.identity;
  }

  /**
   * Send a request and wait for the matching reply, re-sending every `stepMs` up to `tries` times.
   * A displacement (`7E 01 00`) rejects at once with DisplacedError and stops the re-sends.
   */
  private async ask(make: () => Uint8Array, match: (f: KawaiFrame) => boolean, stepMs: number, tries: number): Promise<KawaiFrame> {
    let last: Error = new Error("timeout waiting for piano reply");
    for (let i = 0; i < tries; i++) {
      if (this.state === "closed" || this.released) break;
      const reply = this.waitFor(match, stepMs);
      if (i > 0) this.stats.connectRetries++;
      this.link.send(make());
      try {
        return await reply;
      } catch (e) {
        if (e instanceof DisplacedError) throw e;
        last = e as Error;
      }
    }
    throw new Error(`${last.message} (${tries} tries)`);
  }

  /** CONNECT again after being displaced (7E 01 00) and re-sync everything. */
  takeControl(timeoutMs = 3000): Promise<PianoIdentity> {
    return this.connect(timeoutMs);
  }

  get displaced(): boolean {
    return this.state === "displaced";
  }

  /**
   * Request the full parameter set; resolves on fn 0x03 5F 7F (end of data).
   *
   * The stock sends the dump stop-and-wait — one fn 0x10 per controller ACK, ~235 of them — so
   * its length is ~235 round trips: 7.9 s on the phone capture (33 ms RTT), longer on a link
   * with a slower connection interval. A fixed deadline therefore fails healthy dumps. Instead
   * `idleMs` is the longest silence allowed between frames from the piano, and `capMs` bounds
   * the whole dump. If nothing at all arrives in the first idle window the request is sent once
   * more (it is idempotent: a lost request just starts the dump).
   */
  requestAll(idleMs = 6000, capMs = 120000): Promise<void> {
    return new Promise((resolve, reject) => {
      let idle: unknown;
      let resent = false;
      const finish = (e: Error | null) => {
        this.opt.clearTimer(idle);
        this.opt.clearTimer(cap);
        this.dumpDone = this.dumpFail = this.dumpTouch = null;
        if (e) reject(e);
        else resolve();
      };
      const send = () => this.link.send(appFrame(Fn.Request, [0x5f, 0x7f, 0x7f], this.deviceId));
      const arm = () => {
        this.opt.clearTimer(idle);
        idle = this.opt.setTimer(() => {
          if (this.stats.dumpFrames === 0 && !resent) {
            resent = true;
            this.stats.connectRetries++;
            send();
            arm();
            return;
          }
          finish(new Error(`parameter dump timed out (${this.stats.dumpFrames} frames, idle ${idleMs} ms)`));
        }, idleMs);
      };
      const cap = this.opt.setTimer(() => finish(new Error(`parameter dump timed out (${this.stats.dumpFrames} frames, cap ${capMs} ms)`)), capMs);
      this.stats.dumpFrames = 0;
      this.dumpTouch = () => {
        this.stats.dumpFrames++;
        arm();
      };
      this.dumpDone = () => finish(null);
      this.dumpFail = (e) => finish(e);
      arm();
      send();
    });
  }

  /**
   * Variable-length data request (REMOTE_PROTOCOL.md §9.0): `02 v1 v2 7F`; the piano
   * answers with fn 0x10 messages (delivered to onParam) and ends with `03 v1 v2 7F`.
   * Resolves true on the end marker, false on timeout.
   * `part` 7F = every stored part, any other = that part only; the stock end marker is always
   * `03 v1 v2 7F` (FUN_4014BDC8). v2 00 means the whole group on the stock (FUN_4015205C).
   */
  requestData(v1: number, v2: number, timeoutMs = 3000, part = 0x7f): Promise<boolean> {
    if (this.state === "displaced") {
      // not bound: the stock drops a request from an unbound port without a word (FUN_4014A2C8 fn 0x02 gate)
      this.stats.dropped++;
      this.trace(`drop request ${v1.toString(16)} ${v2.toString(16)} (displaced)`);
      return Promise.resolve(false);
    }
    return new Promise((resolve) => {
      const timer = this.opt.setTimer(() => {
        off();
        resolve(false);
      }, timeoutMs);
      const off = this.onFrame((f) => {
        if (f.fn === Fn.EndOfData && f.body[0] === v1 && f.body[1] === v2) {
          this.opt.clearTimer(timer);
          off();
          resolve(true);
        }
      });
      this.link.send(appFrame(Fn.Request, [v1, v2, part & 0x7f], this.deviceId));
    });
  }

  /** Send a parameter (fn 0x10 by default). Resolves true when ACKed. */
  sendParam(p: Omit<ParamMessage, "fn"> & { fn?: number }): Promise<boolean> {
    const msg = appFrame(p.fn ?? Fn.Param, [p.v1, p.v2, p.part, ...p.data], this.deviceId);
    return this.enqueue(msg);
  }

  sendRaw(msg: Uint8Array): void {
    if (!this.released) this.link.send(msg);
  }

  /**
   * DISCONNECT (`7E 01 00`): the piano unbinds this port and the stock hands control back to the
   * panel (`7E 01 01` to port 3, which then CONNECTs). Only for leaving the piano (user
   * "Disconnect", app closed) — never when replacing a session on the same port: the panel's
   * CONNECT would displace the new session.
   */
  disconnect(): void {
    if (!this.released) this.link.send(appFrame(Fn.System, [...SysCmd.Async], this.deviceId));
    this.release();
  }

  /**
   * Retire the session without a word to the piano: nothing more is sent or handled, pending
   * requests fail, the state becomes "closed". Used when a new session takes over the same port
   * (a CONNECT from the bound port is idempotent on the stock, so no DISCONNECT is needed).
   */
  release(): void {
    if (this.released) return;
    this.released = true;
    const pending = [...(this.inflight ? [this.inflight] : []), ...this.queue];
    if (this.inflight) this.opt.clearTimer(this.inflight.timer);
    this.inflight = null;
    this.queue = [];
    const waiters = this.waiters;
    this.waiters = [];
    for (const p of pending) p.resolve(false);
    for (const w of waiters) {
      this.opt.clearTimer(w.timer);
      w.reject(new Error("session released"));
    }
    this.dumpFail?.(new Error("session released"));
    this.setState("closed");
  }

  private released = false;

  // ---------------- internals ----------------

  private setState(s: State): void {
    if (s !== this.state) this.trace(`state ${this.state} -> ${s}`);
    this.state = s;
    for (const l of [...this.stateListeners]) l(s);
  }

  private enqueue(msg: Uint8Array): Promise<boolean> {
    if (this.released) return Promise.resolve(false);
    if (this.state === "displaced") {
      // not bound any more: the stock would never ACK (and never answer), so do not send
      this.stats.dropped++;
      this.trace(`drop ${ackKey(msg).match(/../g)?.join(" ") ?? ""} (displaced)`);
      return Promise.resolve(false);
    }
    if (!this.opt.handshake || !HANDSHAKE_FNS.has(msg[3])) {
      this.link.send(msg);
      this.stats.sent++;
      return Promise.resolve(true);
    }
    return new Promise((resolve) => {
      this.queue.push({ msg, key: ackKey(msg), tries: 0, resolve });
      this.pump();
    });
  }

  private pump(): void {
    if (this.inflight || this.queue.length === 0) return;
    const p = this.queue.shift()!;
    this.inflight = p;
    this.transmit(p);
  }

  private transmit(p: Pending): void {
    p.tries++;
    this.stats.sent++;
    if (p.tries > 1) this.stats.retried++;
    this.link.send(p.msg);
    p.timer = this.opt.setTimer(() => {
      if (this.inflight !== p) return;
      if (p.tries >= this.opt.maxRetries) {
        this.stats.failed++;
        this.trace(`give up ${p.key.match(/../g)?.join(" ") ?? p.key} after ${p.tries} sends`);
        this.inflight = null;
        p.resolve(false);
        this.pump();
      } else {
        this.transmit(p);
      }
    }, this.opt.ackTimeoutMs);
  }

  private settle(key: string, ok: boolean): void {
    const p = this.inflight;
    if (!p || p.key !== key) return;
    this.opt.clearTimer(p.timer);
    this.inflight = null;
    if (ok) this.stats.acked++;
    else this.stats.failed++;
    p.resolve(ok);
    this.pump();
  }

  /** 7E 01 00 from the piano: drop everything in flight, keep the link. */
  private displace(): void {
    this.stats.displaced++;
    const pending = [...(this.inflight ? [this.inflight] : []), ...this.queue];
    if (this.inflight) this.opt.clearTimer(this.inflight.timer);
    this.inflight = null;
    this.queue = [];
    this.stats.dropped += pending.length;
    const waiters = this.waiters;
    this.waiters = [];
    this.setState("displaced");
    for (const p of pending) p.resolve(false);
    for (const w of waiters) {
      this.opt.clearTimer(w.timer);
      w.reject(new DisplacedError());
    }
    this.dumpFail?.(new DisplacedError());
  }

  private waitFor(match: (f: KawaiFrame) => boolean, timeoutMs: number): Promise<KawaiFrame> {
    return new Promise((resolve, reject) => {
      const w = {
        match,
        resolve,
        reject,
        timer: this.opt.setTimer(() => {
          this.waiters = this.waiters.filter((x) => x !== w);
          reject(new Error("timeout waiting for piano reply"));
        }, timeoutMs),
      };
      this.waiters.push(w);
    });
  }

  private receive(msg: Uint8Array): void {
    if (this.released) return; // a newer session owns this port
    this.stats.received++;
    const f = decodeFrame(msg);
    if (!f) {
      for (const l of [...this.midiListeners]) l(msg);
      this.watchNg(msg); // after the listeners: a client waiting for this reply has it before a re-sync starts
      return;
    }
    // ignore our own echo (e.g. a MIDI thru loop)
    if (f.group === APP_GROUP && f.machine === APP_MACHINE) return;

    this.dumpTouch?.();
    for (const l of [...this.frameListeners]) l(f);
    for (const w of [...this.waiters]) {
      if (w.match(f)) {
        this.opt.clearTimer(w.timer);
        this.waiters = this.waiters.filter((x) => x !== w);
        w.resolve(f);
      }
    }
    switch (f.fn) {
      case Fn.System:
        // 7E 01 00 piano -> controller: the binding went to another port (stock: a CONNECT from
        // another port, e.g. the panel). Ignored before our first CONNECT and after close.
        if (f.body[0] === 0x01 && f.body[1] === 0x00 && this.state !== "closed" && this.state !== "idle") this.displace();
        // 7E 01 01 piano -> controller: CONNECT again (see onInvite). Ignored before our first CONNECT.
        else if (f.body[0] === 0x01 && f.body[1] === 0x01 && f.body.length === 2 && this.state !== "closed" && this.state !== "idle") {
          this.stats.invited++;
          this.trace("piano asks us to CONNECT again (7E 01 01)");
          for (const l of [...this.inviteListeners]) l();
        }
        break;
      case Fn.Ack:
        this.settle(ackKey(msg), true);
        break;
      case Fn.Nack:
        this.settle(ackKey(msg), false);
        break;
      case Fn.EndOfData:
        // The stock firmware resends EndOfData every 0.5 s until it is ACKed and
        // unbinds the controller after 16 tries (the original app answers 07 5F 7F 7F).
        this.link.send(appFrame(Fn.Ack, [f.body[0] ?? 0x5f, f.body[1] ?? 0x7f, f.body[2] ?? 0x7f], this.deviceId));
        if (f.body[0] === 0x5f && f.body[1] === 0x7f) this.dumpDone?.();
        break;
      case Fn.Request:
        // The piano asks us for our state (fn 02 5F 7F): we hold no state of our
        // own, so answer with an immediate end-of-data.
        if (f.body[0] === 0x5f) this.link.send(appFrame(Fn.EndOfData, [0x5f, 0x7f, 0x7f], this.deviceId));
        break;
      case Fn.Param:
      case Fn.Mode:
      case Fn.Remote: {
        const [v1, v2, part, ...data] = f.body;
        if (f.fn === Fn.Param) this.link.send(appFrame(Fn.Ack, [v1, v2, part ?? 0x7f], this.deviceId));
        const p: ParamMessage = { fn: f.fn, v1, v2, part: part ?? 0x7f, data };
        for (const l of this.paramListeners) l(p, true);
        break;
      }
      default:
        break;
    }
  }
}
