# roundtrip_rec.py - Phase 3-6 R0: the core's read-only recorder view (result/firmware/src/recorder/recview.c,
# cave port ports/cave/recview_cave.c) built for ARM with the pinned toolchain and run INSIDE the stock-image
# emulator, compared with the stock firmware's own getters and checks in a few recorder scenarios.
#
#
# No image, no slot payload: the ARM build is linked to a free SDRAM window (RV_BASE) of the emulator only.
# The stock side is driven by the real stock routines (SongSelect / IntSongPart through the SysEx apply path,
# recorder commands FUN_4016A050, record-ring consumer FUN_4016CCDC, task-20 tick FUN_4016DE5C) on a
# SYNTHETIC song store (result/stockfw/tools/stock_songs.py) - plus, when target/CA79NOR.BIN is present, the
# device's own RecorderData loaded into emulator RAM (read only, nothing leaves the process, only structure
# numbers are printed).  The NOR write job FUN_40134048 is recorded, never run: no flash write can happen.
#
# Checks per scenario (S = stock getter / check run in the emulator, C = core view from the ARM build):
#   state  C == S FUN_40167824     song  C == S FUN_40167A40     record part  C == S FUN_40167A4C
#   MusicMode  C == S FUN_4015C244 NOR job  C(JOB) == S FUN_401342AC     table ok  C == (S FUN_4016E174 == 0)
#   SoundMode  C(song flag) == S FUN_40165734 for the selected song   bars  C(max_bars) == the MaxTime the stock
#   sends at SongSelect (descriptor 0xA4) and C(song bars) == header +0x70
#   the core's call writes nothing outside its own window and the stack (stock RAM untouched), and no NOR job
#   was queued by any scenario except the explicit "job queued" one.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_rec.py [-q]   (-q accepted for the Makefile; output is the same)
from __future__ import annotations

import pathlib
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

REPO = rtlib.REPO
FW = REPO / "result/firmware"
TC = rtlib.STOCKFW / ".toolchain/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-"
NOR_DUMP = REPO / "target/CA79NOR.BIN"

ss = rtlib.load("stock_songs")
p3 = ss.p3

# ---- stock routines (REA) ----
GET_STATE, GET_SONG, GET_RECPART, GET_MODE = 0x40167824, 0x40167A40, 0x40167A4C, 0x4015C244
JOBS_ACTIVE = 0x401342AC         # FUN_401342AC: job table slot 0 or 1 pending
BOOT_CHECK = 0x4016E174          # FUN_4016E174: 0 = the boot path keeps the store
IS_SOUNDMODE = 0x40165734        # FUN_40165734: selected song's name +8.. == "- SoundMode"
REC_CMD = 0x4016A050             # FUN_4016A050(blk, cmd)
CUR_BLOCK = 0x4015B4D8           # FUN_4015B4D8(0) -> current settings block
RING_CONSUME = 0x4016CCDC        # FUN_4016CCDC: record ring (FA start / FC stop) in task 20
TASK20_TICK = 0x4016DE5C         # FUN_4016DE5C(d): one task-20 pass
PLAY_WAIT = 0x401650FC           # FUN_401650FC: play start polls it while 1 (DSP side) -> 0
SONG_TEMPO_INIT = 0x401673F8     # FUN_401673F8(bpm): 0x4191570E = 0x4191570C = bpm, accumulator (song load)
SONG_TEMPO_SET = 0x40167294      # FUN_40167294(bpm): 0x4191570C = bpm, ratio vs 0x4191570E, accumulator
JOB_KICK = 0x4013433C           # FUN_4013433C: wakes the NOR worker (not modelled) -> no-op

# ---- the ARM build ----
RV_BASE = 0x41E00000             # code (free SDRAM window in the emulator)
RV_DATA = 0x41E40000             # .data/.bss
RV_SCRATCH = 0x41E50000          # rv_view_t
RV_OUT = 0x41E51000              # encoded view
RV_WIN = (0x41E00000, 0x41E60000)
ENC_LEN = 32 + 7 * 10
CFLAGS = ["-mcpu=arm926ej-s", "-marm", "-mno-unaligned-access", "-Os", "-ffreestanding", "-fno-pic", "-fno-builtin",
          "-fno-common", "-fno-zero-initialized-in-bss", "-fno-stack-protector", "-ffunction-sections",
          "-fdata-sections", "-fno-tree-loop-distribute-patterns", "-Wall", "-Wextra", "-Werror", "-std=c11",
          f"-I{FW}/include", f"-I{FW}/src"]

ST_NAMES = {0: "stopped", 1: "play", 2: "stop", 3: "rec standby", 4: "recording", 5: "rec standby (no metro)",
            6: "count-in"}


def build_arm(tmp: pathlib.Path) -> tuple[bytes, dict]:
    objs = []
    for src in (FW / "src/recorder/recview.c", FW / "ports/cave/recview_cave.c"):
        o = tmp / (src.stem + ".o")
        subprocess.run([f"{TC}gcc", *CFLAGS, "-c", str(src), "-o", str(o)], check=True)
        objs.append(str(o))
    elf = tmp / "recview.elf"
    subprocess.run([f"{TC}ld", "--no-undefined", "-static", "--gc-sections", "-e", "rv_cave_snapshot",
                    "-u", "rv_cave_poll", f"-Ttext=0x{RV_BASE:08X}", f"-Tdata=0x{RV_DATA:08X}",
                    "--no-warn-rwx-segments", *objs, "-o", str(elf)], check=True)
    nm = subprocess.run([f"{TC}nm", str(elf)], check=True, capture_output=True, text=True).stdout
    syms = {ln.split()[2]: int(ln.split()[0], 16) for ln in nm.splitlines() if len(ln.split()) == 3}
    # one flat blob from RV_BASE (text, rodata, data)
    secs = subprocess.run([f"{TC}objdump", "-h", str(elf)], check=True, capture_output=True, text=True).stdout
    binf = tmp / "recview.bin"
    subprocess.run([f"{TC}objcopy", "-O", "binary", str(elf), str(binf)], check=True)
    blob = binf.read_bytes()
    assert "ALLOC" in secs and len(blob) < RV_DATA + 0x10000 - RV_BASE, len(blob)
    und = subprocess.run([f"{TC}nm", "-u", str(elf)], check=True, capture_output=True, text=True).stdout.strip()
    assert not und, f"undefined symbols in the ARM build: {und}"
    return blob, syms


def decode(b: bytes) -> dict:
    v = dict(ver=b[0], state=b[1], transport=b[2], song=b[3], rec_part=b[4], play_sel=b[5], music_mode=b[6],
             busy=b[7], tempo=struct.unpack_from("<H", b, 8)[0], tempo_orig=struct.unpack_from("<H", b, 10)[0],
             pos=struct.unpack_from("<I", b, 12)[0], max_bars=struct.unpack_from("<H", b, 16)[0], max_slot=b[18],
             store=b[19], cur_bar=struct.unpack_from("<H", b, 20)[0], ring=struct.unpack_from("<H", b, 22)[0],
             store_end=struct.unpack_from("<I", b, 24)[0], store_bad=b[28], songs=[])
    for i in range(10):
        ln, bars, fl = struct.unpack_from("<IHB", b, 32 + 7 * i)
        v["songs"].append((ln, bars, fl))
    return v


class Rig:
    def __init__(self, blob, syms, store=None):
        self.pn = ss.Piano()
        e = self.e = self.pn.e
        if store is not None:          # replace the synthetic store (the device's own, from the dump)
            e.write_bytes(ss.REC, store)
        e.write_bytes(RV_BASE, blob)
        self.syms = syms
        e.hook(PLAY_WAIT, lambda em: 0)
        e.hook(JOB_KICK, lambda em: None)
        self.blk = self.call(CUR_BLOCK, 0)

    def call(self, fn, *a, n=20_000_000):
        r = self.e.call(fn, *a, max_insns=n)
        assert r.ok, (hex(fn), r)
        return r.r0

    def core(self):
        r = self.e.call(self.syms["rv_cave_snapshot"], RV_SCRATCH, RV_OUT, max_insns=2_000_000)
        assert r.ok and r.r0 == ENC_LEN, r
        stray = [w for w in r.writes if not (RV_WIN[0] <= w.addr < RV_WIN[1] or 0x40F00000 <= w.addr < 0x41000000)]
        return decode(self.e.read_bytes(RV_OUT, ENC_LEN)), stray, r.insns

    def stock(self):
        return dict(state=self.call(GET_STATE) & 0xFF, song=self.call(GET_SONG) & 0xFF,
                    rec_part=self.call(GET_RECPART) & 0xFF, music_mode=self.call(GET_MODE) & 0xFF,
                    jobs=self.call(JOBS_ACTIVE) & 0xFF, boot_ok=self.call(BOOT_CHECK) == 0,
                    soundmode=self.call(IS_SOUNDMODE) & 0xFF)

    def u(self, a, n):
        return int.from_bytes(self.e.read_bytes(a, n), "little")


def main():
    fails = []

    def check(cond, what):
        if not rtlib.check(what, cond):
            fails.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        blob, syms = build_arm(pathlib.Path(tmp))
    print(f"ARM build: {len(blob)} B at {RV_BASE:#x}, rv_cave_snapshot {syms['rv_cave_snapshot']:#x}")

    def compare(rig, label, *, maxtime=None, layout=ss.LAYOUT, jobs_expected=0):
        c, stray, insns = rig.core()
        s = rig.stock()
        print(f"{label}: state {c['state']} ({ST_NAMES.get(c['state'], '?')}) song {c['song'] + 1} part rec {c['rec_part']}"
              f" play {c['play_sel']} mode {c['music_mode']} tempo {c['tempo']} pos {c['pos']} bar {c['cur_bar']}"
              f"/{c['max_bars']} busy {c['busy']:#x} store {c['store']} ({insns} insns)")
        check(c["ver"] == 1, f"{label}: encoding version 1")
        for k in ("state", "song", "rec_part", "music_mode"):
            check(c[k] == s[k], f"{label}: {k} core {c[k]} == stock getter {s[k]}")
        check(bool(c["busy"] & 1) == bool(s["jobs"]), f"{label}: NOR job bit == FUN_401342AC() ({s['jobs']})")
        check((c["store"] == 0) == s["boot_ok"], f"{label}: table ok ({c['store']}) == boot check FUN_4016E174 passes ({s['boot_ok']})")
        if c["store"] == 0:
            check(bool(c["songs"][c["song"]][2] & 0x10) == bool(s["soundmode"]),
                  f"{label}: SoundMode flag of song {c['song'] + 1} == FUN_40165734() ({s['soundmode']})")
        # fields without a stock getter: the same addresses read independently here
        raw = dict(play_sel=rig.u(0x4191EF72, 1), tempo=rig.u(0x4191570C, 2), tempo_orig=rig.u(0x4191570E, 2),
                   pos=rig.u(0x4191EF58, 4), max_bars=rig.u(0x4191EF70, 2), max_slot=rig.u(0x4191EF76, 1),
                   ring=rig.u(0x4191EF6C, 2))
        check(all(c[k] == v for k, v in raw.items()), f"{label}: play part / tempo / position / MaxTime / ring == stock RAM")
        if maxtime is not None:
            check(c["max_bars"] == maxtime, f"{label}: MaxTime bars {c['max_bars']} == stock MaxTime notification {maxtime}")
        if layout is not None and c["store"] == 0:
            want = [max(layout[i]) if i < len(layout) else 0 for i in range(10)]
            check([g[1] for g in c["songs"]] == want, f"{label}: per-song bars == synthetic layout {want}")
            parts = [((1 if i < len(layout) and layout[i][0] else 0) | (2 if i < len(layout) and layout[i][1] else 0))
                     for i in range(10)]
            check([g[2] & 3 for g in c["songs"]] == parts, f"{label}: per-song parts == layout")
        check(not stray, f"{label}: the core's read wrote nothing outside its window ({len(stray)} stray writes)")
        check(len(rig.pn.jobs) == jobs_expected, f"{label}: NOR jobs queued by the scenario = {len(rig.pn.jobs)}")
        return c

    def maxtime_of(frames):
        for f in frames:
            h = p3.hexs(f)
            if h.startswith("F0 40 7F 10 04 1E 52 18 7F 00"):
                return (f[10] << 7) | f[11]
        return None

    # 1. idle: SongSelect song 2 (2 + 5 bars), then Stop -> state 2
    rig = Rig(blob, syms)
    _, fr = rig.pn.send(ss.song_select(1))
    mt = maxtime_of(fr)
    rig.call(REC_CMD, rig.blk, 2)
    c = compare(rig, "idle (song 2 selected, Stop)", maxtime=mt)
    check(c["state"] == 2 and c["transport"] == 0 and c["busy"] == 0, "idle: state 2, transport idle, not busy")

    # 2. playing: the song's tempo as the loader sets it (FUN_401673F8(120): original = current = 120; the
    # emulator has no boot, so the accumulator starts at 0), a tempo change (FUN_40167294(144), MusicTempo),
    # Play, then 1000 task-20 passes of 2 ms (2 s at 144 bpm = 460.8 ticks of 96 per quarter = bar 2 of 4/4)
    rig.call(SONG_TEMPO_INIT, 120)
    rig.call(SONG_TEMPO_SET, 144)
    c = compare(rig, "tempo set", maxtime=mt)
    check(c["tempo"] == 144 and c["tempo_orig"] == 120, "tempo: current 144, original 120")
    rig.call(REC_CMD, rig.blk, 1)
    c0, _, _ = rig.core()
    for _ in range(1000):
        rig.call(TASK20_TICK, 1)
    c = compare(rig, "playing (Play + 1000 task-20 ticks)", maxtime=mt)
    check(c["state"] == 1 and c["transport"] == 1, "playing: state 1, transport playing")
    check(c0["pos"] == 0 and abs(c["pos"] - 461) <= 2, f"playing: position {c0['pos']} -> {c['pos']} ticks (2 s at 144 bpm = 461)")
    check(c["cur_bar"] == 2, f"playing: bar {c['cur_bar']} == 2 (384 ticks per 4/4 bar)")
    rig.call(REC_CMD, rig.blk, 2)
    c = compare(rig, "stopped after play", maxtime=mt)
    check(c["state"] == 2, "Stop after play: state 2")

    # 3. part select: IntSongPart 1 (part 2) on song 2 -> record part 1, play selector 2
    rig.pn.send(ss.int_song_part(1))
    c = compare(rig, "IntSongPart part 2")
    check(c["rec_part"] == 1 and c["play_sel"] == 2, "IntSongPart 1: record part 1, play selector 2 (FUN_401686FC)")
    rig.pn.send(ss.int_song_part(0x7F))
    c = compare(rig, "IntSongPart both")
    check(c["play_sel"] == 0 and c["rec_part"] == 1, "IntSongPart 7F: play selector 0, record part kept")

    # 4. recording armed (cmd 3), armed without metronome (cmd 5), count-in (cmd 6)
    for cmd, st, tr in ((3, 3, 2), (5, 5, 2), (6, 6, 3)):
        rig = Rig(blob, syms)
        rig.pn.send(ss.song_select(4))
        rig.call(REC_CMD, rig.blk, 2)
        rig.call(REC_CMD, rig.blk, cmd)
        c = compare(rig, f"rec command {cmd}")
        check(c["state"] == st and c["transport"] == tr and c["busy"] & 8,
              f"rec command {cmd}: state {st}, transport {tr}, busy REC")

    # 5. recording: Rec (cmd 4) puts FA in the ring; the task-20 ring consumer starts the recording
    rig = Rig(blob, syms)
    rig.pn.send(ss.song_select(4))
    rig.call(REC_CMD, rig.blk, 2)
    rig.call(REC_CMD, rig.blk, 3)
    rig.call(REC_CMD, rig.blk, 4)
    c = compare(rig, "Rec queued (FA in ring)")
    check(c["ring"] == 1, f"Rec: the record ring holds the FA ({c['ring']})")
    rig.call(RING_CONSUME)
    c = compare(rig, "recording (ring consumed)")
    check(c["state"] == 4 and c["transport"] == 4 and c["busy"] & 8 and c["ring"] == 0,
          "recording: state 4, transport recording, ring empty")

    # 6. a NOR job queued (the RecorderData slot): the enqueue routine itself, worker not run
    rig = Rig(blob, syms)
    rig.e.unhook(ss.NOR_JOB)
    rig.call(ss.NOR_JOB, 1, 1, ss.REC, 0)
    c = compare(rig, "NOR job queued (slot 1)")
    check(c["busy"] & 1, "job queued: core busy JOB")

    # 7. a broken table: a song start off by 2 -> the boot check fails, the core says so and lists no songs
    rig = Rig(blob, syms)
    off3 = rig.u(ss.REC + 12, 4)
    rig.e.write_bytes(ss.REC + 12, struct.pack("<I", off3 + 2))
    c = compare(rig, "misaligned song 4 start", layout=None)
    check(c["store"] == 3 and c["store_bad"] == 3 and all(g == (0, 0, 0) for g in c["songs"]),
          "misaligned: store SONG_ALIGN at song 4, no song entries")

    # 8. the device's own RecorderData (target/CA79NOR.BIN 0x500000, 1 MB) - read only, structure printed only
    if NOR_DUMP.is_file():
        store = NOR_DUMP.read_bytes()[0x500000:0x600000]
        rig = Rig(blob, syms, store=store)
        c, _, _ = rig.core()
        print("device store:", [(i + 1, hex(g[0]), g[1], hex(g[2])) for i, g in enumerate(c["songs"])],
              f"end {c['store_end']:#x}")
        compare(rig, "device store, idle", layout=None)
        offs = struct.unpack_from("<12I", store, 0)
        for n in range(10):
            _, fr = rig.pn.send(ss.song_select(n))
            mt = maxtime_of(fr)
            c, _, _ = rig.core()
            s = rig.stock()
            hdr = struct.unpack_from("<I", store, offs[n] + 0x70)[0]
            check(c["songs"][n][1] == hdr == (mt or 0) and c["max_bars"] == (mt or 0) and c["song"] == n == s["song"],
                  f"device song {n + 1}: core bars {c['songs'][n][1]} == header {hdr} == stock MaxTime {mt}")
            check(bool(c["songs"][n][2] & 0x10) == bool(s["soundmode"]),
                  f"device song {n + 1}: SoundMode flag == FUN_40165734() ({s['soundmode']})")
        check(not rig.pn.jobs, "device store: no NOR job")
    else:
        print("SKIP device store (target/CA79NOR.BIN absent)")

    print("all checks passed" if not fails else f"{len(fails)} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
