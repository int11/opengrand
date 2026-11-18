# roundtrip_recctl.py - Phase 3-6 R1: the core's playback control (result/firmware/src/recorder/recctl.c + recview,
# emulator glue result/firmware/tests/arm/recctl_arm.c) built for ARM with the pinned toolchain and run INSIDE the
# stock-image emulator next to a second, identical emulator driven the stock way (the panel / the app: SysEx frames
# through the stock apply FUN_4016053C).  Both run the stock song player (task-20 pass FUN_4016DE5C) on the same
# SYNTHETIC song store (tools/stock_songs.py).
#
#
# S = stock: SysEx frame -> stock apply (task 19) -> Music queue / direct routine -> task-20 passes.
# C = core:  rca_cmd(cmd, arg) -> recctl decides -> the same queue post FUN_4015D70C / the same stock routine as the
#            apply -> the same task-20 passes; rca_poll() reads the result back with recview.
# Checks per accepted command: the recorder / player / queue / tempo RAM of C == S byte for byte, the frames the
# stock sends to the controller (MaxTime 52 18, MusicProgress 32 52 03, Result) C == S (the fn 0x10 ACK `07` is the
# SysEx front's, not the recorder's), the recview fields C == S, and rca_poll() reports DONE for the command.
# Refusals (busy NOR job / file op, empty song, recording armed / running, deferred mode): C hands nothing over (no
# queue entry, no frame, recorder RAM unchanged) while S shows what the stock would have done.
# Over the whole C run: FUN_4016A050 never runs inside the core's call (only in task-20 passes), the commit path
# FUN_40168774 / FUN_40168408 and the NOR job FUN_40134048 never run, and the glue's own stores stay in its window
# and the stack.  Nothing is recorded and nothing is saved; the NOR job is recorded, never run.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_recctl.py [-q]   (-q: accepted, output is the same)
from __future__ import annotations

import pathlib
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
StockEmu = p3.StockEmu

# ---- stock routines (REA) ----
REC_CMD = 0x4016A050             # FUN_4016A050(blk, cmd): recorder command - callers FUN_4015DBD4 / FUN_4015DCC4 only
CUR_BLOCK = 0x4015B4D8           # FUN_4015B4D8(0) -> current settings block
RING_CONSUME = 0x4016CCDC        # FUN_4016CCDC: record ring consumer (task 20)
TASK20_TICK = 0x4016DE5C         # FUN_4016DE5C(d): one task-20 pass (drains the Music queue first)
PLAY_WAIT = 0x401650FC           # FUN_401650FC: play start polls it while 1 (DSP side) -> 0
SONG_TEMPO_INIT = 0x401673F8     # FUN_401673F8(bpm): song tempo = original tempo = bpm
JOB_KICK = 0x4013433C            # FUN_4013433C: wakes the NOR worker (not modelled)
COMMIT = 0x40168774              # FUN_40168774: recording commit (-> serialise -> FUN_40168408 -> NOR job)
STORE_WRITE = 0x40168408         # FUN_40168408: the only RecorderData writer
JOB_FLAG, FILEOP = 0x4139DAAC, 0x413CA3EA
MQ_HOLD, MQ_COUNT = 0x419156A4, 0x419156A5

# compared RAM: recorder / player state + record ring indices, Music queue + MusicMode + tempo block, NOR job table
WINDOWS = [(0x4191DB00, 0x4191F100), (0x41915598, 0x41915720), (0x4139DAA0, 0x4139DAD0)]

# ---- the ARM build ----
RC_BASE = 0x41E00000
RC_DATA = 0x41E40000
RC_OUT = 0x41E51000
RC_WIN = (0x41E00000, 0x41E60000)
STACK = (0x40F00000, 0x41000000)
CFLAGS = ["-mcpu=arm926ej-s", "-marm", "-mno-unaligned-access", "-Os", "-ffreestanding", "-fno-pic", "-fno-builtin",
          "-fno-common", "-fno-zero-initialized-in-bss", "-fno-stack-protector", "-ffunction-sections",
          "-fdata-sections", "-fno-tree-loop-distribute-patterns", "-Wall", "-Wextra", "-Werror", "-std=c11",
          f"-I{FW}/include", f"-I{FW}/src"]

RC = dict(PLAY=1, STOP=2, PAUSE=3, SELECT=4, PART=5, TEMPO=6, SEEK=7)
RES = {0: "OK", 1: "NOP", 2: "E_ARG", 3: "E_BUSY", 4: "E_REC", 5: "E_PENDING", 6: "E_NODATA", 7: "E_MODE", 8: "E_STATE"}
EV_DONE, EV_TIMEOUT = 1, 2


def build_arm(tmp: pathlib.Path) -> tuple[bytes, dict]:
    objs = []
    for src in (FW / "src/recorder/recview.c", FW / "src/recorder/recctl.c", FW / "src/recorder/recsave.c", FW / "src/recorder/recdel.c",
                FW / "tests/arm/recctl_arm.c"):
        o = tmp / (src.stem + ".o")
        subprocess.run([f"{TC}gcc", *CFLAGS, "-c", str(src), "-o", str(o)], check=True)
        objs.append(str(o))
    elf = tmp / "recctl.elf"
    subprocess.run([f"{TC}ld", "--no-undefined", "-static", "--gc-sections", "-e", "rca_cmd", "-u", "rca_poll", "-u", "rca_rec", "-u", "rca_state",
                    "-u", "rca_save", "-u", "rca_save_state", "-u", "rca_save_changed", "-u", "rca_delete", "-u", "rca_file",
                    f"-Ttext=0x{RC_BASE:08X}", f"-Tdata=0x{RC_DATA:08X}", "--no-warn-rwx-segments", *objs, "-o", str(elf)],
                   check=True)
    nm = subprocess.run([f"{TC}nm", str(elf)], check=True, capture_output=True, text=True).stdout
    syms = {ln.split()[2]: int(ln.split()[0], 16) for ln in nm.splitlines() if len(ln.split()) == 3}
    binf = tmp / "recctl.bin"
    subprocess.run([f"{TC}objcopy", "-O", "binary", str(elf), str(binf)], check=True)
    blob = binf.read_bytes()
    assert len(blob) < RC_DATA + 0x10000 - RC_BASE, len(blob)
    und = subprocess.run([f"{TC}nm", "-u", str(elf)], check=True, capture_output=True, text=True).stdout.strip()
    assert not und, f"undefined symbols in the ARM build: {und}"
    return blob, syms


def frame(*body):
    return bytes([0xF0, 0x40, 0x7F, *body])


def music(v):                    # Music, remote command form (descriptor 0xD0)
    return frame(0x32, 0x08, 0x02, 0x52, 0x08, 0x7F, v)


def music_mode(v):               # MusicMode, mode-change form (descriptor 0xCB)
    return frame(0x31, 0x08, 0x02, 0x52, 0x1D, 0x7F, v)


def tempo(bpm):                  # MusicTempo (0x92)
    return frame(0x10, 0x08, 0x02, 0x52, 0x02, 0x7F, (bpm >> 7) & 0x7F, bpm & 0x7F)


def seek(bar):                   # MusicProgress, remote command form, bar form (0xD1)
    return frame(0x32, 0x08, 0x02, 0x52, 0x03, 0x7F, 0, (bar >> 7) & 0x7F, bar & 0x7F, 0, 0, 0)


class Rig:
    """One stock-image emulator with the synthetic store, the core's ARM build loaded (unused on the S side)."""

    def __init__(self, name, blob, syms, store=None):
        self.name = name
        self.pn = ss.Piano()
        e = self.e = self.pn.e
        if store is not None:
            e.write_bytes(ss.REC, store)
        e.write_bytes(RC_BASE, blob)
        self.syms = syms
        e.hook(PLAY_WAIT, lambda em: 0)
        e.hook(JOB_KICK, lambda em: None)
        self.in_core = False
        self.rec_cmd_in_core = 0
        self.rec_cmd_calls = 0
        self.commits = 0
        self.store_writes = 0

        def on_rec_cmd(em):
            self.rec_cmd_calls += 1
            self.rec_cmd_in_core += self.in_core
            return StockEmu.CONTINUE

        def on_commit(em):       # never run the commit (no recording is made here): count and return
            self.commits += 1
            return 0

        def on_store_write(em):
            self.store_writes += 1
            return 0

        e.hook(REC_CMD, on_rec_cmd)
        e.hook(COMMIT, on_commit)
        e.hook(STORE_WRITE, on_store_write)
        self.blk = self.call(CUR_BLOCK, 0)
        self.stray = []
        self.other_posts = []

    def call(self, fn, *a, n=20_000_000):
        r = self.e.call(fn, *a, max_insns=n)
        assert r.ok, (self.name, hex(fn), r)
        return r.r0

    def u(self, a, n):
        return int.from_bytes(self.e.read_bytes(a, n), "little")

    def w(self, a, v, n=1):
        self.e.write_bytes(a, v.to_bytes(n, "little"))

    def tick(self, n=1):
        for _ in range(n):
            self.call(TASK20_TICK, 1)

    def tick_frames(self, n=1):
        """n task-20 passes; the frames they send (a Music command's Result is sent from task 20)."""
        self.pn.r.reset_logs()
        self.tick(n)
        return self.notify_frames()

    def notify_frames(self):
        """Run the notify task for every queued notification (as Piano.send does) and return all frames sent."""
        out = [bytes(m) for _p, m in self.pn.r.sent]
        for q, a in list(self.pn.r.posts):
            if q != 0 or not 0x40000000 <= a < 0x42000000:   # not the notify mailbox: record it, nothing to send
                self.other_posts.append((q, a))
                continue
            m = self.e.read_bytes(a, 16)
            if m[1] == 0xFF:
                continue
            self.pn.r.sent = []
            nres = self.e.call(ss.NOTIFY_SEND, m[0] * 256 + m[1], m[2], m[3], a + 4, max_insns=5_000_000)
            assert nres.ok, nres
            out += [bytes(x) for _p, x in self.pn.r.sent]
        return out

    # S side
    def send(self, *msgs):
        frames = []
        for msg in msgs:
            _stop, fr = self.pn.send(msg)
            frames += fr
        return [f for f in frames if f[3] != 0x07]   # the fn 0x10 ACK belongs to the SysEx front

    # C side
    def core(self, cmd, arg=0):
        self.pn.r.reset_logs()
        self.in_core = True
        r = self.e.call(self.syms["rca_cmd"], RC[cmd], arg, RC_OUT, max_insns=50_000_000)
        self.in_core = False
        assert r.ok, (cmd, r)
        self.stray += [w for w in r.writes if RC_WIN[0] <= w.pc < RC_WIN[1]
                       and not (RC_WIN[0] <= w.addr < RC_WIN[1] or STACK[0] <= w.addr < STACK[1])]
        res, last = self.e.read_bytes(RC_OUT, 2)
        return RES[res], last, self.notify_frames()

    def poll(self):
        self.in_core = True
        r = self.e.call(self.syms["rca_poll"], max_insns=2_000_000)
        self.in_core = False
        assert r.ok, r
        self.stray += [w for w in r.writes if RC_WIN[0] <= w.pc < RC_WIN[1]
                       and not (RC_WIN[0] <= w.addr < RC_WIN[1] or STACK[0] <= w.addr < STACK[1])]
        return r.r0

    def snap(self):
        return b"".join(self.e.read_bytes(lo, hi - lo) for lo, hi in WINDOWS)

    def view(self):
        return dict(state=self.u(0x4191EF79, 1), song=self.u(0x4191EF77, 1), sel=self.u(0x4191EF72, 1),
                    mode=self.u(0x419156A6, 1), tempo=self.u(0x4191570C, 2), pos=self.u(0x4191EF58, 4),
                    bar=self.u(0x4191EF6E, 2), maxbars=self.u(0x4191EF70, 2), q=self.u(MQ_COUNT, 1))


def main():
    fails = []

    def check(cond, what):
        if not rtlib.check(what, cond):
            fails.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        blob, syms = build_arm(pathlib.Path(tmp))
    print(f"ARM build: {len(blob)} B at {RC_BASE:#x}, rca_cmd {syms['rca_cmd']:#x} rca_poll {syms['rca_poll']:#x}")

    def first_diff(a: bytes, b: bytes):
        off = 0
        for lo, hi in WINDOWS:
            n = hi - lo
            for i in range(n):
                if a[off + i] != b[off + i]:
                    return f"{lo + i:#x}: S {a[off + i]:#04x} C {b[off + i]:#04x}"
            off += n
        return "-"

    def same(S, C, label, fs=None, fc=None):
        sv, cv = S.view(), C.view()
        print(f"{label}: S {sv}\n{' ' * len(label)}  C {cv}")
        a, b = S.snap(), C.snap()
        check(a == b, f"{label}: recorder / player / queue / tempo / job RAM C == S ({first_diff(a, b)})")
        if fs is not None:
            check([p3.hexs(f) for f in fc] == [p3.hexs(f) for f in fs],
                  f"{label}: frames to the controller C == S {[p3.hexs(f) for f in fs]}")

    def done(C, cmd, polls=3):
        ev = 0
        for _ in range(polls):
            ev = C.poll()
            if ev:
                break
        check(ev == (EV_DONE << 8 | RC[cmd]), f"rca_poll: DONE {cmd} ({ev:#x})")

    def step(S, C, label, s_msgs, cmd, arg=0, ticks=0, want="OK"):
        fs = S.send(*s_msgs)
        res, last, fc = C.core(cmd, arg)
        check(res == want, f"{label}: core {cmd} {arg} -> {res} (want {want})")
        fs += S.tick_frames(ticks)
        fc += C.tick_frames(ticks)
        same(S, C, label, fs, fc)
        return res, last

    S, C = Rig("S", blob, syms), Rig("C", blob, syms)

    # 1. song 1 selected (idle): SongSelect apply in task 19 on both sides
    step(S, C, "select song 1", [ss.song_select(0)], "SELECT", 0)
    done(C, "SELECT")
    for r in (S, C):             # the emulator has no boot: give the loaded song its tempo (FUN_401673F8, as the loader)
        r.call(SONG_TEMPO_INIT, 120)

    # 2. play song 1: S enters MusicMode 2 and plays (two queue posts), C posts the same two (mode 0 -> 2 first)
    fs = S.send(music_mode(2), music(1))
    res, _l, fc = C.core("PLAY")
    check(res == "OK", f"play: core PLAY -> {res}")
    check(S.u(MQ_COUNT, 1) == C.u(MQ_COUNT, 1) == 2, "play: two queue entries on both sides before task 20 runs")
    same(S, C, "play queued", fs, fc)
    check(C.poll() == 0, "play: rca_poll waits while the entry is still queued")
    S.tick(), C.tick()
    same(S, C, "play (1 task-20 pass)")
    done(C, "PLAY")
    check(C.view()["state"] == 1 and C.view()["mode"] == 2, "play: state 1, MusicMode 2")
    S.tick(400), C.tick(400)
    same(S, C, "playing +400 passes (0.8 s)")
    check(C.view()["pos"] > 0, f"playing: position advances ({C.view()['pos']} ticks)")

    # 3. tempo change while playing
    step(S, C, "tempo 144", [tempo(144)], "TEMPO", 144)
    done(C, "TEMPO")
    S.tick(300), C.tick(300)
    same(S, C, "tempo 144 +300 passes")

    # 4. seek by bar while playing (stock restarts play in task 19 without the 200 ms wait)
    step(S, C, "seek bar 3 (playing)", [seek(3)], "SEEK", 3)
    done(C, "SEEK")
    check(C.view()["bar"] == 3 and C.view()["state"] == 1, f"seek: bar 3, still playing ({C.view()})")
    S.tick(50), C.tick(50)
    same(S, C, "seek +50 passes")

    # 5. part select while playing: IntSongPart 0 = part 1 -> play selector 1
    step(S, C, "part 1 (playing)", [ss.int_song_part(0)], "PART", 0)
    done(C, "PART")
    check(C.view()["sel"] == 1, "part: play selector 1")

    # 6. pause, play again from the paused place, stop
    step(S, C, "pause", [music(0)], "PAUSE", 0, ticks=1)
    done(C, "PAUSE")
    check(C.view()["state"] == 0, "pause: state 0")
    pos_paused = C.view()["pos"]
    step(S, C, "play from pause", [music(1)], "PLAY", 0, ticks=1)
    done(C, "PLAY")
    S.tick(20), C.tick(20)
    same(S, C, "play from pause +20 passes")
    check(C.view()["pos"] >= pos_paused, f"play from pause: position goes on from {pos_paused} ({C.view()['pos']})")
    step(S, C, "stop", [music(2)], "STOP", 0, ticks=1)
    done(C, "STOP")
    check(C.view()["state"] == 2, "stop: state 2")

    # 7. seek while stopped, then play from there (the play from state 2 waits in task 20, here stubbed)
    step(S, C, "seek bar 2 (stopped)", [seek(2)], "SEEK", 2)
    done(C, "SEEK")
    step(S, C, "play from bar 2", [music(1)], "PLAY", 0, ticks=1)
    done(C, "PLAY")
    check(C.view()["bar"] >= 2, f"play from bar 2: bar {C.view()['bar']}")

    # 8. select song 2 while playing: stock stops, loads, restarts in task 19; the core calls the same apply tail
    step(S, C, "select song 2 (playing)", [ss.song_select(1)], "SELECT", 1)
    done(C, "SELECT")
    check(C.view()["state"] == 1 and C.view()["song"] == 1, "select while playing: song 2, playing again")
    S.tick(30), C.tick(30)
    same(S, C, "song 2 +30 passes")
    step(S, C, "stop song 2", [music(2)], "STOP", 0, ticks=1)
    done(C, "STOP")

    # 9. refusals - C hands nothing over (no queue entry, no frame, RAM unchanged); S shows what the stock does
    def refused(label, s_msgs, cmd, arg, want, ticks=1, s_expect=None):
        """C: the core refuses (checked right after its call: no queue entry, no frame, recorder RAM unchanged).
        Then the same stock-way messages go to BOTH sides (S shows what the stock does with them; on C they keep
        the two machines in step, so the following byte comparisons stay meaningful)."""
        before = C.snap()
        q0 = C.u(MQ_COUNT, 1)
        res, _l, fc = C.core(cmd, arg)
        after = C.snap()
        check(res == want, f"{label}: core {cmd} {arg} -> {res} (want {want})")
        check(C.u(MQ_COUNT, 1) == q0 and not fc, f"{label}: core posted nothing and sent no frame")
        check(after == before, f"{label}: core side recorder RAM unchanged by the core ({first_diff(before, after)})")
        fs = S.send(*s_msgs) if s_msgs else []
        C.send(*s_msgs) if s_msgs else None
        fs += S.tick_frames(ticks)
        C.tick(ticks)
        print(f"{label}: core {cmd} -> {res}; stock {S.view()} frames {[p3.hexs(f) for f in fs]}")
        if s_expect:
            check(s_expect(S, fs), f"{label}: stock side {s_expect.__doc__}")

    def plays(r, _fs):
        """plays anyway (state 1)"""
        return r.view()["state"] == 1

    for r in (S, C):
        r.w(JOB_FLAG, 1)         # a RecorderData NOR job pending (the worker has not run)
    refused("play while NOR job pending", [music(1)], "PLAY", 0, "E_BUSY", s_expect=plays)
    for r in (S, C):
        r.send(music(2)), r.tick()            # back to stop
    refused("select while NOR job pending", [], "SELECT", 2, "E_BUSY")
    for r in (S, C):
        r.w(JOB_FLAG, 0)
        r.w(FILEOP, 8)           # a file operation (USB song load) in progress
    refused("play while file op", [music(1)], "PLAY", 0, "E_BUSY", s_expect=plays)
    for r in (S, C):
        r.send(music(2)), r.tick()
    refused("stop while file op (stop reloads the song)", [], "STOP", 0, "NOP")  # already stopped -> NOP
    for r in (S, C):
        r.w(FILEOP, 0)
    same(S, C, "after busy refusals")

    def dropped(r, _fs):
        """drops it (state stays 2)"""
        return r.view()["state"] == 2

    for r in (S, C):
        r.w(MQ_HOLD, 0x82)       # deferred MusicMode: FUN_4015DBD4 drops Music commands
    refused("play while MusicMode deferred", [music(1)], "PLAY", 0, "E_BUSY", s_expect=dropped)
    for r in (S, C):
        r.w(MQ_HOLD, 0)

    def no_data(r, fs):
        """answers Result 0xEC, stays stopped"""
        return ss.NODATA in [p3.hexs(f) for f in fs] and r.view()["state"] == 2

    step(S, C, "select song 5 (empty)", [ss.song_select(4)], "SELECT", 4)
    done(C, "SELECT")
    refused("play empty song", [music(1)], "PLAY", 0, "E_NODATA", s_expect=no_data)
    refused("seek bar 1 in an empty song", [], "SEEK", 1, "E_NODATA")
    step(S, C, "select song 2", [ss.song_select(1)], "SELECT", 1)
    done(C, "SELECT")
    refused("seek past the end (5 bars)", [], "SEEK", 6, "E_ARG")
    refused("select song 11", [], "SELECT", 10, "E_ARG")
    refused("tempo 501", [], "TEMPO", 501, "E_ARG")

    # the stock has no fast forward for the internal recorder: Music 7 reaches FUN_4016A050 and does nothing
    v0 = S.view()
    S.send(music(7))
    S.tick()
    check(S.view() == v0, "stock Music 7 (FastForward) in MusicMode 2: no change (the core offers no FF)")
    C.send(music(7)), C.tick()   # keep both sides' queues in step
    same(S, C, "after Music 7")

    # 10. recorder armed / recording: everything refused (R2); the stock SongSelect in state 4 would commit
    for r in (S, C):
        r.call(REC_CMD, r.blk, 3)
    C.rec_cmd_calls = 0
    for cmd, arg in (("PLAY", 0), ("STOP", 0), ("SELECT", 0), ("TEMPO", 100), ("SEEK", 1), ("PART", 1)):
        refused(f"armed (state 3): {cmd}", [], cmd, arg, "E_REC", ticks=0)
    for r in (S, C):
        r.call(REC_CMD, r.blk, 4)
        r.call(RING_CONSUME)
    check(S.view()["state"] == C.view()["state"] == 4, "recording: state 4 on both sides")
    refused("recording (state 4): SELECT", [], "SELECT", 0, "E_REC", ticks=0)
    refused("recording (state 4): STOP", [], "STOP", 0, "E_REC", ticks=0)
    S.send(ss.song_select(0))
    check(S.commits == 1, f"stock SongSelect in state 4 calls the commit FUN_40168774 ({S.commits}) - the core never does")

    # whole C run
    check(C.rec_cmd_in_core == 0, f"FUN_4016A050 never ran inside the core's calls (task-20 passes only: {C.rec_cmd_calls} there)")
    check(C.commits == 0 and C.store_writes == 0 and not C.pn.jobs,
          f"core side: no commit, no RecorderData write, no NOR job ({C.commits}, {C.store_writes}, {len(C.pn.jobs)})")
    check(not C.stray, f"the core's own stores stay in its window and the stack ({len(C.stray)} stray)")

    # 11. the device's own song table (target/CA79NOR.BIN 0x500000, read only, structure printed only)
    if NOR_DUMP.is_file():
        store = NOR_DUMP.read_bytes()[0x500000:0x600000]
        S, C = Rig("S", blob, syms, store=store), Rig("C", blob, syms, store=store)
        step(S, C, "device: select song 1", [ss.song_select(0)], "SELECT", 0)
        done(C, "SELECT")
        for r in (S, C):
            r.call(SONG_TEMPO_INIT, 120)
        fs = S.send(music_mode(2), music(1))
        res, _l, fc = C.core("PLAY")
        S.tick(), C.tick()
        check(res == "OK", f"device: core PLAY -> {res}")
        same(S, C, "device: play song 1", fs, fc)
        done(C, "PLAY")
        S.tick(200), C.tick(200)
        step(S, C, "device: seek bar 10", [seek(10)], "SEEK", 10)
        done(C, "SEEK")
        step(S, C, "device: stop", [music(2)], "STOP", 0, ticks=1)
        done(C, "STOP")
        check(C.commits == 0 and C.store_writes == 0 and not C.pn.jobs and not S.pn.jobs, "device: no commit, no NOR job")
    else:
        print("SKIP device store (target/CA79NOR.BIN absent)")

    print("all checks passed" if not fails else f"{len(fails)} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
