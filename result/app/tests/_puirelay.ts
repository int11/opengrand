// The piano's panel-UI relay (cave ports/cave/relay_cave.c, slot payload ota_main feat2 0x04) as a test double,
// rule for rule (R1-R6), between the
// app's PanelUiClient (the phone, stock port 2) and the real panel loader (panelui/uiloader.js in a VM context,
// stock port 3). Not a test file itself: the runner only picks up *.test.ts.
//
// Modelled like the cave and the stock under it:
//   - a frame longer than 256 B or shorter than 8 B is dropped first (R3, length);
//   - op 0x30-0x3F from a port other than 3 -> port 3 verbatim, that port becomes the reply port (R1);
//     op 0x70-0x7F from port 3 -> the reply port verbatim, dropped without one (R2); other directions dropped (R3);
//   - whole or nothing (R4): a frame goes into the target TX ring only if count + length + 512 <= 0x7FE
//     (the stock writers spin at 0x7FE; the relay keeps 512 B for the stock's own frames), else it is dropped;
//   - the port 3 ring drains at `panelBytesPerMs` (panel link); the panel gets a frame when its last byte is out,
//     its replies come back through the port 2 ring to the phone after `bleDelayMs`.
import { runInNewContext } from "node:vm";

import { loaderSource, type UiConfig } from "../scripts/panelpkg.ts";
import type { PuiPort } from "../src/protocol/panelui.ts";

export const RL_FRAME_MAX = 256;
export const RL_RESERVE = 512;
export const RING_LIMIT = 0x7fe;

/** localStorage double (plain map, no quota) */
export class MemStorage {
  readonly m = new Map<string, string>();
  getItem(k: string) {
    return this.m.has(k) ? this.m.get(k)! : null;
  }
  setItem(k: string, v: string) {
    this.m.set(k, String(v));
  }
  removeItem(k: string) {
    this.m.delete(k);
  }
}

/** One page load of the panel loader (the OpenGrand chooser / UI page) with a fake KWM shell. */
export function panelLoader(storage: MemStorage, cfg: UiConfig) {
  const sent: number[][] = [];
  const ctx: Record<string, unknown> = {
    localStorage: storage,
    setTimeout: (f: () => void, ms: number) => setTimeout(f, ms).unref(),
    clearTimeout,
    location: { search: "?embedded=1", replace() {} },
    kwmcore: {
      postMessage(url: string) {
        if (url.startsWith("kwmcore://sendMidiMessage/")) sent.push(JSON.parse(url.slice(url.indexOf("?") + 1)).msg);
      },
    },
    addEventListener() {},
  };
  runInNewContext(loaderSource(cfg), ctx, { filename: "uiloader.js" });
  const ui = ctx.CA79UI as { hookKwm(k: Record<string, unknown>): void; setPage(p: number): void; navigate: (u: string) => void; state(): { slots: Record<string, { seq: number } | undefined> } };
  ui.navigate = () => {};
  ui.setPage(0);
  const KWM: Record<string, unknown> = { onMidiMessage: () => {} };
  ui.hookKwm(KWM);
  return { ui, sent, deliver: (m: number[]) => (KWM.onMidiMessage as (a: number[], id: string, t: string) => void)(m, "USB", "0") };
}

export interface RelayStats {
  toPanel: number;
  fromPanel: number;
  dropLen: number;
  dropDir: number;
  dropNoSrc: number;
  dropFull: number;
  peak3: number;
}

export class PanelRelayModel {
  readonly stats: RelayStats = { toPanel: 0, fromPanel: 0, dropLen: 0, dropDir: 0, dropNoSrc: 0, dropFull: 0, peak3: 0 };
  /** what the stock has queued on port 3 besides us (a dump in flight), bytes */
  stockBacklog3 = 0;
  /** the panel page: null = not on the OpenGrand chooser / UI (the Kawai UI ignores 0x7D) */
  panel: ReturnType<typeof panelLoader> | null;
  /** the panel answers nothing until this time (page reload, busy) */
  stallUntil = 0;
  panelBytesPerMs = 3.125;
  bleDelayMs = 1;
  /** every frame that left on port 3 / port 2, in order */
  readonly port3: Uint8Array[] = [];
  readonly port2: Uint8Array[] = [];
  readonly other: [number, Uint8Array][] = [];
  private src = -1;
  private ring3: Uint8Array[] = [];
  private ring3Bytes = 0;
  private listeners: ((m: Uint8Array) => void)[] = [];
  private timer: ReturnType<typeof setInterval> | null = null;

  constructor(panel: ReturnType<typeof panelLoader> | null) {
    this.panel = panel;
  }

  /** The phone's side (stock port 2). */
  phone(): PuiPort {
    return {
      sendRaw: (m) => setTimeout(() => this.rx(2, m), this.bleDelayMs),
      onMidi: (fn) => {
        this.listeners.push(fn);
        return () => {
          this.listeners = this.listeners.filter((l) => l !== fn);
        };
      },
    };
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  private count3(): number {
    return this.ring3Bytes + this.stockBacklog3;
  }

  /** ng_sysex -> rlc_sysex: a whole 0x7D frame of the relay's ops from stock port `port` */
  rx(port: number, m: Uint8Array): void {
    if (m.length < 8 || m[0] !== 0xf0 || m[1] !== 0x7d || m[m.length - 1] !== 0xf7) return;
    const op = m[6]!;
    const req = op >= 0x30 && op <= 0x3f;
    const rep = op >= 0x70 && op <= 0x7f;
    if (!req && !rep) return; // not the relay's (the cave answers or ignores it)
    if (m.length > RL_FRAME_MAX) return void this.stats.dropLen++;
    if (req) {
      if (port === 3) return void this.stats.dropDir++;
      this.src = port;
      const cnt = this.count3();
      this.stats.peak3 = Math.max(this.stats.peak3, cnt);
      if (cnt + m.length + RL_RESERVE > RING_LIMIT) return void this.stats.dropFull++;
      this.stats.toPanel++;
      this.port3.push(m);
      this.ring3.push(m);
      this.ring3Bytes += m.length;
      this.drain();
      return;
    }
    if (port !== 3) return void this.stats.dropDir++;
    if (this.src < 0) return void this.stats.dropNoSrc++;
    // the reply ring (port 2) is idle in these tests: the stock writer takes it whole
    this.stats.fromPanel++;
    if (this.src !== 2) return void this.other.push([this.src, m]); // DIN / USB asked: not the phone's
    this.port2.push(m);
    setTimeout(() => this.listeners.forEach((l) => l(m)), this.bleDelayMs);
  }

  /** the panel link: drains the port 3 ring; a frame reaches the panel when its last byte is through */
  private drain(): void {
    if (this.timer) return;
    let credit = 0;
    let last = Date.now();
    this.timer = setInterval(() => {
      const now = Date.now();
      credit += (now - last) * this.panelBytesPerMs;
      last = now;
      if (this.stockBacklog3 > 0) {
        const n = Math.min(this.stockBacklog3, Math.floor(credit));
        this.stockBacklog3 -= n;
        credit -= n;
      }
      while (this.ring3.length && this.stockBacklog3 === 0 && credit >= this.ring3[0]!.length) {
        const f = this.ring3.shift()!;
        credit -= f.length;
        this.ring3Bytes -= f.length;
        this.toPanel(f);
      }
      if (!this.ring3.length && this.stockBacklog3 === 0) this.stop();
    }, 1);
  }

  private toPanel(f: Uint8Array): void {
    const p = this.panel;
    if (!p || Date.now() < this.stallUntil) return;
    const before = p.sent.length;
    p.deliver(Array.from(f));
    for (const r of p.sent.splice(before)) this.rx(3, Uint8Array.from(r));
  }
}
