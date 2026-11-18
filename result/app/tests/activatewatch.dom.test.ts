// The app's watch over the loader's Control 1 for older USB-bundle loaders (src/ui/activatewatch.ts) — field
// 2026-11-15 (last lines): the notice stayed 43 min.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const { watchActivate } = await import("../src/ui/activatewatch.ts");

/** The USB bundle 0.2.9 loader's Control 1, as it is on the panel: event, notice, navigate in 300 ms (here: never lands). */
function oldLoader(doc: Document) {
  const listeners: ((k: string, i: Record<string, unknown>) => void)[] = [];
  const nav: string[] = [];
  const ui = {
    on: (f: (k: string, i: Record<string, unknown>) => void) => void listeners.push(f),
    control(sub: number) {
      if (sub !== 1) return 1;
      listeners.forEach((f) => f("activate", {}));
      const el = doc.createElement("div");
      el.id = "ca79ui-notice";
      el.textContent = "새 UI 시작 중 · starting the new UI…";
      doc.body.appendChild(el);
      setTimeout(() => nav.push("index.html?embedded=1"), 30);
      return 0;
    },
  };
  return { ui, nav };
}

test("activate watch (older loader): a reload that never happens → notice down, retried when shown again, at most 3 times", async () => {
  const doc = dom.window.document;
  let vis: "visible" | "hidden" = "visible";
  Object.defineProperty(doc, "visibilityState", { get: () => vis, configurable: true });
  const show = (v: "visible" | "hidden") => { vis = v; doc.dispatchEvent(new dom.window.Event("visibilitychange")); };
  const on = () => !!doc.getElementById("ca79ui-notice");
  try {
    const { ui, nav } = oldLoader(doc);
    assert.equal(watchActivate(ui, { doc, timeoutMs: 80 }), true);
    ui.control(1);
    assert.equal(on(), true);
    await tick(150);
    assert.equal(nav.length, 1);
    assert.equal(on(), false, "notice taken down: the old UI is usable");
    show("hidden");
    show("visible");
    assert.equal(on(), true, "shown again: the loader's Control 1 again");
    await tick(150);
    assert.equal(nav.length, 2);
    assert.equal(on(), false);
    for (let i = 0; i < 3; i++) { show("hidden"); show("visible"); await tick(150); }
    assert.equal(nav.length, 4, "stalls 1–3 retried, the 4th is left for the next boot");
    assert.equal(on(), false);
  } finally {
    delete (doc as unknown as { visibilityState?: string }).visibilityState;
    doc.body.replaceChildren();
  }
});

test("activate watch stands down for a loader that watches itself, or without a loader", () => {
  const { ui } = oldLoader(dom.window.document);
  assert.equal(watchActivate({ ...ui, activateWatch: 1 }), false);
  assert.equal(watchActivate(undefined), false);
});
