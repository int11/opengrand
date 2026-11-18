// PWA pieces emitted by scripts/build.mjs into dist/ (also used by tests/pwa.test.ts):
//   manifest.webmanifest, icon-192.png, icon-512.png, icon-maskable-512.png, sw.js
// Icons are drawn here (a keyboard octave on the app's accent colour, ICON) and
// encoded as PNG with node:zlib — no image tooling needed.
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { crc32, deflateSync } from "node:zlib";

export const THEME = "#191919"; // styles.css --bar (dark theme: browser / status bar)
export const BACKGROUND = "#111111"; // styles.css --bg
export const ICON = "#10818e"; // styles.css --accent (icon background)

const hex = (c: string): number[] => [1, 3, 5].map((i) => parseInt(c.slice(i, i + 2), 16));

function chunk(type: string, data: Buffer): Buffer {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(td) >>> 0);
  return Buffer.concat([len, td, crc]);
}

/** Encode RGBA pixels (size*size*4) as PNG. */
export function encodePng(size: number, rgba: Buffer): Buffer {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0);
  ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 6; // RGBA
  const raw = Buffer.alloc(size * (size * 4 + 1));
  for (let y = 0; y < size; y++) {
    raw[y * (size * 4 + 1)] = 0; // filter: none
    rgba.copy(raw, y * (size * 4 + 1) + 1, y * size * 4, (y + 1) * size * 4);
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw)),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

/**
 * Draw the icon: accent background (rounded square, or full bleed when
 * maskable) with one octave of keys inside the maskable safe zone.
 */
export function drawIcon(size: number, { maskable = false }: { maskable?: boolean } = {}): Buffer {
  const px = Buffer.alloc(size * size * 4);
  const [br, bg, bb] = hex(ICON);
  const S = 4; // supersampling per axis
  const r = maskable ? 0 : size * 0.18;
  // keyboard box: 56% wide, 40% high, centred (inside the 80% maskable safe circle)
  const kx0 = size * 0.22, kx1 = size * 0.78, ky0 = size * 0.3, ky1 = size * 0.7;
  const white = (kx1 - kx0) / 7;
  const gap = Math.max(1, size / 96);
  const blacks = [1, 2, 4, 5, 6]; // black key after white keys 0,1,3,4,5 (C# D# F# G# A#)
  const sample = (x: number, y: number): number[] | null => {
    // outside the rounded square: transparent
    if (r > 0) {
      const cx = Math.min(Math.max(x, r), size - r), cy = Math.min(Math.max(y, r), size - r);
      if ((x - cx) ** 2 + (y - cy) ** 2 > r * r) return null;
    }
    if (x >= kx0 && x < kx1 && y >= ky0 && y < ky1) {
      const k = (x - kx0) / white;
      // black keys: 60% of white width, 62% of the height
      if (y < ky0 + (ky1 - ky0) * 0.62 && blacks.some((b) => Math.abs(k - b) < 0.3)) return [29, 28, 26];
      if (k - Math.floor(k) < gap / white && Math.floor(k) > 0) return [br, bg, bb]; // key gaps
      return [255, 255, 255];
    }
    return [br, bg, bb];
  };
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      let R = 0, G = 0, B = 0, A = 0;
      for (let sy = 0; sy < S; sy++)
        for (let sx = 0; sx < S; sx++) {
          const c = sample(x + (sx + 0.5) / S, y + (sy + 0.5) / S);
          if (!c) continue;
          R += c[0];
          G += c[1];
          B += c[2];
          A++;
        }
      const o = (y * size + x) * 4;
      if (A) {
        px[o] = Math.round(R / A);
        px[o + 1] = Math.round(G / A);
        px[o + 2] = Math.round(B / A);
      }
      px[o + 3] = Math.round((A / (S * S)) * 255);
    }
  }
  return encodePng(size, px);
}

export function manifest(version: string) {
  return {
    name: "OpenGrand",
    short_name: "OpenGrand",
    description: "Kawai CA79 remote — OpenGrand controller app",
    id: "./",
    start_url: "./",
    scope: "./",
    display: "standalone",
    orientation: "any",
    theme_color: THEME,
    background_color: BACKGROUND,
    version,
    icons: [
      { src: "icon-192.png", sizes: "192x192", type: "image/png", purpose: "any" },
      { src: "icon-512.png", sizes: "512x512", type: "image/png", purpose: "any" },
      { src: "icon-maskable-512.png", sizes: "512x512", type: "image/png", purpose: "maskable" },
    ],
  };
}

/** Service worker source with its version and precache list filled in. */
export function serviceWorker(version: string, precache: string[]): string {
  const tpl = readFileSync(new URL("./sw.js", import.meta.url), "utf8");
  return tpl.replace('"__VERSION__"', JSON.stringify(version)).replace("const PRECACHE = __PRECACHE__;", `const PRECACHE = ${JSON.stringify(precache)};`);
}

/**
 * Write manifest, icons and sw.js into `dir`. `assets` are the other built
 * files to precache (index.html, hashed js/css). Returns the files written.
 */
export function emitPwa(dir: string, { version, assets }: { version: string; assets: string[] }): string[] {
  const files: Record<string, string | Buffer> = {
    "manifest.webmanifest": JSON.stringify(manifest(version), null, 2) + "\n",
    "icon-192.png": drawIcon(192),
    "icon-512.png": drawIcon(512),
    "icon-maskable-512.png": drawIcon(512, { maskable: true }),
  };
  for (const [name, data] of Object.entries(files)) writeFileSync(join(dir, name), data);
  const precache = ["./", ...assets, ...Object.keys(files)];
  // the cache name changes whenever any precached file does
  writeFileSync(join(dir, "sw.js"), serviceWorker(`${version}-${hashOf(assets)}`, precache));
  return [...Object.keys(files), "sw.js"];
}

function hashOf(list: string[]): string {
  // the asset names already carry content hashes; fold them into a short tag
  let h = 0;
  for (const c of list.join("|")) h = (Math.imul(h, 31) + c.charCodeAt(0)) | 0;
  return (h >>> 0).toString(36);
}
