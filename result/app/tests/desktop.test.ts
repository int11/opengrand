// Desktop app reconnect bridge (src/transport/desktop.ts): the shell runs the page hook with a user
// gesture and answers the chooser; the app only hands it the request.
import { test } from "node:test";
import assert from "node:assert/strict";
import { DESKTOP_RECONNECT_HOOK, desktopReopenBle, desktopShell } from "../src/transport/desktop.ts";

type G = Record<string, unknown>;
const shell = (reconnect: (g: G) => Promise<string>, remembered: string | null = "CA79") => {
  const g: G = {};
  g.ca79Desktop = { shell: "electron", bluetooth: { remembered, reconnect: () => reconnect(g) } };
  return g;
};
// what the main process does: run the hook if the page set one
const runHook = async (g: G) => (typeof g[DESKTOP_RECONNECT_HOOK] === "function" ? ((g[DESKTOP_RECONNECT_HOOK] as () => void)(), "started") : "no-hook");

test("desktopShell: only the Electron preload counts", () => {
  assert.equal(desktopShell({}), null);
  assert.equal(desktopShell({ ca79Desktop: { shell: "other" } }), null);
  assert.ok(desktopShell(shell(runHook)));
});

test("reconnect: the shell runs the hook, the request's link comes back, the hook is gone", async () => {
  const g = shell(runHook);
  let requests = 0;
  const link = await desktopReopenBle(async () => (requests++, "link"), g);
  assert.equal(link, "link");
  assert.equal(requests, 1);
  assert.equal(g[DESKTOP_RECONNECT_HOOK], undefined);
});

test("reconnect: request failure (piano not found) rejects", async () => {
  const g = shell(runHook);
  await assert.rejects(desktopReopenBle(async () => { throw new Error("NotFoundError"); }, g), /NotFoundError/);
});

test("reconnect: shell remembers nothing / not the desktop app → rejects, no request, no hook left", async () => {
  const g = shell(async () => "none", null);
  let requests = 0;
  await assert.rejects(desktopReopenBle(async () => (requests++, "x"), g), /not started: none/);
  assert.equal(requests, 0);
  assert.equal(g[DESKTOP_RECONNECT_HOOK], undefined);
  await assert.rejects(desktopReopenBle(async () => "x", {}), /not the desktop app/);
});
