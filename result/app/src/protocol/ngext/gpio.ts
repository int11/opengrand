// Op 0x17: the read-only GPIO diagnostic of the power-family slot payloads (ota_main from rebuild 11, 2026-11-15;
// REMOTE_PROTOCOL.md section 10.11). A snapshot of the GPIO bank
// pairs 23 / 45 / 67 + five stock bytes + PINMUX0..19, a change log of ten named pins, and change notices to the bound
// controller that switched them on. The piano never writes a pin for any of these. Older payloads stay silent (null).
import type { NgClient } from "./client.ts";
import { u16, u32 } from "./bytes.ts";
import { NgOp, parseNgReply, unpack7 } from "./wire.ts";

export type GpioPair = 23 | 45 | 67;
export type GpioDir = "in" | "out";

/** A pin named in the project notes. `role` is the
 * English key of its meaning (translated by the UI); `open` = the record still marks its meaning unverified. */
export interface GpioPin {
  name: string; // "GP3[8]"
  pair: GpioPair;
  bit: number; // bit in the pair's 32-bit word (odd bank = high half)
  role: string;
  open: boolean;
}

const pin = (bank: number, n: number, role: string, open = false): GpioPin => {
  const pair = (bank <= 3 ? 23 : bank <= 5 ? 45 : 67) as GpioPair;
  return { name: `GP${bank}[${n}]`, pair, bit: (bank % 2) * 16 + n, role, open };
};

/** The named pins, in the order the view lists them (the open questions first). */
export const GPIO_PINS: readonly GpioPin[] = [
  pin(3, 8, "Mute request input", true),
  pin(6, 15, "Speaker path (amp)", true),
  pin(6, 11, "Output mute release", true),
  pin(5, 14, "Headphone detect"),
  pin(6, 12, "Codec path mark (chip A missing)", true),
  pin(6, 13, "Codec line (chip A side)", true),
  pin(7, 2, "Codec line guard", true),
  pin(7, 12, "Codec line (chip B side)", true),
  pin(3, 10, "Panel power candidate", true),
  pin(2, 14, "Panel reset candidate", true),
  pin(4, 15, "USB memory port power"),
  pin(4, 13, "Bluetooth MIDI power"),
  pin(5, 15, "Bluetooth audio module"),
  pin(3, 1, "Power switch"),
  pin(3, 2, "Power hold"),
  pin(3, 6, "Power hold"),
  pin(2, 10, "NOR ready (RY/BY#)"),
];

/** Change-log / notice signal index -> pin name (REMOTE_PROTOCOL 10.11). */
export const GPIO_SIGNALS = ["GP3[8]", "GP3[1]", "GP5[14]", "GP6[15]", "GP6[11]", "GP6[13]", "GP7[12]", "GP4[15]", "GP5[15]", "GP4[13]"] as const;

export interface GpioSnapshot {
  layout: number;
  notify: boolean;
  started: boolean;
  noticePort: number | null;
  seq: number;
  dir: Record<GpioPair, number>;
  out: Record<GpioPair, number>;
  in: Record<GpioPair, number>;
  headphones: number; // 0x4139E7FA
  gp3Level: number; // 0x4139E7FF: the stock 20 ms tick's GP3[8] sample
  gp3Event: number; // 0x4139E7FD
  model: number; // 0x4139E80A
  chipA: number; // 0x4139E80B: 0xDD / 0xDC
  pinmux: number[]; // PINMUX0..19
  ms: number;
}

export interface GpioEvent {
  seq: number;
  ms: number;
  signal: number;
  pin: string;
  level: number;
}

export interface GpioPinState extends GpioPin {
  dir: GpioDir;
  out: number;
  in: number;
  /** what the pin is now: the input level for an input, the driven level for an output */
  level: number;
}

export function decodeGpioSnapshot(raw: Uint8Array): GpioSnapshot | null {
  if (raw.length < 130 || raw[0] !== 1) return null;
  const word = (o: number) => ({ 23: u32(raw, o), 45: u32(raw, o + 12), 67: u32(raw, o + 24) }) as Record<GpioPair, number>;
  return {
    layout: raw[0]!, notify: !!(raw[1]! & 1), started: !!(raw[1]! & 2), noticePort: raw[2]! === 0x7f ? null : raw[2]!,
    seq: u16(raw, 3), dir: word(5), out: word(9), in: word(13),
    headphones: raw[41]!, gp3Level: raw[42]!, gp3Event: raw[43]!, model: raw[44]!, chipA: raw[45]!,
    pinmux: Array.from({ length: 20 }, (_, i) => u32(raw, 46 + 4 * i)), ms: u32(raw, 126),
  };
}

function decodeEvents(b: Uint8Array, at: number, n: number): GpioEvent[] {
  const out: GpioEvent[] = [];
  for (let i = 0; i < n && at + 8 * i + 8 <= b.length; i++) {
    const o = at + 8 * i, signal = b[o + 6]!;
    out.push({ seq: u16(b, o), ms: u32(b, o + 2), signal, pin: GPIO_SIGNALS[signal] ?? `#${signal}`, level: b[o + 7]! });
  }
  return out;
}

/** `57 01` raw: layout 1, n, seq u16, n events (oldest first). */
export function decodeGpioLog(raw: Uint8Array): { seq: number; events: GpioEvent[] } | null {
  if (raw.length < 4 || raw[0] !== 1) return null;
  return { seq: u16(raw, 2), events: decodeEvents(raw, 4, raw[1]!) };
}

/** A change notice `F0 7D 43 41 37 39 57 7F <pack7: n, seq u16, events>` (any other frame: null). */
export function parseGpioNotice(msg: Uint8Array): { seq: number; events: GpioEvent[] } | null {
  const r = parseNgReply(msg);
  if (!r || r.op !== NgOp.Gpio || r.data[0] !== 0x7f) return null;
  const raw = unpack7(r.data.slice(1));
  if (raw.length < 3) return null;
  return { seq: u16(raw, 1), events: decodeEvents(raw, 3, raw[0]!) };
}

/** Every named pin with its direction (DIR bit 1 = input) and levels. */
export function gpioPins(s: GpioSnapshot): GpioPinState[] {
  return GPIO_PINS.map((p) => {
    const m = 2 ** p.bit;
    const bit = (w: number) => (Math.floor(w / m) % 2);
    const dir: GpioDir = bit(s.dir[p.pair]) ? "in" : "out";
    const out = bit(s.out[p.pair]), inn = bit(s.in[p.pair]);
    return { ...p, dir, out, in: inn, level: dir === "in" ? inn : out };
  });
}

export interface GpioNotifyReply {
  rc: number; // 0 ok, 1 bad argument, 2 not bound
  notify: boolean;
  port: number | null;
}

/** Op 0x17 over an NgClient. */
export class NgGpio {
  private readonly ng: NgClient;
  constructor(ng: NgClient) {
    this.ng = ng;
  }
  async snapshot(timeoutMs = 1000): Promise<GpioSnapshot | null> {
    const d = await this.ng.call(NgOp.Gpio, [0x00], timeoutMs, (x) => x[0] === 0x00);
    return d ? decodeGpioSnapshot(unpack7(d.slice(1))) : null;
  }
  async log(timeoutMs = 1000): Promise<{ seq: number; events: GpioEvent[] } | null> {
    const d = await this.ng.call(NgOp.Gpio, [0x01], timeoutMs, (x) => x[0] === 0x01);
    return d ? decodeGpioLog(unpack7(d.slice(1))) : null;
  }
  async notify(on: boolean, timeoutMs = 1000): Promise<GpioNotifyReply | null> {
    const d = await this.ng.call(NgOp.Gpio, [0x02, on ? 1 : 0], timeoutMs, (x) => x[0] === 0x02);
    return d && d.length >= 4 ? { rc: d[1]!, notify: !!(d[2]! & 1), port: d[3]! === 0x7f ? null : d[3]! } : null;
  }
}
