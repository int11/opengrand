# roundtrip_recsave.py - Phase 3-6 R3: the core's SAVE decision (result/firmware/src/recorder/recsave.c on top of the R2
# record control + the core's delete src/recorder/recdel.c, emulator glue result/firmware/tests/arm/recctl_arm.c rca_save /
# rca_delete / rca_file) built for ARM
# with the pinned toolchain and run INSIDE the stock-image emulator next to a second, identical emulator driven the
# stock way (Kawai SysEx frames through the stock apply FUN_4016053C).  Both run the stock writers for real: the commit
# FUN_40168774 (task-20 ring consumer), the InternalSongDelete apply 0x40160AD4 -> FUN_4016C800 / FUN_4016C908, the
# RecorderData writer FUN_40168408 - on the same SYNTHETIC song store (tools/stock_songs.py).  The NOR job FUN_40134048
# is recorded and its pending byte 0x4139DAAC + slot * 8 set as the stock does (the worker is not run: the test clears
# the byte when "the job is done").
#
#
# Per accepted write: RecorderData mirror (1 MB) / recorder RAM / NOR jobs / frames C == S, and the core's write window
# opens, stays open while the job is pending (the core then refuses loads, deletes and imports), and closes with the
# expected result (OK, only the target song changed).  Refusals: C changes nothing, while S shows what the stock does
# (deletes under a take or a playing song and reloads the player, deletes song 1 for song 12).  Deletes are the core's
# (recctl_cave.c own_delete: recdel.c builds the song, the stock writer FUN_40168408 writes it); S runs the stock delete for comparison only.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_recsave.py [-q]
from __future__ import annotations

import pathlib
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402

rr = rtlib.load("roundtrip_recctl")
rec = rtlib.load("roundtrip_recrec")
ss, p3, StockEmu = rr.ss, rr.p3, rr.StockEmu

JOB_BASE = 0x4139DAAC         # NOR job table: pending byte per slot (0x4139DAAC + slot * 8), FUN_401342AC reads both
DELETE_FN = 0x4016C800        # FUN_4016C800(song, part)
FORMAT_FN = 0x4016C908        # FUN_4016C908(): empty table + job
EDIT_OK = bytes.fromhex("F0407F32041E517E7F0000F7")   # FUN_4014F294(0)
KEEP, OVERWRITE = rec.KEEP, rec.OVERWRITE
V = {0: "PASS", 1: "IGNORE", 2: "BUSY", 3: "ARG"}
R = {0: "NONE", 1: "OK", 2: "NOWRITE", 3: "NOJOB", 4: "TABLE", 5: "OTHER", 6: "STUCK", 7: "DOUBLE"}
K = {0: "-", 1: "COMMIT", 2: "DELETE", 3: "FORMAT", 4: "FILE"}


def delete(song, part, fmt=0x04):
    """Kawai InternalSongDelete, command form (descriptor 0xCE): 32 52 21 7F [fmt, hi7, lo7, part]"""
    return rr.frame(0x32, 0x08, 0x02, 0x52, 0x21, 0x7F, fmt, (song >> 7) & 0x7F, song & 0x7F, part)


class Rig(rec.Rig):
    """roundtrip_recrec's rig (commit + writer run) + the NOR job's pending byte, the R3 glue switched on."""

    def __init__(self, name, blob, syms, store=None):
        super().__init__(name, blob, syms, store)
        self.deletes = 0
        self.delete_args = []

        def on_job(em):
            slot = em.reg(0)
            self.pn.jobs.append((slot, em.reg(1), em.reg(2)))
            self.w(JOB_BASE + 8 * slot, 1)   # FUN_40134048's own store [REA]
            return 0

        def on_delete(em):
            self.deletes += 1
            self.delete_args.append((em.reg(0), em.reg(1)))
            return StockEmu.CONTINUE

        def on_format(em):
            self.deletes += 1
            self.delete_args.append(("format",))
            return StockEmu.CONTINUE

        self.e.hook(ss.NOR_JOB, on_job)
        self.e.hook(DELETE_FN, on_delete)
        self.e.hook(FORMAT_FN, on_format)
        r = self.e.call(syms["rca_save"], 1, max_insns=5_000_000)
        assert r.ok and r.r0 == 1, r

    def worker_done(self):
        self.w(JOB_BASE, 0), self.w(JOB_BASE + 8, 0)

    def jobs_pending(self):
        return self.u(JOB_BASE, 1) | self.u(JOB_BASE + 8, 1)

    def save(self):
        r = self.e.call(self.syms["rca_save_state"], max_insns=100_000)
        assert r.ok
        x = r.r0
        ch = self.e.call(self.syms["rca_save_changed"], max_insns=100_000).r0
        return dict(closed=x & 1, phase=(x >> 1) & 7, kind=K[(x >> 4) & 0xF], last=R[(x >> 8) & 0xF],
                    jobs=(x >> 12) & 0xF, last_jobs=(x >> 16) & 0xF, target=(x >> 20) & 0xFF, changed=ch)

    def core_delete(self, song, part, fmt=0x04):
        self.pn.r.reset_logs()
        self.in_core = True
        r = self.e.call(self.syms["rca_delete"], fmt, song, part, max_insns=200_000_000)
        self.in_core = False
        assert r.ok, r
        self.stray += [w for w in r.writes if rr.RC_WIN[0] <= w.pc < rr.RC_WIN[1]
                       and not (rr.RC_WIN[0] <= w.addr < rr.RC_WIN[1] or rr.STACK[0] <= w.addr < rr.STACK[1])]
        return V[r.r0], self.notify_frames()

    def core_file(self, d0, d1):
        r = self.e.call(self.syms["rca_file"], d0, d1, max_insns=1_000_000)
        assert r.ok
        return V[r.r0]

    def lens(self):
        o = self.offsets()
        return [o[i + 1] - o[i] for i in range(10)]


def main():
    fails = []

    def check(cond, what):
        if not rtlib.check(what, cond):
            fails.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        blob, syms = rr.build_arm(pathlib.Path(tmp))
    print(f"ARM build: {len(blob)} B at {rr.RC_BASE:#x}, rca_save {syms['rca_save']:#x} rca_delete {syms['rca_delete']:#x}")

    def same(S, C, label, fs=None, fc=None):
        a, b = S.snap(), C.snap()
        check(a == b, f"{label}: recorder / player / queue / tempo / job RAM C == S")
        check(S.store_hash() == C.store_hash(), f"{label}: RecorderData mirror (1 MB) C == S ({C.store_hash()})")
        check(S.pn.jobs == C.pn.jobs, f"{label}: NOR jobs C == S ({C.pn.jobs})")
        if fs is not None:
            check([p3.hexs(f) for f in fc] == [p3.hexs(f) for f in fs],
                  f"{label}: frames C == S {[p3.hexs(f) for f in fs]}")

    def done(C, cmd, polls=3):
        ev = 0
        for _ in range(polls):
            ev = C.poll()
            if ev:
                break
        check(ev == (rr.EV_DONE << 8 | rr.RC[cmd]), f"rca_poll: DONE {cmd} ({ev:#x})")

    def fresh():
        S, C = Rig("S", blob, syms), Rig("C", blob, syms)
        for r in (S, C):
            r.call(rr.SONG_TEMPO_INIT, 120)
        return S, C

    def close_window(C, S=None, want="OK", target=None, changed=None):
        """the worker finishes: the job byte falls, the next poll closes the window"""
        st = C.save()
        check(st["phase"] != 0 and C.jobs_pending(), f"window open while the job is pending ({st})")
        for r in (C,) + ((S,) if S else ()):
            r.worker_done()
        C.poll()
        st = C.save()
        check(st["closed"] == 1 and st["phase"] == 0 and st["last"] == want
              and (target is None or st["target"] == target) and (changed is None or st["changed"] == changed),
              f"window closed: {st['last']} (want {want}), target {st['target']}, changed {st['changed']:#x}"
              + (f" (want {changed:#x})" if changed is not None else ""))
        return st

    # ------------------------------------------------ A. a take with the window: R2 commit, then loads refused until the job
    print("A. commit window: arm -> start -> notes -> stop on song 5 (R2), the NOR job pending, then done")
    S, C = fresh()
    S.send(ss.song_select(4)), C.core("SELECT", 4)
    done(C, "SELECT")
    l0 = C.lens()
    fs = S.send(rr.music_mode(2), rr.music(3))
    res, _l, fc = C.core("REC", KEEP)
    fs += S.tick_frames(1)
    fc += C.tick_frames(1)
    check(res == "OK", f"arm -> {res}")
    done(C, "REC")
    fs = S.send(rr.music(4))
    res, _l, fc = C.core("REC_START")
    S.tick(1), C.tick(1)
    done(C, "REC_START")
    for r in (S, C):
        r.notes(rec.PLAY_NOTES)
    fs = S.send(rr.music(2))
    res, _l, fc = C.core("STOP")
    fs += S.tick_frames(1)
    fc += C.tick_frames(1)
    same(S, C, "finish (commit)", fs, fc)
    done(C, "STOP")
    st = C.save()
    check(C.commits == 1 and st["kind"] == "COMMIT" and st["phase"] == 2 and st["jobs"] == 1 and st["target"] == 4,
          f"commit seen (state 4 -> 2 with the job queued): window {st}")
    # while the job is pending: the core lets nothing that loads / writes the table through
    h = C.store_hash()
    res_sel, _l, f1 = C.core("SELECT", 0)
    res_arm, _l, f2 = C.core("REC", KEEP | OVERWRITE)
    v_del, f3 = C.core_delete(0, 0x7F)
    v_imp = C.core_file(0x09, 2)
    check((res_sel, res_arm, v_del, v_imp) == ("E_BUSY", "E_BUSY", "BUSY", "BUSY") and not (f1 or f2 or f3)
          and C.store_hash() == h and C.deletes == 0,
          f"window open: select {res_sel}, arm {res_arm}, delete {v_del}, import {v_imp} - nothing sent or changed")
    close_window(C, S, "OK", 4, 1 << 4)
    l1 = C.lens()
    check(l1[4] > l0[4] and all(l1[i] == l0[i] for i in range(10) if i != 4),
          f"song 5 grew ({l0[4]} -> {l1[4]} B), the other 9 kept their length")
    fs = S.send(ss.int_song_part(0x7F))
    res, _l, fc = C.core("PART", 0x7F)
    check(res == "OK", f"after the window: the core takes table commands again (part 7F -> {res})")
    done(C, "PART")
    same(S, C, "part 7F after the window", fs, fc)

    # ------------------------------------------------ B. delete of the song just recorded: the core's delete vs the stock's
    # The stock FUN_4016C7BC re-serialises from the RECORD track table 0x4191DB14: it
    # deletes nothing.  The cave (and this glue) builds the song itself (recdel.c) and writes it with FUN_40168408, inside
    # the R3 window.  S still runs the stock path: what the stock would have done, for the record.
    print("B. InternalSongDelete 32 52 21 (fmt 04) of the song just recorded (song 5, part 1 only): part 2, then both parts")
    l_take = C.lens()[4]
    # part 2: the song has none - the core writes nothing, answers OK, no window (rs_nowrite)
    j0, h0, sd0 = len(C.pn.jobs), C.store_hash(), S.deletes
    fs = S.send(delete(4, 1))
    v, fc = C.core_delete(4, 1)
    st = C.save()
    check(v == "PASS" and [p3.hexs(f) for f in fc] == [p3.hexs(EDIT_OK)] and C.deletes == 0
          and len(C.pn.jobs) == j0 and C.store_hash() == h0 and st["phase"] == 0 and st["kind"] == "-",
          f"delete song 5 part 2 (absent): core {v}, OK frame, stock delete 0, NOR job 0, table unchanged, no window "
          f"({st['phase']}, {st['kind']})")
    check(S.deletes == sd0 + 1 and S.delete_args[-1] == (4, 1) and len(S.pn.jobs) == j0 + 1,
          f"stock: FUN_4016C800{S.delete_args[-1:]} and a NOR job for the same request")
    for r in (S,):
        r.worker_done()
    # both parts: the core writes the empty song (ROM template) - one job, the window closes OK, only song 5 changed
    js = len(S.pn.jobs)
    fs = S.send(delete(4, 0x7F))
    v, fc = C.core_delete(4, 0x7F)
    C.poll()                     # the pump pass after the apply sees the job: phase 2
    st = C.save()
    check(v == "PASS" and [p3.hexs(f) for f in fc] == [p3.hexs(EDIT_OK)] and C.deletes == 0
          and len(C.pn.jobs) == j0 + 1 and st["kind"] == "DELETE" and st["target"] == 4 and st["phase"] == 2,
          f"delete song 5 both parts: core {v}, OK frame, stock delete 0, NOR jobs {len(C.pn.jobs) - j0}, window {st}")
    st = close_window(C, None, "OK", 4, 1 << 4)
    l5 = C.lens()
    check(l5[4] == 0x74 and all(l5[i] == l1[i] for i in range(10) if i != 4),
          f"song 5 {l_take} -> {l5[4]} B (the empty song), the other 9 unchanged")
    check(len(S.pn.jobs) - js == 2 and S.lens()[4] != 0x74,
          f"stock: two NOR jobs in one call, song 5 still {S.lens()[4]} B (not deleted)")
    check(C.core("SELECT", 4)[0] == "OK", "after the delete window: select song 5 taken again")

    # ------------------------------------------------ C. refusals: the stock would write under a take / a playing song
    print("C. delete refused where the stock reloads the player under a take or a playing song (each on fresh machines)")
    for label, setup in (("recording (state 4)", "rec"), ("playing (state 1)", "play"), ("armed (state 3)", "arm")):
        S, C = fresh()
        for r in (S, C):
            r.send(ss.song_select(0))
            r.send(rr.music_mode(2))
            if setup == "play":
                r.send(rr.music(1))
                r.tick(3)
            else:
                r.send(rr.music(3))
                r.tick()
                if setup == "rec":
                    r.send(rr.music(4))
                    r.tick()
                    r.notes(rec.PLAY_NOTES[:2])
        C.poll()
        st0 = C.view()["state"]
        h, j0 = C.store_hash(), len(C.pn.jobs)
        v, fc = C.core_delete(1, 0x7F)
        check(v == "BUSY" and not fc and C.store_hash() == h and len(C.pn.jobs) == j0 and C.deletes == 0,
              f"{label}: core {v} - nothing deleted, no frame, no job")
        S.send(delete(1, 0x7F))
        check(S.delete_args[:1] == [(1, 0x7F)],
              f"{label}: the stock apply runs the delete anyway (FUN_4016C800{S.delete_args[:1]}, no state test), state {st0}")

    # ------------------------------------------------ D. song >= 10
    print("D. song number 12: the stock deletes song 1")
    S, C = fresh()
    h, l0 = C.store_hash(), C.lens()
    v, fc = C.core_delete(11, 0x7F)
    check(v == "ARG" and not fc and C.store_hash() == h, f"core: song 12 -> {v}, nothing changed")
    S.send(delete(11, 0x7F))
    check(S.delete_args[:1] == [(0, 0x7F)], f"stock: FUN_4016C800{S.delete_args[:1]} = song 1 (0x40160B0C cpyge r12, r6 = 0)")
    del l0
    v_fmt, _f = C.core_delete(1, 0x7F, fmt=0x00)
    check(v_fmt == "IGNORE" and C.store_hash() == h and C.deletes == 0, f"fmt 00: {v_fmt}, the stock drops it too")

    # ------------------------------------------------ E. a broken table: single delete refused, format allowed
    print("E. a song table that fails the check (song 4 start +2): single delete refused, delete-all = format allowed")
    S, C = fresh()
    for r in (S, C):
        o = r.offsets()
        r.e.write_bytes(ss.REC + 4 * 3, (o[3] + 2).to_bytes(4, "little"))
    h = C.store_hash()
    v, fc = C.core_delete(0, 0x7F)
    check(v == "BUSY" and C.store_hash() == h, f"broken table: delete song 1 -> {v}")
    fs = S.send(delete(0x3FFF, 0x7F, fmt=0x7F))
    v, fc = C.core_delete(0x3FFF, 0x7F, fmt=0x7F)
    check(v == "PASS", f"broken table: format -> {v}")
    same(S, C, "format (7F 7F)", fs, fc)
    C.poll()
    close_window(C, S, "OK", 0xFF)
    check(C.lens() == [0x74] * 10, f"format: ten empty songs ({C.lens()})")

    # ------------------------------------------------ F. a commit the core did not post (the stock's own Music 2)
    print("F. a commit through the stock apply (the panel's stop with R2 off): the window opens all the same")
    S, C = fresh()
    C.core("SELECT", 5)
    done(C, "SELECT")
    for r in (C,):
        r.send(rr.music_mode(2), rr.music(3))
        r.tick()
        r.send(rr.music(4))
        r.tick()
        r.notes(rec.PLAY_NOTES[:2])
        r.poll()                 # the pump pass sees state 4
        r.send(rr.music(2))
        r.tick()
    C.poll()
    st = C.save()
    check(st["kind"] == "COMMIT" and st["target"] == 5 and st["jobs"] == 1, f"stock-posted commit: window {st}")
    close_window(C, None, "OK", 5, 1 << 5)

    # ------------------------------------------------ G. import decisions (fn 0x24 09 / 08) and a file window
    print("G. 24 09 / 24 08 decisions and the file window")
    S, C = fresh()
    check(C.core_file(0x09, 3) == "PASS" and C.core_file(0x0A, 3) == "PASS", "idle: import / save to USB pass")
    for r in (C,):
        r.send(rr.music_mode(2), rr.music(3))
        r.tick()
    check(C.core_file(0x09, 3) == "BUSY" and C.core_file(0x08, 0) == "BUSY", "armed: import and USB load refused")
    C.send(rr.music(2)), C.tick(), C.poll()
    C.w(rr.FILEOP, 9)          # the executor starts kind 9 (import) for the 24 09 allowed above
    C.poll()
    st = C.save()
    check(st["kind"] == "FILE" and st["target"] == 3, f"file operation: window {st}")
    C.w(rr.FILEOP, 0)          # ended without writing (refused by the stock, e.g. no KSO)
    C.poll()
    st = C.save()
    check(st["closed"] == 1 and st["last"] == "NOWRITE", f"file operation without a write: {st['last']}")

    check(C.rec_cmd_in_core == 0 and C.commit_in_core == 0, "FUN_4016A050 / the commit never ran inside a core call")
    check(not C.stray, f"the core's own stores stay in its window and the stack ({len(C.stray)} stray)")
    print("all checks passed" if not fails else f"{len(fails)} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
