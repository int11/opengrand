// Settings > Startup settings: the mode changed twice while the piano is still saving the first one (field class
// 2026-11-27: independent async sequences on one link). The card must end up showing the mode the piano holds.
import { change, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const { PianoSession } = await import("../src/protocol/session.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { createContext } = await import("../src/ui/context.ts");
const { startupSection } = await import("../src/ui/startup.ts");
const ng = await import("../src/protocol/ngext.ts");
const { fromHex, decodeFrame } = await import("../src/protocol/sysex.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const i18n = await import("../src/i18n.ts");

async function until(cond: () => boolean, ms = 3000): Promise<void> {
  for (let t = 0; t < ms && !cond(); t += 10) await tick(10);
}

/** A piano with the startup feature (feat2 0x08) that answers each 1C 01 (set mode) `saveMs` later. */
async function slowStartupPiano(saveMs: number) {
  const [app, piano] = LoopbackLink.pair();
  const hdr = [0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39];
  let mode = 0;
  const status = () => {
    const raw = new Array(40).fill(0);
    raw[0] = 1;
    raw[1] = mode;
    raw[37] = 0xff;
    return ng.pack7(raw);
  };
  piano.onMessage((m) => {
    if (m[1] === 0x7d) {
      const op = m[6]!, sub = m[7] ?? 0;
      if (op === 0x01) piano.send(fromHex("F0 7D 43 41 37 39 41 01 02 13 3B 57 08 F7"));
      if (op === 0x1c && sub === 0) piano.send(Uint8Array.from([...hdr, 0x5c, 0, 0, ...status(), 0xf7]));
      if (op === 0x1c && sub === 1) {
        mode = m[8]!;
        const reply = Uint8Array.from([...hdr, 0x5c, 1, 0, ...status(), 0xf7]);
        setTimeout(() => piano.send(reply), saveMs);
      }
      return;
    }
    const f = decodeFrame(m);
    if (!f) return;
    if (f.fn === 0x7e && f.body[0] === 0x01) piano.send(fromHex("F0 40 7F 7E 04 1E 02 00 40 F7"));
    if (f.fn === 0x7e && f.body[0] === 0x05 && f.body[1] === 0x40) piano.send(fromHex("F0 40 7F 7E 04 1E 05 00 00 02 F7"));
    if (f.fn === 0x02) piano.send(fromHex("F0 40 7F 03 04 1E 5F 7F 7F F7"));
  });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  await session.connect(100);
  return { session, mode: () => ng.StartupModes[mode] };
}

test("Startup settings: the mode changed twice quickly - the card shows the mode the piano holds", async () => {
  i18n.setLang("en");
  const p = await slowStartupPiano(60);
  const store = new ParamStore();
  const card = startupSection(createContext(store, { session: () => p.session }));
  document.body.append(card);
  const sel = () => card.querySelector<HTMLSelectElement>("select[name=startupMode]")!;
  await until(() => !!card.querySelector("select[name=startupMode]"));
  change(sel(), "current");
  await tick(10);
  change(sel(), "poweroff");
  await tick(250);
  assert.equal(p.mode(), "poweroff");
  assert.equal(sel().value, "poweroff", "the card shows the first answer's mode, not the piano's");
  card.remove();
});
