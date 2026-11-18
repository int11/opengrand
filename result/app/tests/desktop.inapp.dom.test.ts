// Desktop app whose shell lists Bluetooth devices in the page (preload onDevices / choose), the whole app: 「연결」
// opens ONE dialog that searches by itself; Close cancels the request without a "not found" note; a found row
// connects through the device requestDevice() returns. (Dialog details: desktop.picker.dom.test.ts.)
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

test("desktop: 「연결」 → the dialog searches and lists; Close cancels quietly; a row connects", async () => {
  const doc = dom.window.document;
  (dom.window.HTMLDialogElement.prototype as unknown as { showModal(): void }).showModal = function (this: HTMLDialogElement) { this.open = true; };
  (dom.window.HTMLDialogElement.prototype as unknown as { close(): void }).close = function (this: HTMLDialogElement) {
    if (!this.open) return;
    this.open = false;
    this.dispatchEvent(new dom.window.Event("close"));
  };
  (globalThis as unknown as { location: Location }).location = dom.window.location;
  (globalThis as unknown as { addEventListener: unknown }).addEventListener = dom.window.addEventListener.bind(dom.window);

  const ch = { startNotifications: async () => undefined, writeValueWithoutResponse: async () => undefined, addEventListener: () => {}, removeEventListener: () => {} };
  let connects = 0;
  const device = {
    id: "abc",
    name: "KAWAI CA79",
    gatt: { connect: async () => { connects++; return { getPrimaryService: async () => ({ getCharacteristic: async () => ch }), disconnect: () => {} }; } },
    addEventListener: () => {},
    removeEventListener: () => {},
  };
  let open: { resolve(d: typeof device): void; reject(e: Error): void } | null = null;
  let requests = 0;
  Object.defineProperty(globalThis.navigator, "bluetooth", {
    value: { requestDevice: () => { requests++; return new Promise<typeof device>((resolve, reject) => { open = { resolve, reject }; }); } },
    configurable: true,
  });
  const chooses: string[] = [];
  let listener: ((l: { deviceId: string; deviceName: string }[]) => void) | null = null;
  let reconnects = 0;
  (globalThis as unknown as Record<string, unknown>).ca79Desktop = {
    shell: "electron",
    bluetooth: {
      remembered: null,
      reconnect: async () => { reconnects++; return "none"; },
      onDevices: (cb: typeof listener) => { listener = cb; return () => { if (listener === cb) listener = null; }; },
      choose: (id: string) => {
        chooses.push(id);
        const o = open;
        open = null;
        if (id === "BB:02") o?.resolve(device);
        else o?.reject(Object.assign(new Error("User cancelled the requestDevice() chooser."), { name: "NotFoundError" }));
      },
    },
  };
  await import("../src/main.ts");
  await tick(30);
  const header = doc.querySelector("header")!;
  const note = doc.querySelector<HTMLElement>("#reconnect-note")!;
  const connect = doc.querySelector<HTMLButtonElement>("#connect")!;
  assert.equal(requests, 0, "nothing searches before the user asks");

  connect.click();
  assert.equal(requests, 1, "requestDevice inside the click");
  await tick(20);
  const dlg = doc.querySelector<HTMLElement>("#picker")!;
  assert.equal(doc.querySelectorAll("dialog").length, 1, "one dialog");
  assert.equal(dlg.querySelector("#bt-pair"), null);
  assert.equal(dlg.querySelector<HTMLElement>("#bt-scan")!.dataset.state, "scanning");
  listener!([{ deviceId: "BB:02", deviceName: "KAWAI CA79" }, { deviceId: "AA:01", deviceName: "Headphones" }]);
  assert.deepEqual([...dlg.querySelectorAll(".dev-name")].map((e) => e.textContent), ["KAWAI CA79", "Headphones"]);

  // Close: the request is cancelled; no "No piano found" note, no connection attempt
  [...dlg.querySelectorAll("button")].find((b) => !b.classList.contains("item") && b.id !== "bt-rescan")!.click();
  await tick(20);
  assert.deepEqual(chooses, [""]);
  assert.equal(doc.querySelector("#picker"), null);
  assert.equal(listener, null, "stopped listening");
  assert.equal(note.hidden, true, "a cancel is not an error");
  assert.equal(header.dataset.phase, "idle");
  assert.equal(connects, 0);

  // again, and pick the piano
  connect.click();
  assert.equal(requests, 2);
  await tick(20);
  listener!([{ deviceId: "BB:02", deviceName: "KAWAI CA79" }]);
  doc.querySelector<HTMLButtonElement>('#picker button.item.dev[data-id="BB:02"]')!.click();
  assert.deepEqual(chooses, ["", "BB:02"]);
  await tick(30);
  assert.equal(doc.querySelector("#picker"), null, "the dialog closed");
  assert.equal(connects, 1, "GATT connect on the device the request returned");
  assert.equal(header.dataset.phase, "connecting");
  assert.equal(note.hidden, true);
  assert.equal(requests, 2, "no second requestDevice for the connection");
  assert.equal(reconnects, 0);

  dom.window.dispatchEvent(new dom.window.Event("pagehide"));
  await tick(20);
});
