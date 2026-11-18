// Desktop app, field 2026-11-03: the user disconnected and reconnected "CA79" in Windows Bluetooth settings. The app's
// auto-reconnect and the header 「연결」 kept calling gatt.connect() on the BluetoothDevice it held and never connected;
// restarting the app (the shell's reconnect: requestDevice, the remembered device auto-picked) connected at once.
// main.ts gives the connection manager `reacquire` in the desktop shell: after the held device fails, the next reopen
// looks the device up the way the app start does. Here the old device object never connects again, the shell's
// reconnect hands back a fresh one, and the second header 「연결」 press connects through it — no chooser, no list.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

test("desktop: the held BluetoothDevice never reconnects → header 「연결」 re-acquires it through the shell and connects", async () => {
  const doc = dom.window.document;
  (dom.window.HTMLDialogElement.prototype as unknown as { showModal(): void }).showModal = function (this: HTMLDialogElement) { this.open = true; };
  (globalThis as unknown as { location: Location }).location = dom.window.location;
  (globalThis as unknown as { addEventListener: unknown }).addEventListener = dom.window.addEventListener.bind(dom.window);
  localStorage.setItem("ca79ng.lastLink", JSON.stringify({ key: "ble:abc", name: "CA79" }));

  const ch = {
    startNotifications: async () => undefined,
    writeValueWithoutResponse: async () => undefined,
    addEventListener: () => {},
    removeEventListener: () => {},
  };
  // a BLE-MIDI device (the piano never answers: the connection stays in "connecting" until it is lost)
  const makeDevice = () => {
    const d = {
      id: "abc",
      name: "CA79",
      stale: false,
      connects: 0,
      listeners: new Set<() => void>(),
      gatt: {
        connect: async () => {
          d.connects++;
          if (d.stale) throw new Error("NetworkError: Connection attempt failed.");
          return { getPrimaryService: async () => ({ getCharacteristic: async () => ch }), disconnect: () => {} };
        },
      },
      addEventListener: (_t: string, cb: () => void) => void d.listeners.add(cb),
      removeEventListener: (_t: string, cb: () => void) => void d.listeners.delete(cb),
    };
    return d;
  };
  const devices = [makeDevice(), makeDevice()]; // the object held from the start, the one a new lookup returns
  let requests = 0;
  Object.defineProperty(globalThis.navigator, "bluetooth", {
    value: { requestDevice: async () => devices[Math.min(requests++, 1)] }, // Electron: no getDevices()
    configurable: true,
  });
  let shellReconnects = 0;
  const g = globalThis as unknown as Record<string, unknown>;
  g.ca79Desktop = {
    shell: "electron",
    bluetooth: {
      remembered: "CA79",
      reconnect: async () => {
        shellReconnects++;
        (g.__ca79ngDesktopReconnect as () => void)();
        return "started";
      },
    },
  };
  await import("../src/main.ts");
  await tick(50);
  const [held, fresh] = devices as [ReturnType<typeof makeDevice>, ReturnType<typeof makeDevice>];
  assert.equal(shellReconnects, 1, "start: the shell's reconnect");
  assert.equal(held.connects, 1);
  const header = doc.querySelector("header")!;
  assert.equal(header.dataset.phase, "connecting");

  // Windows dropped the BLE connection and made it again: the held object will not connect any more
  held.stale = true;
  for (const l of [...held.listeners]) l();
  await tick(20);
  assert.equal(header.dataset.phase, "lost");

  const connect = doc.querySelector<HTMLButtonElement>("#connect")!;
  // first press: the piano just lost, through the held object (GATT connect + one retry 0.5 s later) — fails
  connect.click();
  for (let i = 0; i < 30 && header.dataset.phase !== "lost"; i++) await tick(50);
  assert.equal(header.dataset.phase, "lost", "the held device did not reconnect");
  assert.equal(held.connects, 3, "held device: connect + one retry");
  assert.equal(shellReconnects, 1);

  // second press: looked up again the way the app start does (shell reconnect → requestDevice → a fresh object)
  connect.click();
  for (let i = 0; i < 20 && fresh.connects === 0; i++) await tick(20);
  assert.equal(shellReconnects, 2, "the shell's reconnect ran again");
  assert.equal(requests, 2);
  assert.equal(fresh.connects, 1, "connected through the fresh device object");
  assert.equal(held.connects, 3, "the stale object was not tried again");
  await tick(20);
  assert.equal(header.dataset.phase, "connecting");
  assert.equal(doc.querySelector("#picker"), null, "the device list did not open");

  dom.window.dispatchEvent(new dom.window.Event("pagehide"));
  await tick(20);
  assert.equal(header.dataset.phase, "idle");
});
