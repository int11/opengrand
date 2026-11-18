// Kawai remote SysEx framing (wire-compatible with the original piano/app).
//
// Observed on both sides:
//   F0 40 <dev> <fn> <grp> <mach> <v1> <v2> [<part>] <data...> F7
//   - controller -> piano: grp/mach = 08 02 ("Application / Kawaipiano.js")
//   - piano -> controller: grp/mach = the piano model, CA79 = 04 1E
//   - dev = 0x7F (broadcast) from the controller.

export const KAWAI_ID = 0x40;
export const APP_GROUP = 0x08;
export const APP_MACHINE = 0x02;
export const DEV_BROADCAST = 0x7f;

export const Fn = {
  Request: 0x02,
  EndOfData: 0x03,
  Cancel: 0x05,
  Nack: 0x06,
  Ack: 0x07,
  SaveStart: 0x09,
  Param: 0x10,
  FileDump: 0x24,
  Mode: 0x31,
  Remote: 0x32,
  Identity: 0x60,
  System: 0x7e,
} as const;
export type FnCode = (typeof Fn)[keyof typeof Fn];

export interface KawaiFrame {
  dev: number;
  fn: number;
  group: number; // sender model group
  machine: number; // sender model machine
  body: number[]; // bytes after group/machine, before F7 (v1, v2, part, data...)
}

export function isKawaiSysex(msg: ArrayLike<number>): boolean {
  return msg.length >= 7 && msg[0] === 0xf0 && msg[1] === KAWAI_ID && msg[msg.length - 1] === 0xf7;
}

export function encodeFrame(f: KawaiFrame): Uint8Array {
  const out = new Uint8Array(7 + f.body.length);
  out[0] = 0xf0;
  out[1] = KAWAI_ID;
  out[2] = f.dev & 0x7f;
  out[3] = f.fn & 0x7f;
  out[4] = f.group & 0x7f;
  out[5] = f.machine & 0x7f;
  f.body.forEach((b, i) => {
    if (b < 0 || b > 0x7f || !Number.isInteger(b)) throw new RangeError(`SysEx data byte out of range: ${b}`);
    out[6 + i] = b;
  });
  out[out.length - 1] = 0xf7;
  return out;
}

export function decodeFrame(msg: ArrayLike<number>): KawaiFrame | null {
  if (!isKawaiSysex(msg)) return null;
  const body: number[] = [];
  for (let i = 6; i < msg.length - 1; i++) body.push(msg[i]);
  return { dev: msg[2], fn: msg[3], group: msg[4], machine: msg[5], body };
}

/** Build a controller->piano frame (group/machine = 08 02). */
export function appFrame(fn: number, body: number[], dev = DEV_BROADCAST): Uint8Array {
  return encodeFrame({ dev, fn, group: APP_GROUP, machine: APP_MACHINE, body });
}

/** The ACK the piano returns echoes bytes 6..8 (v1 v2 part) of the request. */
export function ackKey(msg: ArrayLike<number>): string {
  return [msg[6], msg[7], msg[8]].map((b) => (b ?? 0).toString(16).padStart(2, "0")).join("");
}

export function toHex(msg: ArrayLike<number>): string {
  return Array.from(msg, (b) => b.toString(16).padStart(2, "0").toUpperCase()).join(" ");
}

export function fromHex(hex: string): Uint8Array {
  const parts = hex.trim().split(/\s+/).filter(Boolean);
  return Uint8Array.from(parts.map((p) => parseInt(p, 16)));
}
