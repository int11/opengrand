// Minimal ZIP reader/writer (no zip64, no encryption) for the panel UI bundle
// tool. Enough for the Kawai appUI.kwm (286 entries, 70 MB, deflate/stored),
// APKs and the LCD update zip. Raw entries can be copied without recompression,
// so the original UI goes into our bundle bit-for-bit.
import { crc32, deflateRawSync, inflateRawSync } from "node:zlib";

export interface ZipEntry {
  name: string;
  method: number; // 0 stored, 8 deflate
  flags: number;
  crc: number;
  compSize: number;
  size: number;
  dosTime: number;
  dosDate: number;
  extAttr: number;
  localOffset: number;
}

export interface ZipReader {
  entries: ZipEntry[];
  get(name: string): ZipEntry | undefined;
  /** compressed bytes as stored */
  raw(e: ZipEntry): Buffer;
  /** uncompressed bytes, CRC-checked */
  read(e: ZipEntry | string): Buffer;
}

export function readZip(buf: Buffer): ZipReader {
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 22 - 0xffff); i--) {
    if (buf.readUInt32LE(i) === 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new Error("zip: end of central directory not found");
  const count = buf.readUInt16LE(eocd + 10);
  const cdSize = buf.readUInt32LE(eocd + 12);
  const cdOff = buf.readUInt32LE(eocd + 16);
  if (count === 0xffff || cdOff === 0xffffffff || cdSize === 0xffffffff) throw new Error("zip: zip64 not supported");
  const entries: ZipEntry[] = [];
  let p = cdOff;
  for (let k = 0; k < count; k++) {
    if (buf.readUInt32LE(p) !== 0x02014b50) throw new Error(`zip: bad central header at ${p}`);
    const nameLen = buf.readUInt16LE(p + 28);
    const extraLen = buf.readUInt16LE(p + 30);
    const commentLen = buf.readUInt16LE(p + 32);
    const flags = buf.readUInt16LE(p + 8);
    const nameBytes = buf.subarray(p + 46, p + 46 + nameLen);
    entries.push({
      name: nameBytes.toString(flags & 0x800 ? "utf8" : "latin1"),
      method: buf.readUInt16LE(p + 10),
      flags,
      dosTime: buf.readUInt16LE(p + 12),
      dosDate: buf.readUInt16LE(p + 14),
      crc: buf.readUInt32LE(p + 16),
      compSize: buf.readUInt32LE(p + 20),
      size: buf.readUInt32LE(p + 24),
      extAttr: buf.readUInt32LE(p + 38),
      localOffset: buf.readUInt32LE(p + 42),
    });
    p += 46 + nameLen + extraLen + commentLen;
  }
  const byName = new Map(entries.map((e) => [e.name, e]));
  const raw = (e: ZipEntry): Buffer => {
    const l = e.localOffset;
    if (buf.readUInt32LE(l) !== 0x04034b50) throw new Error(`zip: bad local header for ${e.name}`);
    const start = l + 30 + buf.readUInt16LE(l + 26) + buf.readUInt16LE(l + 28);
    return buf.subarray(start, start + e.compSize);
  };
  const read = (x: ZipEntry | string): Buffer => {
    const e = typeof x === "string" ? byName.get(x) : x;
    if (!e) throw new Error(`zip: no entry ${String(x)}`);
    const r = raw(e);
    let out: Buffer;
    if (e.method === 0) out = Buffer.from(r);
    else if (e.method === 8) out = inflateRawSync(r);
    else throw new Error(`zip: ${e.name}: method ${e.method} not supported`);
    if (out.length !== e.size || (crc32(out) >>> 0) !== e.crc) throw new Error(`zip: ${e.name}: size/CRC mismatch`);
    return out;
  };
  return { entries, get: (n) => byName.get(n), raw, read };
}

export type NewEntry =
  | { name: string; data: Buffer; store?: boolean; date?: Date } // compressed here (dirs: name ends with "/", empty data)
  | { name: string; copy: ZipEntry; from: ZipReader }; // raw copy from another zip

function dos(d: Date): { time: number; date: number } {
  return {
    time: (d.getHours() << 11) | (d.getMinutes() << 5) | Math.floor(d.getSeconds() / 2),
    date: ((Math.max(1980, d.getFullYear()) - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate(),
  };
}

export function writeZip(items: NewEntry[]): Buffer {
  const seen = new Set<string>();
  const locals: Buffer[] = [];
  const centrals: Buffer[] = [];
  let off = 0;
  for (const it of items) {
    if (seen.has(it.name)) throw new Error(`zip: duplicate entry ${it.name}`);
    seen.add(it.name);
    let method: number, crc: number, size: number, body: Buffer, t: number, d: number, extAttr: number, flags: number;
    const nameBuf = Buffer.from(it.name, "utf8");
    const utf8 = /[^\x20-\x7e]/.test(it.name) ? 0x800 : 0;
    if ("copy" in it) {
      const e = it.copy;
      method = e.method;
      crc = e.crc;
      size = e.size;
      body = it.from.raw(e);
      t = e.dosTime;
      d = e.dosDate;
      extAttr = e.extAttr;
      flags = (e.flags & 0x0806) | utf8; // keep deflate level bits, drop the data-descriptor bit
    } else {
      const dir = it.name.endsWith("/");
      size = it.data.length;
      crc = crc32(it.data) >>> 0;
      const deflated = dir || it.store ? null : deflateRawSync(it.data, { level: 9 });
      if (deflated && deflated.length < it.data.length) {
        method = 8;
        body = deflated;
      } else {
        method = 0;
        body = it.data;
      }
      const dd = dos(it.date ?? new Date(2026, 9, 31, 12, 0, 0));
      t = dd.time;
      d = dd.date;
      extAttr = dir ? 0x10 : 0;
      flags = utf8;
    }
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0);
    lh.writeUInt16LE(20, 4);
    lh.writeUInt16LE(flags, 6);
    lh.writeUInt16LE(method, 8);
    lh.writeUInt16LE(t, 10);
    lh.writeUInt16LE(d, 12);
    lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(body.length, 18);
    lh.writeUInt32LE(size, 22);
    lh.writeUInt16LE(nameBuf.length, 26);
    lh.writeUInt16LE(0, 28);
    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0);
    ch.writeUInt16LE(20, 4);
    ch.writeUInt16LE(20, 6);
    ch.writeUInt16LE(flags, 8);
    ch.writeUInt16LE(method, 10);
    ch.writeUInt16LE(t, 12);
    ch.writeUInt16LE(d, 14);
    ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(body.length, 20);
    ch.writeUInt32LE(size, 24);
    ch.writeUInt16LE(nameBuf.length, 28);
    ch.writeUInt32LE(extAttr, 38);
    ch.writeUInt32LE(off, 42);
    locals.push(lh, nameBuf, body);
    centrals.push(ch, nameBuf);
    off += 30 + nameBuf.length + body.length;
    if (off > 0xffffffff) throw new Error("zip: too large (zip64 not supported)");
  }
  const cd = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(items.length, 8);
  end.writeUInt16LE(items.length, 10);
  end.writeUInt32LE(cd.length, 12);
  end.writeUInt32LE(off, 16);
  return Buffer.concat([...locals, cd, end]);
}
