// `7E 01 01` from the piano = "CONNECT again".
// Field 2026-11-01: after "Apply now" (live payload swap) the new session core started with only the
// app in its table; the built-in panel never dropped its USB link, kept showing "Connected" and got no
// fan-out until Disconnect / Connect. The core now sends the panel the stock's own invitation
// (FUN_4014C080: F0 40 7F 7E 04 1E 01 01 F7, port 3 only), which the original Kawai panel app answers
// with CONNECT + a full parameter request — our panel UI must do the same.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const { PianoSession } = await import("../src/protocol/session.ts");
const { fromHex, toHex, decodeFrame } = await import("../src/protocol/sysex.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const { takeoverBanner } = await import("../src/ui/takeover.ts");
const { ConnectionManager, connView } = await import("../src/state/connection.ts");

const INVITE = "F0 40 7F 7E 04 1E 01 01 F7";
const CONNECT = "F0 40 7F 7E 08 02 01 01 F7";
const REQ_ALL = "F0 40 7F 02 08 02 5F 7F 7F F7";
const hex2 = (n: number) => n.toString(16).padStart(2, "0").toUpperCase();

/** The panel's port on a OpenGrand piano: answers while the core table has it, silent otherwise. */
function piano(link: InstanceType<typeof LoopbackLink>) {
  const st = { bound: false, rx: [] as string[], acks: [] as string[] };
  const out = (h: string) => link.send(fromHex(h));
  link.onMessage((m) => {
    const hex = toHex(m);
    st.rx.push(hex);
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x01) {
      st.bound = true;
      return out("F0 40 7F 7E 04 1E 02 00 40 F7");
    }
    if (f.fn === 0x07) return void st.acks.push(hex);
    if (!st.bound) return;
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) return out("F0 40 7F 7E 04 1E 05 00 00 02 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x41) return out("F0 40 7F 7E 04 1E 05 01 00 01 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x42) return out("F0 40 7F 7E 04 1E 05 02 00 F7");
    if (f.fn === 0x7e && f.body[0] === 0x04 && f.body[1] === 0x41) return out("F0 40 7F 7E 04 1E 04 01 32 2E 31 39 F7");
    if (f.fn === 0x02 && f.body[0] === 0x5f) return out("F0 40 7F 03 04 1E 5F 7F 7F F7");
    if (f.fn === 0x10) return out(`F0 40 7F 07 04 1E ${hex2(f.body[0]!)} ${hex2(f.body[1]!)} ${hex2(f.body[2]!)} F7`);
  });
  return { st, out };
}

test("panel UI: the piano's 7E 01 01 after a live swap -> CONNECT + full request, fan-out reaches it again", async () => {
  const [panel, board] = LoopbackLink.pair();
  const p = piano(board);
  // the panel builds: no boot notice
  const mgr = new ConnectionManager({ makeSession: (l) => new PianoSession(l, { ackTimeoutMs: 60 }), replyMs: 300 });
  const banner = takeoverBanner(mgr);
  dom.window.document.body.append(banner.el);
  assert.equal(await mgr.attach(panel), "ready");
  const s = mgr.session!;
  const params: string[] = [];
  s.onParam((m, fromPiano) => fromPiano && params.push(`${hex2(m.v1)} ${hex2(m.v2)} ${hex2(m.data[0] ?? 0)}`));

  // "Apply now": the new core no longer has the panel; the panel's link never dropped
  p.st.bound = false;
  const before = p.st.rx.length;
  assert.deepEqual(connView(mgr.state).button, { label: "Connected", action: "menu" }, "the panel still says Connected");

  p.out(INVITE);
  await tick(5);
  assert.equal(s.stats.invited, 1);
  assert.equal(p.st.rx[before], CONNECT, "the first thing the panel sends is CONNECT");
  await mgr.settled();
  assert.equal(mgr.phase, "ready");
  assert.equal(mgr.session, s, "same session, same link");
  assert.ok(p.st.rx.slice(before).includes(REQ_ALL), "and a full parameter request");
  assert.equal(banner.el.hidden, true);

  // a fan-out frame from the core (another controller's change) is ACKed and lands in the store
  const acks = p.st.acks.length;
  p.out("F0 40 7F 10 04 1E 55 08 00 04 F7");
  await tick(5);
  assert.deepEqual(p.st.acks.slice(acks), ["F0 40 7F 07 08 02 55 08 00 F7"]);
  assert.deepEqual(params, ["55 08 04"]); // v1 v2 data
  mgr.disconnect();
});

test("7E 01 01 while displaced takes control back; ignored before the first CONNECT and mid-connect", async () => {
  const [panel, board] = LoopbackLink.pair();
  const p = piano(board);
  const mgr = new ConnectionManager({ makeSession: (l) => new PianoSession(l, { ackTimeoutMs: 60 }), replyMs: 300 });
  assert.equal(await mgr.attach(panel), "ready");
  p.st.bound = false;
  p.out("F0 40 7F 7E 04 1E 01 00 F7");
  await tick(5);
  assert.equal(mgr.phase, "displaced");
  p.out(INVITE);
  await tick(5);
  assert.equal(await mgr.settled(), "ready");
  assert.equal(p.st.bound, true);
  mgr.disconnect();

  // a session that has not CONNECTed yet: nothing
  const [a, b] = LoopbackLink.pair();
  const q = piano(b);
  const s = new PianoSession(a, { ackTimeoutMs: 60 });
  let invited = 0;
  s.onInvite(() => invited++);
  q.out(INVITE);
  await tick(5);
  assert.equal(invited, 0);
  assert.equal(q.st.rx.length, 0);
});
