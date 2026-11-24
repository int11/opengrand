// Piano sound selection, Virtual Technician presets and user sounds — the stock app's way
// - a sound is `Sound` 55 00 <part> [MSB, LSB, PC] with the stock sound.json triple; on Main1 the
//   app then sends the sound's own reverb / effect (or rendering + ambience) and VT Default, only
//   the values that differ (stock `sendSound`; the piano does not apply them itself), and reads the
//   55 group (sound + reverb + effect + ambience) and the 50 group (VT) back
// - the VT preset is not a piano value: the stock app derives it by comparing the Main1 VT values
//   with every preset (kawaipiano.js `PIANO.get('VirtualTechnician')` → `verifyVts`); no match =
//   "User", and the current values are kept as the user VT (`PIANO.set('VirtualTechnician','Save')`)
// - a preset is applied by sending its VT values to Main1, only those that differ
//   (kawaipiano.dev.js `sendVtPreset` → `getDiffParams`)
// - user sounds and favorites live in the app (stock: panel files user/userdata.json …), never in
//   the piano's flash
import { findParam, rangeDecode } from "../protocol/codecs.ts";
import { type ParamDef, SOUNDS, type SoundDef, VT_PRESETS } from "../protocol/params.gen.ts";
import type { SavedParam, SoundLibrary, SoundRef, UserSound } from "../state/settings.ts";
import type { ParamStore } from "../state/store.ts";

export const MAIN = 0x00;

/** Every VT parameter of the Technician tab (Main1 unless noted). */
export const VT_PARAMS = [
  "TouchCurve", "Voicing", "ResonanceDepth", "DamperResonance", "DamperNoise", "StringResonance",
  "UndampedStringResonance", "CabinetResonance", "KeyOffEffect", "FallbackNoise", "KeyAttackNoise",
  "HammerDelay", "TopboardSimulation", "DecayTime", "DCAReleaseTime", "MinimumTouch", "StretchTuning",
  "Temperament", "KeyVolume", "HalfPedalAdjust", "SoftPedalDepth", "DamperHold",
];

/**
 * The VT rows the stock app shows for the Main sound (vtVm `get*` → `isShown`): the sound's own
 * list (SoundDef.vt = stock sound.json `parameters` → `availableParams`), with two stock quirks
 * kept on purpose:
 * - Minimum Touch follows Touch Curve (stock `getMintouch` asks for 'touchCurve', not 'minimumTouch')
 * - Touch Curve "Off" hides Minimum Touch and Hammer Delay; a temperament other than Equal hides
 *   Stretch Tuning (stock `getMintouch` / `getHammerdelay` / `getStretchtuning`)
 * A sound the app does not know (not read yet, a MIDI-only sound outside the panel list) shows
 * every row. VT_PARAMS itself is never filtered: preset matching and the user VT use all of it.
 */
export function vtVisible(store: ParamStore, s: SoundDef | undefined = currentSound(store)): Set<string> {
  const vis = new Set(s ? s.vt : VT_PARAMS);
  if (vis.has("TouchCurve")) vis.add("MinimumTouch");
  else vis.delete("MinimumTouch");
  const tc = findParam("TouchCurve");
  const tcOff = tc?.values?.find((v) => v.label === "Off")?.raw;
  if (tcOff !== undefined && store.get(tc, MAIN)?.[0] === tcOff) {
    vis.delete("MinimumTouch");
    vis.delete("HammerDelay");
  }
  const tm = findParam("Temperament");
  const equal = tm?.values?.find((v) => v.label.startsWith("Equal/"))?.raw;
  const cur = store.get(tm, MAIN)?.[0];
  if (equal !== undefined && cur !== undefined && cur !== equal) vis.delete("StretchTuning");
  return vis;
}

export interface Setting {
  p: ParamDef;
  part: number;
  data: number[];
}

// ---------------- sounds ----------------

export function soundParam(): ParamDef {
  return findParam("Sound")!;
}

/** The panel sound with this wire triple (undefined: not a panel sound or not known yet). */
export function soundOf(d: readonly number[] | undefined): SoundDef | undefined {
  if (!d || d.length < 3) return undefined;
  return SOUNDS.find((s) => s.msb === d[0] && s.lsb === d[1] && s.pc === d[2]);
}

export function currentSound(store: ParamStore, part = MAIN): SoundDef | undefined {
  return soundOf(store.get(soundParam(), part));
}

export const isRendering = (s: { msb: number; lsb: number } | undefined): boolean => !!s && s.msb === 95 && s.lsb === 112;
export const isPiano = (s: SoundDef | undefined): boolean => s?.category === "Piano";

/** Read the sound group (55, whole group for v2 0) and the VT group (50) of a part back from the piano. */
export async function rereadSound(store: ParamStore, part = MAIN): Promise<void> {
  await store.reread(soundParam(), part);
  const vt = findParam("TouchCurve");
  if (vt && part === MAIN) await store.reread(vt, part);
}

/**
 * The Main1 values the stock app sends with a panel sound (kawaipiano.dev.js `sendSound` →
 * `_createParamsFromSound`): the sound's own reverb + effect (or, for SK-EX Rendering, rendering
 * character + resonance depth + ambience; table SoundDef.defaults from sound.json), then the VT
 * preset of the sound — `VirtualTechnician: 0` (Default) for every sound.json sound. The global
 * values the stock sound obj also carries (keyboard mode, transpose, lower pedal, damper hold,
 * tuning) and the MIDI2ch sound are left alone on purpose.
 */
export function soundSettings(s: SoundDef): Setting[] {
  const out: Setting[] = [];
  for (const [name, v] of s.defaults) {
    const p = findParam(name);
    const data = p && presetData(p, v, {});
    if (p && data) out.push({ p, part: MAIN, data });
  }
  return [...out, ...presetSettings(VT_PRESETS[0])];
}

/**
 * Select a sound on a part, then read the piano's values back. Resolves false when the piano refused.
 * On Main1 a panel sound also brings its own reverb / effect / VT values like the stock app (the
 * piano firmware keeps them over a sound change — field 2026-11-09), only those that differ.
 */
export async function selectSound(store: ParamStore, lib: SoundLibrary, part: number, s: SoundRef): Promise<boolean> {
  lib.pushRecent(s);
  applying++;
  try {
    const ok = await store.set(soundParam(), part, [s.msb, s.lsb, s.pc]);
    const def = part === MAIN ? soundOf([s.msb, s.lsb, s.pc]) : undefined;
    if (ok && def) await applySettings(store, soundSettings(def));
    if (ok && store.connected) await rereadSound(store, part);
    return ok;
  } finally {
    applying--;
  }
}

// ---------------- VT presets ----------------

/** Raw data of one preset value (preset values are display values, "Off" = the 0 end of a range). */
function presetData(p: ParamDef, v: unknown, preset: Record<string, unknown>): number[] | undefined {
  if (p.name === "Temperament" && typeof v === "string") {
    const label = `${v}/${String(preset.TemperamentKey ?? "C")}`;
    const e = p.values?.find((x) => x.label === label) ?? p.values?.find((x) => x.label.startsWith(`${v}/`));
    return e ? (e.sub === null ? [e.raw] : [e.raw, e.sub]) : undefined;
  }
  if (typeof v === "string") {
    const t = p.range?.encodeTable?.[v];
    if (t !== undefined) return [t];
    const e = p.values?.find((x) => x.label === v || x.label.startsWith(v + "/"));
    return e ? (e.sub === null ? [e.raw] : [e.raw, e.sub]) : undefined;
  }
  if (typeof v === "number") {
    const r = p.range;
    if (!r) return [v];
    const t = r.encodeTable?.[String(v)];
    if (t !== undefined) return [t];
    if (v >= r.uiMin && v <= r.uiMax) return [Math.floor(((v - r.uiMin) / (r.uiMax - r.uiMin)) * (r.rawMax - r.rawMin)) + r.rawMin];
    return [Math.min(r.rawMax, Math.max(r.rawMin, v))];
  }
  return undefined;
}

/** The VT values a preset sets on Main1. */
export function presetSettings(preset: Record<string, unknown>): Setting[] {
  const out: Setting[] = [];
  for (const name of VT_PARAMS) {
    const p = findParam(name);
    const v = preset[name];
    if (!p || v === undefined || v === null || v === "") continue;
    const data = presetData(p, v, preset);
    if (data) out.push({ p, part: MAIN, data });
  }
  return out;
}

/** Same value for the user (ranges compare in display units: 63 and 64 are both "5"). */
export function sameValue(p: ParamDef, a: readonly number[], b: readonly number[]): boolean {
  if (p.range && a.length === 1 && b.length === 1) return rangeDecode(p, a[0]) === rangeDecode(p, b[0]);
  return a.length === b.length && a.every((x, i) => x === b[i]);
}

export type VtState = { kind: "preset"; index: number } | { kind: "user" } | { kind: "unknown" };

/**
 * Which preset the piano's Main1 VT values are (stock `verifyVts`): the first preset whose every
 * known value matches. Like the stock (`verifyParam` → undefined counts as a match) a value the app
 * does not hold yet does not decide; "unknown" only while no VT value is known at all. No match =
 * "User". (Until 2026-11-09 one missing value — e.g. DCAReleaseTime `55 45`, which the VT group get
 * `02 50 00 00` does not carry — made the result "unknown" and the select showed "–".)
 */
export function currentVt(store: ParamStore): VtState {
  if (!presetSettings(VT_PRESETS[0]).some((s) => store.get(s.p, s.part))) return { kind: "unknown" };
  for (let i = 0; i < VT_PRESETS.length; i++) {
    const match = presetSettings(VT_PRESETS[i]).every((s) => {
      const d = store.get(s.p, s.part);
      return !d || sameValue(s.p, d, s.data);
    });
    if (match) return { kind: "preset", index: i };
  }
  return { kind: "user" };
}

// while a preset / user VT / user sound is being sent, the values in between are no preset:
// they must not be taken for the user's own VT
let applying = 0;

/** Send only the values that differ from the piano's (stock `getDiffParams`). */
export async function applySettings(store: ParamStore, settings: Setting[]): Promise<boolean> {
  let ok = true;
  applying++;
  try {
    for (const s of settings) {
      const d = store.get(s.p, s.part);
      if (d && sameValue(s.p, d, s.data)) continue;
      if (!(await store.set(s.p, s.part, s.data))) ok = false;
    }
  } finally {
    applying--;
  }
  return ok;
}

export function applyVtPreset(store: ParamStore, index: number): Promise<boolean> {
  return applySettings(store, presetSettings(VT_PRESETS[index]));
}

/** The VT values of Main1 the app knows, as saved settings. */
export function captureVt(store: ParamStore): SavedParam[] {
  const out: SavedParam[] = [];
  for (const name of VT_PARAMS) {
    if (name === "DamperHold") continue; // a pedal setting, not part of a preset
    const p = findParam(name);
    const d = p && store.get(p, MAIN);
    if (p && d) out.push({ name, part: MAIN, data: [...d] });
  }
  return out;
}

export function settingsOf(saved: readonly SavedParam[]): Setting[] {
  const out: Setting[] = [];
  for (const s of saved) {
    const p = findParam(s.name);
    if (p && !p.varLen && s.data.length === p.len) out.push({ p, part: s.part, data: [...s.data] });
  }
  return out;
}

/**
 * Keep the user VT the way the stock does: when the piano's VT values are no preset, they are
 * the "User" preset (editing any VT value therefore selects User). Returns the state.
 */
export function trackVt(store: ParamStore, lib: SoundLibrary): VtState {
  const st = currentVt(store);
  if (st.kind === "user" && !applying) {
    const cur = captureVt(store);
    const saved = lib.userVt;
    const same = saved && saved.length === cur.length && saved.every((s, i) => s.name === cur[i].name && s.data.join() === cur[i].data.join());
    if (!same && cur.length) lib.setUserVt(cur);
  }
  return st;
}

// ---------------- user sounds ("Save this sound") ----------------

/** Main1 values a user sound keeps besides the sounds (stock sound obj: Main1 params + VT + System Tuning). */
const SOUND_SETTINGS: [string, number][] = [
  ["Reverb", MAIN], ["ReverbType", MAIN], ["ReverbDepth", MAIN], ["ReverbTime", MAIN],
  ["Effect", MAIN], ["EffectType", MAIN], ["EffectSetting1", MAIN], ["EffectSetting2", MAIN],
  ["RenderingCharacter", MAIN], ["AmbienceType", MAIN], ["AmbienceDepth", MAIN],
  ["Tuning", 0x7f],
];

export const USER_PARTS = [0x00, 0x08, 0x09];

/** The current registration: keyboard mode, Main/Layer/Lower sounds, Main1 effects and VT, tuning. */
export function captureUserSound(store: ParamStore, name: string): UserSound {
  const parts: UserSound["parts"] = [];
  for (const part of USER_PARTS) {
    const d = store.get(soundParam(), part);
    if (!d) continue;
    const known = soundOf(d);
    parts.push({ part, sound: { msb: d[0], lsb: d[1], pc: d[2], name: known?.name ?? `${d[0]}/${d[1]}/${d[2]}` } });
  }
  const params: SavedParam[] = [];
  for (const [n, part] of SOUND_SETTINGS) {
    const p = findParam(n);
    const d = p && store.get(p, part);
    if (p && d) params.push({ name: n, part, data: [...d] });
  }
  params.push(...captureVt(store));
  return { name, keyboardMode: store.get(findParam("KeyboardMode"), 0x0f)?.[0] ?? 0, parts, params };
}

/**
 * Put a user sound back: keyboard mode, sounds, then the saved values (stock: a user sound obj goes
 * through the same `sendSound`, with its own values in place of the sound.json defaults). The piano
 * keeps its effect values over a sound change (field 2026-11-09), so only values that differ are sent.
 */
export async function applyUserSound(store: ParamStore, u: UserSound): Promise<boolean> {
  applying++;
  try {
    let ok = await store.set(findParam("KeyboardMode")!, 0x0f, [u.keyboardMode]);
    for (const p of u.parts) {
      if (!(await store.set(soundParam(), p.part, [p.sound.msb, p.sound.lsb, p.sound.pc]))) ok = false;
    }
    if (store.connected) await rereadSound(store, MAIN);
    if (!(await applySettings(store, settingsOf(u.params ?? [])))) ok = false;
    return ok;
  } finally {
    applying--;
  }
}

/** The user sound the piano currently is (sounds + saved values all match), if any. */
export function currentUserSound(store: ParamStore, lib: SoundLibrary): UserSound | undefined {
  return lib.user.find((u) => {
    if ((store.get(findParam("KeyboardMode"), 0x0f)?.[0] ?? -1) !== u.keyboardMode) return false;
    for (const p of u.parts) {
      const d = store.get(soundParam(), p.part);
      if (!d || d[0] !== p.sound.msb || d[1] !== p.sound.lsb || d[2] !== p.sound.pc) return false;
    }
    for (const s of settingsOf(u.params ?? [])) {
      const d = store.get(s.p, s.part);
      if (!d || !sameValue(s.p, d, s.data)) return false;
    }
    return true;
  });
}
