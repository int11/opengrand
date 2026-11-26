// Settings > Power: quick edits of two settings while the piano is still saving the first (field class 2026-11-27:
// independent async sequences on one link). Each write must start from the policy the piano last confirmed plus
// every edit made since, and the card must end up showing what the piano holds.
import { change, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const { PianoSession } = await import("../src/protocol/session.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { createContext } = await import("../src/ui/context.ts");
const { powerPolicySection } = await import("../src/ui/power.ts");
const ng = await import("../src/protocol/ngext.ts");
const { fromHex, decodeFrame } = await import("../src/protocol/sysex.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const i18n = await import("../src/i18n.ts");

async function until(cond: () => boolean, ms = 3000): Promise<void> {
  for (let t = 0; t < ms && !cond(); t += 10) await tick(10);
}

/** A piano that keeps one policy and answers each 18 01 (set) `saveMs` later with the policy now in force. */
async function slowSavePiano(saveMs: number) {
  const [app, piano] = LoopbackLink.pair();
  const out = (b: number[]) => piano.send(Uint8Array.from(b));
  const hdr = [0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39];
  let pol = ng.encodePowerPolicy(ng.POWER_POLICY_DEFAULT);
  const sets: number[][] = [];
  piano.onMessage((m) => {
    if (m[1] === 0x7d) {
      const op = m[6]!, sub = m[7] ?? 0;
      if (op === 0x01) piano.send(fromHex("F0 7D 43 41 37 39 41 01 02 13 3B 57 F7"));
      if (op === 0x18 && sub === 0) out([...hdr, 0x58, 0, 0, ...ng.pack7(pol), 0xf7]);
      if (op === 0x18 && sub === 1) {
        const raw = [...ng.unpack7(m.slice(8, -1))];
        sets.push(raw);
        pol = raw;
        const reply = [...hdr, 0x58, 1, 0, ...ng.pack7(raw), 0xf7];
        setTimeout(() => out(reply), saveMs);
      }
      if (op === 0x19) piano.send(fromHex("F0 7D 43 41 37 39 59 00 00 00 F7"));
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
  return { session, sets, policy: () => ng.decodePowerPolicy(Uint8Array.from(pol))! };
}

test("Settings > Power: two quick edits both reach the piano and the card shows what the piano holds", async () => {
  i18n.setLang("en");
  const p = await slowSavePiano(60);
  const store = new ParamStore();
  const sec = powerPolicySection(createContext(store, { session: () => p.session }));
  document.body.append(sec);
  await until(() => !sec.hidden && !!sec.querySelector("select[name=standbyCap]"));
  const sel = (n: string) => sec.querySelector<HTMLSelectElement>(`select[name=${n}]`)!;
  // the first edit is still being saved when the second is made
  change(sel("apoAction"), "0"); // Power off instead of standby
  await tick(10);
  change(sel("standbyCap"), "120");
  await until(() => p.sets.length >= 2, 1000);
  await tick(150);
  const onPiano = p.policy();
  assert.equal(onPiano.apoStandby, false, `the first edit was undone on the piano by the second write: ${JSON.stringify(p.sets)}`);
  assert.equal(onPiano.standbyCapMin, 120);
  assert.equal(sel("apoAction").value, "0", "card: auto power-off action as on the piano");
  assert.equal(sel("standbyCap").value, "120", "card: standby limit as on the piano");
  sec.remove();
});
