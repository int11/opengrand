// Panel UI package (.c79u) — signing key, packer, loader config (REMOTE_PROTOCOL.md §10.7). Node only.
//
// Key: Ed25519, PKCS#8 PEM at <main checkout>/result/stockfw/secrets/panelui_ed25519.pem (git-ignored
// with the OTA key, but a DIFFERENT key: the OTA key is a per-device HMAC secret patched into the
// piano; this one only signs panel UI packages and its public half is baked into the USB UI bundle).
// Override with CA79_PANELUI_KEY=<path>. From a git worktree the main checkout's secrets/ is used,
// so every build signs with the same key.
import { createHash, createPrivateKey, createPublicKey, generateKeyPairSync, sign, type KeyObject } from "node:crypto";
import { execFileSync } from "node:child_process";
import { chmodSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { runInNewContext } from "node:vm";
import { transformSync } from "esbuild";
import { deflateRawSync } from "node:zlib";

import { crc32 } from "../src/protocol/ngext.ts";
import { encodeHeader, PUI_ABI, PUI_FLAG_DEFLATE, PUI_MAX_PKG } from "../src/protocol/panelui.ts";

const APP = resolve(dirname(fileURLToPath(import.meta.url)), "..");
export const LOADER_SRC = join(APP, "panelui/uiloader.js");

/** The main checkout (not the worktree) — where git-ignored secrets/ and target/ live. */
export function mainRepoRoot(): string {
  try {
    const common = execFileSync("git", ["rev-parse", "--path-format=absolute", "--git-common-dir"], { cwd: APP, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] }).trim();
    if (common) return dirname(common);
  } catch {
    /* not a git checkout */
  }
  return resolve(APP, "../..");
}

export function defaultKeyPath(): string {
  return process.env.CA79_PANELUI_KEY ?? join(mainRepoRoot(), "result/stockfw/secrets/panelui_ed25519.pem");
}

export interface PanelKey {
  priv: KeyObject | null;
  /** raw 32-byte public key */
  pub: Uint8Array;
  /** first 4 bytes of SHA-512(pub), as hex (the header's key id, LE u32 of the same bytes) */
  keyIdHex: string;
  keyId: number;
  path: string;
  created: boolean;
}

export function rawPub(k: KeyObject): Uint8Array {
  const jwk = k.export({ format: "jwk" }) as { x?: string };
  if (!jwk.x) throw new Error("not an Ed25519 key");
  return new Uint8Array(Buffer.from(jwk.x, "base64url"));
}

export function keyFromPem(pem: string, path = "(memory)", created = false): PanelKey {
  const priv = createPrivateKey(pem);
  if (priv.asymmetricKeyType !== "ed25519") throw new Error(`${path}: not an Ed25519 private key`);
  const pub = rawPub(createPublicKey(priv));
  const id = createHash("sha512").update(pub).digest().subarray(0, 4);
  return { priv, pub, keyIdHex: id.toString("hex"), keyId: id.readUInt32LE(0), path, created };
}

export function newKeyPem(): string {
  return generateKeyPairSync("ed25519").privateKey.export({ type: "pkcs8", format: "pem" }).toString();
}

/** Load the signing key; with create, make one (mode 0600) when there is none. */
export function loadKey(path = defaultKeyPath(), opt: { create?: boolean } = {}): PanelKey {
  if (!existsSync(path)) {
    if (!opt.create) throw new Error(`no panel UI signing key at ${path} — run: npm run panelui:key`);
    mkdirSync(dirname(path), { recursive: true });
    writeFileSync(path, newKeyPem(), { mode: 0o600 });
    chmodSync(path, 0o600);
    return keyFromPem(readFileSync(path, "utf8"), path, true);
  }
  return keyFromPem(readFileSync(path, "utf8"), path);
}

// ---- package -----------------------------------------------------------------------

export interface UiFiles {
  js: string;
  css: string;
}

/** Seconds since the Unix epoch: monotonic across builds, fits u32 until 2106. */
export const seqNow = () => Math.floor(Date.now() / 1000);

export function makePackage(files: UiFiles, o: { version: string; seq: number; key: PanelKey; built?: string }): Buffer {
  if (!o.key.priv) throw new Error("signing needs the private key");
  const raw = Buffer.from(JSON.stringify({ format: 1, version: o.version, built: o.built ?? "", js: files.js, css: files.css }), "utf8");
  const payload = deflateRawSync(raw, { level: 9 });
  if (payload.length > PUI_MAX_PKG) throw new Error(`package payload ${payload.length} B > ${PUI_MAX_PKG} B`);
  const header = encodeHeader({
    format: 1, flags: PUI_FLAG_DEFLATE, abi: PUI_ABI, seq: o.seq >>> 0, length: payload.length, unpacked: raw.length,
    crc: crc32(payload), keyId: o.key.keyId, version: o.version, hash: createHash("sha512").update(payload).digest().subarray(0, 32),
  });
  const sig = sign(null, header, o.key.priv);
  return Buffer.concat([Buffer.from(header), sig, payload]);
}

// ---- loader config + cross-check -----------------------------------------------------

export interface UiConfig {
  pub: string; // 64 hex
  keyId: string;
  bundled: { version: string; seq: number };
}

/** uiloader.js with the bundle's trust anchor and bundled-version facts filled in. */
/** The panel screen reporter (ops 0x35 / 0x36): inlined in
 *  the USB bundle pages after the loader and prepended to the app js / probe page of every remote package. */
export const SCREEN_SRC = join(APP, "panelui/panelscreen.js");
export function screenSource(o: { minify?: boolean } = {}): string {
  let src = readFileSync(SCREEN_SRC, "utf8");
  // minified it is ~21 KB, ~8.6 KB deflated in a package (the source 36 KB / 12 KB): the BLE seconds count
  if (o.minify !== false) src = transformSync(src, { minify: true, target: ["es2017", "chrome60"], legalComments: "none" }).code;
  if (/<\/script/i.test(src)) throw new Error("panelui/panelscreen.js must not contain a closing script tag");
  return src;
}

export function loaderSource(cfg: UiConfig | null): string {
  const src = readFileSync(LOADER_SRC, "utf8");
  if (!cfg) return src;
  const re = /\/\*UI_CONFIG\*\/[\s\S]*?\/\*END\*\//;
  if (!re.test(src)) throw new Error("panelui/uiloader.js: UI_CONFIG marker missing");
  return src.replace(re, `/*UI_CONFIG*/${JSON.stringify(cfg).replace(/</g, "\\u003c")}/*END*/`);
}

export function uiConfig(key: PanelKey | null, bundled: { version: string; seq: number }): UiConfig {
  return { pub: key ? Buffer.from(key.pub).toString("hex") : "", keyId: key?.keyIdHex ?? "", bundled };
}

/** Run the panel's own verifier (uiloader.js in a VM) on a package: rc 0 = the panel accepts it. */
export function panelCheck(pkg: Uint8Array, cfg: UiConfig): { rc: number; version?: string; seq?: number; jsBytes?: number } {
  const ctx: Record<string, unknown> = {};
  runInNewContext(loaderSource(cfg), ctx, { filename: "uiloader.js" });
  const ui = ctx.CA79UI as { _: { openPackage(p: Uint8Array): { rc: number; h?: { version: string; seq: number }; files?: { js: string } } } };
  const o = ui._.openPackage(new Uint8Array(pkg));
  return { rc: o.rc, version: o.h?.version, seq: o.h?.seq, jsBytes: o.files?.js.length };
}
