// Touch scrolling never presses a control (field 2026-11-03, ui/tapguard.ts):
// a drag / scroll over buttons, switches, selects and sliders changes nothing; a real tap still clicks;
// a slider moves only on a clear sideways drag, from where it was; mouse and keyboard are unchanged.
import { dom, pianoPair, tick } from "./_dom.ts";
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { JSDOM } from "jsdom";

const w = dom.window as unknown as Window & typeof globalThis;
// jsdom has no PointerEvent: a MouseEvent carrying pointerId / pointerType stands in (the guard reads only those)
class FakePointerEvent extends w.MouseEvent {
  pointerId: number;
  pointerType: string;
  constructor(type: string, init: MouseEventInit & { pointerId?: number; pointerType?: string } = {}) {
    super(type, init);
    this.pointerId = init.pointerId ?? 1;
    this.pointerType = init.pointerType ?? "touch";
  }
}
(w as unknown as Record<string, unknown>).PointerEvent = FakePointerEvent;

const { installTapGuard, sliderFor } = await import("../src/ui/tapguard.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { bindStore, control } = await import("../src/ui/controls.ts");
const S = await import("../src/ui/screens.ts");
const { createContext } = await import("../src/ui/context.ts");

let clock = 1000;
const now = () => clock;
let guard: ReturnType<typeof installTapGuard> | null = null;
afterEach(() => {
  guard?.dispose();
  guard = null;
  document.body.replaceChildren();
});

function ptr(el: Element, type: string, x: number, y: number, pointerType = "touch") {
  el.dispatchEvent(new FakePointerEvent(type, { bubbles: true, cancelable: true, clientX: x, clientY: y, pointerType, pointerId: pointerType === "mouse" ? 9 : 1 }));
}
/** the compat click Chrome may still deliver after a touch (detail 1 = a pointer click, not a key) */
function tapClick(el: Element): boolean {
  return el.dispatchEvent(new w.MouseEvent("click", { bubbles: true, cancelable: true, detail: 1 }));
}
function scrollOver(el: Element, dy = 60, cancel = true) {
  ptr(el, "pointerdown", 100, 200);
  for (let i = 1; i <= 6; i++) ptr(el, "pointermove", 100 + (i % 2), 200 - (dy * i) / 6);
  ptr(el, cancel ? "pointercancel" : "pointerup", 101, 200 - dy);
}
function tap(el: Element) {
  ptr(el, "pointerdown", 100, 200);
  ptr(el, "pointermove", 103, 202);
  ptr(el, "pointerup", 103, 202);
  tapClick(el);
}

test("a touch scroll over a button does not click it; a real tap does", () => {
  guard = installTapGuard(document, { touch: true, now });
  let clicks = 0;
  const b = document.createElement("button");
  b.addEventListener("click", () => clicks++);
  document.body.append(b);
  scrollOver(b); // the browser's scroll took over (pointercancel)
  tapClick(b);
  assert.equal(clicks, 0, "after a scroll (cancel)");
  clock += 1000;
  scrollOver(b, 40, false); // moved 40 px, finger lifted without a cancel
  assert.equal(tapClick(b), false, "click cancelled");
  assert.equal(clicks, 0, "after a drag (up)");
  clock += 1000;
  tap(b);
  assert.equal(clicks, 1, "a tap within the slop clicks");
  // keyboard / programmatic clicks (detail 0) are never touched, even right after a scroll
  scrollOver(b);
  b.click();
  assert.equal(clicks, 2);
});

test("switches and select rows (label rows) do not change on a scroll; a tap toggles", () => {
  guard = installTapGuard(document, { touch: true, now });
  const sw = document.createElement("input");
  sw.type = "checkbox";
  let changes = 0;
  sw.addEventListener("change", () => changes++);
  const label = document.createElement("label");
  label.className = "row";
  const span = document.createElement("span");
  span.textContent = "Switch";
  label.append(span, sw);
  const sel = document.createElement("select");
  let opened = 0;
  sel.addEventListener("mousedown", (e) => !e.defaultPrevented && opened++);
  document.body.append(label, sel);

  scrollOver(span);
  tapClick(span); // label activation would toggle the switch
  scrollOver(sw);
  tapClick(sw);
  assert.equal(sw.checked, false);
  assert.equal(changes, 0);
  scrollOver(sel);
  sel.dispatchEvent(new w.MouseEvent("mousedown", { bubbles: true, cancelable: true, detail: 1 }));
  assert.equal(opened, 0, "a select does not open from a scroll");
  clock += 1000;
  tap(sw);
  assert.equal(sw.checked, true);
  assert.equal(changes, 1);
});

test("a tap that stops a running fling is not a tap", () => {
  guard = installTapGuard(document, { touch: true, now });
  let clicks = 0;
  const b = document.createElement("button");
  b.addEventListener("click", () => clicks++);
  document.body.append(b);
  scrollOver(b);
  clock += 300;
  document.dispatchEvent(new w.Event("scroll")); // momentum still scrolling
  clock += 50;
  tap(b);
  assert.equal(clicks, 0);
  clock += 1000; // the page came to rest
  tap(b);
  assert.equal(clicks, 1);
});

async function sliderRow() {
  const { session, sent } = await pianoPair();
  const store = new ParamStore();
  bindStore(store);
  store.attach(session);
  const row = control(store, "ReverbDepth", { label: "Reverb depth" });
  document.body.append(row);
  const input = row.querySelector("input")!;
  input.getBoundingClientRect = () => ({ width: 200, height: 44, left: 0, top: 0, right: 200, bottom: 44, x: 0, y: 0, toJSON() {} }) as DOMRect;
  let inputs = 0;
  input.addEventListener("input", () => inputs++);
  return { row, input, sent, inputs: () => inputs };
}

test("a vertical scroll that starts on a slider leaves its value alone", async () => {
  guard = installTapGuard(document, { touch: true, now });
  const { row, input, sent, inputs } = await sliderRow();
  assert.equal(sliderFor(row.querySelector("span")), input, "the whole row drives its slider");
  const v0 = input.value;
  scrollOver(input, 80);
  scrollOver(row.querySelector("span")!, -80);
  // slanted but mostly vertical: still a scroll
  ptr(input, "pointerdown", 50, 300);
  ptr(input, "pointermove", 56, 291);
  ptr(input, "pointermove", 70, 250);
  ptr(input, "pointerup", 70, 250);
  tapClick(input);
  await tick();
  assert.equal(input.value, v0);
  assert.equal(inputs(), 0);
  assert.deepEqual(sent, [], "nothing sent to the piano");
  assert.ok(document.documentElement.classList.contains("touch-ui"), "native range takes no touch (styles.css)");
});

test("a clear sideways drag moves the slider from where it was (no jump), input while dragging, change at the end", async () => {
  guard = installTapGuard(document, { touch: true, now });
  const { input, sent, inputs } = await sliderRow();
  const span = Number(input.max) - Number(input.min);
  input.value = String(Number(input.min) + Math.round(span / 2));
  const v0 = Number(input.value);
  let changes = 0;
  input.addEventListener("change", () => changes++);
  ptr(input, "pointerdown", 20, 100); // far left of the track: a native range would jump here
  ptr(input, "pointermove", 24, 101);
  assert.equal(Number(input.value), v0, "below the lock distance nothing moves");
  ptr(input, "pointermove", 30, 101); // locks sideways
  assert.equal(Number(input.value), v0, "locking does not move it");
  ptr(input, "pointermove", 80, 104); // +50 px of a 200 px track
  assert.equal(Number(input.value), v0 + Math.round(span / 4));
  ptr(input, "pointermove", 80, 160); // vertical wander after the lock: the slider keeps the gesture
  assert.equal(Number(input.value), v0 + Math.round(span / 4));
  assert.ok(inputs() >= 1);
  assert.equal(changes, 0);
  ptr(input, "pointerup", 80, 160);
  assert.equal(changes, 1);
  await tick(30);
  assert.equal(sent.length, 1, "one value sent at the end of the drag");
});

test("mouse and keyboard keep the native behaviour (desktop)", () => {
  guard = installTapGuard(document, { now });
  let clicks = 0;
  const b = document.createElement("button");
  b.addEventListener("click", () => clicks++);
  document.body.append(b);
  ptr(b, "pointerdown", 10, 10, "mouse");
  ptr(b, "pointermove", 10, 80, "mouse");
  ptr(b, "pointerup", 10, 80, "mouse");
  tapClick(b);
  assert.equal(clicks, 1, "a mouse click is never swallowed");
  assert.equal(document.documentElement.classList.contains("touch-ui"), false);
  // a touch switches touch mode on; a real mouse moving switches it back off (hybrid PC)
  tap(b);
  assert.ok(document.documentElement.classList.contains("touch-ui"));
  ptr(b, "pointermove", 12, 12, "mouse");
  assert.equal(document.documentElement.classList.contains("touch-ui"), false);
  assert.equal(clicks, 2);
});

test("VT preview target follows a tap on a row, not a scroll starting on it", async () => {
  guard = installTapGuard(document, { touch: true, now });
  const { session } = await pianoPair();
  const store = new ParamStore();
  bindStore(store);
  store.attach(session);
  const ctx = createContext(store);
  const el = S.vtScreen(store, ctx);
  document.body.append(el);
  const target = [...el.querySelectorAll(".row")].find((r) => r.querySelector("span")?.textContent === "Parameter")!.querySelector("select")!;
  const before = target.value;
  const dn = [...el.querySelectorAll(".row")].find((r) => r.querySelector("span")?.textContent === "Damper Noise")!;
  const label = dn.querySelector("span")!;
  scrollOver(label);
  tapClick(label);
  assert.equal(target.value, before);
  clock += 1000;
  tap(label);
  assert.equal(target.value, "DamperNoise");
});

test("without PointerEvent the guard runs on touch events", () => {
  const d2 = new JSDOM("<!doctype html><body><button>b</button></body>");
  const w2 = d2.window;
  const doc2 = w2.document;
  const g2 = installTapGuard(doc2, { touch: true, now });
  let clicks = 0;
  const b = doc2.querySelector("button")!;
  b.addEventListener("click", () => clicks++);
  const touch = (type: string, x: number, y: number) => {
    const e = new w2.Event(type, { bubbles: true, cancelable: true });
    Object.defineProperty(e, "changedTouches", { value: [{ identifier: 3, clientX: x, clientY: y }] });
    b.dispatchEvent(e);
  };
  const click = () => b.dispatchEvent(new w2.MouseEvent("click", { bubbles: true, cancelable: true, detail: 1 }));
  touch("touchstart", 50, 300);
  touch("touchmove", 50, 260);
  touch("touchend", 50, 220);
  click();
  assert.equal(clicks, 0);
  clock += 1000;
  touch("touchstart", 50, 300);
  touch("touchend", 52, 301);
  click();
  assert.equal(clicks, 1);
  g2.dispose();
  w2.close();
});
