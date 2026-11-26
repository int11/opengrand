// Parameter mirror: a set the piano did not take rolls back only to a value the piano holds (field class 2026-11-27:
// fast repeated edits left the piano and the screen out of step). A slider drag queues several sets of one value.
import { test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { PianoSession } = await import("../src/protocol/session.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const { findParam } = await import("../src/protocol/codecs.ts");
const { decodeFrame, encodeFrame } = await import("../src/protocol/sysex.ts");

const vol = findParam("MetronomeVolume")!;

function pair(ack: (n: number) => boolean, opt: { ackTimeoutMs?: number; maxRetries?: number } = {}) {
  const [app, piano] = LoopbackLink.pair();
  let n = 0;
  piano.onMessage((m) => {
    const f = decodeFrame(m);
    if (f && f.fn === 0x10 && ack(n++)) piano.send(encodeFrame({ dev: 0, fn: 0x07, group: 4, machine: 0x1e, body: f.body.slice(0, 3) }));
  });
  const session = new PianoSession(app, { ackTimeoutMs: opt.ackTimeoutMs ?? 1000, maxRetries: opt.maxRetries });
  const store = new ParamStore();
  store.attach(session);
  store.fromWire({ fn: 0x10, v1: vol.v1, v2: vol.v2, part: 0x7f, data: [5] });
  return { store, piano };
}

test("store: a drag the piano took none of shows the piano's value again, not a value from the drag", async () => {
  const { store } = pair(() => false, { ackTimeoutMs: 30, maxRetries: 1 });
  const sets = [6, 7, 8].map((v) => store.set(vol, 0x7f, [v]));
  assert.deepEqual(await Promise.all(sets), [false, false, false]);
  assert.deepEqual(store.get(vol, 0x7f), [5], "the piano never took 6, 7 or 8");
});

test("store: an early set that failed does not roll back over a later one the piano took", async () => {
  // the first set is never ACKed (given up after one send), the next two are
  const { store } = pair((n) => n > 0, { ackTimeoutMs: 30, maxRetries: 1 });
  const sets = [6, 7, 8].map((v) => store.set(vol, 0x7f, [v]));
  assert.deepEqual(await Promise.all(sets), [false, true, true]);
  assert.deepEqual(store.get(vol, 0x7f), [8], "the piano holds 8");
});
