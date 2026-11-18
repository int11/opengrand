// FileAccess client against the STOCK V2.19 fn 0x24 behaviour, replayed byte for byte from
// scripts/analysis/stock_fn24.py (the stock code run under Unicorn):
//   - every request (04-0B, 11) is acknowledged with 24 7F first (0x4014A96C / 98C / 9B0);
//   - FileName / TopOfData / DataPacket / EndOfData / End-of-list are stop-and-wait
//     (FUN_4014B638): an unacknowledged packet is resent 16 times, then the stock drops the
//     controller binding (FUN_4014B55C) and ignores it from then on;
//   - list names are full paths from the root ("\DIR\INNER.MID");
//   - success of an operation is 32 51 7E 7F 00 00 (FUN_4014F294), errors 7E 03 00 hi lo;
//   - 32 51 3A / 3B (rename / delete by id) get no reply at all.
import { test } from "node:test";
import assert from "node:assert/strict";

import { FileAccessClient, FileAccessError, namePackets, packet } from "../src/protocol/fileaccess.ts";
import { PianoSession } from "../src/protocol/session.ts";
import { fromHex, toHex } from "../src/protocol/sysex.ts";
import { LoopbackLink } from "../src/transport/link.ts";

const ACK = fromHex("F0 40 7F 24 04 1E 7F F7");
const END_LIST = fromHex("F0 40 7F 24 04 1E 1F F7");
const OK_STOCK = fromHex("F0 40 7F 32 04 1E 51 7E 7F 00 00 F7");
const NOT_FOUND = fromHex("F0 40 7F 7E 04 1E 03 00 00 56 F7");
/** stock_fn24.py scenario 5: the file task walking \SONG1.MID, \A_LONG…MID, \.HIDDEN.MID (skipped), \DIR\INNER.MID */
const STOCK_NAMES = [
  "F0 40 7F 24 04 1E 03 00 00 5C 53 4F 4E 47 31 2E 00 4D 49 44 00 14 F7",
  "F0 40 7F 24 04 1E 03 00 00 5C 41 5F 4C 4F 4E 47 00 5F 46 49 4C 45 5F 4E 00 41 4D 45 5F 46 4F 52 4F F7",
  "F0 40 7F 24 04 1E 03 01 00 5F 54 57 4F 5F 50 41 00 43 4B 45 54 53 2E 4D 00 49 44 00 7A F7",
  "F0 40 7F 24 04 1E 03 00 00 5C 44 49 52 5C 49 4E 00 4E 45 52 2E 4D 49 44 00 00 6D F7",
].map(fromHex);
const RETRIES = 16;

/** A stock piano reduced to its fn 0x24 behaviour. `retry` delivers a resend of the packet
 *  in flight (what the stock does when our Ack is late). */
function stockPiano(opts: { lateAckOnce?: boolean } = {}) {
  const [app, piano] = LoopbackLink.pair();
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  const rx: string[] = [];
  let bound = true;
  let inflight: { msg: Uint8Array; tries: number; then: () => void } | null = null;
  let lateOnce = opts.lateAckOnce ?? false;
  let op = 0;
  let names: number[][] = [];
  let cur: number[] = [];
  let onResult: (() => Uint8Array) | null = null;

  // stop-and-wait sender: resend on a missing Ack (driven by `timeout()`), unbind after 16 tries
  const sendWait = (msg: Uint8Array, then: () => void) => {
    inflight = { msg, tries: 1, then };
    piano.send(msg);
  };
  const timeout = () => {
    if (!inflight) return;
    if (inflight.tries >= RETRIES) {
      inflight = null;
      bound = false; // FUN_4014B55C
      return;
    }
    inflight.tries++;
    piano.send(inflight.msg);
  };
  const stream = (msgs: Uint8Array[], done?: () => void) => {
    const next = (i: number) => {
      if (i >= msgs.length) return done?.();
      sendWait(msgs[i], () => next(i + 1));
    };
    next(0);
  };

  piano.onMessage((m) => {
    rx.push(toHex(m));
    if (!bound || m[1] !== 0x40 || m[4] !== 0x08) return; // unbound: silence (FUN_40151BCC)
    if (m[3] === 0x10) return void piano.send(Uint8Array.from([0xf0, 0x40, 0x7f, 0x07, 0x04, 0x1e, m[6], m[7], m[8], 0xf7]));
    if (m[3] !== 0x24) return;
    const d0 = m[6];
    if (d0 === 0x7f) {
      if (lateOnce && inflight) {
        lateOnce = false; // this Ack is "late": the stock has already resent the packet
        piano.send(inflight.msg);
      }
      const f = inflight;
      inflight = null;
      f?.then();
      return;
    }
    if (d0 === 0x11) {
      piano.send(ACK);
      stream([...STOCK_NAMES, END_LIST]);
      return;
    }
    if (d0 >= 0x04 && d0 <= 0x0b) {
      piano.send(ACK);
      op = d0;
      names = [];
      cur = [];
      return;
    }
    if (d0 === 0x03) {
      piano.send(ACK);
      const raw = Array.from(m);
      const groups = raw.slice(8, raw.length - 2);
      for (let i = 0; i < groups.length; ) {
        const hdr = groups[i++];
        for (let k = 0; k < 7 && i < groups.length; k++, i++) {
          const b = groups[i] | ((hdr << (k + 1)) & 0x80);
          if (b === 0) {
            names.push(cur);
            cur = [];
          } else cur.push(b);
        }
      }
      const need = op === 0x07 ? 2 : 1;
      if (names.length === need && onResult) piano.send(onResult());
      if (names.length === need && op === 0x04) {
        // read: TopOfData (5 bytes - the stock sends the file size), the data, EndOfData - each waiting for an Ack
        const data = packet(0x02, 0, [1, 2, 3, 0x80, 0xff], 0x7f);
        data[4] = 0x04;
        data[5] = 0x1e;
        data[data.length - 2] = 0;
        let x = 0;
        for (let i = 1; i < data.length - 2; i++) x ^= data[i];
        data[data.length - 2] = x & 0x7f;
        stream([fromHex("F0 40 7F 24 04 1E 01 00 00 00 05 F7"), data, fromHex("F0 40 7F 24 04 1E 7B F7")], () => piano.send(OK_STOCK));
      }
    }
  });
  return {
    session,
    rx,
    timeout,
    get bound() {
      return bound;
    },
    get inflight() {
      return inflight;
    },
    get names() {
      return names.map((n) => String.fromCharCode(...n));
    },
    result(fn: () => Uint8Array) {
      onResult = fn;
    },
  };
}

const flush = () => new Promise((r) => setTimeout(r, 5));

test("stock list: full paths, End of list acked, binding kept, a second list works", async () => {
  const p = stockPiano();
  const fa = new FileAccessClient(p.session, 300);
  const names = await fa.list();
  assert.deepEqual(names, ["SONG1.MID", "A_LONG_FILE_NAME_FOR_TWO_PACKETS.MID", "DIR\\INNER.MID"]);
  await flush();
  assert.equal(p.rx[0], "F0 40 7F 24 08 02 11 F7", "the request is the one the stock handles at 0x4014A96C");
  assert.equal(p.rx.filter((m) => m === "F0 40 7F 24 08 02 7F F7").length, 5, "4 FileName packets + 24 1F acked");
  assert.equal(p.inflight, null, "nothing left waiting for an Ack (24 1F was acked)");
  for (let i = 0; i < RETRIES; i++) p.timeout(); // nothing in flight -> the stock keeps the binding
  assert.equal(p.bound, true);
  assert.deepEqual(await fa.list(), names, "second list answered");
});

test("stock list: the field symptom — 24 1F not acked -> 16 resends -> unbound -> 'timeout waiting for file list'", async () => {
  const p = stockPiano();
  // the old client: ack the names but not 24 1F
  const off = p.session.onFrame((f) => {
    if (f.fn === 0x24 && f.body[0] === 0x03) p.session.sendRaw(fromHex("F0 40 7F 24 08 02 7F F7"));
  });
  p.session.sendRaw(fromHex("F0 40 7F 24 08 02 11 F7"));
  await flush();
  off();
  assert.equal(toHex(p.inflight!.msg), toHex(END_LIST), "the stock is waiting for the Ack of 24 1F");
  for (let i = 0; i < RETRIES; i++) p.timeout();
  assert.equal(p.bound, false, "after 16 tries the stock drops the controller binding");
  await assert.rejects(new FileAccessClient(p.session, 100).list(), /timeout waiting for file list/);
});

test("stock list: a packet resent because our Ack was late is kept once", async () => {
  const p = stockPiano({ lateAckOnce: true });
  const fa = new FileAccessClient(p.session, 300);
  assert.deepEqual(await fa.list(), ["SONG1.MID", "A_LONG_FILE_NAME_FOR_TWO_PACKETS.MID", "DIR\\INNER.MID"]);
});

test("stock delete / rename by name: request acked, names as the stock buffers them, 32 51 7E success", async () => {
  const p = stockPiano();
  const fa = new FileAccessClient(p.session, 300);
  p.result(() => OK_STOCK);
  assert.equal(await fa.delete("DIR\\INNER.MID"), 0);
  assert.deepEqual(p.names, ["\\DIR\\INNER.MID"], "FUN_400A94BC gets the full path");
  assert.equal(p.rx[0], "F0 40 7F 24 08 02 06 F7");
  assert.deepEqual(p.rx.slice(1), namePackets("DIR\\INNER.MID", 0x7f).map(toHex));

  p.rx.length = 0;
  assert.equal(await fa.rename("OLD.MID", "NEW_NAME.MID"), 0);
  assert.deepEqual(p.names, ["\\OLD.MID", "\\NEW_NAME.MID"], "old then new (FUN_400A89BC(old, new))");

  p.result(() => NOT_FOUND);
  await assert.rejects(fa.delete("NOPE.MID"), (e: unknown) => e instanceof FileAccessError && e.code === 0x56);
});

test("stock read: TopOfData and EndOfData are acked, the data arrives after the TopOfData Ack", async () => {
  const p = stockPiano();
  const fa = new FileAccessClient(p.session, 300);
  const data = await fa.read("SONG1.MID");
  assert.deepEqual([...data], [1, 2, 3, 0x80, 0xff]);
  await flush();
  assert.equal(p.inflight, null, "TopOfData, the packet and 24 7B all acked");
  assert.equal(p.bound, true);
});

test("stock: rename / delete by id get no reply (OpenGrand only)", async () => {
  const p = stockPiano();
  const fa = new FileAccessClient(p.session, 150);
  await assert.rejects(fa.deleteById(0), /timeout waiting for result/);
});
