// Connection state machine (queue conn).
// 1. the transition table, row by row (pure reducer);
// 2. the three field symptoms of 2026-11-01 on a fake stock piano with a panel port — first the old
//    flow (to show the fake reproduces them), then the manager;
// 3. transports: the same link reused (Web Bluetooth, Android shell), shell lifecycle, suspend / resume.
import { test } from "node:test";
import assert from "node:assert/strict";

const { INITIAL, reduce, connView, ConnectionManager, TIMED } = await import("../src/state/connection.ts");
type ConnState = import("../src/state/connection.ts").ConnState;
type ConnEvent = import("../src/state/connection.ts").ConnEvent;
type Phase = import("../src/state/connection.ts").Phase;
const { PianoSession } = await import("../src/protocol/session.ts");
const { fromHex, toHex, decodeFrame } = await import("../src/protocol/sysex.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
type MidiLink = import("../src/transport/link.ts").MidiLink;
const { connectBleDevice } = await import("../src/transport/ble.ts");
const android = await import("../src/transport/android.ts");

const tick = (ms = 20) => new Promise((r) => setTimeout(r, ms));
const CONNECT = "F0 40 7F 7E 08 02 01 01 F7";
const DISCONNECT = "F0 40 7F 7E 08 02 01 00 F7";

// ---------------------------------------------------------------- 1. transition table

const at = (phase: Phase, patch: Partial<ConnState> = {}): ConnState => ({ ...INITIAL, phase, gen: 3, seq: 10, key: "ble:A", link: "CA79", ...patch });
const G = 3; // the current session's generation

// [from, event, to, effects (types, arm/disarm left out)]
const TABLE: [ConnState, ConnEvent, Phase, string[]][] = [
  // header button
  [at("idle"), { type: "connectButton" }, "idle", ["picker"]],
  [at("lost"), { type: "connectButton" }, "connecting", ["start:reopen"]], // the piano just lost first (field 2026-11-03)
  [at("failed"), { type: "connectButton" }, "failed", ["picker"]],
  [at("ready"), { type: "connectButton" }, "ready", ["menu"]],
  [at("waking"), { type: "connectButton" }, "waking", ["menu"]],
  [at("displaced"), { type: "connectButton" }, "displaced", ["menu"]],
  [at("connecting"), { type: "connectButton" }, "connecting", []],
  [at("syncing"), { type: "connectButton" }, "syncing", []],
  [at("retaking"), { type: "connectButton" }, "retaking", []],
  // pick: same piano is idempotent, another one starts a new session
  [at("ready"), { type: "pick", key: "ble:A", link: "CA79" }, "ready", []],
  [at("connecting"), { type: "pick", key: "ble:A", link: "CA79" }, "connecting", []],
  [at("syncing"), { type: "pick", key: "ble:A", link: "CA79" }, "syncing", []],
  [at("waking"), { type: "pick", key: "ble:A", link: "CA79" }, "waking", []],
  [at("retaking"), { type: "pick", key: "ble:A", link: "CA79" }, "retaking", []],
  [at("displaced"), { type: "pick", key: "ble:A", link: "CA79" }, "retaking", ["retake"]],
  [at("failed"), { type: "pick", key: "ble:A", link: "CA79" }, "connecting", ["start:current"]],
  [at("lost"), { type: "pick", key: "ble:A", link: "CA79" }, "connecting", ["start:choice"]],
  [at("idle", { key: null }), { type: "pick", key: "ble:A", link: "CA79" }, "connecting", ["start:choice"]],
  [at("ready"), { type: "pick", key: "android:usb:1", link: "USB" }, "connecting", ["start:choice"]],
  // user disconnect, app away / back
  [at("ready"), { type: "disconnect" }, "idle", ["disconnect"]],
  [at("displaced"), { type: "disconnect" }, "idle", ["disconnect"]],
  [at("idle", { key: null }), { type: "disconnect" }, "idle", []],
  [at("ready"), { type: "suspend" }, "idle", ["disconnect"]],
  [at("syncing"), { type: "suspend" }, "idle", ["disconnect"]],
  [at("failed"), { type: "suspend" }, "idle", ["disconnect"]],
  [at("lost"), { type: "suspend" }, "lost", []],
  [at("idle"), { type: "suspend" }, "idle", []],
  [at("idle", { suspended: true }), { type: "resume" }, "connecting", ["start:reopen"]],
  [at("idle"), { type: "resume" }, "idle", []],
  [at("ready"), { type: "resume" }, "ready", []],
  // banner buttons
  [at("displaced"), { type: "retake" }, "retaking", ["retake"]],
  [at("ready"), { type: "retake" }, "ready", []],
  [at("failed"), { type: "retry" }, "connecting", ["start:current"]],
  [at("lost"), { type: "retry" }, "connecting", ["start:reopen"]],
  [at("idle", { suspended: true }), { type: "retry" }, "connecting", ["start:reopen"]],
  [at("idle", { key: null }), { type: "retry" }, "idle", ["picker"]],
  // "Reconnect" while connected: a real CONNECT handshake on the same session (field 2026-11-09: it sent nothing)
  [at("ready"), { type: "retry" }, "retaking", ["retake"]],
  [at("waking"), { type: "retry" }, "retaking", ["retake"]],
  [at("displaced"), { type: "retry" }, "retaking", ["retake"]],
  ...(["connecting", "syncing", "retaking"] as Phase[]).map((p): [ConnState, ConnEvent, Phase, string[]] => [at(p), { type: "retry" }, p, []]),
  // the piano may have dropped our binding (field 2026-11-09, apply now): CONNECT + re-sync once
  ...(["apply", "identity", "notBound"] as const).flatMap((why): [ConnState, ConnEvent, Phase, string[]][] => [
    [at("ready"), { type: "rebind", gen: G, why }, "retaking", ["retake"]],
    [at("waking"), { type: "rebind", gen: G, why }, "retaking", ["retake"]],
    ...(["connecting", "syncing", "retaking", "displaced", "idle", "lost", "failed"] as Phase[]).map((p): [ConnState, ConnEvent, Phase, string[]] => [at(p), { type: "rebind", gen: G, why }, p, []]),
    [at("ready"), { type: "rebind", gen: G - 1, why }, "ready", []],
  ]),
  [at("ready", { rebound: true }), { type: "rebind", gen: G, why: "notBound" }, "ready", []], // once
  [at("ready", { rebound: true }), { type: "rebind", gen: G, why: "apply" }, "retaking", ["retake"]], // each swap is new
  [at("ready", { rebound: true }), { type: "rebind", gen: G, why: "identity" }, "retaking", ["retake"]],
  // session events
  [at("connecting"), { type: "identified", gen: G }, "syncing", []],
  [at("retaking"), { type: "identified", gen: G }, "retaking", []],
  [at("ready"), { type: "identified", gen: G }, "ready", []],
  [at("connecting"), { type: "synced", gen: G, model: "CA79", version: "1" }, "ready", []],
  [at("syncing"), { type: "synced", gen: G, model: "CA79", version: "1" }, "ready", []],
  [at("retaking"), { type: "synced", gen: G, model: "CA79", version: "1" }, "ready", []],
  [at("displaced"), { type: "synced", gen: G, model: "CA79", version: "1" }, "displaced", []],
  ...(["connecting", "syncing", "ready", "waking", "retaking"] as Phase[]).map((p): [ConnState, ConnEvent, Phase, string[]] => [at(p), { type: "displaced", gen: G }, "displaced", []]),
  [at("idle"), { type: "displaced", gen: G }, "idle", []],
  // 7E 01 01 from the piano (stock: to the panel when the bound controller left; OpenGrand: after a live payload swap)
  ...(["ready", "waking", "displaced"] as Phase[]).map((p): [ConnState, ConnEvent, Phase, string[]] => [at(p), { type: "invited", gen: G }, "retaking", ["retake"]]),
  ...(["connecting", "syncing", "retaking", "idle", "lost", "failed"] as Phase[]).map((p): [ConnState, ConnEvent, Phase, string[]] => [at(p), { type: "invited", gen: G }, p, []]),
  [at("ready"), { type: "invited", gen: G - 1 }, "ready", []],
  ...(["connecting", "syncing", "ready", "waking", "displaced", "retaking", "failed"] as Phase[]).map((p): [ConnState, ConnEvent, Phase, string[]] => [at(p), { type: "closed", gen: G }, "lost", []]),
  [at("idle"), { type: "closed", gen: G }, "idle", []],
  ...(["connecting", "syncing", "retaking"] as Phase[]).map((p): [ConnState, ConnEvent, Phase, string[]] => [at(p), { type: "failed", gen: G, error: "x" }, "failed", ["release"]]),
  [at("ready"), { type: "failed", gen: G, error: "x" }, "ready", []],
  [at("displaced"), { type: "failed", gen: G, error: "x" }, "displaced", []],
  [at("ready"), { type: "wakeStart", gen: G }, "waking", []],
  [at("displaced"), { type: "wakeStart", gen: G }, "displaced", []],
  [at("waking"), { type: "wakeDone", gen: G }, "ready", []],
  [at("ready"), { type: "wakeDone", gen: G }, "ready", []],
  // watchdog: the current seq only
  ...(["connecting", "syncing", "retaking"] as Phase[]).map((p): [ConnState, ConnEvent, Phase, string[]] => [at(p), { type: "timeout", seq: 10 }, "failed", ["release"]]),
  [at("waking"), { type: "timeout", seq: 10 }, "ready", []],
  [at("connecting"), { type: "timeout", seq: 9 }, "connecting", []],
  [at("ready"), { type: "timeout", seq: 10 }, "ready", []],
  // an older session's events are never shown (self-displacement guard)
  [at("ready"), { type: "displaced", gen: G - 1 }, "ready", []],
  [at("ready"), { type: "closed", gen: G - 1 }, "ready", []],
  [at("connecting"), { type: "failed", gen: G - 1, error: "x" }, "connecting", []],
  [at("connecting"), { type: "synced", gen: G + 1, model: "", version: "" }, "connecting", []],
  // A3 boot re-grab: a start opens the window (only when enabled), the first displacement in it is taken back once
  [at("idle", { key: null, bootRetake: true }), { type: "pick", key: "ble:A", link: "CA79" }, "connecting", ["start:choice", "grace"]],
  [at("lost", { bootRetake: true }), { type: "retry" }, "connecting", ["start:reopen", "grace"]],
  [at("idle", { suspended: true, bootRetake: true }), { type: "resume" }, "connecting", ["start:reopen", "grace"]],
  [at("ready", { grace: true }), { type: "displaced", gen: G }, "displaced", ["autoArm"]],
  [at("connecting", { grace: true }), { type: "displaced", gen: G }, "displaced", ["autoArm"]],
  [at("ready", { grace: true }), { type: "displaced", gen: G - 1 }, "ready", []],
  [at("ready", { grace: true }), { type: "graceEnd", gen: G }, "ready", []],
  [at("displaced", { autoRetake: true }), { type: "autoRetake", gen: G }, "retaking", ["retake"]],
  [at("displaced"), { type: "autoRetake", gen: G }, "displaced", []],
  [at("displaced", { autoRetake: true }), { type: "autoRetake", gen: G - 1 }, "displaced", []],
  [at("ready", { autoRetake: true }), { type: "autoRetake", gen: G }, "ready", []],
  // auto-reconnect (desktop app, field 2026-11-03): lost schedules the next reopen; a failed reopen goes back to lost
  ...(["connecting", "syncing", "ready", "waking", "displaced", "retaking", "failed"] as Phase[]).map((p): [ConnState, ConnEvent, Phase, string[]] => [at(p, { autoReconnect: true }), { type: "closed", gen: G }, "lost", ["reconnectArm"]]),
  [at("ready", { autoReconnect: true, key: null }), { type: "closed", gen: G }, "lost", []],
  [at("lost", { autoReconnect: true }), { type: "reconnectTick", seq: 10 }, "connecting", ["start:reopen"]],
  [at("lost", { autoReconnect: true }), { type: "reconnectTick", seq: 9 }, "lost", []],
  [at("idle", { autoReconnect: true }), { type: "reconnectTick", seq: 10 }, "idle", []],
  [at("ready", { autoReconnect: true }), { type: "reconnectTick", seq: 10 }, "ready", []],
  [at("lost", { autoReconnect: true }), { type: "retry" }, "connecting", ["start:reopen"]],
  [at("lost", { autoReconnect: true }), { type: "connectButton" }, "connecting", ["start:reopen"]],
  [at("lost", { autoReconnect: true }), { type: "suspend" }, "lost", []],
  [at("lost", { autoReconnect: true }), { type: "disconnect" }, "idle", ["disconnect"]],
  ...(["connecting", "syncing", "retaking"] as Phase[]).map((p): [ConnState, ConnEvent, Phase, string[]] => [at(p, { autoReconnect: true, reopening: true }), { type: "failed", gen: G, error: "x" }, "lost", ["release", "reconnectArm"]]),
  ...(["connecting", "syncing", "retaking"] as Phase[]).map((p): [ConnState, ConnEvent, Phase, string[]] => [at(p, { autoReconnect: true, reopening: true }), { type: "timeout", seq: 10 }, "lost", ["release", "reconnectArm"]]),
  [at("connecting", { autoReconnect: true }), { type: "failed", gen: G, error: "x" }, "failed", ["release"]], // a user's pick is not a reopen
  [at("connecting", { autoReconnect: true, reopening: true }), { type: "synced", gen: G, model: "CA79", version: "1" }, "ready", []],
];

test(`transition table: ${TABLE.length} rows`, () => {
  for (const [from, ev, to, fx] of TABLE) {
    const { state, effects } = reduce(from, ev);
    const what = `${from.phase}${from.suspended ? "(suspended)" : ""} + ${JSON.stringify(ev)}`;
    assert.equal(state.phase, to, what);
    const names = effects.filter((e) => e.type !== "arm" && e.type !== "disarm").map((e) => (e.type === "start" ? `start:${e.source}` : e.type));
    assert.deepEqual(names, fx, what);
    // a phase change arms the watchdog of a timed phase and disarms it otherwise; no change, no timer effect
    const timer = effects.filter((e) => e.type === "arm" || e.type === "disarm");
    if (state.seq === from.seq) assert.deepEqual(timer, [], what);
    else assert.deepEqual(timer.map((e) => e.type), [TIMED.includes(to) ? "arm" : "disarm"], what);
    if (effects.some((e) => e.type === "start")) assert.equal(state.gen, from.gen + 1, `${what}: new session generation`);
  }
});

test("re-bind flags: once per session for 'not bound'; a new session or a manual Reconnect re-arms it; the header says Reconnecting", () => {
  const r = reduce(at("ready"), { type: "rebind", gen: G, why: "notBound" }).state;
  assert.deepEqual([r.rebound, r.rejoin], [true, true]);
  assert.deepEqual(connView(r).status, ["Reconnecting to {name}…", { name: "CA79" }]);
  assert.equal(connView(r).banner, null, "not a takeover: no banner");
  assert.equal(connView(r).button.action, "none");
  const id = reduce(r, { type: "identified", gen: G }).state;
  assert.equal(id.rejoin, true, "the re-arm keeps the header text");
  const back = reduce(id, { type: "synced", gen: G, model: "CA79", version: "1" }).state;
  assert.deepEqual([back.phase, back.rebound, back.rejoin], ["ready", true, false]);
  assert.equal(reduce(back, { type: "rebind", gen: G, why: "notBound" }).state.phase, "ready", "second 'not bound': left alone");
  const man = reduce(back, { type: "retry" }).state;
  assert.deepEqual([man.phase, man.rebound, man.rejoin], ["retaking", false, true]);
  const fresh = reduce(back, { type: "pick", key: "ble:B", link: "B" }).state;
  assert.equal(fresh.rebound, false, "a new session starts unguarded");
  // a displacement retake is not a rejoin: the banner still says Taking control
  assert.deepEqual(connView(reduce(at("displaced"), { type: "retake" }).state).status, ["Taking control…"]);
});

test("A3 boot re-grab flags: the window closes on use and on graceEnd; a manual retake or a lost link cancels a pending re-grab", () => {
  assert.equal(reduce(at("ready", { grace: true }), { type: "graceEnd", gen: G }).state.grace, false);
  const d = reduce(at("ready", { grace: true }), { type: "displaced", gen: G }).state;
  assert.deepEqual([d.grace, d.autoRetake], [false, true]);
  assert.equal(reduce(d, { type: "displaced", gen: G }).state.autoRetake, true, "already displaced: nothing changes");
  assert.equal(reduce(d, { type: "retake" }).state.autoRetake, false);
  assert.equal(reduce(d, { type: "closed", gen: G }).state.autoRetake, false);
  // without bootRetake (default, and the panel builds) a start opens no window
  const s = reduce(at("idle", { key: null }), { type: "pick", key: "ble:A", link: "CA79" }).state;
  assert.equal(s.grace, false);
  assert.equal(reduce({ ...s, phase: "ready" }, { type: "displaced", gen: s.gen }).state.autoRetake, false);
  // the banner says what is about to happen
  assert.deepEqual(connView(d).banner?.msg, ["The panel took control while starting up — taking it back in a moment…"]);
  assert.equal(connView(d).banner?.button?.action, "retake", "the user can still take it back at once");
});

test("auto-reconnect: attempts count up for the backoff, a sync resets them; the banner says it is retrying", () => {
  const lost = reduce(at("ready", { autoReconnect: true }), { type: "closed", gen: G });
  assert.deepEqual(lost.effects.find((e) => e.type === "reconnectArm"), { type: "reconnectArm", seq: lost.state.seq, attempt: 0 });
  const a1 = reduce(lost.state, { type: "reconnectTick", seq: lost.state.seq }).state;
  assert.deepEqual([a1.reopening, a1.reconnects], [true, 1]);
  const back = reduce(a1, { type: "failed", gen: a1.gen, error: "Connection attempt failed" });
  assert.equal(back.state.phase, "lost");
  assert.deepEqual(back.effects.find((e) => e.type === "reconnectArm"), { type: "reconnectArm", seq: back.state.seq, attempt: 1 });
  // a manual reopen keeps the count (the button does not restart the backoff) and also comes back to lost
  const man = reduce(back.state, { type: "retry" }).state;
  assert.deepEqual([man.reopening, man.reconnects], [true, 1]);
  const ok = reduce(reduce(a1, { type: "identified", gen: a1.gen }).state, { type: "synced", gen: a1.gen, model: "CA79", version: "1" }).state;
  assert.deepEqual([ok.phase, ok.reopening, ok.reconnects], ["ready", false, 0]);
  // without the option nothing is scheduled and a failed reopen is a plain failure
  const off = reduce(at("ready"), { type: "closed", gen: G });
  assert.ok(!off.effects.some((e) => e.type === "reconnectArm"));
  const offTry = reduce(off.state, { type: "retry" }).state;
  assert.equal(reduce(offTry, { type: "failed", gen: offTry.gen, error: "x" }).state.phase, "failed");
  // view: lost and an automatic attempt show the same banner (no flicker between attempts)
  assert.deepEqual(connView(lost.state).banner?.msg, ["The connection to {name} was lost — reconnecting automatically…", { name: "CA79" }]);
  assert.deepEqual(connView(a1).banner?.button, { label: "Reconnect", action: "retry", disabled: true });
  assert.equal(connView(at("connecting", { autoReconnect: true })).banner, null, "a user's connect has no banner");
  assert.deepEqual(connView(off.state).banner?.msg, ["The connection to {name} was lost.", { name: "CA79" }]);
});

test("view: every phase has a status line; busy phases disable the Connect button; banner per phase", () => {
  const phases: Phase[] = ["idle", "connecting", "syncing", "ready", "waking", "displaced", "retaking", "lost", "failed"];
  const rows = phases.map((p) => {
    const v = connView(at(p, { model: "CA79", version: "2.0" }));
    return [p, v.button.action, v.banner?.button?.action ?? "-", v.banner?.button?.disabled ?? "-"];
  });
  assert.deepEqual(rows, [
    ["idle", "pick", "-", "-"],
    ["connecting", "none", "-", "-"],
    ["syncing", "none", "-", "-"],
    ["ready", "menu", "-", "-"],
    ["waking", "menu", "-", "-"],
    ["displaced", "menu", "retake", false],
    ["retaking", "none", "retake", true],
    ["lost", "pick", "retry", false],
    ["failed", "pick", "retry", false],
  ]);
  assert.deepEqual(connView(at("ready", { model: "CA79", version: "2.0" })).status, ["{model}{version} · {link}", { model: "CA79", version: " v2.0", link: "CA79" }]);
  assert.deepEqual(connView(at("failed", { error: "timeout" })).status, ["Connection failed: {e}", { e: "no answer" }]);
});

// ---------------------------------------------------------------- 2. fake stock piano with a panel port

/**
 * Stock controller binding with two ports: the app's (a LoopbackLink) and the panel's (simulated).
 * - CONNECT from the app: the app is bound; if the panel was, the panel gets 7E 01 00.
 * - DISCONNECT from the bound app: unbound, and the stock sends 7E 01 01 to the panel (FUN_4014C080),
 *   which CONNECTs `panelMs` later — if the app is bound by then, the app gets 7E 01 00.
 * - an unbound port gets no answer at all.
 */
function stockWithPanel(piano: InstanceType<typeof LoopbackLink>, panelMs = 15) {
  const st = { bound: "app" as "app" | "panel" | null, rx: [] as string[], connects: 0, disconnects: 0, silent: false, displacedApp: 0, onConnect: null as null | ((n: number) => void), onOther: null as null | ((m: Uint8Array) => void) };
  const out = (h: string) => piano.send(fromHex(h));
  const panelConnect = () => {
    if (st.bound === "app") {
      st.displacedApp++;
      out("F0 40 7F 7E 04 1E 01 00 F7");
    }
    st.bound = "panel";
  };
  piano.onMessage((m) => {
    const hex = toHex(m);
    st.rx.push(hex);
    const f = decodeFrame(m);
    if (!f) return st.onOther?.(m); // OpenGrand 0x7D (ngPiano)
    if (st.silent) return;
    if (f.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x01) {
      st.connects++;
      st.bound = "app";
      out("F0 40 7F 7E 04 1E 02 00 40 F7");
      return st.onConnect?.(st.connects);
    }
    if (f.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x00) {
      st.disconnects++;
      if (st.bound === "app") {
        st.bound = null;
        setTimeout(panelConnect, panelMs); // 7E 01 01 to the panel: it CONNECTs again
      }
      return;
    }
    if (st.bound !== "app") return;
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) return out("F0 40 7F 7E 04 1E 05 00 00 02 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x41) return out("F0 40 7F 7E 04 1E 05 01 00 01 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x42) return out("F0 40 7F 7E 04 1E 05 02 00 F7");
    if (f.fn === 0x7e && f.body[0] === 0x04 && f.body[1] === 0x41) return out("F0 40 7F 7E 04 1E 04 01 32 2E 31 39 F7");
    if (f.fn === 0x02 && f.body[0] === 0x5f) return out("F0 40 7F 03 04 1E 5F 7F 7F F7");
  });
  return { st, panelConnect };
}

const fast = (l: MidiLink) => new PianoSession(l, { ackTimeoutMs: 60 });
const mgrFor = (extra: Partial<ConstructorParameters<typeof ConnectionManager>[0]> = {}) => {
  const phases: Phase[] = [];
  const m = new ConnectionManager({ makeSession: fast, replyMs: 300, ...extra });
  m.onChange((s) => { if (phases[phases.length - 1] !== s.phase) phases.push(s.phase); });
  return { m, phases };
};

test("symptom 2+3, old flow: Connect again on the same piano → the app displaces itself", async () => {
  // main.ts before: attach() = old session.disconnect() (DISCONNECT) + a new session CONNECT on a new link
  const [app, piano] = LoopbackLink.pair();
  const { st } = stockWithPanel(piano);
  const s1 = fast(app);
  await s1.connect(300);
  s1.disconnect();
  const s2 = fast(app);
  await s2.connect(300).catch(() => {});
  await tick(40);
  assert.equal(s2.state, "displaced", "the panel took the binding our own DISCONNECT handed it");
  assert.equal(st.displacedApp, 1);
});

test("symptom 2+3 fixed: Connect again on the same piano opens nothing, sends nothing, stays ready", async () => {
  const [app, piano] = LoopbackLink.pair();
  const { st } = stockWithPanel(piano);
  let menus = 0, pickers = 0;
  const { m } = mgrFor({ openMenu: () => menus++, openPicker: () => pickers++ });
  assert.equal(await m.attach(app), "ready");
  const s = m.session;
  const n = st.rx.length;
  // header button while connected: the menu, not the device list
  m.connectButton();
  assert.deepEqual([menus, pickers], [1, 0]);
  // the same piano picked again (list entry with a key / the BLE chooser returning the open link)
  let opened = 0;
  assert.equal(await m.choose({ key: "loopback:a", name: "a", open: async () => { opened++; return app; } }), "ready");
  assert.equal(await m.attach(app), "ready");
  await tick(40);
  assert.equal(opened, 0, "no new link");
  assert.equal(m.session, s, "no new session");
  assert.equal(st.rx.length, n, "nothing sent: no DISCONNECT, no CONNECT");
  assert.equal(st.displacedApp, 0);
  assert.equal(m.phase, "ready");
});

test("switching to another device: the old session goes quietly, never shown as displaced", async () => {
  const [a, pa] = LoopbackLink.pair();
  const A = stockWithPanel(pa);
  const [b, pb] = LoopbackLink.pair();
  const B = stockWithPanel(pb);
  (b as unknown as { key: string }).key = "loopback:b";
  const { m, phases } = mgrFor();
  let aClosed = 0;
  a.onClose(() => aClosed++);
  await m.attach(a);
  const old = m.session!;
  assert.equal(await m.attach(b), "ready");
  assert.notEqual(m.session, old);
  assert.equal(old.state, "closed");
  assert.equal(aClosed, 1, "the old link is closed");
  assert.ok(!A.st.rx.includes(DISCONNECT), "no DISCONNECT: it could hand the binding to the panel and displace a session on the same piano");
  // late frames for the old session (its link still delivers here) change nothing
  pa.send(fromHex("F0 40 7F 7E 04 1E 01 00 F7"));
  await tick(20);
  assert.equal(m.phase, "ready");
  assert.ok(!phases.includes("displaced"), phases.join(" "));
  assert.equal(B.st.connects, 1);
});

test("symptom 1 fixed: Take control again → CONNECT → dump → ready, and the header says so", async () => {
  const [app, piano] = LoopbackLink.pair();
  const { st, panelConnect } = stockWithPanel(piano);
  const { m, phases } = mgrFor();
  await m.attach(app);
  panelConnect(); // the panel takes the binding
  await tick(10);
  assert.equal(m.phase, "displaced");
  const s = m.session!;
  m.retake();
  assert.equal(m.phase, "retaking");
  assert.equal(connView(m.state).button.action, "none", "no Connect while taking control");
  assert.equal(await m.settled(), "ready");
  assert.equal(m.session, s);
  assert.equal(st.bound, "app");
  assert.deepEqual(connView(m.state).status[0], "{model}{version} · {link}");
  assert.equal(connView(m.state).banner, null);
  assert.deepEqual(phases, ["connecting", "syncing", "ready", "displaced", "retaking", "ready"]);
});

test("symptom 1, no answer: retake ends in 'Connection failed · Try again', and Try again recovers", async () => {
  const [app, piano] = LoopbackLink.pair();
  const { st, panelConnect } = stockWithPanel(piano);
  const { m } = mgrFor({ limits: { retaking: 150 } });
  await m.attach(app);
  panelConnect();
  await tick(10);
  st.silent = true; // the piano does not answer (BLE module stuck, piano busy)
  m.retake();
  assert.equal(await m.settled(), "failed", "never stuck in 'connecting'");
  assert.equal(connView(m.state).banner?.button?.action, "retry");
  st.silent = false;
  m.retry();
  assert.equal(await m.settled(), "ready");
});

// A3 (08_bt_audit): the panel's start-up CONNECT displaces an app that reached the freshly powered piano first
const BOOT = { windowMs: 300, delayMs: 30 };

test("A3: displaced right after connecting (panel start-up) → control taken back once; a later panel tap is final", async () => {
  const [app, piano] = LoopbackLink.pair();
  const { st, panelConnect } = stockWithPanel(piano);
  const { m, phases } = mgrFor({ bootRetake: BOOT });
  assert.equal(await m.attach(app), "ready");
  const s = m.session;
  panelConnect(); // the panel finished booting and CONNECTs: nobody touched it
  await tick(5);
  assert.equal(m.phase, "displaced");
  assert.match(connView(m.state).banner!.msg[0], /taking it back in a moment/);
  assert.equal(st.connects, 1, "not at once: the panel is still in its CONNECT / identify steps");
  assert.equal(await m.settled(), "displaced");
  await tick(BOOT.delayMs + 10);
  assert.equal(await m.settled(), "ready");
  assert.equal(m.session, s, "the same session took control again");
  assert.equal(st.bound, "app");
  assert.equal(st.connects, 2);
  // the user taps the panel's "App connected" overlay (still inside the window): no tug of war
  panelConnect();
  await tick(BOOT.delayMs * 3);
  assert.equal(m.phase, "displaced");
  assert.equal(st.bound, "panel");
  assert.equal(st.connects, 2, "nothing sent");
  assert.match(connView(m.state).banner!.msg[0], /Another device/);
  assert.deepEqual(phases, ["connecting", "syncing", "ready", "displaced", "retaking", "ready", "displaced"]);
});

test("A3: a panel that CONNECTs again right after the re-grab wins — once means once", async () => {
  const [app, piano] = LoopbackLink.pair();
  const { st, panelConnect } = stockWithPanel(piano);
  const { m } = mgrFor({ bootRetake: BOOT });
  await m.attach(app);
  panelConnect();
  // the original panel app reloads when displaced during its start-up and CONNECTs again
  st.onConnect = (n) => { if (n === 2) setTimeout(panelConnect, 2); };
  await tick(BOOT.delayMs + 60);
  assert.equal(m.phase, "displaced");
  await tick(BOOT.delayMs * 3);
  assert.equal(m.phase, "displaced");
  assert.equal(st.connects, 2, "one re-grab only");
  assert.equal(st.bound, "panel");
});

test("A3: displacement after the window, or with the feature off, is never taken back automatically", async () => {
  for (const bootRetake of [{ windowMs: 20, delayMs: 10 }, undefined]) {
    const [app, piano] = LoopbackLink.pair();
    const { st, panelConnect } = stockWithPanel(piano);
    const { m } = mgrFor({ bootRetake });
    await m.attach(app);
    await tick(40); // past the window
    panelConnect();
    await tick(60);
    assert.equal(m.phase, "displaced", JSON.stringify(bootRetake));
    assert.equal(st.connects, 1);
    assert.match(connView(m.state).banner!.msg[0], /Another device/);
  }
});

test("A3: 'Take control again' during the wait does it at once and cancels the automatic one", async () => {
  const [app, piano] = LoopbackLink.pair();
  const { st, panelConnect } = stockWithPanel(piano);
  const { m } = mgrFor({ bootRetake: { windowMs: 300, delayMs: 80 } });
  await m.attach(app);
  panelConnect();
  await tick(5);
  m.retake();
  assert.equal(await m.settled(), "ready");
  await tick(120);
  assert.equal(st.connects, 2, "no second CONNECT from the cancelled timer");
  assert.equal(m.phase, "ready");
});

// ---------------------------------------------------------------- 2b. apply now drops the binding (field 2026-11-09)

const NG = (op: number, ...data: number[]) => fromHex(["F0 7D 43 41 37 39", op.toString(16).padStart(2, "0"), ...data.map((b) => b.toString(16).padStart(2, "0")), "F7"].join(" "));
const ngReq = (op: number, ...args: number[]) => NG(op, ...args);

/**
 * OpenGrand session core on top of the fake stock: bound-only BT op `1D 0A 02` answers `5D 0A <rc>` (rc 2 = not bound),
 * Identify answers the payload's feature bytes. `swap()` = apply now: the new core's table is empty (field 2026-11-09).
 * `bindOnConnect: false` = a CONNECT that does not bind (to show there is no CONNECT loop).
 */
function ngPiano(piano: InstanceType<typeof LoopbackLink>, opt: { bindOnConnect?: boolean } = {}) {
  const base = stockWithPanel(piano);
  const ng = { bound: true, feat: 0x3b, swaps: 0, writes: [] as number[] };
  base.st.onConnect = () => { if (opt.bindOnConnect !== false) ng.bound = true; };
  base.st.onOther = (m) => {
    if (m[1] !== 0x7d) return;
    const op = m[6]!;
    if (op === 0x01) piano.send(NG(0x41, 1, 2, 19, ng.feat, 0x7f, 0x3f));
    if (op === 0x1d && m[7] === 0x0a && m[8] === 0x02) {
      ng.writes.push(ng.bound ? 0 : 2);
      piano.send(NG(0x5d, 0x0a, ng.bound ? 0 : 2, 2, 1, 0, 0, 1));
    }
  };
  const swap = (feat = ng.feat) => {
    ng.bound = false;
    ng.feat = feat;
    ng.swaps++;
  };
  return { ...base, ng, swap };
}

test("apply now (field 2026-11-09): a 'not bound' answer → CONNECT + re-sync once → the op answers rc 0", async () => {
  const [app, piano] = LoopbackLink.pair();
  const { st, ng, swap } = ngPiano(piano);
  const { m, phases } = mgrFor();
  assert.equal(await m.attach(app), "ready");
  const s = m.session!;
  swap(); // the live payload swap the app did not see (the field case: no Identify read since)
  s.sendRaw(ngReq(0x1d, 0x0a, 0x02));
  const texts: string[] = [];
  m.onChange((x) => texts.push(String(connView(x).status[0])));
  await tick(10);
  assert.equal(await m.settled(), "ready");
  assert.ok(texts.includes("Reconnecting to {name}…"), texts.join(" | "));
  assert.equal(m.session, s, "same session, same link");
  assert.equal(st.connects, 2, "one more CONNECT");
  s.sendRaw(ngReq(0x1d, 0x0a, 0x02));
  await tick(10);
  assert.deepEqual(ng.writes, [2, 0]);
  assert.equal(m.phase, "ready");
  assert.deepEqual(phases, ["connecting", "syncing", "ready", "retaking", "ready"]);
});

test("apply now: a CONNECT that does not bind is not retried in a loop ('not bound' answered once)", async () => {
  const [app, piano] = LoopbackLink.pair();
  const { st, ng, swap } = ngPiano(piano, { bindOnConnect: false });
  const { m } = mgrFor();
  await m.attach(app);
  const s = m.session!;
  swap();
  for (let i = 0; i < 3; i++) {
    s.sendRaw(ngReq(0x1d, 0x0a, 0x02));
    await tick(10);
    await m.settled();
  }
  assert.deepEqual(ng.writes, [2, 2, 2]);
  assert.equal(st.connects, 2, "one re-bind CONNECT only");
  assert.equal(m.phase, "ready");
  // the user's Reconnect still does a real handshake, and re-arms the automatic one
  m.retry();
  assert.equal(await m.settled(), "ready");
  assert.equal(st.connects, 3);
  s.sendRaw(ngReq(0x1d, 0x0a, 0x02));
  await tick(10);
  assert.equal(await m.settled(), "ready");
  assert.equal(st.connects, 4);
});

test("apply now: the Identify reply changed → CONNECT once; the same Identify again → nothing", async () => {
  const [app, piano] = LoopbackLink.pair();
  const { st, swap } = ngPiano(piano);
  const { m } = mgrFor();
  await m.attach(app);
  const s = m.session!;
  const { NgClient } = await import("../src/protocol/ngext.ts");
  const ng = new NgClient(s);
  assert.ok(await ng.identify(200));
  assert.ok(await ng.identify(200));
  assert.equal(st.connects, 1, "same Identify: nothing");
  swap(0x3f);
  assert.equal((await ng.identify(200))?.features, 0x3f, "the caller still gets its reply");
  assert.equal(await m.settled(), "ready");
  assert.equal(st.connects, 2);
  await ng.identify(200);
  await tick(10);
  assert.equal(st.connects, 2, "the new Identify is the new baseline");
});

test("Reconnect from the connected menu: ready → CONNECT + dump on the same session (it sent nothing before)", async () => {
  const [app, piano] = LoopbackLink.pair();
  const { st } = stockWithPanel(piano);
  const { m } = mgrFor();
  await m.attach(app);
  const s = m.session!;
  const n = st.rx.length;
  m.retry();
  assert.equal(m.phase, "retaking");
  assert.equal(await m.settled(), "ready");
  assert.equal(m.session, s);
  assert.equal(st.connects, 2);
  assert.ok(st.rx.slice(n).includes(CONNECT), st.rx.slice(n).join(" | "));
  assert.ok(!st.rx.slice(n).includes(DISCONNECT), "no DISCONNECT: it would hand the binding to the panel");
});

test("connect watchdog: a piano that never finishes the dump ends in failed, not in syncing", async () => {
  const [app, piano] = LoopbackLink.pair();
  const { st } = stockWithPanel(piano);
  piano.onMessage((msg) => {
    const f = decodeFrame(msg);
    st.rx.push(toHex(msg));
    if (f?.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x01) piano.send(fromHex("F0 40 7F 7E 04 1E 02 00 40 F7"));
    if (f?.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) piano.send(fromHex("F0 40 7F 7E 04 1E 05 00 00 02 F7"));
    // optional queries and the dump request: nothing
  });
  const { m, phases } = mgrFor({ limits: { syncing: 100 }, replyMs: 60 });
  assert.equal(await m.attach(app), "failed");
  assert.ok(phases.includes("syncing"));
  assert.equal(m.session, null, "the stuck session is released");
});

test("link lost → 'Reconnect' reopens the same endpoint; stale close of a released session is ignored", async () => {
  let pair = LoopbackLink.pair();
  stockWithPanel(pair[1]);
  const reopen = async () => {
    pair = LoopbackLink.pair();
    stockWithPanel(pair[1]);
    Object.assign(pair[0], { key: "loopback:a", reopen });
    return pair[0];
  };
  Object.assign(pair[0], { reopen });
  const { m } = mgrFor();
  await m.attach(pair[0]);
  pair[0].close();
  assert.equal(m.phase, "lost");
  assert.equal(connView(m.state).banner?.button?.action, "retry");
  m.retry();
  assert.equal(await m.settled(), "ready");
});

test("auto-reconnect (desktop, field 2026-11-03): piano power-cycled → reopen fails while it boots → back on its own, no click", async () => {
  // the piano is off for the first two attempts (the BLE module does not advertise while it boots), then on
  let pair = LoopbackLink.pair();
  let fake = stockWithPanel(pair[1]);
  let attempts = 0;
  const reopen = async () => {
    attempts++;
    if (attempts <= 2) throw new Error("NetworkError: Connection attempt failed.");
    pair = LoopbackLink.pair();
    fake = stockWithPanel(pair[1]);
    Object.assign(pair[0], { reopen });
    return pair[0];
  };
  Object.assign(pair[0], { reopen });
  const delays: number[] = [];
  const traced: string[] = [];
  const { m, phases } = mgrFor({
    autoReconnect: { delaysMs: [5, 10, 20] },
    trace: (l) => traced.push(l),
    setTimer: (fn, ms) => {
      if (ms <= 20) delays.push(ms); // reconnect delays only (the watchdogs are long)
      return setTimeout(fn, ms);
    },
  });
  assert.equal(await m.attach(pair[0]), "ready");
  pair[0].close(); // gattserverdisconnected: the piano went off
  assert.equal(m.phase, "lost");
  for (let i = 0; i < 50 && (m.phase as Phase) !== "ready"; i++) await tick(10);
  assert.equal(m.phase, "ready", "reconnected without a click");
  assert.equal(attempts, 3);
  assert.deepEqual(delays, [5, 10, 20], "backoff: one delay per attempt");
  assert.equal(fake.st.connects, 1, "the rebooted piano got a CONNECT");
  assert.ok(!phases.includes("failed"), "a failed attempt is lost again, never 'Connection failed'");
  assert.equal(m.state.reconnects, 0);
  assert.ok(traced.some((l) => l.includes("reconnect attempt 3")));
});

test("auto-reconnect stops: user Disconnect while waiting cancels the pending attempt", async () => {
  const pair = LoopbackLink.pair();
  stockWithPanel(pair[1]);
  let attempts = 0;
  Object.assign(pair[0], { reopen: async () => { attempts++; throw new Error("x"); } });
  const { m } = mgrFor({ autoReconnect: { delaysMs: [20] } });
  await m.attach(pair[0]);
  pair[0].close();
  m.disconnect();
  await tick(60);
  assert.equal(m.phase, "idle");
  assert.equal(attempts, 0);
});

test("re-acquire (desktop, field 2026-11-03): the held link never reopens after Windows re-made the BLE connection → a fresh lookup connects", async () => {
  // the held link object is stale: every reopen through it fails; the endpoint looked up afresh connects
  let pair = LoopbackLink.pair();
  let fake = stockWithPanel(pair[1]);
  let held = 0;
  let fresh = 0;
  const stale = async (): Promise<MidiLink> => {
    held++;
    throw new Error("NetworkError: Connection attempt failed.");
  };
  Object.assign(pair[0], { key: "ble:abc", reopen: stale });
  const keys: (string | null)[] = [];
  const traced: string[] = [];
  const again = async (): Promise<MidiLink> => {
    pair = LoopbackLink.pair();
    fake = stockWithPanel(pair[1]);
    Object.assign(pair[0], { key: "ble:abc", reopen: again });
    return pair[0];
  };
  const { m, phases } = mgrFor({
    autoReconnect: { delaysMs: [5, 10] },
    trace: (l) => traced.push(l),
    reacquire: (key) => {
      keys.push(key);
      fresh++;
      return again();
    },
  });
  assert.equal(await m.attach(pair[0]), "ready");
  pair[0].close(); // Windows dropped the BLE connection
  for (let i = 0; i < 50 && (m.phase as Phase) !== "ready"; i++) await tick(10);
  assert.equal(m.phase, "ready", "back without a restart");
  assert.equal(held, 1, "the held link first, once");
  assert.equal(fresh, 1, "then the device looked up afresh");
  assert.deepEqual(keys, ["ble:abc"]);
  assert.equal(fake.st.connects, 1);
  assert.ok(!phases.includes("failed"));
  assert.ok(traced.some((l) => l.includes("re-acquiring")));

  // the next loss reopens the fresh link's own object first (it is good: no second lookup)
  pair[0].close();
  for (let i = 0; i < 50 && (m.phase as Phase) !== "ready"; i++) await tick(10);
  assert.equal(m.phase, "ready");
  assert.equal(fresh, 1, "a held link that reopens needs no lookup");
  m.disconnect();
});

test("re-acquire: the header Connect in lost alternates too; a lookup that fails hands back to the held link; off without the option", async () => {
  const pair = LoopbackLink.pair();
  stockWithPanel(pair[1]);
  const order: string[] = [];
  Object.assign(pair[0], { key: "ble:abc", reopen: async () => { order.push("held"); throw new Error("Connection attempt failed"); } });
  const { m } = mgrFor({
    autoReconnect: { delaysMs: [60000] }, // no automatic attempt during the test
    reacquire: (key) => {
      if (!key?.startsWith("ble:")) return null;
      order.push("fresh");
      return Promise.reject(new Error("NotFoundError: not found"));
    },
  });
  await m.attach(pair[0]);
  pair[0].close();
  for (let n = 0; n < 4; n++) {
    m.connectButton();
    assert.equal(await m.settled(), "lost", "a failed reopen is lost again (auto-reconnect on)");
  }
  assert.deepEqual(order, ["held", "fresh", "held", "fresh"]);
  m.disconnect();

  // browser tab / phone: no reacquire option → only the held link
  const p2 = LoopbackLink.pair();
  stockWithPanel(p2[1]);
  let heldOnly = 0;
  Object.assign(p2[0], { key: "ble:abc", reopen: async () => { heldOnly++; throw new Error("Connection attempt failed"); } });
  const { m: m2 } = mgrFor();
  await m2.attach(p2[0]);
  p2[0].close();
  m2.connectButton();
  assert.equal(await m2.settled(), "failed");
  m2.retry(); // failed → current: the link is dead, so the held reopen again
  assert.equal(await m2.settled(), "failed");
  assert.equal(heldOnly, 2);
});

test("lost without auto-reconnect: the header Connect reopens the piano just lost, not the device list", async () => {
  let pair = LoopbackLink.pair();
  stockWithPanel(pair[1]);
  const reopen = async () => {
    pair = LoopbackLink.pair();
    stockWithPanel(pair[1]);
    Object.assign(pair[0], { reopen });
    return pair[0];
  };
  Object.assign(pair[0], { reopen });
  let pickers = 0;
  const { m } = mgrFor({ openPicker: () => pickers++ });
  await m.attach(pair[0]);
  pair[0].close();
  m.connectButton();
  assert.equal(await m.settled(), "ready");
  assert.equal(pickers, 0);
});

test("suspend (app away) sends DISCONNECT and closes the link; resume reconnects to the same piano", async () => {
  let pair = LoopbackLink.pair();
  let fake = stockWithPanel(pair[1]);
  let closed = 0;
  const reopen = async () => {
    pair = LoopbackLink.pair();
    fake = stockWithPanel(pair[1]);
    Object.assign(pair[0], { reopen });
    pair[0].onClose(() => closed++);
    return pair[0];
  };
  Object.assign(pair[0], { reopen });
  pair[0].onClose(() => closed++);
  const remembered: unknown[] = [];
  const { m, phases } = mgrFor({ remember: (l) => remembered.push(l) });
  await m.attach(pair[0]);
  assert.deepEqual(remembered, [{ key: "loopback:a", name: "a" }]);
  const first = fake;
  m.suspend();
  await tick(30);
  assert.equal(m.phase, "idle");
  assert.equal(m.state.suspended, true);
  assert.ok(first.st.rx.includes(DISCONNECT), "the stock hands control back to the panel");
  assert.equal(closed, 1, "link closed: BLE module free for another device");
  assert.equal(first.st.bound, "panel");
  m.resume();
  assert.equal(await m.settled(), "ready");
  assert.equal(fake.st.connects, 1);
  assert.ok(!phases.includes("displaced"));
  // user Disconnect forgets the piano: no reconnect on the next start
  m.disconnect();
  assert.equal(remembered[remembered.length - 1], null);
  m.resume();
  assert.equal(m.phase, "idle");
});

test("Web Bluetooth: the chooser picking the open piano returns the same link; close lets GATT go after the queued writes", async () => {
  const written: number[] = [];
  let connects = 0, disconnects = 0;
  const ch = {
    value: undefined,
    startNotifications: async () => undefined,
    writeValueWithoutResponse: (d: Uint8Array) => new Promise<void>((r) => setTimeout(() => { written.push(d.length); r(); }, 5)),
    addEventListener: () => {},
    removeEventListener: () => {},
  };
  const listeners: (() => void)[] = [];
  const device = {
    id: "dev1",
    name: "CA79",
    gatt: { connect: async () => { connects++; return { getPrimaryService: async () => ({ getCharacteristic: async () => ch }), disconnect: () => { disconnects++; for (const l of [...listeners]) l(); } }; } },
    addEventListener: (_t: string, cb: () => void) => void listeners.push(cb),
    removeEventListener: (_t: string, cb: () => void) => void listeners.splice(listeners.indexOf(cb), 1),
  };
  const l1 = await connectBleDevice(device);
  const l2 = await connectBleDevice(device);
  assert.equal(l1, l2);
  assert.equal(l1.key, "ble:dev1");
  assert.equal(connects, 1);
  l1.send(fromHex(DISCONNECT));
  l1.close();
  assert.equal(disconnects, 0, "not before the DISCONNECT went out");
  await l1.released;
  assert.equal(written.length, 1);
  assert.equal(disconnects, 1);
  const l3 = await l1.reopen!();
  assert.notEqual(l3, l1);
  assert.equal(connects, 2);
  l3.close();
  await l3.released;
});

test("Android shell: picking the open device again does not reopen it; onDetached → lost; background / foreground → suspend / resume", async () => {
  // fake `ca79ng` bridge on a fake stock piano
  const [shellSide, piano] = LoopbackLink.pair();
  const fake = stockWithPanel(piano);
  const calls: string[] = [];
  const native = (globalThis as unknown as { ca79ngNative: Record<string, (...a: string[]) => void> }).ca79ngNative;
  shellSide.onMessage((m) => native.onMidi!("ble:AA", toHex(m)));
  (globalThis as unknown as { ca79ng: object }).ca79ng = {
    listDevices: () => JSON.stringify([{ id: "ble:AA", name: "CA79", driver: "BLE-MIDI" }]),
    open: (id: string) => (calls.push(`open ${id}`), true),
    send: (_id: string, hex: string) => shellSide.send(fromHex(hex)),
    close: (id: string) => void calls.push(`close ${id}`),
    panel: () => "{}", setBacklight: () => {}, usbPath: () => "", scanBle: () => {},
  };
  try {
    const { m } = mgrFor();
    const off = android.onShellLifecycle((s) => (s === "background" ? m.suspend() : m.resume()));
    const [cand] = android.listAndroid();
    assert.equal(cand!.key, "android:ble:AA");
    assert.equal(await m.choose(cand!), "ready");
    assert.equal(await m.choose(cand!), "ready");
    assert.equal(await m.choose(android.androidCandidate("android:ble:AA", "CA79")!), "ready");
    assert.deepEqual(calls, ["open ble:AA"], "one open for three picks");
    // the shell's activity stopped past its grace: the page lets the piano go
    native.onLifecycle!("background");
    await tick(30);
    assert.equal(m.phase, "idle");
    assert.ok(fake.st.rx.includes(DISCONNECT));
    assert.deepEqual(calls, ["open ble:AA", "close ble:AA"]);
    native.onLifecycle!("foreground");
    assert.equal(await m.settled(), "ready");
    assert.deepEqual(calls, ["open ble:AA", "close ble:AA", "open ble:AA"]);
    // the module went away (MidiManager onDeviceRemoved → onDetached)
    native.onDetached!("ble:AA");
    assert.equal(m.phase, "lost");
    off();
  } finally {
    delete (globalThis as unknown as { ca79ng?: object }).ca79ng;
  }
});
