// Mirror of the piano's parameter store. The piano is the source of truth:
// values arrive in the AllParameter dump and as change notifications; local
// edits are applied optimistically and rolled back if the piano NACKs or the
// ACK never arrives.
import { paramByWire } from "../protocol/codecs.ts";
import type { ParamDef } from "../protocol/params.gen.ts";
import type { ParamMessage, PianoSession } from "../protocol/session.ts";

export type Listener = (key: string, data: number[]) => void;

export function keyOf(p: ParamDef, part: number): string {
  return `${p.name}@${part}`;
}

/** Parameters whose first data byte is a band: one address carries one frame per band (the stock dump sends
 * `51 1A 7F` four times and `51 1B 7F` twice, captures 261031), so each band is also kept under its own key. */
const BANDED = new Set(["UserEqGain", "UserEqFreq"]);

export function bandKey(p: ParamDef, part: number, band: number): string {
  return `${keyOf(p, part)}#${band}`;
}

export class ParamStore {
  private values = new Map<string, number[]>();
  private listeners = new Set<Listener>();
  private session: PianoSession | null = null;
  private rawListeners = new Set<(m: ParamMessage) => void>();
  /** the last value the piano confirmed per key (sent by it, or a set it ACKed): where a failed set goes back to */
  private confirmed = new Map<string, number[]>();
  /** per key: the number of the latest set — a failed set rolls back only when no newer set of that key followed */
  private setSeq = new Map<string, number>();

  attach(session: PianoSession): void {
    this.session = session;
    session.onParam((m) => {
      this.fromWire(m);
      for (const l of this.rawListeners) l(m);
    });
    // after every (re)sync: read what the AllParameter dump did not bring
    session.onState((s) => {
      if (s !== "ready" || this.session !== session) return;
      this.asked.clear();
      this.scheduleReadback();
    });
  }

  // ---- read-back of values the UI shows but the dump did not carry ----
  // The stock dump (02 5F 7F 7F) is the only bulk read, and a dump cut short (the stock's 16-bit
  // wait deadline drops a controller mid-dump) leaves
  // holes. Controls register what they show with want(); once the session is ready every wanted
  // value still unknown is asked for with a single get `02 v1 v2 part` (stock FUN_4015205C),
  // one at a time (the stock request task drops a request while it is answering another).
  private wanted = new Map<string, { p: ParamDef; part: number }>();
  private asked = new Set<string>();
  private readbackRun: Promise<void> | null = null;
  private readbackAgain = false;
  private readbackTimer: ReturnType<typeof setTimeout> | null = null;
  /** gets sent / answered with an end marker */
  readonly readbackStats = { requests: 0, answered: 0 };

  /** A control shows (p, part): read it back if the dump does not bring it. */
  want(p: ParamDef | undefined, part: number): void {
    if (!p || p.fn !== 0x10 || p.varLen) return;
    const key = keyOf(p, part);
    if (this.wanted.has(key)) return;
    this.wanted.set(key, { p, part });
    this.scheduleReadback();
  }

  /** Wanted keys with no value yet (for tests and the info screen). */
  missing(): string[] {
    return [...this.wanted.keys()].filter((k) => !this.values.has(k));
  }

  /** Wanted keys the piano did not answer even to a single get (stock: no value for them). */
  unreadable(): string[] {
    return this.missing().filter((k) => this.asked.has(k));
  }

  private scheduleReadback(): void {
    if (this.session?.state !== "ready" || this.readbackTimer) return;
    this.readbackTimer = setTimeout(() => {
      this.readbackTimer = null;
      void this.readback();
    }, 0);
  }

  /** Ask for every wanted value that is still unknown. Resolves when done. */
  readback(timeoutMs = 1500): Promise<void> {
    if (this.readbackRun) {
      this.readbackAgain = true;
      return this.readbackRun;
    }
    const run = async () => {
      do {
        this.readbackAgain = false;
        const s = this.session;
        if (!s || s.state !== "ready") return;
        // one get per wire id; several parts of one id -> part 7F (every stored part)
        const groups = new Map<string, { v1: number; v2: number; parts: Set<number>; keys: string[] }>();
        for (const [key, { p, part }] of this.wanted) {
          if (this.values.has(key) || this.asked.has(key)) continue;
          const id = `${p.v1}/${p.v2}`;
          const g = groups.get(id) ?? { v1: p.v1, v2: p.v2, parts: new Set<number>(), keys: [] };
          g.parts.add(part);
          g.keys.push(key);
          groups.set(id, g);
        }
        for (const g of groups.values()) {
          if (this.session !== s || s.state !== "ready") return;
          for (const k of g.keys) this.asked.add(k);
          const part = g.parts.size === 1 ? [...g.parts][0] : 0x7f;
          let ok = false;
          for (let i = 0; i < 2 && !ok && this.session === s && s.state === "ready"; i++) {
            this.readbackStats.requests++;
            ok = await s.requestData(g.v1, g.v2, timeoutMs, part);
          }
          if (ok) this.readbackStats.answered++;
        }
      } while (this.readbackAgain);
    };
    this.readbackRun = run().finally(() => {
      this.readbackRun = null;
    });
    return this.readbackRun;
  }

  /** Every parameter / mode / remote message from the piano, including OpenGrand
   * extensions that have no entry in the generated table (REMOTE_PROTOCOL.md §9). */
  onMessage(l: (m: ParamMessage) => void): () => void {
    this.rawListeners.add(l);
    return () => this.rawListeners.delete(l);
  }

  /** Send a message by wire id (for extension messages without a ParamDef). */
  send(m: { fn?: number; v1: number; v2: number; part?: number; data: number[] }): Promise<boolean> {
    const fn = m.fn ?? 0x10;
    const def = fn === 0x10 ? paramByWire(fn, m.v1, m.v2, m.part ?? 0x7f) : undefined;
    // known fixed-length parameters go through set() so the local mirror follows
    if (def && !def.varLen) return this.set(def, m.part ?? 0x7f, m.data);
    if (!this.session) return Promise.resolve(false);
    return this.session.sendParam({ fn: m.fn ?? 0x10, v1: m.v1, v2: m.v2, part: m.part ?? 0x7f, data: m.data });
  }

  /** Variable-length data request `02 v1 v2 7F` (false when not connected or timed out). */
  request(v1: number, v2: number, timeoutMs?: number): Promise<boolean> {
    return this.session ? this.session.requestData(v1, v2, timeoutMs) : Promise.resolve(false);
  }

  /** Read one value (or its whole group, v2 0) back with a single get `02 v1 v2 part` (false: not connected / no answer). */
  reread(p: ParamDef, part: number, timeoutMs = 1500): Promise<boolean> {
    if (!this.session || this.session.state === "displaced") return Promise.resolve(false);
    return this.session.requestData(p.v1, p.v2, timeoutMs, part);
  }

  get connected(): boolean {
    return this.session !== null;
  }

  get(p: ParamDef | undefined, part: number): number[] | undefined {
    return p ? this.values.get(keyOf(p, part)) : undefined;
  }

  /** The last frame of one band of a banded parameter (UserEqGain / UserEqFreq). */
  getBand(p: ParamDef | undefined, part: number, band: number): number[] | undefined {
    return p ? this.values.get(bandKey(p, part, band)) : undefined;
  }

  subscribe(l: Listener): () => void {
    this.listeners.add(l);
    return () => this.listeners.delete(l);
  }

  /** Apply a value locally and send it. Resolves false (and rolls back) on failure. */
  async set(p: ParamDef, part: number, data: number[]): Promise<boolean> {
    const key = BANDED.has(p.name) && data.length ? bandKey(p, part, data[0]) : keyOf(p, part);
    const before = this.values.get(key);
    if (!this.confirmed.has(key) && before) this.confirmed.set(key, before);
    const my = (this.setSeq.get(key) ?? 0) + 1;
    this.setSeq.set(key, my);
    this.put(key, data);
    if (!this.session) return true;
    const ok = await this.session.sendParam({ fn: p.fn, v1: p.v1, v2: p.v2, part, data });
    if (ok) this.confirmed.set(key, data.slice());
    // fast taps: a newer set of this key decides what is shown (its own rollback, if it fails, comes back to the
    // piano's value) — an older one going back to the value it replaced would hide the newer one
    else if (p.fn === 0x10 && this.setSeq.get(key) === my) {
      const was = this.confirmed.get(key);
      if (was) this.put(key, was);
      else this.values.delete(key);
    }
    return ok;
  }

  /** Fire a remote command / mode change without storing it. */
  command(p: ParamDef, part: number, data: number[]): Promise<boolean> {
    if (!this.session) return Promise.resolve(false);
    return this.session.sendParam({ fn: p.fn, v1: p.v1, v2: p.v2, part, data });
  }

  fromWire(m: ParamMessage): void {
    const p = paramByWire(m.fn, m.v1, m.v2, m.part);
    if (!p) return;
    this.confirmed.set(keyOf(p, m.part), m.data.slice());
    this.put(keyOf(p, m.part), m.data);
    if (BANDED.has(p.name) && m.data.length) {
      this.confirmed.set(bandKey(p, m.part, m.data[0]), m.data.slice());
      this.put(bandKey(p, m.part, m.data[0]), m.data);
    }
  }

  private put(key: string, data: number[]): void {
    this.values.set(key, data.slice());
    for (const l of this.listeners) l(key, data);
  }

  size(): number {
    return this.values.size;
  }
}
