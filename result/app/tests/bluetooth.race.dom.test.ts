// 「블루투스」 page: two quick taps on the policy cards while the piano is still answering the first (field class
// 2026-11-27: independent async sequences on one link). The second write must keep the first one's change.
import { tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const i18n = await import("../src/i18n.ts");
const bt = await import("../src/protocol/btport.ts");
const { bluetoothPage } = await import("../src/ui/bluetooth.ts");

type Policy = import("../src/protocol/btport.ts").BtSourcePolicy;

/** A port whose piano holds one policy and answers each 1D 06 write `ms` later. */
function slowPort(ms: number) {
  let policy: Policy = { mode: bt.SRC_MODE.select, flags: 0, holdDs: 15, sweepS: 5, pref: null, strategy: 0, reconnect: bt.SRC_RECONNECT.prefLast, store: 0 };
  const writes: Policy[] = [];
  const sources = () => ({
    policy: { ...policy }, moduleOn: true, canAct: true, restarting: false, streams: 0, closes: 0, unanswered: 0, recoveries: 0, dual: 0, lines: 0,
    calls: 0, queued: 0, devices: [], selected: null, sw: null, autocall: null, refusals: 0, suspends: 0, lineFixes: null, health: null, swTried: 0, codec: null,
  });
  const port = {
    sources: async () => sources(),
    setPolicy: async (p: Policy) => {
      writes.push({ ...p });
      policy = { ...p };
      await new Promise((r) => setTimeout(r, ms));
      return { rc: 0, policy: { ...p } };
    },
    level: async () => null,
    moduleLog: async () => null,
    autocall: async () => null,
  };
  return { port: port as unknown as InstanceType<typeof bt.BtPort>, writes, policy: () => policy };
}

test("Bluetooth page: 「Switch to it」 then 「Reconnect: Off」 tapped quickly both stay on the piano", async () => {
  i18n.setLang("en");
  const p = slowPort(50);
  const page = bluetoothPage(() => p.port, { refreshMs: 60_000 });
  document.body.append(page);
  for (let i = 0; i < 50 && !page.querySelector("[data-reconnect]"); i++) await tick(10);
  page.querySelector<HTMLElement>("[data-mode=follow]")!.click();
  await tick(5);
  page.querySelector<HTMLElement>(`[data-reconnect="${bt.SRC_RECONNECT.off}"]`)!.click();
  for (let i = 0; i < 50 && p.writes.length < 2; i++) await tick(10);
  await tick(120);
  const now = p.policy();
  assert.ok(now.flags & bt.SRC_FLAG.follow, `the follow choice was undone by the second write: ${JSON.stringify(p.writes)}`);
  assert.equal(now.reconnect, bt.SRC_RECONNECT.off);
  page.remove();
});

test("Bluetooth page: 「Listen to this one」 on two devices in a row - each tap gets its own answer (the second one's refusal shows)", async () => {
  i18n.setLang("en");
  const { PianoSession } = await import("../src/protocol/session.ts");
  const { LoopbackLink } = await import("../src/transport/link.ts");
  const ng = await import("../src/protocol/ngext.ts");
  const [app, piano] = LoopbackLink.pair();
  const asked: number[] = [];
  // 1D 05 answered 80 ms later: the device ending in 01 is taken (rc 0), the one ending in 02 refused (rc 3, busy)
  piano.onMessage((m) => {
    if (m[1] !== 0x7d || m[6] !== 0x1d || m[7] !== 5) return;
    const last = ng.unpack7(m.slice(9, -1))[5]!;
    asked.push(last);
    setTimeout(() => piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, 0x5d, 5, last === 1 ? 0 : 3, 0xf7])), 80);
  });
  const session = new PianoSession(app, { ackTimeoutMs: 50 });
  const dev = (last: number) => ({ addr: [2, 0, 0, 0, 0, last], links: 1, a2dp: 1, state: bt.SRC_DEV.addr | bt.SRC_DEV.active, streamS: 0, state2: bt.SRC_DEV2.avrcp, refusals: 0, ps: null });
  const real = new bt.BtPort(new ng.NgClient(session));
  const port = Object.assign(Object.create(real), {
    sources: async () => ({
      policy: { mode: bt.SRC_MODE.select, flags: 0, holdDs: 15, sweepS: 5, pref: null }, moduleOn: true, canAct: true, restarting: false, streams: 0, closes: 0,
      unanswered: 0, recoveries: 0, dual: 0, lines: 0, calls: 0, queued: 0, devices: [dev(1), dev(2)], selected: null, sw: null, autocall: null, refusals: 0,
      suspends: 0, lineFixes: null, health: null, swTried: 0, codec: null,
    }),
    level: async () => null,
    moduleLog: async () => null,
    autocall: async () => null,
  }) as InstanceType<typeof bt.BtPort>;
  const page = bluetoothPage(() => port, { refreshMs: 60_000 });
  document.body.append(page);
  for (let i = 0; i < 50 && page.querySelectorAll("[data-action=use]").length < 2; i++) await tick(10);
  const [a, b] = [...page.querySelectorAll<HTMLElement>("[data-action=use]")];
  a!.click();
  await tick(10);
  b!.click();
  for (let i = 0; i < 50 && asked.length < 2; i++) await tick(10);
  await tick(150);
  assert.deepEqual(asked, [1, 2]);
  assert.match(page.querySelector("#bt-src-msg")!.textContent ?? "", /busy/, "the second tap took the first tap's answer");
  page.remove();
});

