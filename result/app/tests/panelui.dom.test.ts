// Information screen box "Panel UI over Bluetooth" against the panel loader behind a relay double.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";
import { runInNewContext } from "node:vm";
import { randomBytes } from "node:crypto";

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

test("panel UI box: Roll back is one job - a double tap sends one Control 2, a tap during an upload sends none", async () => {
  // fast repeated taps put the panel and the screen out of step; a second roll back would mark the fallback bad too
  const key = keyFromPem(newKeyPem());
  const replies: number[][] = [];
  const ctx: Record<string, unknown> = {
    localStorage: new MemStorage(),
    setTimeout: (f: () => void, ms: number) => setTimeout(f, ms).unref(), clearTimeout,
    location: { search: "?embedded=1", replace() {} },
    kwmcore: { postMessage: (url: string) => url.startsWith("kwmcore://sendMidiMessage/") && replies.push(JSON.parse(url.slice(url.indexOf("?") + 1)).msg) },
    addEventListener() {},
  };
  runInNewContext(loaderSource(uiConfig(key, { version: "0.2.0", seq: 10 })), ctx);
  const ui = ctx.CA79UI as { hookKwm(k: unknown): void; navigate: (u: string) => void };
  ui.navigate = () => {};
  const K: Record<string, unknown> = {};
  ui.hookKwm(K);
  const listeners: ((m: Uint8Array) => void)[] = [];
  const controls: number[] = [];
  const relay: PuiPort = {
    sendRaw(m) {
      if (m[6] === 0x34) controls.push(m[7]!);
      (K.onMidiMessage as (a: number[], id: string) => void)(Array.from(m), "USB");
      for (const r of replies.splice(0)) setTimeout(() => listeners.forEach((l) => l(Uint8Array.from(r))), 1);
    },
    onMidi(fn) {
      listeners.push(fn);
      return () => listeners.splice(listeners.indexOf(fn), 1);
    },
  };
  const pkg = (seq: number) => new Uint8Array(makePackage({ js: "v" + seq, css: "" }, { version: "0.2." + seq, seq, key }));
  assert.equal((await new PanelUiClient(relay).upload(pkg(20), { activate: false })).phase, "done");
  assert.equal((await new PanelUiClient(relay).upload(pkg(21), { activate: false })).phase, "done");
  const box = panelUiBox({ session: () => relay } as unknown as AppContext);
  dom.window.document.body.append(box);
  const rb = box.querySelector("#pui-rollback") as HTMLButtonElement;
  rb.click();
  rb.click();
  await tick(80);
  assert.deepEqual(controls.filter((c) => c === 2), [2], "one Control 2 for a double tap");
  // during an upload (another screen's Send): refused, nothing sent
  controls.length = 0;
  const up = new PanelUiClient(relay).upload(pkg(22), { activate: false });
  await tick(5);
  rb.click();
  await tick(20);
  assert.deepEqual(controls, [], "no control op during the upload");
  assert.match(box.querySelector("#pui-msg")!.textContent!, /Busy/);
  assert.equal((await up).phase, "done");
  box.remove();
});

/** The real panel loader behind a fake relay; `dropActivate` = the panel never answers Control 1 (activate). */
function loaderRelay(o: { dropActivate?: boolean } = {}) {
  const key = keyFromPem(newKeyPem());
  const replies: number[][] = [];
  const ctx: Record<string, unknown> = {
    localStorage: new MemStorage(),
    setTimeout: (f: () => void, ms: number) => setTimeout(f, ms).unref(), clearTimeout,
    location: { search: "?embedded=1", replace() {} },
    kwmcore: { postMessage: (url: string) => url.startsWith("kwmcore://sendMidiMessage/") && replies.push(JSON.parse(url.slice(url.indexOf("?") + 1)).msg) },
    addEventListener() {},
  };
  runInNewContext(loaderSource(uiConfig(key, { version: "0.2.0", seq: 10 })), ctx);
  const ui = ctx.CA79UI as { hookKwm(k: unknown): void; navigate: (u: string) => void };
  ui.navigate = () => {};
  const K: Record<string, unknown> = {};
  ui.hookKwm(K);
  const listeners: ((m: Uint8Array) => void)[] = [];
  const controls: number[] = [];
  const relay: PuiPort = {
    sendRaw(m) {
      if (m[6] === 0x34) controls.push(m[7]!);
      if (o.dropActivate && m[6] === 0x34 && m[7] === 1) return;
      (K.onMidiMessage as (a: number[], id: string) => void)(Array.from(m), "USB");
      for (const r of replies.splice(0)) setTimeout(() => listeners.forEach((l) => l(Uint8Array.from(r))), 1);
    },
    onMidi(fn) {
      listeners.push(fn);
      return () => listeners.splice(listeners.indexOf(fn), 1);
    },
  };
  // incompressible js: a package of some frames, so the upload takes a while
  const pkg = (seq: number) => new Uint8Array(makePackage({ js: "/*" + randomBytes(6000).toString("base64") + "*/", css: "" }, { version: "0.2." + seq, seq, key }));
  return { relay, pkg, controls };
}

test("panel UI box rebuilt during an upload (tab switch, language, reconnect): it follows the running upload, Send / Roll back off, then the result", async () => {
  const { relay, pkg, controls } = loaderRelay();
  const ctx = { session: () => relay } as unknown as AppContext;
  const up = new PanelUiClient(relay).upload(pkg(30), { activate: false });   // the Send of the screen that was replaced
  const box = panelUiBox(ctx);
  dom.window.document.body.append(box);
  const send = box.querySelector("#pui-send") as HTMLButtonElement;
  const rb = box.querySelector("#pui-rollback") as HTMLButtonElement;
  assert.equal(send.disabled, true, "Send off during the upload");
  assert.equal(rb.disabled, true, "Roll back off during the upload");
  assert.equal((box.querySelector("#pui-progress") as HTMLProgressElement).hidden, false);
  assert.match(box.querySelector("#pui-msg")!.textContent!, /Sending… \d+\/\d+ bytes/);
  rb.click();
  assert.equal((await up).phase, "done");
  await tick(40);
  assert.deepEqual(controls, [], "nothing else sent during the upload");
  assert.match(box.querySelector("#pui-msg")!.textContent!, /Stored in slot A\./);
  assert.equal(send.disabled, false);
  assert.equal(rb.disabled, false);
  box.remove();
});

test("panel UI box: a failed Activate after the upload is reported - not 'the panel is restarting its UI'", async () => {
  const { relay, pkg, controls } = loaderRelay({ dropActivate: true });
  const box = panelUiBox({ session: () => relay } as unknown as AppContext);
  dom.window.document.body.append(box);
  const pick = box.querySelector("#pui-file") as HTMLInputElement;
  const bytes = pkg(40);
  Object.defineProperty(pick, "files", { value: [{ name: "ui.c79u", arrayBuffer: async () => bytes.slice().buffer }] });
  pick.dispatchEvent(new dom.window.Event("change"));
  await tick(30);
  assert.equal((box.querySelector("#pui-activate") as HTMLInputElement).checked, true);
  const send = box.querySelector("#pui-send") as HTMLButtonElement;
  send.click();
  await tick(20);
  for (let i = 0; i < 300 && send.disabled; i++) await tick(20);
  const msg = box.querySelector("#pui-msg")!.textContent!;
  assert.ok(controls.includes(1), "Activate was sent");
  assert.doesNotMatch(msg, /restarting its UI/);
  assert.match(msg, /Stored in slot A, but the panel did not restart its UI/);
  box.remove();
});
