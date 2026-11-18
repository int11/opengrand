import { test } from "node:test";
import assert from "node:assert/strict";

import { BleMidiDecoder, BleModule, encodeBlePackets } from "../src/protocol/blemidi.ts";
import { appFrame, decodeFrame, fromHex, toHex } from "../src/protocol/sysex.ts";

function roundTrip(msgs: Uint8Array[], mtu: number): Uint8Array[] {
  const out: Uint8Array[] = [];
  const dec = new BleMidiDecoder((m) => out.push(m));
  for (const p of encodeBlePackets(msgs, 1234, mtu)) {
    assert.ok(p.length <= mtu - 3, `packet ${p.length} exceeds mtu`);
    dec.feed(p);
  }
  return out;
}

test("channel messages round trip in one packet", () => {
  const msgs = [Uint8Array.of(0x90, 60, 100), Uint8Array.of(0xb0, 64, 127), Uint8Array.of(0xc0, 3)];
  assert.deepEqual(roundTrip(msgs, 64), msgs);
});

test("long Kawai SysEx is split across packets and reassembled (MTU 23)", () => {
  const body = Array.from({ length: 60 }, (_, i) => i & 0x7f);
  const sx = appFrame(0x10, body);
  const out = roundTrip([sx, Uint8Array.of(0x80, 60, 0)], 23);
  assert.equal(out.length, 2);
  assert.deepEqual(out[0], sx);
  assert.deepEqual(out[1], Uint8Array.of(0x80, 60, 0));
});

test("decoder handles running status without repeated status byte", () => {
  const out: Uint8Array[] = [];
  const dec = new BleMidiDecoder((m) => out.push(m));
  // header, ts, 90 3C 64, ts, 3E 50 (running status), 40 30 (running status, no ts)
  dec.feed([0x80, 0x81, 0x90, 0x3c, 0x64, 0x82, 0x3e, 0x50, 0x40, 0x30]);
  assert.deepEqual(out.map((m) => Array.from(m)), [
    [0x90, 0x3c, 0x64],
    [0x90, 0x3e, 0x50],
    [0x90, 0x40, 0x30],
  ]);
});

test("timestamps decode as 13-bit ms", () => {
  const ts: number[] = [];
  const dec = new BleMidiDecoder((_m, t) => ts.push(t));
  for (const p of encodeBlePackets([Uint8Array.of(0xf8)], 0x1abc, 23)) dec.feed(p);
  assert.deepEqual(ts, [0x1abc]);
});

test("module rename SysEx matches the observed app bytes", () => {
  const msg = BleModule.setName("CA79");
  assert.equal(toHex(msg.subarray(0, 8)), "F0 00 02 08 10 55 00 01");
  assert.equal(msg.length, 8 + 16 + 1);
  assert.equal(toHex(BleModule.enterDfu()), "F0 00 02 08 10 55 7F 00 01 F7");
  const reply = fromHex("F0 00 02 08 10 55 20 04 31 2E 32 F7");
  assert.equal(BleModule.parseFirmwareReply(reply), "1.2");
});

test("Kawai frame encode/decode", () => {
  const sync = appFrame(0x7e, [0x01, 0x01]);
  assert.equal(toHex(sync), "F0 40 7F 7E 08 02 01 01 F7");
  const f = decodeFrame(fromHex("F0 40 00 10 04 1E 55 00 00 5F 70 00 F7"));
  assert.ok(f);
  assert.equal(f!.group, 0x04);
  assert.equal(f!.machine, 0x1e);
  assert.deepEqual(f!.body, [0x55, 0x00, 0x00, 0x5f, 0x70, 0x00]);
  assert.throws(() => appFrame(0x10, [0x80]));
});

test("SysEx data after a timestamp or a real-time byte stays in the SysEx (no running-status note-offs)", () => {
  const out: number[][] = [];
  const dec = new BleMidiDecoder((m) => out.push(Array.from(m)));
  dec.feed([0x80, 0x81, 0x80, 0x10, 0x40]); // note-off: leaves running status 0x80
  // SysEx split; the continuation packet starts with a timestamp byte (non-spec), then an F8 inside it
  dec.feed([0x80, 0x82, 0xf0, 0x40, 0x7f, 0x24, 0x04, 0x1e, 0x02]);
  dec.feed([0x80, 0x83, 0x00, 0x10, 0x41, 0x84, 0xf8, 0x50, 0x01, 0x85, 0xf7]);
  assert.deepEqual(out, [
    [0x80, 0x10, 0x40],
    [0xf8],
    [0xf0, 0x40, 0x7f, 0x24, 0x04, 0x1e, 0x02, 0x00, 0x10, 0x41, 0x50, 0x01, 0xf7],
  ]);
});

// Field 2026-11-16 (1 MB read, long-read retry): two DataPackets arrived run
// together, `… 6b 24 04 1E 02 6C …` with exactly `F7 F0 40 7F` gone. Is the decoder losing them? No: wherever the module
// cuts its notifications (header + bytes, a cut may fall anywhere, timestamps before every status byte), both frames come
// out whole; the field pattern is what one lost notification holding `ts F7 ts F0 40 7F` gives.
test("two piano SysEx frames survive every notification cut; one lost notification gives the field's glued frame", () => {
  const a = fromHex("F0 40 7F 24 04 1E 02 6A 00 01 02 03 04 05 06 07 00 08 09 0A 0B 0C 0D 0E 00 0F 10 11 12 13 14 15 6B F7");
  const b = fromHex("F0 40 7F 24 04 1E 02 6C 00 16 17 18 19 1A 1B 1C 00 1D 1E 1F 20 21 22 23 00 24 25 26 27 28 29 2A 55 F7");
  const ts = 0x85;
  const stream = (m: Uint8Array) => [ts, ...m.subarray(0, m.length - 1), ts, 0xf7];
  const bytes = [...stream(a), ...stream(b)];
  const decode = (cuts: number[]): Uint8Array[] => {
    const out: Uint8Array[] = [];
    const dec = new BleMidiDecoder((m) => out.push(m));
    let at = 0;
    for (const c of [...cuts, bytes.length]) {
      if (c > at) dec.feed([0x80, ...bytes.slice(at, c)]);
      at = c;
    }
    return out;
  };
  // a timestamp stays with its status byte (BLE-MIDI 1.0): no cut between them
  const legal = (c: number) => (bytes[c] & 0x80) === 0 || bytes[c] === ts;
  for (let i = 1; i < bytes.length; i++)
    for (let j = i; j < bytes.length; j += 3)
      if (legal(i) && legal(j)) assert.deepEqual(decode([i, j]), [a, b], `cuts ${i}, ${j}`);
  // module-sized notifications (19 B after the header) at every phase
  for (let ph = 1; ph < 20; ph++) {
    const cuts: number[] = [];
    for (let c = ph; c < bytes.length; c += 19) cuts.push(legal(c) ? c : c - 1);
    assert.deepEqual(decode(cuts), [a, b], `phase ${ph}`);
  }
  // the lost notification [80 ts F7 ts F0 40 7F]
  const from = a.length; // index of the ts before a's F7 in `bytes`
  const out: Uint8Array[] = [];
  const dec = new BleMidiDecoder((m) => out.push(m));
  dec.feed([0x80, ...bytes.slice(0, from)]);
  // (lost) [0x80, ...bytes.slice(from, from + 6)]
  dec.feed([0x80, ...bytes.slice(from + 6)]);
  assert.equal(out.length, 1);
  assert.ok(toHex(out[0]).includes(toHex(fromHex("6B 24 04 1E 02 6C"))), "glued like the field log");
  assert.equal(out[0].length, a.length + b.length - 4);
});
