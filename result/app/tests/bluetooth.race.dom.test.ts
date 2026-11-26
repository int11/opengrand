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

