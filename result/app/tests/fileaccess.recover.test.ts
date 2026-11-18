// FileAccessClient after a fn 0x24 timeout, and reads longer than 128 DataPackets.
//   - The stock resends an unacknowledged fn 0x24 packet 16 times, then drops the controller and stays
//     silent. Field 2026-11-16: session.takeControl()
//     (CONNECT + re-sync) brings the file channel back without a BLE reconnect
// The client resets its waits, runs its `recover` hook
//     once (not again within recoverGapMs), rejects with FileTimeoutError, and the next operation works.
//   - DataPacket counters are 7 bit: after 7F comes 00 (the stock counter masked by the firmware fix).
//   - head(): Cancel then Ack. The stock sender only looks at the Cancel flag after an Ack (FUN_4014D974 0x4014D9A8);
//     a Cancel alone left the job resending, and the next request got Busy (field 2026-11-16). The job then ends with Result 0xF0.
//   - Damaged DataPackets (field 2026-11-16, 1 MB read: two frames run together with `F7 F0 40 7F` gone, resends damaged
//     too): NAKed until a good resend; a glued frame whose checksum matches by chance is refused by length / counter;
//     the silence allowed between packets (dataTimeoutMs) outlasts the piano's resend cadence (long-read retry).
import { test } from "node:test";
import assert from "node:assert/strict";

import { FileAccessClient, FileTimeoutError, packet } from "../src/protocol/fileaccess.ts";
import { PianoSession } from "../src/protocol/session.ts";
import { fromHex } from "../src/protocol/sysex.ts";
import { LoopbackLink } from "../src/transport/link.ts";

const ACK = fromHex("F0 40 7F 24 04 1E 7F F7");
const END_LIST = fromHex("F0 40 7F 24 04 1E 1F F7");
const END_DATA = fromHex("F0 40 7F 24 04 1E 7B F7");

/** A 02/03 packet as the piano sends it (04 1E header, counter & 0x7F). */
function pianoPacket(v1: number, counter: number, raw: number[]): Uint8Array {
  const p = packet(v1, counter, raw, 0x7f);
  p[4] = 0x04;
  p[5] = 0x1e;
  let x = 0;
  for (let i = 1; i < p.length - 2; i++) x ^= p[i];
  p[p.length - 2] = x & 0x7f;
  return p;
}

/** Stop-and-wait fn 0x24 piano: list -> one name, read -> TopOfData / DataPackets / 7B. `bound = false` = silent. */
function piano(file: Uint8Array) {
  const [app, end] = LoopbackLink.pair();
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  const st = {
    bound: true,
    stallAfter: -1, // read: unbind after sending this many DataPackets (the stock giving up on us)
    resendCounter: -1, // read: resend the DataPacket with this counter once, as if our Ack was late
    sent: 0,
    // read: DataPacket number -> sends of it that arrive damaged (each NAK brings a resend, ota_main 50a352be+ up to 64)
    damage: new Map<number, number>(),
    damageKind: "glue" as "glue" | "glueChk" | "flip",
    damaged: 0,
    naks: 0,
    nakDelayMs: 0, // the resend after a NAK comes this late (the piano's 5 ticks + BLE)
  };
  let queue: Uint8Array[] = [];
  let inflight: Uint8Array | null = null;
  let skipAck = false;
  let reading = false;
  let cancel = false;
  let dataIdx = -1;
  /** The field's damage: the packet runs into the next one with `F7 F0 40 7F` gone; glueChk = and the 7-bit checksum
   *  happens to match; flip = one data byte changed (checksum wrong). */
  const damaged = (a: Uint8Array, b: Uint8Array | undefined): Uint8Array => {
    if (st.damageKind === "flip" || !b) {
      const c = a.slice();
      c[9] ^= 0x01;
      return c;
    }
    const g = Uint8Array.from([...a.subarray(0, a.length - 1), ...b.subarray(3)]);
    if (st.damageKind === "glueChk") {
      let x = 0;
      for (let i = 1; i < g.length - 2; i++) x ^= g[i];
      g[g.length - 2] = x & 0x7f;
    }
    return g;
  };
  const emit = () => {
    if (!inflight) return;
    const left = inflight[6] === 0x02 ? (st.damage.get(dataIdx) ?? 0) : 0;
    if (left > 0) {
      st.damage.set(dataIdx, left - 1);
      st.damaged++;
      return void end.send(damaged(inflight, queue[0]));
    }
    end.send(inflight);
  };
  const NOT_ACCEPTED = fromHex("F0 40 7F 7E 04 1E 03 00 01 70 F7");
  const BUSY = fromHex("F0 40 7F 7E 04 1E 03 01 00 00 F7");
  const next = () => {
    inflight = queue.shift() ?? null;
    if (!inflight) return;
    if (reading && inflight[6] === 0x02) dataIdx = st.sent;
    if (reading && inflight[6] === 0x02 && st.sent++ === st.stallAfter) {
      st.bound = false;
      return;
    }
    emit();
  };
  end.onMessage((m) => {
    if (!st.bound || m[1] !== 0x40 || m[3] !== 0x24) return;
    const d0 = m[6];
    if (d0 === 0x7d) return void (cancel = true); // 0x413CA3E7: no answer, not an Ack
    if (d0 === 0x7e && inflight) {
      st.naks++; // the sender: 5 ticks, then the same packet again
      if (st.nakDelayMs) return void setTimeout(emit, st.nakDelayMs);
      return emit();
    }
    if (d0 === 0x7f && cancel && inflight && inflight[6] === 0x02) {
      cancel = false; // the read loop tests the flag after the Ack: -> Result 0xF0, job over
      queue = [];
      inflight = null;
      return void end.send(NOT_ACCEPTED);
    }
    if ((d0 === 0x11 || d0 === 0x03) && cancel) return void (end.send(ACK), end.send(BUSY)); // the cancelled job still resending
    if (d0 === 0x7f) {
      if (skipAck) return void (skipAck = false);
      if (inflight && inflight[6] === 0x02 && inflight[7] === st.resendCounter) {
        st.resendCounter = -1;
        end.send(inflight); // the resend crosses our Ack
        skipAck = true;
      }
      return next();
    }
    end.send(ACK);
    if (d0 === 0x11) {
      queue = [pianoPacket(0x03, 0, [0x5c, ...Array.from("A.MID", (c) => c.charCodeAt(0)), 0]), END_LIST];
      reading = false;
      next();
    } else if (d0 === 0x03) {
      const n = file.length;
      const top = Uint8Array.from([0xf0, 0x40, 0x7f, 0x24, 0x04, 0x1e, 0x01, (n >> 21) & 0x7f, (n >> 14) & 0x7f, (n >> 7) & 0x7f, n & 0x7f, 0xf7]);
      queue = [top];
      for (let i = 0, c = 0; i < n; i += 21, c++) queue.push(pianoPacket(0x02, c, Array.from(file.subarray(i, i + 21))));
      queue.push(END_DATA);
      reading = true;
      st.sent = 0;
      next();
    }
  });
  return { session, st };
}

test("read: more than 128 DataPackets, counter wraps 7F -> 00, all kept in order", async () => {
  const file = new Uint8Array(300 * 21 + 5).map((_, i) => (i * 7 + (i >> 8)) & 0xff);
  const p = piano(file);
  const got = await new FileAccessClient(p.session, 300).read("BIG.MID");
  assert.equal(got.length, file.length);
  assert.deepEqual(got, file);
});

test("read: identical packets that differ only by the wrapped counter are not taken for resends", async () => {
  const file = new Uint8Array(260 * 21); // all zero: packet k and k+128 are byte-identical
  const p = piano(file);
  assert.equal((await new FileAccessClient(p.session, 300).read("ZERO.MID")).length, file.length);
});

test("read: a resend of the 7F packet right at the wrap is kept once", async () => {
  const file = new Uint8Array(200 * 21).map((_, i) => i & 0xff);
  const p = piano(file);
  p.st.resendCounter = 0x7f;
  assert.deepEqual(await new FileAccessClient(p.session, 300).read("BIG.MID"), file);
  assert.equal(p.st.resendCounter, -1, "the resend happened");
});

test("head(): Cancel + Ack ends the job with Result 0xF0 at once; the next operation is not Busy", async () => {
  const file = new Uint8Array(200 * 21).map((_, i) => (i * 5) & 0xff);
  const p = piano(file);
  const fa = new FileAccessClient(p.session, 300);
  const t0 = Date.now();
  const h = await fa.head("BIG.MID", 50);
  assert.equal(h.length, file.length);
  assert.deepEqual(h.data, file.subarray(0, 63), "three packets (>= 50 B)");
  assert.ok(Date.now() - t0 < 250, "no wait for the timeout");
  assert.deepEqual(await fa.list(), ["A.MID"]);
});

test("timeout: the client resets, re-takes control once, rejects clearly, and the next operation works", async () => {
  const p = piano(new Uint8Array(0));
  p.st.bound = false; // the stock dropped us after its 16 resends
  let calls = 0;
  const fa = new FileAccessClient(p.session, 80, {
    recover: async () => {
      calls++;
      await new Promise((r) => setTimeout(r, 30)); // CONNECT + re-sync takes a while
      p.st.bound = true;
    },
  });
  await assert.rejects(fa.list(), (e: unknown) => e instanceof FileTimeoutError && e.recovering && /file list.*try again/.test(e.message));
  assert.equal(calls, 1);
  assert.deepEqual(await fa.list(), ["A.MID"], "the retry waits for the re-take and works");
  assert.equal(calls, 1);
});

test("timeout mid-read: pending waits are dropped, the retried read returns the whole file", async () => {
  const file = new Uint8Array(150 * 21).map((_, i) => (i * 3) & 0xff);
  const p = piano(file);
  p.st.stallAfter = 140;
  const fa = new FileAccessClient(p.session, 80, { recover: () => void (p.st.bound = true) });
  await assert.rejects(fa.read("BIG.MID"), (e: unknown) => e instanceof FileTimeoutError && e.recovering && /file data/.test(e.message));
  p.st.stallAfter = -1;
  assert.deepEqual(await fa.read("BIG.MID"), file);
  assert.equal(fa.recoveries, 1);
});

test("timeout: no second re-take within recoverGapMs (a dead link does not loop)", async () => {
  const p = piano(new Uint8Array(0));
  p.st.bound = false;
  let t = 0;
  let calls = 0;
  const fa = new FileAccessClient(p.session, 50, { recover: () => void calls++, recoverGapMs: 1000, now: () => t });
  await assert.rejects(fa.list(), (e: unknown) => e instanceof FileTimeoutError && e.recovering);
  t = 500;
  await assert.rejects(fa.list(), (e: unknown) => e instanceof FileTimeoutError && !e.recovering);
  assert.equal(calls, 1);
  t = 1500;
  await assert.rejects(fa.list(), (e: unknown) => e instanceof FileTimeoutError && e.recovering);
  assert.equal(calls, 2);
});

test("timeout without a recover hook: plain FileTimeoutError, message unchanged", async () => {
  const p = piano(new Uint8Array(0));
  p.st.bound = false;
  await assert.rejects(new FileAccessClient(p.session, 50).list(), (e: unknown) => e instanceof FileTimeoutError && e.message === "timeout waiting for file list");
});

test("read: packets damaged by BLE (run together, F7 F0 40 7F gone) are NAKed; the resends, damaged several times, then good", async () => {
  const file = new Uint8Array(200 * 21 + 9).map((_, i) => (i * 11 + (i >> 5)) & 0xff);
  const p = piano(file);
  p.st.damage = new Map([[3, 1], [127, 5], [128, 30], [200, 2]]); // 200 = the short last packet, glued to 7B
  assert.deepEqual(await new FileAccessClient(p.session, 300).read("BIG.MID"), file);
  assert.equal(p.st.damaged, 38);
  assert.equal(p.st.naks, 38, "one NAK per damaged frame, nothing else NAKed");
});

test("read: a glued frame whose 7-bit checksum happens to match is still NAKed (length / counter), never taken", async () => {
  const file = new Uint8Array(140 * 21).map((_, i) => (i * 13) & 0xff);
  const p = piano(file);
  p.st.damageKind = "glueChk";
  p.st.damage = new Map([[10, 3], [129, 1]]);
  assert.deepEqual(await new FileAccessClient(p.session, 300).read("BIG.MID"), file);
  assert.equal(p.st.naks, 4);
});

test("read: a corrupted data byte (bad checksum) resent damaged 20 times, then good", async () => {
  const file = new Uint8Array(50 * 21 + 3).map((_, i) => (i * 3 + 1) & 0xff);
  const p = piano(file);
  p.st.damageKind = "flip";
  p.st.damage = new Map([[7, 20]]);
  assert.deepEqual(await new FileAccessClient(p.session, 300).read("F.MID"), file);
  assert.equal(p.st.naks, 20);
});

test("read: a resend that crosses our Ack and a damaged frame right after it do not shift the data", async () => {
  const file = new Uint8Array(300 * 21 + 1).map((_, i) => (i * 7 + 3) & 0xff);
  const p = piano(file);
  p.st.resendCounter = 0x05;
  p.st.damage = new Map([[6, 2], [133, 1]]);
  assert.deepEqual(await new FileAccessClient(p.session, 300).read("BIG.MID"), file);
});

test("read: the silence allowed between DataPackets outlasts the timeout (the piano's resend cadence), default 1.5 x", async () => {
  const file = new Uint8Array(20 * 21).map((_, i) => i & 0xff);
  const p = piano(file);
  p.st.damage = new Map([[4, 2]]);
  p.st.nakDelayMs = 120; // > timeoutMs 100, < dataTimeoutMs 150
  assert.deepEqual(await new FileAccessClient(p.session, 100).read("F.MID"), file);
  const q = piano(file);
  q.st.damage = new Map([[4, 1]]);
  q.st.nakDelayMs = 120;
  await assert.rejects(new FileAccessClient(q.session, 100, { dataTimeoutMs: 100 }).read("F.MID"), (e: unknown) => e instanceof FileTimeoutError && /file data/.test(e.message));
});
