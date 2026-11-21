// OpenGrand reply decoders and their record types: identify, dump status, the phase2c probe ops,
// core self-test, session core (REMOTE_PROTOCOL.md §10.1-10.5).
import { u16, u32 } from "./bytes.ts";
import { unpack7 } from "./wire.ts";
import { type NgFeatureName, ngFeature } from "../ngver.gen.ts";

export const NgState = ["idle", "start", "running", "closing", "done", "error"] as const;
export type NgStateName = (typeof NgState)[number];

/** Dump error codes, mirrored from the cave payload (result/stockfw/cave/src/ca79ng.c). */
export const NG_ERRORS: Record<number, string> = {
  0: "none",
  1: "no USB memory",
  2: "file system not ready",
  3: "write handle busy",
  4: "flash busy",
  5: "signature mismatch",
  6: "verify mismatch",
  7: "write failed",
  8: "short write",
  9: "close failed",
  10: "size mismatch",
  11: "cancelled",
  12: "already running",
  13: "not supported by this firmware",
};

export const NG_FEATURES: { bit: number; name: string }[] = [
  { bit: 0x01, name: "identify" },
  { bit: 0x02, name: "status" },
  { bit: 0x04, name: "nordump" },
  { bit: 0x08, name: "port" },
  { bit: 0x10, name: "echo" },
  { bit: 0x20, name: "apo" },
  { bit: 0x40, name: "canary" },
];
/** Feature bits in the second identify byte (feat1). */
export const NG_FEATURES1: { bit: number; name: string }[] = [
  { bit: 0x01, name: "btlog" },
  { bit: 0x02, name: "core" },
  { bit: 0x04, name: "ota" },
  { bit: 0x08, name: "hookdemo" },
  { bit: 0x10, name: "power" },
  { bit: 0x20, name: "sysex" },
  { bit: 0x40, name: "tvwake" },
];
/** feat1 bit: the core answers ops 0x18 (power policy) and 0x19 (standby). */
export const NG_FEAT1_POWER = 0x10;
/** feat1 bit (with POWER): standby keeps Bluetooth audio on and a stream start wakes the piano
 * (policy `btWake`, flag bit 2; slot payload ota_power2). */
export const NG_FEAT1_TVWAKE = 0x40;
/** Feature bits in the third identify byte (feat2, the byte after feat1; feat1 has no free bit).
 * Only builds with a non-zero feat2 send it: absent = 0, so older apps that stop at feat1 are unaffected. */
export const NG_FEATURES2: { bit: number; name: string }[] = [
  { bit: 0x01, name: "bt" },
  { bit: 0x02, name: "session" },
  { bit: 0x04, name: "panelui" },
  { bit: 0x08, name: "startup" },
  { bit: 0x10, name: "recorder" },
  { bit: 0x20, name: "remoteoff" },
  { bit: 0x40, name: "readowner" },
];
/** feat2 bit: BT port (op 0x1D: port 2 latch clear, active-sensing guard, BT module read-only query;
 * slot payload ota_bt). */
export const NG_FEAT2_BT = 0x01;
/** feat2 bit: the Phase 3-2 session core (several controllers stay bound, the core answers requests;
 * op 0x1B 03; slot payload ota_session). ota_main = 0x1F. */
export const NG_FEAT2_SESSION = 0x02;
/** feat2 bit: the panel-UI relay (0x7D ops 0x30-0x3F pass through to the panel and back, REMOTE_PROTOCOL.md
 * section 10.7; op 0x1B 10 its counters section 4). ota_main = 0x1F. */
export const NG_FEAT2_PANELUI = 0x04;
/** feat2 bit: startup settings owned by the piano (op 0x1C: mode stock / current / power-off, snapshots in the
 * piano's NOR, restored at power-on). ota_main = 0x1F. */
export const NG_FEAT2_STARTUP = 0x08;
/** feat2 bit: playback of the internal songs by the core (op 0x1E: status, song list, play / stop / pause / song /
 * part / tempo / seek, notices). ota_main = 0x1F. */
export const NG_FEAT2_RECORDER = 0x10;
/** feat2 bit: remote off - op 19 03 from any controller the piano has bound puts it where the auto power-off goes
 * (standby or off), refused while recording, a file operation or a flash job.
 * ota_main = 0x7F. */
export const NG_FEAT2_REMOTE_OFF = 0x20;
/** feat2 bit: Phase 3-4 S3 - the piano's settings store answers parameter requests (byte-identical to the stock's),
 * on at boot from ota_main 63c98efb (earlier builds start off, op 1B 05 04 <0|1> switches it); from ngver 3 always on,
 * 1B 05 04 <x> answers rc 1. Op 1B 05 05 its record. ota_main = 0x7F. */
export const NG_FEAT2_READ_OWNER = 0x40;
/** Feature bits in the fourth identify byte (feat3, after feat2 - feat2 is full). Sent only when non-zero (and then
 * feat2 is there too): absent = 0, so older apps that stop at feat2 are unaffected. */
export const NG_FEATURES3: { bit: number; name: string }[] = [
  { bit: 0x01, name: "recordctl" },
  { bit: 0x02, name: "intakegate" },
  { bit: 0x04, name: "recsave" },
  { bit: 0x08, name: "execgate" },
  { bit: 0x10, name: "midichan" },
  { bit: 0x20, name: "writeowner" },
];
/** feat3 bit: Phase 3-6 R2 record control by the core (op 0x1E cmd 8-11: arm / count-in / start / finish). In ota_main since the 2026-11-16 fold (0x3F with 0x02 / 0x04). */
export const NG_FEAT3_REC2 = 0x01;
/** feat3 bit: Phase 4 - MIDI channel messages that change settings (Program Change, CC 0x7A / 0x5C) and the delayed
 * parameter apply judged by the core at the ota_loader_v3 sites (on at boot from ota_main 63c98efb when the sites are
 * attached, earlier builds start off and op 1B 07 <0|1> switches it; from ngver 3 no off op - 1B 07 <x> answers rc 1),
 * record op 1B 07
 * ota_main = 0x3F (every feat3 bit; the pre-fold ota_main_15 = 0x38). */
export const NG_FEAT3_MIDICH = 0x10;
/** feat3 bit: Phase 3-4 S4 - the piano's core judges every parameter set first (known key, length, the stock descriptor's
 * masks), lets the stock apply it and reads it back into its settings store; after a set of a key whose apply changes
 * nothing else the store keeps answering requests at once. On at boot from ota_main 63c98efb (earlier builds start off,
 * op 1B 05 06 <0|1> switches it); from ngver 3 always on, 1B 05 06 <x> answers rc 1. Op 1B 05 07 its record
 * In ota_main. */
export const NG_FEAT3_WRITE_OWNER = 0x20;

export interface NgIdentity {
  protocol: number;
  /** Stock firmware this cave was built against, e.g. "V2.19". */
  baseVersion: string;
  features: number;
  features1: number;
  /** Third feature byte (0 when the piano sends only five identify bytes). */
  features2?: number;
  /** Fourth feature byte (0 when the piano sends six identify bytes or fewer). */
  features3?: number;
  featureNames: string[];
}

export interface NgDumpStatus {
  state: NgStateName;
  /** 64 KB chunks written so far, out of 96 for a 6 MB dump. */
  chunk: number;
  chunks: number;
  error: number;
  errorText: string;
}

export function decodeIdentity(data: Uint8Array): NgIdentity | null {
  if (data.length < 4) return null;
  const features = data[3]!;
  const features1 = data[4] ?? 0;
  const features2 = data[5] ?? 0;
  const features3 = data[6] ?? 0;
  return {
    protocol: data[0]!,
    baseVersion: `V${data[1]}.${String(data[2]).padStart(2, "0")}`,
    features,
    features1,
    features2,
    features3,
    featureNames: [
      ...NG_FEATURES.filter((f) => features & f.bit).map((f) => f.name),
      ...NG_FEATURES1.filter((f) => features1 & f.bit).map((f) => f.name),
      ...NG_FEATURES2.filter((f) => features2 & f.bit).map((f) => f.name),
      ...NG_FEATURES3.filter((f) => features3 & f.bit).map((f) => f.name),
    ],
  };
}

/** Does this Identify report a feature? The one rule from the protocol SSOT (ngver.gen.ts `ngFeature`,
 * REMOTE_PROTOCOL.md section 10.0): ngver >= the feature's `since`, or its bit is set. null (stock) = no. */
export function ngHas(id: NgIdentity | null | undefined, name: NgFeatureName): boolean {
  return !!id && ngFeature(id.protocol, [id.features, id.features1, id.features2, id.features3], name);
}

export function decodeStatus(data: Uint8Array): NgDumpStatus | null {
  if (data.length < 4) return null;
  const err = data[3]!;
  return {
    state: NgState[data[0]!] ?? "idle",
    chunk: ((data[1]! & 0x7f) << 7) | (data[2]! & 0x7f),
    chunks: 96,
    error: err,
    errorText: NG_ERRORS[err] ?? `unknown (${err})`,
  };
}

// ---------------------------------------------------------------------------------------
// phase2c probe ops (REMOTE_PROTOCOL.md section 10.1). 8-bit fields travel 7-in-8 packed.

export interface NgPortInfo {
  /** Port the request arrived on (0..3; 2 = BLE-MIDI module, 3 = panel [inferred]). */
  port: number;
  /** Stock controller binding: bound port + 1, 0 = none. */
  binding: number;
  /** Port 3 -> 0..2 forwarding flag (0x413B5408). */
  forwarding: number;
  /** Total request length as the piano assembled it. */
  length: number;
}

export function decodePort(data: Uint8Array): NgPortInfo | null {
  const r = unpack7(data);
  if (r.length < 8) return null;
  return { port: r[0]!, binding: r[1]!, forwarding: u32(r, 2), length: u16(r, 6) };
}

export const EchoMode = { Sink: 0, Echo: 1, Generate: 2 } as const;
export type EchoModeCode = (typeof EchoMode)[keyof typeof EchoMode];
/** Largest payload the cave sends back (echo / generate). */
export const NG_ECHO_MAX = 0x7c0;
/** Largest payload a request can carry: the piano assembles at most 0x800 bytes in total. */
export const NG_PAYLOAD_MAX = 0x800 - 9;

export interface NgEchoResult {
  mode: number;
  /** Bytes the piano counted (sink/echo) or generated. */
  count: number;
  crc: number;
  /** Echoed / generated bytes (empty for sink). */
  data: Uint8Array;
}

export function decodeEcho(data: Uint8Array): NgEchoResult | null {
  if (data.length < 8) return null;
  const st = unpack7(data.slice(1, 8));
  return { mode: data[0]!, count: u16(st, 0), crc: u32(st, 2), data: data.slice(8) };
}

export interface NgApoSnapshot {
  /** APO tick counter 0x4139E7E4 (20 ms ticks [unverified]). */
  counter: number;
  /** Forced-off watchdog 0x4139E7E8. */
  watchdog: number;
  /** BLE-MIDI module power 0x4139E7EC. */
  blePower: number;
  /** BT audio module power 0x4139E7F0 (1 = on). */
  btPower: number;
  /** Power switch hold count 0x4139E7F4. */
  switchHold: number;
  /** APO setting 0x4139E7F9: 0 off, 1 = 15 min, 2 = 60 min, 3 = 120 min. */
  apoSetting: number;
  /** Shutdown state 0x4139E7FC: 0 normal, 1 switch held, 2 shutdown requested. */
  state: number;
  /** A2DP link flag 0x413AE5B8 (1 = connected). */
  a2dp: number;
  /** iWRAP READY. flag 0x413AE5C1. */
  btReady: number;
  dialect: "AT" | "iWRAP";
  /** 0x419156A7: 1 = test / wave ROM mode (APO paused). */
  specialMode: number;
  /** All 32 raw bytes, for the record. */
  raw: Uint8Array;
}

export function decodeApo(data: Uint8Array): NgApoSnapshot | null {
  const r = unpack7(data);
  if (r.length < 32) return null;
  return {
    counter: u32(r, 0),
    watchdog: u32(r, 4),
    blePower: u32(r, 8),
    btPower: u32(r, 12),
    switchHold: u32(r, 16),
    apoSetting: r[21]!,
    state: r[24]!,
    a2dp: r[28]!,
    btReady: r[29]!,
    dialect: r[30] ? "AT" : "iWRAP",
    specialMode: r[31]!,
    raw: r,
  };
}

export interface NgBtLog {
  /** Ring write / read index at the snapshot (0..0x7FF). */
  wr: number;
  rd: number;
  lineLen: number;
  seq: number;
  /** 2 KB of BT module output, oldest byte first (zeros = not yet written since boot). */
  bytes: Uint8Array;
  /** The same as text lines (CR/LF split, empty lines and NUL padding dropped). */
  lines: string[];
}

export interface NgCanary {
  /** 0 not armed, 1 fill pending, 2 armed. */
  state: number;
  changed: number;
  first: number;
  last: number;
  lo: number;
  hi: number;
  /** Engine-loop passes since the fill (about 8 ms each [unverified]). */
  ticks: number;
}

export function decodeCanary(data: Uint8Array): NgCanary | null {
  if (data.length < 2 || data[0] !== 0) return null;
  const r = unpack7(data.slice(1));
  if (r.length < 25) return null;
  return {
    state: r[0]!,
    changed: u32(r, 1),
    first: u32(r, 5),
    last: u32(r, 9),
    lo: u32(r, 13),
    hi: u32(r, 17),
    ticks: u32(r, 21),
  };
}

export function btLogLines(bytes: Uint8Array): string[] {
  return String.fromCharCode(...bytes.filter((b) => b !== 0))
    .split(/[\r\n]+/)
    .map((l) => l.replace(/[^\x20-\x7e]/g, "."))
    .filter((l) => l.length > 0);
}

export interface NgLinkSample {
  mode: "sink" | "echo" | "generate";
  bytes: number;
  /** false = timed out (the test stops there). */
  replied: boolean;
  /** Count and CRC match what was sent / requested. */
  ok: boolean;
  /** Request sent -> reply received. */
  ms: number;
}

/** Core self-test (op 0x0A) result: raw 32 B layout of REMOTE_PROTOCOL.md section 10.2. */
export interface NgCoreTest {
  layout: number;
  checks: number;
  /** bit i = check i passed (CORE_TEST_CHECKS order). */
  passMask: number;
  passed: number;
  storeCrcDefaults: number;
  storeCrcAfter: number;
  txCrc: number;
  txBytes: number;
  txFrames: number;
  initialised: boolean;
  guardsOk: boolean;
  apoDue: boolean;
  ticks: number;
  ms: number;
  timeUs: number;
}

/** Names of the self-test checks, bit order (result/firmware/ports/cave/core_cave.h). */
export const CORE_TEST_CHECKS = [
  "R1 guards", "connect", "param set", "store", "request", "version", "NACK", "disconnect", "pack87",
] as const;

export function decodeCoreTest(data: Uint8Array): NgCoreTest | null {
  const r = unpack7(data);
  if (r.length < 32) return null;
  const mask = u16(r, 2);
  let passed = 0;
  for (let i = 0; i < r[1]!; i++) if (mask & (1 << i)) passed++;
  return {
    layout: r[0]!, checks: r[1]!, passMask: mask, passed,
    storeCrcDefaults: u32(r, 4), storeCrcAfter: u32(r, 8), txCrc: u32(r, 12), txBytes: u16(r, 16), txFrames: r[18]!,
    initialised: !!(r[19]! & 1), guardsOk: !!(r[19]! & 2), apoDue: !!(r[19]! & 4),
    ticks: u32(r, 20), ms: u32(r, 24), timeUs: u32(r, 28),
  };
}

/** op 0x1B 03 (session core, feat2 0x02): who is bound. REMOTE_PROTOCOL.md section 10.5. */
export interface SessionCore {
  /** bit p = stock port p bound to the session core (3 = the built-in panel, 2 = BLE-MIDI) */
  bound: number;
  /** stock binding (port + 1, 0 none): the controller that CONNECTed last */
  binding: number;
  /** the port this request came in on - stage 2 builds only (byte 45 bit 7), else undefined */
  port?: number;
}

/** `data` = the reply after the op byte: sub, rc, pack7(48 raw bytes). */
export function decodeSessionCore(data: Uint8Array): SessionCore | null {
  if (data.length < 3 || data[0] !== 0x03 || data[1] !== 0) return null;
  const r = unpack7(data.slice(2));
  if (r.length < 46 || r[0] !== 3) return null;
  const p = r[45]!;
  return { bound: r[2]! & 0x0f, binding: r[3]!, ...(p & 0x80 ? { port: p & 3 } : {}) };
}

