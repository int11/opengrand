// The transport runner's loading target (ui/transport.ts): the newest named request waiting, else the running one's;
// listeners hear every change; the marks go when the job ends, fails, or the session is replaced.
import assert from "node:assert/strict";
import { test } from "node:test";
import type { AppContext } from "../src/ui/context.ts";
import { transport } from "../src/ui/transport.ts";

const tick = (ms = 5) => new Promise((r) => setTimeout(r, ms));
function gate() {
  let open!: () => void;
  const p = new Promise<void>((r) => (open = r));
  return { p, open };
}
function fakeCtx(): { ctx: AppContext; set(s: object): void } {
  let s: object = {};
  return { ctx: { session: () => s } as unknown as AppContext, set: (v) => (s = v) };
}

test("target: running, then the newest waiting one; null when over (also after a failure)", async () => {
  const { ctx } = fakeCtx();
  const tp = transport(ctx);
  let changes = 0;
  tp.onChange(() => changes++);
  const a = gate();
  void tp.request(() => a.p, { kind: "usb", name: "A" });
  assert.deepEqual(tp.target(), { kind: "usb", name: "A" });
  const b = tp.request(async () => {}, { kind: "piano", n: 1 });
  assert.deepEqual(tp.target(), { kind: "piano", n: 1 }, "the waiting tap shows at once");
  const c = tp.request(async () => { throw new Error("x"); }, { kind: "usb", name: "C" });
  assert.deepEqual(tp.target(), { kind: "usb", name: "C" }, "a newer one replaces it");
  assert.equal(await b, false);
  assert.ok(changes >= 3);
  a.open();
  assert.equal(await c, true);
  await tick();
  assert.equal(tp.target(), null, "the failed job is over too");
});

test("target: an unnamed request does not hide the named one on the wire", async () => {
  const { ctx } = fakeCtx();
  const tp = transport(ctx);
  const a = gate();
  void tp.request(() => a.p, { kind: "piano", n: 3 });
  void tp.request(async () => {});
  assert.deepEqual(tp.target(), { kind: "piano", n: 3 });
  a.open();
  await tick();
  assert.equal(tp.target(), null);
});

test("target: a new session clears the old runner's mark and tells its listeners", async () => {
  const f = fakeCtx();
  const tp = transport(f.ctx);
  const a = gate();
  let heard = 0;
  tp.onChange(() => heard++);
  void tp.request(() => a.p, { kind: "stop" });
  assert.deepEqual(tp.target(), { kind: "stop" });
  f.set({});
  assert.equal(tp.target(), null, "another session: nothing of the old one loads");
  const before = heard;
  const tp2 = transport(f.ctx);
  assert.ok(heard > before, "the old runner's listeners heard of the change");
  assert.equal(tp2.target(), null);
  a.open();
  await tick();
});
