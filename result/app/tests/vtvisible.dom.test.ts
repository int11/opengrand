// Technician rows follow the Main sound like the stock VT editor (sound.json availableParams), and a
// sound outside the Piano category gets "Other sound settings" (manual p.34 (8), p.54) in place of the
// VT preset - on the Technician tab and on the Sound tab.
import "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const S = await import("../src/ui/screens.ts");
const { createContext } = await import("../src/ui/context.ts");
const { SOUNDS } = await import("../src/protocol/params.gen.ts");
const { findParam } = await import("../src/protocol/codecs.ts");
const { VT_PARAMS, vtVisible } = await import("../src/features/pianosound.ts");
const i18n = await import("../src/i18n.ts");

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

const sound = (name: string) => SOUNDS.find((s) => s.name === name)!;
const RESONANCE = ["DamperResonance", "StringResonance", "UndampedStringResonance", "CabinetResonance", "KeyOffEffect"];

function storeWith(name?: string) {
  const store = new ParamStore();
  if (name) {
    const s = sound(name);
    void store.set(findParam("Sound")!, 0x00, [s.msb, s.lsb, s.pc]);
  }
  return store;
}
const shownRows = (sec: Element) => [...sec.querySelectorAll(".row")].filter((r) => !(r as HTMLElement).hidden).map((r) => r.querySelector("span")!.textContent);

test("every panel sound has its VT list, only VT parameters, Touch Curve everywhere", () => {
  assert.equal(SOUNDS.length, 67);
  for (const s of SOUNDS) {
    assert.ok(s.vt.length >= 6, s.name);
    for (const n of s.vt) assert.ok(VT_PARAMS.includes(n), `${s.name}: ${n}`);
    assert.ok(s.vt.includes("TouchCurve"), s.name);
    if (s.category === "Piano") assert.ok(!s.vt.includes("DamperHold"), `${s.name}: Damper Hold never on a piano`);
  }
});

test("SK-EX Rendering: Resonance Depth, no damper / string / undamped / cabinet resonance, no key-off", () => {
  const vis = vtVisible(storeWith("SK-EX Rendering"));
  assert.ok(vis.has("ResonanceDepth"));
  for (const n of RESONANCE) assert.ok(!vis.has(n), n);
  for (const n of ["DamperNoise", "TopboardSimulation", "MinimumTouch", "StretchTuning"]) assert.ok(vis.has(n), n);
  assert.ok(!vis.has("DamperHold"));
  assert.equal(vis.size, 16);
});

test("EX / SK-5 / upright pianos: the 20 piano rows, no Resonance Depth, no Damper Hold", () => {
  for (const name of ["SK-EX | Concert", "EX | Concert", "SK-5 | Pop", "UprightPiano | Clasical"]) {
    const vis = vtVisible(storeWith(name));
    assert.equal(vis.size, 20, name);
    for (const n of RESONANCE) assert.ok(vis.has(n), `${name} ${n}`);
    assert.ok(!vis.has("ResonanceDepth") && !vis.has("DamperHold"), name);
  }
});

test("non-piano sounds: the short lists; Minimum Touch follows Touch Curve like the stock UI", () => {
  assert.deepEqual([...vtVisible(storeWith("Jazz Organ"))].sort(), ["DamperHold", "HalfPedalAdjust", "KeyVolume", "MinimumTouch", "SoftPedalDepth", "Temperament", "TouchCurve", "Voicing"]);
  assert.ok(!vtVisible(storeWith("Full Organ")).has("DamperHold"));
  assert.ok(vtVisible(storeWith("Harpsichord")).has("KeyOffEffect"));
  assert.ok(vtVisible(storeWith("Modern Electric Piano")).has("MinimumTouch"));
  assert.ok(!vtVisible(storeWith("Wood Bass")).has("TopboardSimulation"));
});

test("an unknown sound shows every row; Touch Curve Off and a non-Equal temperament hide their rows", async () => {
  assert.equal(vtVisible(storeWith()).size, VT_PARAMS.length);
  const store = storeWith("EX | Concert");
  const tc = findParam("TouchCurve")!;
  await store.set(tc, 0, [tc.values!.find((v) => v.label === "Off")!.raw]);
  let vis = vtVisible(store);
  assert.ok(!vis.has("MinimumTouch") && !vis.has("HammerDelay") && vis.has("TouchCurve"));
  await store.set(tc, 0, [tc.values!.find((v) => v.label === "Normal")!.raw]);
  const tm = findParam("Temperament")!;
  const pure = tm.values!.find((v) => v.label === "Pure Major/C")!;
  await store.set(tm, 0, [pure.raw, pure.sub!]);
  vis = vtVisible(store);
  assert.ok(vis.has("MinimumTouch") && vis.has("HammerDelay") && !vis.has("StretchTuning"));
});

test("Technician tab: an organ shows Other sound settings without the preset; a piano brings them back", async () => {
  const store = storeWith("Jazz Organ");
  const el = S.vtScreen(store, createContext(store));
  document.body.append(el);
  const [preset, rows] = [...el.querySelectorAll("section")];
  assert.equal(preset.hidden, true, "no VT preset for a non-piano sound");
  assert.equal(rows.querySelector("h3")!.textContent, "Other sound settings");
  assert.deepEqual(shownRows(rows), ["Touch Curve", "Voicing", "Minimum Touch", "Temperament", "Key Volume", "Half Pedal Adjust", "Soft Pedal Depth", "Damper Hold"]);
  assert.equal(rows.querySelectorAll(".row").length, 22, "rows stay in the DOM (hidden)");
  const opts = [...el.querySelectorAll("section")].find((s) => s.querySelector("h3")!.textContent === "Sound preview")!.querySelectorAll("option");
  const offered = [...opts].filter((o) => !o.disabled).map((o) => o.value);
  assert.ok(!offered.includes("DamperResonance") && offered.includes("TouchCurve"));
  const target = [...opts][0].parentElement as HTMLSelectElement;
  assert.ok(offered.includes(target.value), "the preview target is a row on screen");
  // the piano changes to a piano sound
  const p = sound("SK-EX Rendering");
  await store.set(findParam("Sound")!, 0, [p.msb, p.lsb, p.pc]);
  assert.equal(preset.hidden, false);
  assert.equal(rows.querySelector("h3")!.textContent, "Advanced");
  assert.equal(shownRows(rows).length, 16);
  assert.ok(shownRows(rows).includes("Resonance Depth") && !shownRows(rows).includes("Damper Resonance"));
  el.remove();
});

test("Sound tab: Other sound settings only for a non-piano Main sound, with that sound's rows", async () => {
  const store = storeWith("EX | Concert");
  const el = S.soundScreen(store, createContext(store));
  document.body.append(el);
  const other = [...el.querySelectorAll("section")].find((s) => s.querySelector("h3")!.textContent === "Other sound settings")!;
  const vt = [...el.querySelectorAll("section")].find((s) => s.querySelector("h3")!.textContent === "Virtual Technician")!;
  assert.equal(other.hidden, true);
  assert.equal(vt.hidden, false);
  const s = sound("Slow Strings");
  await store.set(findParam("Sound")!, 0, [s.msb, s.lsb, s.pc]);
  assert.equal(other.hidden, false);
  assert.equal(vt.hidden, true);
  assert.deepEqual(shownRows(other), ["Touch Curve", "Voicing", "Minimum Touch", "Temperament", "Key Volume", "Half Pedal Adjust", "Soft Pedal Depth", "Damper Hold"]);
  el.remove();
});
