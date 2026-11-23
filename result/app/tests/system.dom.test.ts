// Features 71 LCD brightness, 72 display scale, 73 auto display off,
// 75 startup screen, 77 language: the "System" settings group, the display
// controller and persistence in localStorage.
import { dom, field, change, tapTwice } from "./_dom.ts";
import { beforeEach, test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const S = await import("../src/ui/screens.ts");
const { createContext } = await import("../src/ui/context.ts");
const { DisplayController, brightnessToBacklight } = await import("../src/ui/display.ts");
const settings = await import("../src/state/settings.ts");
const i18n = await import("../src/i18n.ts");
const { STARTUP_STRINGS } = await import("../src/ui/startup.ts");
const { RECORDER_STRINGS } = await import("../src/ui/recorder.ts");
const { OTA_LOADER_STRINGS } = await import("../src/ui/otaloader.ts");

beforeEach(() => {
  localStorage.clear();
  i18n.setLang("en");
});

/** Fake timers + a fake Android bridge capturing setBacklight. */
function harness() {
  const backlight: number[] = [];
  const timers: { fn: () => void; ms: number; id: number }[] = [];
  let next = 1;
  const display = new DisplayController({
    setBacklight: (l) => backlight.push(l),
    root: document.documentElement,
    events: document,
    setTimer: (fn, ms) => { const id = next++; timers.push({ fn, ms, id }); return id; },
    clearTimer: (h) => { const i = timers.findIndex((t) => t.id === h); if (i >= 0) timers.splice(i, 1); },
  });
  const store = new ParamStore();
  let rerenders = 0;
  const ctx = createContext(store, { display, panel: true, rerender: () => rerenders++ });
  ctx.settings = settings.loadSettings();
  display.apply(ctx.settings);
  return { backlight, timers, display, store, ctx, rerenders: () => rerenders };
}

test("settings load defaults, repair bad values and survive a throwing storage", () => {
  const s = settings.loadSettings();
  assert.equal(s.brightness, 8);
  assert.equal(s.scale, 100);
  localStorage.setItem("ca79ng.settings", JSON.stringify({ brightness: 99, scale: 7, autoOffMin: 7, startTab: "nope", lang: "xx" }));
  const r = settings.loadSettings();
  assert.deepEqual([r.brightness, r.scale, r.autoOffMin, r.startTab, r.lang], [10, 100, 0, "sound", "en"]);
  // the Piano tab is in the Sound tab since 2026-11-09: a stored "piano" start screen opens the Sound tab
  localStorage.setItem("ca79ng.settings", JSON.stringify({ startTab: "piano" }));
  assert.equal(settings.loadSettings().startTab, "sound");
  assert.equal((settings.TAB_IDS as readonly string[]).includes("piano"), false);
  localStorage.setItem("ca79ng.settings", "{not json");
  assert.equal(settings.loadSettings().brightness, 8);
  const throwing = { getItem() { throw new Error("SecurityError"); }, setItem() { throw new Error("QuotaExceeded"); } } as unknown as Storage;
  assert.equal(settings.loadSettings(throwing).scale, 100);
  assert.equal(settings.saveSettings(settings.DEFAULT_SETTINGS, throwing), false);
  assert.equal(settings.saveSettings(settings.DEFAULT_SETTINGS, null), false);
});

test("System group: brightness goes to the bridge backlight and is persisted", () => {
  const { backlight, store, ctx } = harness();
  assert.equal(backlight.at(-1), brightnessToBacklight(8));
  const el = S.settingsScreen(store, ctx);
  const sys = el.querySelector("#system-settings")!;
  assert.equal(sys.querySelector("h3")!.textContent, "System");
  change(field(sys, "LCD brightness"), "3", "input");
  assert.equal(backlight.at(-1), Math.round(0.3 * 255));
  assert.equal(JSON.parse(localStorage.getItem("ca79ng.settings")!).brightness, 3);
});

test("display scale is a CSS scale on the root element", () => {
  const { store, ctx } = harness();
  const el = S.settingsScreen(store, ctx);
  const sel = field<HTMLSelectElement>(el, "Display scale");
  assert.deepEqual([...sel.options].map((o) => o.textContent), ["80%", "90%", "100%", "110%", "125%", "150%"]);
  change(sel, "125");
  assert.equal(document.documentElement.style.getPropertyValue("--ui-scale"), "1.25");
  assert.equal(settings.loadSettings().scale, 125);
});

test("auto display off dims after N minutes without touch and wakes on touch", () => {
  const { backlight, timers, display, store, ctx } = harness();
  const el = S.settingsScreen(store, ctx);
  document.body.append(el);
  change(field<HTMLSelectElement>(el, "Auto display off"), "5");
  assert.equal(timers.length, 1);
  assert.equal(timers[0].ms, 5 * 60_000);
  // activity re-arms the timer
  document.dispatchEvent(new dom.window.Event("pointerdown"));
  assert.equal(timers.length, 1);
  timers[0].fn();
  timers.length = 0;
  assert.equal(display.dimmed, true);
  assert.equal(backlight.at(-1), 0);
  assert.ok(document.documentElement.classList.contains("display-off"));
  // the waking touch restores brightness and its click is swallowed
  let clicked = 0;
  const btn = el.querySelector<HTMLButtonElement>("button:not([disabled])")!;   // any live button (the full reset starts disabled)
  btn.addEventListener("click", () => clicked++);
  btn.dispatchEvent(new dom.window.Event("pointerdown", { bubbles: true }));
  btn.click();
  assert.equal(display.dimmed, false);
  assert.equal(backlight.at(-1), brightnessToBacklight(8));
  assert.equal(clicked, 0);
  assert.equal(timers.length, 1, "timer re-armed after waking");
  btn.click();
  assert.equal(clicked, 1);
  // 0 = never
  change(field<HTMLSelectElement>(el, "Auto display off"), "0");
  assert.equal(timers.length, 0);
  el.remove();
});

test("startup screen choice is persisted and offers every tab", () => {
  const { store, ctx } = harness();
  const el = S.settingsScreen(store, ctx);
  const sel = field<HTMLSelectElement>(el, "Startup screen");
  assert.equal(sel.options.length, settings.TAB_IDS.length);
  change(sel, "metronome");
  assert.equal(settings.loadSettings().startTab, "metronome");
});

test("language: switching to Japanese/Korean persists, re-renders and translates every UI string", async () => {
  const { store, ctx, rerenders } = harness();
  const el = S.settingsScreen(store, ctx);
  change(field<HTMLSelectElement>(el, "Language"), "ja");
  assert.equal(rerenders(), 1);
  assert.equal(i18n.getLang(), "ja");
  assert.equal(settings.loadSettings().lang, "ja");
  assert.equal(document.documentElement.lang, "ja");
  const ja = S.settingsScreen(store, ctx);
  assert.equal(ja.querySelector("#system-settings h3")!.textContent, "システム");

  // render every screen in each language; all t() strings must have an entry
  const { chordScreen } = await import("../src/ui/chords.ts");
  const { infoScreen } = await import("../src/ui/info.ts");
  const { recorderScreen } = await import("../src/ui/recorder.ts");
  const { touchCurveEditor, perNoteEditor } = await import("../src/ui/vtedit.ts");
  const { PER_NOTE } = await import("../src/features/vtedit.ts");
  const { RESULT } = await import("../src/protocol/fileaccess.ts");
  for (const lang of ["ja", "ko"] as const) {
    i18n.setLang(lang);
    for (const f of [() => S.soundScreen(store, ctx), () => S.vtScreen(store, ctx), () => S.metronomeScreen(store, new EventTarget()), () => S.musicScreen(store), () => S.settingsScreen(store, ctx), () => chordScreen(ctx), () => infoScreen(ctx), () => recorderScreen(ctx), () => touchCurveEditor(store).el, ...PER_NOTE.map((k) => () => perNoteEditor(store, k).el)]) f();
    for (const r of Object.values(RESULT)) i18n.t(r); // result codes are shown through tl()
    for (const r of S.CM_RESULT) i18n.t(r);
    for (const k of Object.values(S.TAB_TITLES)) i18n.t(k);
  }
  // strings only shown in dynamic states (status line, update/DFU phases, empty lists)
  for (const k of DYNAMIC) i18n.t(k);
  assert.deepEqual(i18n.untranslated("ja"), [], "missing ja");
  assert.deepEqual(i18n.untranslated("ko"), [], "missing ko");
  i18n.setLang("ko");
  assert.equal(i18n.t("Settings"), "설정");
  assert.equal(i18n.t("Update to {v}", { v: "1.2.0" }).includes("1.2.0"), true);
});

const DYNAMIC = [
  // startup settings card (ui/startup.ts): shown only with a feat2 0x04 piano
  ...STARTUP_STRINGS,
  // Recorder tab (ui/recorder.ts): states, refusals and questions a screen test does not reach
  ...RECORDER_STRINGS,
  // OTA loader v4 rows, refusals and confirm (ui/otaloader.ts): shown only with an ota_loader_v4 piano
  ...OTA_LOADER_STRINGS,
  "Writes a signed cave payload into the free NOR slot; the piano starts it at the next power-on and falls back to the previous one if it fails. Hold A0, A#0 and B0 while switching on to start the built-in cave.", "Key {id} imported.", "No key imported.", "empty", "confirmed", "unconfirmed", "rejected", "failed check", "That is not a 32-byte key.", "{n} bytes, features {f0}/{f1}, key {id}.", "Not a OpenGrand payload file (.c79).", "Import the key first.", "Pick a payload file first.", "Uploading… {a}/{b} bytes", "Writing to the slot… ({s})", "Written to {slot}. Turn the piano off and on to start it.", "The built-in cave is running: nothing to roll back.", "Slot rejected. Turn the piano off and on to go back.",
  "Not connected", "Connecting to {name}…", "Disconnected", "Connection failed: {e}", "{model}{version} · {link}",
  "Paused while the app is in the background", "Syncing with {name}…", "Connecting…", "Connected", "Waking the piano…", "Another device (the panel or another app) took control", "The piano's panel took control as it started up — this firmware lets one device control the piano at a time.", "Use this app", "The connection to {name} was lost.", "Reconnect", "Try again", "Disconnect", "Connect another device…", "in use", "Now: {state}", "Standby · Bluetooth audio listening (sound wakes the piano)", "Standby · Bluetooth audio off", "Bluetooth audio playing · keeps the piano on", "Bluetooth audio playing", "Bluetooth audio connected · keeps the piano on", "Bluetooth audio connected", "Bluetooth audio: no device", "To switch the audio source (phone ↔ TV): disconnect Bluetooth on the device playing now, then connect the other one to the piano.",
  "Searching…", "Connect to the piano", "Close", "Connect", "Bluetooth MIDI (pair new)…", "Firmware simulator ({url})",
  "Checking…", "No reply", "Update check failed: {e}", "Check again", "Version {v} on the USB memory is not newer.", "Update to {v}",
  "Update failed: {e}", "Update installed.", "Installing…", "Reading module version…", "Looking for the update package…",
  "Restarting the module in update mode…", "Searching for the module bootloader…", "Updating… {p}%", "Module updated.",
  "Module firmware {v} is up to date.", "Module update failed: {e}", "Connect to the piano first.", "No update package on the USB memory (CA79NG/manifest.json).",
  "Reads CA79NG/blemidi.zip from the USB memory (or the app's storage) and updates the Bluetooth MIDI module.",
  "No favorites yet. Tap ☆ next to a sound.", "No sounds played yet.", "No user sounds saved.", "Save current as user sound", "Name for this user sound", "User {n}", "Delete",
  "USB memory attached", "No USB memory", "{op}…", "{op}: done", "{op} failed: {e}", "{n} files", "Import", "Rename", "New name for {name}", "Delete {name}?",
  "Choose a MIDI file first.", "Upload", "Format the USB memory? All files will be erased.", "Read the USB memory",
  "Finish learning", "Play softly and strongly (at least 4 notes), then finish.", "The piano made a curve from your playing.",
  "Touch curve set to User. Edits apply immediately.", "The piano did not send the curve.", "Saved on the piano.", "Unsaved changes discarded.",
  "{name} set to User. Edits apply immediately.", "The piano did not send the table.", "{r} · {n} notes played",
  "Deleting…", "All recorder songs deleted.", "Song {n} deleted.", "Delete all recorder songs on the piano?",
  "Voicing", "Key volume", "Tuning", "Temperament", "cent",
  "{name}: not supported", "3rd inversion", "{n}th inversion", "Stop", "Start", "Android shell", "Reset all settings to factory defaults?",
  // Power card (only with a OpenGrand power payload) and the start-up wake
  "No limit (until woken)", "This firmware has no \"no limit\" standby (it needs ota_power3).", "Wake the piano when this app opens",
  "Woke the piano from standby.", "No standby limit: it stays in standby until something wakes it and never switches itself off.",
  "After the standby limit it switches off (never while it is saving).",
  // remote off (ui/power.ts, feat2 0x20)
  "Turn off (standby)", "Turn off", "The piano is switching off.", "The piano is not on.",
  "Put the piano into standby now? Speakers go silent; a key, the panel or this app wakes it.",
  "Switch the piano off now? It can only be switched on again with its power button.",
  "Not now: the piano is recording. Stop the recording first.", "Not now: the piano is busy with a file. Try again in a moment.",
  "Not now: the piano is saving. Try again in a moment.",
  "In standby the speakers and Bluetooth audio are off; a key, the panel or this app wakes the piano.",
  "In standby the speakers are off but Bluetooth audio stays on: when the TV or phone starts playing, the piano wakes up.",
  // tab cleanup: the power card's short rule, 「자세히」, the fold's remote off;
  // the Bluetooth page's lines that need a source-manager piano
  "Bluetooth audio does not keep the piano on.", "While Bluetooth audio plays, the piano stays on.", "While Bluetooth audio plays, the piano stays on for up to {n}.",
  "Details", "Off the way auto power-off does", "Installs a new OpenGrand firmware file (.c79) over Bluetooth. It starts at the next power-on.",
  "Playing through the piano: {dev}", "Picked: {dev} — no sound from it yet", "No device picked yet — press “Listen to this one” on the device you want to hear.",
  "Choosing and reconnecting devices", "Reconnect at power-on", "Used when the piano is switched on, and when it wakes from a standby that switched Bluetooth audio off.",
  "Not used now: the Bluetooth module reconnects by itself (AUTOCALL is on — see Advanced below).", "Advanced · developer",
  "Try this when a device plays but the piano stays silent. Every connected device drops for a few seconds.",
  "Bluetooth module's own reconnect (AUTOCALL)", "Module lines", "Power",
];

// The stock looks the startup commands up by their first data byte;
// the old 00 00 00 00 matched nothing (no action on the piano)
test("startup buttons send the stock's bytes; the full factory reset needs the tick and two taps (in-page confirm)", () => {
  const { store, ctx } = harness();
  const sent: { v2: number; data: number[] }[] = [];
  (store as unknown as { command: (p: { v2: number }, part: number, data: number[]) => Promise<boolean> }).command =
    (p, _part, data) => { sent.push({ v2: p.v2, data }); return Promise.resolve(true); };
  const el = S.settingsScreen(store, ctx);
  document.body.replaceChildren(el);
  (el.querySelector('[data-action="startup-save"]') as HTMLButtonElement).click();
  const factory = el.querySelector('[data-action="startup-factory"]') as HTMLButtonElement;
  factory.click();                                // asks in the page, sends nothing
  assert.equal(sent.length, 1);
  assert.equal(factory.dataset.armed, "1");
  assert.match(el.querySelector("[data-confirm=ask]")!.textContent!, /Reset the saved startup settings/);
  factory.click();
  assert.deepEqual(sent, [{ v2: 0x34, data: [0x07, 0x00, 0x7f, 0x7f] }, { v2: 0x36, data: [0x07, 0x7f, 0x7f, 0x7f] }]);
  const full = el.querySelector('[data-action="full-reset"]') as HTMLButtonElement;
  const ack = el.querySelector('input[name="fullResetAck"]') as HTMLInputElement;
  assert.equal(full.disabled, true);
  full.click();                                   // not armed: nothing
  assert.equal(sent.length, 2);
  change(ack, true);
  assert.equal(full.disabled, false);
  full.click();                                   // enabled: the first tap only asks
  assert.equal(sent.length, 2);
  assert.ok(el.querySelector("[data-confirm=ask]")!.textContent!.includes("10 recorder songs"));
  change(ack, false);                             // untick: disarmed, the question goes away
  assert.equal(el.querySelector("[data-confirm=ask]"), null);
  assert.equal(full.dataset.armed, undefined);
  change(ack, true);
  tapTwice(full);
  assert.deepEqual(sent[2], { v2: 0x36, data: [0x04, 0x7f, 0x7f, 0x7f] });
  assert.equal(full.disabled, true);              // disarmed again
  assert.equal(ack.checked, false);
  assert.equal(el.querySelector("[data-confirm=ask]"), null);
  el.remove();
});

// (owner 2026-11-14): each control in the tab where one looks for it
test("settings layout: a 「전원」 card with the auto power-off; the piano's Bluetooth settings only on the Bluetooth tab", () => {
  const { store, ctx } = harness();
  i18n.setLang("ko");
  const el = S.settingsScreen(store, ctx);
  const titles = [...el.querySelectorAll(":scope > section > h3")].map((h) => h.textContent);
  assert.deepEqual(titles.slice(0, 6), ["앱 · 화면", "튜닝 · 키 트랜스포즈", "스피커 / 헤드폰", "전원", "MIDI", "시스템"]);
  assert.ok(!titles.includes("블루투스") && !titles.includes("Bluetooth"), "no Bluetooth card on Settings");
  const power = el.querySelector<HTMLElement>("#power-settings")!;
  assert.equal(field<HTMLSelectElement>(power, "자동 전원 꺼짐").tagName, "SELECT", "the stock auto power-off row is in 「전원」");
  assert.ok(power.querySelector("#power-policy"), "the OpenGrand part (hidden without the feature) sits in the same card");
  assert.ok(![...el.querySelectorAll("#system-settings .row > span:first-child")].some((s) => s.textContent === "자동 전원 꺼짐"));
  for (const label of ["블루투스 오디오", "블루투스 오디오 음량", "블루투스 MIDI"])
    assert.ok(![...el.querySelectorAll(".row > span:first-child")].some((s) => s.textContent === label), label);
  // owner 2026-11-15: one 「시작 설정」 card right after 시스템 - shown on a stock piano too, with the stock
  // actions; the full factory reset stays in 시스템 with its acknowledgement
  assert.equal(titles[6], "시작 설정");
  const startup = el.querySelector<HTMLElement>("#startup-settings")!;
  assert.equal(startup.hidden, false, "stock piano: the card shows the stock actions");
  assert.equal(startup.querySelector<HTMLElement>("#startup-ng")!.hidden, true, "no OpenGrand part without the feature");
  assert.ok(startup.querySelector("#startup-stock [data-action=startup-save]") && startup.querySelector("#startup-stock [data-action=startup-factory]"));
  const sys = el.querySelector("#system-settings")!;
  assert.equal(sys.querySelector("[data-action=startup-save], [data-action=startup-factory]"), null);
  assert.ok(sys.querySelector("[data-action=full-reset]") && sys.querySelector("input[name=fullResetAck]"));
  // no standby / wake / off anywhere on Settings without the power feature, and none in the normal view with it
  assert.equal(el.querySelector("[data-action=standby-now], [data-action=wake]"), null);
});

test("information layout: versions with OpenGrand in one card, the updates, then one closed developer fold with the panel screen", async () => {
  const { ctx } = harness();
  const { infoScreen } = await import("../src/ui/info.ts");
  i18n.setLang("ko");
  const el = infoScreen(ctx);
  const first = el.querySelector(":scope > section")!;
  assert.equal(first.querySelector("h3")!.textContent, "정보");
  assert.ok(first.querySelector("#ng-check") && first.querySelector("#ng-firmware"), "OpenGrand row in the information card");
  assert.ok(![...el.querySelectorAll("h3")].some((h) => h.textContent === "커스텀 펌웨어"));
  const dev = el.querySelector<HTMLElement>("#ng-dev")!;
  assert.equal(dev, el.lastElementChild, "developer fold last");
  assert.equal(dev.hidden, false, "phone / PC: the panel screen tool is there");
  assert.equal(dev.querySelector<HTMLDetailsElement>("#ng-dev-fold")!.open, false);
  assert.ok(dev.querySelector("#ng-panel-screen #panel-screen"), "panel screen inside the fold");
  assert.ok(el.querySelector("#ng-ota details#ota-more"), "firmware update: the mechanism behind 「자세히」");
  // owner 2026-11-16: one developer view - the slot tools and the power tests sit in this fold,
  // the update card keeps the file, the upload and the result; the ids stay for automation
  const fold = dev.querySelector<HTMLDetailsElement>("#ng-dev-fold")!;
  for (const id of ["ota-dev", "ota-state", "ota-refresh", "ota-apply", "ota-rollback", "ota-apply-after", "ota-diag", "power-dev"])
    assert.ok(fold.querySelector(`#${id}`), `#${id} in the developer fold`);
  const ota = el.querySelector<HTMLElement>("#ng-ota")!;
  for (const id of ["ota-file", "ota-upload", "ota-status", "ota-progress", "ota-keys"]) assert.ok(ota.querySelector(`#${id}`), `#${id} on the update card`);
  assert.equal(ota.querySelector("#ota-rollback, #ota-apply, #ota-state"), null);
  // ... and nothing of it is left on the Settings tab (the power tests were in Settings › 전원 › 개발자용)
  const { ctx: c2, store } = harness();
  assert.equal(S.settingsScreen(store, c2).querySelector("#power-dev, #apo-test, #remote-off-row, #gpio-diag, #app-dev"), null);
});
