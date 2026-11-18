# roundtrip_state.py - emulator verification of Phase 3-4 settings state, stage S1 (store first fill):
# the core's kpstate module (result/firmware/src/kproto/kpstate.c) fills its parameter store from the
# stock getter FUN_4015E960 and the result must equal what the stock itself sends for a full dump.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_state.py [-q]
#
# The core side is the real C module (host build result/firmware/build/libkpstate.so, ctypes - built here
# with make); its io.match / io.value are answered from the stock image under StockEmu exactly the way the
# cave's ss_match / ss_value do (ports/cave/sysex_cave.c: FUN_4015205C; FUN_4014C034, FUN_4015B470,
# FUN_4015B00C / FUN_4015B188, FUN_401520B4, FUN_4014EAF4, getter FUN_4015E960, frame builder FUN_4014B0DC,
# key tables 0x400FD8CB/D0/E0).  The reference is independent of that: the stock request task body
# FUN_40152A34(0x7F, 5F, 7F, 7F) run for real (sender FUN_4014BD1C with its ACK wait stubbed "ACK",
# p3_map.StockRun), frames taken at the BLE TX writer.
# Stock settings states (RAM):
#   D  stock defaults: FUN_400B1448(0x40400004) then the boot copy FUN_4015B384(0x40400004) -> live 0x418CE0DC
#   N  this piano's saved settings: UserMemory image (NOR 0x60400000, 0x264E8 B) from target/CA79NOR.BIN
#      (read only) put at 0x40400000 as the boot loader FUN_400A9ACC finds it, then FUN_4015B384 - skipped
#      when the dump is absent
# Checks (numbered as printed):
#   1  static: the image words the store layout rests on (REA): live copy 0x418CE0DC
#      (0x4015C388), ctx block 0x418F19E4 (0x4015C3A4), copy length 0x23A5E (0x4015C38C), ctx table
#      0x400A710E = 01 02 03 04, music tempo 0x4191570C (0x4016E5F8), beat table 0x401235B8 (0x4016ED38)
#   2  D: core fill (4 descriptors per kst_step) reads the same value frames, in the same order, as the
#      stock dump sends; every frame whose key the core knows is in the store with the frame's bytes;
#      unknown / long keys listed; synced; a compare pass right after: 0 differences
#   3  D: the fill calls no reliable sender (FUN_4014C010 / FUN_4014BD1C / FUN_4014B638) and writes no stock
#      RAM (>= 0x41000000 outside the stack); instructions per 4-descriptor step (task 19 budget)
#   4  D: the 11 System values of the settings read-back: the 7 the stock holds are in the store with the
#      values read straight from their RAM (ctx block +0x4A..+0x50, 0x4191570C, recorder bytes), the 4 NG
#      extension values have no stock frame and stay out of the store
#   5  D: stock-internal changes the core did not see (sets through the real Kawai end handler 0x40147DB0 ->
#      FUN_4014A2C8: metronome tempo, rhythm, volume, speaker volume, a Sound): a compare pass finds exactly
#      those keys (fixes them), the next compare finds 0, and the store equals a fresh stock dump
#   6  N: as 2 + 4 on this piano's saved settings; the System values equal the NOR bytes (REA §1.3 table)
#   8  D and N (S4): the pure table of kpstate.c == the measurement (every settings
#      key set to 0 / mask / mask/2 through the stock's end handler; pure = the settings bytes it wrote are read by its own
#      getter only); one above the descriptor mask: the S4 gate refuses it, the stock stores it
#   7  D and N (S3, 19_p3_state_s3): every value frame asked from the store as a request would (descriptor i, frame j,
#      key, device byte 7F / 00 / 10) == the stock builder FUN_4014B0DC's bytes for that cc; every descriptor the core
#      owns (kst_s3_owned) loads no RAM outside the settings image (live 0x418CE0DC + 0x23A5E) in the getter
from __future__ import annotations

import ctypes
import pathlib
import subprocess
import sys

from unicorn import UC_HOOK_MEM_READ

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

p3 = rtlib.load("p3_map")
SE = p3.StockEmu
REPO = rtlib.REPO
FW = REPO / "result/firmware"
LIB = FW / "build/libkpstate.so"
NOR = REPO / "target/CA79NOR.BIN"

# stock routines (REA; the same bindings as cave/stock_session.ld)
DESC, MATCH, PART_X, DESC_HAS, CTX_OF = 0x4014C034, 0x4015205C, 0x4014EAF4, 0x401520B4, 0x4015B470
SLOT16, SLOT5, GETTER, FRAME = 0x4015B188, 0x4015B00C, 0x4015E960, 0x4014B0DC
KEY80, KEY81, KEY82 = 0x400FD8CB, 0x400FD8D0, 0x400FD8E0
DEFAULTS, BOOT_COPY = 0x400B1448, 0x4015B384
SENDERS = {0x4014C010: "FUN_4014C010", 0x4014BD1C: "FUN_4014BD1C", 0x4014B638: "FUN_4014B638"}
UM_RAM, UM_NOR, UM_LEN, UM_MAGIC = 0x40400000, 0x400000, 0x264E8, 0x5AA5A510
LIVE, CTXB = 0x418CE0DC, 0x418F19E4
SCRATCH = 0x50000000           # extra RAM for the io calls' buffers (mapped here, not stock RAM)
STACK_LO = 0x40F00000

QUIET = "-q" in sys.argv
FAILS = []


def check(name, ok, detail=""):
    if not ok:
        FAILS.append(name)
    return rtlib.check(name, ok, detail, quiet=QUIET)


hexs, le = p3.hexs, rtlib.le


# ---------------------------------------------------------------------------------------------- core (C)
def build_lib():
    r = subprocess.run(["make", "-s", "-C", str(FW), "build/libkpstate.so"], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        raise SystemExit("make build/libkpstate.so failed")
    return ctypes.CDLL(str(LIB))


MATCH_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int)
VALUE_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_uint8))


class Core:
    """kpstate.c + store.c via tools/kpstate_host.c; io answered by a StockSide."""

    def __init__(self, lib, side):
        self.lib, self.side = lib, side
        self.read = []                          # value frames the core read, in order
        self.where = []                         # (i, j, getter id, x) of each
        self._m = MATCH_FN(lambda i: 1 if side.match(i) else 0)
        self._v = VALUE_FN(self._value)
        lib.kh_init(self._m, self._v, 0)

    def _value(self, i, j, out):
        r = self.side.value(i, j)
        if isinstance(r, int):
            return r
        for k, b in enumerate(r):
            out[k] = b
        self.read.append(bytes(r))
        self.where.append((i, j) + self.side.last)
        return len(r)

    def run(self, mode, budget=0):
        self.read, self.where = [], []
        return self.lib.kh_run(mode, budget)

    def stats(self):
        a = (ctypes.c_uint32 * 16)()
        self.lib.kh_stats(a)
        k = ["frames", "stored", "unknown", "longval", "bad", "diffs", "passes", "synced", "diffs_total"]
        d = dict(zip(k, a[:9]))
        d["last_diff"], d["last_unknown"], d["indexed"] = bytes(a[9:12]), bytes(a[12:15]), a[15]
        return d

    def idx_get(self, v1, v2, part, sub):
        o = (ctypes.c_uint8 * 4)()
        n = self.lib.kh_idx_get(v1, v2, part, sub, o)
        return n if n <= 0 else bytes(o[:n])

    def get(self, v1, v2, part):
        o = (ctypes.c_uint8 * 4)()
        n = self.lib.kh_get(v1, v2, part, o)
        return n if n <= 0 else bytes(o[:n])

    def put_kind(self, frame):
        ch = ctypes.c_int()
        buf = (ctypes.c_uint8 * len(frame))(*frame)
        return self.lib.kh_put_frame(buf, len(frame), ctypes.byref(ch))


# ---------------------------------------------------------------------------------------------- stock
class StockSide:
    """The stock image under the emulator: settings state, the cave's ss_match / ss_value in Python on the
    stock routines, the real stock dump."""

    def __init__(self, state, nor=None):
        self.run = p3.StockRun()                # RTOS stubbed, TX writers logged, binding 3 (BLE)
        e = self.e = self.run.emu
        e.map_ram(SCRATCH, 0x1000)
        self.calls = {a: 0 for a in SENDERS}
        for a in SENDERS:
            e.hook(a, (lambda aa: lambda em: (self.calls.__setitem__(aa, self.calls[aa] + 1), SE.CONTINUE)[1])(a))
        if state == "D":
            e.call(DEFAULTS, UM_RAM + 4)
        else:
            img = nor[UM_NOR:UM_NOR + UM_LEN]
            e.write_bytes(UM_RAM, img)
        e.call(BOOT_COPY, UM_RAM + 4)
        self.ctx = e.call(CTX_OF, e.read_bytes(p3.BINDING, 1)[0]).r0 & 0xFF
        self.k80 = e.read_bytes(KEY80, 5)
        self.k81 = e.read_bytes(KEY81, 16)
        self.k82 = e.read_bytes(KEY82, 16)
        self.insns = 0                          # instructions spent in io calls since reset_meter()
        self.ram_writes = []                    # stock RAM stores made by io calls

    def reset_meter(self):
        self.insns, self.ram_writes = 0, []
        for a in self.calls:
            self.calls[a] = 0

    def _call(self, addr, *args):
        r = self.e.call(addr, *args, max_insns=2_000_000)
        assert r.ok, f"{addr:#x}: {r}"
        self.insns += r.insns
        for w in r.writes:
            if (w.addr >= 0x41000000 and not SCRATCH <= w.addr < SCRATCH + 0x1000) or 0x40000000 <= w.addr < STACK_LO:
                self.ram_writes.append((addr, w.addr))
        return r.r0

    def match(self, i):
        return self._call(MATCH, i, 0x5F, 0x7F) != 0xFFFFFFFF

    def _get(self, x, i):
        buf = SCRATCH + 0x100
        self.e.write_bytes(buf, bytes(48))
        self.last = (i, x)
        return self._call(GETTER, self.ctx, x, i, buf) == 0

    def _frame(self, desc, key, cc=0x7F):
        src, buf, out = SCRATCH, SCRATCH + 0x100, SCRATCH + 0x200
        self.e.write_bytes(src, desc.to_bytes(4, "little") + bytes([key, 0, 0, 0]) + buf.to_bytes(4, "little"))
        self.e.write_bytes(out, bytes(40))
        n = self._call(FRAME, cc, src, out, self.e.read_bytes(desc + 8, 1)[0]) & 0xFF
        return self.e.read_bytes(out, n)

    def frame_cc(self, i, x, key, cc):
        """getter (i, x) + the stock builder with device byte cc: the frame a request with that cc gets."""
        assert self._get(x, i)
        return self._frame(self._call(DESC, i), key, cc)

    def getter_reads(self, i, x):
        """RAM addresses (>= 0x41000000, below the scratch) the getter loads for (i, x)."""
        reads = set()
        uc = self.e.uc
        h = uc.hook_add(UC_HOOK_MEM_READ, lambda _u, _a, addr, size, _v, _d: reads.update(range(addr, addr + size)),
                        None, 0x41000000, SCRATCH - 1)
        uc.ctl_flush_tb()
        try:
            self._get(x, i)
        finally:
            uc.hook_del(h)
            uc.ctl_flush_tb()
        return reads

    def value(self, i, j):
        """ss_value(port, 0x7F, i, part 0x7F, j): frame bytes, 0 = none for this j, -1 = descriptor done."""
        desc = self._call(DESC, i)
        b = self.e.read_bytes(desc + 3, 1)[0]
        if b == 0x80:
            if j >= 5:
                return -1
            x = self._call(SLOT5, j)
            if self._call(DESC_HAS, i, x) == 0 or not self._get(x, i):
                return 0
            key = self.k80[x]
        elif b in (0x81, 0x82):
            if j >= 16:
                return -1
            x = self._call(SLOT16, j)
            if not self._get(x, i):
                return 0
            key = self.k82[x] if b == 0x82 else (0x0F if x == 0 else self.k81[x])
        elif 0x77 <= i <= 0x79:
            if j >= 4:
                return -1
            if not self._get(self._call(SLOT16, j), i):
                return 0
            key = b
        else:
            if j or b >= 0x80:
                return -1
            x = self._call(PART_X, b)
            if x != 0x7F and self._call(DESC_HAS, i, x) == 0:
                return -1
            if not self._get(x, i):
                return -1
            key = b
        return self._frame(desc, key)

    def dump(self):
        """The stock's own full dump: request body FUN_40152A34(7F, 5F, 7F, 7F), frames at the BLE TX writer."""
        r = self.run.request(0x5F, 0x7F, 0x7F)
        assert r.ok, str(r)
        return [m for _p, m in self.run.sent]

    def set(self, v1, v2, part, data):
        """A set the stock applies on its own (through the real end handler, binding 3; the receive buffer
        holds the frame without its F7, as p3_map delivers it)."""
        r = self.run.deliver(bytes([0xF0, 0x40, 0x7F, 0x10, 0x08, 0x02, v1, v2, part, *data]))
        assert r.ok and self.run.applies, f"{v1:02X} {v2:02X} {part:02X}: not applied ({r})"


# ---------------------------------------------------------------------------------------------- checks
SYSTEM7 = {  # name: (v1, v2) part 0x7F - the 7 the stock holds
    "MetronomeTempo": (0x56, 0x01), "BeatSelect": (0x56, 0x02), "RhythmSelect": (0x56, 0x09),
    "MetronomeMode": (0x56, 0x0A), "MetronomeVolume": (0x56, 0x03), "MusicTempo": (0x52, 0x02),
    "IntSongPart": (0x52, 0x01),
}
NG4 = {"SoundPreviewVolume": (0x50, 0x1F), "SongTranspose": (0x53, 0x02), "MusicRepeat": (0x52, 0x07),
       "PlayMode": (0x52, 0x09)}


def expected_system(side):
    """The 7 values straight from stock RAM, by the getter cases (REA FUN_4015E960)."""
    e = side.e
    blk = e.read_bytes(CTXB, 0x56)
    tempo = le(blk, 0x4E, 2)
    beat = blk[0x4C] if blk[0x4C] <= 9 else 3                 # FUN_4016E5FC: > 9 -> 3
    ent = e.read_bytes(0x401235B8 + 4 * beat, 4)              # {u16 ticks, u8 beats, pad}
    den = {0x30: 3, 0x60: 2}.get(le(ent, 0, 2))
    mt = le(e.read_bytes(0x4191570C, 2), 0, 2)
    kind, rstate = e.read_bytes(0x4139F4C6, 1)[0], e.read_bytes(0x4191EF79, 1)[0]
    if kind != 0:
        part = None
    elif 3 <= rstate <= 6:
        part = bytes([e.read_bytes(0x4191EF73, 1)[0]])
    else:
        sel = e.read_bytes(0x4191EF72, 1)[0]
        part = bytes([0x7F if sel == 0 else (sel - 1) & 0xFF])
    return {
        "MetronomeTempo": bytes([tempo >> 7, tempo & 0x7F]),
        "BeatSelect": None if den is None else bytes([ent[2], den]),
        "RhythmSelect": bytes([blk[0x4B]]), "MetronomeMode": bytes([blk[0x4A]]),
        "MetronomeVolume": bytes([blk[0x50]]),
        "MusicTempo": bytes([(mt >> 7) & 0x7F, mt & 0x7F]),
        "IntSongPart": part,
    }, dict(tempo=tempo, beat_index=blk[0x4C], rhythm=blk[0x4B], mode=blk[0x4A], volume=blk[0x50], on=blk[0x49])


def fill_and_compare(lib, side, tag):
    core = Core(lib, side)
    side.reset_meter()
    steps = core.run(1, 4)
    read = list(core.read)
    where = list(core.where)
    st = core.stats()
    insns_fill, writes, calls = side.insns, list(side.ram_writes), dict(side.calls)
    dump = side.dump()
    vals = [m for m in dump if m[3] == 0x10]
    ends = [m for m in dump if m[3] == 0x03]
    check(f"{tag}: core fill read the stock dump's value frames, same bytes and order",
          read == vals and len(ends) == 1, f"core {len(read)} frames, stock {len(vals)} + End {len(ends)}")
    unknown, longv, mism, nidx = [], [], [], 0
    for m in vals:
        got = core.idx_get(m[6], m[7], m[8], m[9])
        if got != -1:                    # sub-indexed key: the side table
            nidx += 1
            if got != bytes(m[9:-1]):
                mism.append(f"{hexs(m[6:9])} [{m[9]:02X}] side table {got}")
            continue
        kind = core.put_kind(m)          # 1 = SAME (already there), 2/3 = unknown/long
        if kind == 2:
            unknown.append(hexs(m[6:9]))
        elif kind == 3:
            longv.append(hexs(m[6:9]))
        elif kind != 1:
            mism.append(f"{hexs(m[6:9])} kind {kind}")
    check(f"{tag}: every known key is in the store with the dump's bytes", not mism, "; ".join(mism[:6]))
    check(f"{tag}: fill counters add up, synced", st["synced"] == 1 and st["frames"] == len(vals)
          and st["stored"] + st["unknown"] + st["longval"] + st["bad"] == st["frames"] and st["bad"] == 0
          and st["unknown"] == len(unknown) and st["longval"] == len(longv) and st["indexed"] == nidx,
          f"frames {st['frames']} stored {st['stored']} (side table {st['indexed']}) unknown {st['unknown']} long {st['longval']} bad {st['bad']}"
          f", {steps} steps of 4 descriptors, store entries {lib.kh_count()}")
    if not QUIET:
        print(f"      keys the core cannot hold - unknown: {', '.join(unknown) or '-'}; long: {', '.join(longv) or '-'}")
    core.run(2, 0)
    core.where = where
    st2 = core.stats()
    check(f"{tag}: compare pass right after the fill finds 0 differences", st2["diffs"] == 0 and st2["frames"] == len(vals),
          f"diffs {st2['diffs']}")
    return core, dict(insns=insns_fill, steps=steps, writes=writes, calls=calls, unknown=unknown, longv=longv,
                      frames=read, where=list(core.where))


def system_values(core, side, tag, extra=""):
    exp, raw = expected_system(side)
    rows, bad = [], []
    for name, (v1, v2) in SYSTEM7.items():
        got = core.get(v1, v2, 0x7F)
        got = got if isinstance(got, bytes) else None
        rows.append(f"{name} {hexs(got) if got else '-'}")
        if got is None or got != exp[name]:
            bad.append(f"{name} store {got and hexs(got)} RAM {exp[name] and hexs(exp[name])}")
    check(f"{tag}: the 7 System values the stock holds are in the store = stock RAM{extra}", not bad,
          "; ".join(bad) if bad else ", ".join(rows))
    ng = []
    for name, (v1, v2) in NG4.items():
        got = core.get(v1, v2, 0x7F)
        if isinstance(got, bytes):
            ng.append(name)
    check(f"{tag}: the 4 NG extension values have no stock value and stay out of the store", not ng, ", ".join(ng))
    return raw, rows


def s3_frames(lib, core, side, frames, where, tag):
    """7 (S3): every value frame of the walk, asked from the store as a
    request would (frame j of descriptor i, the key, device byte cc) - served frames must be the stock builder's bytes
    for that cc; the owned descriptors (kst_s3_owned, REA FUN_4015E960) must read nothing but the settings image."""
    lib.kh_s3_enable(1)
    out = (ctypes.c_uint8 * 40)()
    served, stock, miss, bad = 0, set(), [], []
    leaks, clean_out = [], set()
    lo, hi = LIVE, LIVE + 0x23A5E
    for f, (i, j, gi, x) in zip(frames, where):
        owned = lib.kh_s3_owned(i) == 1
        outside = {a for a in side.getter_reads(gi, x) if not lo <= a < hi}
        if owned and outside:
            leaks.append(f"{i:02X}/{x}: {sorted(hex(a) for a in outside)[:3]}")
        if not owned and not outside:
            clean_out.add(f"{i:02X} {hexs(f[6:9])}")
        for cc in (0x7F, 0x00, 0x10):
            n = lib.kh_s3_frame(i, j, cc, f[6], f[7], f[8], out)
            if n <= 0:
                if cc == 0x7F:
                    if owned:
                        miss.append(hexs(f[6:9]))   # a key the stock changes itself, or one the store cannot hold
                    else:
                        stock.add(i)
                continue
            want = f if cc == 0x7F else side.frame_cc(gi, x, f[8], cc)
            if bytes(out[:n]) != want:
                bad.append(f"{hexs(f[6:9])} cc {cc:02X}: store {hexs(bytes(out[:n]))} stock {hexs(want)}")
            elif cc == 0x7F:
                served += 1
    a = (ctypes.c_uint32 * 7)()
    lib.kh_s3_stats(a)
    lib.kh_s3_enable(0)
    check(f"{tag}: S3 frames from the store == the stock builder's (cc 7F / 00 / 10)", not bad and served >= 140
          and a[6] == 1, f"served {served} of {len(frames)}, stock-owned descriptors {len(stock)}, owned but not in the "
          f"store {miss}; {'; '.join(bad[:4])}")
    check(f"{tag}: owned descriptors read only the settings image (live 0x418CE0DC + 0x23A5E)", not leaks,
          "; ".join(leaks[:6]) if leaks else f"stock-owned that also read only it (kept the stock's: run byte / KEY / "
          f"ctx-dependent): {sorted(clean_out)}")
    return served, sorted(stock), miss


def spec_keys():
    """(v1, v2) -> (len, data_max) of the fn 0x10 parameters (ca79.json = the stock descriptor masks)."""
    import json
    out = {}
    for prm in json.loads(p3.SPEC.read_text())["params"]:
        if prm["fn"] == 0x10 and not prm["var_len"]:
            out.setdefault((prm["v1"], prm["v2"]), (prm["len"], list(prm["data_max"])))
    return out


def s4_pure(lib, side, frames, where, tag):
    """8 (S4): for every settings key (kst_s3_owned descriptor, single-valued,
    not a key the stock changes itself) set the values 0 / mask / mask/2 through the stock's own end handler and diff the
    settings image (live 0x418CE0DC + 0x23A5E): the key is pure when every byte the apply wrote is read by this key's
    getter only (reader map: getter_reads of every settings value frame of the walk).  Returns (pure, impure{key: why})."""
    lo, hi = LIVE, LIVE + 0x23A5E
    readers, keys = {}, []
    for f, (i, j, gi, x) in zip(frames, where):
        if lib.kh_s3_owned(i) != 1:
            continue
        k = bytes(f[6:9])
        idx = lib.kh_idx_get(f[6], f[7], f[8], f[9], (ctypes.c_uint8 * 4)()) != -1
        rk = k + (bytes([f[9]]) if idx else b"")
        for a in side.getter_reads(gi, x):
            if lo <= a < hi:
                readers.setdefault(a, set()).add(rk)
        if (not idx and not lib.kh_key_stock(f[6], f[7], f[8]) and k not in keys
                and lib.kh_get(f[6], f[7], f[8], (ctypes.c_uint8 * 4)()) > 0):   # a key the core holds
            keys.append(k)
    spec = spec_keys()
    pure, impure = [], {}
    e = side.e
    e.periph[0x01E26064] = 0x100000                # LineInLevel's apply waits for GP5[4] (roundtrip_sysex.py)
    for k in keys:
        ln, mx = spec[(k[0], k[1])]
        vals = [bytes(ln), bytes(mx[:ln]), bytes(m // 2 for m in mx[:ln])]
        why = None
        for v in vals:
            before = e.read_bytes(lo, hi - lo)
            r = side.run.deliver(bytes([0xF0, 0x40, 0x7F, 0x10, 0x08, 0x02, *k, *v]))
            if not r.ok:
                why = f"{hexs(v)}: {r.stop}"
                break
            after = e.read_bytes(lo, hi - lo)
            others = set()
            for o in (o for o in range(hi - lo) if before[o] != after[o]):
                others |= readers.get(lo + o, set()) - {k}
            if others:
                why = f"{hexs(v)} -> {', '.join(sorted(hexs(x) for x in others)[:4])}"
                break
        if why:
            impure[k] = why
        else:
            pure.append(k)
    # the gate (kh_s4_judge) against what the stock does: (a) a packed entry one byte short - FUN_4014EBFC's packed loop has
    # no length check (REA), the stock applies it; the gate drops just that entry; (b) every byte 7F - inside the descriptor
    # masks (ca79.json data_max: the stock table 0x400FAA2C masks are 0x80 "any 7-bit" for nearly all), so the gate lets it
    # through and the stock's own case decides: kept as 7F, or refused (the read-back then gives the store the stock's value)
    out = (ctypes.c_uint8 * 300)()
    cnt = (ctypes.c_uint32 * 4)()
    short_bad, short_applied, short_stop, short_lookup, kept7f, left = [], 0, 0, [], [], []
    ok_e = bytes([5, 0x51, 0x0B, 0x7F, 0x10])                      # speaker volume 0x10: a valid entry
    for k in keys:
        ln, _mx = spec[(k[0], k[1])]
        bad_e = bytes([4 + ln - 1, *k, *bytes(ln - 1)])
        fr = bytes([0xF0, 0x40, 0x7F, 0x10, 0x08, 0x02, 0x5F, 0x00, 0x7F, *ok_e, *bad_e, 0xF7])
        n = lib.kh_s4_judge(fr, len(fr), out, cnt)
        want = bytes([0xF0, 0x40, 0x7F, 0x10, 0x08, 0x02, 0x5F, 0x00, 0x7F, *ok_e, 0xF7])
        if bytes(out[:n]) != want or cnt[0] != 1 or cnt[1] != 1:
            short_bad.append(hexs(k))
        r = side.run.deliver(fr[:-1])
        short_applied += bool(r.ok and len(side.run.applies) == 2)
        short_stop += not r.ok
        if r.ok and len(side.run.applies) == 1:
            short_lookup.append(hexs(k))          # the stock lookup FUN_4014E8AC did not find the short entry either
        v = bytes([0x7F] * ln)
        before = e.read_bytes(lo, hi - lo)
        r = side.run.deliver(bytes([0xF0, 0x40, 0x7F, 0x10, 0x08, 0x02, *k, *v]))
        if not r.ok:
            continue
        after = e.read_bytes(lo, hi - lo)
        if any(before[o] != after[o] and k in readers.get(lo + o, set()) for o in range(hi - lo)):
            kept7f.append(hexs(k))
        else:
            left.append(hexs(k))
    return pure, impure, dict(n=len(keys), short_bad=short_bad, short_applied=short_applied, short_stop=short_stop,
                                  short_lookup=short_lookup, kept7f=kept7f, left=left)


def s4_checks(lib, side, info, tag):
    pure, impure, o = s4_pure(lib, side, info["frames"], info["where"], tag)
    table = [k for k in pure + list(impure) if lib.kh_s4_pure(*k)]
    check(f"{tag}: S4 pure table (kpstate.c S4_PURE) == the measured pure keys ({len(pure)} of {len(pure) + len(impure)}: "
          f"every settings byte the stock apply wrote for 0 / mask / mask/2 is read by that key's getter only)",
          sorted(table) == sorted(pure) and len(pure) == lib.kh_s4_pure(-1, -1, -1),
          "impure: " + "; ".join(f"{hexs(k)} ({w})" for k, w in impure.items()))
    check(f"{tag}: S4 gate on a packed set (valid 51 0B + the key one byte short), all {o['n']} keys: the valid entry kept, the "
          f"short one dropped; the stock's packed path applied the short entry too in {o['short_applied']} of them (the "
          f"stock lookup dropped it for {o['short_lookup']}, {o['short_stop']} stop inside the stock apply in the emulator)",
          not o["short_bad"] and o["short_applied"] + o["short_stop"] + len(o["short_lookup"]) == o["n"]
          and o["short_stop"] <= 2 and len(o["short_lookup"]) <= 2, f"gate wrong: {o['short_bad'][:6]}")
    check(f"{tag}: every byte 7F (inside the masks - the gate lets it through): the stock case kept it for {len(o['kept7f'])}, "
          f"left the value for {len(o['left'])} (S4 read-back = write miss there)", len(o["kept7f"]) + len(o["left"]) > 0,
          "left: " + ", ".join(o["left"][:20]) + (" .." if len(o["left"]) > 20 else ""))
    return pure, impure, o


def main():
    print("roundtrip_state: Phase 3-4 S1 store first fill vs the stock getter")
    lib = build_lib()
    img = rtlib.load("patcher").stock_bytes()
    w = lambda va: le(img, va - 0x40060000)
    check("1 static: store layout words (live copy, ctx block, copy length, ctx table, music tempo, beat table)",
          w(0x4015C388) == LIVE and w(0x4015C3A4) == CTXB and w(0x4015C38C) == 0x23A5E
          and img[0x400A710E - 0x40060000:0x400A7112 - 0x40060000] == b"\x01\x02\x03\x04"
          and w(0x4016E5F8) == 0x4191570C and w(0x4016ED38) == 0x401235B8)

    side = StockSide("D")
    print(" 2 D (stock defaults):")
    core, info = fill_and_compare(lib, side, "2 D")
    per_step = info["insns"] / max(1, info["steps"])
    check("3 D: the fill calls no reliable sender and writes no stock RAM",
          not any(info["calls"].values()) and not info["writes"],
          f"{info['insns']} instructions in {info['steps']} steps (~{per_step:.0f}/step); senders {info['calls']};"
          f" writes {[(hex(a), hex(b)) for a, b in info['writes'][:4]]}")
    system_values(core, side, "4 D")
    print(" 7 D (S3 read owner):")
    s3_frames(lib, core, side, info["frames"], info["where"], "7 D")
    print(" 8 D (S4 write owner):")
    s4_checks(lib, StockSide("D"), info, "8 D")

    # 5: changes the stock makes on its own (the core's mirror did not see them)
    sets = [((0x56, 0x01, 0x7F), [0x00, 0x64]), ((0x56, 0x09, 0x7F), [0x05]), ((0x56, 0x03, 0x7F), [0x30]),
            ((0x51, 0x0B, 0x7F), [0x20]), ((0x55, 0x00, 0x00), [0x00, 0x05, 0x00])]
    for (v1, v2, part), d in sets:
        side.set(v1, v2, part, d)
    before = {k: core.get(*k) for k, _ in sets}
    core.run(2, 4)
    st = core.stats()
    changed = [k for k, _ in sets if core.get(*k) != before[k]]
    dump2 = [m for m in side.dump() if m[3] == 0x10]
    same = all(core.idx_get(m[6], m[7], m[8], m[9]) == bytes(m[9:-1]) if core.idx_get(m[6], m[7], m[8], m[9]) != -1
               else core.put_kind(m) in (1, 2, 3) for m in dump2)
    check("5 D: compare finds exactly the keys the stock changed on its own, fixes them; store = fresh dump",
          st["diffs"] == len(sets) and len(changed) == len(sets) and same,
          f"diffs {st['diffs']} (set {len(sets)}), last {hexs(st['last_diff'])}, changed {len(changed)}")
    core.run(2, 0)
    check("5 D: the next compare finds 0", core.stats()["diffs"] == 0)

    if NOR.exists():
        nor = NOR.read_bytes()                  # read only - never written, never copied
        head, tail = le(nor, UM_NOR), le(nor, UM_NOR + 0x264E4)
        print(" 6 N (this piano's saved settings, target/CA79NOR.BIN):")
        check("6 N: UserMemory magic head == tail == 0x5AA5A510 (boot keeps the image)", head == tail == UM_MAGIC,
              f"{head:#x} {tail:#x}")
        sideN = StockSide("N", nor)
        coreN, infoN = fill_and_compare(lib, sideN, "6 N")
        raw, rows = system_values(coreN, sideN, "6 N")
        s3_frames(lib, coreN, sideN, infoN["frames"], infoN["where"], "7 N")
        s4_checks(lib, StockSide("N", nor), infoN, "8 N")
        blk = nor[UM_NOR + 0x2390C:UM_NOR + 0x2390C + 0x56]
        check("6 N: the metronome bytes the getter read are the NOR bytes 0x60423955..0x6042395C",
              (blk[0x49], blk[0x4A], blk[0x4B], blk[0x4C], le(blk, 0x4E, 2), blk[0x50])
              == (raw["on"], raw["mode"], raw["rhythm"], raw["beat_index"], raw["tempo"], raw["volume"]),
              f"on {raw['on']} mode {raw['mode']} rhythm {raw['rhythm']} beat index {raw['beat_index']}"
              f" tempo {raw['tempo']} volume {raw['volume']}")
        dflt = [m for m in StockSide("D").dump() if m[3] == 0x10]
        diff = [m for m in sideN.dump() if m[3] == 0x10 and m not in dflt]
        if not QUIET:
            print(f"      value frames that differ from the stock defaults: {len(diff)}")
            for m in diff:
                print(f"        {hexs(m)}")
    else:
        print("SKIP 6 N (target/CA79NOR.BIN absent)")

    print("roundtrip_state:", "all checks passed" if not FAILS else f"{len(FAILS)} FAILED")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
