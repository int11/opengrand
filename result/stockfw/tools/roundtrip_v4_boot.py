# roundtrip_v4_boot.py - emulator verification of the ota_loader_v4 loader itself: stage-0's boot choice, rollback,
# the stage-1 Bluetooth update with its interlocks, the frozen NOR guard, power loss at every NOR step, RESELECT.
#
# Machine: roundtrip_ota.Piano on the real ota_loader_v4 image (stage-0 alone, patches/ota_loader_v4.json) with the NOR
# model filled per case: loader slots A (0x1A0000) / B (0x1E0000) and main slots A / B built from the real signed
# build/ota_stage1.c79 and build/ota_main.c79, or variants of them re-signed with the local key (tools/ota_pack.py) so
# that exactly one property is wrong.  The stock NOR routines run unmodified (only stage-0 calls them).  "Reboot" = a
# fresh emulator on the same image with the NOR array carried over; "crash" = the call stops (undefined instruction /
# endless loop), which on the piano means: the user switches it off and on.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_v4_boot.py [-q] [--shard i/n]
#
# Checks (numbered as printed):
#   1  static: ota_loader_v4.SYS differs from stock only at the declared spots (2 hooks, loader copy end 0x40176000,
#      stage-0, root key), revert = stock; stage-0 within its budget (Ed25519 headroom printed), stage-1 within the
#      40 KB window; the frozen tables (s1_entry, s0_svc v2 with nor_enable, s0_boot size 76, the NOR range table
#      s0_nor_ranges: 5 entries, none overlapping BootProgram or another never-writable partition) as include/s0.h
#      says; no stock NOR writer and no
#      reference to stage-0's hook sites 0x40143AE0 / 0x40147E1C in stage-1 or any slot payload; stage-1's allow-list
#      words = the stock words
#   2  stage-0 choice matrix: loader slot A x B content (empty, unconfirmed with 0 / 1 / 2 / 3 tries, confirmed, rejected,
#      CRC / MAC / key id / type / load / length / format 1 / signature alg 2 / entry table / init refusal) x seq order:
#      the slot that runs, the reason and the NOR writes (exactly one try mark per unconfirmed candidate tried, before its
#      load; nothing for confirmed ones) = a Python model of the frozen rules
#   3  escape keys: E1 (mailbox / MainProgram copy / key state) -> pure stock, no NOR op; E2 (each source) -> the
#      lower-seq loader with SAFE, stage-1 starts no main and refuses applying one (rc 24) but takes an upload; flash
#      busy at boot 0 / 3 s / 15 s; busy only while marking -> confirmed loaders only (no mark); try mark not
#      written (writer fails) -> that candidate skipped
#   4  stage-1 Bluetooth update + interlocks: upload into the running loader slot -> 4; loader file to a main slot /
#      main file to a loader slot -> 11; reboot onto the new loader (try 1); while it is unconfirmed a loader upload ->
#      22 and every reject that would leave no confirmed loader -> 23 (no NOR op); confirm op (wrong target -> 4) ->
#      then both are allowed; an incompatible main (format 2, min API 2) -> rc 25 at commit and never booted (reason 8),
#      a compatible upload boots
#   5  rollback: a broken new stage-1 that (a) traps in init, (b) never answers SysEx, (c) hangs in tick -> 3 boots, then
#      the confirmed loader; no USB
#   6  corrupted writes: one bit flipped in the state magic / header / payload / signature of either loader slot -> the
#      other loader runs
#   7  power loss at every NOR operation of a loader commit (erase, every page, seq, state version, magic) -> the
#      running loader boots and its sector is untouched; during the try mark and during the loader confirm write (bits
#      untouched or half cleared) -> the next boot is consistent
#   8  RESELECT (apply target 4): the main is dropped, every hook restored, stage-0 selects again (new loader with a try
#      mark, reason RESELECT, NOR gates OFF again) and the new stage-1 boots the main; a SysEx call in flight in the old
#      stage-1 holds the swap
#   9  stage-0's NOR guard (called from stage-1): outside the slot / kvstore ranges, misaligned, page-crossing, and erase /
#      page program of the running loader sector -> refused (3) before the stock routine, guard counter; the allowed cases
#      reach it; caller-scope edges (stage-0 + window = slot rights, anything else = kvstore only).  Main scope: the running
#      main's calls (from main code) to erase / program loader A / B, main A / B, BootProgram, MainProgram -> 3, slots
#      untouched; kvstore -> allowed; then a stage-1 loader upload with that main running still commits and boots
#  11  NOR gates (stage-0 v2): all OFF at power-on (writes from stage-1 / the main -> 3); nor_enable from the main or
#      with an unknown id -> 3; stage-1 enables gate 0 -> its writes allowed, the main's refused unless main_writable;
#      no revoke (masks only grow); with every gate ON + main_writable, BootProgram / MainProgram / voice-DB tail /
#      UserMemory / TouchAdjust / config A / RecorderData / beyond 16 MB still -> 3; an 8 MB part (0x800000 mirrors
#      0x000000): enable(2) -> 4 (alias); a gate equal to BootProgram's start -> 4; flash busy -> 1; next power-on OFF
#  10  stores of a boot (stage-0 loads stage-1, stage-1 boots a main) and of a loader commit: only stage-0 .data, the
#      window, the main areas, the stack, the NORTi error word, the busy flag
from __future__ import annotations

import pathlib
import struct
import sys
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

ro = rtlib.load("roundtrip_ota")
mm = rtlib.load("make_phase2_manifest")
bi = rtlib.load("build_images")
rh = rtlib.load("roundtrip_ota_apply")
op, pa, images, rp, sb = ro.op, ro.pa, ro.images, ro.rp, ro.sb
STOCKFW = rtlib.STOCKFW
KEY = ro.KEY
QUIET = "-q" in sys.argv
le = rtlib.le

NOR_OF = {0x00: 0x180000, 0x01: 0x1C0000, 0x10: 0x1A0000, 0x11: 0x1E0000}   # target byte -> NOR offset
S_MARK, S_RUN, S_STOCK = 3, 6, 7
R = dict(slot=1, none=2, escape=3, failed=4, busy=5, safe=6, reselect=7)
OP_CONFIRM = 0x0F
WIN = 0x40176000
KEY_STATE = 0x413CAE98


def check(*a, **kw) -> bool:
    return bool(rtlib.check(*a, **kw))


# ---- inputs ---------------------------------------------------------------------------------------------------
LOADER = rtlib.loader()                                  # merged symbols (the image there is seeded; not used)
SYMS = LOADER.syms
MANIFEST = pa.load_manifest(STOCKFW / "patches/ota_loader_v4.json")
STOCK = pa.stock_bytes()
IMAGE = pa.apply_patches(STOCK, MANIFEST)                # stage-0 alone, every slot erased
S1 = (STOCKFW / "build/ota_stage1.c79").read_bytes()
MAIN = (STOCKFW / "build/ota_main.c79").read_bytes()
S1BIN = S1[64:]
S1SYM = rtlib.stage1_symbols()


def resign(c79: bytes, hdr_patch=None, pay_patch=None, *, key=KEY, crc_ok=True) -> bytes:
    """c79 with header bytes / payload bytes changed, CRC and HMAC recomputed (the change is the only defect)."""
    h, pay = bytearray(c79[:64]), bytearray(c79[64:])
    for off, b in (pay_patch or {}).items():
        pay[off:off + len(b)] = b
    for off, b in (hdr_patch or {}).items():
        h[off:off + len(b)] = b
    if crc_ok:
        h[0x14:0x18] = struct.pack("<I", zlib.crc32(bytes(pay)))
    h[0x24:0x28] = struct.pack("<I", op.key_id(key))
    h[0x30:0x40] = op.mac(key, bytes(h[:0x30]), bytes(pay))
    return bytes(h) + bytes(pay)


def s1_patch(fn: str, word: int) -> dict:
    """payload patch: the first instruction of stage-1 function fn replaced by `word` (ARM)."""
    return {S1SYM[fn] - WIN: struct.pack("<I", word)}


UDF, BX_LR, B_SELF, MOV_R0_1 = 0xE7F000F0, 0xE12FFF1E, 0xEAFFFFFE, 0xE3A00001

# loader image variants: name -> (c79, good)
big = bytearray(S1BIN) + bytes(op.WINDOW_SIZE + 4 - len(S1BIN))
IMG = {
    "good": (S1, True),
    "crc": (S1[:64] + bytes([S1[64] ^ 0]) + S1[65:100] + bytes([S1[100] ^ 0x10]) + S1[101:], False),
    "mac": (S1[:0x30] + bytes([S1[0x30] ^ 1]) + S1[0x31:], False),
    "keyid": (resign(S1, key=bytes(range(32))), False),
    "type": (resign(S1, {0x28: b"\x01"}), False),
    "load": (resign(S1, {0x0C: struct.pack("<I", WIN + 0x1000)}), False),
    "len": (resign(op.pack_loader(S1BIN, KEY, 1)[:64] + bytes(big), {0x08: struct.pack("<I", len(big))}), False),
    "fmt1": (resign(S1, {0x04: b"\x01\x00"}), False),
    "alg2": (resign(S1, {0x29: b"\x02"}), False),
    "entry": (resign(S1, None, {8: struct.pack("<I", 0x40180000)}), False),      # init pointer outside the window
}
# init refuses: s1_init starts `mov r0, #1; bx lr`
INIT1 = bytearray(S1)
INIT1[64 + S1SYM["s1_init"] - WIN:64 + S1SYM["s1_init"] - WIN + 8] = struct.pack("<II", MOV_R0_1, BX_LR)
IMG["init1"] = (resign(bytes(INIT1)), False)
STATES = {"empty": None, "u0": (False, 0), "u1": (False, 1), "u2": (False, 2), "u3": (False, 3), "conf": (True, 0),
          "rej": (True, 0)}


def sector(target: int, c79: bytes | None, seq: int, *, confirmed=False, tries=0, rejected=False) -> bytes:
    if c79 is None:
        return b"\xff" * 0x20000
    st = bytearray(mm.seed_blob(images.Seed(target, "-", seq, confirmed), c79))
    for k in range(tries):
        st[0x10 + 2 * k:0x12 + 2 * k] = b"\0\0"
    if rejected:
        st[0xA:0xC] = b"\0\0"
    return bytes(st) + b"\xff" * (0x20000 - len(st))


_BASE = None


def nor_with(**slots) -> bytes:
    """the NOR array of this image with slot sectors replaced: nor_with(la=bytes, lb=..., ma=..., mb=...)"""
    global _BASE
    if _BASE is None:
        pn = ro.Piano(IMAGE, SYMS)
        _BASE = bytes(pn.nor.mem)
        pn.close()
    mem = bytearray(_BASE)
    for k, t in (("ma", 0x00), ("mb", 0x01), ("la", 0x10), ("lb", 0x11)):
        if k in slots:
            mem[NOR_OF[t]:NOR_OF[t] + 0x20000] = slots[k]
    return bytes(mem)


def piano(mem, **kw):
    return ro.Piano(IMAGE, SYMS, nor_mem=mem, **kw)


def s0(pn) -> dict:
    b = pn.emu.read_bytes(SYMS["s0_boot"], 76)
    return dict(slot=b[9], reason=b[10], flags=b[11], request=b[12], reselects=b[15], key_id=le(b, 20),
                lslot=[(le(b, 36 + 8 * i), b[40 + 8 * i], b[41 + 8 * i]) for i in range(2)],
                esc=b[64], guard=le(b, 68, 2), defers=le(b, 70, 2), gates=b[72], gates_main=b[73],
                state=pn.emu.read_bytes(SYMS["s0_state"], 1)[0])


def boot0(pn, limit=2000, before=None):
    """ticks until stage-0 has decided (stage-1 running or stock): (s0 dict, ticks)"""
    for i in range(limit):
        if before:
            before(pn, i)
        pn.tick()
        st = s0(pn)
        if st["state"] in (S_RUN, S_STOCK):
            return st, i + 1
    return s0(pn), limit


def marks(pn) -> list[int]:
    """NOR offsets of the halfword programs (try marks) of this machine so far"""
    return [o.offset for o in pn.nor.ops if o.kind == "program"]


# ---- the model of the frozen stage-0 rules -----------------------------------------------------------------------
def model(a, b, *, safe=False):
    """a, b = (state name, image name, seq) of loader slots A / B -> (slot that runs or None, reason, [mark offsets])"""
    cands = []
    for i, (stn, imn, seq) in enumerate((a, b)):
        if stn == "empty":
            continue
        conf, tries = STATES[stn]
        if stn == "rej" or (not conf and tries >= 3):
            continue
        cands.append((i, seq, conf, tries, IMG[imn][1]))
    cands.sort(key=lambda c: c[1], reverse=not safe)
    writes = []
    for i, _seq, conf, tries, good in cands:
        if not conf:
            writes.append(NOR_OF[0x10 + i] + 0x10 + 2 * tries)
        if good:
            return i, R["safe"] if safe else R["slot"], writes
    return None, R["failed"] if cands else R["none"], writes


def lsector(i, stn, imn, seq):
    if stn == "empty":
        return sector(0x10 + i, None, seq)
    conf, tries = STATES[stn]
    return sector(0x10 + i, IMG[imn][0], seq, confirmed=conf, tries=tries, rejected=stn == "rej")


def run_case(a, b, *, safe=False, mailbox=None):
    pn = piano(nor_with(la=lsector(0, *a), lb=lsector(1, *b)), mailbox=mailbox)
    st, n = boot0(pn)
    got = (st["slot"] if st["state"] == S_RUN else None, st["reason"], marks(pn))
    good_window = st["state"] != S_RUN or pn.emu.read_bytes(WIN, 0x400) == S1BIN[:0x400]
    pn.close()
    return got, model(a, b, safe=safe), good_window


# ---- 1. static ----------------------------------------------------------------------------------------------------
def data_start() -> int:
    """first address of stage-0's .data (below it: code + const, never stored to - check 10)"""
    return min(lo for lo, _hi, sec, kind, _o in sb.layout("s0") if sec == ".cave" and kind == "data")


def scan_refs(elf, addrs):
    """branches to / literal words of the given addresses in an ELF's .cave (ARM or Thumb, +1 allowed)"""
    od = mm.NM.with_name("arm-none-eabi-objdump")
    out = rtlib.subprocess.run([str(od), "-D", "-j", ".cave", str(elf)], capture_output=True, text=True, check=True).stdout
    hits = []
    import re
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+([0-9a-f]{8}|[0-9a-f]{4}(?: [0-9a-f]{4})?)\s+(\S+)\s*(.*)", line)
        if not m:
            continue
        vals = ([int(m[2], 16)] if len(m[2]) == 8 else []) + \
            ([int(t, 16) for t in re.findall(r"\b([0-9a-f]{8})\b", m[4])] if m[3].startswith("b") else [])
        hits += [(int(m[1], 16), v) for v in vals if (v & ~1) in addrs]
    return hits


def static_checks():
    ok = True
    built = (STOCKFW / "build/ota_loader_v4.SYS").read_bytes()
    blob = (STOCKFW / "cave/build/cave_s0.bin").read_bytes()
    ids = sorted(p["id"] for p in MANIFEST["patches"])
    changed = pa.check_only_declared_diffs(STOCK, IMAGE, MANIFEST)
    ok &= check("1 ota_loader_v4.SYS = stock + exactly the declared patches (stage-0 blob, loader copy end 0x40176000, root "
                "key, the 2 hooks), revert = stock, = build/ota_loader_v4.SYS",
                ids == ["cave", "hook_s0_h1", "hook_s0_tick", "loader_end", "ota_key"] and built == IMAGE
                and IMAGE[0x4B3EC:0x4B3F0] == struct.pack("<I", 0x40176000)
                and pa.sha256(pa.apply_patches(IMAGE, MANIFEST, reverse=True)) == pa.STOCK_SHA256,
                f"{len(changed)} bytes, patches {ids}", quiet=QUIET)
    budget, used = 0x40175FE0 - 0x40173A14, len(blob)
    ok &= check(f"1 stage-0 {used} B of its frozen {budget} B ({budget - used} B free - an Ed25519 verifier would replace "
                f"the HMAC/SHA-256 part and must fit there); stage-1 {len(S1BIN)} B of the 40960 B window",
                used <= budget and SYMS["cave_end"] <= 0x40175FE0 and len(S1BIN) <= op.WINDOW_SIZE)
    e = S1BIN[:24]
    ok &= check("1 stage-1 entry table at the window base: 'C79L', ABI 1, size 24, init / sysex / tick / release inside "
                "the image, 4-aligned (ARM)",
                e[:4] == b"C79L" and le(e, 4, 2) == 1 and le(e, 6, 2) == 24
                and all(WIN <= le(e, k) < WIN + len(S1BIN) and le(e, k) % 4 == 0 for k in (8, 12, 16, 20)), e.hex())
    sv = SYMS["s0_svc"] - 0x40173A14
    svc = blob[sv:sv + 32]
    ok &= check("1 stage-0 service table s0_svc: 'C79N', version 2, size 32, six entries inside stage-0 (nor_erase / "
                "nor_prog / nor_prog_hw ARM, cache_sync ARM, sig_check Thumb, nor_enable ARM - appended, the v1 five at "
                "their offsets)",
                svc[:4] == b"C79N" and le(svc, 4, 2) == 2 and le(svc, 6, 2) == 32
                and all(0x40173A14 <= le(svc, k) & ~1 < SYMS["cave_end"] for k in range(8, 32, 4))
                and [le(svc, k) & 1 for k in range(8, 32, 4)] == [0, 0, 0, 0, 1, 0]
                and le(svc, 28) == SYMS["nor_enable"], svc.hex())
    bt = SYMS["s0_boot"] - 0x40173A14
    b0 = blob[bt:bt + 76]
    ok &= check("1 boot block s0_boot: 'C79B', version 1, size 76 (v1 72 + nor_gates / nor_gates_main appended, both 0), "
                "stage-0 v2, alg HMAC, 3 tries, key pointer = 0x40175FE0, window 0x40176000 / 0xA000, service pointer = "
                "s0_svc",
                b0[:4] == b"C79B" and le(b0, 4, 2) == 1 and le(b0, 6, 2) == 76 and b0[8] == 2 and b0[13] == 1
                and b0[14] == 3 and le(b0, 16) == 0x40175FE0 and le(b0, 24) == WIN and le(b0, 28) == 0xA000
                and le(b0, 32) == SYMS["s0_svc"] and b0[72:76] == bytes(4), b0[:36].hex())
    # the frozen NOR range table: exactly the five entries of include/s0.h, none reaching a never-writable partition
    rt = SYMS["s0_nor_ranges"] - 0x40173A14
    tbl = [(le(blob, rt + 12 * i), le(blob, rt + 12 * i + 4), blob[rt + 12 * i + 8], blob[rt + 12 * i + 9])
           for i in range(5)]
    never = [(0x60000000, 0x60060000, "BootProgram"), (0x60060000, 0x60180000, "MainProgram"),
             (0x60200000, 0x60400000, "MainProgram (voice DB incl. tail 0x602E0000, rhythm, DSP)"),
             (0x60400000, 0x60440000, "UserMemory"), (0x60440000, 0x60460000, "TouchAdjust"),
             (0x60460000, 0x60480000, "config A"), (0x60500000, 0x60600000, "RecorderData")]
    hit = [(hex(lo), nm) for lo, hi, _g, _f in tbl for nlo, nhi, nm in never if lo < nhi and nlo < hi]
    ok &= check("1 frozen NOR range table s0_nor_ranges (5 x 12 B, const): slots 0x60180000-0x601FFFFF always / stage-1 "
                "only, kvstore 0x604C0000-0x604FFFFF always + main, gate 0 0x60480000-0x604BFFFF, gate 1 0x60600000-"
                "0x607FFFFF, gate 2 0x60800000-0x60FFFFFF (bits 1 / 2 / 4, no fixed main right); no entry overlaps "
                "BootProgram, MainProgram (incl. the voice-DB tail 0x602E0000), UserMemory, TouchAdjust, config A or "
                "RecorderData - the table is in .cave code (read-only to every caller), so this holds after any enable",
                tbl == [(0x60180000, 0x60200000, 0, 0), (0x604C0000, 0x60500000, 0, 1), (0x60480000, 0x604C0000, 1, 0),
                        (0x60600000, 0x60800000, 2, 0), (0x60800000, 0x61000000, 4, 0)]
                and not hit and all(lo % 0x20000 == 0 and hi % 0x20000 == 0 and lo < hi for lo, hi, _g, _f in tbl)
                and 0x40173A14 <= SYMS["s0_nor_ranges"] < data_start(), f"{[(hex(a), hex(b), g, f) for a, b, g, f in tbl]} {hit}")
    elves = [STOCKFW / "cave/build/loader_s1.elf"] + [STOCKFW / f"cave/build/payload_{p.variant}.elf" for p in
                                                       (*images.OTA_PAYLOADS.values(), *images.TEST_PAYLOADS.values())]
    wr = {e.name: bi.writer_refs(e) for e in elves}
    ok &= check(f"1 no stock NOR writer / busy flag setter reference (branch or literal) in stage-1 or any of the "
                f"{len(elves) - 1} slot payloads", not any(wr.values()), str({k: v[:2] for k, v in wr.items() if v}))
    hk = {e.name: scan_refs(e, {0x40143AE0, 0x40147E1C}) for e in elves}
    ok &= check("1 stage-1 and every slot payload never name stage-0's hook sites 0x40143AE0 / 0x40147E1C (no branch, no "
                "literal - a runtime hook there would cut stage-0 off)", not any(hk.values()),
                str({k: v[:2] for k, v in hk.items() if v}))
    tbl = S1SYM["hk_sites"] - WIN
    sites = [(le(S1BIN, tbl + 12 * i), le(S1BIN, tbl + 12 * i + 4)) for i in range(17)]
    ok &= check("1 stage-1's 17-site allow-list: every original word = the stock image's word there, none is a stage-0 site",
                all(le(STOCK, a - 0x40060000) == w for a, w in sites)
                and not any(a in (0x40143AE0, 0x40147E1C) for a, _w in sites), str([hex(a) for a, _ in sites[:3]]))
    return ok


# ---- 2. matrix -------------------------------------------------------------------------------------------------------
def matrix(half):
    ok = True
    av = [(s, "good") for s in ("empty", "u0", "u1", "u2", "u3", "conf", "rej")] + \
         [("conf", i) for i in IMG if i != "good"] + [("u0", "crc"), ("u1", "init1"), ("u2", "entry")]
    bv = [("empty", "good"), ("u0", "good"), ("u2", "good"), ("u3", "good"), ("conf", "good"), ("rej", "good"),
          ("conf", "crc"), ("u0", "init1"), ("conf", "alg2")]
    av = av[:10] if half == 0 else av[10:]
    bad, n = [], 0
    for sa, ia in av:
        for sb_, ib in bv:
            for sqa, sqb in ((1, 2), (2, 1)):
                got, want, win = run_case((sa, ia, sqa), (sb_, ib, sqb))
                n += 1
                if got != want or not win:
                    bad.append(((sa, ia, sqa), (sb_, ib, sqb), got, want))
    ok &= check(f"2 stage-0 choice matrix part {half + 1}/2: {n} cases (loader A {len(av)} x loader B {len(bv)} x seq "
                "order) - running slot, "
                "reason and the NOR writes (try marks before the load, one per unconfirmed candidate tried) = the model",
                not bad, f"{len(bad)} differ: {bad[:2]}")
    return ok


# ---- 3. escape, busy, mark failure -----------------------------------------------------------------------------------
def escapes():
    ok = True
    both = dict(la=sector(0x10, S1, 1, confirmed=True), lb=sector(0x11, S1, 2, confirmed=True),
                ma=sector(0x00, MAIN, 1))

    def with_keys(pn, keys):
        arr = bytearray(88)
        for k in keys:
            arr[k] = 0xE0
        pn.emu.write_bytes(KEY_STATE, bytes(arr))

    res = []
    for label, kw, keys in (("mailbox", dict(mailbox=(0x55AAA55A, 0x40000007)), ()),
                            ("copy", dict(sdram_code=0x40000007), ()), ("key state", {}, (0, 1, 2))):
        pn = piano(nor_with(**both), **kw)
        with_keys(pn, keys)
        mem0 = bytes(pn.nor.mem)
        st, _ = boot0(pn)
        res.append((label, st["state"] == S_STOCK and st["reason"] == R["escape"] and pn.status() is None
                    and bytes(pn.nor.mem) == mem0 and not pn.nor.ops, st["esc"]))
        pn.close()
    ok &= check("3 E1 (A0+A#0+B0) via the mailbox / MainProgram's copy / the stock key state -> pure stock: no stage-1, "
                "nothing answers 0x7D, no NOR op", all(r[1] for r in res), str(res))
    res = []
    for label, kw, keys in (("mailbox", dict(mailbox=(0x55AAA55A, 0x40000003)), ()),
                            ("copy", dict(sdram_code=0x40000003), ()), ("key state", {}, (0, 1))):
        pn = piano(nor_with(**both), **kw)
        with_keys(pn, keys)
        st, _ = boot0(pn)
        st1, _n = pn.boot()
        res.append((label, st["slot"] == 0 and st["reason"] == R["safe"] and st["flags"] & 1 == 1
                    and st1["active"] == ro.ACT_BUILTIN and st1["reason"] == 7 and not marks(pn), st["esc"]))
        if label == "key state":
            safe_pn = pn
        else:
            pn.close()
    ok &= check("3 E2 (A0+A#0) via each source -> SAFE: the LOWER-seq loader (A, seq 1 < B seq 2), stage-1 starts no main "
                "although main A is valid (reason 7), no NOR op", all(r[1] for r in res), str(res))
    pn = safe_pn
    rc = rh.apply(pn, rh.T_A)
    rc3 = rh.apply(pn, rh.T_NEWEST)
    res_up = pn.upload(1, MAIN, seq=2)
    st, _ = pn.run_job()
    ok &= check("3 in SAFE: apply main A / newest refused (rc 24), a main upload to slot B is taken and committed",
                rc == 24 and rc3 == 24 and res_up == ("commit", 0) and st["job"] == ro.J["done"], f"{rc} {rc3} {res_up}")
    pn.close()
    cases = [("only B", dict(la=sector(0x10, None, 1), lb=sector(0x11, S1, 2, confirmed=True)), 1),
             ("A exhausted", dict(la=sector(0x10, S1, 1, tries=3), lb=sector(0x11, S1, 2, confirmed=True)), 1),
             ("A unconfirmed", dict(la=sector(0x10, S1, 1), lb=sector(0x11, S1, 2, confirmed=True)), 0)]
    res = []
    for label, slots, want in cases:
        pn = piano(nor_with(**slots), mailbox=(0x55AAA55A, 0x40000003))
        st, _ = boot0(pn)
        res.append((label, st["slot"] == want and st["reason"] == R["safe"]))
        pn.close()
    ok &= check("3 E2 with one loader left / the older one exhausted -> the bootable one; the older one unconfirmed -> it "
                "(with a try mark)", all(r[1] for r in res), str(res))

    one = dict(la=sector(0x10, S1, 1, confirmed=True))
    res = []
    for label, ticks, want_state, want_reason in (("none", 0, S_RUN, R["slot"]), ("3 s", 375, S_RUN, R["slot"]),
                                                  ("15 s", 1875, S_STOCK, R["busy"])):
        pn = piano(nor_with(**one))

        def busy(p, i, _t=ticks):
            p.emu.write_bytes(ro.BUSY_FLAG, b"\x01\0\0\0" if i < _t else b"\0\0\0\0")
        st, n = boot0(pn, 2500, busy)
        res.append((label, st["state"] == want_state and st["reason"] == want_reason and not pn.nor.ops, n, st["defers"]))
        pn.close()
    ok &= check("3 a stock writer holds the chip at boot for 0 / 3 s / 15 s -> stage-0 waits (deferred passes counted), "
                "then boots; beyond ~10 s -> stock for this boot (reason busy)", all(r[1] for r in res), str(res))
    pn = piano(nor_with(la=sector(0x10, S1, 1, confirmed=True), lb=sector(0x11, S1, 2)))

    pn.emu.hook(SYMS["nor_prog_hw"], lambda e: 1)   # stage-0's halfword service: "a stock writer owns the chip"
    st, n = boot0(pn, 3000)
    ok &= check("3 busy only while the try mark of the unconfirmed newer loader B is due -> after ~10 s B is skipped "
                "without a mark (flag NOMARK) and the confirmed loader A runs",
                st["slot"] == 0 and st["flags"] & 2 == 2 and not pn.nor.ops and st["lslot"][1][1] & 0x10 == 0x10, f"{st} {n}")
    pn.close()
    pn = piano(nor_with(la=sector(0x10, S1, 1, confirmed=True), lb=sector(0x11, S1, 2)))
    pn.emu.hook(ro.NOR_HW, lambda e: 2)             # the halfword writer reports a failure, writes nothing
    st, _ = boot0(pn)
    ok &= check("3 the try mark cannot be written (stock writer fails) -> the unconfirmed loader B is skipped (flag bad), "
                "never run without a recorded try; confirmed A runs",
                st["slot"] == 0 and st["lslot"][1][1] & 0x10 == 0x10 and st["lslot"][1][2] == 0, str(st["lslot"]))
    pn.close()
    return ok


# ---- 4. stage-1 Bluetooth update + interlocks ---------------------------------------------------------------------------
def status3(pn):
    r = pn.reply(2, rp.req(ro.OP_STATUS), ro.OP_STATUS)
    st = pn.status()
    st.update(lslots=[dict(seq=le(r, 72 + 6 * i), flags=r[76 + 6 * i], tries=r[77 + 6 * i]) for i in range(2)], lrun=r[84],
              s0ver=r[87], gates=r[111])
    return st


def interlocks():
    ok = True
    base = dict(la=sector(0x10, S1, 1, confirmed=True), ma=sector(0x00, MAIN, 1, confirmed=True))
    pn = piano(nor_with(**base))
    pn.boot()
    r_run = pn.begin(0x10, S1, 2)
    n0 = len(pn.nor.ops)
    up_main_ldr = pn.upload(0x11, MAIN, seq=2)          # 106 KB > the loader maximum: refused at Begin
    small_main = rtlib.payload("ota_hookdemo3").data     # a small main: fits the loader length, wrong header
    up_small = pn.upload(0x11, small_main, seq=2)
    st_a, _ = pn.run_job()
    up_ldr_main = pn.upload(1, S1, seq=2)
    st_b, _ = pn.run_job()
    ok &= check("4 Begin into the running loader slot A -> 4; a main file to loader slot B: too long -> 5 at Begin, a small "
                "one -> commit error 11 (header); a loader file into main slot B -> 11; flash never touched",
                r_run == 4 and up_main_ldr == ("begin", 5) and up_small == ("commit", 0) and st_a["err"] == 11
                and up_ldr_main == ("commit", 0) and st_b["err"] == 11 and len(pn.nor.ops) == n0,
                f"{r_run} {up_main_ldr} {up_small}/{st_a['err']} {up_ldr_main}/{st_b['err']}")
    res = pn.upload(0x11, S1, seq=2)
    st, _ = pn.run_job()
    mem = bytes(pn.nor.mem)
    pn.close()
    pn = piano(mem)
    pn.boot()
    s3 = status3(pn)
    ok &= check("4 stage-1 uploaded to loader slot B (seq 2) -> reboot -> loader B runs unconfirmed, one try",
                res == ("commit", 0) and st["job"] == ro.J["done"] and s3["lrun"] == 1 and s3["lslots"][1]["tries"] == 1
                and not s3["lslots"][1]["flags"] & 2, str(s3["lslots"]))
    n0 = len(pn.nor.ops)
    r1 = pn.begin(0x10, S1, 3)
    rj = [pn.auth_op(ro.OP_REJECT, bytes([m]))[0] for m in (0x10, 0x30)]
    pn.tick(3)
    ok &= check("4 while the running loader B is unconfirmed: a loader upload (to A) -> 22; rejecting A (the only confirmed "
                "loader) or both -> 23; no NOR op", r1 == 22 and rj == [23, 23] and len(pn.nor.ops) == n0
                and pn.status()["rej"] == 0, f"{r1} {rj}")
    rc_wrong = pn.auth_op(OP_CONFIRM, bytes([0x10]))[0]
    rc = pn.auth_op(OP_CONFIRM, bytes([0x11]))[0]
    pn.tick(2)
    s3 = status3(pn)
    r2 = pn.begin(0x10, S1, 3)
    pn.status()
    rj2 = pn.auth_op(ro.OP_REJECT, bytes([0x10]))[0]
    pn.tick(3)
    s3b = status3(pn)
    ok &= check("4 confirm op 0x0F: wrong target (A, not running) -> 4; running B -> confirmed (+8 = 0000); now a loader "
                "upload to A begins (0) and rejecting A (B confirmed remains) is done",
                rc_wrong == 4 and rc == 0 and s3["lslots"][1]["flags"] & 2 == 2 and r2 == 0 and rj2 == 0
                and s3b["lslots"][0]["flags"] & 4 == 4, f"{rc_wrong} {rc} {r2} {rj2}")
    pn.close()
    # an incompatible main: format 2, type 1, min stage-1 API 2
    inc = resign(MAIN, {0x04: b"\x02\x00", 0x28: b"\x01", 0x29: b"\x01", 0x2A: b"\x02"})
    pn = piano(nor_with(la=sector(0x10, S1, 1, confirmed=True), ma=sector(0x00, inc, 1)))
    st, _ = pn.boot()
    res = pn.upload(1, inc, seq=2)
    stj, _ = pn.run_job()
    res2 = pn.upload(1, MAIN, seq=2)
    stj2, _ = pn.run_job()
    mem = bytes(pn.nor.mem)
    pn.close()
    pn = piano(mem)
    st2, _ = pn.boot()
    ok &= check("4 a main that needs stage-1 API 2 (format 2 type 1): never booted (no main, reason 8 = no compatible main), "
                "its upload fails the commit with 25; a compatible upload to B commits and boots",
                st["active"] == ro.ACT_BUILTIN and st["reason"] == 8 and stj["err"] == 25 and res2 == ("commit", 0)
                and stj2["job"] == ro.J["done"] and st2["active"] == ro.ACT_B, f"{st['reason']} {stj['err']} {st2['active']}")
    pn.close()
    return ok


# ---- 5. rollback from a broken stage-1 ------------------------------------------------------------------------------------
def boot_crash(mem, ticks=120):
    """one power-on: stage-0 + whatever stage-1 does for `ticks` passes; a stopped call = the piano hangs (crash)"""
    pn = piano(mem)
    crashed = False
    for _ in range(ticks):
        res = pn.emu.call(SYMS["cave_tick"], max_insns=3_000_000)
        if not res.ok:
            crashed = True
            break
    st = s0(pn)
    answers = not crashed and pn.status() is not None
    mem = bytes(pn.nor.mem)
    pn.close()
    return st, crashed, answers, mem


def rollback():
    ok = True
    for label, patch in (("traps in init", s1_patch("s1_init", UDF)), ("never answers SysEx", s1_patch("s1_sysex", BX_LR)),
                         ("hangs in tick", s1_patch("s1_tick", B_SELF))):
        broken = resign(S1, {0x2C: b"\x02\0\0\0"}, patch)
        mem = nor_with(la=sector(0x10, S1, 1, confirmed=True), lb=sector(0x11, broken, 2), ma=sector(0x00, MAIN, 1))
        seen = []
        for _ in range(4):
            st, crashed, answers, mem = boot_crash(mem)
            seen.append((st["slot"], st["lslot"][1][2], crashed, answers))
        want_bad = [(1, k, label != "never answers SysEx", False) for k in (1, 2, 3)]
        ok &= check(f"5 new stage-1 in loader B {label} -> boots 1-3 run it (try marks 1, 2, 3, never confirmed), the 4th "
                    "boot runs the confirmed loader A, which answers", seen[:3] == want_bad and seen[3][0] == 0
                    and seen[3][3], str(seen))
    return ok


# ---- 6. corrupted writes ----------------------------------------------------------------------------------------------------
def corruption():
    res = []
    for newer in (0, 1):
        for region, off in (("state magic", 0x01), ("header", 0x200 + 0x0D), ("payload", 0x200 + 64 + 777),
                            ("signature", 0x200 + 0x35)):
            sec = [bytearray(sector(0x10, S1, 1 + (newer == 0), confirmed=True)),
                   bytearray(sector(0x11, S1, 1 + (newer == 1), confirmed=True))]
            sec[newer][off] ^= 0x04
            pn = piano(nor_with(la=bytes(sec[0]), lb=bytes(sec[1])))
            st, _ = boot0(pn)
            res.append(((newer, region), st["slot"] == 1 - newer and st["state"] == S_RUN and not pn.nor.ops))
            pn.close()
    return check("6 one bit flipped in the state magic / header / payload / signature of the newer loader (A or B) -> it is "
                 "not run, the other loader runs, no NOR op", all(r[1] for r in res), str([r for r in res if not r[1]]))


# ---- 7. power loss ----------------------------------------------------------------------------------------------------------
class PowerCut(Exception):
    pass


def cut_at(pn, m, start, partial=None):
    """make the m-th NOR operation after `start` cut power half-way (erase: half the sector, program: half the words or the
    given partial halfword value)"""
    nor = pn.nor
    orig = nor._start

    def cut(kind, data, lo, hi, status_at, pc):
        orig(kind, data, lo, hi, status_at, pc)
        if len(nor.ops) - start == m:
            if kind == "erase":
                nor.mem[lo:lo + (hi - lo) // 2] = b"\xff" * ((hi - lo) // 2)
            elif partial is not None and len(data) == 1:
                for a, _v in data.items():
                    nor.word_and(a, partial)
            else:
                for a, v in list(data.items())[: max(1, len(data) // 2)]:
                    nor.word_and(a, v) if len(data) > 1 else None
            raise PowerCut(kind)
    nor._start = cut
    return orig


def power_loss():
    ok = True
    base = nor_with(la=sector(0x10, S1, 1, confirmed=True), ma=sector(0x00, MAIN, 1, confirmed=True))
    pn = piano(base)
    pn.boot()
    n0 = len(pn.nor.ops)
    pn.upload(0x11, S1, seq=2)
    pn.run_job()
    n_ops = len(pn.nor.ops) - n0
    pn.close()
    la0 = base[0x1A0000:0x1C0000]
    bad = []
    for m in range(1, n_ops + 1):
        pn = piano(base)
        pn.boot()
        start = len(pn.nor.ops)
        orig = cut_at(pn, m, start)
        what = "?"
        try:
            pn.upload(0x11, S1, seq=2)
            pn.run_job()
        except PowerCut as e:
            what = str(e)
        pn.nor._start = orig
        mem = bytes(pn.nor.mem)
        pn.close()
        pn = piano(mem)
        st, _ = boot0(pn)
        st1, _n = pn.boot()
        good = what != "?" and st["slot"] == 0 and mem[0x1A0000:0x1C0000] == la0 and st1["active"] == ro.ACT_A
        good &= not any(0x1A0000 <= o.offset < 0x1C0000 and o.kind != "program" for o in pn.nor.ops)
        if not good:
            bad.append((m, what, st["slot"]))
        pn.close()
    ok &= check(f"7 power cut at each of the {n_ops} NOR operations of a stage-1 commit to loader B (erase, pages, seq, state "
                "version, magic) -> the next boot runs loader A (untouched sector) and the main", not bad, str(bad[:3]))
    # the try mark
    res = []
    two = nor_with(la=sector(0x10, S1, 1, confirmed=True), lb=sector(0x11, S1, 2))
    for label, partial, want_tries in (("bits untouched", 0xFFFF, 1), ("half the bits", 0x0F0F, 2)):
        pn = piano(two)
        orig = cut_at(pn, 1, 0, partial)
        try:
            boot0(pn)
        except PowerCut:
            pass
        pn.nor._start = orig
        mem = bytes(pn.nor.mem)
        pn.close()
        pn = piano(mem)
        st, _ = boot0(pn)
        res.append((label, st["slot"] == 1 and st["lslot"][1][2] == want_tries, st["lslot"][1]))
        pn.close()
    ok &= check("7 power cut while stage-0 writes the try mark of loader B: untouched -> the next boot marks the same "
                "halfword (tries 1); half written -> counted as a used try, the next one is marked (tries 2); B runs",
                all(r[1] for r in res), str(res))
    # the loader confirm write
    res = []
    for label, partial, conf in (("bits untouched", 0xFFFF, False), ("half the bits", 0x0F0F, True)):
        pn = piano(two)
        pn.boot()
        start = len(pn.nor.ops)
        orig = cut_at(pn, 1, start, partial)
        try:
            pn.auth_op(OP_CONFIRM, bytes([0x11]))
            pn.tick(3)
        except PowerCut:
            pass
        pn.nor._start = orig
        mem = bytes(pn.nor.mem)
        pn.close()
        pn = piano(mem)
        st, _ = boot0(pn)
        res.append((label, st["slot"] == 1 and bool(st["lslot"][1][1] & 2) == conf, st["lslot"][1]))
        pn.close()
    ok &= check("7 power cut during the loader confirm write: untouched -> B still unconfirmed (its tries go on); half "
                "written -> B counts as confirmed (any cleared bit = written, the state halfwords are one-way); B runs",
                all(r[1] for r in res), str(res))
    return ok


# ---- 8. RESELECT ------------------------------------------------------------------------------------------------------------
def reselect():
    ok = True
    mem = bytearray(nor_with(la=sector(0x10, S1, 1, confirmed=True), ma=sector(0x00, MAIN, 1, confirmed=True)))
    mem[0:len(AIS)] = AIS
    pn = piano(bytes(mem))
    pn.boot()
    pn.tick(5)
    hooks0 = pn.status()["hooks"]
    eg = enable(pn, 0, 1)                              # a NOR gate ON before the swap (stage-1 caller)
    gb = s0(pn)["gates"], s0(pn)["gates_main"]
    pn.upload(0x11, S1, seq=2)
    pn.run_job()
    rc = rh.apply(pn, 4)                               # OTA_TGT_RESELECT
    held = []
    for _ in range(60):            # until the main is dropped and stage-1 asked for the reselect
        if s0(pn)["request"] == 1:
            break
        pn.tick()
    pn.emu.write_bytes(SYMS["s0_in_sysex"], b"\x01")   # a SysEx call still inside the old stage-1 (task 19)
    pn.tick(5)
    held.append(s0(pn))
    pn.emu.write_bytes(SYMS["s0_in_sysex"], b"\x00")
    st, n = boot0(pn, 400)
    st1, _ = pn.boot()
    pn.tick(5)
    s3 = status3(pn)
    ok &= check("8 apply target 4 (RESELECT): stage-1 drops the main (every hook restored) and asks stage-0; a SysEx call "
                "in flight holds the swap; then stage-0 selects again: loader B (newest) with a try mark, reason RESELECT, "
                "flag RESELECTED, every NOR gate OFF again (gate 0 was ON + main before); the new stage-1 boots the main and "
                "its hooks come back",
                rc == 0 and hooks0 > 0 and held[0]["state"] == S_RUN and held[0]["request"] == 1 and held[0]["slot"] == 0
                and st["slot"] == 1 and st["reason"] == R["reselect"] and st["flags"] & 4 == 4 and st["reselects"] == 1
                and s3["lrun"] == 1 and s3["lslots"][1]["tries"] == 1 and st1["active"] == ro.ACT_A
                and s3["hooks"] == hooks0 and eg == 0 and gb == (1, 1) and st["gates"] == 0 and st["gates_main"] == 0
                and s3["gates"] == 0, f"gates {eg} {gb} -> {st['gates']}/{st['gates_main']} rc {rc} held {held[0]['state']}/{held[0]['request']} -> {st} hooks {hooks0} "
                f"-> {s3['hooks']}")
    pn.close()
    return ok


# ---- 9. NOR guard -----------------------------------------------------------------------------------------------------------
FROM_S1, FROM_MAIN = WIN + 0x100, ro.RUN + 0x100     # return addresses: inside stage-1's code / inside the main's code


def svc_call(pn, fn, args, frm):
    """stage-0 service function fn called from code at frm (lr = frm, the call ends when it returns there): r0"""
    return pn.call(SYMS[fn], *args, regs={"lr": frm}, until=frm).r0


def guard():
    pn = piano(nor_with(la=sector(0x10, S1, 1, confirmed=True)), nor_allowed=[(0x180000, 0x200000), (0x4C0000, 0x500000)])
    boot0(pn)
    stage = ro.STAGE
    pn.emu.write_bytes(stage, b"\xff" * 0x200)
    refused = [("nor_erase", (0x60000000,)), ("nor_erase", (0x60160000,)), ("nor_erase", (0x60200000,)),
               ("nor_erase", (0x60480000,)), ("nor_erase", (0x601A0000,)), ("nor_erase", (0x601C0100,)),
               ("nor_prog", (0x601A0200, stage, 0x200)), ("nor_prog", (0x601C0100, stage, 0x200)),
               ("nor_prog", (0x601C0000, stage, 0x202)), ("nor_prog", (0x601C0001, stage, 2)),
               ("nor_prog", (0x601C0000, stage, 3)), ("nor_prog", (0x6017FF00, stage, 0x200)),
               ("nor_prog_hw", (0x60000000, 0)), ("nor_prog_hw", (0x60500000, 0)), ("nor_prog_hw", (0x601C0001, 0))]
    allowed = [("nor_erase", (0x601E0000,)), ("nor_erase", (0x604C0000,)), ("nor_prog", (0x604E0000, stage, 0x200)),
               ("nor_prog", (0x601C0100, stage, 0x100)), ("nor_prog_hw", (0x601A0030, 0xFFFF)),
               ("nor_prog_hw", (0x604C0000, 0xFFFF))]
    g0, c0 = s0(pn)["guard"], len(pn.nor_calls)
    rr = [svc_call(pn, f, a, FROM_S1) for f, a in refused]
    g1, c1 = s0(pn)["guard"], len(pn.nor_calls)
    ra = [svc_call(pn, f, a, FROM_S1) for f, a in allowed]
    c2 = len(pn.nor_calls)
    ok = check(f"9 stage-0 NOR guard, called from stage-1: {len(refused)} calls outside the slot / kvstore ranges, "
               "misaligned, odd, crossing a 512 B page or erasing / page-programming the RUNNING loader sector A -> 3 "
               f"before any stock routine, guard counter +{len(refused)}; {len(allowed)} allowed ones (loader B / kvstore "
               "erase, pages, halfwords - also in the running sector) reach the stock writer -> 0",
               rr == [3] * len(refused) and c1 == c0 and g1 == g0 + len(refused) and ra == [0] * len(allowed)
               and c2 == c1 + len(allowed) and pn.nor.fault is None, f"{rr} {ra}")
    # caller-scope edges: [0x40173A14, 0x40180000) = stage-0 + the window = slot rights; just outside = kvstore only
    frm = [(0x40173A14, 0), (0x4017FFFC, 0), (0x40173A10, 3), (0x40180000, 3), (0x40100000, 3), (0xFFFFFF00, 3)]
    rb = [svc_call(pn, "nor_prog_hw", (0x601E0040, 0xFFFF), f) for f, _rc in frm]
    ok &= check("9 caller scope edges: a slot halfword from 0x40173A14 / 0x4017FFFC (stage-0 start, window end) -> 0; "
                "from 0x40173A10, 0x40180000 (main RW), stock code, an unmapped caller -> 3",
                rb == [rc for _f, rc in frm] and pn.nor.fault is None, str(rb))
    pn.close()
    return ok


def main_scope():
    """the running main (booted by stage-1, attach() got hk_svc.nor) may write the kvstore and nothing else"""
    nor_ok = [(0x180000, 0x200000), (0x4C0000, 0x500000)]
    pn = piano(nor_with(la=sector(0x10, S1, 1, confirmed=True), ma=sector(0x00, MAIN, 1, confirmed=True)),
               nor_allowed=nor_ok)
    st1, _ = pn.boot()
    pn.tick(5)
    stage = ro.STAGE
    hk_nor = pn.emu.read_u32(S1SYM["hk_svc"] + 24)
    pn.emu.write_bytes(stage, b"\xff" * 0x200)
    targets = {"loader A (running)": 0x601A0000, "loader B": 0x601E0000, "main A (running)": 0x60180000,
               "main B": 0x601C0000, "BootProgram": 0x60000000, "MainProgram": 0x60060000}
    refused = []
    for _nm, a in targets.items():
        refused += [("nor_erase", (a,)), ("nor_prog", (a + 0x200, stage, 0x200)), ("nor_prog_hw", (a + 0x08, 0)),
                    ("nor_prog_hw", (a + 0x0A, 0)), ("nor_prog_hw", (a + 0x10, 0))]
    allowed = [("nor_erase", (0x604E0000,)), ("nor_prog", (0x604E0000, stage, 0x200)),
               ("nor_prog_hw", (0x604E0200, 0xFFFF)), ("nor_erase", (0x604C0000,)),
               ("nor_prog", (0x604C1E00, stage, 0x100)), ("nor_prog_hw", (0x604FFFFE, 0xFFFF))]
    g0, c0 = s0(pn)["guard"], len(pn.nor_calls)
    slots0 = bytes(pn.nor.mem[0x180000:0x200000])
    rr = [svc_call(pn, f, a, FROM_MAIN) for f, a in refused]
    g1, c1 = s0(pn)["guard"], len(pn.nor_calls)
    slots1 = bytes(pn.nor.mem[0x180000:0x200000])
    ra = [svc_call(pn, f, a, FROM_MAIN) for f, a in allowed]
    c2 = len(pn.nor_calls)
    ok = check(f"9 main scope: the main's service (hk_svc.nor = s0_svc) called from main code: erase / page / confirm, "
               f"reject and attempt halfwords of {', '.join(targets)} ({len(refused)} calls) -> 3 before any stock "
               f"routine, guard counter +{len(refused)}, the four slot sectors byte-identical; {len(allowed)} kvstore "
               "erases / pages / halfwords (both sectors, first and last) -> 0, reach the stock writer",
               st1["active"] == ro.ACT_A and hk_nor == SYMS["s0_svc"] and rr == [3] * len(refused) and c1 == c0
               and g1 == g0 + len(refused) and slots1 == slots0 and ra == [0] * len(allowed)
               and c2 == c1 + len(allowed) and pn.nor.fault is None,
               f"active {st1['active']} nor {hk_nor:#x} {rr} {ra}")
    # stage-1 keeps its slot rights with the main running: a loader upload into loader B commits and boots
    res = pn.upload(0x11, S1, seq=2)
    st, _ = pn.run_job()
    got = bytes(pn.nor.mem[0x1E0000:0x1E0000 + 0x200 + len(S1)])
    lb_ok = got[0x200:] == S1 and got[:4] == b"C79L" and le(got, 4) == 2
    mem = bytes(pn.nor.mem)
    pn.close()
    pn = piano(mem, nor_allowed=nor_ok)
    bst, _ = boot0(pn)
    pn.close()
    ok &= check("9 ... and stage-1's own slot writes still work beside it: loader upload into loader B (erase, pages, seq, "
                "magic) -> done, sealed with seq 2; the next power-on boots loader B (try 1)",
                res == ("commit", 0) and st["err"] == 0 and lb_ok and bst["slot"] == 1 and bst["lslot"][1][2] == 1,
                f"{res} err {st['err']} sealed {lb_ok} -> slot {bst['slot']} {bst['lslot']}")
    return ok


# ---- 11. NOR gates -------------------------------------------------------------------------------------------------------------
GATES = [(0x60480000, 0x604C0000), (0x60600000, 0x60800000), (0x60800000, 0x61000000)]   # include/s0.h gate 0 / 1 / 2
# The model's BootProgram is erased (only the .SYS is loaded); the piano's starts with the AIS header (magic 'TIPA'
# 0x41504954, never erased) - nor_enable's alias refusal compares against it, so the gate cases put one there.
AIS = struct.pack("<I", 0x41504954) + bytes(range(1, 61))
# the NOR model's own protection, opened only for the gate ranges (config block B + TouchCalib = gate 0)
GATE_PROTECTED = [r for r in ro.se.NOR_PROTECTED if r[2] not in ("config blocks A/B", "TouchCalib")] + \
    [(0x460000, 0x480000, "config block A")]
GATE_ALLOWED = [(0x180000, 0x200000), (0x480000, 0x500000), (0x600000, 0x1000000)]
NEVER = {"BootProgram": 0x60000000, "BootProgram end": 0x6005E000, "MainProgram": 0x60060000, "voice DB": 0x60200000,
         "voice-DB tail 0x602E0000": 0x602E0000, "UserMemory": 0x60400000, "TouchAdjust": 0x60440000,
         "config A": 0x60460000, "RecorderData": 0x60500000, "RecorderData end": 0x605E0000, "16 MB": 0x61000000,
         "NOR top": 0x61FE0000}


def gate_piano(chip=0x1000000, patch=None):
    mem = bytearray(nor_with(la=sector(0x10, S1, 1, confirmed=True)))
    mem[0:len(AIS)] = AIS
    for off, b in (patch or {}).items():
        mem[off:off + len(b)] = b
    pn = piano(bytes(mem[:chip]), nor_allowed=[(lo, min(hi, chip)) for lo, hi in GATE_ALLOWED if lo < chip],
               nor_kw=dict(protected=GATE_PROTECTED, chip_size=chip))
    boot0(pn)
    pn.emu.write_bytes(ro.STAGE, b"\xff" * 0x200)
    return pn


def gate_writes(lo, hi):
    """erase / page / halfword at the first and the last sector of [lo, hi)"""
    s = ro.STAGE
    return [("nor_erase", (lo,)), ("nor_prog", (lo + 0x200, s, 0x200)), ("nor_prog_hw", (lo + 0x10, 0xFFFF)),
            ("nor_erase", (hi - 0x20000,)), ("nor_prog", (hi - 0x200, s, 0x200)), ("nor_prog_hw", (hi - 2, 0xFFFF))]


def enable(pn, gate, main=0, frm=FROM_S1):
    return svc_call(pn, "nor_enable", (gate, main), frm)


def gates():
    ok = True
    pn = gate_piano()
    st0, s3 = s0(pn), status3(pn)
    every = [c for lo, hi in GATES for c in gate_writes(lo, hi)]
    g0, c0 = st0["guard"], len(pn.nor_calls)
    r1 = [svc_call(pn, f, a, FROM_S1) for f, a in every]
    r2 = [svc_call(pn, f, a, FROM_MAIN) for f, a in every]
    g1, c1 = s0(pn)["guard"], len(pn.nor_calls)
    ok &= check(f"11 power-on: every NOR gate OFF (s0_boot.nor_gates / nor_gates_main 0, status 111 = 0, status 87 stage-0 "
                f"v2): {len(every)} writes (erase / page / halfword at both ends of gates 0 / 1 / 2) from stage-1 and the "
                "same from the main -> 3 before any stock routine, guard counter +2x",
                st0["gates"] == 0 and st0["gates_main"] == 0 and s3["gates"] == 0 and s3["s0ver"] == 2
                and r1 == [3] * len(every) and r2 == [3] * len(every) and c1 == c0 and g1 == g0 + 2 * len(every),
                f"{st0['gates']} {s3['gates']} {r1} {r2}")
    # the main may not enable anything; unknown ids (incl. ones whose bit would not fit a byte) are refused
    g0 = s0(pn)["guard"]
    rm = [enable(pn, g, 1, FROM_MAIN) for g in range(3)] + [enable(pn, 0, 1, f) for f in (0x40173A10, 0x40180000, 0x40100000)]
    bad = [3, 4, 7, 8, 31, 32, 0x80000000, 0xFFFFFFFF]
    ru = [enable(pn, g, 1) for g in bad]
    st = s0(pn)
    ok &= check(f"11 nor_enable from the main (gates 0-2), from 0x40173A10 / 0x40180000 / stock code, and from stage-1 with "
                f"unknown ids {[hex(g) for g in bad]} -> 3, counted, no gate changes, no NOR op",
                rm == [3] * 6 and ru == [3] * len(bad) and st["gates"] == 0 and st["gates_main"] == 0
                and st["guard"] == g0 + 6 + len(bad) and len(pn.nor_calls) == c1, f"{rm} {ru} {st['gates']}")
    # stage-1 enables gate 0 for itself only: then its writes reach the stock writer, the main's do not
    re0 = enable(pn, 0, 0)
    st = s0(pn)
    w0 = gate_writes(*GATES[0])
    c0 = len(pn.nor_calls)
    a1 = [svc_call(pn, f, a, FROM_S1) for f, a in w0]
    c1 = len(pn.nor_calls)
    a2 = [svc_call(pn, f, a, FROM_MAIN) for f, a in w0]
    o12 = [svc_call(pn, f, a, FROM_S1) for f, a in gate_writes(*GATES[1]) + gate_writes(*GATES[2])]
    ok &= check("11 stage-1 nor_enable(0, main 0) -> 0, nor_gates 0x01: its 6 writes to gate 0 (unused_2 + TouchCalib, both "
                "ends) -> 0 and reach the stock writer; the main's same writes -> 3 (not main_writable); gates 1 / 2 still "
                "refuse stage-1 (3)",
                re0 == 0 and st["gates"] == 1 and st["gates_main"] == 0 and a1 == [0] * 6 and c1 == c0 + 6
                and a2 == [3] * 6 and len(pn.nor_calls) == c1 and o12 == [3] * 12 and pn.nor.fault is None,
                f"{re0} {st['gates']:#x}/{st['gates_main']:#x} {a1} {a2} {o12}")
    # gate 1 with main_writable; no revoke: enable(1, 0) later keeps the main's right; gate 0 gets main too (only grows)
    re1 = [enable(pn, 1, 1), enable(pn, 1, 0), enable(pn, 0, 0)]
    st = s0(pn)
    w1 = gate_writes(*GATES[1])
    b1 = [svc_call(pn, f, a, FROM_MAIN) for f, a in w1] + [svc_call(pn, f, a, FROM_S1) for f, a in w1]
    b0 = [svc_call(pn, f, a, FROM_MAIN) for f, a in w0]
    re0m = enable(pn, 0, 1)
    st2 = s0(pn)
    b0m = [svc_call(pn, f, a, FROM_MAIN) for f, a in w0]
    ok &= check("11 nor_enable(1, main 1) -> 0, then enable(1, main 0) and enable(0, main 0) again -> 0 and change nothing "
                "(no revoke: nor_gates 0x03, nor_gates_main 0x02); gate 1 writes from the main and stage-1 -> 0; gate 0 "
                "from the main still 3; enable(0, main 1) -> 0 adds it (main 0x03), then the main's gate 0 writes -> 0",
                re1 == [0, 0, 0] and st["gates"] == 3 and st["gates_main"] == 2 and b1 == [0] * 12 and b0 == [3] * 6
                and re0m == 0 and st2["gates"] == 3 and st2["gates_main"] == 3 and b0m == [0] * 6 and pn.nor.fault is None,
                f"{re1} {st['gates']:#x}/{st['gates_main']:#x} {b1} {b0} {re0m} {b0m}")
    # gate 2 (16 MB part in the model: no mirror) -> on; then EVERYTHING on, main_writable everywhere: outside the table
    re2 = enable(pn, 2, 1)
    st = s0(pn)
    s3 = status3(pn)
    b2 = [svc_call(pn, f, a, FROM_S1) for f, a in gate_writes(*GATES[2])]
    never = []
    for _nm, a in NEVER.items():
        never += [("nor_erase", (a,)), ("nor_prog", (a + 0x200, ro.STAGE, 0x200)), ("nor_prog_hw", (a + 0x10, 0xFFFF))]
    nor0, c0, g0 = bytes(pn.nor.mem), len(pn.nor_calls), st["guard"]
    rn = [svc_call(pn, f, a, frm) for f, a in never for frm in (FROM_S1, FROM_MAIN)]
    # also never across entries: gate 0 end -> kvstore start in one page is impossible (page-bounded); an erase that is
    # not sector aligned, a page crossing 512 B, an odd halfword inside an ON gate
    mis = [("nor_erase", (0x60480100,)), ("nor_prog", (0x60600100, ro.STAGE, 0x200)), ("nor_prog_hw", (0x60800001, 0))]
    rmis = [svc_call(pn, f, a, FROM_S1) for f, a in mis]
    st3 = s0(pn)
    ok &= check(f"11 nor_enable(2, main 1) -> 0 (16 MB model), gates 0x07 / main 0x07, status 111 = 0x77, its writes -> 0; "
                f"with every gate ON and main_writable: erase / page / halfword at {', '.join(NEVER)} from stage-1 and "
                f"from the main ({len(rn)} calls) and 3 misaligned ones inside ON gates -> 3, no stock routine, NOR "
                "byte-identical, guard counter +all",
                re2 == 0 and st["gates"] == 7 and st["gates_main"] == 7 and s3["gates"] == 0x77 and b2 == [0] * 6
                and rn == [3] * len(rn) and rmis == [3] * 3 and len(pn.nor_calls) == c0 and bytes(pn.nor.mem) == nor0
                and st3["guard"] == g0 + len(rn) + 3 and pn.nor.fault is None,
                f"{re2} {st['gates']:#x} {s3['gates']:#x} {b2} {rn} {rmis}")
    pn.close()
    return ok


def gate_alias():
    ok = True
    # 8 MB part (the model mirrors 0x800000 onto 0x000000): gate 2 refused as a mirror, stays OFF; gate 1 is real NOR
    pn = gate_piano(chip=0x800000)
    c0 = len(pn.nor_calls)
    r = [enable(pn, 2, 1), enable(pn, 1, 0)]
    st = s0(pn)
    w = [svc_call(pn, f, a, FROM_S1) for f, a in gate_writes(*GATES[2])]
    ok &= check("11 8 MB part (0x60800000 mirrors BootProgram): nor_enable(2) -> 4 (alias, gate 2 stays OFF) and every "
                "gate 2 write -> 3, BootProgram untouched; nor_enable(1) -> 0 (0x60600000 is real NOR there)",
                r == [4, 0] and st["gates"] == 2 and st["gates_main"] == 0 and w == [3] * 6 and len(pn.nor_calls) == c0
                and bytes(pn.nor.mem[:len(AIS)]) == AIS and pn.nor.fault is None, f"{r} {st['gates']:#x} {w}")
    pn.close()
    # gate content identical to BootProgram's start (fail closed) -> 4; a stock writer owns the chip -> 1, nothing changed
    pn = gate_piano(patch={0x600000: AIS})
    ra = enable(pn, 1)
    pn.emu.write_bytes(ro.BUSY_FLAG, b"\x01\x00\x00\x00")
    rb = enable(pn, 0)
    stb = s0(pn)
    pn.emu.write_bytes(ro.BUSY_FLAG, b"\x00\x00\x00\x00")
    rc = enable(pn, 0)
    pn.emu.write_bytes(ro.BUSY_FLAG, b"\x01\x00\x00\x00")
    rd = enable(pn, 0, 1)                    # already ON: no read needed, the grant is just recorded
    pn.emu.write_bytes(ro.BUSY_FLAG, b"\x00\x00\x00\x00")
    st = s0(pn)
    ok &= check("11 a gate whose first 64 B equal BootProgram's -> 4 (fail closed); flash busy -> 1 and gate 0 stays OFF; "
                "idle again -> 0; busy while gate 0 is already ON: enable(0, main 1) -> 0 (no read)",
                ra == 4 and rb == 1 and stb["gates"] == 0 and rc == 0 and rd == 0 and st["gates"] == 1
                and st["gates_main"] == 1, f"{ra} {rb} {rc} {rd} {st['gates']:#x}/{st['gates_main']:#x}")
    pn.close()
    # gates are per boot: power off / on with the same NOR -> all OFF again
    pn = gate_piano()
    enable(pn, 0, 1)
    enable(pn, 1, 1)
    mem = bytes(pn.nor.mem)
    pn.close()
    pn = piano(mem, nor_allowed=GATE_ALLOWED, nor_kw=dict(protected=GATE_PROTECTED))
    boot0(pn)
    pn.emu.write_bytes(ro.STAGE, b"\xff" * 0x200)
    st = s0(pn)
    w = [svc_call(pn, f, a, FROM_S1) for f, a in gate_writes(*GATES[0])]
    ok &= check("11 the next power-on: nor_gates / nor_gates_main 0 again, gate 0 writes from stage-1 -> 3",
                st["gates"] == 0 and st["gates_main"] == 0 and w == [3] * 6, f"{st['gates']:#x} {w}")
    pn.close()
    return ok


# ---- 10. stores ----------------------------------------------------------------------------------------------------------------
def stores():
    pn = piano(nor_with(la=sector(0x10, S1, 1, confirmed=True), ma=sector(0x00, MAIN, 1)))
    pn.boot()
    pn.upload(0x11, S1, seq=2)
    pn.run_job()
    data_lo = data_start()
    allowed = [(data_lo, SYMS["cave_end"]), (WIN, 0x40180000), (0x40180000, 0x401A0000), (ro.STAGE, ro.STAGE + 0x20000),
               (ro.RUN, ro.RUN + 0x20000), (ro.VBUF, ro.VBUF + 0x1000), ro.STACK, (ro.KERNEL_ERR, ro.KERNEL_ERR + 4),
               (ro.BUSY_FLAG, ro.BUSY_FLAG + 4), (0x40B90000, 0x40B90200)]
    sites = [le(S1BIN, S1SYM["hk_sites"] - WIN + 12 * i) for i in range(17)]
    hook_w = [w for w in pn.writes if any(a <= w.addr < a + 4 for a in sites)]
    stray = [w for w in pn.writes if not any(lo <= w.addr < hi for lo, hi in allowed) and w not in hook_w]
    code = [w for w in pn.writes if 0x40173A14 <= w.addr < data_lo]
    pn.close()
    return check(f"10 {len(pn.writes)} stores of boot (stage-0 loads stage-1, stage-1 boots main A) + a stage-1 commit: only "
                 "stage-0 .data, the window, core RW, staging, run, read-back, stack, NORTi error word, busy flag and the "
                 "main's allow-listed hook words (single-word stores from stage-1's poke body); never stage-0 code",
                 not stray and not code and all(w.size == 4 and S1SYM["ota_poke_body"] <= w.pc < S1SYM["ota_poke_body"] + 0x24
                                                 for w in hook_w), f"stray {stray[0]}" if stray else (f"code {code[0]}" if code else ""))


UNITS = [rtlib.Unit("static", static_checks, 2), rtlib.Unit("matrix1", lambda: matrix(0), 27),
         rtlib.Unit("matrix2", lambda: matrix(1), 27), rtlib.Unit("escapes", escapes, 20),
         rtlib.Unit("interlocks", interlocks, 15), rtlib.Unit("rollback", rollback, 10),
         rtlib.Unit("corruption", corruption, 5), rtlib.Unit("power_loss", power_loss, 30),
         rtlib.Unit("reselect", reselect, 5), rtlib.Unit("guard", guard, 2), rtlib.Unit("main_scope", main_scope, 4),
         rtlib.Unit("gates", gates, 3), rtlib.Unit("gate_alias", gate_alias, 3), rtlib.Unit("stores", stores, 5)]


def main(argv):
    print(f"image   ota_loader_v4 (stage-0 alone) + per-case slots  stage-0 {SYMS['cave_end'] - 0x40173A14} B  stage-1 "
          f"{len(S1BIN)} B  key id {op.key_id(KEY):08x}")
    results = rtlib.run_units(UNITS, argv)
    ok = all(bool(r) for _u, r in results)
    return rtlib.finish(ok, rtlib.shard_note(UNITS, argv) + " - emulation only: no interrupts, no RTOS scheduling, NOR "
                        "timing is poll counts.")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
