// Desktop app, field 2026-11-03: the piano was power-cycled while the app was minimized; the link closed and
// nothing reopened it (lost waited for a click). main.ts turns the connection manager's auto-reconnect on in the
// desktop shell only: after gattserverdisconnected the banner says it is reconnecting and the same
// BluetoothDevice is connected again after the first backoff delay, without a click and without the chooser.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

test("desktop: piano gone (gattserverdisconnected) → banner 'reconnecting', the same device reconnects on its own", async () => {
  const doc = dom.window.document;
  (dom.window.HTMLDialogElement.prototype as unknown as { showModal(): void }).showModal = function (this: HTMLDialogElement) { this.open = true; };
  (globalThis as unknown as { location: Location }).location = dom.window.location;
  (globalThis as unknown as { addEventListener: unknown }).addEventListener = dom.window.addEventListener.bind(dom.window);
  localStorage.setItem("ca79ng.lastLink", JSON.stringify({ key: "ble:abc", name: "CA79" }));

  // a BLE-MIDI device whose piano never answers (the connection stays in "connecting" until it is lost)
  let connects = 0;
  const devListeners = new Set<() => void>();
  const ch = {
    startNotifications: async () => undefined,
    writeValueWithoutResponse: async () => undefined,
    addEventListener: () => {},
    removeEventListener: () => {},
  };
  const device = {
    id: "abc",
    name: "CA79",
    gatt: {
      connect: async () => {
        connects++;
        return { getPrimaryService: async () => ({ getCharacteristic: async () => ch }), disconnect: () => {} };
      },
    },
    addEventListener: (_t: string, cb: () => void) => void devListeners.add(cb),
    removeEventListener: (_t: string, cb: () => void) => void devListeners.delete(cb),
  };
  let requests = 0;
  Object.defineProperty(globalThis.navigator, "bluetooth", {
    value: { requestDevice: async () => { requests++; return device; } },
    configurable: true,
  });
  const g = globalThis as unknown as Record<string, unknown>;
  g.ca79Desktop = {
    shell: "electron",
    bluetooth: {
      remembered: "CA79",
      reconnect: async () => {
        (g.__ca79ngDesktopReconnect as () => void)();
        return "started";
      },
    },
  };
  await import("../src/main.ts");
  await tick(50);
  assert.equal(requests, 1, "start: the shell's reconnect picks the device once");
  assert.equal(connects, 1);
  const header = doc.querySelector("header")!;
  assert.equal(header.dataset.phase, "connecting");

  for (const l of [...devListeners]) l(); // the piano was switched off
  await tick(20);
  assert.equal(header.dataset.phase, "lost");
  const banner = doc.querySelector<HTMLElement>("#displaced")!;
  assert.equal(banner.hidden, false);
  assert.match(banner.textContent ?? "", /reconnecting automatically|저절로 다시 연결|自動で再接続/);

  // first backoff delay (AUTO_RECONNECT 3 s): the same device again, no chooser
  for (let i = 0; i < 40 && connects < 2; i++) await tick(100);
  assert.equal(connects, 2, "reconnected to the same BluetoothDevice on its own");
  assert.equal(requests, 1, "no second requestDevice (no chooser)");
  assert.equal(header.dataset.phase, "connecting");
  assert.equal(doc.querySelector("#picker"), null, "the device list did not open");

  // leave: the attempt is let go and the backoff loop ends (no timers left behind)
  dom.window.dispatchEvent(new dom.window.Event("pagehide"));
  await tick(20);
  assert.equal(header.dataset.phase, "idle");
});
