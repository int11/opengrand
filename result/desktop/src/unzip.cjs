// Minimal ZIP extractor for the staged desktop update (stored / deflate entries, zip64 sizes; no encryption).
// No Electron import (tests/updater.test.cjs). Entry names are checked with updatepolicy.safeEntryName, so nothing is
// written outside the target folder; every entry's size and CRC-32 are checked.
"use strict";

const fs = require("node:fs");
const path = require("node:path");
const zlib = require("node:zlib");
const { promisify } = require("node:util");
const { safeEntryName } = require("./updatepolicy.cjs");

const inflateRaw = promisify(zlib.inflateRaw);

function zip64Extra(extra, e) {
  for (let p = 0; p + 4 <= extra.length; ) {
    const id = extra.readUInt16LE(p);
    const len = extra.readUInt16LE(p + 2);
    if (id === 0x0001) {
      let q = p + 4;
      const take = () => {
        const v = Number(extra.readBigUInt64LE(q));
        q += 8;
        return v;
      };
      if (e.size === 0xffffffff) e.size = take();
      if (e.compSize === 0xffffffff) e.compSize = take();
      if (e.localOffset === 0xffffffff) e.localOffset = take();
    }
    p += 4 + len;
  }
}

/** Central directory of a zip held in `buf`: [{ name, method, crc, compSize, size, localOffset, dir }]. */
function listZip(buf) {
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 22 - 0xffff); i--) {
    if (buf.readUInt32LE(i) === 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new Error("zip: end of central directory not found");
  let count = buf.readUInt16LE(eocd + 10);
  let cdOff = buf.readUInt32LE(eocd + 16);
  if (count === 0xffff || cdOff === 0xffffffff) {
    const loc = eocd - 20;
    if (loc < 0 || buf.readUInt32LE(loc) !== 0x07064b50) throw new Error("zip: zip64 locator missing");
    const z = Number(buf.readBigUInt64LE(loc + 8));
    if (buf.readUInt32LE(z) !== 0x06064b50) throw new Error("zip: bad zip64 end record");
    count = Number(buf.readBigUInt64LE(z + 32));
    cdOff = Number(buf.readBigUInt64LE(z + 48));
  }
  const out = [];
  let p = cdOff;
  for (let k = 0; k < count; k++) {
    if (buf.readUInt32LE(p) !== 0x02014b50) throw new Error(`zip: bad central header at ${p}`);
    const flags = buf.readUInt16LE(p + 8);
    const nameLen = buf.readUInt16LE(p + 28);
    const extraLen = buf.readUInt16LE(p + 30);
    const commentLen = buf.readUInt16LE(p + 32);
    if (flags & 1) throw new Error("zip: encrypted entry");
    const name = buf.subarray(p + 46, p + 46 + nameLen).toString(flags & 0x800 ? "utf8" : "latin1");
    const e = {
      name,
      method: buf.readUInt16LE(p + 10),
      crc: buf.readUInt32LE(p + 16),
      compSize: buf.readUInt32LE(p + 20),
      size: buf.readUInt32LE(p + 24),
      localOffset: buf.readUInt32LE(p + 42),
      dir: name.endsWith("/"),
    };
    zip64Extra(buf.subarray(p + 46 + nameLen, p + 46 + nameLen + extraLen), e);
    out.push(e);
    p += 46 + nameLen + extraLen + commentLen;
  }
  return out;
}

async function entryData(buf, e) {
  const l = e.localOffset;
  if (buf.readUInt32LE(l) !== 0x04034b50) throw new Error(`zip: bad local header for ${e.name}`);
  const start = l + 30 + buf.readUInt16LE(l + 26) + buf.readUInt16LE(l + 28);
  const raw = buf.subarray(start, start + e.compSize);
  let data;
  if (e.method === 0) data = raw;
  else if (e.method === 8) data = await inflateRaw(raw);
  else throw new Error(`zip: ${e.name}: method ${e.method} not supported`);
  if (data.length !== e.size || (zlib.crc32(data) >>> 0) !== e.crc) throw new Error(`zip: ${e.name}: size/CRC mismatch`);
  return data;
}

/** Extract the zip in `buf` into `dest` (created; must not exist yet). Returns the number of files written. */
async function extractZip(buf, dest) {
  const entries = listZip(buf);
  const names = entries.map((e) => safeEntryName(e.name));
  const bad = entries.find((_e, i) => names[i] === null);
  if (bad) throw new Error(`zip: unsafe entry name ${JSON.stringify(bad.name)}`);
  fs.mkdirSync(dest, { recursive: false });
  const root = path.resolve(dest);
  let files = 0;
  for (let i = 0; i < entries.length; i++) {
    const e = entries[i];
    const target = path.resolve(root, ...names[i].split("/").filter(Boolean));
    if (target !== root && !target.startsWith(root + path.sep)) throw new Error(`zip: ${e.name} escapes the folder`);
    if (e.dir) {
      fs.mkdirSync(target, { recursive: true });
      continue;
    }
    fs.mkdirSync(path.dirname(target), { recursive: true });
    await fs.promises.writeFile(target, await entryData(buf, e), { flag: "wx" });
    files++;
  }
  return files;
}

module.exports = { listZip, extractZip };
