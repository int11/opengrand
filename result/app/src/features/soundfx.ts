// Effect / ambience / temperament helpers of the Sound and Technician tabs - the stock app's behaviour:
// - the two effect sliders are named by the effect type (manual p.33 "Effect parameters", the stock editor picks
//   the names from the type's group: delays → Dry/Wet + Time, Classic Chorus → Mono/Stereo + Speed,
//   rotary → Accel. Speed + Rotary Speed, every other type → Dry/Wet + Speed)
// - picking a type in the app also sends that type's paired defaults (spec `linked_params_on_receive`, emitted as
//   ParamDef.linked): effect type → Setting 1 then Setting 2, ambience type → depth 63. Only on the user's pick
//   in our UI - a value the piano reports (dump, panel) is never answered with defaults
// - Ambience on/off is not a parameter: off = depth 0, on = the depth before off (or 64 when none is known)
// - Temperament `50 0D` is one 2-byte value [type, key]; the UI shows type and key apart
import { enumLabel, findParam, rangeEncode } from "../protocol/codecs.ts";
import type { ParamDef } from "../protocol/params.gen.ts";
import type { ParamStore } from "../state/store.ts";

export type EffectNames = [string, string];

/** Names of Setting 1 / Setting 2 for an EffectType label ("<group>/<type>", as in the protocol table). */
export function effectNames(label: string | undefined): EffectNames {
  const group = label?.split("/").slice(0, -1).join("/") ?? "";
  if (group === "Delay/Reverb") return ["Dry / Wet", "Time"];
  if (label === "Chorus/Classic Chorus") return ["Mono / Stereo", "Speed"];
  if (group === "Rotary+") return ["Accel. Speed", "Rotary Speed"];
  return ["Dry / Wet", "Speed"];
}

/** The EffectType label of the piano's current value (undefined while unknown). */
export function effectLabel(store: ParamStore, part: number): string | undefined {
  const p = findParam("EffectType");
  const d = p && store.get(p, part);
  return p && d ? enumLabel(p, d[0], d[1]) : undefined;
}

/** Raw data of a linked display value. */
function linkedData(p: ParamDef, v: number): number[] {
  return [p.range ? rangeEncode(p, v) : v];
}

/** the stock editor sends the reverb pair the other way round: time, then depth */
const SECOND_FIRST = new Set(["ReverbType"]);

/**
 * Send the paired defaults of a value the user just picked, one after the other in the stock editor's order
 * (the first linked parameter, then the second; reverb: time, then depth). False when the value has no pairing
 * or a send failed.
 */
export async function sendLinked(store: ParamStore, p: ParamDef, part: number, data: readonly number[]): Promise<boolean> {
  const found = p.linked?.[enumLabel(p, data[0], data[1])];
  if (!found?.length) return false;
  const pairs = SECOND_FIRST.has(p.name) ? [...found].reverse() : found;
  for (const [name, v] of pairs) {
    const q = findParam(name);
    if (!q || !(await store.set(q, part, linkedData(q, v)))) return false;
  }
  return true;
}

// ---- Ambience on / off (depth 0 = off) ----

/** stock: switching ambience on with no remembered depth sends 64 */
export const AMBIENCE_ON_DEPTH = 64;

/** depth before the last "off", per part (kept across screen rebuilds like the stock editor's state) */
const ambienceBefore = new Map<number, number>();

export function ambienceSwitch(store: ParamStore, part: number) {
  const p = findParam("AmbienceDepth")!;
  return {
    /** undefined while the depth is unknown */
    isOn: (): boolean | undefined => {
      const d = store.get(p, part);
      return d ? d[0] !== 0 : undefined;
    },
    set: (on: boolean): Promise<boolean> => {
      const d = store.get(p, part);
      if (!on) {
        if (d && d[0] !== 0) ambienceBefore.set(part, d[0]);
        return store.set(p, part, [0]);
      }
      return store.set(p, part, [ambienceBefore.get(part) ?? AMBIENCE_ON_DEPTH]);
    },
  };
}

// ---- Temperament: type and key of one value ----

export const TEMPERAMENT_KEYS = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

/** Temperament types in table order: [raw type byte, name]. */
export function temperamentTypes(): [number, string][] {
  const out = new Map<number, string>();
  for (const v of findParam("Temperament")?.values ?? []) if (!out.has(v.raw)) out.set(v.raw, v.label.split("/")[0]);
  return [...out];
}
