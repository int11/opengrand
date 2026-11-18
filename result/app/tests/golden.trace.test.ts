// Golden-trace conformance: replay the piano side of a real capture (original
// PianoRemote 1.1.5 phone app <-> stock CA79 V2.19 over BLE-MIDI, optional input
// target/captures/phone_pianoremote_1.1.5.txt) against our
// PianoSession and require our outgoing bytes to equal the original app's.
//
// The piano only "answers" once our app has sent what the original app sent
// before that answer, and in the captured order. Every place where we differ
// on purpose — or where the capture itself is damaged — is listed in ALLOW
// below with its reason; anything else is a failure. This guards the connect +
// AllParameter sync against regressions such as a missing ACK (stock resends
// and unbinds the controller after 16 tries).
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";

import { PianoSession } from "../src/protocol/session.ts";
import { fromHex, toHex } from "../src/protocol/sysex.ts";
import { ParamStore } from "../src/state/store.ts";
import { LoopbackLink } from "../src/transport/link.ts";

const TRACE = new URL("../../../target/captures/phone_pianoremote_1.1.5.txt", import.meta.url);
// The capture is private (not in the public repository): without it both tests skip.
const noTrace = !existsSync(TRACE) && "capture target/captures/phone_pianoremote_1.1.5.txt absent (not in the public repository)";

interface Line {
  no: number; // 1-based line number in the trace file
  t: number;
  dir: "PI" | "PH"; // PI = piano -> phone, PH = phone (app) -> piano
  hex: string; // upper-case, space separated, as toHex()
}

function load(): Line[] {
  return readFileSync(TRACE, "utf8")
    .split("\n")
    .map((s, i) => ({ s: s.trim(), no: i + 1 }))
    .filter((x) => x.s)
    .map(({ s, no }) => {
      const [t, dir, ...bytes] = s.split(/\s+/);
      return { no, t: Number(t), dir: dir === "PI->PH" ? "PI" : "PH", hex: bytes.join(" ").toUpperCase() } as Line;
    });
}

const CONNECT = "F0 40 7F 7E 08 02 01 01 F7";
const END_ACK = "F0 40 7F 07 08 02 5F 7F 7F F7";

/** Intentional differences / capture defects. Each entry says what and why. */
const ALLOW = {
  /** PH lines the original app sent that ours does not send during connect. */
  notSent: [
    {
      hex: "F0 00 02 08 10 55 10 04 00 F7",
      reason:
        "BLE-MIDI module firmware-version query (answered by the BLE module, never reaches the main board). " +
        "The original app sends it when the module announces itself; ours sends it only from the update screen (features/dfu.ts).",
    },
  ],
  /** PI lines the capture lost ("F0 F7": the btsnoop SysEx reassembly dropped the body). */
  reconstructed: {
    reason:
      "capture defect: the piano message body was lost in btsnoop reassembly. Line 5 (reply to CONNECT) is the " +
      "stock Mode reply 7E 02 00 40 (stock 0x4014A7B0, firmware kproto connect_port); the others are fn 0x10 " +
      "parameters whose address is taken from the original app's ACK right after it (value byte unknown, set to 00 — the ACK does not depend on it).",
  },
};

/** Rebuild a lost piano line from its context. */
function reconstruct(lines: Line[], i: number): string {
  const next = lines[i + 1];
  if (lines[i - 1]?.hex === CONNECT) return "F0 40 7F 7E 04 1E 02 00 40 F7";
  const m = /^F0 40 7F 07 08 02 (\w\w \w\w \w\w) F7$/.exec(next?.hex ?? "");
  assert.ok(next?.dir === "PH" && m, `trace line ${lines[i].no}: cannot reconstruct lost piano message`);
  return `F0 40 7F 10 04 1E ${m![1]} 00 F7`;
}

const settle = async () => {
  for (let k = 0; k < 4; k++) await new Promise((r) => setImmediate(r));
};

test("golden trace: connect + AllParameter sync matches the original app byte for byte", { skip: noTrace }, async () => {
  const all = load();
  const endAck = all.findIndex((l) => l.dir === "PH" && l.hex === END_ACK);
  assert.ok(endAck > 0, "trace has the EndOfData ACK");
  const lines = all.slice(0, endAck + 1);

  const [app, piano] = LoopbackLink.pair();
  const out: string[] = [];
  piano.onMessage((m) => out.push(toHex(m)));
  const session = new PianoSession(app);
  const store = new ParamStore();
  store.attach(session);

  const mismatches: string[] = [];
  const skipped: string[] = [];
  const rebuilt: number[] = [];
  let connected: Promise<unknown> | null = null;
  let compared = 0;

  for (let i = 0; i < lines.length; i++) {
    const l = lines[i];
    await settle();
    if (l.dir === "PH") {
      const allow = ALLOW.notSent.find((a) => a.hex === l.hex);
      if (allow) {
        skipped.push(`line ${l.no}: ${l.hex}`);
        continue;
      }
      // the user presses Connect where the original app sent CONNECT
      if (!connected && l.hex === CONNECT) {
        connected = session.connect();
        await settle();
      }
      const got = out.shift();
      compared++;
      if (got !== l.hex) mismatches.push(`line ${l.no} (${l.t.toFixed(3)} s): want ${l.hex}, got ${got ?? "(nothing)"}`);
      // no reply is replayed for a request our app did not make
      if (got === undefined) break;
    } else {
      if (out.length) mismatches.push(`before line ${l.no}: extra outgoing ${out.splice(0).join(" | ")}`);
      let hex = l.hex;
      if (hex === "F0 F7") {
        hex = reconstruct(lines, i);
        rebuilt.push(l.no);
      }
      piano.send(fromHex(hex));
    }
  }
  assert.deepEqual(mismatches, [], `outgoing bytes differ from the original app:\n${mismatches.slice(0, 20).join("\n")}`);

  await connected;
  assert.equal(session.state, "ready");
  assert.equal(session.identity?.model, "CA79");
  assert.equal(session.identity?.version, "V2.19");
  assert.equal(session.deviceId, 0x7f, "stock answers with dev 7F, so ours matches the original's 7F");

  // after the EndOfData ACK the original app was silent until the user acted:
  // no keepalive, no extra requests from us either
  await new Promise((r) => setTimeout(r, 50));
  await settle();
  assert.deepEqual(out, [], "no extra outgoing messages after the sync");

  // what was allow-listed is exactly what we expect — no silent growth
  assert.deepEqual(skipped, ["line 2: F0 00 02 08 10 55 10 04 00 F7", "line 3: F0 00 02 08 10 55 10 04 00 F7"]);
  assert.deepEqual(rebuilt, [5, 149, 165, 281, 383], ALLOW.reconstructed.reason);
  // every parameter ACKed: 235 captured + 4 reconstructed + EndOfData = 240 ACKs
  assert.equal(lines.filter((l) => l.dir === "PH" && / 07 08 02 /.test(l.hex)).length, 240);
  assert.equal(compared, lines.filter((l) => l.dir === "PH").length - 2);
  assert.ok(store.size() > 200, `store mirrored ${store.size()} values`);
});

test("golden trace: user edits after sync wait for the piano's ACK like the original", { skip: noTrace }, async () => {
  const all = load();
  const start = all.findIndex((l) => l.dir === "PH" && l.hex === END_ACK) + 1;
  // lines 495..527: metronome volume / sound edits (fn 0x10, ACKed) and a fn 0x31 keyboard-mode query (no ACK)
  const lines = all.slice(start).filter((l) => l.no <= 527);
  assert.ok(lines.length > 20);

  const [app, piano] = LoopbackLink.pair();
  const out: string[] = [];
  piano.onMessage((m) => out.push(toHex(m)));
  // a session that is already synced with the stock piano (dev 7F)
  const session = new PianoSession(app);
  session.deviceId = 0x7f;

  const mismatches: string[] = [];
  for (const l of lines) {
    await settle();
    if (l.dir === "PH") {
      const b = [...fromHex(l.hex)];
      // the user changes a value: our app produces the frame from (fn, v1, v2, part, data)
      void session.sendParam({ fn: b[3], v1: b[6], v2: b[7], part: b[8], data: b.slice(9, -1) });
      await settle();
      const got = out.shift();
      if (got !== l.hex) mismatches.push(`line ${l.no}: want ${l.hex}, got ${got ?? "(nothing)"}`);
    } else {
      if (out.length) mismatches.push(`before line ${l.no}: extra ${out.splice(0).join(" | ")}`);
      piano.send(fromHex(l.hex));
    }
  }
  await settle();
  assert.deepEqual(mismatches, []);
  assert.deepEqual(out, []);
  assert.equal(session.stats.retried, 0);
});
