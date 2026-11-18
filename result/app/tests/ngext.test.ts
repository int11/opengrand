import { test } from "node:test";
import assert from "node:assert/strict";

import {
  NgClient,
  NgOp,
  decodeIdentity,
  decodeStatus,
  ngNotBound,
  ngRequest,
  parseNgReply,
} from "../src/protocol/ngext.ts";
import { PianoSession } from "../src/protocol/session.ts";
import { LoopbackLink } from "../src/transport/link.ts";

/** A piano running the Phase 2b cave: answers the 0x7D family, runs a fake dump. */
function ngPiano(link: LoopbackLink, opts: { features?: number; dumpBusy?: boolean } = {}) {
  const features = opts.features ?? 0x07;
  let state = 0;
  let chunk = 0;
  const reply = (op: number, data: number[]) =>
    link.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...data, 0xf7]));
  link.onMessage((m) => {
    if (m[1] !== 0x7d) return;
    switch (m[6]) {
      case NgOp.Identify:
        reply(NgOp.Identify, [1, 2, 19, features, 0]);
        break;
      case NgOp.DumpStart:
        if (opts.dumpBusy) return reply(NgOp.DumpStart, [12]);
        state = 2;
        chunk = 7;
        return reply(NgOp.DumpStart, [0]);
      case NgOp.Status:
        reply(NgOp.Status, [state, (chunk >> 7) & 0x7f, chunk & 0x7f, 0]);
        break;
      case NgOp.Cancel:
        state = 5;
        reply(NgOp.Cancel, [state]);
        break;
      default:
        break; // unknown op: silence, like the cave
    }
  });
}

test("ngRequest builds the documented frame", () => {
  assert.deepEqual([...ngRequest(NgOp.Identify)], [0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, 0x01, 0xf7]);
  // arguments are masked to 7 bits
  assert.deepEqual([...ngRequest(NgOp.Status, [0xff])].slice(6), [0x03, 0x7f, 0xf7]);
});

test("parseNgReply accepts replies and rejects everything else", () => {
  const ok = Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, 0x41, 1, 2, 19, 7, 0, 0xf7]);
  const r = parseNgReply(ok);
  assert.equal(r?.op, NgOp.Identify);
  assert.deepEqual([...r!.data], [1, 2, 19, 7, 0]);
  // a request (no 0x40 bit) is not a reply
  assert.equal(parseNgReply(ngRequest(NgOp.Identify)), null);
  // another manufacturer, a Kawai frame, a short frame
  assert.equal(parseNgReply(Uint8Array.from([0xf0, 0x7c, 0x43, 0x41, 0x37, 0x39, 0x41, 0xf7])), null);
  assert.equal(parseNgReply(Uint8Array.from([0xf0, 0x40, 0x7f, 0x7e, 0x04, 0x1e, 0x02, 0xf7])), null);
  assert.equal(parseNgReply(Uint8Array.from([0xf0, 0x7d, 0x43, 0xf7])), null);
  // wrong tag
  assert.equal(parseNgReply(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x38, 0x41, 1, 0xf7])), null);
});

test("decodeIdentity and decodeStatus follow REMOTE_PROTOCOL.md section 10", () => {
  const id = decodeIdentity(Uint8Array.from([1, 2, 19, 0x07, 0]))!;
  assert.equal(id.protocol, 1);
  assert.equal(id.baseVersion, "V2.19");
  assert.deepEqual(id.featureNames, ["identify", "status", "nordump"]);
  // the identify-only build reports no dump support
  assert.deepEqual(decodeIdentity(Uint8Array.from([1, 2, 19, 0x03, 0]))!.featureNames, ["identify", "status"]);
  // feat2 is optional: absent = 0 (every image before ota_bt), ota_session = 3B / 77 / 02, ota_main = 3B / 77 / 03
  assert.equal(id.features2, 0);
  const ss = decodeIdentity(Uint8Array.from([1, 2, 19, 0x3b, 0x77, 0x02]))!;
  assert.equal(ss.features2, 0x02);
  assert.ok(ss.featureNames.includes("session") && ss.featureNames.includes("tvwake") && ss.featureNames.includes("sysex"));
  assert.ok(!ss.featureNames.includes("bt"));
  const mn = decodeIdentity(Uint8Array.from([1, 2, 19, 0x3b, 0x77, 0x03]))!;
  assert.equal(mn.features2, 0x03);
  assert.ok(mn.featureNames.includes("bt") && mn.featureNames.includes("session"));
  assert.ok(!mn.featureNames.includes("panelui"));
  // ota_main with the panel-UI relay (04_panel_ui_remote §4) = 3B / 77 / 07
  const rl = decodeIdentity(Uint8Array.from([1, 2, 19, 0x3b, 0x77, 0x07]))!;
  assert.ok(rl.featureNames.includes("panelui") && rl.featureNames.includes("bt") && rl.featureNames.includes("session"));

  const st = decodeStatus(Uint8Array.from([2, 0x01, 0x05, 0]))!;
  assert.equal(st.state, "running");
  assert.equal(st.chunk, 133); // (1 << 7) | 5
  assert.equal(st.errorText, "none");
  assert.equal(decodeStatus(Uint8Array.from([5, 0, 0, 1]))!.errorText, "no USB memory");
  assert.equal(decodeStatus(Uint8Array.from([0, 0, 0]))!, null);
});

test("NgClient talks to a cave-equipped piano", async () => {
  const [app, piano] = LoopbackLink.pair();
  ngPiano(piano);
  const s = new PianoSession(app, { handshake: false });
  const ng = new NgClient(s);

  const id = await ng.identify(200);
  assert.equal(id?.baseVersion, "V2.19");
  assert.ok(id!.featureNames.includes("nordump"));

  assert.equal(await ng.startDump(200), 0);
  const st = await ng.status(200);
  assert.equal(st?.state, "running");
  assert.equal(st?.chunk, 7);
  assert.equal(await ng.cancel(200), "error");
});

test("identify against stock firmware times out instead of hanging", async () => {
  const [app, piano] = LoopbackLink.pair();
  piano.onMessage(() => {
    /* stock: 0x7D is dropped without any reply */
  });
  const ng = new NgClient(new PianoSession(app, { handshake: false }));
  assert.equal(await ng.identify(60), null);
  assert.equal(await ng.status(60), null);
});

test("a firmware without dump support reports it", async () => {
  const [app, piano] = LoopbackLink.pair();
  ngPiano(piano, { features: 0x03, dumpBusy: true });
  const ng = new NgClient(new PianoSession(app, { handshake: false }));
  assert.deepEqual((await ng.identify(200))!.featureNames, ["identify", "status"]);
  assert.equal(await ng.startDump(200), 12);
});

// ---- phase2c probe ops (REMOTE_PROTOCOL.md section 10.1) ----
import {
  EchoMode,
  NG_ECHO_MAX,
  NG_PAYLOAD_MAX,
  btLogLines,
  crc32,
  decodeApo,
  decodeCanary,
  decodeEcho,
  decodePort,
  pack7,
  unpack7,
} from "../src/protocol/ngext.ts";

/** A piano running the phase2c probe cave, mirroring result/stockfw/cave/src/ca79ng.c. */
function probePiano(link: LoopbackLink, port = 2) {
  const apo = Uint8Array.from({ length: 32 }, (_, i) => (0x80 + 3 * i) & 0xff);
  apo.set([0x10, 0x27, 0, 0], 0); // counter 10000
  apo[12] = 1; // BT power
  apo[21] = 1; // APO 15 min
  apo[24] = 0;
  apo[28] = 1; // A2DP
  apo[29] = 1;
  apo[30] = 0; // iWRAP
  apo[31] = 0;
  const ring = new Uint8Array(0x800);
  const text = new TextEncoder().encode("\r\nREADY.\r\nRING 0 00:11:22:33:44:55 19 A2DP\r\n");
  ring.set(text, 0x800 - text.length);
  let canState = 0;
  const reply = (op: number, data: number[]) =>
    link.send(Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, op | 0x40, ...data, 0xf7]));
  const le = (v: number, n: number) => Array.from({ length: n }, (_, i) => (v >>> (8 * i)) & 0xff);
  link.onMessage((m) => {
    if (m[1] !== 0x7d) return;
    const len = m.length;
    switch (m[6]) {
      case NgOp.Identify:
        return reply(NgOp.Identify, [1, 2, 19, 0x7b, 0x01]);
      case NgOp.Port:
        return reply(NgOp.Port, pack7([port, 3, ...le(1, 4), ...le(len, 2)]));
      case NgOp.Echo: {
        const mode = m[7]!;
        if (mode === 0 || mode === 1) {
          const p = m.slice(8, len - 1);
          return reply(NgOp.Echo, [mode, ...pack7([...le(p.length, 2), ...le(crc32(p), 4)]), ...(mode === 1 ? p.slice(0, NG_ECHO_MAX) : [])]);
        }
        if (mode === 2) {
          const n = Math.min(((m[8]! & 0x7f) << 7) | m[9]!, NG_ECHO_MAX);
          const g = Array.from({ length: n }, (_, i) => (m[10]! + i) & 0x7f);
          return reply(NgOp.Echo, [2, ...pack7([...le(n, 2), ...le(crc32(g), 4)]), ...g]);
        }
        return;
      }
      case NgOp.Apo:
        return reply(NgOp.Apo, pack7(apo));
      case NgOp.BtLog:
        if (m[7] === 0) return reply(NgOp.BtLog, [0, ...pack7([0, 0, 0xfd, 0x07, 0, 1])]);
        return reply(NgOp.BtLog, [1, m[8]!, ...pack7([1, ...ring.slice(m[8]! * 128, m[8]! * 128 + 128)])]);
      case NgOp.Canary:
        if (m[7] === 1) {
          canState = 2;
          return reply(NgOp.Canary, [1, 1]);
        }
        return reply(NgOp.Canary, [0, ...pack7([canState, ...le(3, 4), ...le(0x40175340, 4), ...le(0x401ffffc, 4), ...le(0x40175300, 4), ...le(0x40200000, 4), ...le(125, 4)])]);
      default:
        return;
    }
  });
}

test("pack7/unpack7 match the cave's 7-in-8 packing", () => {
  // bit k of the prefix byte = bit 7 of byte k of the group
  assert.deepEqual(pack7([0x80, 0x01, 0xff]), [0x05, 0x00, 0x01, 0x7f]);
  const raw = Uint8Array.from({ length: 30 }, (_, i) => (i * 37) & 0xff);
  const packed = pack7(raw);
  assert.equal(packed.length, 30 + 5);
  assert.ok(packed.every((b) => b < 0x80));
  assert.deepEqual([...unpack7(Uint8Array.from(packed))], [...raw]);
});

test("crc32 is the zlib CRC-32", () => {
  assert.equal(crc32(new TextEncoder().encode("123456789")), 0xcbf43926);
  assert.equal(crc32([]), 0);
});

test("probe decoders follow REMOTE_PROTOCOL.md section 10.1", () => {
  assert.deepEqual(decodePort(Uint8Array.from(pack7([3, 4, 1, 0, 0, 0, 0x00, 0x08]))), { port: 3, binding: 4, forwarding: 1, length: 0x800 });
  const e = decodeEcho(Uint8Array.from([1, ...pack7([5, 0, 0x26, 0x39, 0xf4, 0xcb]), 1, 2, 3, 4, 5]))!;
  assert.equal(e.count, 5);
  assert.equal(e.crc, 0xcbf43926);
  assert.deepEqual([...e.data], [1, 2, 3, 4, 5]);
  const raw = new Array(32).fill(0);
  raw[0] = 0x10; raw[1] = 0x27; raw[12] = 1; raw[21] = 2; raw[24] = 2; raw[28] = 1; raw[30] = 1;
  const a = decodeApo(Uint8Array.from(pack7(raw)))!;
  assert.equal(a.counter, 10000);
  assert.equal(a.btPower, 1);
  assert.equal(a.apoSetting, 2);
  assert.equal(a.state, 2);
  assert.equal(a.a2dp, 1);
  assert.equal(a.dialect, "AT");
  assert.equal(decodeApo(Uint8Array.from(pack7(raw.slice(0, 20)))), null);
  const c = decodeCanary(Uint8Array.from([0, ...pack7([2, 3, 0, 0, 0, 0x40, 0x53, 0x17, 0x40, ...new Array(16).fill(0)])]))!;
  assert.equal(c.state, 2);
  assert.equal(c.changed, 3);
  assert.equal(c.first, 0x40175340);
  assert.deepEqual(btLogLines(Uint8Array.from([0, 0, ...new TextEncoder().encode("\r\nREADY.\r\nRING 1"), 0x80, 0x0d, 0x0a])), ["READY.", "RING 1."]);
});

test("NgClient runs every probe op against a probe piano", async () => {
  const [app, piano] = LoopbackLink.pair();
  probePiano(piano, 2);
  const ng = new NgClient(new PianoSession(app, { handshake: false }));

  const id = await ng.identify(200);
  assert.deepEqual(id!.featureNames, ["identify", "status", "port", "echo", "apo", "canary", "btlog"]);

  const p = await ng.port(200);
  assert.equal(p?.port, 2);
  assert.equal(p?.binding, 3);
  assert.equal(p?.length, 8);

  const pay = Uint8Array.from({ length: 100 }, (_, i) => i);
  const sink = await ng.echo(EchoMode.Sink, pay, 200);
  assert.equal(sink?.count, 100);
  assert.equal(sink?.crc, crc32(pay));
  assert.equal(sink?.data.length, 0);
  const echo = await ng.echo(EchoMode.Echo, pay, 200);
  assert.deepEqual([...echo!.data], [...pay]);
  const gen = await ng.generate(5000, 3, 200);
  assert.equal(gen?.count, NG_ECHO_MAX);
  assert.equal(gen?.crc, crc32(gen!.data));

  let clock = 0;
  const samples = await ng.linkTest([64, NG_PAYLOAD_MAX], () => (clock += 10));
  assert.equal(samples.length, 4);
  assert.ok(samples.every((s) => s.ok), JSON.stringify(samples));
  assert.deepEqual(samples.map((s) => s.bytes), [64, 64, NG_PAYLOAD_MAX, NG_ECHO_MAX]);

  const apo = await ng.apo(200);
  assert.equal(apo?.counter, 10000);
  assert.equal(apo?.a2dp, 1);
  assert.equal(apo?.dialect, "iWRAP");

  const log = await ng.btLog(200);
  assert.equal(log?.wr, 0);
  assert.equal(log?.rd, 0x7fd);
  assert.deepEqual(log?.lines, ["READY.", "RING 0 00:11:22:33:44:55 19 A2DP"]);

  assert.equal((await ng.canary(200))?.state, 0);
  assert.equal(await ng.canaryArm(200), 1);
  const c = await ng.canary(200);
  assert.equal(c?.state, 2);
  assert.equal(c?.changed, 3);
  assert.equal(c?.ticks, 125);
});

test("probe ops time out on stock firmware or a cave without them", async () => {
  const [app, piano] = LoopbackLink.pair();
  ngPiano(piano, { features: 0x07 }); // the Phase 2b cave: silent on 0x05..0x09
  const ng = new NgClient(new PianoSession(app, { handshake: false }));
  assert.equal(await ng.port(60), null);
  assert.equal(await ng.apo(60), null);
  assert.equal(await ng.canary(60), null);
  assert.equal(await ng.btLog(60), null);
});

test("decodeCoreTest names the failed checks", async () => {
  const { decodeCoreTest, pack7, CORE_TEST_CHECKS } = await import("../src/protocol/ngext.ts");
  const raw = new Uint8Array(32);
  raw[0] = 1;
  raw[1] = 9;
  raw[2] = 0xff & ~(1 << 3); // "store" failed
  raw[3] = 0x01;
  raw[19] = 3;
  raw.set([0x10, 0x27, 0, 0], 20); // 10000 ticks
  const c = decodeCoreTest(Uint8Array.from(pack7(raw)))!;
  assert.equal(c.passed, 8);
  assert.equal(c.passMask & (1 << 3), 0);
  assert.equal(CORE_TEST_CHECKS[3], "store");
  assert.equal(c.ticks, 10000);
  assert.ok(c.initialised && c.guardsOk && !c.apoDue);
  assert.equal(decodeCoreTest(Uint8Array.from([0, 1, 2])), null);
});

test("op 0x1C status record decodes (startup settings, REMOTE_PROTOCOL.md section 10.8)", async () => {
  const { decodeStartupStatus, NG_FEAT2_STARTUP, NgOp } = await import("../src/protocol/ngext.ts");
  const raw = new Uint8Array(36);
  raw[0] = 1; raw[1] = 2; raw[2] = 0x01 | 0x04 | 0x08; raw[3] = 3;
  raw[12] = 5; raw[16] = 226; raw[18] = 13; raw[24] = 2; raw[31] = 0x20;
  const st = decodeStartupStatus(raw)!;
  assert.equal(st.mode, "poweroff");
  assert.deepEqual([st.loaded, st.hasCurrent, st.hasPowerOff, st.ready, st.restoring], [true, false, true, true, false]);
  assert.deepEqual([st.powerOffGen, st.records, st.applied, st.saves, st.restoredAtBoot], [5, 226, 13, 2, true]);
  assert.equal(decodeStartupStatus(new Uint8Array(10)), null);
  assert.equal(st.allSettings, null, "a 36-byte record (firmware before record 14 §7): no NG part fields");
  assert.equal(st.ownStart, null);
  // record 14 §7: bytes 36..39 - the mode covers every setting, C / P carry our own settings, this power-on's start
  const raw2 = new Uint8Array(40);
  raw2.set(raw);
  raw2[36] = 0x01 | 0x04; raw2[37] = 2; raw2[38] = 0x03; raw2[39] = 21;
  const st2 = decodeStartupStatus(raw2)!;
  assert.deepEqual([st2.allSettings, st2.currentHasAll, st2.powerOffHasAll, st2.ownStart], [true, false, true, 2]);
  raw2[37] = 0xff;
  assert.equal(decodeStartupStatus(raw2)!.ownStart, null, "0xFF = not decided yet");
  assert.equal(NG_FEAT2_STARTUP, 0x08);
  assert.equal(NgOp.Startup, 0x1c);
});

test("ngNotBound: bound-only sub-ops with their 'not bound' rc; reads and other rcs are not", () => {
  const r = (...b: number[]) => Uint8Array.from([0xf0, 0x7d, 0x43, 0x41, 0x37, 0x39, ...b, 0xf7]);
  assert.deepEqual(ngNotBound(r(0x5d, 0x0a, 0x02, 0x02, 0x01)), { op: 0x1d, sub: 0x0a }, "field 2026-11-09: 1D 0A 02 -> 5D 0A 02");
  assert.deepEqual(ngNotBound(r(0x58, 0x01, 0x02)), { op: 0x18, sub: 0x01 });
  assert.deepEqual(ngNotBound(r(0x59, 0x03, 0x02, 0x00, 0x00)), { op: 0x19, sub: 0x03 });
  assert.deepEqual(ngNotBound(r(0x5c, 0x02, 0x02)), { op: 0x1c, sub: 0x02 });
  assert.deepEqual(ngNotBound(r(0x5e, 0x02, 0x09, 0x01)), { op: 0x1e, sub: 0x02 }, "recorder: 9 = not bound");
  assert.equal(ngNotBound(r(0x5e, 0x02, 0x02, 0x01)), null, "recorder rc 2 = bad argument");
  assert.equal(ngNotBound(r(0x5d, 0x0a, 0x00, 0x02)), null, "rc 0");
  assert.equal(ngNotBound(r(0x5d, 0x04, 0x02)), null, "1D 04 answers any port: byte 2 is data");
  assert.equal(ngNotBound(r(0x59, 0x02, 0x02, 0x00)), null, "wake answers any port");
  assert.equal(ngNotBound(r(0x5b, 0x03, 0x02)), null, "1B 03 is a read (pack7 data)");
  assert.equal(ngNotBound(r(0x1d, 0x0a, 0x02)), null, "a request echoed back");
});
