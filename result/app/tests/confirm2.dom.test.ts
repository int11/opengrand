// In-page two-step confirm (src/ui/confirm2.ts) — field 2026-11-15:
// an automated test drove the desktop app over CDP and accepted window.confirm for a song delete, yet Electron's native
// confirm box 「피아노의 곡 N을(를) 지울까요?」 stayed on the user's screen. No native dialog may exist in the app.
import "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";
import { tick } from "./_dom.ts";

const { CONFIRM, confirmTap } = await import("../src/ui/confirm2.ts");
const { h } = await import("../src/ui/dom.ts");

function files(dir: string): string[] {
  return readdirSync(dir).flatMap((n) => {
    const p = join(dir, n);
    return statSync(p).isDirectory() ? files(p) : p.endsWith(".ts") ? [p] : [];
  });
}

test("no native confirm() / alert() anywhere in the app source (the desktop app would show an Electron dialog)", () => {
  const src = new URL("../src/", import.meta.url).pathname;
  const hits: string[] = [];
  for (const f of files(src)) {
    readFileSync(f, "utf8").split("\n").forEach((line, i) => {
      const code = line.replace(/\/\/.*$/, "").replace(/CA79UI\?: \{ confirm\(\): void \}/g, "");
      // CA79UI.confirm() is the panel loader's "this version works" call, not a dialog
      if (/(^|[^.\w])(window\.|globalThis\.)?(confirm|alert)\s*\(/.test(code)) hits.push(`${f.slice(src.length)}:${i + 1}: ${line.trim()}`);
    });
  }
  assert.deepEqual(hits, []);
});

test("confirmTap: first tap asks in the page, a second deliberate tap runs; a double tap is one decision; time-out disarms", async () => {
  const gap = CONFIRM.gapMs, ms = CONFIRM.ms;
  CONFIRM.gapMs = 40;
  CONFIRM.ms = 120;
  try {
    let runs = 0;
    const btn = h("button", { onclick: (e: Event) => confirmTap(e.currentTarget, "Delete song 3 on the piano?", () => runs++) }, "Delete");
    const row = h("div", { class: "transport" }, btn, h("button", {}, "Other"));
    document.body.replaceChildren(row);
    btn.click();
    assert.equal(runs, 0);
    assert.equal(btn.textContent, "Tap again to confirm");
    assert.equal(btn.dataset.armed, "1");
    const ask = document.querySelector("[data-confirm=ask]")!;
    assert.equal(ask.previousElementSibling, row, "the question sits under the button row");
    assert.equal(ask.getAttribute("role"), "alert");
    assert.match(ask.textContent!, /Delete song 3 on the piano\? Tap the button again to confirm\./);
    btn.click(); // same gesture (within gapMs): still armed, nothing runs
    assert.equal(runs, 0);
    await tick(60);
    btn.click();
    assert.equal(runs, 1);
    assert.equal(btn.textContent, "Delete");
    assert.equal(document.querySelector("[data-confirm=ask]"), null);
    // armed, then left alone: disarms by itself
    btn.click();
    await tick(200);
    assert.equal(btn.textContent, "Delete");
    assert.equal(btn.dataset.armed, undefined);
    assert.equal(document.querySelector("[data-confirm=ask]"), null);
    btn.click(); // a fresh first tap again
    assert.equal(runs, 1);
  } finally {
    CONFIRM.gapMs = gap;
    CONFIRM.ms = ms;
    document.body.replaceChildren();
  }
});
