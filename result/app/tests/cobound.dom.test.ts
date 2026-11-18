// Two controllers bound at once (Phase 3-2 stage 2):
// the header line "Panel also connected" / on the panel "App also connected" from op 0x1B 03, shown only
// with a session-core payload (Identify feat2 0x02), and the piano's change frames (fan-out) applied live
// to the store and ACKed like any piano fn 0x10.
import { tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const i18n = await import("../src/i18n.ts");
const { coBoundText, coBoundBadge } = await import("../src/ui/cobound.ts");
const { decodeSessionCore } = await import("../src/protocol/ngext.ts");
const { PianoSession } = await import("../src/protocol/session.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const { paramByWire } = await import("../src/protocol/codecs.ts");

/** The cave's 8 -> 7 bit packing (sysex_cave.c pack7). */
function pack7(raw: number[]): number[] {
  const out: number[] = [];
  for (let i = 0; i < raw.length; i += 7) {
    const g = raw.slice(i, i + 7);
    out.push(g.reduce((hi, b, k) => hi | (((b >> 7) & 1) << k), 0));
    out.push(...g.map((b) => b & 0x7f));
  }
  return out;
}

/** Reply data (after the op byte) of 1B 03 for a table, a binding and (stage 2) the asking port. */
function core03(bound: number, binding: number, port?: number): number[] {
  const raw = new Array<number>(48).fill(0);
  raw[0] = 3;
  raw[1] = 0x07;
  raw[2] = bound;
  raw[3] = binding;
  if (port !== undefined) raw[45] = 0x80 | port;
  return [0x03, 0x00, ...pack7(raw)];
}

test("1B 03 decode: table, binding, the asking port only when the payload says (byte 45 bit 7)", () => {
  assert.deepEqual(decodeSessionCore(Uint8Array.from(core03(0x0c, 4, 2))), { bound: 0x0c, binding: 4, port: 2 });
  assert.deepEqual(decodeSessionCore(Uint8Array.from(core03(0x0c, 4))), { bound: 0x0c, binding: 4 });
  assert.equal(decodeSessionCore(Uint8Array.from([0x03, 0x01])), null); // rc 1
});

test("header text: the other controller named, from the app and from the panel; alone → nothing", () => {
  i18n.setLang("en");
  assert.equal(coBoundText({ bound: 0x0c, binding: 4, port: 2 }, null), "Panel also connected");
  assert.equal(coBoundText({ bound: 0x0c, binding: 4, port: 3 }, null), "App also connected (Bluetooth)");
  assert.equal(coBoundText({ bound: 0x0c, binding: 4 }, 3), "App also connected (Bluetooth)"); // stage 1 payload: our port inferred
  assert.equal(coBoundText({ bound: 0x0c, binding: 4, port: 2 }, 3), "Panel also connected"); // the payload's word wins
  assert.equal(coBoundText({ bound: 0x0d, binding: 4, port: 3 }, null), "Also connected: the app (Bluetooth), a USB device");
  assert.equal(coBoundText({ bound: 0x0c, binding: 3 }, null), "Another controller is also connected");
  assert.equal(coBoundText({ bound: 0x04, binding: 3, port: 2 }, null), null);
  assert.equal(coBoundText({ bound: 0x04, binding: 3 }, null), null);
  i18n.setLang("ko");
  assert.equal(coBoundText({ bound: 0x0c, binding: 4, port: 2 }, null), "패널도 연결됨");
  assert.equal(coBoundText({ bound: 0x0c, binding: 4, port: 3 }, null), "앱도 연결됨(블루투스)");
  i18n.setLang("en");
});

/** A session stand-in that answers the 0x7D ops the badge asks (Identify, 1B 03). */
function fakeSession(feat2: number, table: () => number[] | null) {
  const listeners: ((m: Uint8Array) => void)[] = [];
  const asked: number[][] = [];
  const reply = (op: number, data: number[]) => {
    const m = Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...data, 0xf7]);
    queueMicrotask(() => listeners.forEach((l) => l(m)));
  };
  return {
    asked,
    onMidi(fn: (m: Uint8Array) => void) {
      listeners.push(fn);
      return () => listeners.splice(listeners.indexOf(fn), 1);
    },
    sendRaw(m: Uint8Array) {
      const op = m[6]!;
      asked.push([...m.slice(6, m.length - 1)]);
      if (op === 0x01) reply(0x01, feat2 ? [1, 2, 19, 0x3b, 0x77, feat2] : [1, 2, 19, 0x3b, 0x57]);
      if (op === 0x1b && m[7] === 0x03) {
        const t = table();
        if (t) reply(0x1b, t);
      }
    },
  };
}

function fakeConn() {
  const fns: ((s: { phase: string; gen: number }) => void)[] = [];
  const conn = {
    state: { phase: "idle", gen: 0 } as { phase: string; gen: number },
    onChange(fn: (s: { phase: string; gen: number }) => void) { fns.push(fn); },
    go(phase: string) { conn.state = { phase, gen: conn.state.gen + (phase === "ready" ? 1 : 0) }; fns.forEach((f) => f(conn.state)); },
  };
  return conn;
}

test("badge: session-core payload → 1B 03 polled, line follows the table; other firmware → hidden, never asked", async () => {
  i18n.setLang("ko");
  Object.defineProperty(document, "hidden", { value: false, configurable: true }); // jsdom: a visible page
  let table = core03(0x0c, 4, 2);
  const s = fakeSession(0x03, () => table);
  const conn = fakeConn();
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const b = coBoundBadge(() => s as any, conn as any, () => 2, 60_000);
  assert.equal(b.el.hidden, true);
  conn.go("ready");
  await tick(30);
  assert.equal(b.el.hidden, false);
  assert.equal(b.el.textContent, "패널도 연결됨");
  assert.deepEqual(s.asked.map((a) => a[0]), [0x01, 0x1b]);
  table = core03(0x04, 3, 2); // the panel let go
  b.refresh();
  await tick(30);
  assert.equal(b.el.hidden, true);
  assert.deepEqual(s.asked.map((a) => a[0]), [0x01, 0x1b, 0x1b]); // Identify once per session
  conn.go("lost");
  await tick(5);
  assert.equal(b.el.hidden, true);

  const old = fakeSession(0, () => core03(0x0c, 4));
  const conn2 = fakeConn();
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const b2 = coBoundBadge(() => old as any, conn2 as any, () => 2, 60_000);
  conn2.go("ready");
  await tick(30);
  b2.refresh();
  await tick(30);
  assert.equal(b2.el.hidden, true);
  assert.deepEqual(old.asked.map((a) => a[0]), [0x01]); // no session core: 1B 03 never sent
  i18n.setLang("en");
});

test("fan-out frame from the piano: ACKed with its key and applied to the store at once", async () => {
  const [app, piano] = LoopbackLink.pair();
  const got: Uint8Array[] = [];
  piano.onMessage((m) => got.push(m));
  const s = new PianoSession(app);
  const store = new ParamStore();
  store.attach(s);
  const p = paramByWire(0x10, 0x55, 0x08, 0x00)!;
  assert.ok(p, "reverb type is in the parameter table");
  const seen: string[] = [];
  store.subscribe((k) => seen.push(k));
  // what the panel's set looks like on the app's port: the stock notification bytes (cc 7F, 04 1E)
  piano.send(Uint8Array.from([0xf0, 0x40, 0x7f, 0x10, 0x04, 0x1e, 0x55, 0x08, 0x00, 0x04, 0xf7]));
  await tick(5);
  assert.deepEqual(store.get(p, 0x00), [0x04]);
  assert.equal(seen.length, 1);
  assert.deepEqual([...got[0]!], [0xf0, 0x40, 0x7f, 0x07, 0x08, 0x02, 0x55, 0x08, 0x00, 0xf7]);
  // the stock's own notice (headphone 51 29 7F) the same way
  piano.send(Uint8Array.from([0xf0, 0x40, 0x7f, 0x10, 0x04, 0x1e, 0x51, 0x29, 0x7f, 0x01, 0xf7]));
  await tick(5);
  assert.deepEqual([...got[1]!.slice(3, 9)], [0x07, 0x08, 0x02, 0x51, 0x29, 0x7f]);
});
