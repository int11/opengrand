// MIDI files for the KSO converter tests. The same cases go through the stock firmware in the emulator
// (result/stockfw/tools/stock_kso.py), which writes tests/fixtures/kso_stock.json.
import type { SmfToKsoOptions } from "../../src/protocol/kso.ts";

type Ev = [tick: number, ...bytes: number[]];

const vlq = (v: number): number[] => {
  const b = [v & 0x7f];
  for (v >>= 7; v; v >>= 7) b.unshift((v & 0x7f) | 0x80);
  return b;
};

/** Build an SMF from tracks of [absolute tick, bytes...] events (an end-of-track is added). */
export function buildSmf(format: 0 | 1, ppq: number, tracks: Ev[][]): Uint8Array {
  let out = [0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, format, 0, tracks.length, ppq >> 8, ppq & 255];
  for (const t of tracks) {
    const body: number[] = [];
    let at = 0;
    for (const [tick, ...bytes] of [...t].sort((a, b) => a[0] - b[0])) {
      body.push(...vlq(tick - at), ...bytes);
      at = tick;
    }
    body.push(0, 0xff, 0x2f, 0);
    out = out.concat([0x4d, 0x54, 0x72, 0x6b, (body.length >> 24) & 255, (body.length >> 16) & 255, (body.length >> 8) & 255, body.length & 255], body);
  }
  return Uint8Array.from(out);
}

export const tempo = (tick: number, bpm: number): Ev => {
  const us = Math.round(60_000_000 / bpm);
  return [tick, 0xff, 0x51, 3, us >> 16, (us >> 8) & 255, us & 255];
};
export const sig = (tick: number, num: number, den: number): Ev => [tick, 0xff, 0x58, 4, num, Math.log2(den), 24, 8];
/** note on + note off */
export const note = (ch: number, tick: number, len: number, key: number, vel = 80): Ev[] => [
  [tick, 0x90 | ch, key, vel],
  [tick + len, 0x80 | ch, key, 0],
];

export interface KsoCase {
  name: string;
  smf: Uint8Array;
  opts?: SmfToKsoOptions;
}

const Q = 480;
/** Hand-written bytes: SysEx before the first note, running status, note-on with velocity 0 as note-off. */
const RAW_TRACK = [
  0x00, 0xf0, 0x05, 0x7e, 0x7f, 0x09, 0x01, 0xf7, // GM reset
  0x00, 0xff, 0x51, 0x03, 0x06, 0x1a, 0x80, // 150 bpm
  0x60, 0x90, 60, 70, 0x30, 64, 71, // running status
  0x30, 60, 0, 0x00, 64, 0, // velocity 0 = off
  0x00, 0xf7, 0x02, 0x01, 0x02, // escape
  0x60, 0x91, 48, 50, 0x81, 0x40, 0x81, 48, 0x40,
  0x00, 0xff, 0x2f, 0x00,
];
export const RAW_SMF = Uint8Array.from([
  0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 0x4d, 0x54, 0x72, 0x6b, 0, 0, 0, RAW_TRACK.length, ...RAW_TRACK,
]);

export const KSO_CASES: KsoCase[] = [
  {
    name: "one part, three quarter notes",
    smf: buildSmf(0, Q, [[tempo(0, 120), ...note(0, 0, Q, 60), ...note(0, Q, Q, 64, 100), ...note(0, 2 * Q, Q / 2, 67, 1)]]),
  },
  {
    name: "two tracks, 3/4, pedal, long note, tempo change",
    opts: { name: "WALTZ" },
    smf: buildSmf(1, Q, [
      [sig(0, 3, 4), tempo(0, 96), tempo(3 * Q, 132)],
      [...note(0, 0, Q, 72), ...note(0, Q, Q, 76), ...note(0, 2 * Q, 4 * Q, 79, 127), [0, 0xb0, 64, 127], [3 * Q - 10, 0xb0, 64, 0], [5, 0xb0, 7, 100]],
      [...note(1, 0, 3 * Q, 48, 60), ...note(1, 3 * Q, 3 * Q, 43, 60), [3 * Q, 0xb1, 67, 127], [0, 0xc1, 5]],
    ]),
  },
  {
    name: "6/8 then 2/4, fast tempo, repeated key, channels as parts",
    smf: buildSmf(0, 96, [
      [
        sig(0, 6, 8), tempo(0, 300), sig(2 * 288, 2, 4), tempo(2 * 288, 60),
        ...note(0, 0, 200, 60), [48, 0x90, 60, 90], [100, 0x80, 60, 64],
        ...note(1, 0, 288, 36), ...note(1, 288, 288, 41), ...note(2, 576, 96, 30), ...note(0, 576 + 100, 80, 84),
        ...note(9, 0, 10, 36),
      ],
    ]),
  },
  {
    name: "odd resolution, one part forced",
    opts: { split: "single", name: "long title here" },
    smf: buildSmf(1, 100, [[...note(0, 0, 33, 60), ...note(0, 33, 67, 62)], [...note(1, 150, 1000, 50)]]),
  },
  { name: "SysEx, running status, velocity-0 note off", smf: RAW_SMF },
];
