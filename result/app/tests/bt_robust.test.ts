// Connect / dump robustness against a lossy, slow BLE link.
import { test } from "node:test";
import assert from "node:assert/strict";

import { DisplacedError, PianoSession } from "../src/protocol/session.ts";
import { decodeFrame, encodeFrame, toHex } from "../src/protocol/sysex.ts";
import { LoopbackLink } from "../src/transport/link.ts";
import { GattWriter } from "../src/transport/ble.ts";

const CONNECT = "F0 40 7F 7E 08 02 01 01 F7";
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/**
 * Stock-like piano: the dump is stop-and-wait (next fn 0x10 only after the controller's ACK),
 * like the phone capture (235 parameters, one per round trip).
 */
function piano(
  link: LoopbackLink,
  o: { dropConnect?: number; dropDumpRequest?: number; params?: number; rttMs?: number; stallAfter?: number; displaceOnConnect?: boolean } = {},
) {
  let dropConnect = o.dropConnect ?? 0;
  let dropDump = o.dropDumpRequest ?? 0;
  const n = o.params ?? 20;
  const seen: string[] = [];
  let next = 0;
  const tx = (fn: number, body: number[], dev = 0x00) => link.send(encodeFrame({ dev, fn, group: 0x04, machine: 0x1e, body }));
  const sendParam = () => {
    if (o.stallAfter !== undefined && next >= o.stallAfter) return;
    if (next < n) tx(0x10, [0x50, next & 0x7f, 0x00, 0x00]);
    else tx(0x03, [0x5f, 0x7f, 0x7f]);
  };
  link.onMessage((m) => {
    const hex = toHex(m);
    seen.push(hex);
    const f = decodeFrame(m)!;
    if (hex === CONNECT) {
      if (o.displaceOnConnect) return tx(0x7e, [0x01, 0x00]);
      if (dropConnect > 0) return void dropConnect--;
      return tx(0x7e, [0x02, 0x00, 0x40]);
    }
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) tx(0x7e, [0x05, 0x00, 0x00, 0x02]);
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x41) tx(0x7e, [0x05, 0x01, 0x00, 0x01]);
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x42) tx(0x7e, [0x05, 0x02, 0x00]);
    if (f.fn === 0x7e && f.body[0] === 0x04 && f.body[1] === 0x41) tx(0x7e, [0x04, 0x01, 0x32]);
    if (f.fn === 0x02 && f.body[0] === 0x5f) {
      if (dropDump > 0) return void dropDump--;
      next = 0;
      sendParam();
    }
    if (f.fn === 0x07 && f.body[0] === 0x50) {
      next++;
      setTimeout(sendParam, o.rttMs ?? 0);
    }
  });
  return seen;
}

test("first CONNECT lost (fresh link / piano boot): connect re-sends it and succeeds", async () => {
  const [app, p] = LoopbackLink.pair();
  const seen = piano(p, { dropConnect: 1 });
  const s = new PianoSession(app);
  const id = await s.connect(300);
  assert.equal(id.model, "CA79");
  assert.equal(seen.filter((x) => x === CONNECT).length, 2);
  assert.equal(s.stats.connectRetries, 1);
  assert.equal(s.state, "ready");
});

test("no answer at all: fails after connectTries CONNECTs with the old message", async () => {
  const [app, p] = LoopbackLink.pair();
  const seen = piano(p, { dropConnect: 99 });
  const s = new PianoSession(app, { connectTries: 3 });
  await assert.rejects(s.connect(300), /timeout waiting for piano reply \(3 tries\)/);
  assert.equal(seen.filter((x) => x === CONNECT).length, 3);
});

test("displaced while connecting: stops re-sending at once", async () => {
  const [app, p] = LoopbackLink.pair();
  const seen = piano(p, { displaceOnConnect: true });
  const s = new PianoSession(app);
  await assert.rejects(s.connect(300), DisplacedError);
  await sleep(400);
  assert.equal(seen.filter((x) => x === CONNECT).length, 1);
});

test("slow stop-and-wait dump longer than the old fixed deadline completes", async () => {
  const [app, p] = LoopbackLink.pair();
  // 60 params × 15 ms ≈ 0.9 s, the old deadline was connect(150) × 4 = 0.6 s
  piano(p, { params: 60, rttMs: 15 });
  const s = new PianoSession(app);
  let got = 0;
  s.onParam(() => got++);
  const t0 = Date.now();
  await s.connect(150);
  assert.ok(Date.now() - t0 > 600, "the dump really took longer than the old deadline");
  assert.equal(got, 60);
  assert.ok(s.stats.dumpFrames >= 61);
});

test("lost AllParameter request is sent once more", async () => {
  const [app, p] = LoopbackLink.pair();
  const seen = piano(p, { dropDumpRequest: 1, params: 5 });
  const s = new PianoSession(app);
  await s.connect(100);
  assert.equal(seen.filter((x) => x.includes(" 02 08 02 5F 7F 7F ")).length, 2);
});

test("a dump that stalls still times out (idle window), with progress in the message", async () => {
  const [app, p] = LoopbackLink.pair();
  piano(p, { params: 10, stallAfter: 4 });
  const s = new PianoSession(app);
  await assert.rejects(s.connect(100), /parameter dump timed out \(\d+ frames, idle 200 ms\)/);
});

test("GattWriter: first packets go out back to back, the rest are paced below the UART rate", async () => {
  let t = 0;
  const times: number[] = [];
  const w = new GattWriter(
    async () => {
      times.push(t);
    },
    { now: () => t, sleep: async (ms) => void (t += ms), bytesPerSec: 2500, burstBytes: 160 },
  );
  const pk = Array.from({ length: 20 }, () => new Uint8Array(20));
  assert.equal(await w.send(pk), true);
  assert.deepEqual(times.slice(0, 8), [0, 0, 0, 0, 0, 0, 0, 0]);
  // after the burst: 20 B per 8 ms (2500 B/s)
  assert.equal(times[8], 8);
  assert.equal(times[19], 96);
  const avg = (400 - 160) / (times[19] / 1000);
  assert.ok(avg <= 2500 + 1, `paced rate ${avg} B/s`);
});

test("GattWriter: a late timer (hidden-page clamping) never lets more than burst + rate × elapsed out", async () => {
  // field 2026-11-03: the desktop app minimized → Chromium clamped timers to ~1 s and an OTA upload took ~7 min.
  // The pacing refills from the clock, not from the requested sleep: a late wake-up only slows it down; it never
  // sends a bigger burst than the module's buffer takes. (The desktop shell turns the clamping off: main.cjs.)
  let t = 0;
  const log: [number, number][] = [];
  let sleeps = 0;
  const w = new GattWriter(
    async (p) => {
      log.push([t, p.length]);
    },
    { now: () => t, sleep: async (ms) => { sleeps++; t += Math.max(ms, 1000); }, bytesPerSec: 2500, burstBytes: 160 },
  );
  const pk = Array.from({ length: 100 }, () => new Uint8Array(20));
  assert.equal(await w.send(pk), true);
  let sent = 0;
  for (const [at, n] of log) {
    sent += n;
    assert.ok(sent <= 160 + (2500 * at) / 1000, `${sent} B by ${at} ms`);
  }
  // each late wake-up refills only up to the burst: 8 packets per second instead of 125
  assert.equal(sleeps, Math.ceil((2000 - 160) / 160));
});

test("GattWriter: a rejected write is retried; a dead write drops the rest of that message only", async () => {
  let fail = 1;
  const out: number[] = [];
  const w = new GattWriter(
    async (p) => {
      if (fail > 0) {
        fail--;
        throw new Error("GATT operation failed");
      }
      out.push(p[0]);
    },
    { sleep: async () => {} },
  );
  assert.equal(await w.send([Uint8Array.of(1), Uint8Array.of(2)]), true);
  assert.deepEqual(out, [1, 2]);
  assert.equal(w.stats.retried, 1);

  fail = 99;
  assert.equal(await w.send([Uint8Array.of(3), Uint8Array.of(4), Uint8Array.of(5)]), false);
  assert.equal(w.stats.dropped, 3);
  fail = 0;
  assert.equal(await w.send([Uint8Array.of(6)]), true, "the next message goes out normally");
  assert.deepEqual(out, [1, 2, 6]);
});
