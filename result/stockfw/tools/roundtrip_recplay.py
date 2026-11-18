# roundtrip_recplay.py - Phase 3-6 R1 wired into ota_main (feat2 0x10):
# the core's playback control (recctl) at the front end 0x40147E94 and the pump tail 0x401539AC, op 0x1E, run in the
# stock-image emulator with the payload applied live.  The app (BLE, stock port 2) and the panel (port 3) are both
# bound; the panel holds the stock binding (it CONNECTed last - the field).  Task 20 (FUN_4016DE5C(1)) runs after
# the pump tail every 2 ms.  Song table: tools/stock_songs.py's SYNTHETIC store (never the owner's NOR dump).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_recplay.py [ota_main]
#
# Checks (numbered as printed):
#   1  Identify feat2 0x7F; op 1E 00 from the app = 5E 00 00 + 48 B (state 2 stop, table OK, song bars as the store);
#      op 1E 01 = 5E 01 00 + 70 B (songs 1-4 parts / bars of stock_songs.LAYOUT, 5-10 empty)
#   2  the app selects song 1 with op 1E 02 04 (5E 02 00 04, then 5E 03 01 04): the panel gets the stock's MaxTime
#      52 18 (3 bars) and progress 32 52 03 (stock, its binding), the app the same Kawai frames (core: ring tap) and
#      5E 00 with song 0; the stock apply FUN_4016053C never ran for it
#   3  the app plays song 1 with op 1E 02 01: 5E 02 00 01, the recorder reaches state 1 (task 20 ran the queue: the
#      stock FUN_4016A050 only inside task 20), 5E 03 01 01; the panel gets the stock's Music status
#      F0 40 7F 32 04 1E 52 08 7F 01 F7 - the same bytes as on S (the stock alone, the panel's own 32 52 08 7F 01),
#      the app gets it too (relayed) + a 5E 00 with state 1; the panel never gets a 0x7D frame (it never subscribed)
#   4  the panel's Kawai Stop 32 52 08 7F 02 while the app's play runs: the core takes it (no FUN_4016053C), state 2,
#      the app's 5E 00 says so
#   5  refusals, the same for the panel (Kawai), the app (Kawai) and the app (op): NOR job pending (RecorderData slot
#      flag 0x4139DAB4), recording (state 4 - SongSelect would commit), MusicMode pending (0x419156A4): Kawai senders get
#      the stock's Busy F0 40 7F 7E 04 1E 03 01 00 00 F7, the op gets 5E 02 <rc>; nothing reaches the stock queue or
#      routines (queue count, recorder state, song unchanged); the panel's Stop in state 4 is left to the stock (R2)
#   6  stack: at the stock SongSelect FUN_401687E4 the core's paths (panel Kawai select through the front, app op
#      select through the pump tail) use at most 128 B (192 B with S4 on from boot - images boot_on, the bound of
#      roundtrip_sysex 8) more than S's stock path (panel select, FUN_4014A2C8 ->
#      FUN_4014EBFC -> FUN_4016053C); pump-tail passes: most instructions / deepest stack
#   7  over the whole H run: commit FUN_40168774, RecorderData writer FUN_40168408 and NOR job FUN_40134048 never ran,
#      FUN_4016A050 never ran outside task 20
# R2 payloads (feat3 0x01 - ota_main since the 2026-11-16 fold, before it ota_main_rec2): 1 expects Identify ... 7F 01; 5 the panel's Stop in state 4 is the
# core's (no stock apply, no Busy); the commit and the RecorderData writer RUN (the NOR job is counted, not run) and
#   8  a take on the synthetic store: the app selects the empty song 5 (op), the panel arms with its own Kawai Music 3
#      (core: Music 3 through the queue -> state 3), the app starts with op 1E 02 0B (-> state 4), then the panel's
#      Kawai Stop and the app's op Stop arrive before task 20 runs: ONE finish (fc_out) -> one commit, one RecorderData
#      write, one NOR job, all inside task 20; state 2, the table passes the check, the app gets 5E 01; the panel never
#      got a Busy or a 0x7D frame
#   7  then counts that one commit / write / job instead of none
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rf = rtlib.load("roundtrip_fanout")
rss, rs, images, check = rf.rss, rf.rs, rf.images, rf.check
ss = rf.ss
APP, PANEL = rf.APP, rf.PANEL
ctl, connect, has = rf.ctl, rf.connect, rf.has
SE = rs.SE

OP = 0x1E
REC_CMD, COMMIT, STORE_WRITE, NOR_JOB = 0x4016A050, 0x40168774, 0x40168408, 0x40134048
APPLY = 0x4016053C
SELECT = 0x401687E4
PLAY_WAIT, JOB_KICK, SONG_TEMPO_INIT = 0x401650FC, 0x4013433C, 0x401673F8
STATE, SONG, MQ_COUNT, MQ_HOLD, JOB1 = 0x4191EF79, 0x4191EF77, 0x419156A5, 0x419156A4, 0x4139DAB4
BUSY = bytes.fromhex("F0407F7E041E03010000F7")
PLAY_STATUS = bytes.fromhex("F0407F32041E52087F01F7")
CMD = dict(PLAY=1, STOP=2, PAUSE=3, SELECT=4, PART=5, TEMPO=6, SEEK=7)
RC = {0: "OK", 1: "NOP", 2: "E_ARG", 3: "E_BUSY", 4: "E_REC", 5: "E_PENDING", 6: "E_NODATA", 7: "E_MODE",
      8: "E_STATE", 9: "NOT_BOUND"}


def op_frame(*args):
    return rs.rp.req(OP, *args) + b"\xF7"


def ng(tx, port, sub=None):
    """0x7D replies 5E <sub> .. to port"""
    head = bytes.fromhex("F07D434137395E")
    return [f for _t, p, f in tx if p == port and f.startswith(head) and (sub is None or f[7] == sub)]


def status_of(f):
    raw = rtlib.unpack7(f[9:-1])
    return dict(ver=raw[0], state=raw[1], song=raw[3], mode=raw[6], busy=raw[7], max_bars=raw[16] | raw[17] << 8,
                store=raw[19], bar=raw[29] | raw[30] << 8, pend=raw[32], subs=raw[34], flags=raw[35], n=len(raw))


def kawai(tx, port, head):
    return [f for _t, p, f in tx if p == port and f.startswith(head)]


SAVE_R = {0: "NONE", 1: "OK", 2: "NOWRITE", 3: "NOJOB", 4: "TABLE", 5: "OTHER", 6: "STUCK", 7: "DOUBLE"}
SAVE_K = {0: "-", 1: "COMMIT", 2: "DELETE", 3: "FORMAT", 4: "FILE"}


def save_of(f):
    """5E 04 00 <pack7 24 B> (R3 save decision, REMOTE_PROTOCOL §10.9)"""
    raw = rtlib.unpack7(f[9:-1])
    return dict(on=raw[1], phase=raw[2], kind=SAVE_K.get(raw[3]), target=raw[4], jobs=raw[5], last=SAVE_R.get(raw[6]),
                last_kind=SAVE_K.get(raw[7]), last_target=raw[8], last_jobs=raw[9], windows=raw[10] | raw[11] << 8,
                changed=raw[22] | raw[23] << 8, n=len(raw))


def save_query(n, port):
    """op 1E 04 -> the 5E 04 reply (None when none came)"""
    got = ng(op_and_run(n, port, [0x04], 20), port, 4)
    return save_of(got[-1]) if got else None


class Probe:
    """counts + stack depth at stock routines, and whether a call ran inside task 20"""

    def __init__(self, n, run_commit=False, job_byte=False):
        self.n, e = n, n.e
        self.counts = {COMMIT: 0, STORE_WRITE: 0, NOR_JOB: 0, REC_CMD: 0, APPLY: 0, SELECT: 0}
        self.rec_outside = 0
        self.commit_outside = 0
        self.sel_sp = []
        self.in_t20 = False
        for a in ((NOR_JOB,) if run_commit else (COMMIT, STORE_WRITE, NOR_JOB)):
            e.hook(a, (lambda aa: lambda em: (self._c(aa), 0)[1])(a))   # never run: count and return
        if job_byte:   # R3 payload: the job's pending byte 0x4139DAAC + slot * 8 goes up as FUN_40134048 does (the test
            # clears it when "the job is done") - the save window waits for it
            e.hook(NOR_JOB, lambda em: (self._c(NOR_JOB), em.write_bytes(JOB1 - 8 + 8 * em.reg(0), b"\x01"), 0)[2])
        if run_commit:   # R2 payload: the stock commit and the RecorderData writer run (the job is only counted)
            e.hook(COMMIT, lambda em: (self._c(COMMIT), self._commit(), SE.CONTINUE)[2])
            e.hook(STORE_WRITE, lambda em: (self._c(STORE_WRITE), SE.CONTINUE)[1])
        e.hook(REC_CMD, lambda em: (self._rec(), SE.CONTINUE)[1])
        e.hook(APPLY, lambda em: (self._c(APPLY), SE.CONTINUE)[1])
        e.hook(SELECT, lambda em: (self._c(SELECT), self.sel_sp.append(em.reg(13)), SE.CONTINUE)[2])
        e.hook(PLAY_WAIT, lambda em: 0)
        e.hook(JOB_KICK, lambda em: None)

    def _c(self, a):
        self.counts[a] += 1

    def _commit(self):
        self.commit_outside += not self.in_t20

    def _rec(self):
        self.counts[REC_CMD] += 1
        self.rec_outside += not self.in_t20


class NetR(rf.NetF):
    """roundtrip_fanout.NetF + task 20's own dtq posts kept apart: while a song plays, task 20 posts notices for the
    notify task (FUN_40151A38 -> notify_q, as NetF does) and others; NetF would hand those to the request task model"""

    probe = None

    def _post(self, em):
        if self.probe is not None and self.probe.in_t20 and not (rf.NOTIFY_ENQ <= em.reg(14) < rf.NOTIFY_ENQ_END):
            self.t20_posts = getattr(self, "t20_posts", 0) + 1
            return 0
        return super()._post(em)


def wrap_t20(n, probe):
    """roundtrip_fanout runs task 20 with e.call(TASK20, 1): mark those calls"""
    orig = n.e.call

    def call(addr, *a, **k):
        if addr == rf.TASK20:
            probe.in_t20 = True
            try:
                return orig(addr, *a, **k)
            finally:
                probe.in_t20 = False
        return orig(addr, *a, **k)
    n.e.call = call


def h_rig(m, run_commit=False, job_byte=False):
    side = rf.songs_in(rf.side_settings(m, m.data))
    side.emu.call(SONG_TEMPO_INIT, 120)
    n = NetR(side, pump=True)
    n.pump_log = []
    p = n.probe = Probe(n, run_commit, job_byte)
    wrap_t20(n, p)
    rf.both_bound(n)
    n.run(100)
    n.t20 = True
    n.run(50)
    return n, p


def s_rig(m):
    """S: the stock (ota_loader_v4 built-in cave), the panel alone"""
    side = rf.songs_in(rf.side_settings(m, None))
    side.emu.call(SONG_TEMPO_INIT, 120)
    n = NetR(side)
    p = n.probe = Probe(n)
    wrap_t20(n, p)
    connect(n, PANEL, full=False)
    n.run(50)
    n.t20 = True
    return n, p


def op_and_run(n, port, args, ms):
    n0 = len(n.tx)
    n.send(port, op_frame(*args))
    n.run(ms)
    return n.tx[n0:]


def kawai_and_run(n, port, fn, body, ms):
    n0 = len(n.tx)
    n.send(port, ctl(fn, body))
    n.run(ms)
    return n.tx[n0:]


def main(argv):
    name = rtlib.first_arg(argv, "ota_main")
    pay = images.OTA_PAYLOADS_BY_NAME[name]
    if not pay.feat2 & images.FEAT2_REC:
        raise SystemExit(f"{name}: no playback control (feat2 0x10)")
    rec2 = bool(pay.feat3 & images.FEAT3_REC2)
    rec3 = bool(pay.feat3 & images.FEAT3_REC3)
    ident_tail = bytes([0x7F, pay.feat3]) if pay.feat3 else bytes([0x7F])
    _img, _stock, _manifest, image, syms = rtlib.loader()
    data = (rf.STOCKFW / "build" / f"{name}.c79").read_bytes()
    m = rss.Machines(image, syms, data)
    ok, rows = True, []

    n, pr = h_rig(m, rec2, rec3)
    core = rss.core_diag(n.s)
    # ---- 1 identify, status, list ----
    n0 = len(n.tx)
    n.send(APP, rs.rp.req(0x01) + b"\xF7")
    n.run(20)
    ident = [f for _t, p, f in n.tx[n0:] if p == APP and f[:7] == bytes.fromhex("F07D4341373941")]
    tx = op_and_run(n, APP, [0x00], 20)
    st = ng(tx, APP, 0)
    s0 = status_of(st[0]) if st else None
    tx = op_and_run(n, APP, [0x01], 20)
    ls = ng(tx, APP, 1)
    lraw = rtlib.unpack7(ls[0][9:-1]) if ls else b""
    songs = [(lraw[7 * i + 4] | lraw[7 * i + 5] << 8, lraw[7 * i + 6] & 3) for i in range(10)] if len(lraw) == 70 else None
    want = [(max(a, b), (1 if a else 0) | (2 if b else 0)) for a, b in ss.LAYOUT] + [(0, 0)] * 6
    ok &= check(f"1 {name}: table {core and hex(core['bound'])} binding {n.b(rs.BINDING)}; Identify "
                f"{ident and ident[0][7:-1].hex()}; 1E 00 -> {s0}; 1E 01 songs (bars, parts) {songs}",
                core is not None and core["bound"] == 0x0C and n.b(rs.BINDING) == 4 and len(ident) == 1
                and ident[0][7:-1].endswith(ident_tail) and s0 is not None and s0["n"] == 48 and s0["ver"] == 1
                and s0["store"] == 0 and s0["subs"] == 1 << APP and songs == want)
    rows.append(("1", "Identify · 1E 00 상태 · 1E 01 곡 목록",
                 f"feat2 `0x7F`{' · feat3 `0x%02X`' % pay.feat3 if pay.feat3 else ''}, 상태 48 B(표 검사 OK), 곡 목록 = 합성 곡 표 {songs[:4] if songs else None} + 빈 곡 6"))

    # ---- 2 app selects song 1 (op) ----
    a0 = pr.counts[APPLY]
    n0 = len(n.tx)
    n.send(APP, op_frame(0x02, CMD["SELECT"], 0, 0))
    n.run(20)
    posted = n.run_notify_posts()
    n.run(200)
    tx = n.tx[n0:]
    r2, e2 = ng(tx, APP, 2), ng(tx, APP, 3)
    mt_p, mt_a = [f for f in kawai(tx, PANEL, rf.VALUE_HEAD) if f[6:8] == b"\x52\x18"], \
        [f for f in kawai(tx, APP, rf.VALUE_HEAD) if f[6:8] == b"\x52\x18"]
    pg_p, pg_a = kawai(tx, PANEL, bytes.fromhex("F0407F32041E5203")), kawai(tx, APP, bytes.fromhex("F0407F32041E5203"))
    sts = [status_of(f) for f in ng(tx, APP, 0)]
    ok &= check(f"2 app op SELECT 0: replies {[f[7:-1].hex() for f in r2]} events {[f[7:-1].hex() for f in e2]}; "
                f"notify posts {posted}; MaxTime panel {[f.hex() for f in mt_p]} app {len(mt_a)}; progress panel "
                f"{len(pg_p)} app {len(pg_a)}; app 5E 00 songs {[s['song'] for s in sts]}; stock apply runs "
                f"{pr.counts[APPLY] - a0}; panel 0x7D frames {len(ng(tx, PANEL))}",
                [f[7:-1] for f in r2] == [bytes([2, 0, 4])] and [f[7:-1] for f in e2] == [bytes([3, 1, 4])]
                and len(mt_p) == 1 and mt_p == mt_a and mt_p[0] == bytes.fromhex(ss.maxtime(3).replace(" ", ""))
                and len(pg_p) == len(pg_a) >= 1 and pr.counts[APPLY] == a0 and not ng(tx, PANEL)
                and n.b(SONG) == 0)
    rows.append(("2", "앱 op `1E 02 04` 곡 1 선택(패널이 스톡 바인딩)",
                 f"`5E 02 00 04` → `5E 03 01 04`, MaxTime(3마디) 패널(스톡) = 앱(탭 중계), 스톡 적용 0회, 패널에 0x7D 0"))

    # ---- 3 app plays song 1 (op) ----
    # S: the stock, the panel's own Music play
    s, sp_ = s_rig(m)
    s0n = len(s.tx)
    s.send(PANEL, ctl(0x10, [0x52, 0x00, 0x7F, 0, 0, 0]))
    s.run(100)
    s.send(PANEL, ctl(0x31, [0x52, 0x1D, 0x7F, 0x02]))   # the stock drops Music in MusicMode 0: the panel enters 2
    s.run(50)
    s.send(PANEL, ctl(0x32, [0x52, 0x08, 0x7F, 0x01]))
    s.run(600)
    s_play = kawai(s.tx[s0n:], PANEL, PLAY_STATUS)
    s_state = s.b(STATE)
    s_sel_sp = list(sp_.sel_sp)
    s.s.close()
    a0 = pr.counts[APPLY]
    tx = op_and_run(n, APP, [0x02, CMD["PLAY"], 0, 0], 600)
    r3, e3 = ng(tx, APP, 2), ng(tx, APP, 3)
    h_play_p, h_play_a = kawai(tx, PANEL, PLAY_STATUS), kawai(tx, APP, PLAY_STATUS)
    sts = [status_of(f)["state"] for f in ng(tx, APP, 0)]
    st3 = n.b(STATE)
    ok &= check(f"3 app op PLAY: replies {[f[7:-1].hex() for f in r3]} events {[f[7:-1].hex() for f in e3]}; state "
                f"{st3} (S {s_state}); Music status to the panel {len(h_play_p)} (S {len(s_play)}), the app "
                f"{len(h_play_a)}; app 5E 00 states {sts}; FUN_4016A050 outside task 20 {pr.rec_outside}; stock apply "
                f"{pr.counts[APPLY] - a0}; panel 0x7D frames {len(ng(tx, PANEL))}",
                [f[7:-1] for f in r3] == [bytes([2, 0, 1])] and [f[7:-1] for f in e3] == [bytes([3, 1, 1])]
                and st3 == 1 and s_state == 1 and h_play_p == s_play and len(h_play_p) == 1 and h_play_a == h_play_p
                and 1 in sts and pr.rec_outside == 0 and pr.counts[APPLY] == a0 and not ng(tx, PANEL))
    rows.append(("3", "앱 op `1E 02 01` 재생",
                 f"`5E 02 00 01` → 상태 1 → `5E 03 01 01`, 패널은 스톡의 `32 52 08 7F 01`(S와 같은 바이트), 앱도 (중계 + `5E 00`), "
                 f"`FUN_4016A050`은 태스크 20 안에서만"))

    # ---- 4 the panel's Kawai stop ----
    a0 = pr.counts[APPLY]
    tx = kawai_and_run(n, PANEL, 0x32, [0x52, 0x08, 0x7F, 0x02], 600)
    sts = [status_of(f)["state"] for f in ng(tx, APP, 0)]
    ok &= check(f"4 panel Kawai Stop while playing: state {n.b(STATE)}; stock apply {pr.counts[APPLY] - a0}; app 5E 00 "
                f"states {sts}; frames to the panel {[f.hex() for _t, p, f in tx if p == PANEL]}",
                n.b(STATE) == 2 and pr.counts[APPLY] == a0 and 2 in sts and not kawai(tx, PANEL, BUSY))
    rows.append(("4", "패널 Kawai 정지(`32 52 08 7F 02`), 앱 재생 중", f"코어가 받아 상태 2, 앱 `5E 00`에 정지"))

    # ---- 5 refusals, same for every controller ----
    n.t20 = False
    res = []

    def refusal(label, poke, unpoke, op_cmd, kawai_fn, kawai_body, want_rc):
        poke()
        q0, st0, sg0, sel0 = n.b(MQ_COUNT), n.b(STATE), n.b(SONG), pr.counts[SELECT]
        a_ = pr.counts[APPLY]
        tp = kawai_and_run(n, PANEL, kawai_fn, kawai_body, 50)
        ta = kawai_and_run(n, APP, kawai_fn, kawai_body, 50)
        to = op_and_run(n, APP, [0x02, op_cmd, 0, 0], 50)
        rc = [f[8] for f in ng(to, APP, 2)]
        bp, ba = len(kawai(tp, PANEL, BUSY)), len(kawai(ta, APP, BUSY))
        same = (n.b(MQ_COUNT), n.b(STATE), n.b(SONG), pr.counts[SELECT], pr.counts[APPLY]) == (q0, st0, sg0, sel0, a_)
        unpoke()
        res.append((label, bp, ba, rc, same))
        return bp == 1 and ba == 1 and rc == [want_rc] and same

    ok5 = refusal("NOR job", lambda: n.s.poke(JOB1, b"\x01"), lambda: n.s.poke(JOB1, b"\x00"),
                  CMD["PLAY"], 0x32, [0x52, 0x08, 0x7F, 0x01], 3)
    ok5 &= refusal("recording (state 4)", lambda: n.s.poke(STATE, b"\x04"), lambda: n.s.poke(STATE, b"\x02"),
                   CMD["SELECT"], 0x10, [0x52, 0x00, 0x7F, 0, 0, 1], 4)
    ok5 &= refusal("MusicMode pending", lambda: n.s.poke(MQ_HOLD, b"\x82"), lambda: n.s.poke(MQ_HOLD, b"\x00"),
                   CMD["PLAY"], 0x32, [0x52, 0x08, 0x7F, 0x01], 3)
    # the panel's Stop in state 4 is R2's: left to the stock (FUN_4016053C runs, the core sends no Busy)
    n.s.poke(STATE, b"\x04")
    a_ = pr.counts[APPLY]
    tp = kawai_and_run(n, PANEL, 0x32, [0x52, 0x08, 0x7F, 0x02], 50)
    stop_rec = (pr.counts[APPLY] - a_, len(kawai(tp, PANEL, BUSY)))
    n.s.poke(STATE, b"\x02")
    n.run(50)
    ok &= check(f"5 refusals (label, Busy to the panel, Busy to the app, op rc, nothing reached the stock) {res}; panel "
                f"Stop in state 4 -> stock apply {stop_rec[0]}, Busy {stop_rec[1]}",
                ok5 and stop_rec == ((0, 0) if rec2 else (1, 0)))
    rows.append(("5", "거절: NOR 잡 · 녹음 중(상태 4) · MusicMode 보류",
                 "패널(Kawai) · 앱(Kawai) 둘 다 스톡 Busy `7E 03 01 00 00`, 앱 op는 `5E 02 03` · `04` · `03`, 큐 · 상태 · 곡 그대로, "
                 "스톡 적용 0 · 녹음 중 패널 Stop은 " + ("코어가 받음(R2 켬)" if rec2 else "스톡으로(R2)")))

    # ---- 5b (R3 payload) the pokes above left state 4 -> 2 without a job: a commit window that sees no job ----
    if rec3:
        sv0 = save_query(n, APP)
        sel_busy = [f[7:-1].hex() for f in ng(op_and_run(n, APP, [0x02, CMD["SELECT"], 0, 0], 50), APP, 2)]
        n0 = len(n.tx)
        n.run(3200)                                               # RS_WAIT_POLLS: 3 s for a job
        closed = [save_of(f) for f in ng(n.tx[n0:], APP, 4)]
        sv1 = save_query(n, APP)
        ok &= check(f"5b R3: state 4 -> 2 without a job: 1E 04 {sv0}; op select meanwhile {sel_busy}; after 3 s 5E 04 to "
                    f"the app {closed[-1:] if closed else closed}; 1E 04 {sv1}",
                    sv0 is not None and sv0["on"] == 1 and sv0["phase"] == 1 and sv0["kind"] == "COMMIT"
                    and sel_busy == ["020304"] and len(closed) == 1 and closed[0]["last"] == "NOJOB"
                    and closed[0]["last_kind"] == "COMMIT" and sv1["phase"] == 0)
        rows.append(("5b", "R3: 위 찌르기의 상태 4 → 2(잡 없음)", "확정 창이 열림(`5E 04` 단계 1 · 종류 1), 그동안 op 곡 선택 `5E 02 03 04`(바쁨), "
                     "3 s 뒤 창이 닫히며 앱에 `5E 04` 결과 3(잡 없음) 한 번"))

    # ---- 6 stack at FUN_401687E4 ----
    # a boot-on payload (images boot_on) runs the panel's set through S4's
    # io_apply -> s4_apply: the bound of roundtrip_sysex 8 for that path (192 B, measured 168 B in the Thumb build)
    lim = 192 if pay.boot_on else 128
    n.t20 = True
    pr.sel_sp.clear()
    kawai_and_run(n, PANEL, 0x10, [0x52, 0x00, 0x7F, 0, 0, 1], 200)
    sp_front = list(pr.sel_sp)
    pr.sel_sp.clear()
    op_and_run(n, APP, [0x02, CMD["SELECT"], 0, 0], 200)
    sp_pump = list(pr.sel_sp)
    base = max(s_sel_sp) if s_sel_sp else None
    ex_front = [base - x for x in sp_front] if base else None
    ex_pump = [base - x for x in sp_pump] if base else None
    pl = rf.pump_stats(n.pump_log)
    # a pass without an op command: playing (5E 00 bar notices) and stopped
    op_and_run(n, APP, [0x02, CMD["PLAY"], 0, 0], 100)
    p0 = len(n.pump_log)
    n.run(600)
    play_idle = rf.pump_stats(n.pump_log[p0:])
    op_and_run(n, APP, [0x02, CMD["STOP"], 0, 0], 100)
    p0 = len(n.pump_log)
    n.run(300)
    stop_idle = rf.pump_stats(n.pump_log[p0:])
    ok &= check(f"6 stack at FUN_401687E4: S (stock panel select) sp {base and hex(base)}; H extra bytes: front "
                f"{ex_front}, pump tail {ex_pump}; pump tail (passes, max insns, max stack B) all {pl} (the op select runs the "
                f"stock loader there), playing {play_idle}, stopped {stop_idle}",
                base is not None and len(sp_front) == 1 and len(sp_pump) == 1 and max(ex_front) <= lim
                and max(ex_pump) <= lim)
    rows.append(("6", "스택(`FUN_401687E4` 진입) · 펌프 꼬리",
                 f"S 대비 앞단 {ex_front} B · 펌프 꼬리 {ex_pump} B(≤ {lim}{', S4 켬' if pay.boot_on else ''}); 펌프 꼬리 한 차례 최대 명령어 · 스택: 전체 {pl[1]} · "
                 f"{pl[2]} B(op 곡 선택 = 스톡 로더), 재생 중 {play_idle[1]} · {play_idle[2]} B, 정지 {stop_idle[1]} · {stop_idle[2]} B"))

    # ---- 8 (R2 payload) a take: panel arm, app start, panel + app stop together ----
    takes = 0
    if rec2:
        takes = 1
        a0 = pr.counts[APPLY]
        op_and_run(n, APP, [0x02, CMD["STOP"], 0, 0], 100)
        tx8 = op_and_run(n, APP, [0x02, CMD["SELECT"], 0, 4], 200)
        sel8 = [f[7:-1].hex() for f in ng(tx8, APP, 3)]
        n0 = len(n.tx)
        n.send(PANEL, ctl(0x32, [0x52, 0x08, 0x7F, 0x03]))       # the panel's own Rec button (Music 3)
        n.run(100)
        st_arm = n.b(STATE)
        tx = op_and_run(n, APP, [0x02, 11, 0, 0], 100)            # op start
        r_start = [f[7:-1].hex() for f in ng(tx, APP, 2)]
        st_rec = n.b(STATE)
        c0 = dict(pr.counts)
        t20 = n.t20
        n.t20 = False                                             # both stops before task 20 runs
        n.send(PANEL, ctl(0x32, [0x52, 0x08, 0x7F, 0x02]))
        n.run(2)
        n.send(APP, op_frame(0x02, CMD["STOP"], 0, 0))
        n.run(4)
        q_after = n.b(MQ_COUNT)
        n.t20 = t20
        n.run(400)
        tx = n.tx[n0:]
        r_stop = [f[7:-1].hex() for f in ng(tx, APP, 2) if f[9] == CMD["STOP"]]
        lists = ng(tx, APP, 1)
        st_end = status_of(ng(tx, APP, 0)[-1]) if ng(tx, APP, 0) else None
        d = {k: pr.counts[k] - c0[k] for k in (COMMIT, STORE_WRITE, NOR_JOB)}
        ok &= check(f"8 take: select song 5 events {sel8}; panel Music 3 -> state {st_arm}; op start replies {r_start} -> "
                    f"state {st_rec}; panel Stop + op Stop before task 20: queue {q_after}, op stop replies {r_stop}; "
                    f"then state {n.b(STATE)}, commit / write / NOR job {list(d.values())}, commit outside task 20 "
                    f"{pr.commit_outside}, FUN_4016A050 outside {pr.rec_outside}; app 5E 01 {len(lists)}, last 5E 00 "
                    f"{st_end}; Busy to the panel {len(kawai(tx, PANEL, BUSY))}, panel 0x7D {len(ng(tx, PANEL))}; stock apply "
                    f"{pr.counts[APPLY] - a0}",
                    sel8 == ["030104"] and st_arm == 3 and r_start == ["02000b"] and st_rec == 4 and q_after == 1
                    and len(r_stop) == 1 and r_stop[0] in ("020102", "020502") and n.b(STATE) == 2
                    and list(d.values()) == [1, 1, 1] and pr.commit_outside == 0 and pr.rec_outside == 0
                    and len(lists) >= 1 and st_end is not None and st_end["store"] == 0 and st_end["state"] == 2
                    and not kawai(tx, PANEL, BUSY) and not ng(tx, PANEL) and pr.counts[APPLY] == a0)
        rows.append(("8", "녹음 한 번(R2 켬): 앱 곡 5 선택 → 패널 Rec(Music 3) → 앱 op 시작 → 패널 정지 + 앱 op 정지(태스크 20 전)",
                     f"상태 3 → 4 → 2, 끝내기 1번(큐 1) → 확정 · 곡 쓰기 · NOR 잡 각 1, 모두 태스크 20 안; 앱 op 정지 `5E 02 {r_stop[0][2:] if r_stop else '-'}`, "
                     f"`5E 01` 곡 표, 표 검사 OK; 패널에 Busy · 0x7D 0, 스톡 적용 0"))

    # ---- 8b (R3 payload) the take's write window: song commands refused while the job runs, then 5E 04 OK ----
    if rec3:
        sv0 = save_query(n, APP)
        sel_busy = [f[7:-1].hex() for f in ng(op_and_run(n, APP, [0x02, CMD["SELECT"], 0, 0], 50), APP, 2)]
        tk = kawai_and_run(n, APP, 0x32, [0x52, 0x21, 0x7F, 0x04, 0, 1, 0x7F], 50)   # delete song 2 meanwhile
        n0 = len(n.tx)
        n.s.poke(JOB1, b"\x00")                                  # the NOR worker finishes
        n.run(50)
        closed = [save_of(f) for f in ng(n.tx[n0:], APP, 4)]
        sel_ok = [f[7:-1].hex() for f in ng(op_and_run(n, APP, [0x02, CMD["SELECT"], 0, 4], 200), APP, 3)]
        ok &= check(f"8b R3 take window: 1E 04 {sv0}; op select {sel_busy}, Kawai delete Busy "
                    f"{len(kawai(tk, APP, BUSY))}; job done -> 5E 04 {closed}; select again events {sel_ok}",
                    sv0 is not None and sv0["phase"] == 2 and sv0["kind"] == "COMMIT" and sv0["target"] == 4
                    and sv0["jobs"] == 1 and sel_busy == ["020304"] and len(kawai(tk, APP, BUSY)) == 1
                    and len(closed) == 1 and closed[0]["last"] == "OK" and closed[0]["last_kind"] == "COMMIT"
                    and closed[0]["last_target"] == 4 and closed[0]["last_jobs"] == 1 and closed[0]["changed"] == 1 << 4
                    and sel_ok == ["030104"])
        rows.append(("8b", "R3: 8의 확정 창(NOR 잡 진행 중)", "`1E 04` 단계 2 · 확정 · 곡 5 · 잡 1, op 곡 선택 `5E 02 03 04`(바쁨), Kawai 곡 삭제 Busy; "
                     "잡 끝 → 앱에 `5E 04` 결과 1 OK · 종류 확정 · 대상 곡 5 · 잡 1 · 바뀐 곡 비트 곡 5 하나, 그 뒤 곡 선택 받음"))

    # ---- 7 nothing recorded or saved (R2: only the take of 8) ----
    c = pr.counts
    ok &= check(f"7 whole H run: commit {c[COMMIT]}, RecorderData writer {c[STORE_WRITE]}, NOR job {c[NOR_JOB]}, "
                f"FUN_4016A050 {c[REC_CMD]} (outside task 20 {pr.rec_outside}, commit outside {pr.commit_outside})",
                c[COMMIT] == takes and c[STORE_WRITE] == takes and c[NOR_JOB] == takes and pr.rec_outside == 0
                and pr.commit_outside == 0 and c[REC_CMD] >= 2)
    rows.append(("7", "녹음 · 저장 없음" if not rec2 else "확정 · 저장은 8의 한 번뿐",
                 f"확정 · 곡 쓰기 · NOR 잡 {takes}, `FUN_4016A050` {c[REC_CMD]}회 모두 태스크 20 안"))
    n.s.close()

    print("\n| # | 시나리오 | 결과 |\n|---|---|---|")
    for r in rows:
        print(f"| {r[0]} | {r[1]} | {r[2]} |")
    return rtlib.finish(ok)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
