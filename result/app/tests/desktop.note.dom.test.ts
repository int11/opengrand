// Desktop app start with a remembered Bluetooth piano that the shell does not remember yet (first start
// after the update): a desktop note, not the Chrome-flag one, and no request.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

test("desktop, shell remembers nothing: desktop note, stays idle", async () => {
  const doc = dom.window.document;
  (globalThis as unknown as { location: Location }).location = dom.window.location;
  (globalThis as unknown as { addEventListener: unknown }).addEventListener = dom.window.addEventListener.bind(dom.window);
  localStorage.setItem("ca79ng.lastLink", JSON.stringify({ key: "ble:abc", name: "CA79 BLE" }));
  let requests = 0;
  Object.defineProperty(globalThis.navigator, "bluetooth", { value: { requestDevice: async () => { requests++; throw new Error("no"); } }, configurable: true });
  (globalThis as unknown as Record<string, unknown>).ca79Desktop = { shell: "electron", bluetooth: { remembered: null, reconnect: async () => "none" } };
  await import("../src/main.ts");
  await tick(20);
  const note = doc.querySelector<HTMLElement>("#reconnect-note")!;
  assert.equal(note.hidden, false);
  assert.match(note.textContent ?? "", /^CA79 BLE was not reconnected automatically: choose it once under Connect/);
  assert.doesNotMatch(note.textContent ?? "", /browser/);
  assert.equal(requests, 0);
  assert.equal(doc.querySelector("header .status")?.textContent, "Not connected");
});
