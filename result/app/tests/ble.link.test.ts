// Web Bluetooth link: re-CONNECT on the same piano (field 2026-10-31).
// After a failed sync the app CONNECTed again: a second BleLink on the same characteristic. The old
// session's 7E 01 00 and the new 7E 01 01 went out on two write chains at once; Chrome refuses a GATT
// write while one is in progress, the catch swallowed it, and the piano never saw the CONNECT (op 1B:
// seen +1 per pair). The old link also kept listening: every RX was handled twice, then three times.
import { test } from "node:test";
import assert from "node:assert/strict";

import { BleLink, bleReopenAvailable, connectBleDevice, isGattDropped, reopenBleMidi, type GattChar } from "../src/transport/ble.ts";
import { BleMidiDecoder, encodeBlePackets } from "../src/protocol/blemidi.ts";
import { fromHex, toHex } from "../src/protocol/sysex.ts";

/** A characteristic that behaves like Chrome's: one GATT operation at a time, the next one rejected. */
function chromeChar() {
  const written: string[] = [];
  let busy = false;
  let refused = 0;
  const listeners = new Set<(e: Event) => void>();
  const dec = new BleMidiDecoder((m) => written.push(toHex(m)));
  const ch: GattChar & { notify(msg: Uint8Array): void } = {
    value: undefined,
    startNotifications: async () => undefined,
    writeValueWithoutResponse(data: Uint8Array) {
      if (busy) {
        refused++;
        return Promise.reject(new Error("NetworkError: GATT operation already in progress."));
      }
      busy = true;
      return new Promise<void>((resolve) =>
        setTimeout(() => {
          busy = false;
          dec.feed(data);
          resolve();
        }, 2),
      );
    },
    addEventListener: (_t, cb) => void listeners.add(cb),
    removeEventListener: (_t, cb) => void listeners.delete(cb),
    notify(msg: Uint8Array) {
      for (const p of encodeBlePackets([msg], 0, 23)) {
        ch.value = new DataView(p.buffer, p.byteOffset, p.byteLength);
        for (const l of [...listeners]) l(new Event("characteristicvaluechanged"));
      }
    },
  };
  return { ch, written, refused: () => refused, listeners };
}

const settle = () => new Promise((r) => setTimeout(r, 30));
const ASYNC = "F0 40 7F 7E 08 02 01 00 F7";
const SYNC = "F0 40 7F 7E 08 02 01 01 F7";

test("re-CONNECT on the same characteristic: the old link's 7E 01 00 and the new 7E 01 01 both go out, in order", async () => {
  const c = chromeChar();
  const old = new BleLink("CA79", c.ch, () => {}, 23);
  old.send(fromHex("F0 40 7F 07 08 02 55 04 3B F7"));
  await settle();
  const fresh = new BleLink("CA79", c.ch, () => {}, 23);
  old.send(fromHex(ASYNC)); // attach(): session?.disconnect() on the old link
  fresh.send(fromHex(SYNC)); // then connect() on the new one, same tick
  await settle();
  assert.deepEqual(c.written.slice(1), [ASYNC, SYNC]);
  assert.equal(c.refused(), 0);
  assert.equal(old.writer, fresh.writer);
  assert.equal(old.writer.stats.dropped, 0);
});

test("only the newest link on a characteristic listens", async () => {
  const c = chromeChar();
  const rx: string[] = [];
  const a = new BleLink("CA79", c.ch, () => {}, 23);
  a.onMessage((m) => rx.push("a " + toHex(m)));
  const b = new BleLink("CA79", c.ch, () => {}, 23);
  b.onMessage((m) => rx.push("b " + toHex(m)));
  const d = new BleLink("CA79", c.ch, () => {}, 23);
  d.onMessage((m) => rx.push("d " + toHex(m)));
  c.ch.notify(fromHex("F0 7D 43 41 37 39 5B 00 00 F7"));
  assert.deepEqual(rx, ["d F0 7D 43 41 37 39 5B 00 00 F7"]);
  assert.equal(c.listeners.size, 1);
  d.close();
  assert.equal(c.listeners.size, 0);
});

test("a write the browser refuses is retried, not lost", async () => {
  const c = chromeChar();
  const a = new BleLink("CA79", c.ch, () => {}, 23);
  void c.ch.writeValueWithoutResponse(new Uint8Array([0x80, 0x80, 0xf0, 0x80, 0xf7])); // someone else's write
  a.send(fromHex(SYNC));
  await new Promise((r) => setTimeout(r, 120));
  assert.equal(c.refused(), 1);
  assert.equal(a.writer.stats.retried, 1);
  assert.equal(a.writer.stats.dropped, 0);
  assert.deepEqual(c.written.slice(-1), [SYNC]);
});

// ---- first connect after a page reload (field M2, 2026-11-01) ----

const DROPPED = "GATT Server is disconnected. Cannot retrieve services. (Re)connect first with `device.gatt.connect`.";

/** A device whose GATT setup fails with the given errors, one per connect, then works. */
function flakyDevice(id: string, failures: Error[]) {
  let connects = 0;
  const ch = { value: undefined, startNotifications: async () => undefined, writeValueWithoutResponse: async () => undefined, addEventListener: () => {}, removeEventListener: () => {} };
  const device = {
    id,
    name: "CA79",
    gatt: {
      connect: async () => {
        const fail = failures[connects++];
        return {
          getPrimaryService: async () => {
            if (fail) throw fail;
            return { getCharacteristic: async () => ch };
          },
          disconnect: () => {},
        };
      },
    },
    addEventListener: () => {},
    removeEventListener: () => {},
  };
  return { device, connects: () => connects };
}

const netErr = (msg: string, name = "NetworkError") => Object.assign(new Error(msg), { name });

test("first connect: 'GATT Server is disconnected' right after gatt.connect() → connected on the second try", async () => {
  const d = flakyDevice("m2-a", [netErr(DROPPED)]);
  const link = await connectBleDevice(d.device, 23, { retryDelayMs: 1 });
  assert.equal(d.connects(), 2);
  assert.equal(link.key, "ble:m2-a");
  link.close();
});

test("first connect: retried once only, and never for other errors (chooser cancelled, service missing)", async () => {
  const twice = flakyDevice("m2-b", [netErr(DROPPED), netErr(DROPPED)]);
  await assert.rejects(connectBleDevice(twice.device, 23, { retryDelayMs: 1 }), /GATT Server is disconnected/);
  assert.equal(twice.connects(), 2);
  const other = flakyDevice("m2-c", [netErr("No Services matching UUID found in Device.", "NotFoundError")]);
  await assert.rejects(connectBleDevice(other.device, 23, { retryDelayMs: 1 }), /No Services/);
  assert.equal(other.connects(), 1);
  assert.equal(isGattDropped(netErr("Connection attempt failed.")), true);
  assert.equal(isGattDropped(netErr("User cancelled the requestDevice() chooser.", "NotFoundError")), false);
});

test("auto-reconnect after a reload needs navigator.bluetooth.getDevices(): without it, a clear error and bleReopenAvailable() false", async () => {
  const nav = globalThis.navigator as unknown as Record<string, unknown>;
  const had = Object.getOwnPropertyDescriptor(nav, "bluetooth");
  try {
    Object.defineProperty(nav, "bluetooth", { value: { requestDevice: async () => { throw new Error("no"); } }, configurable: true });
    assert.equal(bleReopenAvailable(), false);
    await assert.rejects(reopenBleMidi("ble:x"), /getDevices\(\) is not available/);
    const d = flakyDevice("x", []);
    Object.defineProperty(nav, "bluetooth", { value: { requestDevice: async () => d.device, getDevices: async () => [d.device] }, configurable: true });
    assert.equal(bleReopenAvailable(), true);
    const link = await reopenBleMidi("ble:x");
    assert.equal(link.key, "ble:x");
    link.close();
  } finally {
    if (had) Object.defineProperty(nav, "bluetooth", had);
    else delete nav.bluetooth;
  }
});
