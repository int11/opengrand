// BT port (0x7D op 0x1D, slot payload ota_bt; REMOTE_PROTOCOL.md section 10.6): Identify feat2, the
// client against a simulated piano that behaves like result/firmware/ports/cave/bt_cave.c (query state
// machine, 48-byte text chunks, switches only from the bound port), and the Information screen button.
import { tick } from "./_dom.ts";
import { test } from "node:test";
import assert from "node:assert/strict";

const ng = await import("../src/protocol/ngext.ts");
const bt = await import("../src/protocol/btport.ts");
const { PianoSession } = await import("../src/protocol/session.ts");
const { LoopbackLink } = await import("../src/transport/link.ts");
const { ParamStore } = await import("../src/state/store.ts");
const { createContext } = await import("../src/ui/context.ts");
const { infoScreen } = await import("../src/ui/info.ts");

const MODULE_TEXT = "> SET\r\nSET BT NAME CA79\r\nSET BT PAGEMODE 3 2000 1\r\nSET PROFILE A2DP SINK\r\nSET\r\n> LIST\r\nLIST 0\r\n" +
  "> INFO\r\nWRAP THOR AI (4.1.0 build 1066)\r\nCopyright (c) 2003-2011 Bluegiga Technologies Inc.\r\n";

function btPiano(opts: { feat2?: boolean; moduleOff?: boolean; bound?: boolean } = {}) {
  const [app, piano] = LoopbackLink.pair();
  const s = { q: 0, polls: 0, sw: 3, starts: 0, chunks: 0 };
  const send = (op: number, d: number[]) => piano.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...d.map((b) => b & 0x7f), 0xf7]));
  piano.onMessage((m) => {
    if (m[1] !== 0x7d) return;
    const op = m[6]!;
    const a = Array.from(m.slice(7, m.length - 1));
    if (op === 0x01) send(0x01, [1, 2, 19, 0x3b, 0x57, ...(opts.feat2 === false ? [] : [0x01])]);
    if (op !== 0x1d) return;
    const sub = a[0] ?? 0;
    if (sub === 0) {
      if (s.q === 2 && ++s.polls >= 3) s.q = 3;
      const raw = new Array(20).fill(0);
      raw.splice(0, 8, 1, s.sw, s.q, 0x10, MODULE_TEXT.length & 0xff, MODULE_TEXT.length >> 8, 3, 0);
      raw[8] = 0; raw[9] = 0xff; raw[12] = 2; raw[14] = 7; raw[18] = opts.moduleOff ? 0 : 1;
      send(0x1d, [0, 0, ...ng.pack7(raw)]);
    } else if (sub === 1) {
      const rc = opts.moduleOff ? 4 : s.q === 2 ? 3 : 0;
      if (rc === 0) { s.q = 2; s.polls = 0; s.starts++; }
      send(0x1d, [1, rc, a[1] ?? 0]);
    } else if (sub === 2) {
      const off = ((a[1] ?? 0) << 7) | (a[2] ?? 0);
      const chunk = [...MODULE_TEXT.slice(off, off + 48)].map((c) => c.charCodeAt(0));
      s.chunks++;
      send(0x1d, [2, 0, s.q, MODULE_TEXT.length >> 7, MODULE_TEXT.length & 0x7f, off >> 7, off & 0x7f, ...chunk]);
    } else if (sub === 3) {
      const rc = opts.bound === false ? 2 : 0;
      if (rc === 0) s.sw = a[1]! & 3;
      send(0x1d, [3, rc, s.sw]);
    }
  });
  return { app, piano, s };
}

test("identify: feat2 is the byte after feat1, absent = 0 (older payloads, the simulator)", () => {
  const id = ng.decodeIdentity(Uint8Array.from([1, 2, 19, 0x3b, 0x57, 0x01]))!;
  assert.equal(id.features2, 1);
  assert.ok(id.featureNames.includes("bt") && id.featureNames.includes("tvwake"));
  const old = ng.decodeIdentity(Uint8Array.from([1, 2, 19, 0x3b, 0x57]))!;
  assert.equal(old.features2, 0);
  assert.ok(!old.featureNames.includes("bt"));
  assert.equal(old.features3, 0);
});

test("identify: feat3 is the byte after feat2, absent = 0 (ota_main feat2 0x7F sends six bytes)", () => {
  const main = ng.decodeIdentity(Uint8Array.from([1, 2, 19, 0x3b, 0x77, 0x7f]))!;
  assert.equal(main.features3, 0);
  assert.ok(main.featureNames.includes("readowner") && !main.featureNames.includes("recordctl"));
  const r2 = ng.decodeIdentity(Uint8Array.from([1, 2, 19, 0x3b, 0x77, 0x7f, 0x01]))!;
  assert.equal(r2.features3, ng.NG_FEAT3_REC2);
  assert.equal(r2.features2, 0x7f);
  assert.ok(r2.featureNames.includes("recordctl"));
  const s4 = ng.decodeIdentity(Uint8Array.from([1, 2, 19, 0x3b, 0x77, 0x7f, 0x28]))!;   // ota_main: executor gate 0x08 + S4 0x20
  assert.equal((s4.features3 ?? 0) & ng.NG_FEAT3_WRITE_OWNER, ng.NG_FEAT3_WRITE_OWNER);
  assert.ok(s4.featureNames.includes("writeowner") && !s4.featureNames.includes("recordctl"));
});

test("BtPort: status decode, query -> poll until done -> text in 48-byte chunks", async () => {
  const p = btPiano();
  const port = new bt.BtPort(new ng.NgClient(new PianoSession(p.app)));
  assert.equal(await port.feat2(), 1);
  const st = await port.status();
  assert.equal(st?.switches, 3);
  assert.equal(st?.p2Unlatches, 2);
  assert.equal(st?.asDisarms, 7);
  assert.equal(st?.moduleOn, true);
  const r = await port.query("all", { pollMs: 1, sleep: async () => { await tick(1); } });
  assert.equal(r?.rc, 0);
  assert.equal(r?.text, MODULE_TEXT);
  assert.equal(r?.status?.query, 3);
  assert.equal(p.s.starts, 1);
  assert.equal(p.s.chunks, Math.ceil(MODULE_TEXT.length / 48));
});

test("BtPort: refusals and switches", async () => {
  const off = btPiano({ moduleOff: true });
  const r = await new bt.BtPort(new ng.NgClient(new PianoSession(off.app))).query("set", { pollMs: 1, sleep: async () => { await tick(1); } });
  assert.equal(r?.rc, 4);
  assert.equal(bt.BT_RC[4], "BT audio module off");
  const p = btPiano();
  const port = new bt.BtPort(new ng.NgClient(new PianoSession(p.app)));
  assert.deepEqual(await port.setSwitches(bt.BT_SW_UNLATCH), { rc: 0, switches: 1 });
  const nb = btPiano({ bound: false });
  assert.deepEqual(await new bt.BtPort(new ng.NgClient(new PianoSession(nb.app))).setSwitches(0), { rc: 2, switches: 3 });
});

test("Information: the BT module diagnostics button shows only with feat2 'bt' and prints the module text", async () => {
  for (const feat2 of [false, true]) {
    const p = btPiano({ feat2 });
    const session = new PianoSession(p.app);
    const el = infoScreen(createContext(new ParamStore(), { session: () => session }));
    document.body.replaceChildren(el);
    (el.querySelector("#ng-check") as HTMLButtonElement).click();
    for (let i = 0; i < 100 && !(el.querySelector("#ng-out, output")?.textContent ?? "").includes("OpenGrand"); i++) await tick(10);
    await tick(50);
    const box = el.querySelector("#bt-diag") as HTMLElement;
    assert.equal(box.hidden, !feat2, `feat2 ${feat2}`);
    if (!feat2) continue;
    (el.querySelector("#bt-diag-run") as HTMLButtonElement).click();
    for (let i = 0; i < 400 && !(el.querySelector("#bt-diag-out")?.textContent ?? "").includes("WRAP THOR"); i++) await tick(20);
    assert.ok((el.querySelector("#bt-diag-out")?.textContent ?? "").includes("SET BT PAGEMODE 3 2000 1"));
    assert.match(el.querySelector("#bt-diag-status")?.textContent ?? "", /2/);
  }
});

test("1D 00 status: 20-byte layout has no ring fields, 28-byte layout carries the port 2 ring repairs (12_main_m4_hang)", () => {
  const raw = new Array(28).fill(0);
  raw[0] = 1;
  raw[20] = 0x10; raw[22] = 0x11; raw[24] = 3; raw[26] = 0xff; raw[27] = 0xff;
  assert.equal(bt.decodeBtStatus(Uint8Array.from(raw.slice(0, 20)))!.p2Ring, undefined);
  assert.deepEqual(bt.decodeBtStatus(Uint8Array.from(raw))!.p2Ring, { rd: 0x10, wr: 0x11, repairs: 3, lastDiff: -1 });
});
