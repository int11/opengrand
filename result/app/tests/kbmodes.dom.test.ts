// Keyboard modes on the Sound tab (owner's manual p.29 Dual, p.30 Split, p.69 4 Hands): only the settings of the
// active mode show, each on the part the stock remote app's editors use (Dual = Layer 08, Split = Lower 09,
// 4 Hands = right 0A / left 0B with balance and split point on the left), with the stock octave limits.
import { change, field, pianoPair, tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const S = await import("../src/ui/screens.ts");
const { createContext } = await import("../src/ui/context.ts");
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
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  return { ...p, store, el };
}

const group = (el: HTMLElement, mode: string) => el.querySelector<HTMLElement>(`[data-kbmode="${mode}"]`)!;
const shown = (el: HTMLElement) => [...el.querySelectorAll<HTMLElement>("[data-kbmode]")].filter((g) => !g.hidden).map((g) => g.dataset.kbmode);
const labels = (g: HTMLElement) => [...g.querySelectorAll(".row > span:first-child")].map((s) => s.textContent);
const out = (g: HTMLElement, label: string) => field(g, label).parentElement!.querySelector("output")!.textContent;
const bounds = (g: HTMLElement, label: string) => [field(g, label).min, field(g, label).max].map(Number);
const sets = (sent: string[]) => sent.filter((m) => m.startsWith("F0 40 7F 10 "));

test("only the active mode's settings show; the mode follows the piano (panel changes too)", async () => {
  const { el, notify } = await setup();
  assert.deepEqual(shown(el), [], "mode not known yet: none");
  notify([0x53, 0x00, 0x0f, 1]);
  await tick();
  assert.deepEqual(shown(el), ["Dual"]);
  assert.deepEqual(labels(group(el, "Dual")), ["Balance", "Layer octave", "Layer dynamics"]);
  notify([0x53, 0x00, 0x0f, 2]);
  await tick();
  assert.deepEqual(shown(el), ["Split"]);
  assert.deepEqual(labels(group(el, "Split")), ["Balance", "Split point", "Lower octave", "Lower pedal"]);
  notify([0x53, 0x00, 0x0f, 3]);
  await tick();
  assert.deepEqual(shown(el), ["4Hands"]);
  assert.deepEqual(labels(group(el, "4Hands")), ["Balance", "Right octave", "Left octave", "Split point"]);
  notify([0x53, 0x00, 0x0f, 0]);
  await tick();
  assert.deepEqual(shown(el), [], "Single: none");
  el.remove();
});

test("octave limits per mode: Dual -2..+2, Split 0..+3, 4 Hands right -3..0 / left 0..+3", async () => {
  const { el } = await setup();
  assert.deepEqual(bounds(group(el, "Dual"), "Layer octave"), [-2, 2]);
  assert.deepEqual(bounds(group(el, "Split"), "Lower octave"), [0, 3]);
  assert.deepEqual(bounds(group(el, "4Hands"), "Right octave"), [-3, 0]);
  assert.deepEqual(bounds(group(el, "4Hands"), "Left octave"), [0, 3]);
  assert.deepEqual(bounds(group(el, "Split"), "Split point"), [22, 108]);
  assert.deepEqual(bounds(group(el, "Dual"), "Balance"), [0, 16]);
  el.remove();
});

test("each mode's settings go to its own part", async () => {
  const { el, sent } = await setup();
  const dual = group(el, "Dual"), split = group(el, "Split"), fh = group(el, "4Hands");
  change(field(dual, "Balance"), "4");
  change(field(dual, "Layer octave"), "-2");
  change(field(split, "Balance"), "12");
  change(field(split, "Split point"), "55");
  change(field(split, "Lower octave"), "3");
  change(field(fh, "Balance"), "16");
  change(field(fh, "Right octave"), "-3");
  change(field(fh, "Left octave"), "1");
  change(field(fh, "Split point"), "64");
  await tick(60);
  assert.deepEqual(sets(sent), [
    "F0 40 7F 10 08 02 53 04 08 20 F7", // Dual balance on Layer (step 4 = raw 32)
    "F0 40 7F 10 08 02 53 03 08 3E F7", // Layer octave -2
    "F0 40 7F 10 08 02 53 04 09 60 F7", // Split balance on Lower (step 12 = raw 96)
    "F0 40 7F 10 08 02 53 06 09 37 F7", // split point G3 on Lower
    "F0 40 7F 10 08 02 53 03 09 43 F7", // Lower octave +3
    "F0 40 7F 10 08 02 53 04 0B 7F F7", // 4 Hands balance on the left part (last step = raw 127)
    "F0 40 7F 10 08 02 53 03 0A 3D F7", // right octave -3
    "F0 40 7F 10 08 02 53 03 0B 41 F7", // left octave +1
    "F0 40 7F 10 08 02 53 06 0B 40 F7", // 4 Hands split point E4 on the left part
  ]);
  el.remove();
});

test("read back: the piano's 4 Hands values and balance steps show per part", async () => {
  const { el, notify } = await setup();
  // the piano's own 4 Hands values (stock dump: right 3E, left 42, balance 40, split 3C)
  notify([0x53, 0x03, 0x0a, 0x3e]);
  notify([0x53, 0x03, 0x0b, 0x42]);
  notify([0x53, 0x04, 0x0b, 0x40]);
  notify([0x53, 0x06, 0x0b, 0x3c]);
  notify([0x53, 0x04, 0x09, 0x00]);
  notify([0x53, 0x04, 0x08, 0x48]);
  await tick();
  const fh = group(el, "4Hands");
  assert.equal(out(fh, "Right octave"), "-2");
  assert.equal(out(fh, "Left octave"), "+2");
  assert.equal(out(fh, "Balance"), "Left 9 · Right 9");
  assert.equal(out(fh, "Split point"), "C4");
  assert.equal(out(group(el, "Split"), "Balance"), "Sub 9 · Main 1");
  assert.equal(out(group(el, "Dual"), "Balance"), "Sub 8 · Main 9");
  // the Split split point (Lower) is a different value from the 4 Hands one
  assert.equal(out(group(el, "Split"), "Split point"), "–");
  el.remove();
});
