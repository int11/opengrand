// The sender (PanelUiClient + the Information box) against the piano's relay model (tests/_puirelay.ts = the
// cave's relay_cave.c rules) and the real panel loader: window 1 and 2, an aborted transfer,
// no panel UI, a stalled panel (ack timeouts), a busy panel ring (R4 drops), and the relay rules themselves.
import { dom, tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";
import { randomBytes } from "node:crypto";
import { JSDOM, VirtualConsole } from "jsdom";

import { probeFiles } from "../scripts/bundlecore.ts";

import { keyFromPem, makePackage, newKeyPem, uiConfig } from "../scripts/panelpkg.ts";
import { PanelUiClient, PuiControl, puiRequest, PanelUiOp } from "../src/protocol/panelui.ts";
import { panelUiBox } from "../src/ui/paneluibox.ts";
import type { AppContext } from "../src/ui/context.ts";
import { MemStorage, PanelRelayModel, panelLoader, RL_RESERVE, RING_LIMIT } from "./_puirelay.ts";

const KEY = keyFromPem(newKeyPem());
const CFG = uiConfig(KEY, { version: "0.2.0", seq: 1000 });
const pkg = (seq: number, size = 6000) =>
  new Uint8Array(makePackage({ js: `window.__ca79ngBooted = true; /* ${randomBytes(size).toString("hex")} */`, css: "" }, { version: `0.3.${seq % 100}`, seq, key: KEY }));

/** fast = a 10x faster panel link than the real 31.25 kbaud [unverified] one, to keep the suite short */
function rig(withPanel = true, fast = true) {
  const store = new MemStorage();
  const relay = new PanelRelayModel(withPanel ? panelLoader(store, CFG) : null);
  if (fast) relay.panelBytesPerMs = 31.25;
  return { store, relay, cli: new PanelUiClient(relay.phone()) };
}

const storedA = (store: MemStorage) => store.getItem("ca79ng.ui.slot.A");
const b64 = (u: Uint8Array) => Buffer.from(u).toString("base64");

test("relay model: window 1 and window 2 store the same bytes; every frame passes verbatim, nothing dropped", async () => {
  const file = pkg(2000);
  const ms: number[] = [];
  for (const window of [1, 2]) {
    const { store, relay, cli } = rig(true, false); // the real link rate: the window matters here
    const t0 = Date.now();
    const fin = await cli.upload(file, { window, ackTimeoutMs: 2000 });
    ms.push(Date.now() - t0);
    relay.stop();
    assert.equal(fin.phase, "done", fin.message);
    assert.equal(storedA(store), b64(file), `window ${window}: slot A == the package`);
    const s = relay.stats;
    assert.equal(s.dropLen + s.dropDir + s.dropNoSrc + s.dropFull, 0);
    assert.equal(s.toPanel, s.fromPanel, "one reply per request");
    assert.ok(relay.port3.every((f) => f.length <= 245), "phone frames <= 245 B (Data 240 + CRC-32)");
    assert.ok(s.peak3 + 245 + RL_RESERVE <= RING_LIMIT, `window ${window} keeps the stock reserve (peak ${s.peak3})`);
    if (window === 2) assert.ok(s.peak3 >= 200, "window 2 queues a second frame behind the first");
  }
  assert.ok(ms[1]! < ms[0]!, `window 2 faster (${ms})`);
});

test("relay model: an aborted transfer (Control 4) fails cleanly; the next upload starts over and finishes", async () => {
  const { store, relay, cli } = rig();
  const file = pkg(2100, 12000);
  let aborted = false;
  const fin = await cli.upload(file, {
    window: 2,
    onProgress: (p) => {
      if (!aborted && p.phase === "upload" && p.sent > p.total * 0.4) {
        aborted = true;
        void new PanelUiClient(relay.phone()).control(PuiControl.Abort);
      }
    },
  });
  assert.ok(aborted);
  assert.equal(fin.phase, "error");
  assert.equal(fin.rc, 8, "the panel stopped receiving: Data answers rc 8");
  const st = await cli.status();
  assert.equal(st?.rxState, 0);
  const again = await cli.upload(file);
  relay.stop();
  assert.equal(again.phase, "done", again.message);
  assert.equal(storedA(store), b64(file));
});

test("relay model: a sender that vanishes mid-way, the next one resumes at the panel's received", async () => {
  const { store, relay, cli } = rig();
  const file = pkg(2200, 12000);
  // the first sender's link dies at 50 %: its frames stop reaching the piano
  const phone = relay.phone();
  let alive = true;
  const dying = new PanelUiClient({ sendRaw: (m) => alive && phone.sendRaw(m), onMidi: phone.onMidi });
  const first = await dying.upload(file, {
    ackTimeoutMs: 50, maxRetries: 1,
    onProgress: (p) => {
      if (p.phase === "upload" && p.sent > p.total / 2) alive = false;
    },
  });
  assert.equal(first.phase, "error");
  const st = (await cli.status())!;
  assert.equal(st.rxState, 1);
  assert.ok(st.received > 0 && st.received < st.total);
  let firstSent = -1;
  const fin = await cli.upload(file, { onProgress: (p) => p.phase === "upload" && firstSent < 0 && (firstSent = p.sent) });
  relay.stop();
  assert.equal(fin.phase, "done", fin.message);
  assert.ok(firstSent >= st.received, `resumed at ${firstSent} >= ${st.received}`);
  assert.equal(storedA(store), b64(file));
});

test("relay model: no panel UI -> no reply, the box says so, the upload gives up; the relay drops nothing", async () => {
  const { relay, cli } = rig(false);
  assert.equal(await cli.status(300), null);
  const box = panelUiBox({ session: () => relay.phone() } as unknown as AppContext);
  dom.window.document.body.append(box);
  (box.querySelector("#pui-check") as HTMLButtonElement).click();
  await tick(1700);
  assert.match(box.querySelector("#pui-state")!.textContent!, /No reply/);
  box.remove();
  const fin = await new PanelUiClient(relay.phone()).upload(pkg(2300), { ackTimeoutMs: 50 });
  relay.stop();
  assert.equal(fin.phase, "error");
  assert.match(fin.message!, /no reply/);
  assert.equal(relay.stats.fromPanel, 0);
  assert.equal(relay.stats.dropLen + relay.stats.dropDir + relay.stats.dropFull, 0);
  assert.ok(relay.stats.toPanel >= 4, "the requests were relayed (Status + 3 Begins)");
});

test("relay model: the panel stalls mid-transfer -> ack timeouts, Status resume, done", async () => {
  const { store, relay, cli } = rig();
  const file = pkg(2400, 16000);
  let stalled = false;
  const fin = await cli.upload(file, {
    ackTimeoutMs: 150,
    onProgress: (p) => {
      if (!stalled && p.phase === "upload" && p.sent > p.total / 2) {
        stalled = true;
        relay.stallUntil = Date.now() + 400;
      }
    },
  });
  relay.stop();
  assert.ok(stalled);
  assert.equal(fin.phase, "done", fin.message);
  assert.ok(fin.retries >= 0);
  assert.equal(storedA(store), b64(file));
});

test("relay model: a stock dump filling the panel ring -> the relay drops whole frames (R4), the sender recovers", async () => {
  const { store, relay, cli } = rig();
  const file = pkg(2500, 16000);
  let busy = false;
  const fin = await cli.upload(file, {
    ackTimeoutMs: 300,
    onProgress: (p) => {
      if (!busy && p.phase === "upload" && p.sent > p.total / 3) {
        busy = true;
        relay.stockBacklog3 = 1500; // stock frames ahead of us on the panel link
      }
    },
  });
  relay.stop();
  assert.equal(fin.phase, "done", fin.message);
  assert.ok(relay.stats.dropFull >= 1, "dropped, not waited for");
  assert.equal(storedA(store), b64(file));
});

test("relay model: R1-R3 - length, direction, orphan replies, the latest asker gets the reply", async () => {
  const { relay } = rig();
  const got: Uint8Array[] = [];
  relay.phone().onMidi((m) => got.push(m));
  relay.rx(2, puiRequest(PanelUiOp.Data, new Array(249).fill(1))); // 257 B
  relay.rx(3, puiRequest(PanelUiOp.Status)); // a request from the panel
  relay.rx(2, puiRequest(0x70)); // a reply from the phone's port
  relay.rx(3, puiRequest(0x70)); // a reply nobody asked for
  assert.deepEqual([relay.stats.dropLen, relay.stats.dropDir, relay.stats.dropNoSrc, relay.stats.toPanel], [1, 2, 1, 0]);
  relay.rx(0, puiRequest(PanelUiOp.Status)); // DIN asks: the reply goes to port 0, not the phone
  await tick(30);
  relay.stop();
  assert.equal(relay.stats.toPanel, 1);
  assert.equal(relay.stats.fromPanel, 1);
  assert.equal(got.length, 0, "the phone (port 2) did not get port 0's reply");
});

test("pack --probe: the small first-transfer page (with the screen reporter) goes through the relay, shows the quota and rolls back", async () => {
  const files = probeFiles("0.2.1");
  const file = new Uint8Array(makePackage(files, { version: "0.2.1", seq: 2600, key: KEY }));
  // ~2 KB page + the screen reporter (panelui/panelscreen.js minified, ~8.6 KB deflated)
  assert.ok(file.length < 12000, `small: ${file.length} B`);
  const { store, relay, cli } = rig();
  const fin = await cli.upload(file, { activate: true });
  relay.stop();
  assert.equal(fin.phase, "done", fin.message);
  assert.equal(storedA(store), b64(file));
  // the page itself (the loader injects js + css into ng/index.html)
  const calls: number[] = [];
  let quotaRuns = 0;
  const vc = new VirtualConsole(); // location.reload() is "not implemented" in jsdom: keep it quiet
  const w = new JSDOM("<!doctype html><body></body>", { runScripts: "outside-only", virtualConsole: vc }).window;
  Object.assign(w, { CA79UI: { probeQuota: () => (quotaRuns++, 5238784), state: () => ({ running: "A" }), control: (s: number) => (calls.push(s), 0) } });
  w.eval(files.js);
  assert.equal((w as unknown as { __ca79ngBooted: boolean }).__ca79ngBooted, true);
  // field 2026-11-02 (record 04 §5): the quota probe ran before the first paint and froze the panel for seconds;
  // the page now paints first and measures only on a tap
  const text = () => w.document.getElementById("probe")!.textContent!;
  assert.equal(quotaRuns, 0, "nothing heavy before the first paint");
  assert.match(text(), /quota \(UTF-16 chars\): not measured[\s\S]*viewport: \d+x\d+[\s\S]*"running":"A"/);
  const btn = (id: string) => w.document.getElementById(id) as HTMLButtonElement;
  btn("probe-measure").click();
  assert.match(text(), /measuring/, "says so before the freeze");
  assert.equal(quotaRuns, 0, "measured after that line could be painted");
  await new Promise((r) => setTimeout(r, 150));
  assert.equal(quotaRuns, 1);
  assert.match(text(), /quota \(UTF-16 chars\): 5238784 \(\d+ ms\)/);
  // a tap queued during the measuring freeze lands right after it: dropped
  btn("probe-back").click();
  assert.equal(btn("probe-back").textContent, "Roll back / 되돌리기", "queued tap ignored");
  await new Promise((r) => setTimeout(r, 550));
  // roll back takes two taps
  btn("probe-back").click();
  assert.deepEqual(calls, [], "one tap only arms roll back");
  assert.match(btn("probe-back").textContent!, /Tap again/);
  btn("probe-back").click();
  assert.deepEqual(calls, [2], "roll back = Control 2 (then the page reloads into the previous version)");
  w.close();
});
