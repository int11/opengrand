# roundtrip_metro.py - emulator verification of Phase 3-5 metronome stage 1: the core module
# result/firmware/src/kproto/kpmetro.c judges metronome sets, keeps the store and makes the BeatCounter
# fan-out frames, while the stock song task (task 20, FUN_4016DE5C) keeps generating the beats and the sound.
# The stock metronome paths run for real; the core's decisions / frames must equal what the stock does.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_metro.py [-q]
#
# (design steps M0-M5).
# No image, no slot payload, no cave wiring: the core side is the real C module as a host shared library
# (result/firmware/build/libkpmetro.so via tools/kpmetro_host.c, ctypes, built here with make) - plus a
# freestanding ARM compile of kpmetro.c with the pinned toolchain (size, no unexpected externals).
# Virtual time: one cycle = one 2 ms period, in the order the stock runs it (2 ms handler 0x400A9D88 signals
# task 19 before task 20; 19 has the higher priority):
#   task 19   sets arrive through the real Kawai end handler 0x40147DB0 (-> apply FUN_4016053C); the core
#             judges the same set (kpm_set); then the core's 2 ms poll (the pump tail 0x401539AC) reads the
#             stock run state (kpm_poll)
#   task 20   FUN_4016DE5C(1)
# Stock BeatCounter frames are taken at the TX writers (p3_map.StockRun, binding 3 = BLE port 2), TG calls
# at FUN_4012F448 (note on) / FUN_4012F488 (note off) / FUN_4012EA00 (volume).
# Checks (as printed):
#   0  static: the bytes the module's rules come from (beat table 0x401235B8, set table 0x400FD88A via
#      literal 0x4014FDD8 - all 65 cells == kpm_beat_index, run-state literals 0x4016ED38.., reload flag
#      literal 0x4016FC34, accumulator literals 0x40166E68..0x40166E70 / 0x40166F04, bl sites of the 2 ms
#      chain); ARM build of kpmetro.c
# Every rig runs the boot settings init's song-task init FUN_4016E054(block + 0x12) after the boot copy
# (FUN_400AF648: FUN_400A9ACC, then FUN_4016E054 - it sets the half-beat position the click needs).
#   M0 the 2 ms handler's two calls signal the semaphores task 19's loop (pump tail 0x401539AC) and task
#      20's loop wait on (run, ids poisoned); 10 s idle: no BeatCounter, no TG note, the core makes no frame
#   M1 metronome on, 120, 4/4, 10 s: clicks 0x48 + 0x30 x3 on part 0x1BC every 250 cycles (500 ms),
#      BeatCounter to the binding port only; the core's frames == the stock's (bytes, order, <= 1 cycle late);
#      and without the boot init in rhythm mode (half-beat position 0): only the set's BeatCounter, core too
#   M2 sets while it runs (tempo, 6/8, volume, a refused beat, an out-of-range tempo, a refused rhythm, back
#      to 4/4, off/on): after every set the core's block bytes == stock block +0x49..+0x50, the core's
#      accumulator == stock 0x41915704/08; frames == stock; no double click; plus a modelled preemption of
#      FUN_4016EA18 by a beat set at two positions (the set's writes made between its counter loads and its
#      write-back; the core judges and polls inside that window, as task 19 would): frames == stock
#   M3 rhythm mode, rhythm 0 then 50 (reload in task 20): notes on part 0x1B8, no click; frames == stock
#   M4 a song playing (synthetic store, stock_songs) and a metronome tempo set: the song position speed
#      follows the metronome tempo (shared accumulator) - the core's FX_TEMPO prediction
#   M5 the core's store == the stock getter's value frames for the 6 metronome keys after every M2 set
#      (a raw-set mirror would differ); BeatCounter ports: kpm_stock_ports == where FUN_4014AD04 sent it for
#      bindings 1..4; fan-out = bound ports minus those
#   N  this piano's saved metronome settings (target/CA79NOR.BIN, read only): the core's cfg from the boot
#      copy == NOR bytes, M1 on them
from __future__ import annotations

import ctypes
import pathlib
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

p3 = rtlib.load("p3_map")
ss = rtlib.load("stock_songs")
SE = p3.StockEmu
REPO = rtlib.REPO
FW = REPO / "result/firmware"
LIB = FW / "build/libkpmetro.so"
NOR = REPO / "target/CA79NOR.BIN"
TC = rtlib.STOCKFW / ".toolchain/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-"

# stock (REA)
DEFAULTS, BOOT_COPY = 0x400B1448, 0x4015B384
UM = 0x40400004
TASK20 = 0x4016DE5C                 # FUN_4016DE5C(d)
NOTE_ON, NOTE_OFF, TG_VOL = 0x4012F448, 0x4012F488, 0x4012EA00
BEATCOUNTER = 0x4014BF24
EA18 = 0x4016EA18
FORWARD = 0x4014794C                # FUN_4014794C: port-3 forwarding on (FUN_4014AD04)
CFG = 0x418F1A2D                    # block 0x418F19E4 + 0x49
POS, LEN, HALF, BEAT, BPB, ON, RELOAD, REC_STATE = (0x4191F006, 0x4191F008, 0x4191F00A, 0x4191F00E, 0x4191F00F,
                                                    0x4191F010, 0x4191F0DC, 0x4191EF79)
ACC_Q, ACC_R = 0x41915704, 0x41915708
SONG_POS, SONG_TEMPO = 0x4191EF58, 0x4191570C
REC_CMD, CUR_BLOCK, SONG_TEMPO_INIT, PLAY_WAIT = 0x4016A050, 0x4015B4D8, 0x401673F8, 0x401650FC
SONG_INIT = 0x4016E054              # FUN_4016E054(block + 0x12): E908 (click part, FUN_4016E610) + EF68 at boot
H2MS, SIG19, SIG20 = 0x400A9D88, 0x40153838, 0x401738FC
SEM19, SEM20 = 0x413CA410, 0x41942854   # semaphore id words (literals 0x401539B4 / 0x401739F4)

QUIET = "-q" in sys.argv
FAILS = []


def check(name, ok, detail=""):
    if not ok:
        FAILS.append(name)
    return rtlib.check(name, ok, detail, quiet=QUIET)


hexs, le = p3.hexs, rtlib.le


# ---------------------------------------------------------------------------------------------- core (C)
def build_lib():
    r = subprocess.run(["make", "-s", "-C", str(FW), "build/libkpmetro.so"], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        raise SystemExit("make build/libkpmetro.so failed")
    return ctypes.CDLL(str(LIB))


U8 = ctypes.c_uint8


class Core:
    """kpmetro.c via tools/kpmetro_host.c."""

    def __init__(self, lib, cfg: bytes):
        self.lib = lib
        lib.kmh_init((U8 * 8)(*cfg))

    def set(self, v2, data):
        d = (U8 * max(1, len(data)))(*data)
        cfg, fr, info = (U8 * 8)(), (U8 * 12)(), (ctypes.c_uint32 * 3)()
        n = self.lib.kmh_set(v2, d, len(data), cfg, fr, info)
        return bytes(cfg), (bytes(fr) if n else None), tuple(info)

    def poll(self, e):
        s16 = lambda a: int.from_bytes(e.read_bytes(a, 2), "little", signed=True)
        b = lambda a: e.read_bytes(a, 1)[0]
        beat = b(BEAT)
        out = (U8 * 24)()
        n = self.lib.kmh_poll(s16(POS), s16(LEN), s16(HALF), beat - 256 if beat > 127 else beat, b(BPB), b(ON), b(RELOAD),
                              b(REC_STATE), out)
        return [bytes(out[12 * k:12 * k + 12]) for k in range(n)]

    def store_get(self, v2):
        o = (U8 * 4)()
        n = self.lib.kmh_store_get(v2, o)
        return bytes(o[:n]) if n > 0 else None

    def stats(self):
        a = (ctypes.c_uint32 * 7)()
        self.lib.kmh_stats(a)
        return dict(zip(["sets", "ignored", "refused", "beats", "starts", "reloads", "resyncs"], a))


# ---------------------------------------------------------------------------------------------- stock
class Rig:
    """The stock under the emulator + the core module, driven cycle by cycle (one cycle = 2 ms)."""

    def __init__(self, lib, state="D", nor=None, piano=None, boot=True, core=True):
        if piano is not None:
            self.pn, self.run = piano, piano.r
        else:
            self.pn, self.run = None, p3.StockRun()
        e = self.e = self.run.emu
        if state == "D":
            e.call(DEFAULTS, UM)
        else:
            e.write_bytes(UM - 4, nor[0x400000:0x400000 + 0x264E8])
        e.call(BOOT_COPY, UM)
        if boot:   # the boot settings init FUN_400AF648 runs the song-task init FUN_4016E054(block + 0x12) next
            r = e.call(SONG_INIT, e.call(CUR_BLOCK, 0).r0 + 0x12, max_insns=5_000_000)
            assert r.ok, r
        self.t = 0
        self.notes, self.vols, self.bc_calls = [], [], []
        e.hook(NOTE_ON, lambda em: (self.notes.append((self.t, "on", em.reg(0), em.reg(1), em.reg(2))), SE.CONTINUE)[1])
        e.hook(NOTE_OFF, lambda em: (self.notes.append((self.t, "off", em.reg(0), em.reg(1), 0)), SE.CONTINUE)[1])
        e.hook(TG_VOL, lambda em: (self.vols.append((self.t, em.reg(0), em.reg(1))), SE.CONTINUE)[1])
        e.hook(BEATCOUNTER, lambda em: (self.bc_calls.append((self.t, em.reg(1))), SE.CONTINUE)[1])
        e.hook(PLAY_WAIT, lambda em: 0)
        self.core = Core(lib, self.cfg()) if core else None   # the shim holds ONE module instance
        self.stock_frames = []      # (time, port, frame) BeatCounter frames at the TX writers
        self.core_frames = []       # (time, frame) the core's
        self.set_log = []           # per set: (key, data, stock cfg, core cfg, info, stock acc, ok)

    def cfg(self):
        return bytes(self.e.read_bytes(CFG, 8))

    def acc(self):
        return le(self.e.read_bytes(ACC_Q, 4)), le(self.e.read_bytes(ACC_R, 4))

    def _take_sent(self, t):
        for port, m in self.run.sent:
            if bytes(m[3:9]) == b"\x32\x04\x1E\x56\x0B\x7F":
                self.stock_frames.append((t, port, bytes(m)))
        self.run.sent = []

    def set(self, v2, data):
        """A set as task 19 handles it: the stock end handler + apply, and the core's judgement of it."""
        self.run.deliver(bytes([0xF0, 0x40, 0x7F, 0x10, 0x08, 0x02, 0x56, v2, 0x7F, *data]))
        self._take_sent(self.t)
        ccfg, fr, info = self.core.set(v2, data)
        if fr:
            self.core_frames.append((self.t, fr))
        scfg = self.cfg()
        q, r = self.acc()
        ok = ccfg == scfg
        self.set_log.append(((0x56, v2), bytes(data), scfg, ccfg, info, (q, r), ok))
        return ok

    def flush(self):
        """The next task-19 pump tail's poll (sees the last task-20 pass), no new cycle."""
        for fr in self.core.poll(self.e):
            self.core_frames.append((self.t, fr))

    def cycle(self, n=1, sets=()):
        for _ in range(n):
            for v2, d in sets:
                self.set(v2, d)
            sets = ()
            for fr in self.core.poll(self.e):                    # task 19 pump tail
                self.core_frames.append((self.t, fr))
            self.run.sent = []
            r = self.e.call(TASK20, 1, max_insns=2_000_000)     # task 20
            assert r.ok, r
            self._take_sent(self.t + 0.5)
            self.t += 1


def compare_frames(rig, tag, since=0):
    rig.flush()
    s = [(t, f) for t, _p, f in rig.stock_frames if t >= since]
    c = [(t, f) for t, f in rig.core_frames if t >= since]
    same = [f for _t, f in s] == [f for _t, f in c]
    lag = [ct - st for (st, _), (ct, _) in zip(s, c)]
    ok = same and all(0 <= x <= 1 for x in lag)
    vals = [f[9] for _t, f in s]
    check(f"{tag}: core BeatCounter frames == stock (bytes, order, <= 1 cycle late)", ok,
          f"stock {len(s)} core {len(c)}, values {vals[:12]}{'...' if len(vals) > 12 else ''}, lag {sorted(set(lag))}")
    if not ok and not QUIET:
        print("      stock:", [(t, f[9]) for t, f in s][-8:])
        print("      core: ", [(t, f[9]) for t, f in c][-8:])
    return ok


def clicks(rig, since=0, part=0x1BC):
    return [(t, n) for t, k, p, n, _v in rig.notes if k == "on" and p == part and t >= since]


# ---------------------------------------------------------------------------------------------- checks
def static(lib):
    img = rtlib.load("patcher").stock_bytes()
    w = lambda va: le(img, va - 0x40060000)
    by = lambda va, n: img[va - 0x40060000:va - 0x40060000 + n]
    beat = by(0x401235B8, 40)
    ent = (ctypes.c_uint32 * 2)()
    rows = []
    for i in range(12):
        lib.kmh_beat_entry(i, ent)
        j = i if i <= 9 else 3
        rows.append((ent[0], ent[1]) == (le(beat, 4 * j, 2), beat[4 * j + 2]))
    check("0 static: beat table 0x401235B8 (literal 0x4016ED38) == kpm_beat_entry (index > 9 -> 3)",
          w(0x4016ED38) == 0x401235B8 and all(rows), hexs(beat))
    tab = w(0x4014FDD8)
    cells = by(tab, 65)
    bad = [(num, den) for den in range(6) for num in range(14)
           if lib.kmh_beat_index(num, den) != ((cells[den * 13 + num] if cells[den * 13 + num] != 0xFF else -1)
                                               if num < 13 and den < 5 else -1)]
    check("0 static: BeatSelect set table (literal 0x4014FDD8) == kpm_beat_index, all cells", tab == 0x400FD88A and not bad,
          f"table {tab:#x}, mismatches {bad[:6]}")
    lits = [w(0x4016ED3C + 4 * k) for k in range(6)]
    check("0 static: run-state literals 0x4016ED3C.. = len, bpb, half, on, beat, pos; reload flag 0x4016FC34",
          lits == [LEN, BPB, HALF, ON, BEAT, POS] and w(0x4016FC34) == RELOAD, " ".join(f"{x:#x}" for x in lits))
    check("0 static: accumulator FUN_40165ED4: /60000 (0x40166E6C), quotient 0x41915704, rest 0x41915708",
          w(0x40166E6C) == 60000 and w(0x40166E70) == ACC_Q and w(0x40166F04) == ACC_R)
    bl = lambda va: (va + 8 + (((w(va) & 0xFFFFFF) ^ 0x800000) - 0x800000) * 4) if (w(va) >> 24) == 0xEB else None
    check("0 static: 2 ms chain bl 0x400A9D90 -> FUN_40153838 (task 19), 0x400A9D94 -> FUN_401738FC (task 20),"
          " task 20 loop 0x401739E4 -> FUN_4016DE5C",
          bl(0x400A9D90) == SIG19 and bl(0x400A9D94) == SIG20 and bl(0x401739E4) == TASK20,
          f"{bl(0x400A9D90):#x} {bl(0x400A9D94):#x} {bl(0x401739E4):#x}")
    # freestanding ARM compile of the module (the cave build later links it)
    flags = ["-mcpu=arm926ej-s", "-marm", "-mno-unaligned-access", "-Os", "-ffreestanding", "-fno-pic", "-fno-builtin",
             "-fno-common", "-fno-stack-protector", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra",
             "-Werror", "-std=c11", f"-I{FW}/include", f"-I{FW}/src"]
    if (TC.parent).exists():
        with tempfile.TemporaryDirectory() as tmp:
            o = pathlib.Path(tmp) / "kpmetro.o"
            subprocess.run([f"{TC}gcc", *flags, "-c", str(FW / "src/kproto/kpmetro.c"), "-o", str(o)], check=True)
            size = subprocess.run([f"{TC}size", str(o)], capture_output=True, text=True, check=True).stdout.split("\n")[1].split()
            und = set(subprocess.run([f"{TC}nm", "-u", str(o)], capture_output=True, text=True, check=True).stdout.split()) - {"U"}
        allowed = {"memcpy", "memset", "memcmp", "param_put", "param_slot_from_part", "__aeabi_uidiv", "__aeabi_uidivmod"}
        check("0 ARM build: kpmetro.c compiles freestanding (arm926ej-s, -Os), externals only libc / store",
              und <= allowed, f"text {size[0]} B, data {size[1]} B, bss {size[2]} B; externals {sorted(und)}")
    else:
        print("SKIP 0 ARM build (pinned toolchain absent)")


def m0(lib):
    rig = Rig(lib)
    e = rig.e
    gives = []
    e.hook(p3.SEM_GIVE, lambda em: (gives.append(em.reg(0)), 0)[1])
    e.write_u32(SEM19, 0x19)                   # ids are made at boot (cre_sem); poisoned here to tell them apart
    e.write_u32(SEM20, 0x20)
    r19, r20 = e.call(SIG19, max_insns=100_000), e.call(SIG20, max_insns=100_000)
    img = rtlib.load("patcher").stock_bytes()
    w = lambda va: le(img, va - 0x40060000)
    waits = (w(0x401539B4) == SEM19 and w(0x401739F4) == SEM20 and w(0x40153970) >> 24 == 0xEB
             and w(0x401539AC) >> 24 == 0xEB)
    check("M0 2 ms handler: FUN_40153838 sig_sem(*0x413CA410) = task 19's loop FUN_4015395C wai_sem (0x40153970, pump"
          " tail 0x401539AC in the same loop), FUN_401738FC sig_sem(*0x41942854) = task 20's FUN_401739B0 wai_sem",
          r19.ok and r20.ok and gives == [0x19, 0x20] and waits, f"sig_sem ids {[hex(x) for x in gives]}")
    rig.cycle(5000)
    st = rig.core.stats()
    check("M0 10 s idle (metronome off): no BeatCounter, no click note, the core makes no frame",
          not rig.stock_frames and not rig.bc_calls and not clicks(rig) and not rig.core_frames,
          f"stock {len(rig.stock_frames)} core {len(rig.core_frames)} notes {len(rig.notes)}, polls 5000, stats {st}")


def m1(lib, rig, tag="M1"):
    t0 = rig.t
    rig.cycle(1, [(0x08, [1])])
    rig.cycle(5000 - 1)
    cl = clicks(rig, t0)
    notes = [n for _t, n in cl]
    gaps = sorted({b - a for (a, _), (b, _) in zip(cl, cl[1:])})
    bpb = rig.e.read_bytes(BPB, 1)[0]
    pat = [0x48] + [0x30] * (bpb - 1)
    check(f"{tag}: clicks on part 0x1BC: 0x48 then 0x30 x{bpb - 1} repeating, every 250 cycles (500 ms at 120)",
          notes == (pat * 40)[:len(notes)] and len(notes) >= 19 and gaps in ([250], [249, 250, 251], [249, 250], [250, 251]),
          f"{len(notes)} clicks, gaps {gaps}, first at cycle {cl[0][0] - t0 if cl else None}")
    ports = {p for _t, p, _f in rig.stock_frames}
    check(f"{tag}: the stock sends BeatCounter to its binding port only (binding 3 = BLE port 2)", ports == {p3.PORT},
          f"ports {sorted(ports)}")
    compare_frames(rig, tag, t0)


def m1_noboot(lib):
    """Without the boot init FUN_4016E054 (its FUN_4016E908 -> FUN_4016E610 sets the half-beat position) and
    in rhythm mode (FUN_4016EF68 does not set it) the half-beat position stays 0: FUN_4016EA18 then marks
    every wrap as a half beat (0x80) and task 20 sends no BeatCounter - the core's half != 0 rule."""
    rig = Rig(lib, boot=False)
    rig.cycle(1, [(0x0A, [1])])
    rig.cycle(1, [(0x08, [1])])
    rig.cycle(1500)
    half = le(rig.e.read_bytes(HALF, 2), 0, 2)
    check("M1 rhythm mode without the boot init (half-beat position 0): BeatCounter only from the sets",
          half == 0 and [f[9] for _t, _p, f in rig.stock_frames] == [1],
          f"half {half}, stock frames {len(rig.stock_frames)}")
    compare_frames(rig, "M1 rhythm mode without the boot init")


def m2(lib):
    rig = Rig(lib)
    rig.cycle(1, [(0x08, [1])])
    rig.cycle(300)
    plan = [(0x01, [0x00, 0x64]),           # tempo 100
            (0x02, [0x06, 0x03]),           # 6/8
            (0x03, [0x40]),                 # volume
            (0x02, [0x07, 0x02]),           # refused beat (7/4): restart anyway
            (0x01, [0x00, 0x05]),           # tempo 5: stored, accumulator 120
            (0x09, [0x64]),                 # rhythm 100: refused
            (0x01, [0x01, 0x7F]),           # tempo 255
            (0x08, [0x01, 0x02]),           # wrong length: not applied
            (0x02, [0x04, 0x02]),           # back to 4/4
            (0x08, [0x00]),                 # off
            (0x08, [0x01])]                 # on
    t0 = 0
    for k, s in enumerate(plan):
        rig.cycle(1, [s])
        rig.cycle(377 + 13 * k)              # sets land at various places in the beat
    bad = [(hexs(bytes([*key, 0x7F])), hexs(d), sc.hex(), cc.hex()) for key, d, sc, cc, _i, _a, ok in rig.set_log if not ok]
    check("M2 after every set the core's block bytes == stock block +0x49..+0x50", not bad,
          f"{len(rig.set_log)} sets; {bad[:3]}")
    accbad = []
    for key, d, sc, cc, info, acc, ok in rig.set_log:
        if info[1] & 0x10:
            q, r = (ctypes.c_uint32 * 2)(), None
            bpm = lib.kmh_accum(le(cc, 5, 2), q)
            if (q[0], q[1]) != acc or bpm != info[2]:
                accbad.append((hexs(d), (q[0], q[1]), acc))
    check("M2 the core's tempo accumulator (FX_TEMPO) == stock 0x41915704/08 after every set that rewrites it",
          not accbad, f"{accbad[:3]}" if accbad else
          ", ".join(f"{hexs(d)}->{i[2]} bpm" for _k, d, _s, _c, i, _a, _o in rig.set_log if i[1] & 0x10))
    st = rig.core.stats()
    check("M2 verdicts: 1 ignored (wrong length), 2 refused (beat 7/4, rhythm 100)",
          st["ignored"] == 1 and st["refused"] == 2, f"{st}")
    compare_frames(rig, "M2", t0)
    cl = clicks(rig)
    dbl = [(a, b) for (a, _), (b, _) in zip(cl, cl[1:]) if b - a < 20]
    check("M2 no double click (two click note-ons closer than 40 ms)", not dbl, f"{len(cl)} clicks, close pairs {dbl[:4]}")
    return rig


def m2_race(lib, start):
    """Modelled preemption (metronome race): task 19 applies a beat set (6/8) while task 20 is inside
    FUN_4016EA18 after it loaded the counters into registers.  The set's RAM writes - taken from a second
    emulator that applied the same set to the same state (E668 writes absolute values) - are made from a hook
    at that instruction; its BeatCounter 1 is counted as the stock's.  The core judges the same set."""
    rig = Rig(lib)
    rig.cycle(1, [(0x08, [1])])
    rig.cycle(250 + 2)                             # beat 2 just started
    e = rig.e
    ref = Rig(lib, core=False)
    ref.e.write_bytes(CFG, rig.cfg())
    ref.e.write_bytes(POS, e.read_bytes(POS, 11))
    ref.run.deliver(bytes([0xF0, 0x40, 0x7F, 0x10, 0x08, 0x02, 0x56, 0x02, 0x7F, 0x06, 0x03]))
    assert [m for _p, m in ref.run.sent if m[3] == 0x32] == [bytes.fromhex("F0407F32041E560B7F017FF7")]
    snap = {a: bytes(ref.e.read_bytes(a, n)) for a, n in ((CFG, 8), (POS, 11))}
    fired = []
    LOADED = 0x4016EA54                            # after the five counter loads 0x4016EA30..0x4016EA50 (capstone)

    def preempt(em):
        if fired or em.reg(0) == 0 or le(em.read_bytes(POS, 2), 0, 2) < start:
            return SE.CONTINUE
        for a, b in snap.items():
            em.write_bytes(a, b)
        # task 19 while task 20 waits: the set's BeatCounter 1 (from the reference apply), the core's
        # judgement of the same set and its pump-tail poll
        rig.stock_frames.append((rig.t, p3.PORT, bytes.fromhex("F0407F32041E560B7F017FF7")))
        ccfg, fr, _info = rig.core.set(0x02, [0x06, 0x03])
        if fr:
            rig.core_frames.append((rig.t, fr))
        rig.flush()
        fired.append((rig.t, ccfg))
        return SE.CONTINUE
    e.hook(LOADED, preempt)
    before = None
    for _ in range(300):
        before = bytes(e.read_bytes(POS, 11))
        rig.cycle(1)
        if fired:
            break
    e.unhook(LOADED)
    t = fired[0][0] if fired else None
    after = bytes(e.read_bytes(POS, 11))
    rig.cycle(1000)
    kept = le(after, 0, 2) >= le(after, 2, 2)      # stale position >= new length: wraps at the next pass anyway
    cl = clicks(rig, t)
    check(f"M2 race (modelled) at position {start:#x}: FUN_4016EA18 writes the stale position back over the restart",
          bool(fired) and fired[0][1] == snap[CFG],
          f"cycle {t}: core cfg {fired[0][1].hex() if fired else None} stock {snap[CFG].hex()}; before {before.hex()} set wrote {snap[POS].hex()} after {after.hex()}"
          f" -> {'same as a restart (wraps next pass)' if kept else 'restart delayed: bar continues to the new length'};"
          f" next clicks at {[(c - t, n) for c, n in cl[:3]]}")
    compare_frames(rig, f"M2 race at {start:#x}")


def m3(lib):
    rig = Rig(lib)
    rig.cycle(1, [(0x0A, [0x01])])
    rig.cycle(1, [(0x08, [0x01])])
    rig.cycle(2500)
    r0 = clicks(rig, 0, 0x1B8)
    bpb0 = rig.e.read_bytes(BPB, 1)[0]
    t1 = rig.t
    rig.cycle(1, [(0x09, [50])])
    rig.cycle(2500)
    r1 = clicks(rig, t1, 0x1B8)
    bpb1, len1 = rig.e.read_bytes(BPB, 1)[0], le(rig.e.read_bytes(LEN, 2), 0, 2)
    check("M3 rhythm mode: notes on part 0x1B8 for rhythm 0 and 50, no click on 0x1BC",
          len(r0) > 10 and len(r1) > 10 and not clicks(rig),
          f"rhythm 0: {len(r0)} notes ({bpb0} beats/bar), rhythm 50: {len(r1)} notes ({bpb1} beats/bar, beat {len1} ticks)")
    st = rig.core.stats()
    check("M3 the rhythm reload in task 20 (FUN_4016FBC0 -> FUN_4016EF68) gives BeatCounter 1 - the core sees the flag fall",
          st["reloads"] == 1, f"{st}")
    compare_frames(rig, "M3")


def m4(lib):
    pn = ss.Piano()
    rig = Rig(lib, piano=pn)
    pn.send(ss.song_select(1))
    rig.e.call(REC_CMD, rig.e.call(CUR_BLOCK, 0).r0, 2)
    rig.e.call(SONG_TEMPO_INIT, 120)
    rig.e.call(REC_CMD, rig.e.call(CUR_BLOCK, 0).r0, 1)
    pos = lambda: le(rig.e.read_bytes(SONG_POS, 4))
    rig.cycle(10)
    a = pos()
    rig.cycle(500)
    b = pos()
    rig.cycle(1, [(0x01, [0x00, 60])])          # metronome tempo 60 while the song plays (metronome off)
    c = pos()
    rig.cycle(500)
    d = pos()
    st, mt = rig.e.read_bytes(REC_STATE, 1)[0], le(rig.e.read_bytes(SONG_TEMPO, 2), 0, 2)
    q = (ctypes.c_uint32 * 2)()
    lib.kmh_accum(60, q)
    check("M4 a metronome tempo set moves a playing song (shared accumulator) - as the core's FX_TEMPO predicts",
          st == 1 and abs((b - a) - 192) <= 1 and abs((d - c) - 96) <= 1 and rig.acc() == (q[0], q[1]) and mt == 120,
          f"state {st}, song tempo byte {mt} unchanged, position +{b - a} ticks / 1 s at 120 -> +{d - c} at 60"
          f" (96 per quarter), accumulator {rig.acc()}")


def m5(lib, rig):
    # store == getter after every M2 set: replay the M2 sets on a fresh pair and ask both after each
    r = Rig(lib)
    bad, mirror_diff = [], 0
    for key, d, *_ in rig.set_log:
        r.set(key[1], list(d))
        for v2 in (0x01, 0x02, 0x03, 0x08, 0x09, 0x0A):
            r.run.request(0x56, v2, 0x7F)
            f = [m for _p, m in r.run.sent if m[3] == 0x10]
            stock = bytes(f[0][9:-1]) if f else None
            core = r.core.store_get(v2)
            if stock != core:
                bad.append((hexs(d), v2, stock, core))
        if key[1] in (0x02, 0x09) and len(d) == (2 if key[1] == 0x02 else 1):
            r.run.request(0x56, key[1], 0x7F)
            f = [m for _p, m in r.run.sent if m[3] == 0x10]
            if f and bytes(f[0][9:-1]) != bytes(d):
                mirror_diff += 1
        r.run.sent = []
    check("M5 the core's store == the stock getter's value frames, 6 keys after every M2 set", not bad,
          f"{len(rig.set_log)} sets x 6 keys; a raw-set mirror would differ after {mirror_diff} of them; {bad[:3]}")
    # where the stock sends BeatCounter, by binding (FUN_4014AD04)
    rows, okp = [], True
    for b in (1, 2, 3, 4):
        x = Rig(lib)
        x.e.write_bytes(p3.BINDING, bytes([b]))
        x.run.sent = []
        x.e.call(BEATCOUNTER, 0x7F, 1, 0x7F, max_insns=200_000)
        ports = sorted({p for p, m in x.run.sent})
        fwd = x.e.call(FORWARD, max_insns=100_000).r0 != 0
        exp = lib.kmh_stock_ports(b, int(fwd))
        okp &= sum(1 << p for p in ports) == exp
        rows.append(f"binding {b} fwd {int(fwd)} -> {ports}")
    check("M5 kpm_stock_ports == the ports FUN_4014AD04 wrote, bindings 1..4 (fan-out = bound ports minus these)",
          okp and lib.kmh_stock_ports(0, 0) == 0 and lib.kmh_stock_ports(4, 1) == 0x0F, "; ".join(rows))


def main():
    print("roundtrip_metro: Phase 3-5 metronome stage 1 - core judges, stock plays")
    lib = build_lib()
    static(lib)
    m0(lib)
    rig = Rig(lib)
    m1(lib, rig)
    m1_noboot(lib)
    rig2 = m2(lib)
    m2_race(lib, 0x10)
    m2_race(lib, 0x50)
    m3(lib)
    m4(lib)
    m5(lib, rig2)
    if NOR.exists():
        nor = NOR.read_bytes()                  # read only - never written, never copied
        rn = Rig(lib, "N", nor)
        want = nor[0x42390C + 0x49:0x42390C + 0x49 + 8]
        check("N this piano's saved metronome settings: core cfg (boot copy) == NOR 0x60423955..0x6042395C",
              rn.cfg() == want, f"{want.hex()}")
        m1(lib, rn, "N M1")
    else:
        print("SKIP N (target/CA79NOR.BIN absent)")
    print("roundtrip_metro:", "all checks passed" if not FAILS else f"{len(FAILS)} FAILED")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
