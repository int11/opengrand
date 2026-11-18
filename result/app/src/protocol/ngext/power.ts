// Power policy and partial standby (REMOTE_PROTOCOL.md section 10.4, firmware ca79ng/power.h).
import { u16 } from "./bytes.ts";
import type { NgClient } from "./client.ts";
import { NgOp, pack7, parseNgReply, unpack7 } from "./wire.ts";

/** Max hold value meaning "as long as the stream is open". */
export const HOLD_UNLIMITED = 0xffff;
export const HOLD_MAX_MIN = 1440;
export const CAP_MIN_MIN = 30;
export const CAP_MAX_MIN = 1440;
/** Standby cap value meaning "no cap": standby until something wakes the piano, never the real
 * power-off (slot payload ota_power3 and later; older payloads answer rc 1). */
export const CAP_NONE = 0xffff;

export interface PowerPolicy {
  /** APO expiry enters partial standby (true, OpenGrand default) instead of the real power-off. */
  apoStandby: boolean;
  /** Bluetooth audio "connected, not playing" also holds the APO (default false). */
  holdConnected: boolean;
  /** Max hold per STREAMING START in minutes: 0 = rule off (stock), HOLD_UNLIMITED = no cap. While the stream is open the
   * piano does not auto-off up to this; when it closes the menu APO time counts from then; at the max hold the APO acts
   * right away unless the piano was used within the menu time (09_p3_power §9; payloads up to ota_main 12545df6 waited
   * the menu time after it). Default 300 (5 h, owner decision 2026-11-02). */
  maxHoldMin: number;
  /** Standby length before the real power-off, minutes (CAP_MIN_MIN..CAP_MAX_MIN), CAP_NONE = no cap. */
  standbyCapMin: number;
  /** Standby keeps the Bluetooth audio module on and an A2DP stream start wakes the piano ("TV wake",
   * default true). Wire: flag bit 2 = the opposite (standby switches it off). Only payloads with
   * NG_FEAT1_TVWAKE act on it; ota_power ignores the bit and always switches the module off. */
  btWake: boolean;
}

export const POWER_POLICY_DEFAULT: PowerPolicy = { apoStandby: true, holdConnected: false, maxHoldMin: 300, standbyCapMin: 480, btWake: true };

/** Raw 8 B wire form (version 1). */
export function encodePowerPolicy(p: PowerPolicy): number[] {
  const flags = (p.apoStandby ? 1 : 0) | (p.holdConnected ? 2 : 0) | (p.btWake ? 0 : 4);
  return [1, flags, p.maxHoldMin & 0xff, (p.maxHoldMin >> 8) & 0xff, p.standbyCapMin & 0xff, (p.standbyCapMin >> 8) & 0xff, 0, 0];
}

export function decodePowerPolicy(raw: Uint8Array): PowerPolicy | null {
  if (raw.length < 6 || raw[0] !== 1) return null;
  return { apoStandby: !!(raw[1]! & 1), holdConnected: !!(raw[1]! & 2), maxHoldMin: u16(raw, 2), standbyCapMin: u16(raw, 4), btWake: !(raw[1]! & 4) };
}

export const PolicyResult = { Ok: 0, BadValue: 1, NotBound: 2, BadState: 3, SaveFailed: 4, NoHooks: 5, Busy: 6, ApoOff: 7 } as const;

/** Stock APO limits per menu setting (table 0x4009EB1C), 20 ms ticks: 15 / 60 / 120 min; 0 = Off. */
export const APO_LIMIT_TICKS = [0, 45000, 180000, 360000] as const;

/** Seconds until the auto power-off from the op 0x1A status (menu limit - stock counter, 20 ms ticks); null = not
 * counting (APO Off, held by Bluetooth audio, not on). */
export function apoSecondsLeft(d: Pick<PowerDiag, "apoSetting" | "counter" | "holding" | "state">): number | null {
  const lim = APO_LIMIT_TICKS[d.apoSetting as 0 | 1 | 2 | 3] ?? 0;
  if (!lim || d.holding || d.state !== "on") return null;
  return Math.max(0, Math.round((lim - d.counter) / 50));
}

/** APO test shortcut 19 04 (09_p3_power §8): allowed seconds (0 = the full menu time again). */
export const APO_TEST_MIN_S = 10;
export const APO_TEST_MAX_S = 600;

export interface ApoTestReply extends StandbyReply {
  /** seconds asked (echo) */
  secs: number;
  /** stock menu setting 0 Off, 1 15 min, 2 60 min, 3 120 min */
  setting: number;
  /** Bluetooth audio holds the auto power-off now (the countdown waits for STOP) */
  held: boolean;
}

/** Why a remote off (19 03) was refused with rc Busy (ca79ng/power.h POWER_OFF_*). */
export const OffReason = { None: 0, Recording: 1, FileOp: 2, Flash: 3 } as const;

export interface RemoteOffReply extends StandbyReply {
  /** OffReason, meaningful with rc Busy */
  why: number;
}

export interface PowerPolicyReply {
  /** 0 ok, 1 bad value, 2 not bound (set from a port that did not CONNECT), 4 saved in RAM only */
  rc: number;
  policy: PowerPolicy;
}

export const StandbyState = ["on", "standby", "waking", "off-pending", "off"] as const;
export type StandbyStateName = (typeof StandbyState)[number];

export interface StandbyReply {
  rc: number;
  state: StandbyStateName;
}

/** Unsolicited standby notice `59 7F 00 <state>` (piano -> bound controllers, never the panel). */
export function parseStandbyNotice(msg: Uint8Array): StandbyStateName | null {
  const r = parseNgReply(msg);
  if (!r || r.op !== NgOp.Standby || r.data.length < 3 || r.data[0] !== 0x7f) return null;
  return StandbyState[r.data[2]!] ?? null;
}

function decodePolicyReply(d: Uint8Array): PowerPolicyReply | null {
  if (d.length < 2) return null;
  const policy = decodePowerPolicy(unpack7(d.slice(2)));
  return policy ? { rc: d[1]!, policy } : null;
}

/** Op 0x1A (slot payload ota_power only): what the power port sees on the piano. */
export interface PowerDiag {
  hooks: number; // bit 0 Kawai SysEx (panel touch), 1 headphone poll, 2 executor, 3 APO (20 ms)
  state: StandbyStateName;
  silenced: boolean;
  holding: boolean; // BT audio holds the auto power-off now
  btAudio: "idle" | "connected" | "streaming";
  store: "unknown" | "blank" | "mounted" | "error";
  apoSetting: number;
  counter: number; // stock APO counter, 20 ms ticks
  timerTicks: number;
  expiries: number;
  standbyMs: number;
  heldMs: number;
  hpSkips: number;
  execClears: number;
  touches: number;
  norErrors: number;
  relight: number;
  /** last wake cause: 0 none, 1 app, 2 panel touch, 3 power button, 4 key, 5 Bluetooth stream start */
  wake: number;
  /** layout 2 (ota_power2, TV wake) only */
  tv?: { btModuleOn: boolean; armed: boolean; latched: boolean; rxFlag: number; unlatches: number; ringResets: number };
}

export function decodePowerDiag(raw: Uint8Array): PowerDiag | null {
  if (raw.length < 48 || (raw[0] !== 1 && raw[0] !== 2) || (raw[0] === 2 && raw.length < 52)) return null;
  const u32 = (o: number) => (raw[o]! | (raw[o + 1]! << 8) | (raw[o + 2]! << 16) | (raw[o + 3]! << 24)) >>> 0;
  return {
    hooks: raw[1]!, state: StandbyState[raw[2]!] ?? "on", silenced: !!(raw[3]! & 1), holding: !!(raw[3]! & 8),
    btAudio: (["idle", "connected", "streaming"] as const)[raw[4]!] ?? "idle",
    store: (["unknown", "blank", "mounted", "error"] as const)[raw[5]!] ?? "error",
    apoSetting: raw[6]!, counter: u32(8), timerTicks: u32(12), expiries: u32(16), standbyMs: u32(20), heldMs: u32(24),
    hpSkips: u16(raw, 36), execClears: u16(raw, 38), touches: u16(raw, 40), norErrors: u16(raw, 42), relight: raw[47]!,
    wake: raw[7]!,
    ...(raw[0] === 2
      ? { tv: { btModuleOn: !!(raw[48]! & 1), armed: !!(raw[48]! & 2), latched: !!(raw[48]! & 4), rxFlag: raw[49]!, unlatches: raw[50]!, ringResets: raw[51]! } }
      : {}),
  };
}

/** Power policy / standby over an NgClient-like request function. */
export class NgPower {
  private readonly ng: NgClient;
  constructor(ng: NgClient) {
    this.ng = ng;
  }
  async get(timeoutMs = 700): Promise<PowerPolicyReply | null> {
    const d = await this.ng.call(NgOp.PowerPolicy, [0x00], timeoutMs, (x) => x[0] === 0x00);
    return d ? decodePolicyReply(d) : null;
  }
  /** Sets and saves; the reply carries the policy now in force. */
  // set / reset save to NOR on the piano: the first save after a power-on may wait for the stock flash
  async set(p: PowerPolicy, timeoutMs = 5000): Promise<PowerPolicyReply | null> {
    const d = await this.ng.call(NgOp.PowerPolicy, [0x01, ...pack7(encodePowerPolicy(p))], timeoutMs, (x) => x[0] === 0x01);
    return d ? decodePolicyReply(d) : null;
  }
  async reset(timeoutMs = 5000): Promise<PowerPolicyReply | null> {
    const d = await this.ng.call(NgOp.PowerPolicy, [0x02], timeoutMs, (x) => x[0] === 0x02);
    return d ? decodePolicyReply(d) : null;
  }
  async standby(sub: "query" | "enter" | "wake", timeoutMs = 700): Promise<StandbyReply | null> {
    const code = { query: 0, enter: 1, wake: 2 }[sub];
    const d = await this.ng.call(NgOp.Standby, [code], timeoutMs, (x) => x[0] === code);
    return d && d.length >= 3 ? { rc: d[1]!, state: StandbyState[d[2]!] ?? "on" } : null;
  }
  /** Remote off (19 03, Identify feat2 NG_FEAT2_REMOTE_OFF): where the auto power-off goes (standby or the real off).
   * `59 03 <rc> <state> <why>`. */
  async remoteOff(timeoutMs = 1500): Promise<RemoteOffReply | null> {
    const d = await this.ng.call(NgOp.Standby, [0x03], timeoutMs, (x) => x[0] === 0x03);
    return d && d.length >= 4 ? { rc: d[1]!, state: StandbyState[d[2]!] ?? "on", why: d[3]! } : null;
  }
  /** APO test shortcut (19 04 <s_hi7> <s_lo7>, slot payloads with feat2 NG_FEAT2_REMOTE_OFF from 2026-11-02): the auto
   * power-off expires in `secs` seconds (10..600) as if the menu time were almost over; 0 = the full menu time again.
   * RAM only. `59 04 <rc> <state> <s_hi7> <s_lo7> <setting> <flags>`; rc 7 = the menu APO is Off. Older payloads
   * stay silent (null). */
  async apoTest(secs: number, timeoutMs = 1500): Promise<ApoTestReply | null> {
    const d = await this.ng.call(NgOp.Standby, [0x04, (secs >> 7) & 0x7f, secs & 0x7f], timeoutMs, (x) => x[0] === 0x04);
    return d && d.length >= 7
      ? { rc: d[1]!, state: StandbyState[d[2]!] ?? "on", secs: (d[3]! << 7) | d[4]!, setting: d[5]!, held: !!(d[6]! & 1) }
      : null;
  }
  /** Op 0x1A: only the slot payload ota_power answers (null elsewhere, e.g. the simulator). */
  async diag(timeoutMs = 700): Promise<PowerDiag | null> {
    const d = await this.ng.call(NgOp.PowerDiag, [], timeoutMs);
    return d ? decodePowerDiag(unpack7(d)) : null;
  }
}
