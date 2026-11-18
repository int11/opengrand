// Information screen box "Panel UI over Bluetooth" against the panel loader behind a relay double.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";
import { runInNewContext } from "node:vm";

import { keyFromPem, loaderSource, makePackage, newKeyPem, uiConfig } from "../scripts/panelpkg.ts";
import { PanelUiClient, type PuiPort } from "../src/protocol/panelui.ts";
import { MemStorage } from "./_puirelay.ts";
import { panelUiBox } from "../src/ui/paneluibox.ts";
import type { AppContext } from "../src/ui/context.ts";

test("panel UI box: Check shows the panel's versions, Roll back answers 'nothing to roll back'", async () => {
  const replies: number[][] = [];
  const ctx: Record<string, unknown> = {
    setTimeout: (f: () => void, ms: number) => setTimeout(f, ms).unref(), clearTimeout,
    location: { search: "", replace() {} },
    kwmcore: { postMessage: (url: string) => url.startsWith("kwmcore://sendMidiMessage/") && replies.push(JSON.parse(url.slice(url.indexOf("?") + 1)).msg) },
  };
  runInNewContext(loaderSource(uiConfig(keyFromPem(newKeyPem()), { version: "0.2.0", seq: 10 })), ctx);
  const K: Record<string, unknown> = {};
  (ctx.CA79UI as { hookKwm(k: unknown): void }).hookKwm(K);
  const listeners: ((m: Uint8Array) => void)[] = [];
  const relay: PuiPort = {
    sendRaw(m) {
      (K.onMidiMessage as (a: number[], id: string) => void)(Array.from(m), "USB");
      for (const r of replies.splice(0)) setTimeout(() => listeners.forEach((l) => l(Uint8Array.from(r))), 1);
    },
    onMidi(fn) {
      listeners.push(fn);
      return () => listeners.splice(listeners.indexOf(fn), 1);
    },
  };
  const box = panelUiBox({ session: () => relay } as unknown as AppContext);
  dom.window.document.body.append(box);
  (box.querySelector("#pui-check") as HTMLButtonElement).click();
  await tick(30);
  assert.match(box.querySelector("#pui-state")!.textContent!, /chooser \/ none · A – · B – · bundled 0\.2\.0/);
  (box.querySelector("#pui-rollback") as HTMLButtonElement).click();
  await tick(30);
  assert.match(box.querySelector("#pui-msg")!.textContent!, /16/, "rc 16: no stored slot to roll back");
  (box.querySelector("#pui-send") as HTMLButtonElement).click();
  await tick(5);
  assert.match(box.querySelector("#pui-msg")!.textContent!, /Pick a package first/);
  box.remove();
});

test("panel UI box: rolling back the version on the screen restarts the panel UI (Control 2, then Control 1)", async () => {
  // A stored version that runs but cannot be seen or used (the dark probe of 11-03)
  const key = keyFromPem(newKeyPem());
  const store = new MemStorage();
  const replies: number[][] = [];
  const nav: string[] = [];
  const ctx: Record<string, unknown> = {
    localStorage: store,
    setTimeout: (f: () => void, ms: number) => setTimeout(f, ms).unref(), clearTimeout,
    location: { search: "?embedded=1", replace() {} },
    kwmcore: { postMessage: (url: string) => url.startsWith("kwmcore://sendMidiMessage/") && replies.push(JSON.parse(url.slice(url.indexOf("?") + 1)).msg) },
    addEventListener() {},
  };
  runInNewContext(loaderSource(uiConfig(key, { version: "0.2.0", seq: 10 })), ctx);
  const ui = ctx.CA79UI as { hookKwm(k: unknown): void; boot(b: { js: string; css: string }): string; navigate: (u: string) => void; state(): { slots: Record<string, { state: string }> } };
  ui.navigate = (u) => nav.push(u);
  const K: Record<string, unknown> = {};
  ui.hookKwm(K);
  const listeners: ((m: Uint8Array) => void)[] = [];
  const relay: PuiPort = {
    sendRaw(m) {
      (K.onMidiMessage as (a: number[], id: string) => void)(Array.from(m), "USB");
      for (const r of replies.splice(0)) setTimeout(() => listeners.forEach((l) => l(Uint8Array.from(r))), 1);
    },
    onMidi(fn) {
      listeners.push(fn);
      return () => listeners.splice(listeners.indexOf(fn), 1);
    },
  };
  const fin = await new PanelUiClient(relay).upload(new Uint8Array(makePackage({ js: "1", css: "" }, { version: "0.2.9", seq: 20, key })), { activate: false });
  assert.equal(fin.phase, "done", fin.message);
  const el = () => ({ appendChild() {} });
  ctx.document = { head: el(), body: el(), createElement: () => ({}) };
  assert.equal(ui.boot({ js: "a.js", css: "a.css" }), "A");
  const box = panelUiBox({ session: () => relay } as unknown as AppContext);
  dom.window.document.body.append(box);
  (box.querySelector("#pui-rollback") as HTMLButtonElement).click();
  await tick(60);
  assert.equal(ui.state().slots.A!.state, "bad");
  assert.match(box.querySelector("#pui-msg")!.textContent!, /restarting into the previous UI/);
  await new Promise((r) => setTimeout(r, 350)); // the loader's Control 1 reloads after 0.3 s
  assert.deepEqual(nav, ["index.html?embedded=1"]);
  box.remove();
});
