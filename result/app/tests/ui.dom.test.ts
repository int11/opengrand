// UI smoke test in jsdom: every screen renders, generated controls reflect
// store values, and user edits produce the right SysEx on the link.
import { after, test } from "node:test";
import assert from "node:assert/strict";
import { JSDOM } from "jsdom";

const dom = new JSDOM("<!doctype html><body><div id=app></div></body>", { url: "http://localhost/" });
const g = globalThis as unknown as Record<string, unknown>;
g.window = dom.window;
g.document = dom.window.document;
g.HTMLElement = dom.window.HTMLElement;
g.Node = dom.window.Node;
g.CustomEvent = dom.window.CustomEvent;
g.requestAnimationFrame = (f: () => void) => setTimeout(f, 0) as unknown as number;
g.cancelAnimationFrame = (h: number) => clearTimeout(h);
const realSetInterval = globalThis.setInterval;
// the Music screen polls progress; keep the test process from staying alive
g.setInterval = ((f: () => void, ms: number) => realSetInterval(f, ms).unref()) as unknown;
after(() => dom.window.close());

const { ParamStore } = await import("../src/state/store.ts");
const { bindStore } = await import("../src/ui/controls.ts");
const S = await import("../src/ui/screens.ts");
const { PianoSession } = await import("../src/protocol/session.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const { toHex, encodeFrame, decodeFrame } = await import("../src/protocol/sysex.ts");

function setup() {
  const [app, piano] = LoopbackLink.pair();
  const sent: string[] = [];
  piano.onMessage((m) => {
    sent.push(toHex(m));
    const f = decodeFrame(m);
    if (f && f.fn === 0x10) piano.send(encodeFrame({ dev: 0, fn: 0x07, group: 4, machine: 0x1e, body: f.body.slice(0, 3) }));
  });
  const store = new ParamStore();
  bindStore(store);
  store.attach(new PianoSession(app, { ackTimeoutMs: 50 }));
  return { store, sent, piano };
}

test("all screens render without throwing", () => {
  const { store } = setup();
  const ev = new EventTarget();
  for (const el of [S.soundScreen(store), S.vtScreen(store), S.metronomeScreen(store, ev), S.musicScreen(store), S.settingsScreen(store)]) {
    assert.ok(el.querySelectorAll("section").length > 0);
    assert.equal(el.querySelectorAll(".muted").length >= 0, true);
  }
});

test("VT screen exposes the 22 technician controls bound to parameters", () => {
  const { store } = setup();
  const el = S.vtScreen(store);
  const rows = el.querySelectorAll("section:nth-of-type(2) .row");
  assert.equal(rows.length, 22);
  assert.equal(el.querySelectorAll(".row.muted").length, 0, "every VT parameter exists in the protocol table");
});

test("controls follow piano notifications and send edits", async () => {
  const { store, sent, piano } = setup();
  const el = S.soundScreen(store);
  document.body.append(el);
  // piano tells us ReverbType = Concert Hall (3)
  piano.send(encodeFrame({ dev: 0, fn: 0x10, group: 4, machine: 0x1e, body: [0x55, 0x08, 0x00, 3] }));
  await new Promise((r) => setTimeout(r, 5));
  const reverb = [...el.querySelectorAll("section")].find((s) => s.querySelector("h3")?.textContent === "Reverb")!;
  const sel = [...reverb.querySelectorAll("label")].find((l) => l.textContent?.startsWith("Type"))!.querySelector("select")!;
  assert.equal(sel.selectedOptions[0].textContent, "Concert Hall");
  // user picks Cathedral
  sel.value = String([...sel.options].findIndex((o) => o.textContent === "Cathedral"));
  sel.dispatchEvent(new dom.window.Event("change"));
  await new Promise((r) => setTimeout(r, 20));
  assert.ok(sent.includes("F0 40 7F 10 08 02 55 08 00 05 F7"), sent.join("\n"));
  // a piano sound sends a 3-byte Sound message
  const chip = [...el.querySelectorAll(".sound-item .item")].find((b) => b.textContent === "EX | Concert") as HTMLElement;
  chip.click();
  await new Promise((r) => setTimeout(r, 20));
  assert.ok(sent.some((s) => s.startsWith("F0 40 7F 10 08 02 55 00 00 5F 1B")), "EX Concert = bank 95/27");
});

test("metronome controls send tempo and run commands", async () => {
  const { store, sent } = setup();
  const el = S.metronomeScreen(store, new EventTarget());
  document.body.append(el);
  const plus = [...el.querySelectorAll("button")].find((b) => b.textContent === "+")!;
  plus.click();
  const start = [...el.querySelectorAll("button")].find((b) => b.textContent === "Start")!;
  start.click();
  await new Promise((r) => setTimeout(r, 20));
  assert.ok(sent.includes("F0 40 7F 10 08 02 56 01 7F 00 79 F7"), "tempo 121"); // default 120 + 1
  assert.ok(sent.includes("F0 40 7F 32 08 02 56 08 7F 01 F7"), "metronome run (fn 0x32)");
});

test("metronome button follows the piano: start/stop from the panel or another app (56 08 notification) and beats", async () => {
  const { store, piano } = setup();
  const beats = document.createElement("div"); // jsdom target, so jsdom's CustomEvent can be dispatched
  const el = S.metronomeScreen(store, beats);
  document.body.append(el);
  const runBtn = () => [...el.querySelectorAll("button")].find((b) => b.textContent === "Start" || b.textContent === "Stop")!;
  assert.equal(runBtn().textContent, "Start");
  // the core fans out the run state to the other controllers as a value frame (F0 40 7F 10 04 1E 56 08 7F 01 F7)
  piano.send(encodeFrame({ dev: 0, fn: 0x10, group: 4, machine: 0x1e, body: [0x56, 0x08, 0x7f, 0x01] }));
  await new Promise((r) => setTimeout(r, 20));
  assert.equal(runBtn().textContent, "Stop");
  piano.send(encodeFrame({ dev: 0, fn: 0x10, group: 4, machine: 0x1e, body: [0x56, 0x08, 0x7f, 0x00] }));
  await new Promise((r) => setTimeout(r, 20));
  assert.equal(runBtn().textContent, "Start");
  beats.dispatchEvent(new CustomEvent("beat", { detail: 2 }));
  assert.equal(runBtn().textContent, "Stop");
});

test("two-value settings: a switch only for an on/off pair, a select otherwise (Speaker volume Normal / Low, Headphone volume Normal / High) - field 2026-11-13 C17", async () => {
  const { store, sent, piano } = setup();
  const el = S.settingsScreen(store);
  document.body.append(el);
  const row = (label: string) => [...el.querySelectorAll("label")].find((l) => l.querySelector("span")?.textContent === label)!;
  piano.send(encodeFrame({ dev: 0, fn: 0x10, group: 4, machine: 0x1e, body: [0x51, 0x0b, 0x7f, 1] }));
  piano.send(encodeFrame({ dev: 0, fn: 0x10, group: 4, machine: 0x1e, body: [0x51, 0x0a, 0x7f, 1] }));
  await new Promise((r) => setTimeout(r, 5));
  const sv = row("Speaker volume");
  assert.equal(sv.querySelector("input[type=checkbox]"), null, "Normal / Low is a choice, not a switch");
  const svSel = sv.querySelector("select") as HTMLSelectElement;
  assert.equal(svSel.selectedOptions[0]?.textContent, "Normal");
  svSel.value = String([...svSel.options].findIndex((o) => o.textContent === "Low"));
  svSel.dispatchEvent(new dom.window.Event("change"));
  await new Promise((r) => setTimeout(r, 20));
  assert.ok(sent.includes("F0 40 7F 10 08 02 51 0B 7F 00 F7"), "Low = 0: " + sent.join("\n"));
  const hp = row("Headphone volume");
  assert.equal(hp.querySelector("input[type=checkbox]"), null, "Normal 1 / High 2 is not a switch");
  const sel = hp.querySelector("select") as HTMLSelectElement;
  assert.equal(sel.selectedOptions[0]?.textContent, "Normal");
  sel.value = String([...sel.options].findIndex((o) => o.textContent === "High"));
  sel.dispatchEvent(new dom.window.Event("change"));
  await new Promise((r) => setTimeout(r, 20));
  assert.ok(sent.includes("F0 40 7F 10 08 02 51 0A 7F 02 F7"), "High = 2");
  // two-byte on/off (Transmit Program Change Off 5 0 / On 5 1): on when the piano says 5 1, sends both bytes
  piano.send(encodeFrame({ dev: 0, fn: 0x10, group: 4, machine: 0x1e, body: [0x58, 0x09, 0x7f, 5, 1] }));
  await new Promise((r) => setTimeout(r, 5));
  const pc = [...el.querySelectorAll("label")].find((l) => l.querySelector("input[type=checkbox]") && /Program/i.test(l.textContent ?? ""))?.querySelector("input") as HTMLInputElement;
  assert.ok(pc && pc.checked, "5 1 = on");
  pc.checked = false;
  pc.dispatchEvent(new dom.window.Event("change"));
  await new Promise((r) => setTimeout(r, 20));
  assert.ok(sent.includes("F0 40 7F 10 08 02 58 09 7F 05 00 F7"), "off = 5 0: " + sent.filter((x) => x.includes("58 09")).join(" | "));
});
