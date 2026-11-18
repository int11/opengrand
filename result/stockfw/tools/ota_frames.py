# ota_frames.py - the fast upload framing of the ota_loader_v4 stage-1 (REMOTE_PROTOCOL 10.3 "Data v2", cave/include/ota.h
# "fast upload"): v2 data frames with a CRC-32 each, optional LZ tokens, and the reference LZ decoder.  Library only
# (roundtrip_ota_window.py); the app has the same encoder in result/app/src/protocol/ota.ts (lzFrames).
#
# A v2 frame on the wire:
#   F0 7D 'C' 'A' '7' '9' 12 <0x40 | off[19:14]> <off[13:7]> <off[6:0]> <pack7: crc32 u32 · flags u8 · data> F7
# crc32 = zlib CRC-32 of flags || data, off = file offset of the frame's first decoded byte.
# LZ tokens (flags bit 0, LZ4 block layout): token = min(literals, 15) << 4 | min(match - 4, 15); 15 -> more length
# bytes follow (each added, 255 = continue); the literals; then (unless the frame ends) the match distance u16 LE and
# its extra length bytes.  Matches copy byte by byte from `distance` bytes back in the FILE (anything already
# received), so frames are decoded strictly in order and each holds whole tokens.
from __future__ import annotations

import zlib

D2_FLAG = 0x40          # ota.h OTA_D2_FLAG
D2_LZ = 0x01            # OTA_D2_LZ
D2_MAX = 1536           # OTA_D2_MAX (bytes after the flags byte)
D2_WINDOW = 4           # OTA_D2_WINDOW
BC_DATA2, BC_LZ = 0x01, 0x02
MIN_MATCH = 4
MAX_DIST = 0xFFFF
TAG = b"\x43\x41\x37\x39"
OP_DATA = 0x12


def pack7(raw: bytes) -> bytes:
    out = bytearray()
    for i in range(0, len(raw), 7):
        g = raw[i:i + 7]
        out.append(sum(1 << k for k, b in enumerate(g) if b & 0x80))
        out += bytes(b & 0x7F for b in g)
    return bytes(out)


def frame(off: int, flags: int, data: bytes) -> bytes:
    """the whole SysEx (F0 .. F7) of one v2 data frame"""
    body = bytes([flags]) + data
    crc = zlib.crc32(body).to_bytes(4, "little")
    return (b"\xF0\x7D" + TAG + bytes([OP_DATA, D2_FLAG | ((off >> 14) & 0x3F), (off >> 7) & 0x7F, off & 0x7F])
            + pack7(crc + body) + b"\xF7")


def raw_frames(data: bytes, size: int) -> list[tuple[int, int, int, bytes]]:
    """[(off, end, flags, frame data)] of an uncompressed upload in `size`-byte pieces"""
    return [(o, min(o + size, len(data)), 0, data[o:o + size]) for o in range(0, len(data), size)]


def _ext(n: int) -> bytes:
    out = bytearray()
    while n >= 255:
        out.append(255)
        n -= 255
    out.append(n)
    return bytes(out)


def _lits_cost(n: int) -> int:
    return 1 + n + (len(_ext(n - 15)) if n >= 15 else 0)


def lz_frames(data: bytes, budget: int = D2_MAX, chain: int = 48, lazy: bool = True) -> list[tuple[int, int, int, bytes]]:
    """[(off, end, flags, tokens)]: data as LZ frames of at most `budget` token bytes each (greedy, hash chains)."""
    n = len(data)
    head: dict[int, int] = {}
    prev = [-1] * n
    hpos = 0
    frames = []
    pos = 0

    def insert_to(p):
        nonlocal hpos
        while hpos < p and hpos + MIN_MATCH <= n:
            k = int.from_bytes(data[hpos:hpos + MIN_MATCH], "little")
            prev[hpos] = head.get(k, -1)
            head[k] = hpos
            hpos += 1

    def best_at(p):
        if p + MIN_MATCH > n:
            return 0, 0
        insert_to(p)
        j = head.get(int.from_bytes(data[p:p + MIN_MATCH], "little"), -1)
        best, bd, tries = 0, 0, chain
        lim = min(n - p, 0xFFFF)
        while j >= 0 and tries:
            if j < p:
                if p - j > MAX_DIST:
                    break
                ln = 0
                while ln < lim and data[j + ln] == data[p + ln]:
                    ln += 1
                if ln > best:
                    best, bd = ln, p - j
                    if ln == lim:
                        break
                tries -= 1
            j = prev[j]
        return (best, bd) if best >= MIN_MATCH else (0, 0)

    while pos < n:
        start, out, lit0 = pos, bytearray(), pos
        while pos < n:
            ml, md = best_at(pos)
            if ml and lazy and pos + 1 < n and best_at(pos + 1)[0] > ml + 1:
                ml = 0                                       # a longer match one byte later: take a literal
            nl = pos - lit0
            if ml:
                tok = bytes([(min(nl, 15) << 4) | min(ml - MIN_MATCH, 15)])
                piece = tok + (_ext(nl - 15) if nl >= 15 else b"") + data[lit0:pos] + md.to_bytes(2, "little") + \
                    (_ext(ml - MIN_MATCH - 15) if ml - MIN_MATCH >= 15 else b"")
                if len(out) + len(piece) > budget:
                    break
                out += piece
                pos += ml
                lit0 = pos
            else:
                if len(out) + _lits_cost(nl + 1) > budget:
                    break
                pos += 1
        nl = pos - lit0
        while nl and len(out) + _lits_cost(nl) > budget:   # (only when a match did not fit and no room)
            nl -= 1
        if nl or not out:
            if not nl and not out:                           # budget below a 2-byte token: cannot progress
                raise ValueError("LZ budget too small")
            out += bytes([min(nl, 15) << 4]) + (_ext(nl - 15) if nl >= 15 else b"") + data[lit0:lit0 + nl]
        pos = lit0 + nl
        frames.append((start, pos, D2_LZ, bytes(out)))
    return frames


def lz_decode(tokens: bytes, out: bytearray, pos: int, lim: int) -> int | None:
    """reference decoder = cave/src/s1.c lz_decode: tokens decoded into out[pos:], never past lim; new pos or None"""
    i, n = 0, len(tokens)
    while i < n:
        t = tokens[i]
        i += 1
        ln = t >> 4
        if ln == 15:
            while True:
                if i >= n:
                    return None
                b = tokens[i]
                i += 1
                ln += b
                if b != 255:
                    break
        if ln > n - i or ln > lim - pos:
            return None
        out[pos:pos + ln] = tokens[i:i + ln]
        pos += ln
        i += ln
        if i == n:
            break
        if n - i < 2:
            return None
        d = tokens[i] | tokens[i + 1] << 8
        i += 2
        ln = (t & 15) + 4
        if t & 15 == 15:
            while True:
                if i >= n:
                    return None
                b = tokens[i]
                i += 1
                ln += b
                if b != 255:
                    break
        if d == 0 or d > pos or ln > lim - pos:
            return None
        for _ in range(ln):
            out[pos] = out[pos - d]
            pos += 1
    return pos
