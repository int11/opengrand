#!/usr/bin/env python3
"""Pack / inspect OpenGrand update containers (format: include/ca79ng/update.h).

  mkupd.py pack -o CA79NG.UPD --version 1.0.0 main=build/main.bin [dsp=dsp.bin voice=voice.bin ...]
  mkupd.py info CA79NG.UPD

Section types: boot main dsp voice rhythm blemidi panel. Load addresses default
to the original layout (main 0x40060000). Signing (Ed25519) is a separate step
that fills the 64-byte signature slot and sets flags bit 0.
"""
import argparse
import hashlib
import struct
import sys
import time
import zlib

MAGIC = b"C79NGUP1"
TYPES = {"boot": 1, "main": 2, "dsp": 3, "voice": 4, "rhythm": 5, "blemidi": 6, "panel": 7}
LOADS = {"main": 0x40060000}
HDR, ENTRY = 96, 56
MODEL = (0x04, 0x1E, 0x0002)  # CA79


def version_word(s):
    a, b, c = (int(x) for x in s.split("."))
    return a << 16 | b << 8 | c


def pack(args):
    secs = []
    for spec in args.sections:
        name, path = spec.split("=", 1)
        if name not in TYPES:
            sys.exit(f"unknown section type {name}")
        with open(path, "rb") as f:
            secs.append((name, f.read()))
    n = len(secs)
    table_end = HDR + ENTRY * n
    off = (table_end + 4 + 15) & ~15
    entries, payload = b"", b""
    for name, data in secs:
        pad = (-(off + len(payload)) % 16) if payload else 0
        payload += b"\0" * pad
        start = off + len(payload)
        entries += struct.pack(">6I", TYPES[name], 0, start, len(data), LOADS.get(name, 0), 0) + hashlib.sha256(data).digest()
        payload += data
    total = off + len(payload)
    head = MAGIC + struct.pack(">HHBBHIIII", 1, n, MODEL[0], MODEL[1], MODEL[2], version_word(args.version),
                               int(time.time()), total, 0)
    crc = zlib.crc32(head + b"\0" * 64 + entries) & 0xFFFFFFFF
    blob = head + b"\0" * 64 + entries + struct.pack(">I", crc)
    blob += b"\0" * (off - len(blob)) + payload
    assert len(blob) == total
    with open(args.output, "wb") as f:
        f.write(blob)
    print(f"{args.output}: {total} bytes, {n} section(s), version {args.version}")


def info(args):
    with open(args.file, "rb") as f:
        b = f.read()
    if b[:8] != MAGIC:
        sys.exit("not a OpenGrand update container")
    hv, n, g, m, sub, ver, bt, total, flags = struct.unpack(">HHBBHIIII", b[8:32])
    table_end = HDR + ENTRY * n
    crc_ok = zlib.crc32(b[:32] + b"\0" * 64 + b[HDR:table_end]) & 0xFFFFFFFF == struct.unpack(">I", b[table_end:table_end + 4])[0]
    print(f"model {g:02X} {m:02X} sub {sub:04X}  version {ver >> 16}.{ver >> 8 & 255}.{ver & 255}  "
          f"built {time.strftime('%Y-%m-%d %H:%M', time.gmtime(bt))}  size {total} (file {len(b)})  "
          f"signed={'yes' if flags & 1 else 'no'}  header crc {'ok' if crc_ok else 'BAD'}")
    inv = {v: k for k, v in TYPES.items()}
    for i in range(n):
        e = b[HDR + i * ENTRY:HDR + (i + 1) * ENTRY]
        t, fl, off, ln, load, _ = struct.unpack(">6I", e[:24])
        ok = hashlib.sha256(b[off:off + ln]).digest() == e[24:56]
        print(f"  {inv.get(t, t):8} off 0x{off:06X} len {ln:8} load 0x{load:08X} sha256 {'ok' if ok else 'BAD'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("sections", nargs="+")
    i = sub.add_parser("info")
    i.add_argument("file")
    a = ap.parse_args()
    pack(a) if a.cmd == "pack" else info(a)


if __name__ == "__main__":
    main()
