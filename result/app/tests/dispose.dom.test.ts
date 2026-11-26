// A replaced screen leaves nothing behind (the panel runs Chrome 71 for days): main.ts show() builds each tab in a
// scope (src/ui/scope.ts) and disposes it when the tab is replaced - its control bindings, store / MIDI listeners and
// timers go with it. A reply from the previous piano is not applied after a switch.
import { change, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const { PianoSession } = await import("../src/protocol/session.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { createContext } = await import("../src/ui/context.ts");
const { boundControls } = await import("../src/ui/controls.ts");
const { createScope, inScope } = await import("../src/ui/scope.ts");
const S = await import("../src/ui/screens.ts");
const { infoScreen, NOR_POLL } = await import("../src/ui/info.ts");
const { powerDevSection, APO_TEST } = await import("../src/ui/power.ts");
const { powerButton } = await import("../src/ui/powerbtn.ts");
const ng = await import("../src/protocol/ngext.ts");
const { fromHex, decodeFrame } = await import("../src/protocol/sysex.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const i18n = await import("../src/i18n.ts");

type Store = InstanceType<typeof ParamStore>;
type Session = InstanceType<typeof PianoSession>;
const rawListeners = (store: Store) => (store as unknown as { rawListeners: Set<unknown> }).rawListeners.size;
const midiListeners = (s: Session) => (s as unknown as { midiListeners: unknown[] }).midiListeners.length;
const hex = (b: number) => b.toString(16).padStart(2, "0");

async function until(cond: () => boolean, ms = 3000): Promise<void> {
  for (let t = 0; t < ms && !cond(); t += 10) await tick(10);
}

/** build like main.ts show(): in a scope, shown, then replaced (removed + disposed) */
function showAndReplace<T extends HTMLElement>(build: () => T): T {
  const scope = createScope();
  const el = inScope(scope, build);
  document.body.append(el);
  el.remove();
  scope.dispose();
  return el;
}

/** A Kawai + OpenGrand piano: answers the handshake, Identify (`feat`), and `ng(op, sub, m)` for every other 0x7D op. */
async function ngPiano(feat: string, ng7d: (op: number, sub: number, m: Uint8Array, out: (h: string) => void) => void) {
  const [app, piano] = LoopbackLink.pair();
  const out = (h: string) => piano.send(fromHex(h));
  piano.onMessage((m) => {
    if (m[1] === 0x7d) {
      const op = m[6]!, sub = m[7] ?? 0;
      if (op === 0x01) out(`F0 7D 43 41 37 39 41 01 02 13 ${feat} F7`);
      else ng7d(op, sub, m, out);
      return;
    }
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01) out("F0 40 7F 7E 04 1E 02 00 40 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) out("F0 40 7F 7E 04 1E 05 00 00 02 F7");
    if (f.fn === 0x02) out("F0 40 7F 03 04 1E 5F 7F 7F F7");
  });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  await session.connect(100);
  return { session, out };
}

test("controls: screens built and replaced in a scope leave no store binding behind", () => {
  i18n.setLang("en");
  const store = new ParamStore();
  const ctx = createContext(store);
  const before = boundControls();
  for (let i = 0; i < 3; i++) {
    showAndReplace(() => S.settingsScreen(store, ctx));
    showAndReplace(() => S.soundScreen(store, ctx));
    showAndReplace(() => S.vtScreen(store, ctx));
    showAndReplace(() => S.metronomeScreen(store, new EventTarget()));
  }
  assert.equal(boundControls(), before, "bindings of replaced screens are still in the map");
  // the screen being shown keeps its bindings and follows the store
  const scope = createScope();
  const el = inScope(scope, () => S.settingsScreen(store, ctx));
  document.body.append(el);
  assert.ok(boundControls() > before);
  el.remove();
  scope.dispose();
  assert.equal(boundControls(), before);
});

test("Settings: the recorder-reset listener goes with the screen", () => {
  i18n.setLang("en");
  const store = new ParamStore();
  const before = rawListeners(store);
  const old = showAndReplace(() => S.settingsScreen(store, createContext(store)));
  assert.equal(rawListeners(store), before, "store.onMessage listener of the replaced Settings tab is still subscribed");
  store.fromWire({ fn: 0x32, v1: 0x52, v2: 0x22, part: 0x7f, data: [0, 0x7f, 0] });
  assert.equal(old.querySelector("#rec-reset")!.textContent, "", "the replaced tab still follows the piano");
});

test("Technician: the preview-stop listener goes with the screen", () => {
  i18n.setLang("en");
  const store = new ParamStore();
  const before = rawListeners(store);
  showAndReplace(() => S.vtScreen(store, createContext(store)));
  assert.equal(rawListeners(store), before, "store.onMessage listener of the replaced Technician tab is still subscribed");
});

test("Info > Developer > GPIO: change notices stop with the screen (listener gone, notices switched off)", async () => {
  i18n.setLang("en");
  const asked: number[][] = [];
  const polHex = ng.pack7(ng.encodePowerPolicy(ng.POWER_POLICY_DEFAULT)).map(hex).join(" ");
  const { session, out } = await ngPiano("3B 77 7F 38", (op, sub, m, send) => {
    if (op === 0x18) send(`F0 7D 43 41 37 39 58 ${hex(sub)} 00 ${polHex} F7`);
    if (op === 0x19 && sub === 0x00) send("F0 7D 43 41 37 39 59 00 00 00 F7");
    if (op === 0x17 && sub === 0x02) {
      asked.push([...m.slice(7, m.length - 1)]);
      send(`F0 7D 43 41 37 39 57 02 00 ${m[8] ? "03 02" : "02 7F"} F7`);
    }
  });
  const store = new ParamStore();
  const scope = createScope();
  const sec = inScope(scope, () => powerDevSection(createContext(store, { session: () => session })));
  document.body.append(sec);
  const box = sec.querySelector<HTMLInputElement>("[data-action=gpio-notify]")!;
  const base = midiListeners(session);
  change(box, true);
  await until(() => /Change notices on/.test(sec.querySelector("#gpio-status")!.textContent!));
  assert.equal(midiListeners(session), base + 1);
  sec.remove();
  scope.dispose();
  await tick(100);
  assert.equal(midiListeners(session), base, "the GPIO notice listener outlives the screen");
  out(`F0 7D 43 41 37 39 57 7F ${ng.pack7([1, 6, 0, 1, 0x70, 0x79, 0, 0, 0, 0, 1]).map(hex).join(" ")} F7`);
  await tick(30);
  assert.equal(sec.querySelectorAll(".gpio-event").length, 0);
  assert.deepEqual(asked, [[0x02, 0x01], [0x02, 0x00]], "the piano's notices were not switched off");
});

/** a cave piano with the NOR dump: status (op 3) answers while `answer()` */
function dumpPiano(answer: () => boolean) {
  const [app, piano] = LoopbackLink.pair();
  let polls = 0;
  piano.onMessage((m) => {
    if (m[1] !== 0x7d) return;
    const reply = (op: number, data: number[]) => piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...data, 0xf7]));
    if (m[6] === 0x01) reply(0x01, [1, 2, 19, 0x07, 0]);
    if (m[6] === 0x02) reply(0x02, [0]);
    if (m[6] === 0x03) {
      polls++;
      if (answer()) reply(0x03, [2, 0, 48, 0]); // running, chunk 48
    }
  });
  return { session: new PianoSession(app, { handshake: false }), polls: () => polls };
}

async function startDump(session: Session, scope = createScope()) {
  const el = inScope(scope, () => infoScreen(createContext(new ParamStore(), { session: () => session })));
  document.body.append(el);
  (el.querySelector("#ng-check") as HTMLButtonElement).click();
  await until(() => !(el.querySelector("#ng-dump") as HTMLElement).hidden);
  (el.querySelector("#ng-dump button") as HTMLButtonElement).click();
  return { el, scope };
}

test("Info > NOR dump: the status poll stops when the screen is replaced", async () => {
  i18n.setLang("en");
  const saved = { ...NOR_POLL };
  Object.assign(NOR_POLL, { ms: 20 });
  try {
    const p = dumpPiano(() => true);
    const { el, scope } = await startDump(p.session);
    await until(() => p.polls() >= 4);
    assert.match(el.querySelector("#ng-dump-status")!.textContent!, /Dumping… 50%/);
    el.remove();
    scope.dispose();
    await tick(30);
    const n = p.polls();
    await tick(200);
    assert.ok(p.polls() <= n + 1, `still polling after the screen was replaced: ${n} → ${p.polls()}`);
  } finally {
    Object.assign(NOR_POLL, saved);
  }
});

test("Info > NOR dump: the status poll stops after repeated no-replies (piano gone)", async () => {
  i18n.setLang("en");
  const saved = { ...NOR_POLL };
  Object.assign(NOR_POLL, { ms: 20, timeoutMs: 40 });
  try {
    let on = true;
    const p = dumpPiano(() => on);
    const { el, scope } = await startDump(p.session);
    await until(() => p.polls() >= 3);
    on = false; // disconnected: status() returns null from now on
    await tick(1000);
    const n = p.polls();
    await tick(1000); // longer than a status() timeout: a running poll sends at least once

    assert.equal(p.polls(), n, `still polling a piano that does not answer: ${n} → ${p.polls()}`);
    assert.match(el.querySelector("#ng-dump-status")!.textContent!, /No reply/);
    el.remove();
    scope.dispose();
  } finally {
    Object.assign(NOR_POLL, saved);
  }
});

test("Info > Developer > APO test: Cancel (and a new press) stops the previous countdown loop", async () => {
  i18n.setLang("en");
  const saved = { ...APO_TEST };
  APO_TEST.pollMs = 20;
  try {
    const polHex = ng.pack7(ng.encodePowerPolicy(ng.POWER_POLICY_DEFAULT)).map(hex).join(" ");
    const diag = new Uint8Array(60);
    diag[0] = 2; diag[6] = 1;
    new DataView(diag.buffer).setUint32(8, 45000 - 2950, true);
    const diagHex = ng.pack7([...diag]).map(hex).join(" ");
    let diags = 0;
    const { session } = await ngPiano("3B 77 3F", (op, sub, m, out) => {
      if (op === 0x18) out(`F0 7D 43 41 37 39 58 ${hex(sub)} 00 ${polHex} F7`);
      if (op === 0x19 && sub === 0x00) out("F0 7D 43 41 37 39 59 00 00 00 F7");
      if (op === 0x19 && sub === 0x04) out(`F0 7D 43 41 37 39 59 04 00 00 ${hex(m[8]!)} ${hex(m[9]!)} 01 00 F7`);
      if (op === 0x1a) {
        diags++;
        out(`F0 7D 43 41 37 39 5A ${diagHex} F7`);
      }
    });
    const store = new ParamStore();
    const scope = createScope();
    const sec = inScope(scope, () => powerDevSection(createContext(store, { session: () => session })));
    document.body.append(sec);
    await until(() => !!sec.querySelector("[data-action=apo-test-60]"));
    const press = (a: string) => sec.querySelector<HTMLButtonElement>(`[data-action=${a}]`)!.click();
    press("apo-test-60");
    await until(() => /in 60 s/.test(sec.querySelector("#power-dev-status")?.textContent ?? ""));
    press("apo-test-300");
    await until(() => /in 300 s/.test(sec.querySelector("#power-dev-status")?.textContent ?? ""));
    await tick(100);
    press("apo-test-cancel");
    await until(() => /full menu time/.test(sec.querySelector("#power-dev-status")?.textContent ?? ""));
    await tick(50);
    const n = diags;
    await tick(250);
    assert.ok(diags <= n + 1, `the countdown kept reading op 0x1A after Cancel: ${n} → ${diags}`);
    // a press, then the screen replaced: the loop stops too
    press("apo-test-60");
    await until(() => /in 60 s/.test(sec.querySelector("#power-dev-status")?.textContent ?? ""));
    scope.dispose(); // still in the document: only the dispose can stop it
    await tick(50);
    const m = diags;
    await tick(250);
    assert.ok(diags <= m + 1, `the countdown kept reading op 0x1A after dispose: ${m} → ${diags}`);
    sec.remove();
  } finally {
    Object.assign(APO_TEST, saved);
  }
});

test("header power button: a reply from the previous piano is not applied after a switch", async () => {
  i18n.setLang("en");
  // piano 1 answers 「enter standby」 late; piano 2 is on
  const p1 = await ngPiano("3B 77 3F", (op, sub, _m, out) => {
    if (op === 0x19 && sub === 0x00) out("F0 7D 43 41 37 39 59 00 00 00 F7");
    if (op === 0x19 && sub === 0x01) setTimeout(() => out("F0 7D 43 41 37 39 59 01 00 01 F7"), 150);
  });
  const p2 = await ngPiano("3B 77 3F", (op, sub, _m, out) => {
    if (op === 0x19 && sub === 0x00) out("F0 7D 43 41 37 39 59 00 00 00 F7");
  });
  let cur: Session = p1.session;
  const fns: ((s: { phase: string; gen: number }) => void)[] = [];
  const conn = { state: { phase: "ready", gen: 1 }, onChange: (fn: (s: { phase: string; gen: number }) => void) => fns.push(fn) };
  const said: string[] = [];
  const pb = powerButton(() => cur, conn as never, { say: (m) => said.push(m) });
  fns.forEach((fn) => fn(conn.state));
  await until(() => !pb.el.hidden && pb.state() === "on");
  pb.el.click(); // 19 01 to piano 1 ...
  await tick(10);
  cur = p2.session; // ... and the app switches to piano 2 before the reply
  conn.state = { phase: "ready", gen: 2 };
  fns.forEach((fn) => fn(conn.state));
  await until(() => pb.state() === "on");
  await tick(250); // piano 1's reply has arrived
  assert.equal(pb.state(), "on", "piano 1's standby reply was applied to piano 2's button");
  assert.deepEqual(said, [], "piano 1's reply was reported");
});
