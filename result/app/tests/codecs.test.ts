import { test } from "node:test";
import assert from "node:assert/strict";

import { beat, bpm, enumLabel, findParam, paramByWire, progress, rangeDecode, rangeEncode, tuning } from "../src/protocol/codecs.ts";
import { PARAMS, SOUNDS, RHYTHMS } from "../src/protocol/params.gen.ts";
import { ParamStore } from "../src/state/store.ts";

test("generated tables cover the CA79 protocol", () => {
  assert.ok(PARAMS.length >= 200);
  assert.equal(SOUNDS.length, 67);
  assert.equal(SOUNDS[0].name, "SK-EX Rendering");
  assert.equal(RHYTHMS.length, 100);
});

test("range codec matches the JS encoder (ResonanceDepth, HalfPedalAdjust)", () => {
  const rd = findParam("ResonanceDepth")!;
  assert.deepEqual([1, 2, 3, 4, 5, 6, 7, 8, 9, 10].map((v) => rangeEncode(rd, v)), [12, 24, 37, 50, 63, 75, 88, 101, 114, 127]);
  for (let v = 1; v <= 10; v++) assert.equal(rangeDecode(rd, rangeEncode(rd, v)), v);
  const hp = findParam("HalfPedalAdjust")!;
  assert.equal(rangeEncode(hp, 1), 12);
  assert.equal(rangeEncode(hp, 10), 127);
  assert.throws(() => rangeEncode(rd, 11));
});

test("enum labels and wire lookup", () => {
  const rt = findParam("ReverbType")!;
  assert.equal(enumLabel(rt, 5), "Cathedral");
  assert.equal(paramByWire(0x10, 0x55, 0x08, 0x00)?.name, "ReverbType");
  // same (fn,v1,v2) for the Global part and per part: resolved by part byte
  assert.equal(paramByWire(0x10, 0x53, 0x02, 0x0f)?.scope, "system");
});

test("data codecs", () => {
  assert.deepEqual(bpm.encode(300), [2, 44]);
  assert.equal(bpm.decode([2, 44]), 300);
  assert.deepEqual(beat.encode(6, 8), [6, 3]);
  assert.deepEqual(beat.decode([6, 3]), { num: 6, den: 8 });
  assert.deepEqual(tuning.encode(442.5), [69]);
  assert.equal(tuning.decode([38]), 427);
  assert.deepEqual(progress.decode([0, 0, 12, 3, 0, 0]), { bar: 12, beat: 3 });
});

test("store applies wire messages and notifies", () => {
  const s = new ParamStore();
  const seen: string[] = [];
  s.subscribe((k) => seen.push(k));
  s.fromWire({ fn: 0x10, v1: 0x55, v2: 0x08, part: 0, data: [3] });
  assert.deepEqual(s.get(findParam("ReverbType"), 0), [3]);
  assert.deepEqual(seen, ["ReverbType@0"]);
});
