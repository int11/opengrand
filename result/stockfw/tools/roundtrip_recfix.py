# roundtrip_recfix.py - Phase 3-6 R3 repair of a broken commit (2026-11-24 field incident: the piano formatted all ten
# songs at the next power-on after a take with no keys played).  Stock bug [REA FUN_4016804C]: the serialiser sets track
# slot 0 of the song to the write cursor unconditionally (0x401682A0) and only a slot-0 take that holds data replaces
# it - so a take of PART 2 with no events on a song without part 1 leaves slot 0 = the song length, pointing at the next
# song; the boot check FUN_4016E174 fails on it and the next power-on formats the store.  The core (recsave.h head
# comment, recdel.h rd_repair) mends the target song with the stock writer FUN_40168408, before the NOR worker starts
# when it can (the job still pending programs the mended RAM), else right after the job (a second job).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_recfix.py [ota_main] [-q] [--no-h] [--payload=<file.c79>]
# A payload without the repair (the field's ota_main before this change) fails 7 and 8: R3 TABLE, the boot check fails.
#
# All on tools/stock_songs.py's SYNTHETIC store (songs 1-4 recorded, 3 = part 2 only, 5-10 empty).
# S  the stock alone (Kawai frames through the stock apply, ARM-glue rig of roundtrip_recsave.py, R3 not polled):
#    1  MusicMode 2, song 6, IntSongPart 1 (record part 2), Music 3 -> 4 -> 300 task-20 passes, no keys -> Music 2:
#       one commit, one NOR job, song 6 = 0x74 B with slot 0 = 0x74 (its own length), the boot check FAILS
#    2  the same on song 3 (part 2 only): the boot check fails as well; part 1 with no keys / part 2 with keys: passes
#    3  Music 1 in record standby (state 3): the stock stays in 3 (nothing to play), no commit
# C  the core's glue (recctl_arm.c rca_* = the cave's rcc_pump order) on the same sequence through rc_cmd (cmd 5 part,
#    8 arm, 11 start, 2 stop):
#    4  worker idle when the commit is seen: the core rewrites song 6 = the ROM empty template while the commit's job is
#       still pending, R3 result FIXED, jobs posted 2 (commit + mend, the same pending slot - one programming on the
#       piano), every job posted AFTER the mend sees a table that passes, the boot check passes at the end
#    5  worker busy (0x4139DAC4) when the commit is seen: no write while it runs; job done -> the mend -> a second job,
#       FIXED, the boot check passes; nothing else in the store moved
#    6  a part-1 take with no keys (valid): OK, no mend; a take with keys on part 2: OK, no mend
# H  the slot payload (default ota_main) applied live, the app bound (BLE): the field's bytes (Kawai MusicMode 2, op
#    select song 6, Kawai IntSongPart 1, Kawai Music 3, the raw Music 4, 300 ms, the raw Music 2):
#    7  worker idle: 5E 04 last FIXED, song 6 = template, every NOR job posted after the mend sees a passing table, the
#       boot check passes, 1E 01 lists song 6 empty
#    8  worker busy at the commit (poked): mended right after the job, 5E 04 FIXED, the boot check passes
from __future__ import annotations

import hashlib
import pathlib
import struct
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402

rsv = rtlib.load("roundtrip_recsave")
rr, rec, ss = rsv.rr, rsv.rec, rsv.ss
StockEmu = rr.StockEmu

BOOT_CHECK = 0x4016E174       # FUN_4016E174: the RecorderData boot check (0 = passes; else the boot path formats)
TEMPLATE = 0x400A7574         # ROM empty song headers, 0x74 B each
WORKER = 0x4139DAC4           # the NOR worker is erasing / programming (FUN_401340B0)
JOB_BASE = rsv.JOB_BASE
LAYOUT = [(3, 0), (2, 5), (0, 4), (200, 0)]
rsv.R[8] = "FIXED"   # RS_R_FIXED (recsave.h)
KEEP, OVERWRITE = rec.KEEP, rec.OVERWRITE


def table(buf):
    return list(struct.unpack_from("<12I", buf, 0))


def store_ok(buf):
    """the boot check's rules + recview's structure rules on a 1 MB RecorderData image"""
    o = table(buf)
    if o[11] != ss.REC_END or buf[ss.REC_END:ss.REC_END + 2] != b"\x5A\xA5" or buf[o[10]:o[10] + 2] != b"\x5A\xA5":
        return False
    for i in range(10):
        if not (0x30 <= o[i] < ss.REC_END) or o[i] & 3 or o[i + 1] < o[i] + 0x74:
            return False
        for t in range(16):
            to = struct.unpack_from("<I", buf, o[i] + 0x2C + 4 * t)[0]
            if to and (o[i] + to >= ss.REC_END or buf[o[i] + to] != 0xFA):
                return False
    return True


def song_of(buf, i):
    o = table(buf)
    return bytes(buf[o[i]:o[i + 1]])


def slots(b):
    return [struct.unpack_from("<I", b, 0x2C + 4 * t)[0] for t in range(16)]


class Rig(rsv.Rig):
    """roundtrip_recsave's rig; every NOR job post also keeps whether the RAM table then passes"""

    def __init__(self, name, blob, syms, store=None):
        super().__init__(name, blob, syms, store)
        self.job_ok = []

        def on_job(em):
            slot = em.reg(0)
            self.pn.jobs.append((slot, em.reg(1), em.reg(2)))
            self.w(JOB_BASE + 8 * slot, 1)
            self.job_ok.append(store_ok(bytes(em.read_bytes(ss.REC, 0x100000))))
            return 0

        self.e.hook(ss.NOR_JOB, on_job)

    def store(self):
        return bytes(self.e.read_bytes(ss.REC, 0x100000))

    def boot(self):
        return self.call(BOOT_CHECK)


def stock_take(blob, syms, song, part, notes=False):
    S = Rig("S", blob, syms, store=ss.store(LAYOUT))
    S.call(rr.SONG_TEMPO_INIT, 120)
    for m in (rr.music_mode(2), ss.song_select(song), ss.int_song_part(part), rr.music(3), rr.music(4)):
        S.send(m)
        S.tick(2)
    if notes:
        S.notes(rec.PLAY_NOTES)
    S.tick(300)
    S.send(rr.music(2))
    S.tick(5)
    return S


def core_take(blob, syms, song, part, notes=False, worker_busy=False):
    """C: the same take through the core (rc_cmd), rca_poll after every task-20 pass as the cave's pump"""
    C = Rig("C", blob, syms, store=ss.store(LAYOUT))
    C.call(rr.SONG_TEMPO_INIT, 120)

    def run(n):
        for _ in range(n):
            C.tick()
            C.poll()

    C.send(rr.music_mode(2))
    run(2)
    res = [C.core("SELECT", song)[0]]
    run(2)
    res.append(C.core("PART", part)[0])
    run(2)
    res.append(C.core("REC", KEEP | OVERWRITE)[0])
    run(2)
    res.append(C.core("REC_START")[0])
    run(2)
    if notes:
        C.notes(rec.PLAY_NOTES)
    run(300)
    base = C.store()
    res.append(C.core("STOP")[0])
    C.tick()                       # task 20: FC -> the commit -> its NOR job (pending byte up)
    if worker_busy:
        C.w(WORKER, 1)             # the worker took the job before the core's next pass
    C.poll()
    seen = dict(C.save(), store=C.store(), jobs=len(C.pn.jobs))
    if worker_busy:
        for _ in range(20):
            C.tick()
            C.poll()
        seen["busy_jobs"] = len(C.pn.jobs)
        C.w(WORKER, 0)
        C.worker_done()            # the job programmed (the broken table) and finished
    for _ in range(3):
        C.tick()
        C.poll()
    C.w(WORKER, 0)
    C.worker_done()                # the mend's job (or the commit's, re-posted) done
    for _ in range(3):
        C.tick()
        C.poll()
    return C, res, base, seen


def main(argv):
    quiet = "-q" in argv
    name = next((a for a in argv[1:] if not a.startswith("-")), "ota_main")
    fails = []

    def check(cond, what):
        if not rtlib.check(what, cond):
            fails.append(what)
        return cond

    with tempfile.TemporaryDirectory() as tmp:
        blob, syms = rr.build_arm(pathlib.Path(tmp))
    tpl = None

    # ---------------------------------------------------------------- S: the stock bug
    S = stock_take(blob, syms, 5, 1)
    b6 = song_of(S.store(), 5)
    tpl = [bytes(S.e.read_bytes(TEMPLATE + 0x74 * i, 0x74)) for i in range(10)]
    check(S.commits == 1 and len(S.pn.jobs) == 1 and len(b6) == 0x74 and slots(b6)[0] == 0x74 and not store_ok(S.store())
          and S.boot() != 0,
          f"1 S song 6, part 2, no keys: commits {S.commits}, jobs {len(S.pn.jobs)}, song 6 {len(b6)} B slot 0 "
          f"{slots(b6)[0]:#x} (= its length), table check {store_ok(S.store())}, boot check {S.boot():#x} (fails)")
    rows = [("1", "스톡: 빈 곡 6, 파트 2, 건반 없음", f"확정 1 · 잡 1, 곡 6 {len(b6)} B 슬롯 0 = `{slots(b6)[0]:#x}`(곡 길이), 부팅 검사 실패")]
    res2 = []
    for song, part, notes, want in ((2, 1, False, False), (5, 0, False, True), (5, 1, True, True), (0, 1, False, True)):
        S = stock_take(blob, syms, song, part, notes)
        res2.append((song + 1, part + 1, notes, S.boot() == 0))
        check((S.boot() == 0) == want, f"2 S song {song + 1}, part {part + 1}, keys {notes}: boot check "
                                       f"{'passes' if S.boot() == 0 else 'FAILS'} (want {'pass' if want else 'fail'})")
    rows.append(("2", "스톡: 곡 3(파트 2만) 파트 2 건반 없음 / 곡 6 파트 1 건반 없음 / 곡 6 파트 2 건반 / 곡 1(파트 1) 파트 2 건반 없음",
                 " · ".join(f"곡 {s} 파트 {p}{' 건반' if n else ''}: {'통과' if ok else '실패'}" for s, p, n, ok in res2)))
    S = Rig("S", blob, syms, store=ss.store(LAYOUT))
    for m in (rr.music_mode(2), ss.song_select(5), rr.music(3)):
        S.send(m)
        S.tick(2)
    S.send(rr.music(1))
    S.tick(50)
    st = S.view()["state"]
    check(st == 3 and S.commits == 0, f"3 S Music 1 in record standby: state {st} (stays armed), commits {S.commits}")
    rows.append(("3", "스톡: 녹음 대기(3)에서 Music 1", f"상태 {st} 그대로, 확정 0 (코어는 `E_REC` Busy)"))

    # ---------------------------------------------------------------- C: the core's glue
    C, res, base, seen = core_take(blob, syms, 5, 1)
    fin = C.store()
    a6 = song_of(fin, 5)
    sv = C.save()
    others = all(song_of(fin, i) == song_of(base, i) for i in range(10) if i != 5)
    check(res == ["OK"] * 5 and a6 == tpl[5] and store_ok(fin) and C.boot() == 0 and sv["last"] == "FIXED"
          and len(C.pn.jobs) == 2 and C.job_ok[-1] and others and C.commits == 1,
          f"4 C song 6, part 2, no keys, worker idle: cmds {res}; song 6 = template {a6 == tpl[5]}, others unchanged "
          f"{others}, table {store_ok(fin)}, boot check {C.boot():#x}; R3 {sv['last']} jobs {sv['last_jobs']}; NOR job "
          f"posts {len(C.pn.jobs)} table-ok-at-post {C.job_ok}")
    rows.append(("4", "코어(rc_cmd 5 · 8 · 11 · 2) 곡 6 파트 2 건반 없음, 작업자 쉼",
                 f"확정 잡이 대기 중에 곡 6 = 빈 곡 틀로 고침, R3 `FIXED`, 잡 넣기 {len(C.pn.jobs)}(같은 대기 칸 — 실기에선 한 번 프로그램), "
                 f"마지막 잡 때 표 통과 {C.job_ok[-1]}, 부팅 검사 통과"))

    C, res, base, seen = core_take(blob, syms, 5, 1, worker_busy=True)
    fin = C.store()
    sv = C.save()
    others = all(song_of(fin, i) == song_of(base, i) for i in range(10) if i != 5)
    check(seen["busy_jobs"] == 1 and not store_ok(seen["store"]) and song_of(fin, 5) == tpl[5] and store_ok(fin)
          and C.boot() == 0 and sv["last"] == "FIXED" and sv["last_jobs"] == 2 and len(C.pn.jobs) == 2 and C.job_ok == [False, True]
          and others,
          f"5 C worker busy at the commit: no write while it runs (jobs {seen['busy_jobs']}, table still broken), after the "
          f"job: mended, R3 {sv['last']} jobs {sv['last_jobs']}, table-ok-at-post {C.job_ok}, boot check {C.boot():#x}, "
          f"others unchanged {others}")
    rows.append(("5", "코어, 확정 때 작업자가 이미 돎", f"도는 동안 쓰지 않음, 잡 끝나자 고침 → 둘째 잡(표 통과), R3 `FIXED` 잡 2, 부팅 검사 통과"))

    r6 = []
    for part, notes in ((0, False), (1, True)):
        C, res, base, seen = core_take(blob, syms, 5, part, notes)
        sv = C.save()
        r6.append((part + 1, notes, sv["last"], len(C.pn.jobs), C.boot() == 0))
        check(sv["last"] == "OK" and len(C.pn.jobs) == 1 and C.boot() == 0,
              f"6 C song 6, part {part + 1}, keys {notes}: R3 {sv['last']}, NOR jobs {len(C.pn.jobs)} (no mend), boot check "
              f"{C.boot():#x}")
    rows.append(("6", "코어, 정상 녹음(파트 1 건반 없음 · 파트 2 건반)", " · ".join(f"파트 {p}{' 건반' if n else ''}: {l}, 잡 {j}" for p, n, l, j, _b in r6)))

    # ---------------------------------------------------------------- H: the payload
    if "--no-h" in argv:   # the glue sections only (no built payload needed)
        return rtlib.finish(not fails)
    rp = rtlib.load("roundtrip_recplay")
    rp.SAVE_R[8] = "FIXED"
    images = rp.images
    pay = images.OTA_PAYLOADS_BY_NAME[name]
    if not pay.feat3 & images.FEAT3_REC3:
        raise SystemExit(f"{name}: no save decision (feat3 0x04)")
    _img, _stock, _manifest, image, hsyms = rtlib.loader()
    pay_file = next((a.split("=", 1)[1] for a in argv[1:] if a.startswith("--payload=")), None)
    data = pathlib.Path(pay_file).read_bytes() if pay_file else (rp.rf.STOCKFW / "build" / f"{name}.c79").read_bytes()

    def h_take(worker_busy):
        m = rp.rss.Machines(image, hsyms, data)
        n, pr = rp.h_rig(m, run_commit=True, job_byte=True)
        e = n.e
        posts = []

        def on_job(em):
            pr.counts[rp.NOR_JOB] += 1
            em.write_bytes(rp.JOB1 - 8 + 8 * em.reg(0), b"\x01")
            posts.append(store_ok(bytes(em.read_bytes(ss.REC, 0x100000))))
            if worker_busy and len(posts) == 1:
                em.write_bytes(WORKER, b"\x01")   # the worker takes the commit's job at once
            return 0

        e.hook(rp.NOR_JOB, on_job)
        rp.save_query(n, rp.APP)
        base = bytes(e.read_bytes(ss.REC, 0x100000))
        rp.kawai_and_run(n, rp.APP, 0x31, [0x52, 0x1D, 0x7F, 2], 20)          # MusicMode 2
        rp.op_and_run(n, rp.APP, [0x02, 4, 0, 5], 30)                         # op select song 6
        rp.kawai_and_run(n, rp.APP, 0x10, [0x52, 0x01, 0x7F, 1], 20)          # IntSongPart 1 = record part 2
        rp.kawai_and_run(n, rp.APP, 0x32, [0x52, 0x08, 0x7F, 3], 30)          # Music 3: record standby
        rp.kawai_and_run(n, rp.APP, 0x32, [0x52, 0x08, 0x7F, 4], 300)         # the raw Music 4, no keys
        rp.kawai_and_run(n, rp.APP, 0x32, [0x52, 0x08, 0x7F, 2], 20)          # the raw Music 2: the commit
        mid = bytes(e.read_bytes(ss.REC, 0x100000))
        if worker_busy:
            n.run(40)
            busy_posts = len(posts)
            n.s.poke(WORKER, b"\x00")
        n.s.poke(rp.JOB1, b"\x00")
        n.run(20)
        n.s.poke(WORKER, b"\x00")
        n.s.poke(rp.JOB1, b"\x00")
        n0 = len(n.tx)
        n.run(40)
        closed = [rp.save_of(f) for f in rp.ng(n.tx[n0:], rp.APP, 4)]
        sv = rp.save_query(n, rp.APP)
        fin = bytes(e.read_bytes(ss.REC, 0x100000))
        tl = rp.op_and_run(n, rp.APP, [0x01], 20)
        ls = rp.ng(tl, rp.APP, 1)
        lraw = rtlib.unpack7(ls[0][9:-1]) if ls else b""
        boot = e.call(BOOT_CHECK, max_insns=50_000_000).r0
        out = dict(posts=posts, sv=sv, closed=closed, fin=fin, base=base, mid=mid, boot=boot,
                   listed=(struct.unpack_from("<I", lraw, 7 * 5)[0] if len(lraw) == 70 else None),
                   commits=pr.counts[rp.COMMIT], busy_posts=busy_posts if worker_busy else None)
        n.s.close()
        return out

    for k, busy in (("7", False), ("8", True)):
        h = h_take(busy)
        others = all(song_of(h["fin"], i) == song_of(h["base"], i) for i in range(10) if i != 5)
        ok = (h["commits"] == 1 and song_of(h["fin"], 5) == tpl[5] and store_ok(h["fin"]) and h["boot"] == 0 and others
              and h["sv"] is not None and h["sv"]["last"] == "FIXED" and h["posts"] and h["posts"][-1] and h["listed"] == 0x74)
        # the commit's own post sees the broken table, the mend's post a passing one; idle: the mend reached the still
        # pending job (one job in the window), busy: nothing while the worker ran, then a second job
        ok = ok and h["posts"] == [False, True] and h["sv"]["last_jobs"] == (2 if busy else 1)
        if busy:
            ok = ok and h["busy_posts"] == 1
        check(ok, f"{k} H {name} the field's bytes, worker {'busy' if busy else 'idle'} at the commit: commits {h['commits']}, "
                  f"song 6 = template {song_of(h['fin'], 5) == tpl[5]}, others unchanged {others}, table {store_ok(h['fin'])}, "
                  f"boot check {h['boot']:#x}, 5E 04 {h['sv'] and (h['sv']['last'], h['sv']['last_jobs'])}, closed "
                  f"{[(c['last'], c['last_jobs']) for c in h['closed']]}, NOR job posts table-ok {h['posts']}, 1E 01 song 6 "
                  f"length {h['listed']}")
        rows.append((k, f"`{name}` 실기 바이트 그대로(Kawai MusicMode 2 · op 곡 6 · IntSongPart 1 · Music 3 · 원시 Music 4 · 300 ms · 원시 Music 2), 작업자 {'바쁨' if busy else '쉼'}",
                     f"`5E 04` `{h['sv'] and h['sv']['last']}` 잡 {h['sv'] and h['sv']['last_jobs']}, 곡 6 = 빈 곡 틀, 잡 넣을 때 표 {h['posts']}, 부팅 검사 통과, `1E 01` 곡 6 {h['listed']} B"))

    if not quiet:
        print("\n| # | 시나리오 | 결과 |\n|---|---|---|")
        for r in rows:
            print(f"| {r[0]} | {r[1]} | {r[2]} |")
    return rtlib.finish(not fails, "" if not fails else f" ({len(fails)} failed)")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
