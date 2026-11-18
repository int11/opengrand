// User touch curve (feature 13, REMOTE_PROTOCOL.md §9.1) and per-note VT editors
// (feature 35, §9.2): data models and the messages that edit them.
import { findParam } from "../protocol/codecs.ts";
import type { ParamMessage } from "../protocol/session.ts";
import type { ParamStore } from "../state/store.ts";

const USER = 0x40;

// ---------------- touch curve ----------------

/** Linear interpolation between existing points; ends fixed at 0->0 and 127->127 (§9.1). */
export function interpolate(exist: boolean[], vel: number[]): number[] {
  const xs = [0];
  const ys = [0];
  for (let i = 1; i < 127; i++) if (exist[i]) { xs.push(i); ys.push(vel[i]); }
  xs.push(127);
  ys.push(127);
  const out: number[] = [];
  for (let i = 0, seg = 0; i < 128; i++) {
    while (seg < xs.length - 2 && i > xs[seg + 1]) seg++;
    const x0 = xs[seg], x1 = xs[seg + 1], y0 = ys[seg], y1 = ys[seg + 1];
    out.push(Math.round(x1 === x0 ? y1 : y0 + ((y1 - y0) * (i - x0)) / (x1 - x0)));
  }
  return out;
}

export function encodeName(name: string): number[] {
  const out: number[] = [];
  for (const ch of [...name].slice(0, 16)) {
    const c = ch.codePointAt(0)!;
    out.push(c & 0x7f, (c >> 7) & 0x7f, (c >> 14) & 0x7f);
  }
  return out;
}

export function decodeName(d: number[]): string {
  let s = "";
  for (let i = 0; i + 2 < d.length; i += 3) s += String.fromCodePoint(d[i] | (d[i + 1] << 7) | (d[i + 2] << 14));
  return s;
}

export class TouchCurveModel {
  exist: boolean[] = Array(128).fill(false);
  vel: number[] = Array.from({ length: 128 }, (_, i) => i);
  name = "";
  private readonly store: ParamStore;
  private listeners = new Set<() => void>();
  private pending: { exist: boolean[]; vel: number[] } | null = null;
  private off: () => void;

  constructor(store: ParamStore) {
    this.store = store;
    this.off = store.onMessage((m) => this.fromPiano(m));
  }

  dispose(): void {
    this.off();
  }

  onChange(l: () => void): () => void {
    this.listeners.add(l);
    return () => this.listeners.delete(l);
  }

  private changed(): void {
    for (const l of this.listeners) l();
  }

  /** Piano -> controller: 5A 0C name, 5A 04/05…/06 chunks, 5A 0D bulk, 31 5A 09 edit exit. */
  fromPiano(m: ParamMessage): void {
    if (m.v1 !== 0x5a) return;
    if (m.fn === 0x31 && m.v2 === 0x09) {
      void this.load(); // another controller discarded its edit: re-read the stored curve
      return;
    }
    if (m.fn !== 0x10) return;
    const d = m.data;
    switch (m.v2) {
      case 0x0c:
        this.name = decodeName(d.slice(1));
        this.changed();
        break;
      case 0x04:
        this.pending = { exist: this.exist.slice(), vel: this.vel.slice() };
        break;
      case 0x05: {
        const p = this.pending ?? { exist: this.exist.slice(), vel: this.vel.slice() };
        this.pending = p;
        for (let i = 0; 2 + 2 * i + 1 < d.length && d[1] + i < 128; i++) {
          p.exist[d[1] + i] = d[2 + 2 * i] !== 0;
          p.vel[d[1] + i] = d[3 + 2 * i];
        }
        break;
      }
      case 0x06:
        if (this.pending) ({ exist: this.exist, vel: this.vel } = this.pending);
        this.pending = null;
        this.changed();
        break;
      case 0x0d:
        for (let i = 0; i < 128 && 2 + 2 * i < d.length; i++) {
          this.exist[i] = d[1 + 2 * i] !== 0;
          this.vel[i] = d[2 + 2 * i];
        }
        this.changed();
        break;
    }
  }

  /** Select the user curve and read it (02 5A 7F 7F). */
  async load(): Promise<boolean> {
    await this.store.send({ v1: 0x50, v2: 0x00, part: 0x00, data: [USER] });
    return this.store.request(0x5a, 0x7f);
  }

  points(): number[] {
    return this.exist.map((e, i) => (e ? i : -1)).filter((i) => i > 0 && i < 127);
  }

  /** Add or move a point (5A 07 [old, new, exist, vel]). */
  setPoint(x: number, y: number, from = x): Promise<boolean> {
    x = clamp(x, 1, 126);
    y = clamp(y, 1, 127);
    this.exist[from] = false;
    this.exist[x] = true;
    this.vel[x] = y;
    this.vel = interpolate(this.exist, this.vel);
    this.changed();
    return this.store.send({ v1: 0x5a, v2: 0x07, data: [from, x, 1, y] });
  }

  removePoint(x: number): Promise<boolean> {
    this.exist[x] = false;
    this.vel = interpolate(this.exist, this.vel);
    this.changed();
    return this.store.send({ v1: 0x5a, v2: 0x07, data: [x, x, 0, this.vel[x]] });
  }

  clear(): Promise<boolean> {
    this.exist.fill(false);
    this.vel = this.vel.map((_, i) => i);
    this.changed();
    return this.store.send({ v1: 0x5a, v2: 0x08, data: [] });
  }

  /** Name: header 5A 01, data 5A 02 [7F, offset, 3 bytes per char], end 5A 03. */
  async setName(name: string): Promise<boolean> {
    this.name = [...name].slice(0, 16).join("");
    await this.store.send({ v1: 0x5a, v2: 0x01, data: [0x7f, 0, 0, 0] });
    await this.store.send({ v1: 0x5a, v2: 0x02, data: [0x7f, 0, ...encodeName(this.name)] });
    return this.store.send({ v1: 0x5a, v2: 0x03, data: [] });
  }

  /** Learn from playing: 32 5A 0B [7F, 1] start / [7F, 2] stop; the piano then sends the new curve. */
  learn(start: boolean): Promise<boolean> {
    return this.store.send({ fn: 0x32, v1: 0x5a, v2: 0x0b, data: [0x7f, start ? 1 : 2] });
  }

  save(): Promise<boolean> {
    return this.store.send({ fn: 0x32, v1: 0x5a, v2: 0x0a, data: [] });
  }

  /** Discard unsaved edits (31 5A 09) and re-read the stored curve. */
  async exit(): Promise<boolean> {
    await this.store.send({ fn: 0x31, v1: 0x5a, v2: 0x09, data: [] });
    return this.store.request(0x5a, 0x7f);
  }
}

// ---------------- per-note editors ----------------

export interface PerNoteKind {
  id: "voicing" | "keyVolume" | "tuning" | "temperament";
  label: string;
  v2: number;
  size: number; // 128 notes or 12 pitch classes
  limit: number; // values are -limit..+limit
  unit: string;
  base: { v2: number; user: (store: ParamStore) => number[] }; // parameter switched to User first
}


export const PER_NOTE: PerNoteKind[] = [
  { id: "voicing", label: "Voicing", v2: 0x17, size: 128, limit: 5, unit: "", base: { v2: 0x01, user: () => [USER] } },
  { id: "keyVolume", label: "Key volume", v2: 0x10, size: 128, limit: 50, unit: "", base: { v2: 0x0f, user: () => [USER] } },
  { id: "tuning", label: "Tuning", v2: 0x0c, size: 128, limit: 50, unit: "cent", base: { v2: 0x0b, user: () => [USER] } },
  { id: "temperament", label: "Temperament", v2: 0x0e, size: 12, limit: 50, unit: "cent", base: { v2: 0x0d, user: (store) => [USER, store.get(findParam("Temperament"), 0)?.[1] ?? 0] } },
];

/** Editable notes (the original UI edits A0..C8). */
export const FIRST_NOTE = 21;
export const LAST_NOTE = 108;

export class PerNoteModel {
  readonly kind: PerNoteKind;
  values: number[];
  private readonly store: ParamStore;
  private listeners = new Set<() => void>();
  private off: () => void;

  constructor(store: ParamStore, kind: PerNoteKind) {
    this.store = store;
    this.kind = kind;
    this.values = Array(kind.size).fill(0);
    this.off = store.onMessage((m) => this.fromPiano(m));
  }

  dispose(): void {
    this.off();
  }

  onChange(l: () => void): () => void {
    this.listeners.add(l);
    return () => this.listeners.delete(l);
  }

  fromPiano(m: ParamMessage): void {
    if (m.v1 !== 0x50 || m.v2 !== this.kind.v2) return;
    if (m.fn === 0x31) {
      void this.load(false); // edit discarded elsewhere: re-read
      return;
    }
    if (m.fn !== 0x10 || m.data.length < 3) return;
    const oct = m.data[1];
    const start = oct === 0x7f || this.kind.size === 12 ? 0 : oct * 12;
    m.data.slice(2).forEach((b, i) => {
      if (start + i < this.kind.size) this.values[start + i] = b - 64;
    });
    for (const l of this.listeners) l();
  }

  /** Switch the base parameter to User (unless told not to) and read the table (02 50 v2 7F). */
  async load(selectUser = true, temperamentKey?: number): Promise<boolean> {
    if (selectUser) {
      const data = this.kind.base.user(this.store);
      if (this.kind.size === 12 && temperamentKey !== undefined) data[1] = temperamentKey;
      await this.store.send({ v1: 0x50, v2: this.kind.base.v2, part: 0x00, data });
    }
    return this.store.request(0x50, this.kind.v2);
  }

  /** Set one value locally and send its octave (oct = n fills notes 12n..12n+11; temperament: all 12). */
  set(index: number, value: number): Promise<boolean> {
    this.values[index] = clamp(Math.round(value), -this.kind.limit, this.kind.limit);
    for (const l of this.listeners) l();
    if (this.kind.size === 12) return this.store.send({ v1: 0x50, v2: this.kind.v2, data: [0x7f, 0x7f, ...this.values.map((v) => v + 64)] });
    const oct = Math.floor(index / 12);
    const vals = this.values.slice(oct * 12, oct * 12 + 12).map((v) => v + 64);
    return this.store.send({ v1: 0x50, v2: this.kind.v2, data: [0x7f, oct, ...vals] });
  }

  /** All values back to 0 (oct = 7F, all notes). */
  reset(): Promise<boolean> {
    this.values.fill(0);
    for (const l of this.listeners) l();
    return this.store.send({ v1: 0x50, v2: this.kind.v2, data: [0x7f, 0x7f, ...this.values.map((v) => v + 64)] });
  }

  save(): Promise<boolean> {
    return this.store.send({ fn: 0x32, v1: 0x50, v2: this.kind.v2, data: this.kind.size === 12 ? [] : [0] });
  }

  async exit(): Promise<boolean> {
    await this.store.send({ fn: 0x31, v1: 0x50, v2: this.kind.v2, data: this.kind.size === 12 ? [] : [0] });
    return this.load(false);
  }
}

function clamp(v: number, lo: number, hi: number): number {
  return Math.max(lo, Math.min(hi, v));
}
