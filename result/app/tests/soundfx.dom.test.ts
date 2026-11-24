// Effect parameter names, type pairing, ambience on/off (Sound tab); Temperament type + key and the VT term lines
// (Technician tab); Brilliance level, User EQ and Low Volume Balance (Settings tab).
import { change, field, pianoPair, tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const S = await import("../src/ui/screens.ts");
const { createContext } = await import("../src/ui/context.ts");
const { PARAMS } = await import("../src/protocol/params.gen.ts");
const { findParam } = await import("../src/protocol/codecs.ts");
const FX = await import("../src/features/soundfx.ts");
const EQ = await import("../src/features/usereq.ts");
const { VT_HELP } = await import("../src/ui/screens/technician.ts");
const { VT_PARAMS } = await import("../src/features/pianosound.ts");
const i18n = await import("../src/i18n.ts");

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

async function setup() {
  const p = await pianoPair({ endGets: true });
  const store = new ParamStore();
  bindStore(store);
  store.attach(p.session);
  const ctx = createContext(store, { session: () => p.session });
  return { ...p, store, ctx };
}

const label = (row: Element | null | undefined) => row?.closest(".row")?.querySelector("span")?.textContent;
const sel = (el: ParentNode, name: string) => field<HTMLSelectElement>(el, name);
const pick = (s: HTMLSelectElement, text: string) => change(s, String([...s.options].findIndex((o) => o.textContent === text)));
const setsOf = (sent: string[], v1v2: string) => sent.filter((m) => m.startsWith(`F0 40 7F 10 08 02 ${v1v2}`));

test("effect names: every effect type gets the two names of its group (manual p.33)", () => {
  const expect: Record<string, [string, string]> = {
    "Delay/Reverb": ["Dry / Wet", "Time"],
    Chorus: ["Dry / Wet", "Speed"],
    Tremolo: ["Dry / Wet", "Speed"],
    "Tremolo+": ["Dry / Wet", "Speed"],
    "Auto Pan": ["Dry / Wet", "Speed"],
    "Auto Pan+": ["Dry / Wet", "Speed"],
    Phaser: ["Dry / Wet", "Speed"],
    "Phaser+": ["Dry / Wet", "Speed"],
    "Rotary+": ["Accel. Speed", "Rotary Speed"],
  };
  const types = PARAMS.find((p) => p.name === "EffectType")!.values!;
  assert.equal(types.length, 24);
  for (const v of types) {
    const want = v.label === "Chorus/Classic Chorus" ? ["Mono / Stereo", "Speed"] : expect[v.label.split("/").slice(0, -1).join("/")];
    assert.deepEqual(FX.effectNames(v.label), want, v.label);
  }
});

test("effect type picked in the app: the type, then its Setting 1 and Setting 2 defaults; the sliders are renamed", async () => {
  const { store, ctx, sent, notify } = await setup();
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  notify([0x55, 0x11, 0x00, 0x06, 0x00]); // Mono Delay
  await tick();
  const fx = [...el.querySelectorAll("section")].find((s) => s.querySelector("h3")?.textContent === "Effect")!;
  const names = () => [...fx.querySelectorAll(".row.slider > span")].map((s) => s.textContent);
  assert.deepEqual(names(), ["Dry / Wet", "Time"]);
  pick([...fx.querySelectorAll("select")][0] as HTMLSelectElement, "Chorus/Classic Chorus");
  await tick(80);
  const order = sent.filter((m) => /^F0 40 7F 10 08 02 55 1[123] 00/.test(m));
  assert.deepEqual(order, ["F0 40 7F 10 08 02 55 11 00 00 01 F7", "F0 40 7F 10 08 02 55 12 00 65 F7", "F0 40 7F 10 08 02 55 13 00 07 F7"]);
  assert.deepEqual(names(), ["Mono / Stereo", "Speed"]);
  el.remove();
});

test("effect type changed on the panel: the sliders are renamed and no default is sent", async () => {
  const { store, ctx, sent, notify } = await setup();
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  notify([0x55, 0x11, 0x00, 25, 0]); // Rotary1
  await tick(40);
  const fx = [...el.querySelectorAll("section")].find((s) => s.querySelector("h3")?.textContent === "Effect")!;
  assert.deepEqual([...fx.querySelectorAll(".row.slider > span")].map((s) => s.textContent), ["Accel. Speed", "Rotary Speed"]);
  assert.deepEqual([...setsOf(sent, "55 12"), ...setsOf(sent, "55 13"), ...setsOf(sent, "55 11")], []);
  el.remove();
});

test("ambience: a switch (depth 0 = off), off remembers the depth, on brings it back; a picked type sends depth 63", async () => {
  const { store, ctx, sent, notify } = await setup();
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  const sw = field(el, "Ambience on");
  assert.equal(sw.indeterminate, true, "unknown until the piano tells its depth");
  change(sw, true);
  await tick(40);
  assert.deepEqual(setsOf(sent, "55 61"), [], "a tap on an unknown depth sends nothing");
  assert.equal(sw.indeterminate, true);
  // no depth to come back to: on = 64
  notify([0x55, 0x61, 0x00, 0x00]);
  await tick();
  assert.equal(sw.checked, false);
  change(sw, true);
  await tick(40);
  assert.ok(sent.includes("F0 40 7F 10 08 02 55 61 00 40 F7"), "on with nothing remembered = 64");
  notify([0x55, 0x61, 0x00, 0x50]);
  await tick();
  assert.equal(sw.checked, true);
  change(sw, false);
  await tick(40);
  assert.ok(sent.includes("F0 40 7F 10 08 02 55 61 00 00 F7"), "off = depth 0");
  sent.length = 0;
  change(sw, true);
  await tick(40);
  assert.deepEqual(setsOf(sent, "55 61"), ["F0 40 7F 10 08 02 55 61 00 50 F7"], "on = the depth before off");
  sent.length = 0;
  pick(sel(el, "Ambience"), "Large Room");
  await tick(60);
  assert.deepEqual(sent.filter((m) => /^F0 40 7F 10 08 02 55 6[01]/.test(m)), ["F0 40 7F 10 08 02 55 60 00 03 F7", "F0 40 7F 10 08 02 55 61 00 3F F7"]);
  el.remove();
});

test("pairing: a rendering character's ambience (a list position, not a wire value) is never sent", async () => {
  const { store, sent } = await setup();
  const rc = findParam("RenderingCharacter")!;
  assert.equal(await FX.sendLinked(store, rc, 0, [1]), false);
  await tick();
  assert.deepEqual([...setsOf(sent, "50 2A"), ...setsOf(sent, "55 60")], []);
});

test("reverb type picked in the app: the type, then its time and depth defaults (stock order)", async () => {
  const { store, ctx, sent } = await setup();
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  const rv = [...el.querySelectorAll("section")].find((s) => s.querySelector("h3")?.textContent === "Reverb")!;
  pick(rv.querySelector("select")!, "Lounge");
  await tick(80);
  // Lounge: depth 50, time 32 (protocol table linked defaults)
  assert.deepEqual(sent.filter((m) => /^F0 40 7F 10 08 02 55 0[389] 00/.test(m)), ["F0 40 7F 10 08 02 55 08 00 01 F7", "F0 40 7F 10 08 02 55 09 00 20 F7", "F0 40 7F 10 08 02 55 03 00 32 F7"]);
  el.remove();
});

test("temperament: type and key are separate selects over one value; each keeps the other byte", async () => {
  const { store, ctx, sent, notify } = await setup();
  const el = S.vtScreen(store, ctx);
  document.body.append(el);
  const type = sel(el, "Temperament");
  const key = sel(el, "Temperament key");
  assert.equal(type.options.length, 8);
  assert.equal(key.options.length, 12);
  // unknown: nothing is sent
  pick(type, "Werckmeister");
  await tick();
  assert.deepEqual(setsOf(sent, "50 0D"), []);
  notify([0x50, 0x0d, 0x00, 4, 9]); // Meantone / A
  await tick();
  assert.equal(type.selectedOptions[0].textContent, "Meantone");
  assert.equal(key.selectedOptions[0].textContent, "A");
  pick(type, "Werckmeister");
  await tick(40);
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 0D 00 05 09 F7"), "type changes, key A kept");
  pick(key, "D");
  await tick(40);
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 0D 00 05 02 F7"), "key changes, type Werckmeister kept");
  // the key note only for Equal
  const note = [...el.querySelectorAll("p")].find((p) => p.textContent === "The key has no effect with Equal temperament.")!;
  assert.equal(note.hidden, true);
  notify([0x50, 0x0d, 0x00, 0, 0]);
  await tick();
  assert.equal(note.hidden, false);
  el.remove();
});

test("technician: one line for every VT term, translated", () => {
  for (const n of VT_PARAMS) assert.ok(VT_HELP[n], n);
  const store = new ParamStore();
  const el = S.vtScreen(store);
  const lines = [...el.querySelectorAll(".vt-param > p.row-note")].map((p) => p.textContent);
  assert.equal(lines.length, VT_PARAMS.length);
  i18n.setLang("ko");
  const ko = S.vtScreen(store);
  for (const p of ko.querySelectorAll(".vt-param > p.row-note")) assert.match(p.textContent!, /[가-힣]/);
});

test("technician: the notes and the temperament type + key follow the Main sound's rows", async () => {
  const { store, ctx, notify } = await setup();
  const el = S.vtScreen(store, ctx);
  document.body.append(el);
  const organ = (await import("../src/protocol/params.gen.ts")).SOUNDS.find((s) => s.name === "Jazz Organ")!;
  notify([0x55, 0x00, 0x00, organ.msb, organ.lsb, organ.pc]);
  await tick();
  const wrap = (txt: string) => [...el.querySelectorAll<HTMLElement>(".vt-param")].find((w) => w.querySelector(".row span")?.textContent === txt)!;
  assert.equal(wrap("Damper Resonance").hidden, true, "a row the organ has not hides with its note");
  assert.equal(wrap("Temperament").hidden, false);
  assert.ok(wrap("Temperament").querySelector("select") && field(wrap("Temperament"), "Temperament key"));
  el.remove();
});

test("tone control: Brilliance level only for Brilliance (+1 dB = 50 14 7F 41), User EQ only for User", async () => {
  const { store, ctx, sent, notify } = await setup();
  const el = S.settingsScreen(store, ctx);
  document.body.append(el);
  const brill = field(el, "Brilliance level");
  const eq = el.querySelector<HTMLElement>("#user-eq")!;
  notify([0x51, 0x10, 0x7f, 0x01]);
  await tick();
  assert.equal(brill.closest<HTMLElement>(".row")!.hidden, false);
  assert.equal(eq.hidden, true);
  change(brill, "1");
  await tick(40);
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 14 7F 41 F7"), sent.join("\n"));
  notify([0x51, 0x10, 0x7f, 0x40]);
  await tick();
  assert.equal(brill.closest<HTMLElement>(".row")!.hidden, true);
  assert.equal(eq.hidden, false);
  el.remove();
});

test("user EQ: the four same-address band frames fill four sliders; edits send [band, gain+64, 0] / the frequency codec", async () => {
  const { store, ctx, sent, notify } = await setup();
  notify([0x51, 0x10, 0x7f, 0x40]); // User
  // the dump's band frames (capture 261031: 51 1A 7F ×4, 51 1B 7F ×2), gains changed so they differ
  for (const [b, g] of [[0, 0x3d], [1, 0x40], [2, 0x44], [3, 0x46]]) notify([0x51, 0x1a, 0x7f, b, g, 0]);
  notify([0x51, 0x1b, 0x7f, 0x01, 0x3d, 0x56]);
  notify([0x51, 0x1b, 0x7f, 0x02, 0x4b, 0x4a]);
  await tick();
  const el = S.settingsScreen(store, ctx);
  document.body.append(el);
  const gains = ["Low band", "Mid-low band", "Mid-high band", "High band"].map((n) => field(el, n));
  assert.deepEqual(gains.map((g) => g.value), ["-3", "0", "4", "6"]);
  assert.equal(sel(el, "Mid-low frequency").selectedOptions[0].textContent, "560 Hz");
  assert.equal(sel(el, "Mid-high frequency").selectedOptions[0].textContent, "1.25 kHz");
  change(gains[1], "-2");
  await tick(40);
  assert.ok(sent.includes("F0 40 7F 10 08 02 51 1A 7F 01 3E 00 F7"), "Mid-low -2 dB");
  pick(sel(el, "Mid-low frequency"), "1 kHz");
  await tick(40);
  assert.ok(sent.includes("F0 40 7F 10 08 02 51 1B 7F 01 47 5B F7"), "Mid-low 1 kHz");
  // the other bands keep their own values
  const gain = findParam("UserEqGain")!;
  assert.deepEqual(store.getBand(gain, 0x7f, 3), [3, 0x46, 0]);
  assert.deepEqual(store.getBand(gain, 0x7f, 1), [1, 0x3e, 0]);
  el.remove();
});

test("user EQ codec: the piano's defaults decode to 560 Hz / 1250 Hz; every offered frequency round-trips", () => {
  assert.equal(EQ.eqFreq.decode([1, 0x3d, 0x56]), 560);
  assert.equal(EQ.eqFreq.decode([2, 0x4b, 0x4a]), 1250);
  assert.deepEqual(EQ.eqFreq.encode(1, 560), [1, 0x3d, 0x56]);
  assert.deepEqual(EQ.eqFreq.encode(2, 1250), [2, 0x4b, 0x4a]);
  EQ.EQ_FREQS.forEach((f, i) => assert.equal(EQ.nearestFreq(EQ.eqFreq.decode(EQ.eqFreq.encode(1, f))), i, String(f)));
  assert.deepEqual(EQ.eqGain.encode(0, -6), [0, 58, 0]);
  assert.equal(EQ.eqGain.decode([3, 70, 0]), 6);
});

test("low volume balance: EqualizeMasterVolume 51 31 7F, Off / Type 1-5", async () => {
  const { store, ctx, sent } = await setup();
  const el = S.settingsScreen(store, ctx);
  document.body.append(el);
  const s = sel(el, "Low volume balance");
  assert.deepEqual([...s.options].map((o) => o.textContent), ["Off", "Type 1", "Type 2", "Type 3", "Type 4", "Type 5"]);
  pick(s, "Type 2");
  await tick(40);
  assert.ok(sent.includes("F0 40 7F 10 08 02 51 31 7F 02 F7"));
  el.remove();
});
