// OpenGrand extension SysEx framing: manufacturer ID, tag, op codes, request / reply, 7-in-8 packing, CRC-32
// (result/protocol/REMOTE_PROTOCOL.md §10; protocol/ngext.ts re-exports all of it).
//
//   request  F0 7D 43 41 37 39 <op> [args] F7
//   reply    F0 7D 43 41 37 39 <op|0x40> [data] F7

export const NG_ID = 0x7d;
export const NG_TAG = [0x43, 0x41, 0x37, 0x39] as const; // "CA79"

export const NgOp = {
  Identify: 0x01,
  DumpStart: 0x02,
  Status: 0x03,
  Cancel: 0x04,
  // phase2c probe (read-only diagnostics, REMOTE_PROTOCOL.md section 10.1)
  Port: 0x05,
  Echo: 0x06,
  Apo: 0x07,
  BtLog: 0x08,
  Canary: 0x09,
  // core_probe: the OpenGrand core linked in, dormant (REMOTE_PROTOCOL.md section 10.2)
  CoreTest: 0x0a,
  // ota_loader: remote cave update (REMOTE_PROTOCOL.md section 10.3, src/protocol/ota.ts)
  OtaStatus: 0x10,
  // power-family slot payloads (ota_main from rebuild 11): read-only GPIO diagnostic (REMOTE_PROTOCOL.md section 10.11)
  Gpio: 0x17,
  // OpenGrand core: power policy and partial standby (REMOTE_PROTOCOL.md section 10.4)
  PowerPolicy: 0x18,
  Standby: 0x19,
  // slot payload ota_power: power port diagnostics (read only, REMOTE_PROTOCOL.md section 10.4)
  PowerDiag: 0x1a,
  // slot payload ota_sysex+: SysEx front / session core records (REMOTE_PROTOCOL.md section 10.5)
  Front: 0x1b,
  // slot payload ota_main (feat2 0x08): startup settings owned by the piano (REMOTE_PROTOCOL.md section 10.8)
  Startup: 0x1c,
  // slot payload ota_main (feat2 0x10): playback of the internal songs by the core (REMOTE_PROTOCOL.md section 10.9)
  Recorder: 0x1e,
  // ota_main ngver 4: RAM canary over the unreferenced SDRAM blocks (REMOTE_PROTOCOL.md section 10.13)
  RamCanary: 0x20,
} as const;
export type NgOpCode = (typeof NgOp)[keyof typeof NgOp];

export function ngRequest(op: NgOpCode, args: number[] = []): Uint8Array {
  return Uint8Array.from([0xf0, NG_ID, ...NG_TAG, op, ...args.map((b) => b & 0x7f), 0xf7]);
}

/** Decode a OpenGrand reply; returns null for anything else (including our own echo). */
export function parseNgReply(msg: Uint8Array): { op: number; data: Uint8Array } | null {
  if (msg.length < 8 || msg[0] !== 0xf0 || msg[1] !== NG_ID || msg[msg.length - 1] !== 0xf7) return null;
  for (let i = 0; i < NG_TAG.length; i++) if (msg[2 + i] !== NG_TAG[i]) return null;
  const op = msg[6]!;
  if ((op & 0x40) === 0) return null; // a request echoed back, not a reply
  return { op: op & 0x3f, data: msg.slice(7, msg.length - 1) };
}

/**
 * Sub-ops answered only on a bound controller port (REMOTE_PROTOCOL.md section 10.4-10.9), with the rc that means
 * "this port is not bound": `<op|0x40> <sub> <rc> ...` for all of them. Reads (any port) never answer it, so a
 * reply alone tells: `1D 06` / `1D 09` / `1D 0A` read with rc 0, write with rc 2 when unbound.
 */
const NOT_BOUND: Record<number, { subs: readonly number[]; rc: number }> = {
  0x17: { subs: [0x02], rc: 2 }, // GPIO change notices on
  0x18: { subs: [0x01, 0x02], rc: 2 }, // power policy set / defaults
  0x19: { subs: [0x01, 0x03, 0x04], rc: 2 }, // standby enter / remote off / APO test
  0x1b: { subs: [0x01], rc: 2 }, // SysEx front off / on
  0x1c: { subs: [0x01, 0x02, 0x03], rc: 2 }, // startup settings
  0x1d: { subs: [0x03, 0x05, 0x06, 0x07, 0x09, 0x0a], rc: 2 }, // BT port switches / source act / policy write / restart / AUTOCALL / class
  0x1e: { subs: [0x02], rc: 9 }, // recorder command (recctl RC_*, 9 = not bound)
  0x20: { subs: [0x01], rc: 2 }, // RAM canary arm
};

/**
 * A OpenGrand reply saying the port that asked is not bound (field 2026-11-09: after apply now the new payload's
 * session core starts with an empty binding table, `1D 0A 02` -> `5D 0A 02 ...`). Replies go only to the port that
 * asked, so every such reply this app sees is about its own port. `{ op, sub }` or null.
 */
export function ngNotBound(msg: Uint8Array): { op: number; sub: number } | null {
  const r = parseNgReply(msg);
  const rule = r && NOT_BOUND[r.op];
  if (!r || !rule || r.data.length < 2) return null;
  const sub = r.data[0]!;
  return rule.subs.includes(sub) && r.data[1] === rule.rc ? { op: r.op, sub } : null;
}

// 8-bit fields travel 7-in-8 packed (REMOTE_PROTOCOL.md section 10.1).

/** Inverse of the cave's pack7(): each group is a prefix byte holding bit 7 of the up to
 *  seven bytes that follow it. */
export function unpack7(data: Uint8Array): Uint8Array {
  const out: number[] = [];
  for (let i = 0; i < data.length; ) {
    const hi = data[i]!;
    const n = Math.min(7, data.length - i - 1);
    for (let k = 0; k < n; k++) out.push(data[i + 1 + k]! | ((hi >> k) & 1 ? 0x80 : 0));
    i += 1 + n;
  }
  return Uint8Array.from(out);
}

/** The cave's packing, for tests and simulators. */
export function pack7(raw: Uint8Array | number[]): number[] {
  const out: number[] = [];
  for (let i = 0; i < raw.length; i += 7) {
    const g = Array.from(raw.slice(i, i + 7) as ArrayLike<number>);
    out.push(g.reduce((hi, b, k) => hi | ((b & 0x80 ? 1 : 0) << k), 0), ...g.map((b) => b & 0x7f));
  }
  return out;
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

/** CRC-32 (zlib / IEEE 802.3), the one the cave reports. */
export function crc32(data: Uint8Array | number[]): number {
  let c = 0xffffffff;
  for (const b of data) c = CRC_TABLE[(c ^ b) & 0xff]! ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
