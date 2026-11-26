// Chords tab 「Play on piano」 tapped again before the first chord's 1.2 s are over (field class 2026-11-27: rapid taps
// produced interleaved multi-frame sequences): the first tap's note-offs must not cut the second chord short.
import { byText, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const { createContext } = await import("../src/ui/context.ts");
const { chordScreen } = await import("../src/ui/chords.ts");
const i18n = await import("../src/i18n.ts");

test("chords: a second Play within 1.2 s ends the first chord at once and keeps its own notes for 1.2 s", async () => {
  i18n.setLang("en");
  const log: { at: number; m: number[] }[] = [];
  const t0 = Date.now();
  const session = { sendRaw: (m: Uint8Array) => log.push({ at: Date.now() - t0, m: [...m] }) };
  const ctx = createContext(new ParamStore(), { session: () => session as never });
  const el = chordScreen(ctx);
  document.body.append(el);
  const play = byText<HTMLButtonElement>(el, "button", /Play on piano/);
  play.click();
  await tick(600);
  play.click();
  const second = Date.now() - t0;
  const mark = log.length; // everything the second tap sent ends here
  await tick(700); // 1.3 s after the first tap, 0.7 s after the second
  const isOff = (e: { m: number[] }) => (e.m[0]! & 0xf0) === 0x80;
  const ons = log.filter((e) => (e.m[0]! & 0xf0) === 0x90);
  assert.equal(ons.length % 2, 0);
  const late = log.slice(mark).filter(isOff);
  assert.equal(late.length, 0, `the first tap's note-offs cut the second chord after ${late.map((e) => e.at - second).join(", ")} ms`);
  // the first chord ended before the second started (no note left hanging)
  assert.equal(log.slice(0, mark).filter(isOff).length, ons.length / 2);
  el.remove();
  await tick(1300);
});
