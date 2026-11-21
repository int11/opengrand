// Op 0x20: the RAM canary of ota_main (ngver 4, REMOTE_PROTOCOL.md section 10.13). After an arm from a bound controller
// the piano fills the chosen unreferenced SDRAM blocks with addr ^ 0xC79A5A5C, a bounded step per engine-loop pass,
// then keeps re-reading them and counts the words that changed. Reads (status, region record) answer any port; disarm
// stops it and keeps the records. Older payloads stay silent (null).
import type { NgClient } from "./client.ts";
import { u16, u32 } from "./bytes.ts";
import { NgOp, unpack7 } from "./wire.ts";

/** The eight blocks in the piano's table (bit i of the mask = region i), the 4 KB guards already taken off. */
export const RAM_CANARY_REGIONS = [
  { name: "T1-A", lo: 0x40601000, hi: 0x409ff000 },
  { name: "T1-B", lo: 0x41944000, hi: 0x41cff000 },
  { name: "T1-C", lo: 0x403c1000, hi: 0x403ff000 },
  { name: "T1-D", lo: 0x40002000, hi: 0x4005f000 },
  { name: "T1-E", lo: 0x400b5000, hi: 0x400bf000 },
  { name: "T2-a", lo: 0x40c01000, hi: 0x40dff000 },
  { name: "T2-b", lo: 0x41d01000, hi: 0x41ef7000 },
  { name: "T2-c", lo: 0x41eff000, hi: 0x41fff000 },
] as const;
/** T1-A + T1-B: the two blocks layout L1 needs. */
export const RAM_CANARY_DEFAULT_MASK = 0x03;

export type RamCanaryPhase = "off" | "filling" | "verifying";
const PHASE: RamCanaryPhase[] = ["off", "filling", "verifying"];

export interface RamCanaryStatus {
  regions: number;
  phase: RamCanaryPhase;
  armed: number; // mask
  filled: number;
  dirty: number; // regions where a changed word was ever seen since the arm
  budget: number; // words per pass
  passes: number;
  filledWords: number;
  verifiedWords: number;
  lastUs: number;
  maxUs: number;
}

export interface RamCanaryRegion {
  index: number;
  state: RamCanaryPhase;
  armed: boolean;
  lo: number;
  hi: number;
  cursor: number; // words
  sweeps: number; // completed verify sweeps
  changed: number; // changed words in the last sweep
  changedMax: number;
  minAddr: number;
  maxAddr: number;
  firstAddr: number;
  firstValue: number;
  firstSweep: number; // 0 = no change seen
}

export interface RamCanaryReply {
  rc: number; // 0 ok, 1 bad argument, 2 not bound
  armed: number;
}

export function decodeRamCanaryStatus(raw: Uint8Array): RamCanaryStatus | null {
  if (raw.length < 32 || raw[0] !== 1) return null;
  return {
    regions: raw[1]!, phase: PHASE[raw[2]!] ?? "off", armed: u16(raw, 4), filled: u16(raw, 6), dirty: u16(raw, 8),
    budget: u16(raw, 10), passes: u32(raw, 12), filledWords: u32(raw, 16), verifiedWords: u32(raw, 20),
    lastUs: u32(raw, 24), maxUs: u32(raw, 28),
  };
}

export function decodeRamCanaryRegion(index: number, raw: Uint8Array): RamCanaryRegion | null {
  if (raw.length < 48) return null;
  return {
    index, state: PHASE[raw[0]!] ?? "off", armed: raw[1] === 1, lo: u32(raw, 4), hi: u32(raw, 8), cursor: u32(raw, 12),
    sweeps: u32(raw, 16), changed: u32(raw, 20), changedMax: u32(raw, 24), minAddr: u32(raw, 28), maxAddr: u32(raw, 32),
    firstAddr: u32(raw, 36), firstValue: u32(raw, 40), firstSweep: u32(raw, 44),
  };
}

/** Op 0x20 over an NgClient. */
export class NgRamCanary {
  private readonly ng: NgClient;
  constructor(ng: NgClient) {
    this.ng = ng;
  }
  async status(timeoutMs = 1000): Promise<RamCanaryStatus | null> {
    const d = await this.ng.call(NgOp.RamCanary, [0x00], timeoutMs, (x) => x[0] === 0x00);
    return d ? decodeRamCanaryStatus(unpack7(d.slice(1))) : null;
  }
  async region(i: number, timeoutMs = 1000): Promise<RamCanaryRegion | null> {
    const d = await this.ng.call(NgOp.RamCanary, [0x02, i], timeoutMs, (x) => x[0] === 0x02 && x[1] === i);
    return d ? decodeRamCanaryRegion(i, unpack7(d.slice(2))) : null;
  }
  /** every region of the table, in order (null where the piano did not answer) */
  async regions(n: number = RAM_CANARY_REGIONS.length, timeoutMs = 1000): Promise<(RamCanaryRegion | null)[]> {
    const out: (RamCanaryRegion | null)[] = [];
    for (let i = 0; i < n; i++) out.push(await this.region(i, timeoutMs));
    return out;
  }
  async arm(mask: number, timeoutMs = 1000): Promise<RamCanaryReply | null> {
    const d = await this.ng.call(NgOp.RamCanary, [0x01, mask & 0x7f, (mask >> 7) & 0x7f], timeoutMs, (x) => x[0] === 0x01);
    return d && d.length >= 4 ? { rc: d[1]!, armed: d[2]! | (d[3]! << 7) } : null;
  }
  async disarm(timeoutMs = 1000): Promise<RamCanaryReply | null> {
    const d = await this.ng.call(NgOp.RamCanary, [0x03], timeoutMs, (x) => x[0] === 0x03);
    return d && d.length >= 4 ? { rc: d[1]!, armed: d[2]! | (d[3]! << 7) } : null;
  }
}
