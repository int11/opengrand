// Connection / session lifecycle races (review 2026-11-27, after the fast-tap field test): two async
// sequences on one MIDI link, a guard checked before an await and set after it, a retired session
// still sending, a promise that never settles.
import { test } from "node:test";
import assert from "node:assert/strict";

import { PianoSession } from "../src/protocol/session.ts";
import { decodeFrame, encodeFrame, toHex } from "../src/protocol/sysex.ts";
import { LoopbackLink } from "../src/transport/link.ts";

const tick = (ms = 20) => new Promise((r) => setTimeout(r, ms));

/** Fake stock piano: CONNECT / identity / dump; counts the dump requests it answered. */
function fakePiano(link: LoopbackLink) {
  const tx = (fn: number, body: number[], dev = 0x00) => link.send(encodeFrame({ dev, fn, group: 0x04, machine: 0x1e, body }));
  const seen: string[] = [];
  link.onMessage((m) => {
    seen.push(toHex(m));
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x01) tx(0x7e, [0x02, 0x00, 0x00]);
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) tx(0x7e, [0x05, 0x00, 0x00, 0x02]);
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x41) tx(0x7e, [0x05, 0x01, 0x00, 0x01]);
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x42) tx(0x7e, [0x05, 0x02, 0x00]);
    if (f.fn === 0x7e && f.body[0] === 0x04 && f.body[1] === 0x41) tx(0x7e, [0x04, 0x01, 0x32]);
    if (f.fn === 0x02 && f.body[0] === 0x5f) {
      tx(0x10, [0x55, 0x00, 0x00, 95, 112, 0]);
      tx(0x03, [0x5f, 0x7f, 0x7f]);
    }
    if (f.fn === 0x02 && f.body[0] === 0x55) tx(0x03, [f.body[0], f.body[1], 0x7f]);
  });
  return seen;
}

test("two overlapping takeControl() on one session: both resolve, one CONNECT + one dump on the wire", async () => {
  // e.g. the file client's recover (fn 0x24 timeout) and the manager's re-bind ("not bound") at once: before, the
  // second dump request took over the first one's end-of-dump handler and the first timed out / re-CONNECTed
  const [app, piano] = LoopbackLink.pair();
  const seen = fakePiano(piano);
  const s = new PianoSession(app);
  await s.connect(200);
  seen.length = 0;
  const r = await Promise.allSettled([s.takeControl(200), s.takeControl(200)]);
  assert.deepEqual(r.map((x) => x.status), ["fulfilled", "fulfilled"], String(r.map((x) => (x as PromiseRejectedResult).reason?.message)));
  assert.equal(s.state, "ready");
  assert.equal(seen.filter((x) => x === "F0 40 7F 7E 08 02 01 01 F7").length, 1, seen.join("\n"));
  assert.equal(seen.filter((x) => x === "F0 40 00 02 08 02 5F 7F 7F F7").length, 1, seen.join("\n"));
});

test("a retired session sends no data request on the link the next session uses", async () => {
  // a screen that still holds the old session (released when a new one took the same port) asks for a value:
  // before, requestData went out on the shared link and waited out its whole timeout
  const [app, piano] = LoopbackLink.pair();
  const seen = fakePiano(piano);
  const old = new PianoSession(app);
  await old.connect(200);
  old.release();
  seen.length = 0;
  const t0 = Date.now();
  assert.equal(await old.requestData(0x55, 0x00, 300), false);
  await tick();
  assert.deepEqual(seen, []);
  assert.ok(Date.now() - t0 < 200, "answers at once");
});

const { ConnectionManager } = await import("../src/state/connection.ts");
type MidiLink = import("../src/transport/link.ts").MidiLink;

test("another piano whose open failed: Try again / picking it again opens that piano, never the one before", async () => {
  // connected to A, 「Connect another device…」 → B, B does not open (failed, banner names B). Before, 「Try again」
  // and a re-pick of B from the list started a session on A's link — header B, every control went to A.
  const [a, pa] = LoopbackLink.pair();
  fakePiano(pa);
  const made: MidiLink[] = [];
  const m = new ConnectionManager({ makeSession: (l) => (made.push(l), new PianoSession(l)), replyMs: 100 });
  assert.equal(await m.attach(a), "ready");
  let opens = 0;
  const B = { key: "ble:B", name: "B", open: async (): Promise<MidiLink> => { opens++; throw new Error("B is off"); } };
  assert.equal(await m.choose(B), "failed");
  assert.equal(m.state.key, "ble:B");
  m.retry();
  assert.equal(await m.settled(), "failed");
  assert.equal(opens, 2, "Try again opens B");
  assert.equal(await m.choose(B), "failed");
  assert.equal(opens, 3, "picking B again opens B");
  assert.equal(made.length, 1, "no second session on A's link");
  m.disconnect();
});

const { connectBleDevice } = await import("../src/transport/ble.ts");
const { encodeBlePackets } = await import("../src/protocol/blemidi.ts");
type GattChar = import("../src/transport/ble.ts").GattChar;

test("BLE: an open that finishes after a newer open of the same piano does not deafen the newer link", async () => {
  // desktop auto-reconnect: a gatt.connect() hangs, the watchdog gives up, the next attempt opens; then the hung one
  // comes back. Before, it built a second link on the characteristic, which took the notifications from the link the
  // session runs on — the session heard nothing more.
  const listeners = new Set<(e: Event) => void>();
  const ch: GattChar = {
    value: undefined,
    startNotifications: async () => undefined,
    writeValueWithoutResponse: async () => undefined,
    addEventListener: (_t, cb) => void listeners.add(cb),
    removeEventListener: (_t, cb) => void listeners.delete(cb),
  };
  const server = { getPrimaryService: async () => ({ getCharacteristic: async () => ch }), disconnect: () => {} };
  const pending: (() => void)[] = [];
  const dev = {
    id: "race1",
    name: "CA79",
    gatt: { connect: () => new Promise<typeof server>((r) => pending.push(() => r(server))) },
    addEventListener: () => {},
    removeEventListener: () => {},
  };
  const stale = connectBleDevice(dev);
  const fresh = connectBleDevice(dev);
  await tick(1);
  pending[1]!(); // the newer attempt opens first
  const live = await fresh;
  const got: string[] = [];
  live.onMessage((m) => got.push(toHex(m)));
  pending[0]!(); // then the hung one comes back
  await stale;
  for (const p of encodeBlePackets([Uint8Array.from([0x90, 0x3c, 0x40])], 0, 23)) {
    ch.value = new DataView(p.buffer, p.byteOffset, p.byteLength);
    for (const l of [...listeners]) l(new Event("characteristicvaluechanged"));
  }
  assert.deepEqual(got, ["90 3C 40"]);
  live.close();
});
