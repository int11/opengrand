// App start with a remembered Bluetooth piano in a browser without navigator.bluetooth.getDevices()
// (field M2, 2026-11-01): no silent nothing — one line says
// why the app did not reconnect and what to do.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

test("remembered BLE piano, no getDevices(): the app says it cannot reconnect by itself and stays idle", async () => {
  const doc = dom.window.document;
  (globalThis as unknown as { location: Location }).location = dom.window.location;
  (globalThis as unknown as { addEventListener: unknown }).addEventListener = dom.window.addEventListener.bind(dom.window);
  localStorage.setItem("ca79ng.lastLink", JSON.stringify({ key: "ble:abc", name: "CA79 BLE" }));
  let chooser = 0;
  Object.defineProperty(globalThis.navigator, "bluetooth", { value: { requestDevice: async () => { chooser++; throw new Error("no"); } }, configurable: true });
  await import("../src/main.ts");
  await tick(20);
  const note = doc.querySelector<HTMLElement>("#reconnect-note")!;
  assert.equal(note.hidden, false);
  assert.match(note.textContent ?? "", /^CA79 BLE was not reconnected automatically: .*Tap Connect/);
  assert.equal(doc.querySelector("header .status")?.textContent, "Not connected");
  assert.equal(chooser, 0, "no chooser without a user gesture");
  assert.equal(localStorage.getItem("ca79ng.lastLink") !== null, true, "the piano is still remembered");
});
