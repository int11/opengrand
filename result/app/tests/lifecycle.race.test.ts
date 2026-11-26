// Connection / session lifecycle races (review 2026-11-27, after the fast-tap field test): two async
// sequences on one MIDI link, a guard checked before an await and set after it, a retired session
// still sending, a promise that never settles.
import { test } from "node:test";
import assert from "node:assert/strict";

import { PianoSession } from "../src/protocol/session.ts";
import { decodeFrame, encodeFrame, toHex } from "../src/protocol/sysex.ts";
import { LoopbackLink } from "../src/transport/link.ts";

const tick = (ms = 20) => new Promise((r) => setTimeout(r, ms));

/** Fake stock piano: CONNECT / identity / dump; counts the dump requests it answered. */
function fakePiano(link: LoopbackLink) {
  const tx = (fn: number, body: number[], dev = 0x00) => link.send(encodeFrame({ dev, fn, group: 0x04, machine: 0x1e, body }));
  const seen: string[] = [];
  link.onMessage((m) => {
    seen.push(toHex(m));
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x01) tx(0x7e, [0x02, 0x00, 0x00]);
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) tx(0x7e, [0x05, 0x00, 0x00, 0x02]);
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x41) tx(0x7e, [0x05, 0x01, 0x00, 0x01]);
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x42) tx(0x7e, [0x05, 0x02, 0x00]);
    if (f.fn === 0x7e && f.body[0] === 0x04 && f.body[1] === 0x41) tx(0x7e, [0x04, 0x01, 0x32]);
    if (f.fn === 0x02 && f.body[0] === 0x5f) {
      tx(0x10, [0x55, 0x00, 0x00, 95, 112, 0]);
      tx(0x03, [0x5f, 0x7f, 0x7f]);
    }
    if (f.fn === 0x02 && f.body[0] === 0x55) tx(0x03, [f.body[0], f.body[1], 0x7f]);
  });
  return seen;
}

test("two overlapping takeControl() on one session: both resolve, one CONNECT + one dump on the wire", async () => {
  // e.g. the file client's recover (fn 0x24 timeout) and the manager's re-bind ("not bound") at once: before, the
  // second dump request took over the first one's end-of-dump handler and the first timed out / re-CONNECTed
  const [app, piano] = LoopbackLink.pair();
  const seen = fakePiano(piano);
  const s = new PianoSession(app);
  await s.connect(200);
  seen.length = 0;
  const r = await Promise.allSettled([s.takeControl(200), s.takeControl(200)]);
  assert.deepEqual(r.map((x) => x.status), ["fulfilled", "fulfilled"], String(r.map((x) => (x as PromiseRejectedResult).reason?.message)));
  assert.equal(s.state, "ready");
  assert.equal(seen.filter((x) => x === "F0 40 7F 7E 08 02 01 01 F7").length, 1, seen.join("\n"));
  assert.equal(seen.filter((x) => x === "F0 40 00 02 08 02 5F 7F 7F F7").length, 1, seen.join("\n"));
});

test("a retired session sends no data request on the link the next session uses", async () => {
  // a screen that still holds the old session (released when a new one took the same port) asks for a value:
  // before, requestData went out on the shared link and waited out its whole timeout
  const [app, piano] = LoopbackLink.pair();
  const seen = fakePiano(piano);
  const old = new PianoSession(app);
  await old.connect(200);
  old.release();
  seen.length = 0;
  const t0 = Date.now();
  assert.equal(await old.requestData(0x55, 0x00, 300), false);
  await tick();
  assert.deepEqual(seen, []);
  assert.ok(Date.now() - t0 < 200, "answers at once");
});

const { ConnectionManager } = await import("../src/state/connection.ts");
type MidiLink = import("../src/transport/link.ts").MidiLink;

test("another piano whose open failed: Try again / picking it again never connects to the piano before under its name", async () => {
  // connected to A, 「Connect another device…」 → B, B does not open (failed, banner names B). Before, 「Try again」
  // and a re-pick of B from the list started a session on A's link — header B, every control went to A.
  const [a, pa] = LoopbackLink.pair();
  fakePiano(pa);
  const made: MidiLink[] = [];
  let pickers = 0;
  const m = new ConnectionManager({ makeSession: (l) => (made.push(l), new PianoSession(l)), replyMs: 100, openPicker: () => pickers++ });
  assert.equal(await m.attach(a), "ready");
  let opens = 0;
  const B = { key: "ble:B", name: "B", open: async (): Promise<MidiLink> => { opens++; throw new Error("B is off"); } };
  assert.equal(await m.choose(B), "failed");
  assert.equal(m.state.key, "ble:B");
  m.retry();
  await tick(5);
  assert.equal(m.phase, "failed");
  assert.equal(m.state.error, "nolink", "B never opened: nothing to reopen, choose it again (as a websocket that never opened)");
  assert.equal(pickers, 1);
  assert.equal(opens, 1);
  assert.equal(await m.choose(B), "failed");
  assert.equal(opens, 2, "picking B again opens B");
  assert.equal(made.length, 1, "no second session on A's link");
  m.disconnect();
});

test("a link that opens after the app let the piano go (suspend / disconnect) is closed, not left holding it", async () => {
  // the open was under way when the page went to the background: before, the late link stayed open with no session —
  // a BLE piano's one connection held by a background tab, which is what the handoff is for
  for (const leave of ["suspend", "disconnect"] as const) {
    const [l] = LoopbackLink.pair();
    let closed = 0;
    l.onClose(() => closed++);
    let open!: () => void;
    const m = new ConnectionManager({ makeSession: (x) => new PianoSession(x), replyMs: 60 });
    void m.choose({ key: "ble:late", name: "CA79", open: () => new Promise<MidiLink>((r) => (open = () => r(l))) });
    await tick(1);
    assert.equal(m.phase, "connecting");
    m[leave]();
    assert.equal(m.phase, "idle");
    open();
    await tick(5);
    assert.equal(closed, 1, leave);
    assert.equal(m.currentLink, null);
  }
});

const { connectBleDevice } = await import("../src/transport/ble.ts");
const { encodeBlePackets } = await import("../src/protocol/blemidi.ts");
type GattChar = import("../src/transport/ble.ts").GattChar;

test("BLE: an open that finishes after a newer open of the same piano does not deafen the newer link", async () => {
  // desktop auto-reconnect: a gatt.connect() hangs, the watchdog gives up, the next attempt opens; then the hung one
  // comes back. Before, it built a second link on the characteristic, which took the notifications from the link the
  // session runs on — the session heard nothing more.
  const listeners = new Set<(e: Event) => void>();
  const ch: GattChar = {
    value: undefined,
    startNotifications: async () => undefined,
    writeValueWithoutResponse: async () => undefined,
    addEventListener: (_t, cb) => void listeners.add(cb),
    removeEventListener: (_t, cb) => void listeners.delete(cb),
  };
  const server = { getPrimaryService: async () => ({ getCharacteristic: async () => ch }), disconnect: () => {} };
  const pending: (() => void)[] = [];
  const dev = {
    id: "race1",
    name: "CA79",
    gatt: { connect: () => new Promise<typeof server>((r) => pending.push(() => r(server))) },
    addEventListener: () => {},
    removeEventListener: () => {},
  };
  const stale = connectBleDevice(dev);
  const fresh = connectBleDevice(dev);
  await tick(1);
  pending[1]!(); // the newer attempt opens first
  const live = await fresh;
  const got: string[] = [];
  live.onMessage((m) => got.push(toHex(m)));
  pending[0]!(); // then the hung one comes back
  await stale;
  for (const p of encodeBlePackets([Uint8Array.from([0x90, 0x3c, 0x40])], 0, 23)) {
    ch.value = new DataView(p.buffer, p.byteOffset, p.byteLength);
    for (const l of [...listeners]) l(new Event("characteristicvaluechanged"));
  }
  assert.deepEqual(got, ["90 3C 40"]);
  live.close();
});

const { DESKTOP_RECONNECT_HOOK, desktopReopenBle } = await import("../src/transport/desktop.ts");

test("desktop reconnect: a second reconnect while the first hook was never run settles the first", async () => {
  // the shell said "started" but did not run the hook (no window focus …); the next reopen sets its own hook. Before,
  // the first promise was left pending for ever (the start that awaited it never ended).
  const g: Record<string, unknown> = {};
  let calls = 0;
  g.ca79Desktop = { shell: "electron", bluetooth: { remembered: "CA79", reconnect: async () => (calls++ === 0 ? "started" : ((g[DESKTOP_RECONNECT_HOOK] as () => void)(), "started")) } };
  const first = desktopReopenBle(async () => "old", g);
  await tick(1);
  const second = desktopReopenBle(async () => "new", g);
  assert.equal(await second, "new");
  const r = await Promise.race([first.then(() => "resolved", () => "rejected"), tick(50).then(() => "pending")]);
  assert.equal(r, "rejected");
});

const { createContext } = await import("../src/ui/context.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { PARAMS } = await import("../src/protocol/params.gen.ts");

/** A session on a loopback whose piano ACKs / NACKs each fn 0x10 as `answer` says, and can send 7E 01 00. */
async function storeOnPiano(answer: (data: number[]) => "ack" | "nack" | "none") {
  const [a, pa] = LoopbackLink.pair();
  fakePiano(pa);
  const tx = (fn: number, body: number[]) => pa.send(encodeFrame({ dev: 0, fn, group: 0x04, machine: 0x1e, body }));
  const held: (() => void)[] = [];
  pa.onMessage((m) => {
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x01) tx(0x7e, [0x02, 0x00, 0x00]);
    else if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) tx(0x7e, [0x05, 0x00, 0x00, 0x02]);
    else if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x41) tx(0x7e, [0x05, 0x01, 0x00, 0x01]);
    else if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x42) tx(0x7e, [0x05, 0x02, 0x00]);
    else if (f.fn === 0x7e && f.body[0] === 0x04 && f.body[1] === 0x41) tx(0x7e, [0x04, 0x01, 0x32]);
    else if (f.fn === 0x02 && f.body[0] === 0x5f) tx(0x03, [0x5f, 0x7f, 0x7f]);
    else if (f.fn === 0x10) {
      const how = answer(f.body.slice(3));
      const reply = () => tx(how === "ack" ? 0x07 : 0x06, [f.body[0], f.body[1], f.body[2]]);
      if (how !== "none") held.push(reply);
    }
  });
  const s = new PianoSession(a, { ackTimeoutMs: 40, maxRetries: 1 });
  await s.connect(60);
  const store = new ParamStore();
  store.attach(s);
  return { s, store, tx, flush: () => held.splice(0).forEach((r) => r()) };
}

const TEMPO = PARAMS.find((p) => p.name === "MetronomeTempo")!;

test("store: a dropped set behind a newer one does not roll the newer value back", async () => {
  // fast taps: 2 then 3; the piano refuses 2 and takes 3. Before, 2's rollback put back the value from before both
  // taps after 3 was shown, and the screen kept it while the piano had 3.
  const p = await storeOnPiano((d) => (d[1] === 2 ? "nack" : "ack"));
  p.tx(0x10, [TEMPO.v1, TEMPO.v2, 0x7f, 0, 1]); // the piano's value: 1
  await tick(5);
  const a = p.store.set(TEMPO, 0x7f, [0, 2]);
  const b = p.store.set(TEMPO, 0x7f, [0, 3]);
  await tick(5);
  p.flush(); // NACK 2
  await tick(5);
  p.flush(); // ACK 3
  assert.deepEqual([await a, await b], [false, true]);
  assert.deepEqual(p.store.get(TEMPO, 0x7f), [0, 3]);
});

test("store: both sets dropped by a displacement end on the piano's value, not on the first tap", async () => {
  const p = await storeOnPiano(() => "none");
  p.tx(0x10, [TEMPO.v1, TEMPO.v2, 0x7f, 0, 1]);
  await tick(5);
  const a = p.store.set(TEMPO, 0x7f, [0, 2]);
  const b = p.store.set(TEMPO, 0x7f, [0, 3]);
  await tick(5);
  p.tx(0x7e, [0x01, 0x00]); // 7E 01 00: another device took control, both dropped
  assert.deepEqual([await a, await b], [false, false]);
  assert.deepEqual(p.store.get(TEMPO, 0x7f), [0, 1]);
});

test("file channel recover: the CONNECT + re-sync runs through the connection manager, a failed one is not left as Connected", async () => {
  // fn 0x24 timeout → recover. Before, the context called session.takeControl() behind the manager's back: when the
  // piano did not answer, the session stayed "connecting" (every screen that needs "ready" went blank) while the
  // header kept saying Connected, and nothing offered 「Try again」.
  const [a, pa] = LoopbackLink.pair();
  let mute = false;
  const seen = fakePiano(pa);
  const real = pa.send.bind(pa);
  pa.send = (m) => (mute ? undefined : real(m));
  const m = new ConnectionManager({ makeSession: (l) => new PianoSession(l), replyMs: 60 });
  assert.equal(await m.attach(a), "ready");
  const s = m.session!;
  const ctx = createContext(new ParamStore(), { session: () => m.session, retake: (x) => m.rejoin(x) });
  const fa = ctx.files()!;
  mute = true;
  seen.length = 0;
  await (fa as unknown as { opt: { recover(): Promise<unknown> } }).opt.recover().catch(() => undefined); // as FileAccessClient does
  assert.ok(seen.includes("F0 40 7F 7E 08 02 01 01 F7"), "a CONNECT went out");
  assert.equal(m.phase, "failed");
  assert.equal(s.state, "closed", "the session the screens held is retired, not left connecting");
  m.disconnect();
});

test("rejoin of a session that is not the manager's current one does nothing", async () => {
  const [a, pa] = LoopbackLink.pair();
  fakePiano(pa);
  const m = new ConnectionManager({ makeSession: (l) => new PianoSession(l), replyMs: 60 });
  assert.equal(await m.attach(a), "ready");
  const other = new PianoSession(new LoopbackLink());
  assert.equal(await m.rejoin(other), "ready");
  assert.equal(m.phase, "ready");
  m.disconnect();
});

const { connView, watchConnects } = await import("../src/state/connection.ts");

/** A manager on a fake piano that can be muted, with main.ts's on-connect hook counted and the header texts kept. */
async function rejoinRig() {
  const [a, pa] = LoopbackLink.pair();
  let mute = false;
  const seen = fakePiano(pa);
  const real = pa.send.bind(pa);
  pa.send = (m) => (mute ? undefined : real(m));
  let wakes = 0;
  const m = new ConnectionManager({ makeSession: (l) => new PianoSession(l), replyMs: 60, wake: () => (wakes++, null) });
  const n = { connected: 0, rejoined: 0 };
  watchConnects(m, { connected: () => n.connected++, rejoined: () => n.rejoined++ });
  const texts: string[] = [];
  m.onChange((x) => texts.push(String(connView(x).status[0])));
  assert.equal(await m.attach(a), "ready");
  const ctx = createContext(new ParamStore(), { session: () => m.session, retake: (x) => m.rejoin(x) });
  const recover = () => (ctx.files() as unknown as { opt: { recover(): Promise<unknown> } }).opt.recover(); // as FileAccessClient does
  const displace = () => real(encodeFrame({ dev: 0, fn: 0x7e, group: 0x04, machine: 0x1e, body: [0x01, 0x00] }));
  return { m, n, texts, seen, recover, displace, mute: (on: boolean) => { mute = on; }, wakes: () => wakes };
}

test("file channel recover on the same session: main.ts's on-connect hook does not run again, the header does not say Reconnecting", async () => {
  // before: rejoin went retaking → ready like a new connection, so main.ts redrew the current tab (the recorder's
  // "control re-taken; try again" line and any half-done UI gone), re-ran usbPlayback / the loader confirm, and the
  // header said "Reconnecting to CA79…" while the link never went anywhere
  const r = await rejoinRig();
  assert.equal(r.n.connected, 1);
  const s = r.m.session;
  r.texts.length = 0;
  r.seen.length = 0;
  assert.equal(await r.recover(), "ready");
  assert.ok(r.seen.includes("F0 40 7F 7E 08 02 01 01 F7"), "a CONNECT went out");
  assert.equal(r.m.session, s, "same session");
  assert.deepEqual([r.n.connected, r.n.rejoined], [1, 1]);
  assert.ok(!r.texts.includes("Reconnecting to {name}…"), r.texts.join(" | "));
  assert.ok(r.texts.includes("Syncing with {name}…"), r.texts.join(" | "));
  assert.equal(r.wakes(), 1, "open-to-wake once per session");
  r.m.disconnect();
});

test("a file channel recover that fails, then Try again: the on-connect hook runs for the new session", async () => {
  const r = await rejoinRig();
  r.mute(true);
  await r.recover().catch(() => undefined);
  assert.equal(r.m.phase, "failed");
  r.mute(false);
  r.m.retry();
  assert.equal(await r.m.settled(), "ready");
  assert.deepEqual([r.n.connected, r.n.rejoined], [2, 0]);
  r.m.disconnect();
});

test("a file channel recover cut by a displacement, then Take control again: the on-connect hook runs", async () => {
  const r = await rejoinRig();
  r.mute(true);
  const p = r.recover();
  await tick(5);
  r.displace();
  assert.equal(await p, "displaced");
  await tick(5); // the CONNECT the displacement cut ends first (a retake in the same tick would join it)
  r.mute(false);
  r.m.retake();
  assert.equal(await r.m.settled(), "ready");
  assert.deepEqual([r.n.connected, r.n.rejoined], [2, 0]);
  r.m.disconnect();
});

test("Reconnect from the connected menu still counts as a connect (the user asked for it)", async () => {
  const r = await rejoinRig();
  r.texts.length = 0;
  r.m.retry();
  assert.equal(await r.m.settled(), "ready");
  assert.deepEqual([r.n.connected, r.n.rejoined], [2, 0]);
  assert.ok(r.texts.includes("Reconnecting to {name}…"), r.texts.join(" | "));
  r.m.disconnect();
});
