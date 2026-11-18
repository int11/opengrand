# roundtrip_recrec.py - Phase 3-6 R2: the core's RECORD control (result/firmware/src/recorder/recctl.c with
# rc_enable_rec, emulator glue result/firmware/tests/arm/recctl_arm.c rca_rec(1)) built for ARM with the pinned
# toolchain and run INSIDE the stock-image emulator next to a second, identical emulator driven the stock way (Kawai
# SysEx frames through the stock apply FUN_4016053C).  Both run the stock recorder - task-20 pass FUN_4016DE5C:
# Music queue FUN_4015DE0C -> FUN_4016A050, ring consumer FUN_4016CCDC -> FA FUN_401692D0 / FC commit FUN_40168774 ->
# serialiser -> the RecorderData writer FUN_40168408 -> NOR job FUN_40134048 (recorded, not run) - on the same
# SYNTHETIC song store (tools/stock_songs.py) with the same notes fed into the record ring (FUN_4016A76C).
#
#
# Unlike roundtrip_recctl.py (R1) the commit RUNS here on both sides: a take is armed, started, played and finished,
# and the RecorderData mirror 0x40500000 (1 MB) the stock writes must be byte-identical C == S, with the same NOR jobs.
# Per accepted command: recorder / player / queue / tempo / NOR-job RAM C == S, frames to the controller C == S,
# rca_poll() DONE.  Refusals: C posts nothing, sends nothing, changes no RAM; S shows what the stock does instead
# (start without arming records, count-in while recording restarts the take, two stops in one pass commit twice,
# FA dropped on a full ring).  Over the whole C run FUN_4016A050 and the commit never run inside a core call.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_recrec.py
from __future__ import annotations

import hashlib
import pathlib
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402

rr = rtlib.load("roundtrip_recctl")
ss, p3, StockEmu = rr.ss, rr.p3, rr.StockEmu

NOTE = 0x4016A76C            # FUN_4016A76C(note, vel): 90 note vel into the record ring (keyboard path, task 18)
RING_COUNT = 0x4191EF6C      # u16 ring fill; FA / FC / note writers drop at > 0xFFF
STATE = 0x4191EF79
REC_PART = 0x4191EF73
REC_PREP = 0x401692D0        # FA consumer: prepare the take, state 4
COUNT_IN = 0x40169EDC        # Music 6: count-in, no state test

rr.RC.update(REC=8, REC_NOMETRO=9, COUNT_IN=10, REC_START=11)
rr.RES.update({10: "E_WRONG", 11: "E_EXISTS"})
KEEP, OVERWRITE = 0x7F, 0x100


class Rig(rr.Rig):
    """roundtrip_recctl's rig, but the commit and the RecorderData writer RUN (counted, not replaced)."""

    def __init__(self, name, blob, syms, store=None):
        super().__init__(name, blob, syms, store)
        self.commit_in_core = 0
        self.preps = 0

        def on_commit(em):
            self.commits += 1
            self.commit_in_core += self.in_core
            return StockEmu.CONTINUE

        def on_store_write(em):
            self.store_writes += 1
            return StockEmu.CONTINUE

        def on_prep(em):
            self.preps += 1
            return StockEmu.CONTINUE

        self.e.hook(rr.COMMIT, on_commit)
        self.e.hook(rr.STORE_WRITE, on_store_write)
        self.e.hook(REC_PREP, on_prep)
        r = self.e.call(syms["rca_rec"], 1, max_insns=100_000)
        assert r.ok and r.r0 == 1, r

    def store_hash(self):
        return hashlib.sha256(self.e.read_bytes(ss.REC, 0x100000)).hexdigest()[:16]

    def offsets(self):
        return [rtlib.le(self.e.read_bytes(ss.REC, 0x30), 4 * i) for i in range(11)]

    def notes(self, seq):
        """the same keyboard notes on both sides: (note, vel) into the ring, then n task-20 passes"""
        for note, vel, passes in seq:
            self.call(NOTE, note, vel)
            self.tick(passes)

    def rstate(self):
        r = self.e.call(self.syms["rca_state"], max_insns=100_000)
        assert r.ok
        return dict(fc_out=r.r0 & 1, defer=(r.r0 >> 1) & 1, n_finish=r.r0 >> 8)


PLAY_NOTES = [(60, 100, 20), (60, 0, 20), (64, 90, 15), (64, 0, 25), (67, 80, 30), (67, 0, 10)]


def main():
    fails = []

    def check(cond, what):
        if not rtlib.check(what, cond):
            fails.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        blob, syms = rr.build_arm(pathlib.Path(tmp))
    print(f"ARM build: {len(blob)} B at {rr.RC_BASE:#x}, rca_cmd {syms['rca_cmd']:#x} rca_rec {syms['rca_rec']:#x}")

    def first_diff(a: bytes, b: bytes):
        off = 0
        for lo, hi in rr.WINDOWS:
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
        check(S.store_hash() == C.store_hash(), f"{label}: RecorderData mirror 0x40500000 (1 MB) C == S ({C.store_hash()})")
        check(S.pn.jobs == C.pn.jobs, f"{label}: NOR jobs C == S ({C.pn.jobs})")
        if fs is not None:
            check([p3.hexs(f) for f in fc] == [p3.hexs(f) for f in fs],
                  f"{label}: frames to the controller C == S {[p3.hexs(f) for f in fs]}")

    def done(C, cmd, polls=3):
        ev = 0
        for _ in range(polls):
            ev = C.poll()
            if ev:
                break
        check(ev == (rr.EV_DONE << 8 | rr.RC[cmd]), f"rca_poll: DONE {cmd} ({ev:#x})")

    def step(S, C, label, s_msgs, cmd, arg=0, ticks=1, want="OK"):
        fs = S.send(*s_msgs)
        res, _last, fc = C.core(cmd, arg)
        check(res == want, f"{label}: core {cmd} {arg:#x} -> {res} (want {want})")
        fs += S.tick_frames(ticks)
        fc += C.tick_frames(ticks)
        same(S, C, label, fs, fc)
        return res

    def refused(S, C, label, cmd, arg, want):
        """C refuses: nothing posted, no frame, recorder RAM and store unchanged."""
        before, h = C.snap(), C.store_hash()
        q0 = C.u(rr.MQ_COUNT, 1)
        res, _l, fc = C.core(cmd, arg)
        check(res == want, f"{label}: core {cmd} {arg:#x} -> {res} (want {want})")
        check(C.u(rr.MQ_COUNT, 1) == q0 and not fc and C.snap() == before and C.store_hash() == h,
              f"{label}: core posted nothing, sent nothing, changed nothing")

    def fresh():
        S, C = Rig("S", blob, syms), Rig("C", blob, syms)
        S.send(ss.song_select(4))
        C.core("SELECT", 4)
        done(C, "SELECT")
        for r in (S, C):
            r.call(rr.SONG_TEMPO_INIT, 120)
        return S, C

    # ---------------------------------------------------------------- A. a take: arm, start, notes, finish
    print("A. record part 1 of the empty song 5: arm -> start -> notes -> finish (the stock commit runs on both sides)")
    S, C = fresh()
    h0, off0 = C.store_hash(), C.offsets()
    fs = S.send(rr.music_mode(2), rr.music(3))
    res, _l, fc = C.core("REC", KEEP)
    check(res == "OK", f"arm: core REC -> {res}")
    check(C.u(rr.MQ_COUNT, 1) == S.u(rr.MQ_COUNT, 1) == 2, "arm: MusicMode 2 + Music 3 queued on both sides")
    fs += S.tick_frames(1)
    fc += C.tick_frames(1)
    same(S, C, "arm (state 3)", fs, fc)
    done(C, "REC")
    check(C.view()["state"] == 3, "arm: state 3")
    refused(S, C, "armed: play", "PLAY", 0, "E_REC")
    refused(S, C, "armed: select", "SELECT", 1, "E_REC")
    refused(S, C, "armed: other part", "REC_NOMETRO", 1, "E_REC")
    step(S, C, "start (FA -> state 4)", [rr.music(4)], "REC_START")
    done(C, "REC_START")
    check(C.view()["state"] == 4 and C.preps == S.preps == 1, f"start: state 4, FA prepared the take once ({C.preps})")
    for r in (S, C):
        r.notes(PLAY_NOTES)
    same(S, C, "recording + 6 notes / 120 passes")
    refused(S, C, "recording: count-in (stock would restart the take)", "COUNT_IN", KEEP, "E_REC")
    refused(S, C, "recording: arm", "REC", KEEP, "E_REC")
    refused(S, C, "recording: seek", "SEEK", 1, "E_REC")
    step(S, C, "finish = stop (FC -> commit)", [rr.music(2)], "STOP")
    done(C, "STOP")
    check(C.commits == S.commits == 1 and len(C.pn.jobs) == 1, f"finish: one commit, one NOR job each ({C.commits}, {C.pn.jobs})")
    check(C.store_hash() != h0 and C.offsets()[5] - C.offsets()[4] > off0[5] - off0[4],
          f"finish: song 5 grew in the store ({off0[5] - off0[4]} -> {C.offsets()[5] - C.offsets()[4]} B)")
    check(C.commit_in_core == 0 and C.rec_cmd_in_core == 0, "commit and FUN_4016A050 ran only in task-20 passes")
    check(C.rstate()["fc_out"] == 0, "finish: fc_out cleared once state 2 was seen")

    # ---------------------------------------------------------------- B. overdub part 2 with a count-in, finish by pause
    print("B. overdub: part 1 is recorded now")
    refused(S, C, "arm part 1 again without overwrite", "REC", 0, "E_EXISTS")
    step(S, C, "count-in for part 2 (IntSongPart 1 + Music 6)", [ss.int_song_part(1), rr.music(6)], "COUNT_IN", 1)
    done(C, "COUNT_IN")
    check(C.view()["state"] == 6 and C.u(REC_PART, 1) == 1, "count-in: state 6, record part 2")
    for r in (S, C):
        r.tick(50)
    same(S, C, "count-in +50 passes")
    refused(S, C, "count-in: stop (no stock command leaves 6)", "STOP", 0, "E_WRONG")
    v0 = S.view()
    S.send(rr.music(2)), S.tick()
    check(S.view()["state"] == 6, "stock Music 2 in state 6: nothing happens (stays 6)")
    C.send(rr.music(2)), C.tick()
    step(S, C, "start from count-in", [rr.music(4)], "REC_START")
    done(C, "REC_START")
    for r in (S, C):
        r.notes(PLAY_NOTES[:4])
    step(S, C, "finish = pause in 4 (stock Music 0 -> FC as well)", [rr.music(0)], "PAUSE")
    done(C, "PAUSE")
    check(C.commits == S.commits == 2 and len(C.pn.jobs) == 2, f"overdub: second commit, second NOR job ({C.pn.jobs})")
    del v0

    # ---------------------------------------------------------------- C. leave standby without recording
    print("C. leave the armed states without a take")
    step(S, C, "select song 6 (empty)", [ss.song_select(5)], "SELECT", 5)
    done(C, "SELECT")
    step(S, C, "arm, metronome off (Music 5)", [rr.music(5)], "REC_NOMETRO", KEEP | OVERWRITE)
    done(C, "REC_NOMETRO")
    check(C.view()["state"] == 5, "state 5")
    v5 = S.view()
    S.send(rr.music(2)), S.tick()
    check(S.view()["state"] == 5, f"stock Music 2 in state 5: nothing happens ({S.view()['state']})")
    C.send(rr.music(2)), C.tick()
    same(S, C, "after the stock's ignored stop in 5")
    step(S, C, "stop in 5 (core: Music 3 then 2)", [rr.music(3), rr.music(2)], "STOP")
    done(C, "STOP")
    check(C.view()["state"] == 2 and C.commits == 2 and len(C.pn.jobs) == 2, "state 2, no commit, no NOR job")
    step(S, C, "arm (Music 3)", [rr.music(3)], "REC", KEEP)
    done(C, "REC")
    step(S, C, "cancel arm: stop in 3", [rr.music(2)], "STOP")
    done(C, "STOP")
    check(C.view()["state"] == 2 and C.commits == 2 and len(C.pn.jobs) == 2, "cancelled: state 2, no commit, no NOR job")
    del v5

    # ---------------------------------------------------------------- D. a finish waits for a NOR job
    print("D. finish while a NOR job is pending: the core defers the FC until the job is gone")
    step(S, C, "arm", [rr.music(3)], "REC", KEEP | OVERWRITE)
    done(C, "REC")
    step(S, C, "start", [rr.music(4)], "REC_START")
    done(C, "REC_START")
    for r in (S, C):
        r.notes(PLAY_NOTES[:2])
        r.w(rr.JOB_FLAG, 1)     # a RecorderData NOR job queued (worker not run)
    q0 = C.u(rr.MQ_COUNT, 1)
    res, _l, fc = C.core("STOP")
    check(res == "OK" and C.u(rr.MQ_COUNT, 1) == q0 and not fc and C.rstate()["defer"] == 1,
          f"stop during the NOR job: accepted, nothing posted yet (deferred) ({res})")
    for _ in range(20):
        check(C.poll() == 0, "deferred: rca_poll waits") if _ == 0 else C.poll()
        C.tick()
    S.tick(20)
    check(C.view()["state"] == 4 and C.commits == 2 and C.u(rr.MQ_COUNT, 1) == 0, "deferred: still recording, no commit")
    same(S, C, "deferred +20 passes (S: nothing sent yet either)")
    for r in (S, C):
        r.w(rr.JOB_FLAG, 0)
    check(C.poll() == 0 and C.u(rr.MQ_COUNT, 1) == 1 and C.rstate()["fc_out"] == 1, "job gone: rca_poll posts the FC (Music 2)")
    fs = S.send(rr.music(2))
    fs += S.tick_frames(1)
    fc = C.tick_frames(1)
    same(S, C, "deferred finish", fs, fc)
    done(C, "STOP")
    check(C.commits == S.commits == 3, f"deferred finish: one commit ({C.commits})")

    # whole C run so far
    check(C.rec_cmd_in_core == 0 and C.commit_in_core == 0, "FUN_4016A050 / the commit never ran inside a core call")
    check(not C.stray, f"the core's own stores stay in its window and the stack ({len(C.stray)} stray)")

    # ---------------------------------------------------------------- E. what the stock does that the core refuses
    print("E. stock behaviours the core refuses (each on fresh machines)")
    S, C = fresh()
    S.send(rr.music_mode(2)), C.send(rr.music_mode(2))
    S.tick(), C.tick()
    refused(S, C, "start without arming (state 2)", "REC_START", 0, "E_WRONG")
    S.send(rr.music(4)), S.tick()
    check(S.view()["state"] == 4, f"stock Music 4 in state 2: records without arming (state {S.view()['state']})")

    S, C = fresh()
    for r in (S, C):
        r.send(rr.music_mode(2))
        r.send(rr.music(3))
        r.tick()
        r.send(rr.music(4))
        r.tick()
    check(S.view()["state"] == C.view()["state"] == 4, "both recording")
    refused(S, C, "count-in while recording", "COUNT_IN", KEEP, "E_REC")
    S.send(rr.music(6)), S.tick()
    check(S.view()["state"] == 6, "stock Music 6 in state 4: count-in starts, the take is left uncommitted (state 6)")

    S, C = fresh()
    for r in (S, C):
        r.send(rr.music_mode(2))
        r.send(rr.music(3))
        r.tick()
        r.send(rr.music(4))
        r.tick()
        r.notes(PLAY_NOTES[:2])
    S.send(rr.music(2), rr.music(2))         # two stops before task 20 runs (panel + app)
    S.tick(2)
    res1, _l, _f = C.core("STOP")
    res2, _l, _f = C.core("STOP")
    C.tick(2)
    check(res1 == "OK" and res2 in ("NOP", "E_PENDING"), f"core: second stop not posted ({res1}, {res2})")
    check(S.commits == 2 and len(S.pn.jobs) == 2, f"stock: two stops in one pass commit twice ({S.commits} commits, {len(S.pn.jobs)} NOR jobs)")
    check(C.commits == 1 and len(C.pn.jobs) == 1, f"core: one commit, one NOR job ({C.commits}, {len(C.pn.jobs)})")

    S, C = fresh()
    for r in (S, C):
        r.send(rr.music_mode(2))
        r.send(rr.music(3))
        r.tick()
        r.w(RING_COUNT, 0x1000, 2)          # ring full (count only; no pass runs while it is set)
    refused(S, C, "start with the ring full", "REC_START", 0, "E_BUSY")
    S.call(rr.REC_CMD, S.blk, 4)              # what task 20 would run for Music 4: FUN_40169F74 drops the FA
    for r in (S, C):
        r.w(RING_COUNT, 0, 2)
    S.tick(), C.tick()
    check(S.view()["state"] == 3, f"stock Music 4 with the ring full: FA dropped silently, still armed ({S.view()['state']})")

    # ---------------------------------------------------------------- F. the device's own song table (structure only)
    if rr.NOR_DUMP.is_file():
        print("F. the device's RecorderData (target/CA79NOR.BIN 0x500000, read only, copied into emulator RAM only)")
        store = rr.NOR_DUMP.read_bytes()[0x500000:0x600000]
        S, C = Rig("S", blob, syms, store=store), Rig("C", blob, syms, store=store)
        offs = C.offsets()
        empty = [i for i in range(10) if offs[i + 1] - offs[i] <= 0x74]
        check(bool(empty), f"device: an empty song to record into ({empty})")
        if empty:
            n = empty[0]
            S.send(ss.song_select(n))
            C.core("SELECT", n)
            done(C, "SELECT")
            for r in (S, C):
                r.call(rr.SONG_TEMPO_INIT, 120)
            fs = S.send(rr.music_mode(2), rr.music(3))
            res, _l, fc = C.core("REC", KEEP)
            fs += S.tick_frames(1)
            fc += C.tick_frames(1)
            check(res == "OK", f"device: arm song {n + 1} -> {res}")
            same(S, C, f"device: arm song {n + 1}", fs, fc)
            done(C, "REC")
            step(S, C, "device: start", [rr.music(4)], "REC_START")
            done(C, "REC_START")
            for r in (S, C):
                r.notes(PLAY_NOTES)
            step(S, C, "device: finish", [rr.music(2)], "STOP")
            done(C, "STOP")
            o2 = C.offsets()
            check(C.commits == S.commits == 1 and len(C.pn.jobs) == 1 and all(
                o2[i + 1] - o2[i] == offs[i + 1] - offs[i] for i in range(10) if i != n),
                f"device: one commit, one NOR job, the other 9 songs keep their length (song {n + 1}: "
                f"{offs[n + 1] - offs[n]} -> {o2[n + 1] - o2[n]} B)")
            res, _l, _f = C.core("SELECT", n)
            check(res == "OK", f"device: the table still passes the boot-check rules after the commit (select -> {res})")
    else:
        print("SKIP F. device store (target/CA79NOR.BIN absent)")

    print("all checks passed" if not fails else f"{len(fails)} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
