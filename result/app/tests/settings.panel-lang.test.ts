// User decision 2026-11-01: the built-in panel starts in Korean (its WebView locale is not the user's and
// the stock UI has no Korean); once the user picks a language, that choice is kept.
import { test } from "node:test";
import assert from "node:assert/strict";
import { loadSettings } from "../src/state/settings.ts";

function mem(init: Record<string, string> = {}): Storage {
  const m = new Map(Object.entries(init));
  return { getItem: (k) => m.get(k) ?? null, setItem: (k, v) => void m.set(k, v), removeItem: (k) => void m.delete(k), clear: () => m.clear(), key: () => null, get length() { return m.size; } } as Storage;
}

test("panel: Korean by default, a stored choice wins; outside the panel the locale decides", () => {
  const g = globalThis as { kwmcore?: unknown };
  try {
    g.kwmcore = { postMessage() {} };
    assert.equal(loadSettings(mem()).lang, "ko");
    assert.equal(loadSettings(mem({ "ca79ng.settings": JSON.stringify({ lang: "en" }) })).lang, "en");
  } finally {
    delete g.kwmcore;
  }
  assert.notEqual(loadSettings(mem()).lang, undefined);
});
