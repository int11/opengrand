// USB first-install container for the original panel shell: <usb root>/version.xxx + appUI.xxx, installed by the
// shell's UI [UPDATE]. ENCRYPT ONLY, with key material the user supplies — this repo ships no key, no key
// derivation and no decryptor. Advanced: not needed for remote (.c79u) updates once our bundle is installed.
//
// Key file (path in env CA79_KWM_KEY or --kwm-key <path>), JSON, all values lowercase hex except "c1":
//   {
//     "versionKey": "<64 hex: AES-256 key for version.xxx>",  "versionIv": "<32 hex>",
//     "c1":         "<the token line written into version.xxx, digits>",
//     "key2": "<64 hex>", "iv2": "<32 hex>",   // encrypts the MD5 line of version.xxx
//     "key3": "<64 hex>", "iv3": "<32 hex>"    // encrypts appUI.xxx
//   }
// The tool does not know or check how c1 relates to key2/iv2/key3/iv3 on the panel: the user provides a
// consistent set. Container layout (all AES-256-CBC, PKCS#7):
//   version.xxx = AES(versionKey, versionIv, "<X.Y.Z>\n<c1>\n<base64(AES(key2, iv2, md5hex(zip)))>")
//   appUI.xxx   = AES(key3, iv3, zip)
import { createCipheriv, createHash } from "node:crypto";
import { readFileSync } from "node:fs";

export const VERSION_RE = /^\d+\.\d+\.\d+$/;

export interface UsbKey {
  versionKey: Buffer;
  versionIv: Buffer;
  c1: string;
  key2: Buffer;
  iv2: Buffer;
  key3: Buffer;
  iv3: Buffer;
}

/** The key file path from --kwm-key or CA79_KWM_KEY; null = no USB container can be built. */
export function usbKeyPath(opt?: string): string | null {
  return opt ?? process.env.CA79_KWM_KEY ?? null;
}

export function parseUsbKey(json: string, where = "(key file)"): UsbKey {
  const o = JSON.parse(json) as Record<string, unknown>;
  const hex = (k: string, bytes: number): Buffer => {
    const v = o[k];
    if (typeof v !== "string" || !new RegExp(`^[0-9a-fA-F]{${bytes * 2}}$`).test(v)) throw new Error(`${where}: "${k}" must be ${bytes * 2} hex digits`);
    return Buffer.from(v, "hex");
  };
  const c1 = o.c1;
  if (typeof c1 !== "string" || !/^\d{1,256}$/.test(c1)) throw new Error(`${where}: "c1" must be a string of digits`);
  return { versionKey: hex("versionKey", 32), versionIv: hex("versionIv", 16), c1, key2: hex("key2", 32), iv2: hex("iv2", 16), key3: hex("key3", 32), iv3: hex("iv3", 16) };
}

export function loadUsbKey(path: string): UsbKey {
  return parseUsbKey(readFileSync(path, "utf8"), path);
}

function aes(data: Buffer, key: Buffer, iv: Buffer): Buffer {
  const c = createCipheriv("aes-256-cbc", key, iv);
  return Buffer.concat([c.update(data), c.final()]);
}

export interface UsbBundle {
  versionXxx: Buffer;
  appUiXxx: Buffer;
  md5: string;
}

export function encryptUsbBundle(zip: Buffer, version: string, k: UsbKey): UsbBundle {
  if (!VERSION_RE.test(version)) throw new Error(`version "${version}" must match ${VERSION_RE} (the shell rejects others)`);
  const md5 = createHash("md5").update(zip).digest("hex");
  const sc2 = aes(Buffer.from(md5, "ascii"), k.key2, k.iv2).toString("base64");
  const versionXxx = aes(Buffer.from(`${version}\n${k.c1}\n${sc2}`, "utf8"), k.versionKey, k.versionIv);
  return { versionXxx, appUiXxx: aes(zip, k.key3, k.iv3), md5 };
}
