// Switching the piano on with the phone / PC app already waiting (field 2026-11-18, escape boot = stock firmware).
// The app reconnects first; the built-in panel's start-up CONNECT then takes the stock's single binding. The panel
// UI (main.ts, KWMcore shell) keeps trying until one of its sessions connects: a session that gets no answer fails,
// and the next try comes 3 s later. Any CONNECT of such a try displaces whoever holds the binding — the stock sends
// 7E 01 00 to the old controller on every CONNECT from another port (0x4014A774) and asks nobody back.
//
// Here: a fake stock piano with two ports (single binding, as the stock) or with the session core (ota_main: both
// bound, nobody displaced), the app's ConnectionManager, and a panel driver that runs main.ts's tryLink loop.
import { test } from "node:test";
import assert from "node:assert/strict";

const { ConnectionManager, connView } = await import("../src/state/connection.ts");
type Phase = import("../src/state/connection.ts").Phase;
const { PianoSession } = await import("../src/protocol/session.ts");
const { fromHex, decodeFrame } = await import("../src/protocol/sysex.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
type MidiLink = import("../src/transport/link.ts").MidiLink;

const tick = (ms = 20) => new Promise((r) => setTimeout(r, ms));

type Port = "app" | "panel";

/**
 * Two controllers on one piano. Stock (`coBound` false): the last CONNECT holds the binding, a CONNECT from the
 * other port sends 7E 01 00 to the one bound before, an unbound port gets no answer. Session core (`coBound` true):
 * every CONNECT binds its port too, nobody is displaced (firmware tests/test_kpsess.c "the panel CONNECTs while
 * the app is bound"). `deafUntil[p]`: frames to that port are lost before this time (a shell whose MIDI input is
 * not open yet, a page still loading).
 */
function piano(opt: { coBound?: boolean } = {}) {
  const t0 = Date.now();
  const st = { t0, bound: new Set<Port>(), connects: { app: 0, panel: 0 }, displaced: { app: 0, panel: 0 }, deafUntil: { app: 0, panel: 0 } };
  const ends: Record<Port, InstanceType<typeof LoopbackLink>> = {} as never;
  const links: Record<Port, InstanceType<typeof LoopbackLink>> = {} as never;
  const out = (p: Port, h: string) => { if (Date.now() - t0 >= st.deafUntil[p]) ends[p].send(fromHex(h)); };
  for (const p of ["app", "panel"] as Port[]) {
    const [ctl, pia] = LoopbackLink.pair();
    links[p] = ctl;
    ends[p] = pia;
    pia.onMessage((m) => {
      const f = decodeFrame(m);
      if (!f) return;
      if (f.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x01) {
        st.connects[p]++;
        const other: Port = p === "app" ? "panel" : "app";
        if (!opt.coBound && st.bound.has(other)) {
          st.displaced[other]++;
          out(other, "F0 40 7F 7E 04 1E 01 00 F7");
          st.bound.delete(other);
        }
        st.bound.add(p);
        return out(p, "F0 40 7F 7E 04 1E 02 00 40 F7");
      }
      if (!st.bound.has(p)) return;
      if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) return out(p, "F0 40 7F 7E 04 1E 05 00 00 02 F7");
      if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x41) return out(p, "F0 40 7F 7E 04 1E 05 01 00 01 F7");
      if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x42) return out(p, "F0 40 7F 7E 04 1E 05 02 00 F7");
      if (f.fn === 0x7e && f.body[0] === 0x04 && f.body[1] === 0x41) return out(p, "F0 40 7F 7E 04 1E 04 01 32 2E 31 39 F7");
      if (f.fn === 0x02 && f.body[0] === 0x5f) return out(p, "F0 40 7F 03 04 1E 5F 7F 7F F7");
    });
  }
  return { st, links };
}

const fast = (l: MidiLink) => new PianoSession(l, { ackTimeoutMs: 60 });

/** The panel UI's auto-connect (main.ts `tryLink`): attach; not ready / displaced → try again `retryMs` later. */
function panelUi(link: MidiLink, retryMs: number) {
  const m = new ConnectionManager({ makeSession: fast, replyMs: 300 }); // panel builds: no boot window
  const phases: Phase[] = [];
  m.onChange((s) => { if (phases[phases.length - 1] !== s.phase) phases.push(s.phase); });
  let stopped = false;
  const tryLink = async (): Promise<void> => {
    if (stopped || !["idle", "lost", "failed"].includes(m.phase)) return;
    const ph = await m.attach(link);
    if (ph !== "ready" && ph !== "waking" && ph !== "displaced") setTimeout(() => void tryLink(), retryMs);
  };
  return { m, phases, start: () => void tryLink(), stop: () => { stopped = true; } };
}

// connect(): 4 tries ≥ 250 ms apart → a session that hears nothing fails after ~1 s; the panel tries again 0.3 s later
const RETRY = 300;
const WINDOW = 5000;

// With the old rule (take control back once, here 1.1 s after) this scenario gave the field order of 2026-11-18:
// app connecting > syncing > ready > displaced > retaking > ready > displaced ("Take control again"),
// panel connecting > failed > connecting > syncing > ready — the re-grab landed while the panel's failed session
// was gone, and its next try took control again.
test("stock, piano switched on with the app waiting: the panel's start-up takes control and keeps it — the app never fights back", async () => {
  const { st, links } = piano();
  const app = new ConnectionManager({ makeSession: fast, replyMs: 300, bootNotice: { windowMs: WINDOW } });
  const appPhases: Phase[] = [];
  app.onChange((s) => { if (appPhases[appPhases.length - 1] !== s.phase) appPhases.push(s.phase); });
  assert.equal(await app.attach(links.app), "ready");
  // the panel starts; its first session hears nothing (input not open yet) — its CONNECT still binds it
  st.deafUntil.panel = Date.now() - st.t0 + 1200;
  const panel = panelUi(links.panel, RETRY);
  panel.start();
  await tick(60);
  assert.equal(app.phase, "displaced", "the panel's first CONNECT displaced the app");
  const v = connView(app.state);
  assert.match(v.banner!.msg[0], /panel took control as it started up/);
  assert.equal(v.banner!.button!.label, "Use this app");
  // the panel's start-up runs its course: a failed try, then one that connects
  await tick(3000);
  assert.equal(panel.m.phase, "ready");
  assert.equal(app.phase, "displaced");
  assert.equal(st.connects.app, 1, "the app sent no CONNECT of its own after the panel took over");
  assert.equal(st.displaced.panel, 0, "the panel was never displaced by the app");
  assert.ok(st.connects.panel >= 2, "the panel tried more than once (a failed start-up session)");
  assert.deepEqual(appPhases, ["connecting", "syncing", "ready", "displaced"]);
  // the user can still choose the app: one CONNECT, the panel shows its own banner (and stays there)
  app.retake();
  assert.equal(await app.settled(), "ready");
  await tick(50);
  assert.equal(panel.m.phase, "displaced");
  assert.equal(st.displaced.panel, 1);
  panel.stop();
});

test("session core (ota_main): the panel's start-up CONNECT displaces nobody — both stay connected, nothing to show", async () => {
  const { st, links } = piano({ coBound: true });
  const app = new ConnectionManager({ makeSession: fast, replyMs: 300, bootNotice: { windowMs: WINDOW } });
  assert.equal(await app.attach(links.app), "ready");
  const panel = panelUi(links.panel, RETRY);
  panel.start();
  assert.equal(await panel.m.settled(), "ready");
  await tick(100);
  assert.equal(app.phase, "ready");
  assert.equal(connView(app.state).banner, null);
  assert.deepEqual([st.displaced.app, st.displaced.panel, st.connects.app], [0, 0, 1]);
  panel.stop();
});
