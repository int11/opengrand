import { test } from "node:test";
import assert from "node:assert/strict";

import { PianoSession } from "../src/protocol/session.ts";
import { decodeFrame, encodeFrame, toHex } from "../src/protocol/sysex.ts";
import { LoopbackLink } from "../src/transport/link.ts";

/** A tiny fake piano speaking the observed protocol. */
function fakePiano(link: LoopbackLink, opts: { dropAcks?: number } = {}) {
  let drops = opts.dropAcks ?? 0;
  const tx = (fn: number, body: number[], dev = 0x00) => link.send(encodeFrame({ dev, fn, group: 0x04, machine: 0x1e, body }));
  const seen: string[] = [];
  link.onMessage((m) => {
    seen.push(toHex(m));
    const f = decodeFrame(m)!;
    if (f.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x01) tx(0x7e, [0x02, 0x00, 0x00]);
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) tx(0x7e, [0x05, 0x00, 0x00, 0x02]);
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x41) tx(0x7e, [0x05, 0x01, 0x00, 0x01]);
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x42) tx(0x7e, [0x05, 0x02, 0x00]);
    if (f.fn === 0x7e && f.body[0] === 0x04 && f.body[1] === 0x41) tx(0x7e, [0x04, 0x01, ..."2.19".split("").map((c) => c.charCodeAt(0))]);
    if (f.fn === 0x02 && f.body[0] === 0x5f) {
      tx(0x10, [0x55, 0x00, 0x00, 95, 112, 0]); // Sound = SK-EX Rendering
      tx(0x10, [0x56, 0x01, 0x0f, 0, 120]); // MetronomeTempo
      tx(0x03, [0x5f, 0x7f, 0x7f]);
    }
    if (f.fn === 0x10) {
      if (drops > 0) {
        drops--;
        return;
      }
      tx(0x07, [f.body[0], f.body[1], f.body[2]]);
    }
  });
  return seen;
}

test("connect: sync, identify CA79, receive full dump", async () => {
  const [app, piano] = LoopbackLink.pair();
  const seen = fakePiano(piano);
  const s = new PianoSession(app);
  const params: number[][] = [];
  s.onParam((p) => params.push([p.v1, p.v2, p.part, ...p.data]));
  const id = await s.connect(500);
  assert.equal(id.model, "CA79");
  assert.equal(id.version, "2.19");
  assert.equal(s.state, "ready");
  assert.equal(seen[0], "F0 40 7F 7E 08 02 01 01 F7");
  assert.ok(seen.includes("F0 40 00 02 08 02 5F 7F 7F F7"));
  assert.deepEqual(params[0], [0x55, 0x00, 0x00, 95, 112, 0]);
  assert.equal(params.length, 2);
  // every parameter received from the piano is acknowledged
  assert.ok(seen.includes("F0 40 00 07 08 02 55 00 00 F7"));
  // and so is the end of the dump: stock resends it and unbinds us after 16 tries
  assert.ok(seen.includes("F0 40 00 07 08 02 5F 7F 7F F7"));
});

test("fn 0x31/0x32 are not queued behind ACKs", async () => {
  const [app, piano] = LoopbackLink.pair();
  fakePiano(piano, { dropAcks: 100 });
  const s = new PianoSession(app, { ackTimeoutMs: 1000 });
  await s.connect(500);
  const t0 = Date.now();
  assert.equal(await s.sendParam({ fn: 0x32, v1: 0x56, v2: 0x08, part: 0x7f, data: [1] }), true);
  assert.equal(await s.sendParam({ fn: 0x31, v1: 0x53, v2: 0x00, part: 0x7f, data: [1] }), true);
  assert.ok(Date.now() - t0 < 100);
});

test("parameter set waits for ACK and retries lost ACKs", async () => {
  const [app, piano] = LoopbackLink.pair();
  fakePiano(piano, { dropAcks: 2 });
  const s = new PianoSession(app, { ackTimeoutMs: 20 });
  await s.connect(500);
  const ok = await s.sendParam({ v1: 0x55, v2: 0x08, part: 0x00, data: [3] });
  assert.equal(ok, true);
  assert.equal(s.stats.retried, 2);
  const ok2 = await s.sendParam({ v1: 0x55, v2: 0x07, part: 0x00, data: [1] });
  assert.equal(ok2, true);
});

test("gives up after maxRetries", async () => {
  const [app, piano] = LoopbackLink.pair();
  fakePiano(piano, { dropAcks: 100 });
  const s = new PianoSession(app, { ackTimeoutMs: 5, maxRetries: 3 });
  await s.connect(500);
  assert.equal(await s.sendParam({ v1: 0x50, v2: 0x00, part: 0, data: [1] }), false);
  assert.equal(s.stats.failed, 1);
});
