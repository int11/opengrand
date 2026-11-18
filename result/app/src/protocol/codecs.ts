// Value codecs between UI values and raw SysEx data bytes.
// Behaviour follows the original kawaipiano.js codecs as extracted in
// (the stock app's protocol table, classes F/K/X/Ne.*).
import { PARAMS, type ParamDef } from "./params.gen.ts";

export function findParam(name: string, fn = 0x10): ParamDef | undefined {
  return PARAMS.find((p) => p.name === name && p.fn === fn) ?? PARAMS.find((p) => p.name === name);
}

export function paramByWire(fn: number, v1: number, v2: number, part: number): ParamDef | undefined {
  const cands = PARAMS.filter((p) => p.fn === fn && p.v1 === v1 && p.v2 === v2);
  if (cands.length <= 1) return cands[0];
  const scope = part === 0x7f ? "global" : part === 0x0f ? "system" : "part";
  return cands.find((p) => p.scope === scope) ?? cands[0];
}

// ---- "min" (range) parameters: F(range) ----
export function rangeEncode(p: ParamDef, ui: number): number {
  const r = p.range;
  if (!r) return clamp7(Math.round(ui));
  const exact = r.encodeTable?.[String(ui)];
  if (exact !== undefined) return exact;
  if (ui < r.uiMin || ui > r.uiMax) throw new RangeError(`${p.name}: ${ui} outside ${r.uiMin}..${r.uiMax}`);
  return Math.floor(((ui - r.uiMin) / (r.uiMax - r.uiMin)) * (r.rawMax - r.rawMin)) + r.rawMin;
}

export function rangeDecode(p: ParamDef, raw: number): number {
  const r = p.range;
  if (!r) return raw;
  if (r.encodeTable) {
    for (const [ui, v] of Object.entries(r.encodeTable)) if (v === raw && !Number.isNaN(Number(ui))) return Number(ui);
  }
  const ui = Math.round(((raw - r.rawMin) / (r.rawMax - r.rawMin)) * (r.uiMax - r.uiMin)) + r.uiMin;
  return Math.max(r.uiMin, Math.min(r.uiMax, ui));
}

// ---- enums ----
export function enumLabel(p: ParamDef, raw: number, sub?: number): string {
  const v = p.values?.find((e) => e.raw === raw && (e.sub === null || sub === undefined || e.sub === sub));
  return v?.label ?? String(raw);
}

export function enumOptions(p: ParamDef): { label: string; data: number[] }[] {
  return (p.values ?? []).map((v) => ({ label: v.label, data: v.sub === null ? [v.raw] : [v.raw, v.sub] }));
}

// ---- data types (Ne.*) ----
export const bpm = {
  encode: (n: number): number[] => {
    const v = Math.max(10, Math.min(500, Math.round(n)));
    return [v >> 7, v & 0x7f];
  },
  decode: (d: number[]): number => ((d[0] ?? 0) << 7) | (d[1] ?? 0),
};

const DENOMS = [1, 2, 4, 8, 16, 32, 64];
export const beat = {
  encode: (num: number, den: number): number[] => [num, Math.max(0, DENOMS.indexOf(den))],
  decode: (d: number[]): { num: number; den: number } => ({ num: d[0] ?? 4, den: DENOMS[d[1] ?? 2] ?? 4 }),
};

export const tuning = {
  encode: (hz: number): number[] => [clamp7(Math.round(2 * (hz - 440) + 64))],
  decode: (d: number[]): number => 440 + ((d[0] ?? 64) - 64) / 2,
};

/** Signed values stored with a 0x40 offset (transpose, octave shift). */
export const offset64 = {
  encode: (v: number): number[] => [clamp7(v + 64)],
  decode: (d: number[]): number => (d[0] ?? 64) - 64,
};

/** MusicProgress / repeat points: [0, bar14 hi, lo, beat, 0, 0] or [1, 35-bit time]. */
export const progress = {
  decode: (d: number[]): { bar: number; beat: number } | { ms: number } => {
    if (d[0] === 1) {
      let t = 0;
      for (let i = 1; i <= 5; i++) t = t * 128 + (d[i] ?? 0);
      return { ms: t };
    }
    return { bar: ((d[1] ?? 0) << 7) | (d[2] ?? 0), beat: d[3] ?? 1 };
  },
};

function clamp7(v: number): number {
  return Math.max(0, Math.min(127, v));
}
