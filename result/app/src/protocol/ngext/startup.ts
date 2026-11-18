// Startup settings owned by the piano (0x7D op 0x1C, REMOTE_PROTOCOL.md section 10.8; firmware
// result/firmware/ports/cave/startup_cave.c, core src/kproto/kpstart.c). The piano keeps a mode and its snapshots
// itself; every device only reads and writes the piano's values (owner decision 2026-11-01).
import { u16 } from "./bytes.ts";
import type { NgClient } from "./client.ts";
import { NgOp, unpack7 } from "./wire.ts";

/** stock = the stock UserMemory start for the sound settings, the factory values for the rest (Bluetooth devices,
 * reconnect, power policy, auto power off, lock), current = the snapshot taken with "save current settings as startup",
 * poweroff = the settings at the last power-off (saved 3 s after every change).  Since record 14 §7 a mode covers every
 * piano setting except Bluetooth pairing and the Bluetooth module's own settings. */
export const StartupModes = ["stock", "current", "poweroff"] as const;
export type StartupMode = (typeof StartupModes)[number];

export const StartupResult = { Ok: 0, BadValue: 1, NotBound: 2, Busy: 3, SaveFailed: 4 } as const;

export interface StartupStatus {
  mode: StartupMode;
  loaded: boolean;
  hasCurrent: boolean;
  hasPowerOff: boolean;
  /** the boot restore is over (or there was nothing to restore) */
  ready: boolean;
  restoring: boolean;
  savePending: boolean;
  dirty: boolean;
  /** snapshot generations (saves since the store was made) */
  currentGen: number;
  powerOffGen: number;
  /** this power-on: records in the snapshot, sets applied, differences left */
  records: number;
  applied: number;
  left: number;
  saves: number;
  saveFailures: number;
  /** restored at this power-on (not after a live payload swap) */
  restoredAtBoot: boolean;
  /** record 14 §7 (status bytes 36..38; null from a firmware before): the mode covers every setting (chosen by a
   * firmware of §7), snapshot C / P carries our own settings, what they started from this power-on (0 kept as they
   * were, 1 factory, 2 the snapshot; null not decided) */
  allSettings: boolean | null;
  currentHasAll: boolean;
  powerOffHasAll: boolean;
  ownStart: number | null;
}

export function decodeStartupStatus(raw: Uint8Array): StartupStatus | null {
  if (raw.length < 32 || raw[0] !== 1) return null;
  const u32 = (o: number) => (raw[o]! | (raw[o + 1]! << 8) | (raw[o + 2]! << 16) | (raw[o + 3]! << 24)) >>> 0;
  const f = raw[2]!;
  return {
    mode: StartupModes[raw[1]!] ?? "stock",
    loaded: !!(f & 0x01), hasCurrent: !!(f & 0x02), hasPowerOff: !!(f & 0x04), ready: !!(f & 0x08),
    restoring: !!(f & 0x10), savePending: !!(f & 0x20), dirty: !!(f & 0x40),
    currentGen: u32(8), powerOffGen: u32(12), records: u16(raw, 16), applied: u16(raw, 18), left: u16(raw, 20),
    saves: u16(raw, 24), saveFailures: u16(raw, 26), restoredAtBoot: !!(raw[31]! & 0x20),
    allSettings: raw.length >= 40 ? !!(raw[36]! & 0x01) : null,
    currentHasAll: raw.length >= 40 && !!(raw[36]! & 0x02), powerOffHasAll: raw.length >= 40 && !!(raw[36]! & 0x04),
    ownStart: raw.length >= 40 && raw[37]! !== 0xff ? raw[37]! : null,
  };
}

export interface StartupReply {
  rc: number;
  status: StartupStatus | null;
}

/** Op 0x1C over an NgClient. Status answers on any port; mode / save / clear only from the bound controller. */
export class NgStartup {
  private readonly ng: NgClient;
  constructor(ng: NgClient) {
    this.ng = ng;
  }
  private async op(sub: number, args: number[], timeoutMs: number): Promise<StartupReply | null> {
    const d = await this.ng.call(NgOp.Startup, [sub, ...args], timeoutMs, (x) => x[0] === sub);
    return d && d.length >= 2 ? { rc: d[1]!, status: decodeStartupStatus(unpack7(d.slice(2))) } : null;
  }
  async status(timeoutMs = 700): Promise<StartupStatus | null> {
    return (await this.op(0x00, [], timeoutMs))?.status ?? null;
  }
  /** The piano writes the mode to its NOR (after a stock flash job, if one runs: up to a few seconds). */
  setMode(mode: StartupMode, timeoutMs = 6000): Promise<StartupReply | null> {
    return this.op(0x01, [StartupModes.indexOf(mode)], timeoutMs);
  }
  /** Snapshot the piano's settings now as the startup settings (the mode becomes "current"). */
  saveCurrent(timeoutMs = 6000): Promise<StartupReply | null> {
    return this.op(0x02, [], timeoutMs);
  }
  /** Forget both snapshots: the next power-on is the stock's (factory values for everything but the sound). */
  clear(timeoutMs = 6000): Promise<StartupReply | null> {
    return this.op(0x03, [], timeoutMs);
  }
}
