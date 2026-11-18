// Full sync that loses its binding mid-dump (field 2026-10-31).
// The stock dump sender FUN_4014BD1C waits for each value's ACK with a 16-bit deadline (FUN_4014BCAC):
// for 0.5 s in every 131 s each wait times out at once, a value is resent every ~5 ms, and after 16
// sends the controller is unbound without a word — although the app ACKed every frame. The piano then
// ignores that port until the next CONNECT. connect() now CONNECTs and requests once more.
import { test } from "node:test";
import assert from "node:assert/strict";

const { PianoSession } = await import("../src/protocol/session.ts");
const { fromHex, toHex, decodeFrame } = await import("../src/protocol/sysex.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");

/** Stock-like: one binding; the first full dump stops after `cut` values and unbinds (16 unACKed sends). */
function stockPiano(piano: InstanceType<typeof LoopbackLink>, cut: number) {
  const st = { bound: false, dumps: 0, connects: 0, rx: [] as string[] };
  const out = (h: string) => piano.send(fromHex(h));
  const values = ["51 0F 7F 00", "55 04 38 00", "55 04 39 00", "55 04 3A 00", "55 04 3B 00"];
  piano.onMessage((m) => {
    st.rx.push(toHex(m));
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x01) {
      st.bound = true;
      st.connects++;
      return out("F0 40 7F 7E 04 1E 02 00 40 F7");
    }
    if (!st.bound) return;
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) return out("F0 40 7F 7E 04 1E 05 00 00 02 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x41) return out("F0 40 7F 7E 04 1E 05 01 00 01 F7");
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x42) return out("F0 40 7F 7E 04 1E 05 02 00 F7");
    if (f.fn === 0x7e && f.body[0] === 0x04 && f.body[1] === 0x41) return out("F0 40 7F 7E 04 1E 04 01 32 2E 31 39 F7");
    if (f.fn === 0x02 && f.body[0] === 0x5f) {
      st.dumps++;
      const n = st.dumps === 1 ? cut : values.length;
      for (const v of values.slice(0, n)) out(`F0 40 7F 10 04 1E ${v} F7`);
      if (st.dumps === 1) {
        st.bound = false; // FUN_4014BD1C: 16 sends without an ACK seen -> unbind, nothing sent to the app
        return;
      }
      return out("F0 40 7F 03 04 1E 5F 7F 7F F7");
    }
  });
  return st;
}

test("dump cut by the stock's unbind: connect() CONNECTs again, re-requests and gets ready", async () => {
  const [app, piano] = LoopbackLink.pair();
  const st = stockPiano(piano, 2);
  const s = new PianoSession(app);
  const id = await s.connect(40);
  assert.equal(id.model, "CA79");
  assert.equal(s.state, "ready");
  assert.equal(st.connects, 2);
  assert.equal(st.dumps, 2);
  // the second CONNECT comes after the first dump's ACKs and before the second request
  const i2 = st.rx.lastIndexOf("F0 40 7F 7E 08 02 01 01 F7");
  assert.ok(i2 > st.rx.indexOf("F0 40 7F 07 08 02 55 04 38 F7"));
  assert.ok(st.rx.indexOf("F0 40 7F 02 08 02 5F 7F 7F F7", i2) > i2);
});

test("a dump that fails twice still fails (no endless retry)", async () => {
  const [app, piano] = LoopbackLink.pair();
  const st = stockPiano(piano, 2);
  piano.onMessage((m) => {
    const f = decodeFrame(m);
    if (f && f.fn === 0x7e && f.body[0] === 0x01 && f.body[1] === 0x01) {
      st.connects++;
      piano.send(fromHex("F0 40 7F 7E 04 1E 02 00 40 F7"));
    } else if (f && f.fn === 0x7e && f.body[0] === 0x05) {
      piano.send(fromHex(`F0 40 7F 7E 04 1E 05 ${f.body[1] === 0x40 ? "00 00 02" : f.body[1] === 0x41 ? "01 00 01" : "02 00"} F7`));
    }
    // never answers the dump request
  });
  const s = new PianoSession(app);
  await assert.rejects(s.connect(30), /parameter dump timed out/);
  assert.equal(st.connects, 2);
});
