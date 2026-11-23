// Features 69 (app-side user data resets), 78 (information), 79 (favorites /
// recent), 81 (sound preview), 82 (lock mode), 11/50/51/52 (music tab).
import { byText, change, field, pianoPair, tapTwice, tick } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const S = await import("../src/ui/screens.ts");
const { createContext } = await import("../src/ui/context.ts");
const { SoundLibrary, RECENT_MAX } = await import("../src/state/settings.ts");
const { infoScreen, APP_VERSION } = await import("../src/ui/info.ts");
const i18n = await import("../src/i18n.ts");

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

async function setup() {
  const p = await pianoPair();
  const store = new ParamStore();
  bindStore(store);
  store.attach(p.session);
  const ctx = createContext(store, { session: () => p.session });
  return { ...p, store, ctx };
}

test("library: favorites toggle, recent is MRU without duplicates, data persists", () => {
  const lib = new SoundLibrary();
  const a = { msb: 95, lsb: 27, pc: 4, name: "EX | Concert" };
  const b = { msb: 121, lsb: 0, pc: 17, name: "Jazz Organ" };
  assert.equal(lib.toggleFavorite(a), true);
  assert.equal(lib.isFavorite({ ...a, name: "renamed" }), true);
  for (let i = 0; i < 30; i++) lib.pushRecent({ msb: 0, lsb: 0, pc: i, name: `S${i}` });
  lib.pushRecent(b);
  lib.pushRecent(a);
  lib.pushRecent(b);
  assert.equal(lib.recent.length, RECENT_MAX);
  assert.deepEqual(lib.recent.slice(0, 2).map((s) => s.name), ["Jazz Organ", "EX | Concert"]);
  const again = new SoundLibrary();
  assert.equal(again.favorites.length, 1);
  assert.equal(again.recent[0].name, "Jazz Organ");
  assert.equal(again.toggleFavorite(a), false);
  // corrupt storage is ignored
  localStorage.setItem("ca79ng.library", JSON.stringify({ favorites: [{ msb: 999 }], recent: "x" }));
  assert.equal(new SoundLibrary().favorites.length, 0);
});

test("sound tab: star a sound, see it under Favorites; selecting fills Recent", async () => {
  const { store, ctx, sent } = await setup();
  const el = S.soundScreen(store, ctx);
  document.body.append(el);
  const firstItem = el.querySelector(".sound-item")!;
  const name = firstItem.querySelector(".item")!.textContent!;
  (firstItem.querySelector(".star") as HTMLElement).click();
  byText(el, "button.chip", /Favorites/).click();
  assert.deepEqual([...el.querySelectorAll(".sound-item .item")].map((b) => b.textContent), [name]);
  (el.querySelector(".sound-item .item") as HTMLElement).click();
  await tick();
  assert.ok(sent.some((s) => s.startsWith("F0 40 7F 10 08 02 55 00 00")), "Sound sent for Main");
  byText(el, "button.chip", "Recent").click();
  assert.equal(el.querySelector(".sound-item .item")!.textContent, name);
  // user sound: save current registration, then it is listed
  byText(el, "button.chip", "User").click();
  byText(el, "button", "Save current as user sound").click();
  assert.equal(ctx.library.user.length, 1);
  assert.equal(ctx.library.user[0].parts[0].sound.name, name);
  el.remove();
});

test("user data resets clear favorites / recent / user sounds (feature 69)", async () => {
  const { store, ctx } = await setup();
  const s = { msb: 1, lsb: 2, pc: 3, name: "x" };
  ctx.library.toggleFavorite(s);
  ctx.library.pushRecent(s);
  ctx.library.saveUser({ name: "Mine", keyboardMode: 1, parts: [{ part: 0, sound: s }] });
  const el = S.settingsScreen(store, ctx);
  assert.match(el.querySelector("#user-data p")!.textContent!, /1 favorites, 1 recent, 1 user sounds/);
  (el.querySelector('[data-reset="favorites"]') as HTMLElement).click();
  assert.equal(ctx.library.favorites.length, 1, "first tap only asks (in-page confirm)");
  assert.match(el.querySelector("[data-confirm=ask]")!.textContent!, /Clear all favorites\?/);
  (el.querySelector('[data-reset="favorites"]') as HTMLElement).click();
  assert.equal(el.querySelector("[data-confirm=ask]"), null);
  assert.equal(ctx.library.favorites.length, 0);
  assert.equal(ctx.library.recent.length, 1);
  tapTwice(el.querySelector('[data-reset="recent"]'));
  tapTwice(el.querySelector('[data-reset="user"]'));
  const fresh = new SoundLibrary();
  assert.deepEqual([fresh.favorites.length, fresh.recent.length, fresh.user.length], [0, 0, 0]);
});

test("sound preview: start/stop for the edited VT parameter, preview volume (feature 81)", async () => {
  const { store, ctx, sent } = await setup();
  const el = S.vtScreen(store, ctx);
  document.body.append(el);
  // user taps Damper Noise -> preview target follows (a tap = click; pointerdown alone may be a scroll starting)
  const dn = [...el.querySelectorAll(".row")].find((r) => r.querySelector("span")?.textContent === "Damper Noise")!;
  (dn.querySelector("span") as HTMLElement).click();
  assert.equal(field<HTMLSelectElement>(el, "Parameter").value, "DamperNoise");
  byText(el, "button", /Preview$/).click();
  await tick();
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 1D 7F 03 F7"), "SoundPreviewStart Damper Noise");
  byText(el, "button", /Stop$/).click();
  await tick();
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 1E 7F 03 F7"), "SoundPreviewStop Damper Noise");
  // switching target while playing stops the previous preview first
  change(field<HTMLSelectElement>(el, "Parameter"), "Temperament");
  byText(el, "button", /Preview$/).click();
  await tick();
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 1D 7F 0D F7"), "SoundPreviewStart Temperament (13)");
  const vol = field(el, "Preview volume");
  change(vol, "90", "input");
  await tick(30);
  assert.ok(sent.includes("F0 40 7F 10 08 02 50 1F 7F 5A F7"), "SoundPreviewVolume 90");
  // the 22 technician rows are unchanged
  assert.equal(el.querySelectorAll("section:nth-of-type(2) .row").length, 22);
  el.remove();
});

test("lock mode control sends LockMode [mode, 0] (feature 82)", async () => {
  const { store, ctx, sent, notify } = await setup();
  const el = S.settingsScreen(store, ctx);
  const sel = field<HTMLSelectElement>(el, "Lock mode");
  assert.deepEqual([...sel.options].map((o) => o.textContent), ["Off", "Panel", "Volume", "Panel+Volume"]);
  change(sel, "3");
  await tick();
  assert.ok(sent.includes("F0 40 7F 10 08 02 51 22 7F 03 00 F7"), sent.join("\n"));
  notify([0x51, 0x22, 0x7f, 1, 0]);
  await tick(5);
  assert.equal(sel.selectedOptions[0].textContent, "Panel");
});

test("information screen shows app, model, firmware and BLE module versions (feature 78)", async () => {
  const { store, ctx, session, piano } = await setup();
  session.identity = { group: 4, machine: 0x1e, sub1: 0, sub2: 2, model: "CA79", version: "1.0.0" };
  // the BLE module answers the firmware query
  piano.onMessage((m) => {
    if (m[1] === 0x00 && m[6] === 0x10 && m[7] === 0x04) piano.send(Uint8Array.from([0xf0, 0, 2, 8, 0x10, 0x55, 0x20, 0x04, ..."2.1.3".split("").map((c) => c.charCodeAt(0)), 0xf7]));
  });
  const el = infoScreen(ctx);
  document.body.append(el);
  assert.equal(el.querySelector("#app-version")!.textContent, APP_VERSION);
  assert.equal(el.querySelector("#model")!.textContent, "CA79");
  assert.equal(el.querySelector("#fw-version")!.textContent, "1.0.0");
  byText(el, "button", "Check").click();
  await tick(20);
  assert.equal(el.querySelector("#ble-version")!.textContent, "2.1.3");
  void store;
  el.remove();
});

