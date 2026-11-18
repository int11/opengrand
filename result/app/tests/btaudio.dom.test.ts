// Bluetooth audio wording (ui/btaudio.ts) and the app entry with the connection state machine
// One text for the header badge and the power card, the
// header drawn from the connection state only.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const i18n = await import("../src/i18n.ts");
const { btAudioText } = await import("../src/ui/btaudio.ts");

test("BT audio text: standby (module on / off), playing, connected, idle — holding shown", () => {
  i18n.setLang("en");
  const d = (state: "on" | "standby", btAudio: "idle" | "connected" | "streaming", holding = false, on?: boolean) =>
    btAudioText({ state, btAudio, holding, ...(on === undefined ? {} : { tv: { btModuleOn: on } }) });
  assert.equal(d("standby", "idle", false, true), "Standby · Bluetooth audio listening (sound wakes the piano)");
  assert.equal(d("standby", "connected", false, false), "Standby · Bluetooth audio off");
  assert.equal(d("standby", "idle"), "Standby · Bluetooth audio off");
  assert.equal(d("on", "streaming", true), "Bluetooth audio playing · keeps the piano on");
  assert.equal(d("on", "streaming"), "Bluetooth audio playing");
  assert.equal(d("on", "connected", true), "Bluetooth audio connected · keeps the piano on");
  assert.equal(d("on", "idle"), "Bluetooth audio: no device");
  i18n.setLang("ko");
  assert.equal(d("on", "streaming"), "블루투스 오디오 재생 중");
  i18n.setLang("en");
});

test("app entry: header from the connection state — Connect opens the list, no remembered piano → idle", async () => {
  const doc = dom.window.document;
  (globalThis as unknown as { location: Location }).location = dom.window.location;
  (globalThis as unknown as { addEventListener: unknown }).addEventListener = dom.window.addEventListener.bind(dom.window);
  (dom.window.HTMLDialogElement.prototype as unknown as { showModal(): void }).showModal = function (this: HTMLDialogElement) { this.open = true; };
  (dom.window.HTMLDialogElement.prototype as unknown as { close(): void }).close = function (this: HTMLDialogElement) { this.open = false; };
  try { (globalThis as unknown as { navigator: unknown }).navigator ??= dom.window.navigator; } catch { /* read-only navigator */ }
  await import("../src/main.ts");
  const btn = doc.querySelector<HTMLButtonElement>("#connect")!;
  assert.equal(btn.textContent, "Connect");
  assert.equal(btn.disabled, false);
  assert.equal(doc.querySelector("header .status")?.textContent, "Not connected");
  assert.equal(doc.querySelector<HTMLElement>("#displaced")!.hidden, true);
  assert.equal(doc.querySelector<HTMLElement>("#bt-audio")!.hidden, true);
  btn.click();
  await tick(20);
  const dlg = doc.querySelector("dialog.sheet");
  assert.ok(dlg, "device list opened");
  dlg!.remove();
});

test("「블루투스」 entry points: the tab and the header chip open the same page", async () => {
  // One page, reached from the tab list / the scrolling tab row and the header chip
  const doc = dom.window.document;
  const tab = doc.querySelector<HTMLButtonElement>("nav #tabbar button[data-tab=bluetooth]");
  assert.ok(tab, "a regular tab, in the one tab row");
  assert.ok(tab!.querySelector(".ico svg"), "with its icon");
  doc.querySelector<HTMLButtonElement>("nav button[data-tab=sound]")!.click();
  assert.equal(doc.querySelector("#bt-page"), null);
  tab!.click();
  assert.ok(doc.querySelector("main #bt-page"), "the tab opens the page");
  assert.equal(tab!.classList.contains("on"), true);
  doc.querySelector<HTMLButtonElement>("nav button[data-tab=sound]")!.click();
  assert.equal(doc.querySelector("#bt-page"), null);
  const chip = doc.querySelector<HTMLButtonElement>("header #bt-audio")!;
  assert.equal(chip.tagName, "BUTTON");
  assert.ok(chip.querySelector(".ico svg") && chip.querySelector(".lbl"), "icon + short state");
  chip.click();
  assert.ok(doc.querySelector("main #bt-page"), "the header chip opens the same page");
  assert.ok(doc.querySelector("#bt-audio-card") && doc.querySelector("#bt-ac-card") && doc.querySelector("#bt-midi-card"));
  assert.equal(doc.querySelector<HTMLElement>("#bt-midi-connect")!.hidden, false, "phone / PC: the connect menu is reachable from the page");
  doc.querySelector<HTMLButtonElement>("nav button[data-tab=sound]")!.click();
});
