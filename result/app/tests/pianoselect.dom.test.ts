// Piano tab selection (field 2026-11-03): the chosen sound reaches the piano with the stock wire
// triple, is read back and shows as selected; panel changes show too; VT preset derived like the
// stock app (an edit = "User"); "Save this sound" → User category.
import { byText, change, field, pianoPair, tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const S = await import("../src/ui/screens.ts");
const { createContext } = await import("../src/ui/context.ts");
const { SOUNDS, VT_PRESETS } = await import("../src/protocol/params.gen.ts");
const PS = await import("../src/features/pianosound.ts");
const i18n = await import("../src/i18n.ts");

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

async function setup() {
  // the loopback piano also ends every get `02 v1 v2 part` (stock: values, then `03 v1 v2 7F`)
  const p = await pianoPair({ endGets: true });
  const store = new ParamStore();
  bindStore(store);
  store.attach(p.session);
  const ctx = createContext(store, { session: () => p.session });
  return { ...p, store, ctx };
}

/** The piano reports every value of a VT preset on Main1 (as after a dump). */
function reportPreset(notify: (b: number[]) => void, index: number) {
  for (const s of PS.presetSettings(VT_PRESETS[index])) notify([s.p.v1, s.p.v2, s.part, ...s.data]);
}

const pressed = (el: HTMLElement) => [...el.querySelectorAll<HTMLElement>(".sound-item .item[aria-pressed=true]")].map((b) => b.textContent);

// The original app's sound table (extracted from it, so not in the public repository): compared when present.
const SOUNDS_JSON = new URL("../../../target/tables/sounds_CA79.json", import.meta.url);
const noSoundsJson = !existsSync(SOUNDS_JSON) && "target/tables/sounds_CA79.json absent (not in the public repository)";

test("sound table: the wire triple is the stock app's (sound.json msb/lsb/pc), not the manual's program number", { skip: noSoundsJson }, () => {
  const stock = JSON.parse(readFileSync(SOUNDS_JSON, "utf8")) as { msb: number; lsb: number; pc: number; category: string }[];
  const panel = stock.filter((x) => !x.category.includes("MIDI") && x.category !== "Hidden");
  SOUNDS.forEach((s, i) => assert.deepEqual([s.msb, s.lsb, s.pc], [panel[i].msb, panel[i].lsb, panel[i].pc], s.name));
});

test("sound table: 67 panel sounds, the triple the piano reports maps back to its sound", () => {
  assert.equal(SOUNDS.length, 67);
  // golden trace 261031_phone_pianoremote_1.1.5: the piano reported Main1 `55 00 00 5F 1B 00`
  assert.equal(PS.soundOf([0x5f, 0x1b, 0x00])?.name, "EX | Concert");
  const concert = SOUNDS.find((s) => s.name === "SK-EX | Concert")!;
  assert.deepEqual([concert.msb, concert.lsb, concert.pc, concert.prog], [121, 0, 0, 1]);
});

test("sound tab (piano folded in): a chosen sound is sent with the stock triple, read back, and shows as selected", async () => {
  const { store, ctx, sent } = await setup();
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  assert.deepEqual(pressed(el), [], "nothing selected before the piano told its sound");
  byText(el, ".sound-item .item", "SK-EX | Concert").click();
  await tick(60);
  assert.ok(sent.includes("F0 40 7F 10 08 02 55 00 00 79 00 00 F7"), "Sound Main1 121/0/0");
  assert.ok(sent.includes("F0 40 7F 02 08 02 55 00 00 F7"), "get of the sound group (55 00) for Main1");
  assert.ok(sent.includes("F0 40 7F 02 08 02 50 00 00 F7"), "get of the VT group (50 00) for Main1");
  assert.deepEqual(pressed(el), ["SK-EX | Concert"]);
  assert.match(el.querySelector(".current-sound")!.textContent!, /Selected: SK-EX \| Concert/);
  assert.equal(ctx.library.recent[0].name, "SK-EX | Concert");
  el.remove();
});

test("sound tab (piano folded in): a sound changed on the panel shows as selected; SK-EX Rendering hides VT and reverb", async () => {
  const { store, ctx, notify } = await setup();
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  notify([0x55, 0x00, 0x00, 0x5f, 0x1b, 0x00]);
  await tick();
  assert.deepEqual(pressed(el), ["EX | Concert"]);
  const sections = [...el.querySelectorAll<HTMLElement>("section")];
  const vt = sections.find((s) => s.querySelector("h3")?.textContent === "Virtual Technician")!;
  const reverb = sections.find((s) => s.querySelector("h3")?.textContent === "Reverb")!;
  const rendering = sections.find((s) => s.querySelector("h3")?.textContent === "SK-EX Rendering")!;
  assert.equal(vt.hidden, false);
  assert.equal(rendering.hidden, true);
  notify([0x55, 0x00, 0x00, 0x5f, 0x70, 0x00]);
  await tick();
  assert.deepEqual(pressed(el), ["SK-EX Rendering"]);
  assert.equal(vt.hidden, true, "no VT presets for SK-EX Rendering (manual p.22)");
  assert.equal(reverb.hidden, true, "no reverb for SK-EX Rendering (manual p.24)");
  assert.equal(rendering.hidden, false);
  el.remove();
});

test("VT preset: derived from the piano's values; an edit selects User and keeps it; a preset sends only differences", async () => {
  const { store, ctx, notify, sent } = await setup();
  notify([0x55, 0x00, 0x00, 0x5f, 0x1b, 0x00]);
  reportPreset(notify, 0);
  await tick();
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  const sel = field<HTMLSelectElement>(el, "Virtual Technician preset");
  assert.equal(sel.value, "0", "Default");
  // the panel edits Damper Noise -> User, and the user VT is remembered
  notify([0x50, 0x03, 0x00, 77]);
  await tick();
  assert.equal(sel.value, "user");
  assert.deepEqual(ctx.library.userVt?.find((s) => s.name === "DamperNoise")?.data, [77]);
  // choose Noiseless: only the values that differ are sent ("Off" = the 0 end of a range)
  sent.length = 0;
  change(sel, "1");
  await tick(80);
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 02 00 00 F7"), "Damper Resonance Off");
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 03 00 00 F7"), "Damper Noise Off");
  assert.ok(!sent.some((m) => m.startsWith("F0 40 7F 10 08 02 50 00 00")), "Touch Curve unchanged (Normal) is not sent");
  assert.equal(sel.value, "1", "Noiseless now");
  // back to User: the remembered values return
  sent.length = 0;
  change(sel, "user");
  await tick(80);
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 03 00 4D F7"), "Damper Noise 77 again");
  assert.equal(sel.value, "user");
  el.remove();
});

test("Save this sound: the registration with effects and VT goes to the Sound tab's User category and comes back", async () => {
  const { store, ctx, notify, sent } = await setup();
  notify([0x53, 0x00, 0x0f, 0x00]);
  notify([0x55, 0x00, 0x00, 0x5f, 0x1b, 0x00]);
  notify([0x55, 0x08, 0x00, 0x03]); // ReverbType Concert Hall
  reportPreset(notify, 2);
  await tick();
  const piano = S.soundScreen(store, ctx);
  document.body.append(piano);
  byText(piano, "button", "Save this sound").click();
  assert.equal(ctx.library.user.length, 1);
  const u = ctx.library.user[0];
  assert.equal(u.parts[0].sound.name, "EX | Concert");
  assert.deepEqual(u.params?.find((p) => p.name === "ReverbType")?.data, [3]);
  assert.ok(u.params?.some((p) => p.name === "StringResonance"), "VT values saved");
  // favorite star for the current sound
  (piano.querySelector("[data-action=favorite-current]") as HTMLElement).click();
  assert.equal(ctx.library.isFavorite(SOUNDS.find((s) => s.name === "EX | Concert")!), true);
  piano.remove();

  // somebody changes the sound; the user sound puts everything back
  notify([0x55, 0x00, 0x00, 0x79, 0x00, 0x00]);
  notify([0x55, 0x08, 0x00, 0x01]);
  await tick();
  const sound = S.soundScreen(store, ctx);
  document.body.append(sound);
  byText(sound, "button.chip", "User").click();
  sent.length = 0;
  byText(sound, ".sound-item .item", "User 1").click();
  await tick(150);
  assert.ok(sent.includes("F0 40 7F 10 08 02 55 00 00 5F 1B 00 F7"), "sound back");
  assert.ok(sent.includes("F0 40 7F 10 08 02 55 08 00 03 F7"), "reverb type back");
  assert.ok(byText(sound, ".sound-item .item", "User 1").classList.contains("on"), "the user sound shows as selected");
  sound.remove();
});

test("sound tab: the chosen part's sound shows as selected and follows the piano", async () => {
  const { store, ctx, notify } = await setup();
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  byText(el, "button.chip", "Piano").click();
  notify([0x55, 0x00, 0x00, 0x5f, 0x1e, 0x00]);
  await tick();
  assert.deepEqual([...el.querySelectorAll(".sound-item .item.on")].map((b) => b.textContent), ["SK-5 | Clasical"]);
  el.remove();
});

// user 2026-11-09: no Piano tab — the Sound tab opens on the Piano category and shows, right below the list,
// what a sound changes; tuning / transpose are global and on the Settings tab
test("sound tab: Piano category first and open, sound-dependent sections below the list, tuning / transpose on Settings", async () => {
  const { store, ctx, notify } = await setup();
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  const chips = [...el.querySelectorAll<HTMLElement>("button.chip[data-cat]")];
  assert.equal(chips[0].dataset.cat, "Piano");
  assert.ok(chips[0].classList.contains("on"), "Piano open by default");
  assert.deepEqual([...el.querySelectorAll(".sound-item .item")].map((b) => b.textContent), SOUNDS.filter((s) => s.category === "Piano").map((s) => s.name));
  const titles = [...el.querySelectorAll(":scope > section > h3")].map((h) => h.textContent);
  assert.deepEqual(titles, ["Sound select", "SK-EX Rendering", "Virtual Technician", "Other sound settings", "Effect", "Reverb", "Keyboard mode", "Part"]);
  const sec = (t: string) => [...el.querySelectorAll<HTMLElement>(":scope > section")].find((s) => s.querySelector("h3")?.textContent === t)!;
  assert.equal([...el.querySelectorAll(".row > span:first-child")].some((s) => s.textContent === "Tuning" || s.textContent === "Transpose"), false);
  assert.equal(sec("Other sound settings").hidden, true, "not for a piano sound");
  // an organ: no VT preset, no rendering, its Other sound settings; effect and reverb stay
  notify([0x55, 0x00, 0x00, ...(() => { const o = SOUNDS.find((s) => s.category === "Organ")!; return [o.msb, o.lsb, o.pc]; })()]);
  await tick();
  assert.deepEqual(["SK-EX Rendering", "Virtual Technician", "Other sound settings", "Effect", "Reverb"].map((t) => sec(t).hidden), [true, true, false, false, false]);
  // the star + Save this sound sit with the list
  assert.ok(sec("Sound select").querySelector("[data-action=favorite-current]") && sec("Sound select").querySelector("[data-action=save-sound]"));
  el.remove();
  const set = S.settingsScreen(store, ctx);
  const tt = set.querySelector("#tuning-transpose")!;
  assert.ok(field(tt, "Tuning") && field(tt, "Transpose"));
});

// field 2026-11-09: the piano keeps reverb / effect over a sound change (reverb off stayed off
// EX Warm → EX Concert → EX Warm); the stock app sends the sound's own values itself
// (kawaipiano.dev.js `sendSound` → `_createParamsFromSound` → `getDiffParams`)
test("sound tab (piano folded in): a sound brings its own reverb / effect and VT Default like the stock app — only the values that differ", async () => {
  const { store, ctx, notify, sent } = await setup();
  const warm = SOUNDS.find((s) => s.name === "EX | Warm")!;
  notify([0x55, 0x00, 0x00, warm.msb, warm.lsb, warm.pc]);
  for (const s of PS.soundSettings(warm)) notify([s.p.v1, s.p.v2, s.part, ...s.data]);
  notify([0x55, 0x07, 0x00, 0x00]); // the user turned reverb off
  reportPreset(notify, 1); // Noiseless
  await tick();
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  sent.length = 0;
  byText(el, ".sound-item .item", "EX | Concert").click();
  await tick(150);
  const sets = sent.filter((m) => m.startsWith("F0 40 7F 10 08 02"));
  assert.equal(sets[0], "F0 40 7F 10 08 02 55 00 00 5F 1B 00 F7", "the sound first");
  assert.ok(sets.includes("F0 40 7F 10 08 02 55 07 00 01 F7"), "EX Concert's reverb On (sound.json reverbOnOff)");
  assert.ok(!sets.some((m) => m.includes(" 55 08 00 ") || m.includes(" 55 03 00 ") || m.includes(" 55 11 00 ")), "type / depth / effect already EX Concert's: not sent");
  assert.ok(sets.includes("F0 40 7F 10 08 02 50 02 00 3F F7"), "VT Default (VirtualTechnician 0): Damper Resonance 5");
  assert.ok(!sets.some((m) => m.includes(" 50 00 00 ")), "Touch Curve Normal in both: not sent");
  assert.ok(!sets.some((m) => m.includes(" 53 ") || m.includes(" 51 03 ")), "keyboard mode / transpose / tuning untouched");
  const vt = field<HTMLSelectElement>(el, "Virtual Technician preset");
  assert.equal(vt.value, "0", "Default after the select");
  assert.ok(!ctx.library.userVt, "values in between are not taken for a user VT");

  // SK-EX Rendering: rendering character, resonance depth and ambience instead of reverb / effect
  sent.length = 0;
  byText(el, ".sound-item .item", "SK-EX Rendering").click();
  await tick(150);
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 2A 00 4B F7"), "Resonance Depth 6");
  assert.ok(sent.includes("F0 40 7F 10 08 02 55 61 00 3F F7"), "Ambience Depth 63");
  assert.ok(!sent.some((m) => m.startsWith("F0 40 7F 10 08 02 55 07")), "reverb is not part of a Rendering sound");
  el.remove();

  // another part (Layer) takes only the sound, as the stock editor's PIANO.set('Timbre', id, 'Layer')
  sent.length = 0;
  await PS.selectSound(store, ctx.library, 0x08, warm);
  assert.deepEqual(sent.filter((m) => m.startsWith("F0 40 7F 10 08 02")), ["F0 40 7F 10 08 02 55 00 08 5F 1D 00 F7"]);
});

// field 2026-11-09: EX | Warm, the VT group read was answered, the preset showed "–"
test("VT preset: a value the app does not hold yet does not block the match (stock verifyParam); no match = User", async () => {
  const { store, ctx, notify, sent } = await setup();
  notify([0x55, 0x00, 0x00, 0x5f, 0x1d, 0x00]);
  await tick();
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  const sel = field<HTMLSelectElement>(el, "Virtual Technician preset");
  assert.equal(sel.value, "", "no VT value yet: –");
  // what `02 50 00 00` brings: every Default value except DCAReleaseTime (55 45, the 55 group)
  for (const s of PS.presetSettings(VT_PRESETS[0])) if (s.p.name !== "DCAReleaseTime") notify([s.p.v1, s.p.v2, s.part, ...s.data]);
  await tick(80);
  assert.equal(sel.value, "0", "Default, not –");
  assert.ok(store.missing().includes("DCAReleaseTime@0"), "the missing value is wanted: read back once the session is ready");
  notify([0x50, 0x00, 0x00, 0x03]); // Touch Curve edited on the panel
  await tick();
  assert.equal(sel.value, "user", "no preset matches: User");
  el.remove();
});
