// The header chips' poll pace (ui/pollpace.ts): slower while the window is shown but not focused (record
// an idle app should send as few frames as it can).
import { test } from "node:test";
import assert from "node:assert/strict";
import { BACKGROUND_FACTOR, pollDelay } from "../src/ui/pollpace.ts";

test("pollDelay: focused = the period, not focused = BACKGROUND_FACTOR times it, no document = the period", () => {
  assert.equal(pollDelay(10000, { hasFocus: () => true }), 10000);
  assert.equal(pollDelay(10000, { hasFocus: () => false }), 10000 * BACKGROUND_FACTOR);
  assert.equal(pollDelay(15000, {}), 15000);
  assert.equal(pollDelay(15000, undefined), 15000);
  assert.ok(BACKGROUND_FACTOR >= 6);
});
