// Metronome tab: the Start / Stop button follows what the piano got, and a rebuilt tab leaves no listener behind
// (field class 2026-11-27: dropped actions still changed the UI; stale handlers after a screen is rebuilt).
import { pianoPair, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const { ParamStore } = await import("../src/state/store.ts");
const S = await import("../src/ui/screens.ts");
const { fromHex } = await import("../src/protocol/sysex.ts");
const i18n = await import("../src/i18n.ts");

const DISPLACED = "F0 40 7F 7E 04 1E 01 00 F7"; // 7E 01 00: the panel took the binding back

test("metronome: Start pressed while the piano does not take commands (displaced) stays Start", async () => {
  i18n.setLang("en");
  const p = await pianoPair();
  await p.session.connect(200).catch(() => undefined);
  const store = new ParamStore();
  store.attach(p.session);
  p.piano.send(fromHex(DISPLACED));
  await tick();
  assert.equal(p.session.state, "displaced");
  const el = S.metronomeScreen(store, new EventTarget());
  document.body.append(el);
  const runBtn = () => [...el.querySelectorAll("button")].find((b) => b.textContent === "Start" || b.textContent === "Stop")!;
  runBtn().click();
  await tick();
  assert.equal(runBtn().textContent, "Start", "the run command was dropped: the button must not say Stop");
  el.remove();
});

test("metronome: a tab left and built again keeps no store / beat listener of the old one", async () => {
  i18n.setLang("en");
  const store = new ParamStore();
  const beats = new EventTarget();
  const old = S.metronomeScreen(store, beats);
  document.body.append(old);
  await tick();
  old.remove();
  const now = S.metronomeScreen(store, beats);
  document.body.append(now);
  const runBtn = (el: HTMLElement) => [...el.querySelectorAll("button")].find((b) => b.textContent === "Start" || b.textContent === "Stop")!;
  beats.dispatchEvent(new CustomEvent("beat", { detail: 1 }));
  store.fromWire({ fn: 0x10, v1: 0x56, v2: 0x01, part: 0x7f, data: [0x00, 0x64] });
  assert.equal(runBtn(now).textContent, "Stop");
  assert.equal(now.querySelector(".tempo")!.textContent, "100");
  assert.equal(runBtn(old).textContent, "Start", "the removed tab still follows the beats");
  assert.equal(old.querySelector(".tempo")!.textContent, "–", "the removed tab still follows the store");
  now.remove();
});
