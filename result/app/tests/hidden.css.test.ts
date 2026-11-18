// A rule that sets `display` on an element the app hides with the `hidden` attribute beats the browser's own
// [hidden] { display: none } (same or higher specificity, author origin) - the element stays visible. Field
// 2026-11-15: the header power button (hidden until a piano with the power feature is connected) showed 「대기로」
// while not connected, on the phone and the desktop app.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";

const css = readFileSync(join(import.meta.dirname, "..", "styles.css"), "utf8");

test("the header power button has a [hidden] rule that wins over its display: flex", () => {
  assert.match(css, /header \.powerbtn \{[^}]*display:\s*flex/, "the rule that caused it is still there (else drop this test)");
  assert.match(css, /header \.powerbtn\[hidden\] \{\s*display:\s*none;?\s*\}/);
  assert.ok(css.indexOf("header .powerbtn[hidden]") > css.indexOf("header .powerbtn {"), "after the display rule");
});
