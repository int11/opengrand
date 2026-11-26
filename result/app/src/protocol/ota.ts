// OpenGrand remote cave update over Bluetooth (REMOTE_PROTOCOL.md section 10.3).
//
// The piano runs image ota_loader (USB-flashed once). Its resident stage keeps two cave
// slots in NOR; this client uploads a signed payload file (build/ota_*.c79 from `make image`)
// into the slot that is not running, commits it, and can reject (roll back) a slot.
// begin / commit / reject carry an 8-byte HMAC-SHA256 tag over the piano's current challenge,
// keyed with the per-device key (result/stockfw/secrets/ota_key.bin) that the owner imports
// into the app once. Data chunks are 200 raw bytes (240-byte SysEx frames, below the 256 B
// the BLE path carries) sent strictly stop-and-wait: the next chunk only after the ACK - unless the loader offers the
// fast upload in its Begin reply (stage-1 image version 2): then v2 frames with a CRC each, up to 1536 B, LZ tokens,
// two in flight (OTA_D2, sendWindowed).
// ota_loader_v2 adds "apply now" (op 0x15): switch the running cave to a verified slot or the
// built-in without a power cycle; its status reply (layout 2) also carries the apply state and
// boot diagnostics (escape-key mailbox words). The first ota_loader answers layout 1 (48 bytes).
// ota_loader_v4 (section 10.12): a frozen stage-0 boots one of two loader slots (stage-1, which answers every op
// here as the v3 resident did) and stage-1 boots one of two main slots. Status layout 3 (112 bytes) appends the
// loader slots and stage-0's boot record; op 0x0E reads one slot's header, op 0x0F confirms the running loader; the
// target byte of Begin / Commit / info / confirm names a loader slot as 0x10 / 0x11, reject mask bits 4-5. A .c79
// says what it is (format 2: type byte 0x28 = 1 main / 2 loader; format 1 = main).
import { crc32, NG_ID, NG_TAG, pack7, parseNgReply, unpack7 } from "./ngext.ts";
import type { PianoSession } from "./session.ts";
import { withXferLock, xferBusy } from "./xferlock.ts";

export const OtaOp = { Info: 0x0e, Confirm: 0x0f, Status: 0x10, Begin: 0x11, Data: 0x12, Commit: 0x13, Reject: 0x14, Apply: 0x15 } as const;
/** Apply-now targets (cave/include/ota.h OTA_TGT_*). 2 is "built-in" up to ota_loader_v3 and "no main" on v4;
 *  4 (v4, status caps2 bit 4) drops the main and lets stage-0 pick a loader again (RESELECT). */
export const ApplyTarget = { A: 0, B: 1, BuiltIn: 2, NoMain: 2, Newest: 3, Reselect: 4 } as const;
/** Target byte of Begin / Commit / slot info / loader confirm op, v4 (0 / 1 = main A / B as before). */
export const OtaTarget = { MainA: 0x00, MainB: 0x01, LoaderA: 0x10, LoaderB: 0x11 } as const;
/** Reject mask bits (loader bits: v4 only). */
export const REJECT_BIT = { mainA: 0x01, mainB: 0x02, loaderA: 0x10, loaderB: 0x20 } as const;
/** status caps bits (layout 2) */
export const OTA_CAP = { applyNow: 0x01, bootDiag: 0x02, keyEscape: 0x04 } as const;
/** status caps2 bits (layout 3, byte 89) */
export const OTA_CAP2 = { loaderSlots: 0x01, loaderConfirm: 0x02, slotInfo: 0x04, norService: 0x08, reselect: 0x10 } as const;
/** stage-0 boot flags (layout 3, byte 86; cave/include/s0.h S0F_*) */
export const S0_FLAG = { safe: 0x01, noMark: 0x02, reselected: 0x04 } as const;
/** stage-0 boot reason (layout 3, byte 85; s0.h S0R_*) */
export const S0_REASON = ["selecting", "newest loader slot", "no bootable loader", "escape keys E1", "all loaders failed", "flash busy at boot", "SAFE mode (E2)", "loader reselected"] as const;
/** .c79 image types (header 0x28, format 2) */
export const IMAGE_TYPE = { main0: 0, main: 1, loader: 2 } as const;
/** stage-0 tries an unconfirmed loader this many times (s0.h S0_MAX_TRIES) */
export const LOADER_TRIES = 3;
/** largest loader file: 64 B header + the 40 KB stage-1 window (status 104..107) */
export const OTA_LOADER_MAX = 0xa040;
export const OTA_APPLY_STATE = ["idle", "reading slot", "verifying", "quiescing", "switching", "done", "error"] as const;
/** Raw payload bytes per data op - the one chunk-size constant (cave/include/ota.h OTA_CHUNK). */
export const OTA_CHUNK = 200;
/** Fast upload (stage-1 image version 2, cave/include/ota.h "fast upload", REMOTE_PROTOCOL 10.3 Data v2): Begin's reply
 *  grows to 9 raw bytes (rc, challenge, caps, window, max u16); v2 data frames carry a CRC-32 each, may be LZ tokens,
 *  and several may be in flight. */
export const OTA_D2 = { flag: 0x40, lz: 0x01, max: 1536, window: 4, capData2: 0x01, capLz: 0x02 } as const;
/** What this app sends when the loader offers v2 frames (the loader's own limits are the ceiling): 1024 B frames, two
 *  in flight - enough to keep the 31.25 kbaud UART busy across the BLE round trip (emulator model,
 *  tools/roundtrip_ota_window.py 7) while at most ~2.4 KB sit between the phone and the piano. */
export const OTA_FAST_DEFAULTS = { chunk: 1024, window: 2 } as const;
export const OTA_HDR_SIZE = 64;
export const OTA_IMAGE_MAX = 0x1fe00;
export const OTA_KEY_STORAGE = "ca79ng.otaKey";

export const OTA_ERRORS: Record<number, string> = {
  0: "ok", 1: "bad arguments", 2: "authentication failed (wrong key?)", 3: "busy", 4: "slot not allowed",
  5: "bad length", 6: "sequence not newer", 7: "out of order", 8: "not receiving", 9: "incomplete",
  10: "CRC mismatch", 11: "header invalid (other key / build?)", 12: "signature mismatch", 13: "flash busy too long",
  14: "erase failed", 15: "program failed", 16: "read-back mismatch", 17: "address guard", 18: "seal failed",
  19: "still booting", 20: "the running cave did not go idle", 21: "install failed - built-in running",
  22: "the running loader is not confirmed", 23: "the last confirmed loader must stay", 24: "SAFE mode: no main is started",
  25: "main not compatible with this loader (API / ABI)",
};
/** User texts (i18n source strings) for the refusals of ota_loader_v4 - the app refuses the same cases itself
 *  before it sends anything (rc 22, 23, 24, 25) and shows the same text when the piano refuses. */
export const OTA_RC_HELP: Record<number, string> = {
  22: "The running loader is not confirmed yet. Confirm it first, then update or reject the other loader.",
  23: "Refused: one confirmed loader must stay to fall back to.",
  24: "SAFE mode (switched on with A0 + A#0 held): no main is started. Uploads still work; switch off and on normally to run the main.",
  25: "This main is not compatible with the piano's loader (API / ABI). Update the loader first.",
};
/** Refusals only the app makes (no rc): code -> user text (i18n source strings). */
export const OTA_LOCAL_HELP = {
  needV4: "This file needs OTA loader v4 (install opengrand_install.SYS by USB).",
  noLoaderOta: "This piano's loader cannot update itself over Bluetooth (needs OTA loader v4).",
  unknownType: "Unknown firmware file type.",
  signature: "Unknown signature type in this file.",
  tooLarge: "The file is too large for the slot.",
  loaderKey: "The loader file is signed with another key than the piano's.",
} as const;
export type OtaLocalRefusal = keyof typeof OTA_LOCAL_HELP;
export const OTA_JOB = ["idle", "receiving", "queued", "verifying", "erasing", "programming", "reading back", "sealing", "done", "error"] as const;
export const OTA_ACTIVE: Record<number, string> = { 0: "slot A", 1: "slot B", 2: "built-in", 0x7f: "selecting" };
export const OTA_REASON = ["selecting", "newest valid slot", "no valid slot", "escape keys held", "slot failed verification", "flash busy at boot", "applied live", "SAFE mode (no main)", "no compatible main"] as const;
/** Slot flags; the loader slots use the same bits (booted = running now), liveOk only on mains. */
export const SLOT_FLAG = { valid: 0x01, confirmed: 0x02, rejected: 0x04, booted: 0x08, bad: 0x10, exhausted: 0x20, scanned: 0x40, liveOk: 0x80 } as const;

/** What runs as the main: value 2 is "built-in" up to ota_loader_v3 and "no main" on v4 (layout 3). */
export function activeName(st: Pick<OtaStatus, "active" | "layout">): string {
  if (st.active === 2 && st.layout >= 3) return "no main";
  return OTA_ACTIVE[st.active] ?? String(st.active);
}

export interface OtaSlot { seq: number; flags: number; tries: number }
/** Layout 3 (ota_loader_v4 stage-1): the loader slots and stage-0's boot record. */
export interface OtaLoaderStatus {
  slots: [OtaSlot, OtaSlot];
  /** running loader slot: 0 = A, 1 = B */
  running: number;
  s0Reason: number; s0Flags: number; s0Version: number;
  /** stage-1 API (a format-2 main needs header 0x2A <= this) */
  api: number;
  caps2: number;
  /** image version of the running stage-1 (its header 0x2C) */
  version: number;
  /** key id stage-0 checks loader files with / key id stage-1 uses for mains and the auth tags */
  rootKeyId: number; workKeyId: number;
  mainAbiMin: number; mainAbiMax: number;
  imageMax: number; maxSeq16: number; reselects: number;
  safe: boolean;
}
export interface OtaStatus {
  job: number; err: number; booted: boolean; active: number; reason: number; jobSlot: number; rejectPending: number;
  received: number; total: number; challenge: number; keyId: number; slots: [OtaSlot, OtaSlot];
  guardTrips: number; busyDefers: number; imageMax: number; abi: number; chunk: number;
  /** status layout: 1 = ota_loader (48 B), 2 = ota_loader_v2 (72 B, the fields below) */
  layout: number;
  caps: number; applyState: number; applyErr: number; applyTarget: number; live: boolean; applyCount: number;
  /** boot diagnostics latched at the first engine-loop pass: BootProgram mailbox words, *0x40000000,
   *  escape source bits (1 mailbox, 2 copy, 4 key state, 0x40 key scan running), keys down, A0..F#1 mask */
  diag: { mbox0: number; mbox1: number; copy: number; esc: number; keysDown: number; keysLow: number; escWait: number } | null;
  /** layout 3 only (ota_loader_v4), else null: then every loader feature stays hidden */
  loader: OtaLoaderStatus | null;
}

const u16 = (b: Uint8Array, o: number) => b[o]! | (b[o + 1]! << 8);
const u32 = (b: Uint8Array, o: number) => (b[o]! | (b[o + 1]! << 8) | (b[o + 2]! << 16) | (b[o + 3]! << 24)) >>> 0;
const le32 = (v: number) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];

export function decodeOtaStatus(data: Uint8Array): OtaStatus | null {
  const r = unpack7(data);
  if (r.length < 48) return null;
  const v2 = r[0]! >= 2 && r.length >= 70;
  const slot = (i: number): OtaSlot => ({ seq: u32(r, 24 + 6 * i), flags: r[28 + 6 * i]!, tries: r[29 + 6 * i]! });
  return {
    job: r[1]!, err: r[2]!, booted: !!r[3], active: r[4]!, reason: r[5]!, jobSlot: r[6]!, rejectPending: r[7]!,
    received: u32(r, 8), total: u32(r, 12), challenge: u32(r, 16), keyId: u32(r, 20), slots: [slot(0), slot(1)],
    guardTrips: u16(r, 36), busyDefers: u16(r, 38), imageMax: u32(r, 40), abi: r[44]!, chunk: r[45]!,
    layout: r[0]!, caps: v2 ? r[48]! : 0, applyState: v2 ? r[49]! : 0, applyErr: v2 ? r[50]! : 0,
    applyTarget: v2 ? r[51]! : 0, live: v2 ? !!r[52] : false, applyCount: v2 ? r[53]! : 0,
    diag: v2 ? { mbox0: u32(r, 54), mbox1: u32(r, 58), copy: u32(r, 62), esc: r[66]!, keysDown: r[67]!, keysLow: r[68]!, escWait: r[69]! } : null,
    loader: r[0]! >= 3 && r.length >= 112 ? decodeLoader(r) : null,
  };
}

/** Bytes 72..111 of layout 3 (later bytes are ignored: append-only). */
function decodeLoader(r: Uint8Array): OtaLoaderStatus {
  const slot = (i: number): OtaSlot => ({ seq: u32(r, 72 + 6 * i), flags: r[76 + 6 * i]!, tries: r[77 + 6 * i]! });
  return {
    slots: [slot(0), slot(1)], running: r[84]!, s0Reason: r[85]!, s0Flags: r[86]!, s0Version: r[87]!, api: r[88]!,
    caps2: r[89]!, version: u32(r, 90), rootKeyId: u32(r, 94), workKeyId: u32(r, 98), mainAbiMin: r[102]!,
    mainAbiMax: r[103]!, imageMax: u32(r, 104), maxSeq16: u16(r, 108), reselects: r[110]!, safe: !!(r[86]! & S0_FLAG.safe),
  };
}

/** op 0x0E reply (56 raw bytes): one slot as stage-1 read it, with the first 48 header bytes of its file. */
export interface OtaSlotInfo {
  target: number; flags: number; seq: number; tries: number; stateVersion: number;
  /** null when the slot holds no file (header bytes zero) */
  image: { format: number; type: number; payload: number; keyId: number; version: number; minApi: number; abi: number } | null;
}

export function decodeSlotInfo(data: Uint8Array): OtaSlotInfo | null {
  const r = unpack7(data);
  if (r.length < 56) return null;
  const h = r.slice(8, 56);
  const format = u16(h, 4);
  const image = String.fromCharCode(...h.slice(0, 4)) !== "C79P" ? null : {
    format, type: format >= 2 ? h[0x28]! : IMAGE_TYPE.main0, payload: u32(h, 8), keyId: u32(h, 0x24),
    version: format >= 2 ? u32(h, 0x2c) : 0, minApi: format >= 2 ? h[0x2a]! : 0, abi: u32(h, 0x1c),
  };
  return { target: r[0]!, flags: r[1]!, seq: u32(r, 2), tries: r[6]!, stateVersion: r[7]!, image };
}

// ---- SHA-256 / HMAC (sync, no Web Crypto: works in insecure contexts and the Android shell) ----
const K = Uint32Array.from([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
  0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
  0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

export function sha256(data: Uint8Array): Uint8Array {
  const n = data.length;
  const total = Math.ceil((n + 9) / 64) * 64;
  const m = new Uint8Array(total);
  m.set(data);
  m[n] = 0x80;
  const dv = new DataView(m.buffer);
  dv.setUint32(total - 8, Math.floor(n / 0x20000000));
  dv.setUint32(total - 4, (n << 3) >>> 0);
  const h = Uint32Array.from([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  const w = new Uint32Array(64);
  const ror = (x: number, k: number) => (x >>> k) | (x << (32 - k));
  for (let o = 0; o < total; o += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(o + 4 * i);
    for (let i = 16; i < 64; i++) {
      const a = w[i - 15]!, b = w[i - 2]!;
      w[i] = (w[i - 16]! + (ror(a, 7) ^ ror(a, 18) ^ (a >>> 3)) + w[i - 7]! + (ror(b, 17) ^ ror(b, 19) ^ (b >>> 10))) >>> 0;
    }
    let [a, b, c, d, e, f, g, k] = h as unknown as number[];
    for (let i = 0; i < 64; i++) {
      const t1 = (k! + (ror(e!, 6) ^ ror(e!, 11) ^ ror(e!, 25)) + ((e! & f!) ^ (~e! & g!)) + K[i]! + w[i]!) >>> 0;
      const t2 = ((ror(a!, 2) ^ ror(a!, 13) ^ ror(a!, 22)) + ((a! & b!) ^ (a! & c!) ^ (b! & c!))) >>> 0;
      k = g; g = f; f = e; e = (d! + t1) >>> 0; d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    [a, b, c, d, e, f, g, k].forEach((v, i) => (h[i] = (h[i]! + v!) >>> 0));
  }
  const out = new Uint8Array(32);
  const ov = new DataView(out.buffer);
  h.forEach((v, i) => ov.setUint32(4 * i, v));
  return out;
}

export function hmacSha256(key: Uint8Array, msg: Uint8Array): Uint8Array {
  const k = new Uint8Array(64);
  k.set(key.length > 64 ? sha256(key) : key);
  const inner = new Uint8Array(64 + msg.length);
  const outer = new Uint8Array(64 + 32);
  for (let i = 0; i < 64; i++) {
    inner[i] = k[i]! ^ 0x36;
    outer[i] = k[i]! ^ 0x5c;
  }
  inner.set(msg, 64);
  outer.set(sha256(inner), 64);
  return sha256(outer);
}

const ascii = (s: string) => Array.from(s, (c) => c.charCodeAt(0));

export function keyId(key: Uint8Array): number {
  return u32(hmacSha256(key, Uint8Array.from(ascii("C79 key id"))), 0);
}

/** Tag of begin / commit / reject: HMAC(key, "C79A" | op | challenge LE | args)[:8]. */
export function authTag(key: Uint8Array, op: number, challenge: number, args: number[]): number[] {
  return Array.from(hmacSha256(key, Uint8Array.from([...ascii("C79A"), op, ...le32(challenge), ...args])).slice(0, 8));
}

export function parseKey(text: string | Uint8Array): Uint8Array | null {
  if (text instanceof Uint8Array) return text.length === 32 ? text : null;
  const hex = text.replace(/[\s:]/g, "");
  return /^[0-9a-fA-F]{64}$/.test(hex) ? Uint8Array.from(hex.match(/../g)!, (b) => parseInt(b, 16)) : null;
}

export interface OtaFileInfo {
  length: number; payload: number; feat0: number; feat1: number; keyId: number;
  /** header format: 1 = main of the v3 era (0x28..0x2F zero), 2 = v4 */
  format: number;
  /** 0x28 (format 2): IMAGE_TYPE; format 1 reads 0 (a main) */
  type: number;
  /** "main" | "loader", null for a type this app does not know */
  kind: "main" | "loader" | null;
  /** 0x29 signature algorithm (1 = HMAC; format 1 reads 1), 0x2A lowest stage-1 API a main needs, 0x2C image version */
  alg: number; minApi: number; version: number;
  abi: number;
}

/** Header of a .c79 payload file (tools/ota_pack.py); null if it is not one. */
export function parseOtaFile(file: Uint8Array): OtaFileInfo | null {
  if (file.length < OTA_HDR_SIZE + 24 || file.length > OTA_IMAGE_MAX) return null;
  const format = u16(file, 4);
  if (String.fromCharCode(...file.slice(0, 4)) !== "C79P" || format < 1 || u16(file, 6) !== OTA_HDR_SIZE) return null;
  if (u32(file, 8) !== file.length - OTA_HDR_SIZE) return null;
  const feat = u32(file, 0x20);
  const v4 = format >= 2;
  const type = v4 ? file[0x28]! : IMAGE_TYPE.main0;
  const kind = !v4 || type === IMAGE_TYPE.main ? "main" : type === IMAGE_TYPE.loader ? "loader" : null;
  return {
    length: file.length, payload: u32(file, 8), feat0: feat & 0xff, feat1: (feat >> 8) & 0xff, keyId: u32(file, 0x24),
    format, type, kind, alg: v4 ? file[0x29]! : 1, minApi: v4 ? file[0x2a]! : 0, version: v4 ? u32(file, 0x2c) : 0,
    abi: u32(file, 0x1c),
  };
}

/** Where an upload of `info` goes on the piano in `st`, or why the piano would refuse it (the firmware's own
 *  interlocks, checked before anything is sent). Mains: as before (the slot not running, else the empty / older one;
 *  seq = newest main + 1). Loaders (v4 only): the loader slot not running, seq = newest loader + 1. */
export type UploadPlan =
  | { ok: true; kind: "main" | "loader"; slot: number; target: number; seq: number }
  | { ok: false; rc?: number; refusal?: OtaLocalRefusal; message: string };

export function planUpload(info: OtaFileInfo, st: OtaStatus): UploadPlan {
  const no = (message: string, more: { rc?: number; refusal?: OtaLocalRefusal } = {}): UploadPlan => ({ ok: false, message, ...more });
  const L = st.loader;
  const newest = (slots: OtaSlot[]) => Math.max(0, ...slots.filter((s) => s.flags & SLOT_FLAG.valid).map((s) => s.seq));
  if (!info.kind) return no(`unknown image type ${info.type}`, { refusal: "unknownType" });
  if (info.alg !== 1) return no(`signature algorithm ${info.alg} is not supported`, { refusal: "signature" });
  if (info.kind === "loader") {
    if (!L || !(L.caps2 & OTA_CAP2.loaderSlots)) return no("this loader cannot take a loader file (needs ota_loader_v4)", { refusal: "noLoaderOta" });
    const max = L.imageMax || OTA_LOADER_MAX;
    if (info.length > max) return no(`the file is larger than a loader slot takes (${max} B)`, { refusal: "tooLarge" });
    if (info.keyId !== L.rootKeyId) return no("the loader file is signed with another key than the piano's loader key", { refusal: "loaderKey" });
    if (L.running > 1) return no(OTA_ERRORS[4]!, { rc: 4 });
    if (!(L.slots[L.running]!.flags & SLOT_FLAG.confirmed)) return no(OTA_ERRORS[22]!, { rc: 22 });
    const slot = 1 - L.running;
    return { ok: true, kind: "loader", slot, target: OtaTarget.LoaderA + slot, seq: newest(L.slots) + 1 };
  }
  if (info.format >= 2 && !L) return no("this file needs ota_loader_v4", { refusal: "needV4" });
  if (L ? info.abi < L.mainAbiMin || info.abi > L.mainAbiMax || info.minApi > L.api : info.abi !== st.abi)
    return no(OTA_ERRORS[25]!, { rc: 25 });
  if (st.imageMax && info.length > st.imageMax) return no(`the file is larger than a slot takes (${st.imageMax} B)`, { refusal: "tooLarge" });
  const slot = st.active === 0 ? 1 : st.active === 1 ? 0 : !(st.slots[0].flags & SLOT_FLAG.valid) ? 0
    : !(st.slots[1].flags & SLOT_FLAG.valid) ? 1 : st.slots[0].seq <= st.slots[1].seq ? 0 : 1;
  return { ok: true, kind: "main", slot, target: slot, seq: newest(st.slots) + 1 };
}

/** Reject mask that rolls back the running loader, or the rc the piano would refuse it with (23: a confirmed,
 *  sealed, not rejected loader must remain; 4: no loader slot running). */
export function loaderRollbackMask(st: OtaStatus): { ok: true; mask: number } | { ok: false; rc: number } {
  const L = st.loader;
  if (!L || L.running > 1) return { ok: false, rc: 4 };
  const other = L.slots[1 - L.running]!;
  const keeps = (other.flags & (SLOT_FLAG.valid | SLOT_FLAG.confirmed | SLOT_FLAG.rejected)) === (SLOT_FLAG.valid | SLOT_FLAG.confirmed);
  return keeps ? { ok: true, mask: L.running ? REJECT_BIT.loaderB : REJECT_BIT.loaderA } : { ok: false, rc: 23 };
}

/** Name of a target byte ("slot A", "loader slot B"). */
export function targetName(target: number): string {
  return target >= OtaTarget.LoaderA ? `loader slot ${target === OtaTarget.LoaderB ? "B" : "A"}` : OTA_ACTIVE[target] ?? String(target);
}

// ---- fast upload framing (REMOTE_PROTOCOL 10.3 Data v2; tools/ota_frames.py is the Python twin) ----
/** One v2 data frame: file bytes [off, end) carried as `data` (raw when flags = 0, LZ tokens when flags = OTA_D2.lz). */
export interface D2Frame { off: number; end: number; flags: number; data: Uint8Array }

/** The whole SysEx of a v2 data frame: 12 <0x40 | off[19:14]> <off[13:7]> <off[6:0]> <pack7: crc32(flags || data) · flags · data>. */
export function d2Message(f: D2Frame): Uint8Array {
  const body = new Uint8Array(1 + f.data.length);
  body[0] = f.flags;
  body.set(f.data, 1);
  const packed = pack7([...le32(crc32(body)), ...body]);
  return Uint8Array.from([0xf0, NG_ID, ...NG_TAG, OtaOp.Data, OTA_D2.flag | ((f.off >> 14) & 0x3f), (f.off >> 7) & 0x7f, f.off & 0x7f, ...packed, 0xf7]);
}

export function rawFrames(file: Uint8Array, size: number): D2Frame[] {
  const out: D2Frame[] = [];
  for (let o = 0; o < file.length; o += size) out.push({ off: o, end: Math.min(o + size, file.length), flags: 0, data: file.subarray(o, o + size) });
  return out;
}

const LZ_MIN = 4;
const lzExt = (n: number, out: number[]) => {
  while (n >= 255) { out.push(255); n -= 255; }
  out.push(n);
};
const lzLitCost = (n: number) => 1 + n + (n >= 15 ? Math.floor((n - 15) / 255) + 1 : 0);

/** The file as LZ frames of at most `budget` token bytes each (LZ4 block layout, decoded by stage-1's lz_decode): greedy
 *  with one-step lazy matching over hash chains; matches reach back up to 64 KB into anything already sent, every
 *  frame holds whole tokens. */
export function lzFrames(file: Uint8Array, budget: number = OTA_D2.max, chain = 48): D2Frame[] {
  const n = file.length;
  const head = new Map<number, number>();
  const prev = new Int32Array(n).fill(-1);
  const key = (p: number) => (file[p]! | (file[p + 1]! << 8) | (file[p + 2]! << 16) | (file[p + 3]! << 24)) >>> 0;
  let hpos = 0;
  const insertTo = (p: number) => {
    for (; hpos < p && hpos + LZ_MIN <= n; hpos++) {
      const k = key(hpos);
      prev[hpos] = head.get(k) ?? -1;
      head.set(k, hpos);
    }
  };
  const bestAt = (p: number): [number, number] => {
    if (p + LZ_MIN > n) return [0, 0];
    insertTo(p);
    let j = head.get(key(p)) ?? -1;
    let best = 0, bd = 0, tries = chain;
    const lim = Math.min(n - p, 0xffff);
    while (j >= 0 && tries) {
      if (j < p) {
        if (p - j > 0xffff) break;
        let l = 0;
        while (l < lim && file[j + l] === file[p + l]) l++;
        if (l > best) { best = l; bd = p - j; if (l === lim) break; }
        tries--;
      }
      j = prev[j]!;
    }
    return best >= LZ_MIN ? [best, bd] : [0, 0];
  };
  const frames: D2Frame[] = [];
  let pos = 0;
  while (pos < n) {
    const start = pos;
    const out: number[] = [];
    let lit0 = pos;
    while (pos < n) {
      let [ml, md] = bestAt(pos);
      if (ml && pos + 1 < n && bestAt(pos + 1)[0] > ml + 1) ml = 0;      // a longer match one byte later: a literal
      const nl = pos - lit0;
      if (ml) {
        const piece = [(Math.min(nl, 15) << 4) | Math.min(ml - LZ_MIN, 15)];
        if (nl >= 15) lzExt(nl - 15, piece);
        for (let k = lit0; k < pos; k++) piece.push(file[k]!);
        piece.push(md & 0xff, md >> 8);
        if (ml - LZ_MIN >= 15) lzExt(ml - LZ_MIN - 15, piece);
        if (out.length + piece.length > budget) break;
        out.push(...piece);
        pos += ml;
        lit0 = pos;
      } else {
        if (out.length + lzLitCost(nl + 1) > budget) break;
        pos++;
      }
    }
    let nl = pos - lit0;
    while (nl && out.length + lzLitCost(nl) > budget) nl--;
    if (nl || !out.length) {
      if (!nl) throw new Error("LZ budget too small");
      out.push(Math.min(nl, 15) << 4);
      if (nl >= 15) lzExt(nl - 15, out);
      for (let k = lit0; k < lit0 + nl; k++) out.push(file[k]!);
    }
    pos = lit0 + nl;
    frames.push({ off: start, end: pos, flags: OTA_D2.lz, data: Uint8Array.from(out) });
  }
  return frames;
}

/** Reference decoder (= cave/src/s1.c lz_decode): tokens into out from pos, never past lim; new pos or -1 if malformed. */
export function lzDecode(t: Uint8Array, out: Uint8Array, pos: number, lim: number): number {
  let i = 0;
  const n = t.length;
  const more = (len: number): number => {
    for (;;) {
      if (i >= n) return -1;
      const b = t[i++]!;
      len += b;
      if (b !== 255) return len;
    }
  };
  while (i < n) {
    const tok = t[i++]!;
    let len = tok >> 4;
    if (len === 15 && (len = more(len)) < 0) return -1;
    if (len > n - i || len > lim - pos) return -1;
    out.set(t.subarray(i, i + len), pos);
    pos += len;
    i += len;
    if (i === n) break;
    if (n - i < 2) return -1;
    const d = t[i]! | (t[i + 1]! << 8);
    i += 2;
    len = (tok & 15) + 4;
    if ((tok & 15) === 15 && (len = more(len)) < 0) return -1;
    if (d === 0 || d > pos || len > lim - pos) return -1;
    for (let k = 0; k < len; k++, pos++) out[pos] = out[pos - d]!;
  }
  return pos;
}

/** The loader's fast-upload offer, from Begin's reply (null = an older loader: 200 B stop-and-wait). */
export interface FastOffer { caps: number; window: number; max: number }
export function decodeBeginOffer(r: Uint8Array): FastOffer | null {
  if (r.length < 9 || !(r[5]! & OTA_D2.capData2) || !r[6] || u16(r, 7) < 16) return null;
  return { caps: r[5]!, window: r[6]!, max: u16(r, 7) };
}

export type OtaPhase = "status" | "begin" | "upload" | "commit" | "done" | "error";
export interface OtaProgress {
  phase: OtaPhase; sent: number; total: number; message?: string; status?: OtaStatus;
  /** what is written and where, once planned (target = the Begin / Commit target byte) */
  kind?: "main" | "loader"; target?: number; seq?: number;
  /** the piano's refusal code (or the same case refused by the app before sending), or an app-only refusal */
  rc?: number; refusal?: OtaLocalRefusal;
  /** times the upload was sent again from the start (damaged frame / CRC error) */
  restarts?: number;
  /** fast upload only: times the sender went back to the piano's `received` (lost / damaged frame, timeout) */
  resends?: number;
  /** how the data goes: v2 frames (chunk bytes each, `window` in flight, LZ or raw) or the old 200 B stop-and-wait */
  framing?: { v2: boolean; chunk: number; window: number; lz: boolean };
}

/** Options of OtaClient.update (tests, diagnostics); a loader without the fast upload always gets 200 B stop-and-wait. */
export interface OtaUploadOptions {
  /** v2 frame size in bytes (clamped to the loader's max), default OTA_FAST_DEFAULTS.chunk */
  chunk?: number;
  /** frames in flight (clamped to the loader's window), default OTA_FAST_DEFAULTS.window */
  window?: number;
  /** LZ frames when the loader offers them: "auto" (default: when they are at least 3 % smaller), true, false */
  lz?: boolean | "auto";
  /** false = always the old 200 B stop-and-wait (default true) */
  fast?: boolean;
}

export class OtaClient {
  private readonly session: PianoSession;
  constructor(session: PianoSession) {
    this.session = session;
  }

  /** What firmware job holds this link (upload, apply, reject, loader confirm), or null. */
  static busy(session: PianoSession): string | null {
    return xferBusy(session, "ota");
  }

  /** One firmware job per link (src/protocol/xferlock.ts): `refused` without sending anything while another runs. */
  private job<T>(what: string, fn: () => Promise<T>, refused: () => T): Promise<T> {
    return withXferLock(this.session, "ota", what, fn, refused);
  }

  private request(op: number, args: number[], timeoutMs: number, accept?: (d: Uint8Array) => boolean): Promise<Uint8Array | null> {
    return new Promise((resolve) => {
      let done = false;
      const finish = (v: Uint8Array | null) => {
        if (done) return;
        done = true;
        off();
        clearTimeout(timer);
        resolve(v);
      };
      const off = this.session.onMidi((m) => {
        const r = parseNgReply(m);
        if (r && r.op === op && (!accept || accept(r.data))) finish(r.data);
      });
      const timer = setTimeout(() => finish(null), timeoutMs);
      this.session.sendRaw(Uint8Array.from([0xf0, NG_ID, ...NG_TAG, op, ...args.map((b) => b & 0x7f), 0xf7]));
    });
  }

  async status(timeoutMs = 1000): Promise<OtaStatus | null> {
    const d = await this.request(OtaOp.Status, [], timeoutMs);
    return d ? decodeOtaStatus(d) : null;
  }

  private async authed(op: number, key: Uint8Array, args: number[], timeoutMs = 1500): Promise<number | null> {
    const r = await this.authedRaw(op, key, args, timeoutMs);
    return r ? r[0]! : null;
  }

  /** An authenticated op's whole reply (rc first; Begin of a fast-upload loader adds caps · window · max). */
  private async authedRaw(op: number, key: Uint8Array, args: number[], timeoutMs = 1500): Promise<Uint8Array | null> {
    const st = await this.status();
    if (!st) return null;
    const d = await this.request(op, pack7([...args, ...authTag(key, op, st.challenge, args)]), timeoutMs);
    const r = d ? unpack7(d) : null;
    return r && r.length >= 1 ? r : null;
  }

  /** v2 data frames with up to `window` in flight (REMOTE_PROTOCOL 10.3 Data v2). Every frame is answered with
   *  rc · received · off; `received` is cumulative. Replies come back in send order, so a reply answers the oldest
   *  outstanding copy of its frame and every older copy without a reply was lost. On rc 7 (gap) / 10 (CRC) the sender
   *  goes back to `received` - unless a copy of that frame sent after the refused one is still on its way; on a
   *  timeout it goes back too and keeps one frame in flight from then on. Same piano-side checks as before: the
   *  file's CRC, header and signature at Commit. */
  private sendWindowed(frames: D2Frame[], window: number, onSent: (sent: number, resends: number) => void):
    Promise<{ ok: true; resends: number } | { ok: false; message: string; resends: number }> {
    const total = frames[frames.length - 1]!.end;
    const index = new Map(frames.map((f, i) => [f.off, i]));
    const wire = frames.map(d2Message);
    const timeoutMs = 1500 + window * Math.max(...wire.map((w) => w.length)) * 0.4;
    const replies: { rc: number; rx: number; off: number }[] = [];
    let wake: (() => void) | null = null;
    const offMidi = this.session.onMidi((m) => {
      const r = parseNgReply(m);
      if (!r || r.op !== OtaOp.Data) return;
      const u = unpack7(r.data);
      if (u.length < 9) return;
      replies.push({ rc: u[0]!, rx: u32(u, 1), off: u32(u, 5) });
      wake?.();
    });
    return (async () => {
      let base = 0, nxt = 0, seq = 0, resends = 0, timeouts = 0;
      let out: { seq: number; off: number; at: number }[] = [];
      const at = (o: number) => index.get(o) ?? frames.length;
      try {
        while (base < total) {
          for (let r = replies.shift(); r; r = replies.shift()) {
            const k = out.findIndex((x) => x.off === r!.off);
            const sNak = k >= 0 ? out[k]!.seq : -1;
            if (k >= 0) out.splice(0, k + 1);                 // this copy answered, the older ones lost
            if (r.rx > base) {
              if (r.rx > total || (r.rx < total && !index.has(r.rx)))
                return { ok: false as const, message: `the piano holds ${r.rx} bytes, not a frame boundary`, resends };
              base = r.rx;
              out = out.filter((x) => x.off >= base);
              nxt = Math.max(nxt, at(base));
              onSent(base, resends);
            }
            if (r.rc === 0) continue;
            if (r.rc === 7 || r.rc === 10) {
              if (k >= 0 && !out.some((x) => x.off === base && x.seq > sNak)) {
                nxt = at(base);
                resends++;
              }
              continue;
            }
            return { ok: false as const, message: `upload stopped at ${base} B: ${OTA_ERRORS[r.rc] ?? r.rc}`, resends };
          }
          if (base >= total) break;
          if (out.length && Date.now() - out[0]!.at > timeoutMs) {
            if (++timeouts > 8) return { ok: false as const, message: `upload stopped at ${base} B: no reply`, resends };
            window = 1;                                       // something on the way could not keep up: one at a time
            out = [];
            nxt = at(base);
            resends++;
          }
          while (out.length < window && nxt < frames.length) {
            this.session.sendRaw(wire[nxt]!);
            out.push({ seq: ++seq, off: frames[nxt]!.off, at: Date.now() });
            nxt++;
          }
          if (!replies.length) {
            const wait = out.length ? Math.max(1, out[0]!.at + timeoutMs - Date.now() + 1) : 1;
            await new Promise<void>((res) => {
              const t = setTimeout(res, wait);
              wake = () => { clearTimeout(t); res(); };
            });
            wake = null;
          }
        }
        return { ok: true as const, resends };
      } finally {
        offMidi();
      }
    })();
  }

  /** One data op; returns [rc, bytes received so far] or null on timeout. */
  async data(offset: number, chunk: Uint8Array, timeoutMs = 2000): Promise<[number, number] | null> {
    const d = await this.request(OtaOp.Data, [(offset >> 14) & 0x7f, (offset >> 7) & 0x7f, offset & 0x7f, ...pack7(chunk)], timeoutMs);
    const r = d ? unpack7(d) : null;
    return r && r.length >= 5 ? [r[0]!, u32(r, 1)] : null;
  }

  /** Marks slots (bit 0 = main A, bit 1 = main B; v4: bit 4 = loader A, bit 5 = loader B) never to boot again;
   *  takes effect at the next power-on. */
  reject(key: Uint8Array, mask: number): Promise<number | null> {
    return this.job("reject", () => this.authed(OtaOp.Reject, key, [mask & 0x33]), () => 3);
  }

  /** Slot info (op 0x0E, v4, no auth): what stage-1 read of one slot. Unknown targets get no answer (null). */
  async info(target: number, timeoutMs = 1000): Promise<OtaSlotInfo | null> {
    // the reply names its target (raw byte 0): an overlapping read of another slot is not this one's answer
    const d = await this.request(OtaOp.Info, [target], timeoutMs, (x) => unpack7(x.subarray(0, 2))[0] === target);
    return d ? decodeSlotInfo(d) : null;
  }

  /** Confirm the running loader (op 0x0F, v4): without it stage-0 gives the new loader up after LOADER_TRIES
   *  unconfirmed power-ons and runs the previous one. The reply only means "accepted": the slot's confirm mark is
   *  written by the engine loop, so the status is polled until the loader shows confirmed. */
  confirmLoader(key: Uint8Array, pollMs = 200, timeoutMs = 5000): Promise<{ ok: boolean; rc?: number; message: string; status?: OtaStatus }> {
    return this.job("loader confirm", () => this.confirmLoaderNow(key, pollMs, timeoutMs), () => ({ ok: false, rc: 3, message: OTA_ERRORS[3]! }));
  }

  private async confirmLoaderNow(key: Uint8Array, pollMs: number, timeoutMs: number): Promise<{ ok: boolean; rc?: number; message: string; status?: OtaStatus }> {
    const st = await this.status();
    if (!st) return { ok: false, message: "no reply" };
    const L = st.loader;
    if (!L || !(L.caps2 & OTA_CAP2.loaderConfirm)) return { ok: false, message: "this loader has no confirm op", status: st };
    if (L.running > 1) return { ok: false, rc: 4, message: OTA_ERRORS[4]!, status: st };
    if (L.slots[L.running]!.flags & SLOT_FLAG.confirmed) return { ok: true, message: "already confirmed", status: st };
    const rc = await this.authed(OtaOp.Confirm, key, [OtaTarget.LoaderA + L.running]);
    if (rc !== 0) return { ok: false, rc: rc ?? undefined, message: rc === null ? "no reply" : OTA_ERRORS[rc] ?? String(rc), status: st };
    for (let i = 0; i < Math.ceil(timeoutMs / pollMs); i++) {
      const s = await this.status();
      if (s?.loader && s.loader.slots[s.loader.running]!.flags & SLOT_FLAG.confirmed) return { ok: true, message: "confirmed", status: s };
      await new Promise((r) => setTimeout(r, pollMs));
    }
    return { ok: false, message: "the piano did not mark the loader confirmed in time" };
  }

  /** Apply now (ota_loader_v2): switch the running cave to `target` without a power cycle and wait
   *  until the piano reports done / error. A live-applied slot is not boot-tested: it is still on
   *  probation (3 boot attempts) at the next power-on. */
  apply(key: Uint8Array, target: number = ApplyTarget.Newest, onStatus: (s: OtaStatus) => void = () => {},
    pollMs = 200, timeoutMs = 20_000): Promise<{ ok: boolean; message: string; status?: OtaStatus; rc?: number }> {
    return this.job("apply", () => this.applyNow(key, target, onStatus, pollMs, timeoutMs),
      () => ({ ok: false, message: `apply refused: ${OTA_ERRORS[3]}`, rc: 3 }));
  }

  private async applyNow(key: Uint8Array, target: number, onStatus: (s: OtaStatus) => void, pollMs: number, timeoutMs: number):
    Promise<{ ok: boolean; message: string; status?: OtaStatus; rc?: number }> {
    const st = await this.status();
    if (!st) return { ok: false, message: "no reply" };
    if (!(st.caps & OTA_CAP.applyNow)) return { ok: false, message: "this loader cannot apply live - turn the piano off and on", status: st };
    const reselect = target === ApplyTarget.Reselect;
    if (target > ApplyTarget.Newest && !(reselect && st.loader && st.loader.caps2 & OTA_CAP2.reselect))
      return { ok: false, message: `apply refused: ${OTA_ERRORS[1]}`, status: st, rc: 1 };
    // v4 SAFE mode (E2): stage-1 starts no main, so it refuses to apply one (rc 24) - say so without asking
    if (st.loader?.safe && target !== ApplyTarget.NoMain && !reselect)
      return { ok: false, message: `apply refused: ${OTA_ERRORS[24]}`, status: st, rc: 24 };
    const before = st.loader?.reselects ?? 0;
    const rc = await this.authed(OtaOp.Apply, key, [target & 7]);
    if (rc !== 0) return { ok: false, message: `apply refused: ${rc === null ? "no reply" : OTA_ERRORS[rc] ?? rc}`, rc: rc ?? undefined };
    for (let i = 0; i < Math.ceil(timeoutMs / pollMs); i++) {
      await new Promise((r) => setTimeout(r, pollMs));
      const s = await this.status();
      if (!s) continue;
      onStatus(s);
      if (reselect) {
        // the old stage-1 reports "done" just before stage-0 drops it: done is the new stage-1 answering, which
        // stage-0's reselect counter (status 110, stops at 255) tells apart; at 255 (10.12): byte 86 bit 2 and an
        // idle apply state (0) - a fresh stage-1 starts idle, the old one still says 3 (quiescing) or 5 (done)
        const L = s.loader;
        if (L && (L.reselects !== before || (before === 255 && L.s0Flags & S0_FLAG.reselected && s.applyState === 0))) {
          this.session.noteSwapped();
          return { ok: true, message: targetName(OtaTarget.LoaderA + L.running), status: s };
        }
        if (s.applyState === 6) return { ok: false, message: OTA_ERRORS[s.applyErr] ?? String(s.applyErr), status: s, rc: s.applyErr };
        continue;
      }
      if (s.applyState === 5) {
        // the new payload's session core knows only the stock binding: this app CONNECTs again (connection manager)
        this.session.noteSwapped();
        return { ok: true, message: activeName(s), status: s };
      }
      if (s.applyState === 6) return { ok: false, message: OTA_ERRORS[s.applyErr] ?? String(s.applyErr), status: s, rc: s.applyErr };
    }
    return { ok: false, message: "the piano did not finish applying in time" };
  }

  /** Upload + commit + wait for the piano to finish writing. Resolves with the final progress.
   *
   *  The resident stores whatever a Data frame unpacks to (ota.c ota_op: only `off == received` and "not past the
   *  end" are checked, no per-frame CRC) and answers `received = off + stored`. A frame that lost a byte or a BLE
   *  packet on the way still ends in F7, so it is stored short and every later byte sits one frame-tail off: the
   *  upload runs to the end and fails only at J_VERIFY as "CRC mismatch" (field 2026-11-16, ota_main 1ec47025
   *  twice; same class as the panel loader). Every frame this
   *  client sends ends on the grid (k·OTA_CHUNK or the end), so a `received` off that grid = a damaged frame:
   *  Begin again (the resident takes a new Begin while receiving or after a failed job: received = 0) and send it
   *  all again. A changed byte keeps the length and shows only as the job's CRC error, which also sends it again.
   *  `maxRestarts` (default 2) bounds both.
   *
   *  Fast upload: a loader whose Begin reply offers v2 frames (stage-1 image version 2) gets them instead - a CRC-32
   *  per frame (a damaged frame is refused and only it is sent again), up to 1536 B, LZ tokens when that is smaller,
   *  several frames in flight (sendWindowed). Progress, restarts (a CRC error at Commit still sends it all again) and
   *  the result stay as above; `resends` counts the go-backs. Every other loader (v3, v4 before image version 2) answers
   *  Begin with 5 bytes and gets the 200 B stop-and-wait frames. */
  update(file: Uint8Array, key: Uint8Array, onProgress: (p: OtaProgress) => void = () => {}, pollMs = 300,
    maxRestarts = 2, opts: OtaUploadOptions = {}): Promise<OtaProgress> {
    return this.job("upload", () => this.updateNow(file, key, onProgress, pollMs, maxRestarts, opts), () => {
      const p: OtaProgress = { phase: "error", sent: 0, total: file.length, message: OTA_ERRORS[3], rc: 3 };
      onProgress(p);
      return p;
    });
  }

  private async updateNow(file: Uint8Array, key: Uint8Array, onProgress: (p: OtaProgress) => void, pollMs: number,
    maxRestarts: number, opts: OtaUploadOptions): Promise<OtaProgress> {
    let restarts = 0;
    let resends = 0;
    let framing: OtaProgress["framing"];
    const report = (p: OtaProgress): OtaProgress => {
      const full = { ...p, restarts, ...(framing ? { framing, resends } : {}) };
      onProgress(full);
      return full;
    };
    let planned: Pick<OtaProgress, "kind" | "target" | "seq"> = {};
    const fail = (message: string, status?: OtaStatus, why: Pick<OtaProgress, "rc" | "refusal"> = {}): OtaProgress =>
      report({ phase: "error", sent: 0, total: file.length, message, status, ...planned, ...why });
    const info = parseOtaFile(file);
    if (!info) return fail("not a OpenGrand payload file (.c79)");
    // a main is signed with the key the app holds; a loader with stage-0's root key (status 94..97, checked below)
    if (info.kind !== "loader" && info.keyId !== keyId(key)) return fail("the file was signed with a different key than the one imported");
    report({ phase: "status", sent: 0, total: file.length });
    const st = await this.status();
    if (!st) return fail("no reply - is ota_loader on the piano?");
    if (st.keyId !== keyId(key)) return fail("the piano has a different key (key id mismatch)", st);
    if (!st.booted) return fail("the piano is still selecting its cave - try again in a second", st);
    const plan = planUpload(info, st);
    if (!plan.ok) return fail(plan.message, st, { rc: plan.rc, refusal: plan.refusal });
    const { target, seq } = plan;
    planned = { kind: plan.kind, target, seq };
    const crc = crc32(file);
    const onGrid = (n: number) => n === file.length || n % OTA_CHUNK === 0;
    const refused = (what: string, rc: number | null) =>
      fail(`${what} refused: ${rc === null ? "no reply" : OTA_ERRORS[rc] ?? rc}`, undefined, rc === null ? {} : { rc });
    for (;;) {
      report({ phase: "begin", sent: 0, total: file.length, ...planned });
      const br = await this.authedRaw(OtaOp.Begin, key, [target, ...le32(file.length), ...le32(crc), ...le32(seq)]);
      const rc = br ? br[0]! : null;
      if (rc !== 0) return refused("begin", rc);
      const offer = opts.fast === false ? null : decodeBeginOffer(br!);
      let off = 0;
      let retries = 0;
      let damaged = -1;
      if (offer) {
        // fast upload: v2 frames (CRC each), several in flight; a frame can no longer be stored short
        const chunk = Math.max(16, Math.min(opts.chunk ?? OTA_FAST_DEFAULTS.chunk, offer.max));
        const window = Math.max(1, Math.min(opts.window ?? OTA_FAST_DEFAULTS.window, offer.window));
        let frames = rawFrames(file, chunk);
        if (offer.caps & OTA_D2.capLz && opts.lz !== false) {
          const lz = lzFrames(file, chunk);
          const lzBytes = lz.reduce((a, f) => a + f.data.length, 0);
          if (opts.lz === true || lzBytes <= file.length * 0.97) frames = lz;
        }
        framing = { v2: true, chunk, window, lz: frames[0]!.flags === OTA_D2.lz };
        const before = resends;
        const res = await this.sendWindowed(frames, window, (sent, n) => {
          resends = before + n;
          report({ phase: "upload", sent, total: file.length });
        });
        resends = before + res.resends;
        if (!res.ok) return fail(res.message);
        off = file.length;
      } else framing = { v2: false, chunk: OTA_CHUNK, window: 1, lz: false };
      while (!offer && off < file.length) {
        const chunk = file.slice(off, off + OTA_CHUNK);
        const r = await this.data(off, chunk);         // stop-and-wait: one frame in flight
        if (!r || (r[0] !== 0 && r[0] !== 7)) {
          if (++retries > 5) return fail(`upload stopped at ${off} B: ${r ? OTA_ERRORS[r[0]] ?? r[0] : "no reply"}`);
          continue;
        }
        retries = 0;
        if (!onGrid(r[1]) || r[1] > file.length) {     // the piano stored a frame of the wrong length
          damaged = r[1];
          break;
        }
        off = r[1];                                     // the piano says where it is (also after a lost ACK)
        report({ phase: "upload", sent: off, total: file.length });
      }
      let why: string;
      if (damaged >= 0) why = `damaged frame: the piano holds ${damaged} bytes, off the ${OTA_CHUNK}-byte frame grid`;
      else {
        report({ phase: "commit", sent: off, total: file.length, ...planned });
        const c = await this.authed(OtaOp.Commit, key, [target]);
        if (c !== 0) return refused("commit", c);
        let end: OtaStatus | null = null;
        for (let i = 0; i < Math.ceil(60_000 / pollMs) && !end; i++) {
          await new Promise((r) => setTimeout(r, pollMs));
          const s = await this.status();
          if (!s) continue;
          if (s.job === 8 || s.job === 9) end = s;
          else report({ phase: "commit", sent: file.length, total: file.length, status: s, message: OTA_JOB[s.job] });
        }
        if (!end) return fail("the piano did not finish writing in 60 s");
        if (end.job === 8) return report({ phase: "done", sent: file.length, total: file.length, status: end, message: targetName(target), ...planned });
        // OE_CRC: every byte arrived, one of them damaged on the way; checked in RAM, the flash was not touched
        if (end.err !== 10 || restarts >= maxRestarts) {
          const more = end.err === 10 && restarts ? ` (after ${restarts} resend${restarts > 1 ? "s" : ""})` : "";
          return fail(`writing failed: ${OTA_ERRORS[end.err] ?? end.err}${more}`, end, { rc: end.err });
        }
        why = "CRC mismatch: damaged on the way";
      }
      if (damaged >= 0 && restarts >= maxRestarts) return fail(`${why}; gave up after ${restarts} resends`);
      restarts++;
      report({ phase: "begin", sent: 0, total: file.length, message: `${why} - sending it again from the start` });
    }
  }
}

