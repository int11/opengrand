// Sound select tapped twice in quick succession (field class 2026-11-27: independent async sequences interleaved on
// one MIDI link): the second sound's sequence must not start before the first one's has ended, and the piano must end
// with the second sound's own reverb / effect values.
import { pianoPair, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { SOUNDS } = await import("../src/protocol/params.gen.ts");
const { SoundLibrary } = await import("../src/state/settings.ts");
const PS = await import("../src/features/pianosound.ts");

const sound = (name: string) => SOUNDS.find((s) => s.name === name)!;
const hex = (b: number) => b.toString(16).toUpperCase().padStart(2, "0");
const setFrame = (v1: number, v2: number, part: number, data: number[]) => ["F0 40 7F 10 08 02", ...[v1, v2, part, ...data].map(hex), "F7"].join(" ");

test("sound select: a second sound tapped while the first is still sent waits, and its own values win on the piano", async () => {
  const p = await pianoPair({ endGets: true });
  const store = new ParamStore();
  store.attach(p.session);
  const lib = new SoundLibrary();
  const concert = sound("SK-EX | Concert");
  const ep = sound("Classic Electric Piano");
  // the piano plays SK-EX Concert with its own values
  p.notify([0x55, 0x00, 0x00, concert.msb, concert.lsb, concert.pc]);
  for (const s of PS.soundSettings(concert)) p.notify([s.p.v1, s.p.v2, s.part, ...s.data]);
  await tick();
  p.sent.length = 0;
  const a = PS.selectSound(store, lib, PS.MAIN, ep);
  const b = PS.selectSound(store, lib, PS.MAIN, concert);
  assert.deepEqual(await Promise.all([a, b]), [true, true]);
  const soundB = p.sent.indexOf(setFrame(0x55, 0x00, 0x00, [concert.msb, concert.lsb, concert.pc]));
  const lastA = Math.max(...PS.soundSettings(ep).map((s) => p.sent.lastIndexOf(setFrame(s.p.v1, s.p.v2, s.part, s.data))));
  assert.ok(soundB > lastA, `the second sound went out at ${soundB}, before the first sound's values ended at ${lastA}:\n${p.sent.join("\n")}`);
  // the last value of each of Concert's settings on the wire is Concert's
  for (const s of PS.soundSettings(concert)) {
    const last = [...p.sent].reverse().find((l) => l.startsWith(setFrame(s.p.v1, s.p.v2, s.part, []).slice(0, -3)));
    if (last) assert.equal(last, setFrame(s.p.v1, s.p.v2, s.part, s.data), s.p.name);
  }
});
