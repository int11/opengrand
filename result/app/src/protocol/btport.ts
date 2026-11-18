// OpenGrand BT port, 0x7D op 0x1D (slot payload ota_bt, Identify feat2 0x01):
// BLE-MIDI port 2 latch clear / active-sensing guard switches and the read-only BT audio module query
// (iWRAP SET / LIST / INFO). REMOTE_PROTOCOL.md section 10.6
import { NgClient, type NgOpCode, pack7, unpack7 } from "./ngext.ts";

export const BT_OP = 0x1d as NgOpCode;
export const BT_RC = ["ok", "bad value", "not bound", "busy", "BT audio module off", "UART1 receive latched", "test mode", "no such device", "piano not on", "no AVRCP link", "wrong state (read the module setting first / already that value)"] as const;
export const BtQuery = { all: 0, set: 1, list: 2, info: 3 } as const;
export type BtQueryName = keyof typeof BtQuery;
export const BT_SW_UNLATCH = 0x01;
export const BT_SW_AS_GUARD = 0x02;

export interface BtStatus {
  layout: number;
  switches: number;
  /** 0 idle, 1 sending, 2 receiving, 3 done */
  query: number;
  /** 1 truncated, 2 ring reset seen, 4 may have missed bytes, 8 aborted, 0x10 a command got no reply,
   *  0x20 a command answered SYNTAX ERROR was sent once more */
  queryFlags: number;
  textLength: number;
  sent: number;
  p2Flag: number;
  p2As: number;
  p2Count: number;
  p2Unlatches: number;
  asDisarms: number;
  p2Streak: number;
  uart1Flag: number;
  moduleOn: boolean;
  testMode: boolean;
  p2Latched: boolean;
  /** 28-byte status (2026-11-01 payloads): port 2 RX ring read / write index, ring repairs, last (wr - rd) - count */
  p2Ring?: { rd: number; wr: number; repairs: number; lastDiff: number };
  /** 40-byte status (BT audio source manager payloads): engine-loop passes, dropped module commands, 0x1D requests
   *  seen / dropped busy, the stage task 24 was last in (0 idle, 1 UART1 send, 2/3 module on/off), UART1 IER */
  loop?: { passes: number; txDrops: number; reqSeen: number; reqBusy: number; stage: number; ier: number };
}

/** 1D 0F: answered on task 19 at once - readable while the engine loop is stuck. */
export interface BtHeartbeat {
  passes: number;
  stage: number;
  txCount: number;
  txRd: number;
  txWr: number;
  ier: number;
  pending: boolean;
  sending: boolean;
  querying: boolean;
  moduleOn: boolean;
  busyDrops: number;
}

export function decodeBtHeartbeat(raw: Uint8Array): BtHeartbeat | null {
  if (raw.length < 16 || raw[0] !== 1) return null;
  const f = raw[13]!;
  return {
    passes: u16(raw, 2) + u16(raw, 4) * 65536, stage: raw[1]!, txCount: u16(raw, 6), txRd: u16(raw, 8), txWr: u16(raw, 10),
    ier: raw[12]!, pending: !!(f & 1), sending: !!(f & 2), querying: !!(f & 4), moduleOn: !!(f & 8), busyDrops: raw[15]!,
  };
}

// ---- BT audio source manager (1D 04 - 1D 09) ----
/** select = only the selected source plays (default since 2026-11-02); off = table + manual actions; newest = legacy */
export const SRC_MODE = { off: 0, newest: 1, select: 2 } as const;
/** follow (mode select): a source that starts playing by itself becomes the selected one instead of being refused */
export const SRC_FLAG = { closeOthers: 0x01, callPref: 0x02, keepPref: 0x04, recover: 0x08, follow: 0x10, noAvrcpCall: 0x20, noPlayStatus: 0x40 } as const;
export const SRC_DEV = { stream: 0x01, incoming: 0x02, closed: 0x04, pref: 0x08, active: 0x10, addr: 0x20, closing: 0x40 } as const;
/** second state byte (layout 2): selected, refused (it tried to play while not selected), has AVRCP, switch target,
 *  being called (sink-initiated CALL), parked (the piano closed its sound link to switch away; picking it calls it) */
export const SRC_DEV2 = { selected: 0x01, refused: 0x02, avrcp: 0x04, target: 0x08, calling: 0x10, parked: 0x20 } as const;
/** 1D 05 act: use = 「이 기기로 듣기」 (select it and switch: the one playing is paused, never closed; call it if needed,
 *  resume / play it); only = close every other source (last resort); pause /
 *  play one device; mute = close its A2DP link only (AVRCP stays). */
export const SRC_ACT = { close: 0, use: 1, call: 2, only: 3, pause: 4, play: 5, mute: 6 } as const;
/** policy defaults on the piano since 2026-11-02: mode select, flags keepPref; strategy 0 (firmware after ota_main
 *  a12c870a - the only one since the strategies were removed) */
export const SRC_POLICY_DEFAULT = { mode: 2, flags: 0x04, holdDs: 15, sweepS: 5, pref: null, strategy: 0 } as const;
/* (the switching strategies - 1D 06 byte 13, 1D 04 byte 5 bits 4-6 = strategy + 1 - were
 * removed in §24: one way to switch.  The app writes strategy 0 only; firmware after §24 reports 0 and refuses another
 * value; older firmware takes the 0 - the decided way.) */
/** the piano's own reconnect while the module's AUTOCALL is off (1D 06 byte 14 / 1D 04 byte 116 bits 0-1 = value + 1;
 *  firmware after ota_main e5bb2372, user decision 2026-11-09): off / the preferred device only / the preferred, else the
 *  device picked last (default).  Kept on the piano's NOR with the preferred device. */
export const SRC_RECONNECT = { off: 0, pref: 1, prefLast: 2 } as const;
/** experimental switch options (1D 06 byte 15 / 1D 04 byte 116 bit 7, bit 6 = the firmware knows them; firmware after
 *  ota_main 87579f5c) - RAM on the piano, its firmware default at every power-on (off).  lateStop: a device left by
 *  the switch that reports paused and still keeps its sound open the wait later gets the piano's stop (4.5 s after the
 *  piano's pause up to ota_main 753d97fb; since then the policy wait after its paused report, default 1.0 s) */
export const SRC_OPTION = { lateStop: 0x01 } as const;
/** the BT policy on the piano's NOR (1D 06 reply byte 16 / 1D 04 byte 116 bits 2-5) */
export const POLICY_STORE = { kept: 0x01, loaded: 0x02, pending: 0x04, failed: 0x08 } as const;
/** the switch (layout 2 tail); direct = the piano's START on the picked device while the other stream is open (only
 *  firmware from before the strategies were removed, with its strategy DIRECT / select only) */
export const SW_PHASE = { idle: 0, pause: 1, suspend: 2, close: 3, link: 4, call: 5, settle: 6, play: 7, start: 8, reopen: 9, direct: 10 } as const;
/** what the switch tried on the target (1D 04 byte 106): AVRCP PLAY, the sink's A2DP STREAMING START, reopening its
 *  A2DP (CLOSE + CALL), a fresh CALL, PLAY sent after its stream was already up (firmware after ota_main 6f181a70 -
 *  Windows streams silence on a fresh link while paused; older firmware never sets it).  `late`: the PLAY went after
 *  the switch had ended - the target's AVRCP came back late reporting paused (firmware after ota_main 1afe1978; set
 *  with `play`, so the result text is the PLAY one) */
export const SW_TRIED = { play: 0x01, start: 0x02, reopen: 0x04, fresh: 0x08, playUp: 0x10, kept: 0x20, notOurs: 0x40, late: 0x80 } as const;
/** a source's play status (1D 04 bytes 112..115, one per slot; firmware after ota_main 9e6c8c32): the AVRCP
 *  PLAYBACK_STATUS_CHANGED notification in bits 0-2 + live (registered), none (the device does not report it), ours (the
 *  piano paused it).  `kept` / `notOurs` above: no PLAY sent - it already plays / its status unknown and the piano had
 *  not paused it (Windows takes PLAY as play/pause, field 2026-11-02 night). */
export const PLAY_STATUS = { unknown: 0, stopped: 1, playing: 2, paused: 3, seeking: 4, error: 5 /* AVRCP 0xFF, firmware after
  ota_main 5ba5bb64 */ } as const;
export const PS_FLAG = { live: 0x08, none: 0x10, ours: 0x20 } as const;
/** module health (1D 04 byte 102): loop = it rebooted by itself twice within a minute (the piano holds its own calls),
 *  suspect = a device's lone AVRCP browsing channel came right before a reboot, pin = the firmware could not keep the
 *  module's decided settings this power-on (device type / AVRCP browsing / audio routing) */
export const MOD_HEALTH = { loop: 0x01, suspect: 0x02, pin: 0x04 } as const;
/** the `A2DP CODEC` event (1D 04 byte 107) */
export const CODEC_NAME: Record<number, string> = { 1: "SBC", 2: "AAC", 3: "aptX", 4: "aptX LL", 5: "?" };
export const SW_RESULT = { none: 0, playing: 1, ready: 2, busy: 3, noLink: 4, cancelled: 5 } as const;
/** AUTOCALL as the piano read it from the module's `SET` listing; 1D 09 ops */
export const AC_STATE = { unknown: 0, on: 1, off: 2 } as const;
export const AC_OP = { state: 0, read: 1, off: 2, restore: 3 } as const;
/** class of device as the piano read it from the module's `SET` listing (`SET BT CLASS <hex>`); 1D 0A ops (record
 *  an Android TV lists only devices whose class says audio) */
export const CO_STATE = { unknown: 0, known: 1 } as const;
export const CO_OP = { state: 0, read: 1, write: 2, restore: 3 } as const;
/** Rendering + Audio service bits, Audio/Video major, Loudspeaker minor - what op 1D 0A 02 writes by default */
export const COD_AUDIO_SINK = 0x240414;
/** a class the piano accepts for a write: Audio/Video major (0x04), the Rendering service bit, format bits 0 */
export const codAudioSink = (c: number) => c >= 0 && c <= 0xffffff && (c & 3) === 0 && ((c >> 8) & 0x1f) === 0x04 && (c & 0x040000) !== 0;
/** `001f00` - the module's own six hex digits */
export const codHex = (c: number) => c.toString(16).padStart(6, "0");
/** AVRCP controller features as the piano read them from the module's `SET` listing (`SET PROFILE AVRCP CONTROLLER
 *  <hex>`); 1D 0C ops (a Windows PC's lone browsing channel 0x1b rebooted the module).
 *  AN986 3.2 / RN 6.5.3: bits 0-3 = AVRCP categories 1-4, bit 4 = browsing. */
export const AVF_STATE = { unknown: 0, controller: 1, other: 2 } as const;
export const AVF_OP = { state: 0, read: 1, write: 2, restore: 3 } as const;
export const AVF_BROWSE = 0x10;
/** what 1D 0C 02 writes by default: categories 1-4, no browsing */
export const AVF_NO_BROWSE = 0x0f;
/** `1f` - two lower-case hex digits, the module's form */
export const avfHex = (f: number) => f.toString(16).padStart(2, "0");
/** audio routing as the piano read it from the module's `SET` listing (`SET CONTROL AUDIO INTERNAL I2S_SLAVE [EVENT]
 *  [KEEPALIVE]`, RM 6.67); 1D 0D ops (the phone (aptX) -> PC (SBC) silence, D74).
 *  EVENT: the module prints `AUDIO ROUTE <link> …` / `A2DP CODEC …`; KEEPALIVE: its DSP stays on between streams. */
export const AUD_STATE = { unknown: 0, plain: 1, other: 2 } as const;
export const AUD_OP = { state: 0, read: 1, write: 2, restore: 3 } as const;
export const AUD_FLAG = { event: 0x01, keepalive: 0x02 } as const;
/** the stock factory line's flags (`… KEEPALIVE`, REA 0x4013EE30) */
export const AUD_FACTORY = AUD_FLAG.keepalive;
/** the two flag sets a write may write: variant 1 = KEEPALIVE off + EVENT, variant 2 = EVENT added */
export const AUD_WRITES = [AUD_FLAG.event, AUD_FLAG.event | AUD_FLAG.keepalive] as const;
/** the module line for flags f (RM 6.67 order) - exactly what the piano sends */
export const audLine = (f: number) =>
  `SET CONTROL AUDIO INTERNAL I2S_SLAVE${f & AUD_FLAG.event ? " EVENT" : ""}${f & AUD_FLAG.keepalive ? " KEEPALIVE" : ""}`;

export interface BtSourcePolicy {
  mode: number;
  flags: number;
  /** two streams: the newest wins after this many 0.1 s */
  holdDs: number;
  /** close the other sources after this many s of streaming */
  sweepS: number;
  pref: number[] | null;
  /** the strategy byte (1D 06 byte 13): 0 since written as 0 whatever it holds; null = a
   *  firmware without it (left out when the policy is written, with everything after it) */
  strategy?: number | null;
  /** SRC_RECONNECT; null = a firmware without the choice (left out when the policy is written) */
  reconnect?: number | null;
  /** POLICY_STORE bits (read only); null = a firmware that keeps nothing */
  store?: number | null;
  /** SRC_OPTION bits; null = a firmware without them (left out when the policy is written) */
  options?: number | null;
  /** the late stop's wait after the paused report, 0.1 s units, 0 = the firmware default (1.0 s; firmware after
   * reports its default, 10) - 1D 06 byte 16 /
   *  1D 04 byte 119; null = a firmware without it (left out when the policy is written) */
  lateWait?: number | null;
}

export interface BtSource {
  addr: number[];
  /** bit per iWRAP link id */
  links: number;
  a2dp: number;
  state: number;
  streamS: number;
  /** SRC_DEV2 bits (layout 2; 0 on layout 1) */
  state2: number;
  /** times the piano refused its stream lately */
  refusals: number;
  /** PLAY_STATUS | PS_FLAG byte (1D 04 of 116 bytes; null before) */
  ps: number | null;
}

export interface BtSwitch {
  phase: number;
  result: number;
  /** running: since the tap; done: how long it took */
  ms: number;
}

export interface BtSources {
  policy: BtSourcePolicy;
  moduleOn: boolean;
  canAct: boolean;
  restarting: boolean;
  streams: number;
  closes: number;
  unanswered: number;
  recoveries: number;
  dual: number;
  lines: number;
  calls: number;
  queued: number;
  devices: BtSource[];
  /** layout 2 (null on layout 1): the selected source, the switch, AUTOCALL */
  selected: number[] | null;
  sw: BtSwitch | null;
  autocall: { state: number; op: number; rc: number } | null;
  refusals: number;
  suspends: number;
  /** 1D 04 of 119 bytes (null before): module lines whose play status could not be read (asked again) / read after a
   *  garbage prefix - field 2026-11-09, interleaved lines */
  lineFixes: { garbled: number; recovered: number } | null;
  /** 1D 04 of 112 bytes (null before): module reboots, the switch's tries, the last codec */
  health: { flags: number; reboots: number; sinceS: number | null } | null;
  swTried: number;
  codec: { codec: number; dev: number; rate: number } | null;
}

/** aa:bb:cc:dd:ee:ff */
export const macText = (a: number[]) => a.map((b) => b.toString(16).padStart(2, "0")).join(":");
/** the last two bytes, for the UI: …11:a1 */
export const macShort = (a: number[]) => "…" + macText(a.slice(4));

export function decodeBtSources(raw: Uint8Array): BtSources | null {
  const v2 = raw[0] === 2 && raw.length >= 102;
  const v3 = v2 && raw.length >= 112;
  const v4 = v2 && raw.length >= 116;
  const v5 = v2 && raw.length >= 119;
  const v6 = v2 && raw.length >= 120;
  if (!v2 && (raw.length < 76 || raw[0] !== 1)) return null;
  const stride = v2 ? 14 : 12;
  const devices: BtSource[] = [];
  for (let i = 0; i < 4; i++) {
    const o = 28 + stride * i;
    if (!(raw[o]! & 0x80)) continue;
    devices.push({
      state: raw[o]! & 0x7f, addr: [...raw.slice(o + 1, o + 7)], links: u16(raw, o + 7), a2dp: u16(raw, o + 9), streamS: raw[o + 11]!,
      state2: v2 ? raw[o + 12]! : 0, refusals: v2 ? raw[o + 13]! : 0, ps: v4 ? raw[112 + i]! : null,
    });
  }
  const t = 84;
  return {
    selected: v2 && raw[t + 11] ? [...raw.slice(t + 12, t + 18)] : null,
    sw: v2 ? { phase: raw[t + 1]!, result: raw[t + 2]!, ms: raw[t + 3]! * 100 } : null,
    autocall: v2 ? { state: raw[t + 4]!, op: raw[t + 5]!, rc: raw[t + 6]! } : null,
    refusals: v2 ? u16(raw, t + 7) : 0,
    suspends: v2 ? u16(raw, t + 9) : 0,
    health: v3 ? { flags: raw[t + 18]!, reboots: u16(raw, t + 19), sinceS: raw[t + 21] === 0xff ? null : raw[t + 21]! } : null,
    swTried: v3 ? raw[t + 22]! : 0,
    codec: v3 && raw[t + 23] ? { codec: raw[t + 23]!, dev: raw[t + 24]!, rate: raw[t + 25]! } : null,
    lineFixes: v5 ? { garbled: raw[117]!, recovered: raw[118]! } : null,
    policy: { mode: raw[1]!, flags: raw[2]!, holdDs: raw[3]!, sweepS: raw[4]!, pref: raw[18] ? [...raw.slice(19, 25)] : null,
      strategy: (raw[5]! >> 4) & 7 ? ((raw[5]! >> 4) & 7) - 1 : null,
      ...(v5 ? { reconnect: raw[116]! & 3 ? (raw[116]! & 3) - 1 : null, store: (raw[116]! >> 2) & 0x0f,
        options: raw[116]! & 0x40 ? (raw[116]! & 0x80 ? SRC_OPTION.lateStop : 0) : null } : {}),
      ...(v6 ? { lateWait: raw[119]! } : {}) },
    moduleOn: !!(raw[5]! & 1), canAct: !!(raw[5]! & 2), restarting: !!(raw[5]! & 4), streams: raw[6]!,
    closes: u16(raw, 8), unanswered: u16(raw, 10), recoveries: u16(raw, 12), dual: u16(raw, 14), lines: u16(raw, 16),
    calls: u16(raw, 25), queued: raw[27]!, devices,
  };
}

const unpackAddr = (d: Uint8Array) => [...unpack7(d)].slice(0, 6);

export interface BtAutocall {
  rc: number;
  op: number;
  state: number;
  /** an op whose read-back has not come yet */
  busy: number;
  lastRc: number;
  /** the line was read from this module (else the stock factory line) */
  saved: boolean;
  line: string;
}

export interface BtClass {
  rc: number;
  op: number;
  /** CO_STATE */
  state: number;
  /** an op whose read-back has not come yet */
  busy: number;
  /** 0 the read-back shows what was asked, 5 it does not, 3 dropped (standby) */
  lastRc: number;
  /** the class to restore was read from this module this power-on (else 001f00) */
  saved: boolean;
  /** the class read last */
  cod: number;
  /** what restore writes */
  restore: number;
  /** what a write writes (the last one asked, else 240414) */
  write: number;
}

const cod7 = (d: Uint8Array, i: number) => ((d[i]! << 21) | (d[i + 1]! << 14) | (d[i + 2]! << 7) | d[i + 3]!) >>> 0;

/** 5D 0A reply bytes after the op byte (rc first) */
export function decodeBtClass(d: Uint8Array): BtClass | null {
  if (d.length < 19 || d[0] !== 0x0a) return null;
  return { rc: d[1]!, op: d[2]!, state: d[3]!, busy: d[4]!, lastRc: d[5]!, saved: !!d[6], cod: cod7(d, 7), restore: cod7(d, 11), write: cod7(d, 15) };
}

/** 1D 0A request bytes after the op byte: op, and for a write of another class than 240414 its 24 bits as 4 x 7 */
export function encodeBtClass(op: number, cod?: number): number[] {
  return cod == null ? [0x0a, op] : [0x0a, op, (cod >> 21) & 0x7f, (cod >> 14) & 0x7f, (cod >> 7) & 0x7f, cod & 0x7f];
}

// ---- the piano's record-bus level (1D 0B, read only) ----
/** |sample| from here counts as signal (firmware BT_LVL_FLOOR, about -66 dBFS) */
export const LVL_FLOOR = 16;
export const LVL_FLAG = { moved: 0x01, first: 0x02, rec: 0x04, wpOdd: 0x08 } as const;

/** One scan of the DSP's capture of the piano's record bus (piano + BT audio + LINE IN + USB playback, after the
 *  record gain - the last ~26 ms). */
export interface BtLevel {
  layout: number;
  /** LVL_FLAG bits */
  flags: number;
  /** peak |sample| of slot 0 / slot 1 (L / R), 0..32767 */
  peakL: number;
  peakR: number;
  mean: number;
  /** samples (of 2304) with |x| >= LVL_FLOOR */
  over: number;
  gain: number;
  /** the DSP's write position in the two buffers (bytes), 0xFFFF outside */
  pos: number;
  count: number;
}

export function decodeBtLevel(raw: Uint8Array): BtLevel | null {
  if (raw.length < 16 || raw[0] !== 1) return null;
  return { layout: raw[0]!, flags: raw[1]!, peakL: u16(raw, 2), peakR: u16(raw, 4), mean: u16(raw, 6), over: u16(raw, 8),
    gain: u16(raw, 10), pos: u16(raw, 12), count: u16(raw, 14) };
}

/** "signal" / "silent" / "unknown" (the capture is not running or not known to run yet) */
export type LevelVerdict = "signal" | "silent" | "unknown";

export function levelVerdict(l: BtLevel): LevelVerdict {
  if (l.flags & LVL_FLAG.wpOdd) return "unknown";
  if (!(l.flags & LVL_FLAG.moved)) return "unknown";   // first scan, or the DSP did not move since the last one
  return Math.max(l.peakL, l.peakR) >= LVL_FLOOR ? "signal" : "silent";
}

/** peak in dBFS, rounded (-Infinity for 0) */
export function levelDb(peak: number): number {
  return peak > 0 ? Math.round(20 * Math.log10(peak / 32768)) : -Infinity;
}

export interface BtAvrcpFeatures {
  rc: number;
  op: number;
  /** AVF_STATE */
  state: number;
  /** an op whose read-back has not come yet */
  busy: number;
  /** 0 the read-back shows what was asked, 5 it does not, 3 dropped (standby) */
  lastRc: number;
  /** the features to restore were read from this module this power-on (else the factory 1f) */
  saved: boolean;
  /** the controller features read last */
  features: number;
  /** what restore writes */
  restore: number;
  /** what a write writes (the last one asked, else 0f) */
  write: number;
}

/** 5D 0C reply bytes after the op byte (rc first) */
export function decodeBtAvrcpFeatures(d: Uint8Array): BtAvrcpFeatures | null {
  if (d.length < 13 || d[0] !== 0x0c) return null;
  const v = (i: number) => (d[i]! << 7) | d[i + 1]!;
  return { rc: d[1]!, op: d[2]!, state: d[3]!, busy: d[4]!, lastRc: d[5]!, saved: !!d[6], features: v(7), restore: v(9), write: v(11) };
}

/** 1D 0C request bytes after the op byte: op, and for a write of other features than 0f the features byte */
export function encodeBtAvrcpFeatures(op: number, features?: number): number[] {
  return features == null ? [0x0c, op] : [0x0c, op, features & 0x7f];
}

export interface BtAudio {
  rc: number;
  op: number;
  /** AUD_STATE */
  state: number;
  /** an op whose read-back has not come yet */
  busy: number;
  /** 0 the read-back shows what was asked, 5 it does not, 3 dropped (standby) */
  lastRc: number;
  /** the flags to restore were read from this module this power-on (else the factory KEEPALIVE) */
  saved: boolean;
  /** AUD_FLAG bits read last */
  flags: number;
  /** what restore writes */
  restore: number;
  /** what a write writes (the last one asked, else EVENT) */
  write: number;
  /** the last `AUDIO ROUTE <link> <type> …` event (only with EVENT): link id, its type A2DP, the device slot (1D 04
   *  order) on that link; null = none since the module started */
  route: { link: number; a2dp: boolean; dev: number | null } | null;
}

/** 5D 0D reply bytes after the op byte (rc first) */
export function decodeBtAudio(d: Uint8Array): BtAudio | null {
  if (d.length < 12 || d[0] !== 0x0d) return null;
  const r = d[10]! & 0x1f;
  return { rc: d[1]!, op: d[2]!, state: d[3]!, busy: d[4]!, lastRc: d[5]!, saved: !!d[6], flags: d[7]!, restore: d[8]!, write: d[9]!,
    route: r ? { link: r - 1, a2dp: !!(d[10]! & 0x40), dev: d[11]! === 0x7f ? null : d[11]! } : null };
}

/** 1D 0D request bytes after the op byte: op, and for a write the flag set (one of AUD_WRITES) */
export function encodeBtAudio(op: number, flags?: number): number[] {
  return flags == null ? [0x0d, op] : [0x0d, op, flags & 0x7f];
}

// ---- module line log (1D 08, developer view) ----
export const LOG_FLAG = { sent: 0x01, cut: 0x02, event: 0x04 } as const;

export interface BtLogRecord {
  /** 14-bit sequence number */
  seq: number;
  /** piano time, ms (10 ms resolution) since the BT port started */
  tMs: number;
  /** LOG_FLAG bits: sent = a command the piano sent to the module, event = the piano's own note, else a module line */
  flags: number;
  text: string;
}

export interface BtLogChunk {
  first: number;
  next: number;
  nowMs: number;
  records: BtLogRecord[];
}

const u28 = (b: Uint8Array, o: number) => ((b[o]! << 21) | (b[o + 1]! << 14) | (b[o + 2]! << 7) | b[o + 3]!) >>> 0;

/** 5D 08 data (after the op): sub 08, rc, first:2, next:2, now:4, records (seq:2 t:4 flags len text). */
export function decodeBtLog(d: Uint8Array): BtLogChunk | null {
  if (d.length < 10 || d[0] !== 8 || d[1] !== 0) return null;
  const records: BtLogRecord[] = [];
  for (let o = 10; o + 8 <= d.length;) {
    const n = d[o + 7]!;
    if (o + 8 + n > d.length) break;
    records.push({ seq: (d[o]! << 7) | d[o + 1]!, tMs: u28(d, o + 2) * 10, flags: d[o + 6]!, text: String.fromCharCode(...d.slice(o + 8, o + 8 + n)) });
    o += 8 + n;
  }
  return { first: (d[2]! << 7) | d[3]!, next: (d[4]! << 7) | d[5]!, nowMs: u28(d, 6) * 10, records };
}

const u16 = (b: Uint8Array, o: number) => b[o]! | (b[o + 1]! << 8);

export function decodeBtStatus(raw: Uint8Array): BtStatus | null {
  if (raw.length < 20) return null;
  return {
    layout: raw[0]!, switches: raw[1]!, query: raw[2]!, queryFlags: raw[3]!, textLength: u16(raw, 4), sent: raw[6]!,
    p2Flag: raw[8]!, p2As: raw[9]!, p2Count: u16(raw, 10), p2Unlatches: u16(raw, 12), asDisarms: u16(raw, 14),
    p2Streak: raw[16]!, uart1Flag: raw[17]!, moduleOn: !!(raw[18]! & 1), testMode: !!(raw[18]! & 2), p2Latched: !!(raw[18]! & 4),
    ...(raw.length >= 28
      ? { p2Ring: { rd: u16(raw, 20), wr: u16(raw, 22), repairs: u16(raw, 24), lastDiff: (u16(raw, 26) << 16) >> 16 } }
      : {}),
    ...(raw.length >= 40
      ? { loop: { passes: u16(raw, 28) + u16(raw, 30) * 65536, txDrops: u16(raw, 32), reqSeen: u16(raw, 34), reqBusy: u16(raw, 36), stage: raw[38]!, ier: raw[39]! } }
      : {}),
  };
}

export interface BtQueryResult {
  rc: number;
  text: string;
  status: BtStatus | null;
}

export class BtPort {
  private readonly ng: NgClient;
  constructor(ng: NgClient) {
    this.ng = ng;
  }

  /** Third identify byte (feat2); 0 when the piano sends five bytes, null = no OpenGrand reply. */
  async feat2(timeoutMs = 700): Promise<number | null> {
    const d = await this.ng.call(0x01 as NgOpCode, [], timeoutMs);
    return d ? (d[5] ?? 0) : null;
  }

  async status(timeoutMs = 700): Promise<BtStatus | null> {
    const d = await this.ng.call(BT_OP, [0], timeoutMs, (x) => x[0] === 0);
    return d && d.length >= 2 ? decodeBtStatus(unpack7(d.slice(2))) : null;
  }

  /** 1D 0F heartbeat (answered on task 19 even while the engine loop is stuck), null = no reply. */
  async heartbeat(timeoutMs = 700): Promise<BtHeartbeat | null> {
    const d = await this.ng.call(BT_OP, [0x0f], timeoutMs, (x) => x[0] === 0x0f);
    return d && d.length >= 2 ? decodeBtHeartbeat(unpack7(d.slice(2))) : null;
  }

  /** 1D 04: the BT audio sources the module holds and the policy, null = no reply (older payloads). */
  async sources(timeoutMs = 700): Promise<BtSources | null> {
    const d = await this.ng.call(BT_OP, [4], timeoutMs, (x) => x[0] === 4);
    return d && d.length >= 2 && d[1] === 0 ? decodeBtSources(unpack7(d.slice(2))) : null;
  }

  /** 1D 05: SRC_ACT on a device; rc (BT_RC), null = no reply. */
  async sourceAction(act: number, addr: number[], timeoutMs = 700): Promise<number | null> {
    const d = await this.ng.call(BT_OP, [5, act, ...pack7(addr)], timeoutMs, (x) => x[0] === 5);
    return d && d.length >= 2 ? d[1]! : null;
  }

  /** 1D 08: module line log records from sequence `from` on (null = no reply: a payload without the log). */
  async moduleLog(from: number, timeoutMs = 700): Promise<BtLogChunk | null> {
    const d = await this.ng.call(BT_OP, [8, (from >> 7) & 0x7f, from & 0x7f], timeoutMs, (x) => x[0] === 8);
    return d ? decodeBtLog(d) : null;
  }

  /** 1D 06 without arguments: the policy (RAM on the piano; the reconnect choice and the preferred device also on NOR,
   *  firmware after ota_main e5bb2372). */
  async policy(timeoutMs = 700): Promise<BtSourcePolicy | null> {
    const d = await this.ng.call(BT_OP, [6], timeoutMs, (x) => x[0] === 6);
    return d && d.length >= 14 && d[1] === 0 ? decodePolicy(d.slice(2)) : null;
  }

  async setPolicy(p: BtSourcePolicy, timeoutMs = 700): Promise<{ rc: number; policy: BtSourcePolicy } | null> {
    const d = await this.ng.call(BT_OP, [6, p.mode, p.flags, p.holdDs & 0x7f, p.sweepS & 0x7f, p.pref ? 1 : 0, ...pack7(p.pref ?? [0, 0, 0, 0, 0, 0]),
      ...(p.strategy != null ? [0 /* the strategy: one way since */, ...(p.reconnect != null
        ? [p.reconnect & 0x7f, ...(p.options != null ? [p.options & 0x7f, ...(p.lateWait != null ? [p.lateWait & 0x7f] : [])] : [])] : [])] : [])],
      timeoutMs, (x) => x[0] === 6);
    return d && d.length >= 14 ? { rc: d[1]!, policy: decodePolicy(d.slice(2)) } : null;
  }

  /** 1D 09: AUTOCALL (a module setting) - state (op 0), read (1: the piano sends `SET`), off (2) / restore (3): the
   *  only module-setting writes, bound controller only.  line = what restore would write.  null = no reply. */
  async autocall(op: number, timeoutMs = 700): Promise<BtAutocall | null> {
    const d = await this.ng.call(BT_OP, [9, op], timeoutMs, (x) => x[0] === 9);
    if (!d || d.length < 8) return null;
    return { rc: d[1]!, op: d[2]!, state: d[3]!, busy: d[4]!, lastRc: d[5]!, saved: !!d[6], line: String.fromCharCode(...d.slice(8, 8 + d[7]!)) };
  }

  /** 1D 0A: class of device (a module setting) - state (op 0), read (1: the piano sends `SET`), write (2: an audio sink
   *  class, 240414 unless `cod` given) / restore (3: the class read before, else 001f00): with 1D 09 the only
   *  module-setting writes, bound controller only.  null = no reply (a payload without the op). */
  async deviceClass(op: number, cod?: number, timeoutMs = 700): Promise<BtClass | null> {
    const d = await this.ng.call(BT_OP, encodeBtClass(op, cod), timeoutMs, (x) => x[0] === 0x0a);
    return d ? decodeBtClass(d) : null;
  }

  /** 1D 0B: one scan of the piano's record bus (read only, any port).  null = no reply (a payload without the op). */
  async level(timeoutMs = 700): Promise<BtLevel | null> {
    const d = await this.ng.call(BT_OP, [0x0b], timeoutMs, (x) => x[0] === 0x0b);
    return d && d.length >= 2 && d[1] === 0 ? decodeBtLevel(unpack7(d.slice(2))) : null;
  }

  /** 1D 0C: AVRCP controller features (a module setting) - state (op 0), read (1: the piano sends `SET`), write (2:
   *  without the browsing bit, 0f unless `features` given) / restore (3: the features read before, else 1f): with
   *  1D 09 / 1D 0A the only module-setting writes, bound controller only; the module applies it after a restart
   *  (1D 07).  null = no reply (a payload without the op). */
  async avrcpFeatures(op: number, features?: number, timeoutMs = 700): Promise<BtAvrcpFeatures | null> {
    const d = await this.ng.call(BT_OP, encodeBtAvrcpFeatures(op, features), timeoutMs, (x) => x[0] === 0x0c);
    return d ? decodeBtAvrcpFeatures(d) : null;
  }

  /** 1D 0D: audio routing (a module setting `SET CONTROL AUDIO INTERNAL I2S_SLAVE …`) - state (op 0), read (1: the
   *  piano sends `SET`), write (2: `flags` one of AUD_WRITES, EVENT unless given) / restore (3: the flags read before,
   *  else KEEPALIVE): with 1D 09 / 0A / 0C the only module-setting writes, bound controller only; the module applies it
   *  after a restart (1D 07).  null = no reply (a payload without the op). */
  async audio(op: number, flags?: number, timeoutMs = 700): Promise<BtAudio | null> {
    const d = await this.ng.call(BT_OP, encodeBtAudio(op, flags), timeoutMs, (x) => x[0] === 0x0d);
    return d ? decodeBtAudio(d) : null;
  }

  /** 1D 07: restart the BT audio module (stock power off / on) - links drop, AUTOCALL re-links. */
  async restartModule(timeoutMs = 700): Promise<number | null> {
    const d = await this.ng.call(BT_OP, [7], timeoutMs, (x) => x[0] === 7);
    return d && d.length >= 2 ? d[1]! : null;
  }

  /** rc (BT_RC index), null = no reply. */
  async start(which: BtQueryName = "all", timeoutMs = 700): Promise<number | null> {
    const d = await this.ng.call(BT_OP, [1, BtQuery[which]], timeoutMs, (x) => x[0] === 1);
    return d && d.length >= 2 ? d[1]! : null;
  }

  async setSwitches(sw: number, timeoutMs = 700): Promise<{ rc: number; switches: number } | null> {
    const d = await this.ng.call(BT_OP, [3, sw & 3], timeoutMs, (x) => x[0] === 3);
    return d && d.length >= 3 ? { rc: d[1]!, switches: d[2]! } : null;
  }

  /** The captured module text, chunk by chunk. */
  async readText(timeoutMs = 700): Promise<string | null> {
    let off = 0;
    let out = "";
    for (let i = 0; i < 200; i++) {
      const d = await this.ng.call(BT_OP, [2, (off >> 7) & 0x7f, off & 0x7f], timeoutMs,
        (x) => x[0] === 2 && x.length >= 7 && ((x[5]! << 7) | x[6]!) === off);
      if (!d || d[1] !== 0) return null;
      const total = (d[3]! << 7) | d[4]!;
      const chunk = d.slice(7);
      out += String.fromCharCode(...chunk);
      off += chunk.length;
      if (off >= total || chunk.length === 0) break;
    }
    return out;
  }

  /** Start a query, wait until the piano is done (polling status), read the text. */
  async query(which: BtQueryName = "all", opts: { pollMs?: number; maxMs?: number; sleep?: (ms: number) => Promise<void> } = {}): Promise<BtQueryResult | null> {
    const sleep = opts.sleep ?? ((ms: number) => new Promise<void>((r) => setTimeout(r, ms)));
    const rc = await this.start(which);
    if (rc === null) return null;
    if (rc !== 0) return { rc, text: "", status: await this.status() };
    let st: BtStatus | null = null;
    for (let t = 0; t < (opts.maxMs ?? 20000); t += opts.pollMs ?? 250) {
      await sleep(opts.pollMs ?? 250);
      st = await this.status();
      if (st && st.query === 3) break;
    }
    const text = await this.readText();
    return { rc: 0, text: text ?? "", status: st };
  }
}

/** 5D 06 data after rc: mode flags hold sweep has_pref pack7(pref) [strategy [reconnect store [options]]] - the strategy
 *  byte from firmware after ota_main a12c870a, reconnect + NOR store bits after e5bb2372, options after 87579f5c, the
 *  late stop's wait after 753d97fb (older:
 *  null / left out) */
function decodePolicy(d: Uint8Array): BtSourcePolicy {
  return { mode: d[0]!, flags: d[1]!, holdDs: d[2]!, sweepS: d[3]!, pref: d[4] ? unpackAddr(d.slice(5, 12)) : null,
    strategy: d.length >= 13 ? d[12]! : null, ...(d.length >= 15 ? { reconnect: d[13]!, store: d[14]! } : {}),
    ...(d.length >= 16 ? { options: d[15]! } : {}), ...(d.length >= 17 ? { lateWait: d[16]! } : {}) };
}
