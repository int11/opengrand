# roundtrip_recdel.py - InternalSongDelete owned by the core (result/firmware/src/recorder/recdel.c + ports/cave/
# recctl_cave.c own_delete), run in the stock-image emulator.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_recdel.py [ota_main] [--payload=<file.c79>]
# payloads with R3 (feat3 0x04 - ota_main since the fold, before it ota_main_rec3; save decision): each delete also inside its write window
# (1E 04 phase 2 while the job runs, 5E 04 OK with only that song changed), an absent part leaves no window.
#
# S. the STOCK delete (the ARM glue rig of roundtrip_recsave.py, Kawai frames through the stock apply FUN_4016053C):
#    two takes on song 5 (part 1, then part 2 - the field's song 4) prepare the record track table (FUN_40168A70); then
#    32 52 21 7F 04 00 04 7F (song 5, both parts) answers 32 51 7E 7F 00 00 and queues NOR jobs but both parts stay
#    byte for byte (only the start-settings block is re-written), and 04 00 01 7F (song 2) turns song 2 into a copy of
#    song 5 - FUN_4016C7BC re-serialises the record track table, it removes nothing.  This is the field bug of
#    2026-11-15 (song 4 just recorded, "deleted", both parts still there) and what it would have done to another song.
# H. the slot payload (default ota_main) applied live on ota_loader_v4, the app (BLE, port 2) and the panel (port 3)
#    bound, tools/stock_songs.py's SYNTHETIC store (never the owner's dump): the app's Kawai delete bytes
#    1 status byte 31 bit 0 (the app's cue that this firmware deletes)
#    2 song 2 (part 1 + part 2), part 2: one stock writer FUN_40168408 call + one NOR job, the stock delete
#      FUN_4016C800 never runs, 32 51 7E 7F 00 00; song 2 keeps part 1 only (its bytes = part 1's, part 2's gone),
#      songs 1, 3-10 byte for byte, the table passes the core's boot-check rules, 1E 01 lists it at once
#    3 song 2 again, both parts (7F, the app's "Delete"): song 2 = the ROM empty-song template (0x400A7574 + 0x74),
#      the others unchanged
#    4 song 3 (part 2 only), part 2 = its last track: the template
#    5 refusals: a NOR job pending, playing (state 1), song 12 -> the stock's Busy, no write
#    6 the field's shape: S's store (song 5 serialised by the stock from two takes) put in: part 1 -> part 2 alone,
#      moved down to part 1's place, its bytes unchanged; then the song -> the template; the others unchanged
# A payload without the fix (--payload=<the field's 547ced84 .c79>) fails from 1 on: the stock apply runs the delete,
# which reads the empty record track table (address 0 - no take since this rig's boot) and stops the emulator.
from __future__ import annotations

import pathlib
import struct
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402

rp = rtlib.load("roundtrip_recplay")
rf, rss, images, check = rp.rf, rp.rss, rp.images, rp.check
ss, APP, PANEL, ctl = rp.ss, rp.APP, rp.PANEL, rp.ctl
SE = rp.SE

STOCK_DELETE = 0x4016C800
TEMPLATE = 0x400A7574
EDIT_OK = bytes.fromhex("F0407F32041E517E7F0000F7")
BUSY = rp.BUSY


def table(e):
    head = e.read_bytes(ss.REC, 0x30)
    offs = list(struct.unpack_from("<12I", head))
    return offs


def songs(e):
    o = table(e)
    return [e.read_bytes(ss.REC + o[i], o[i + 1] - o[i]) for i in range(10)]


def tracks(b):
    return [struct.unpack_from("<I", b, 0x2C + 4 * t)[0] for t in range(16)] if len(b) >= 0x74 else []


def store_ok(e):
    """the boot check FUN_4016E174's rules + ours (recview.h): starts in range, 4-aligned, ascending, tracks start FA"""
    o = table(e)
    if o[11] != ss.REC_END or e.read_bytes(ss.REC + ss.REC_END, 2) != b"\x5A\xA5":
        return False
    if e.read_bytes(ss.REC + o[10], 2) != b"\x5A\xA5":
        return False
    for i in range(10):
        if not (0x30 <= o[i] < ss.REC_END) or o[i] & 3 or o[i + 1] < o[i]:
            return False
        b = e.read_bytes(ss.REC + o[i], o[i + 1] - o[i])
        for t in tracks(b):
            if t and (t >= len(b) or b[t] != 0xFA):
                return False
    return True


def stock_part():
    """S: the stock delete after a take (roundtrip_recsave's rig, the stock apply runs the delete for real)"""
    rsv = rtlib.load("roundtrip_recsave")
    rr, rec = rsv.rr, rsv.rec
    with tempfile.TemporaryDirectory() as tmp:
        blob, syms = rr.build_arm(pathlib.Path(tmp))
    S = rsv.Rig("S", blob, syms)
    S.call(rr.SONG_TEMPO_INIT, 120)
    S.send(ss.song_select(4))
    S.send(rr.music_mode(2))
    for part in (0, 1):   # the field's song 4: part 1, then part 2 (each its own take, the stock commit)
        S.send(ss.int_song_part(part))
        S.send(rr.music(3))
        S.tick(1)
        S.send(rr.music(4))
        S.tick(1)
        S.notes(rec.PLAY_NOTES)
        S.send(rr.music(2))
        S.tick(1)
        S.worker_done()

    def song_bytes(i):
        o = S.offsets()
        return S.e.read_bytes(ss.REC + o[i], o[i + 1] - o[i])

    take = song_bytes(4)
    store = S.e.read_bytes(ss.REC, 0x100000)   # the stock-serialised two-part song, for H section 6
    j0 = len(S.pn.jobs)
    f5 = S.send(rsv.delete(4, 0x7F))
    S.worker_done()
    after5 = song_bytes(4)
    j5 = len(S.pn.jobs) - j0
    before2 = song_bytes(1)
    f2 = S.send(rsv.delete(1, 0x7F))
    S.worker_done()
    after2 = song_bytes(1)
    def body(b):   # the two tracks (part 1 from its offset to the data end +0x6C)
        t = tracks(b)
        return b[t[0]:struct.unpack_from("<I", b, 0x6C)[0]] if t and t[0] else b""

    t5, t5a = [t for t in tracks(take) if t], [t for t in tracks(after5) if t]
    ok = check(f"S stock delete after two takes on song 5 (tracks {t5}, {len(take)} B): song 5 both parts -> frames "
               f"{[rr.p3.hexs(f) for f in f5]}, NOR jobs {j5}; song 5 now {len(after5)} B, tracks {t5a}, both parts' bytes "
               f"unchanged {body(after5) == body(take)} (only the start-settings block re-written); song 2 both parts -> "
               f"song 2 {len(before2)} -> {len(after2)} B, now a copy of song 5 {after2[0x20:] == after5[0x20:]}",
               len(t5) == 2 and len(t5a) == 2 and body(after5) == body(take) and j5 >= 1 and EDIT_OK in f5
               and after2 != before2 and after2[0x20:] == after5[0x20:])
    return ok, (len(take), len(after5), len(before2), len(after2)), store


def main(argv):
    name = next((a for a in argv[1:] if not a.startswith("--")), "ota_main")
    pay_file = next((a.split("=", 1)[1] for a in argv[1:] if a.startswith("--payload=")), None)
    pay = images.OTA_PAYLOADS_BY_NAME[name]
    if not pay.feat2 & images.FEAT2_REC:
        raise SystemExit(f"{name}: no playback control (feat2 0x10)")
    ok, s_lens, s_store = stock_part()
    rows = [("S", "스톡 삭제(곡 5에 파트 1 · 2 녹음 직후, 곡 5 · 다른 곡 2)",
             f"곡 5 {s_lens[0]} → {s_lens[1]} B, 두 파트 바이트 그대로(시작 설정 블록만 다시 씀) · OK 응답 · NOR 잡 둘 — 곡 2는 {s_lens[2]} → {s_lens[3]} B, 곡 5의 복사본")]

    _img, _stock, _manifest, image, syms = rtlib.loader()
    data = pathlib.Path(pay_file).read_bytes() if pay_file else (rf.STOCKFW / "build" / f"{name}.c79").read_bytes()
    m = rss.Machines(image, syms, data)
    rec3 = bool(pay.feat3 & images.FEAT3_REC3)
    n, pr = rp.h_rig(m, run_commit=True, job_byte=rec3)   # R3: the job's pending byte goes up, the window waits for it
    if rec3:
        rp.save_query(n, APP)                                   # subscribe to 5E 04
    e = n.e
    stock_del = [0]
    e.hook(STOCK_DELETE, lambda em: (stock_del.__setitem__(0, stock_del[0] + 1), SE.CONTINUE)[1])
    tpl = [e.read_bytes(TEMPLATE + 0x74 * i, 0x74) for i in range(10)]

    # ---- 1 status byte 31 ----
    tx = rp.op_and_run(n, APP, [0x00], 20)
    st = rp.ng(tx, APP, 0)
    raw = rtlib.unpack7(st[0][9:-1]) if st else b""
    ok &= check(f"1 {name}: 1E 00 status byte 31 = {raw[31] if len(raw) > 31 else None:#04x} (state {raw[1]}, busy {raw[7]}, table {raw[19]})",
                len(raw) == 48 and raw[31] & 1 == 1)
    rows.append(("1", "`1E 00` 상태 바이트 31", "`0x01`(이 펌웨어는 곡을 스스로 지운다 — 앱이 삭제 단추를 켜는 조건)"))

    def delete(song, part, label, want_song):
        before = songs(e)
        c0 = dict(pr.counts)
        d0 = stock_del[0]
        try:
            tx = rp.kawai_and_run(n, APP, 0x32, [0x52, 0x21, 0x7F, 0x04, (song >> 7) & 0x7F, song & 0x7F, part], 50)
        except RuntimeError as ex:   # a payload without the fix: the stock delete reads the empty track table (addr 0)
            return check(f"{label}: the emulator stopped ({str(ex)[:160]})", False), before[song], b"", None
        after = songs(e)
        tl = rp.op_and_run(n, APP, [0x01], 20)
        ls = rp.ng(tl, APP, 1)
        lraw = rtlib.unpack7(ls[0][9:-1]) if ls else b""
        flags = lraw[7 * song + 6] & 3 if len(lraw) == 70 else None
        ok_ = (EDIT_OK in [f for _t, p, f in tx if p in (APP, PANEL)]
               and pr.counts[rp.STORE_WRITE] - c0[rp.STORE_WRITE] == 1 and pr.counts[rp.NOR_JOB] - c0[rp.NOR_JOB] == 1
               and stock_del[0] == d0 and all(after[i] == before[i] for i in range(10) if i != song)
               and want_song(before[song], after[song]) and store_ok(e))
        r = check(f"{label}: song {song + 1} {len(before[song])} -> {len(after[song])} B, tracks "
                  f"{[t for t in tracks(after[song]) if t]}; writer {pr.counts[rp.STORE_WRITE] - c0[rp.STORE_WRITE]}, NOR "
                  f"job {pr.counts[rp.NOR_JOB] - c0[rp.NOR_JOB]}, stock delete {stock_del[0] - d0}; OK frame "
                  f"{EDIT_OK in [f for _t, p, f in tx if p in (APP, PANEL)]}; other songs unchanged "
                  f"{all(after[i] == before[i] for i in range(10) if i != song)}; table check {store_ok(e)}; 1E 01 parts "
                  f"{flags}", ok_)
        if rec3:   # R3: the delete's write window holds until the job is done, then 5E 04 OK with only this song
            sv = rp.save_query(n, APP)
            n0 = len(n.tx)
            n.s.poke(rp.JOB1, b"\x00")
            n.run(20)
            closed = [rp.save_of(f) for f in rp.ng(n.tx[n0:], APP, 4)]
            win = (sv is not None and sv["phase"] == 2 and sv["kind"] == "DELETE" and sv["target"] == song
                   and len(closed) == 1 and closed[0]["last"] == "OK" and closed[0]["last_kind"] == "DELETE"
                   and closed[0]["last_jobs"] == 1 and closed[0]["changed"] == 1 << song)
            r &= check(f"{label}: R3 window 1E 04 {sv and (sv['phase'], sv['kind'], sv['target'], sv['jobs'])} -> 5E 04 "
                       f"{[(c['last'], c['last_kind'], c['last_target'], c['last_jobs'], hex(c['changed'])) for c in closed]}", win)
        else:
            n.s.poke(rp.JOB1, b"\x00")
            n.run(20)
        return r, before[song], after[song], flags

    # ---- 2 song 2 part 2 ----
    def part2_gone(b, a):
        t, end = tracks(b), struct.unpack_from("<I", b, 0x6C)[0]   # part 2 = [t1, data end): the last track
        cut = end - t[1]
        return (tracks(a)[1] == 0 and tracks(a)[0] == t[0] and a[0x74:t[1]] == b[0x74:t[1]] and a[:0x2C] == b[:0x2C]
                and struct.unpack_from("<I", a, 0x6C)[0] == t[1] and len(a) == len(b) - cut + (-(len(b) - cut)) % 4)
    r, b2, a2, fl = delete(1, 1, "2 song 2 part 2", part2_gone)
    ok &= r and fl == 1
    rows.append(("2", "곡 2(파트 1 + 2) 파트 2 지우기", f"{len(b2)} → {len(a2)} B, 파트 1만 남음(바이트 그대로), 스톡 쓰기 1 · NOR 잡 1, "
                 f"스톡 삭제 0, `32 51 7E 7F 00 00`, 다른 곡 그대로, 표 검사 OK, `1E 01` 파트 {fl}"))

    # ---- 3 song 2 whole ----
    r, b3, a3, fl = delete(1, 0x7F, "3 song 2 both parts", lambda b, a: a == tpl[1])
    ok &= r and fl == 0
    rows.append(("3", "곡 2 「삭제」(7F)", f"{len(b3)} → {len(a3)} B = ROM 빈 곡 틀(`0x400A7574 + 0x74`), 다른 곡 그대로"))

    # ---- 4 song 3 part 2 = its only track ----
    r, b4, a4, fl = delete(2, 1, "4 song 3 part 2 (its only track)", lambda b, a: a == tpl[2])
    ok &= r and fl == 0
    rows.append(("4", "곡 3(파트 2만) 파트 2 지우기", f"{len(b4)} → {len(a4)} B = 빈 곡 틀"))

    # ---- 4b (R3) a part the song does not have: nothing written, no window left open (rs_nowrite) ----
    if rec3:
        before, c0 = songs(e), dict(pr.counts)
        tx = rp.kawai_and_run(n, APP, 0x32, [0x52, 0x21, 0x7F, 0x04, 0, 0, 1], 30)   # song 1 = part 1 only
        sv = rp.save_query(n, APP)
        r = check(f"4b R3 song 1 part 2 (absent): OK frame {EDIT_OK in [f for _t, p, f in tx if p in (APP, PANEL)]}, writer "
                  f"{pr.counts[rp.STORE_WRITE] - c0[rp.STORE_WRITE]}, NOR job {pr.counts[rp.NOR_JOB] - c0[rp.NOR_JOB]}, "
                  f"table unchanged {songs(e) == before}; 1E 04 phase {sv and sv['phase']} windows {sv and sv['windows']}",
                  EDIT_OK in [f for _t, p, f in tx if p in (APP, PANEL)] and pr.counts[rp.STORE_WRITE] == c0[rp.STORE_WRITE]
                  and pr.counts[rp.NOR_JOB] == c0[rp.NOR_JOB] and songs(e) == before and sv is not None
                  and sv["phase"] == 0 and sv["windows"] == 3)
        ok &= r
        rows.append(("4b", "R3: 곡 1(파트 1만) 파트 2 지우기", "`32 51 7E 7F 00 00`, 쓰기 · NOR 잡 0, 곡 표 그대로, 쓰기 창이 남지 않음(`1E 04` 단계 0, 창 수 그대로)"))

    # ---- 5 refusals ----
    res = []
    t20, n.t20 = n.t20, False   # task 20 would end the poked "playing" state (no song plays) before the request lands
    for label, poke, unpoke, song in (("NOR job", (rp.JOB1, 1), (rp.JOB1, 0), 0),
                                      ("playing", (rp.STATE, 1), (rp.STATE, 2), 0),
                                      ("song 12", None, None, 11)):
        if poke:
            n.s.poke(poke[0], bytes([poke[1]]))
        h0, c0 = songs(e), pr.counts[rp.STORE_WRITE]
        tx = rp.kawai_and_run(n, APP, 0x32, [0x52, 0x21, 0x7F, 0x04, 0, song, 0x7F], 30)
        res.append((label, BUSY in [f for _t, p, f in tx if p == APP], songs(e) == h0 and pr.counts[rp.STORE_WRITE] == c0))
        if unpoke:
            n.s.poke(unpoke[0], bytes([unpoke[1]]))
    n.t20 = t20
    ok &= check(f"5 refusals (label, Busy to the app, nothing written) {res}", all(b and w for _l, b, w in res))
    rows.append(("5", "거절: NOR 잡 · 재생 중 · 곡 12", "앱에 스톡 Busy `7E 03 01 00 00`, 곡 표 그대로"))

    # ---- 6 the field's shape: a song the STOCK serialised from two takes (S above), part 1 then the song ----
    e.write_bytes(ss.REC, s_store)
    n.s.poke(rp.JOB1, b"\x01")   # as after a NOR job: the job's end makes the core read the table again (R3: the settled
    n.run(20)                   # table it compares the next write with)
    n.s.poke(rp.JOB1, b"\x00")
    n.run(20)
    t5 = tracks(songs(e)[4])

    def part1_gone(b, a):
        t, cut = tracks(b), tracks(b)[1] - tracks(b)[0]   # part 1 = [t0, t1): part 2 moves down to t0
        return (tracks(a)[0] == 0 and tracks(a)[1] == t[0] and a[t[0]:] == b[t[1]:t[1] + len(a) - t[0]]
                and len(a) == len(b) - cut and a[0x74:t[0]] == b[0x74:t[0]])
    r, b6, a6, fl6 = delete(4, 0, "6 song 5 (stock-serialised parts 1 + 2) part 1", part1_gone)
    r7, _b7, a7, fl7 = delete(4, 0x7F, "6 then song 5 both parts", lambda b, a: a == tpl[4])
    ok &= r and r7 and fl6 == 2 and fl7 == 0 and t5[0] != 0 and t5[1] != 0
    rows.append(("6", "스톡이 녹음으로 쓴 곡 5(파트 1 + 2, 실기 곡 4 모양): 파트 1, 그다음 곡",
                 f"{len(b6)} → {len(a6)} B 파트 2만(제자리로 당김, 바이트 그대로) → {len(a7)} B 빈 곡 틀, 다른 곡 그대로"))
    n.s.close()

    print("\n| # | 시나리오 | 결과 |\n|---|---|---|")
    for r in rows:
        print(f"| {r[0]} | {r[1]} | {r[2]} |")
    return rtlib.finish(ok)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
