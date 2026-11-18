// Controller displaced (7E 01 00) — INDEX-displaced.
// Field case (2026-10-31, PC Chrome over BLE): while the Files tab scanned the internal songs the
// panel CONNECTed, the stock handed it the binding and sent us `F0 40 7F 7E 04 1E 01 00 F7`; from
// then on it ignored every request, and the scan resent each SongSelect 16 times, song after song.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const { PianoSession, DisplacedError } = await import("../src/protocol/session.ts");
const { InternalSongs } = await import("../src/protocol/songs.ts");
const { fromHex, toHex, decodeFrame } = await import("../src/protocol/sysex.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const { takeoverBanner } = await import("../src/ui/takeover.ts");
const { ConnectionManager } = await import("../src/state/connection.ts");
const i18n = await import("../src/i18n.ts");

const DISPLACED = "F0 40 7F 7E 04 1E 01 00 F7";
const CONNECT = "F0 40 7F 7E 08 02 01 01 F7";
const hex2 = (n: number) => n.toString(16).padStart(2, "0").toUpperCase();

/**
 * Stock-like piano with one controller binding. Answers the connect sequence and the recorder
 * probes; after `displaceAfter` SongSelects it gives the binding away (7E 01 00) and stays silent
 * for this port until the next CONNECT.
 */
function stockPiano(piano: InstanceType<typeof LoopbackLink>, displaceAfter: number) {
  const st = { bound: false, selects: 0, rx: [] as { t: number; hex: string }[], displacedAt: 0, displacedIdx: 0 };
  const out = (h: string) => piano.send(fromHex(h));
  piano.onMessage((m) => {
    const hex = toHex(m);
    st.rx.push({ t: Date.now(), hex });
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x01) {
      st.bound = true;
      st.selects = 0;
      if (st.displacedAt) displaceAfter = Infinity; // taken back: this binding is kept
      return out("F0 40 7F 7E 04 1E 02 00 40 F7");
    }
    if (!st.bound) return; // stock: an unbound port gets no answer at all
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) return out("F0 40 7F 7E 04 1E 05 00 00 02 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x41) return out("F0 40 7F 7E 04 1E 05 01 00 01 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x42) return out("F0 40 7F 7E 04 1E 05 02 00 F7");
    if (f.fn === 0x7e && f.body[0] === 0x04 && f.body[1] === 0x41) return out("F0 40 7F 7E 04 1E 04 01 32 2E 31 39 F7");
    if (f.fn === 0x02 && f.body[0] === 0x5f) return out("F0 40 7F 03 04 1E 5F 7F 7F F7");
    if (f.fn === 0x10 && f.body[0] === 0x52 && f.body[1] === 0x00) {
      if (++st.selects > displaceAfter) {
        // the panel CONNECTed in between: the old controller is told and dropped
        st.bound = false;
        st.displacedAt = Date.now();
        st.displacedIdx = st.rx.length; // everything the app sends from here on
        return out(DISPLACED);
      }
      return [`F0 40 7F 07 04 1E 52 00 7F F7`, `F0 40 7F 10 04 1E 52 18 7F 00 00 ${hex2(4)} 00 00 00 F7`].forEach(out);
    }
    if (f.fn === 0x10) return out(`F0 40 7F 07 04 1E ${hex2(f.body[0]!)} ${hex2(f.body[1]!)} ${hex2(f.body[2]!)} F7`);
    if (f.fn === 0x31 && f.body[0] === 0x52) return out(`F0 40 7F 32 04 1E 52 1D 7F 02 F7`);
  });
  return st;
}

test("field case: 7E 01 00 during a song scan stops the scan, no resends, link not broken", async () => {
  const [app, piano] = LoopbackLink.pair();
  const st = stockPiano(piano, 3);
  const s = new PianoSession(app, { ackTimeoutMs: 60, maxRetries: 16 });
  const states: string[] = [];
  s.onState((x) => states.push(x));
  await s.connect(300);
  assert.equal(s.state, "ready");

  const songs = new InternalSongs(s, 200);
  await assert.rejects(songs.scan(), (e: unknown) => e instanceof DisplacedError);
  // without the fix: 16 sends x 60 ms for the unanswered SongSelect, then the same for every
  // remaining request of the scan (~30 of them)
  assert.ok(st.displacedAt > 0);
  assert.ok(Date.now() - st.displacedAt < 100, `scan stopped ${Date.now() - st.displacedAt} ms after 7E 01 00`);
  assert.equal(s.state, "displaced");
  assert.ok(!states.includes("closed"), "the connection is not marked broken");

  // nothing is sent after the displacement: no resend of the unanswered SongSelect, no next song
  const sentAfter = () => st.rx.length - st.displacedIdx;
  await tick(400);
  assert.equal(sentAfter(), 0, `frames sent after 7E 01 00: ${st.rx.slice(st.displacedIdx).map((r) => r.hex).join(" | ")}`);
  assert.equal(s.stats.retried, 0);

  // a new request fails at once and is not sent either
  assert.equal(await s.sendParam({ v1: 0x55, v2: 0x01, part: 0x00, data: [0x40] }), false);
  assert.equal(sentAfter(), 0);

  // take control again: CONNECT, identity and the full dump, then requests work
  const id = await s.takeControl(300);
  assert.equal(id.model, "CA79");
  assert.equal(s.state, "ready");
  const tx = st.rx.slice(st.displacedIdx).map((r) => r.hex);
  assert.equal(tx[0], CONNECT);
  assert.ok(tx.includes("F0 40 7F 02 08 02 5F 7F 7F F7"), "full re-sync requested");
  assert.equal(await s.sendParam({ v1: 0x55, v2: 0x01, part: 0x00, data: [0x40] }), true);
  const info = await songs.info(0, false);
  assert.equal(info.bars, 4);
});

test("displaced: an in-flight request and the queue behind it resolve false at once", async () => {
  const [app, piano] = LoopbackLink.pair();
  const st = stockPiano(piano, Infinity);
  const s = new PianoSession(app, { ackTimeoutMs: 1000 });
  await s.connect(300);
  st.bound = false; // the piano stops answering ...
  const a = s.sendParam({ v1: 0x55, v2: 0x02, part: 0x00, data: [1] });
  const b = s.sendParam({ v1: 0x55, v2: 0x03, part: 0x00, data: [1] });
  await tick(10);
  piano.send(fromHex(DISPLACED)); // ... and says why
  const t0 = Date.now();
  assert.deepEqual(await Promise.all([a, b]), [false, false]);
  assert.ok(Date.now() - t0 < 100);
  assert.equal(s.stats.dropped, 2);
  // 7E 01 01 from the piano (OpenGrand asks the panel to reconnect) is not a displacement
  const [app2, piano2] = LoopbackLink.pair();
  stockPiano(piano2, Infinity);
  const s2 = new PianoSession(app2);
  await s2.connect(300);
  piano2.send(fromHex("F0 40 7F 7E 04 1E 01 01 F7"));
  await tick(5);
  assert.equal(s2.state, "ready");
});

test("banner: 'Another device took control' with a button that takes it back", async () => {
  // the banner draws the connection state (src/state/connection.ts); its button is the manager's "retake"
  i18n.setLang("en");
  const [app, piano] = LoopbackLink.pair();
  const st = stockPiano(piano, Infinity);
  const mgr = new ConnectionManager({ makeSession: (l) => new PianoSession(l, { ackTimeoutMs: 60 }), replyMs: 300 });
  const banner = takeoverBanner(mgr);
  dom.window.document.body.append(banner.el);
  assert.equal(await mgr.attach(app), "ready");
  const s = mgr.session!;
  assert.equal(banner.el.hidden, true);

  st.bound = false;
  piano.send(fromHex(DISPLACED));
  await tick(5);
  assert.equal(banner.el.hidden, false);
  assert.match(banner.el.textContent ?? "", /Another device .*took control/);
  const btn = banner.el.querySelector<HTMLButtonElement>("[data-action=take-control]")!;
  assert.equal(btn.textContent, "Take control again");
  const before = st.rx.length;
  btn.click();
  assert.equal(btn.disabled, true, "no second click while taking control");
  await tick(60);
  assert.equal(st.rx[before]?.hex, CONNECT);
  assert.equal(s.state, "ready");
  assert.equal(mgr.session, s, "same session, same link");
  assert.equal(banner.el.hidden, true);

  // displaced again, then the user disconnects: the banner goes away
  st.bound = false;
  piano.send(fromHex(DISPLACED));
  await tick(5);
  assert.equal(banner.el.hidden, false);
  mgr.disconnect();
  assert.equal(banner.el.hidden, true);
  banner.el.remove();
});

test("second ota_sysex field failure: once displaced a data request is not sent, and the session says why", async () => {
  // Field 2026-10-31: the panel took the binding back, the app went
  // "displaced" and dropped every set without a TX line, but the Concert Magic screen's `02 52 0D 7F`
  // still went out and was never answered - it looked like a stuck queue. Now the request is dropped too,
  // and trace lines + describe() make the queue state visible in the MIDI log.
  const [app, piano] = LoopbackLink.pair();
  const st = stockPiano(piano, Infinity);
  const lines: string[] = [];
  const s = new PianoSession(app, { ackTimeoutMs: 1000, trace: (l) => lines.push(l) });
  await s.connect(300);
  assert.equal(s.describe(), "ready");
  st.bound = false;
  const a = s.sendParam({ v1: 0x55, v2: 0x08, part: 0x00, data: [4] });
  const b = s.sendParam({ v1: 0x55, v2: 0x08, part: 0x00, data: [0] });
  assert.equal(s.describe(), "ready inflight=55 08 00#1 queued=1");
  piano.send(fromHex(DISPLACED));
  assert.deepEqual(await Promise.all([a, b]), [false, false]);
  const n0 = st.rx.length;
  assert.equal(await s.requestData(0x52, 0x0d, 1500), false);
  assert.equal(await s.sendParam({ v1: 0x60, v2: 0x00, part: 0x7f, data: [3] }), false);
  await tick(20);
  assert.equal(st.rx.length, n0, "nothing sent while displaced");
  assert.equal(s.describe(), "displaced");
  assert.ok(lines.includes("state ready -> displaced"), lines.join(" | "));
  assert.ok(lines.includes("drop request 52 d (displaced)"), lines.join(" | "));
  assert.ok(lines.includes("drop 60 00 7f (displaced)"), lines.join(" | "));
});
