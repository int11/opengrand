// MIDI -> KSO converter (src/protocol/kso.ts). tests/fixtures/kso_stock.json is written by the stock firmware
// emulator (result/stockfw/tools/stock_kso.py --write): each case's KSO was imported by the stock importer,
// passed its boot check, was played by the stock player with every note / pedal / tempo at its tick, and was
// saved back as SMF by the stock exporter - the bytes compared here.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { checkKso, KsoError, ksoToSmf, parseKso, smfToKso } from "../src/protocol/kso.ts";
import { buildSmf, KSO_CASES, note, sig, tempo } from "./fixtures/kso_cases.ts";

const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join(" ");
const stock: { name: string; kso: string; stockSmf: string }[] = JSON.parse(
  readFileSync(new URL("./fixtures/kso_stock.json", import.meta.url), "utf8"),
);

/** Note-ons / offs / pedals / tempos of a format-0 SMF as "tick kind ..." lines. */
function smfLines(smf: Uint8Array): string[] {
  const out: string[] = [];
  let p = 22;
  let tick = 0;
  while (p < smf.length) {
    let d = 0;
    for (;;) {
      const c = smf[p++];
      d = (d << 7) | (c & 0x7f);
      if (c < 0x80) break;
    }
    tick += d;
    const st = smf[p];
    if (st === 0xff) {
      const n = smf[p + 2];
      if (smf[p + 1] === 0x51) out.push(`${tick} tempo ${Math.round(60e6 / ((smf[p + 3] << 16) | (smf[p + 4] << 8) | smf[p + 5]))}`);
      if (smf[p + 1] === 0x2f) out.push(`${tick} end`);
      p += 3 + n;
    } else {
      out.push(`${tick} ${st.toString(16)} ${smf[p + 1]} ${(st & 0xf0) === 0x80 ? "" : smf[p + 2]}`.trim());
      p += 3;
    }
  }
  return out;
}

test("every case: the KSO is the one the stock firmware accepted, and ksoToSmf equals the stock SMF export", () => {
  assert.equal(stock.length, KSO_CASES.length);
  KSO_CASES.forEach((c, i) => {
    const { kso } = smfToKso(c.smf, c.opts);
    assert.equal(stock[i].name, c.name);
    assert.equal(hex(kso), stock[i].kso, `${c.name}: fixture is stale - run tools/stock_kso.py --write`);
    assert.equal(checkKso(kso), null);
    assert.equal(hex(ksoToSmf(kso)), stock[i].stockSmf, c.name);
  });
});

test("file layout: header, name, big-endian offsets, 4-byte aligned, no data byte the bar counter could misread", () => {
  const { kso, bars, notes } = smfToKso(KSO_CASES[1].smf, KSO_CASES[1].opts);
  assert.equal(String.fromCharCode(...kso.subarray(0, 4)), "hKSO");
  assert.equal(String.fromCharCode(...kso.subarray(0x10, 0x30)), "WALTZ   - SoundMode".padEnd(32));
  assert.equal((kso.length - 0x10) % 4, 0);
  const dv = new DataView(kso.buffer);
  assert.equal(dv.getUint32(0x30), 0); // no part settings: the piano's current sound
  assert.equal(dv.getUint32(0x34), 0x84); // tempo list right after the header
  assert.equal(dv.getUint32(0x38), 16);
  assert.equal(dv.getUint32(0x80), bars);
  assert.deepEqual([bars, notes], [2, [3, 2]]);
  const song = parseKso(kso);
  assert.deepEqual(song.tempos, [{ tick: 0, bpm: 96 }, { tick: 288, bpm: 132 }]);
  assert.deepEqual(song.tracks.map((t) => !!t), [true, true, ...new Array(14).fill(false)]);
  // part 1: FA 3/4, note, pedal down, beat, note, beat, long note (FE form), pedal up 2 ticks before the bar end
  assert.equal(hex(song.tracks[0]!), "fa 22 / 48 00 60 fd 50 40 / b0 00 40 7f / f8 / 4c 00 60 fd 50 40 / f8 / 4f 00 fe fd 03 00 7f 40 / b0 5e 40 00 / fa 22 f8 f8 / fc".replace(/ \/ /g, " "));
});

test("round trip: notes, pedals and tempo changes come back at the same ticks", () => {
  const Q = 96;
  const smf = buildSmf(1, Q, [
    [tempo(0, 100), sig(0, 4, 4), tempo(5 * Q, 250), tempo(9 * Q, 399)],
    [...note(0, 0, Q, 60, 1), ...note(0, Q + 7, 300, 64, 127), ...note(0, 4 * Q, 20000, 67, 64), [10, 0xb0, 64, 127], [50, 0xb0, 64, 0]],
    [...note(1, 3, 95, 40, 33), ...note(1, 8 * Q - 1, 2, 41, 34), [8 * Q, 0xb1, 66, 127]],
  ]);
  const r = smfToKso(smf);
  assert.deepEqual(r.warnings, ["1 notes longer than 16383 ticks were shortened"]);
  assert.deepEqual(smfLines(ksoToSmf(r.kso, { leadBar: false })), [
    "0 tempo 100", "0 tempo 100", // the stock exporter writes the first tempo twice
    "0 90 60 1", "3 92 40 33", "10 b0 64 127", "50 b0 64 0", "96 80 60", "98 82 40",
    "103 90 64 127", "384 90 67 64", "403 80 64", "480 tempo 250", "767 92 41 34", "768 b2 66 127", "769 82 41", "864 tempo 399",
    `${384 + 16383} 80 67`, `${Math.ceil((384 + 20000) / 384) * 384} end`, // the song keeps the length of the MIDI file
  ]);
  // the piano's own SMF export puts one empty bar in front
  assert.equal(smfLines(ksoToSmf(r.kso))[2], "384 90 60 1");
});

test("what the piano cannot hold is left out with a warning", () => {
  const smf = buildSmf(0, 480, [[
    sig(0, 7, 32), tempo(0, 5), ...note(0, 0, 480, 60), ...note(9, 0, 480, 36), ...note(3, 0, 480, 50), ...note(5, 0, 480, 52),
    [0, 0xc0, 1], [0, 0xe0, 0, 64], [0, 0xb0, 1, 5], [0, 0xb0, 64, 1],
  ]]);
  const r = smfToKso(smf);
  assert.deepEqual(r.warnings, [
    "3 channels with notes: the first is part 1, the others are merged into part 2",
    "1 drum notes (channel 10) were left out",
    "1 controller events other than the three pedals were left out",
    "2 program change / pitch bend / aftertouch events were left out",
    "an unsupported time signature was ignored (4/4 or the previous one is used)",
    "a tempo outside 10-400 bpm was limited",
  ]);
  assert.deepEqual(r.notes, [1, 2]);
  assert.deepEqual(parseKso(r.kso).tempos, [{ tick: 0, bpm: 10 }]);
  assert.deepEqual(smfToKso(smf, { split: "single" }).notes, [3, 0]);
});

test("errors: not MIDI, SMPTE timing, no notes, too large", () => {
  const code = (fn: () => unknown) => {
    try {
      fn();
    } catch (e) {
      return e instanceof KsoError ? e.code : String(e);
    }
    return "no error";
  };
  assert.equal(code(() => smfToKso(new Uint8Array(20))), "smf");
  const smpte = buildSmf(0, 480, [note(0, 0, 10, 60)]);
  smpte[12] = 0xe7;
  assert.equal(code(() => smfToKso(smpte)), "smf");
  assert.equal(code(() => smfToKso(buildSmf(0, 480, [[tempo(0, 120)]]))), "empty");
  const many: [number, ...number[]][] = [];
  for (let k = 0; k < 180000; k++) many.push(...note(0, k * 24, 12, 60 + (k % 12)));
  assert.equal(code(() => smfToKso(buildSmf(0, 96, [many]))), "size");
});

test("checkKso refuses what the stock importer would store and then format away at the next power-on", () => {
  const good = smfToKso(KSO_CASES[0].smf).kso;
  const edit = (fn: (b: Uint8Array) => Uint8Array | void) => {
    const b = good.slice();
    return checkKso(fn(b) ?? b);
  };
  assert.equal(edit(() => {}), null);
  assert.equal(edit((b) => Uint8Array.from([...b, 0])), "song length is not a multiple of 4");
  assert.equal(edit((b) => void (b[0x3b] = 2)), "track slot count is not 16");
  assert.equal(edit((b) => void (b[3] = 0x58)), "not a KSO file");
  const track = new DataView(good.buffer).getUint32(0x3c);
  assert.equal(edit((b) => void (b[track] = 0xfc)), "track 1 does not start with a bar");
  assert.equal(edit((b) => void (b[track + 4] = 0xfa)), "track 1 has a data byte the bar counter would misread");
  assert.equal(edit((b) => void (b[0x3f] = 0xff)), "track 1 offset out of range");
  assert.throws(() => ksoToSmf(good.subarray(0, 40)), KsoError);
});
