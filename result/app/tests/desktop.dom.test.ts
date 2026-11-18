// Desktop app start with a remembered Bluetooth piano (no getDevices() in Electron): the app asks the shell to
// reconnect and calls requestDevice once inside the shell's hook; when the piano does not show up the
// connection picker opens (same as reopenRemembered on the web). No Chrome-flag note.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

test("desktop: remembered BLE piano → reconnect through the shell, then the picker when it is not found", async () => {
  const doc = dom.window.document;
  (dom.window.HTMLDialogElement.prototype as unknown as { showModal(): void }).showModal = function (this: HTMLDialogElement) { this.open = true; };
  (globalThis as unknown as { location: Location }).location = dom.window.location;
  (globalThis as unknown as { addEventListener: unknown }).addEventListener = dom.window.addEventListener.bind(dom.window);
  localStorage.setItem("ca79ng.lastLink", JSON.stringify({ key: "ble:abc", name: "CA79 BLE" }));
  let requests = 0;
  let reconnects = 0;
  Object.defineProperty(globalThis.navigator, "bluetooth", {
    value: { requestDevice: async () => { requests++; throw Object.assign(new Error("User cancelled the requestDevice() chooser."), { name: "NotFoundError" }); } },
    configurable: true,
  });
  const g = globalThis as unknown as Record<string, unknown>;
  g.ca79Desktop = {
    shell: "electron",
    bluetooth: {
      remembered: "CA79 BLE",
      reconnect: async () => {
        reconnects++;
        (g.__ca79ngDesktopReconnect as () => void)();
        return "started";
      },
    },
  };
  await import("../src/main.ts");
  await tick(50);
  assert.equal(reconnects, 1);
  assert.equal(requests, 1, "requestDevice once, inside the shell's hook");
  assert.equal(doc.querySelector<HTMLElement>("#reconnect-note")!.hidden, true, "no Chrome-flag note on desktop");
  assert.ok(doc.querySelector("#picker"), "not found → the connection picker opens");
});
