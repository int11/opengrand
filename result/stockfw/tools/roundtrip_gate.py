# roundtrip_gate.py - Phase 4 A intake gate: the core decides which of the
# remaining Kawai frames the stock handler FUN_4014A2C8 runs - from the existing front-end hook 0x40147E94, after the
# session core - and the stock only executes.  H = ota_main (feat3 0x02) applied live on ota_loader_v4, O = the same
# ota_main with the gate taken out in the emulator (its verdict kpg_rx answers KPG_PASS at once, check 0: every frame
# goes on to the stock handler - the stock decides, the reference for "same observable behaviour"), S = the stock
# alone (ota_loader_v4 built-in cave, the app is the stock binding).  Machines and network as roundtrip_fanout.py (NetF: the
# virtual 2 ms clock, the panel / BLE app link models, the pump tail every 2 ms on H / O).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_gate.py [ota_main]
#
# FUN_4014A2C8 entries are counted at its first instruction (the stub tail-calls it when the front passes a frame).
# App + panel bound on H / O, the panel CONNECTs last = the stock binding (4):
# 0 O: kpg_rx hooked to answer KPG_PASS; a frame H drops (fn 08 from the binding) enters the stock handler, 1B 06
#   dropped stands still.
#   1 info 7E 04 40 / 04 41 / 05 40 / 05 41 / 05 42: from the panel (binding) the stock answers, from the app (bound, not the
#     binding) the CORE answers with the same bytes, to the app only, the stock handler never entered (deliberate difference);
#     O: the app gets nothing (the stock's gate FUN_40151BCC), the stock handler entered each time; S (app = binding): the
#     same bytes as H's panel / app.  7E 04 42 (4 MB checksum) from the app: dropped on H, nothing sent.
#   2 refusals of the binding's frames the stock ignores (fn 08 / 20, foreign device byte, m[4] 09, 7E 00 / 03, 7E 04 3F,
#     7E 05 43, fn 03 with n 9): H nothing sent, FUN_4014A2C8 entries 0, op 1B 06 dropped + n; O: the same nothing sent but
#     every frame entered the stock handler.  App fn 09 / fn 03: dropped on H (stock: not the binding).
#   3 test mode 7E 02 03 xx: model class *0x419156A7 as booted -> H drops it from the panel; class 1 -> H passes the panel's
#     (FUN_4013D614 called once with xx, stubbed), drops the app's (no call); O: the app's enters the stock handler, no call.
#   4 the binding's file intake (panel = binding): no USB memory -> 24 7F + 7E 03 00 00 21; a list running -> 24 7F + Busy;
#     a job pending -> 24 7F + Busy for 24 06; mode 5 (*0x4139F4C6) -> 24 7F only.  H: the core sends them, FUN_4014F018 not
#     called, the stock handler not entered; S (the app as binding, same RAM states): the stock's own bytes - equal.  Idle with
#     USB memory: H passes, the stock acks and queues the list (FUN_4014F018 once, the file task gets message 0).
#   5 op 1B 06 counters agree with 1-4; op 1B 03 / 1B 04 still answer (the session core untouched).
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rf = rtlib.load("roundtrip_fanout")
rss, rs, images, check = rf.rss, rf.rs, rf.images, rf.check
APP, PANEL, ctl, connect = rf.APP, rf.PANEL, rf.ctl, rf.connect
SE = rs.SE
STOCKFW = rf.STOCKFW

A2C8 = 0x4014A2C8                    # FUN_4014A2C8: the stock Kawai handler (REA)
TEST_MODE = 0x4013D614               # FUN_4013D614(m[8]): factory test mode - stubbed, counted
MODEL = 0x419156A7                   # FUN_4015BA64 (literal 0x4015C39C): model class, test mode only when != 0
MODE = 0x4139F4C6                    # FUN_400B143C (literal 0x400B173C): 5 = the list request only ACKs
FILE_LIST = rf.FILE_LIST             # FUN_4014F018
MEDIA, LIST_BUSY, FILE_OP = rf.MEDIA, rf.LIST_BUSY, rf.FILE_OP
FILE_ACK, BUSY = rf.FILE_ACK, rf.BUSY
NOMEDIA = bytes.fromhex("F0407F7E041E03000021F7")   # FUN_4014DC0C(0x21) = FUN_4014AEE0(0x7F, 3, 0, {00 21})
SYS_HEAD = bytes.fromhex("F0407F7E041E")
INFO = [[0x04, 0x40], [0x04, 0x41], [0x05, 0x40], [0x05, 0x41], [0x05, 0x42]]
# the words the info getters read (FUN_400B39B8 / FUN_400B39F0 / FUN_400B39D8, literal 0x4014ACD4) - the last is a byte
INFO_RAM = {0x413A77A0: 2, 0x413A77B0: 1, 0x413A77A4: 0x33}
INFO_D = 0x418CE0EC


class Rig:
    """One side + NetF + counters of the stock handler, the list request and test mode."""

    def __init__(self, side, pump):
        self.n = rf.NetF(side, pump=pump)
        self.e, self.s = self.n.e, side
        self.a2c8 = 0
        self.lists = 0
        self.tests = []

        def on_a2c8(em):
            self.a2c8 += 1
            return SE.CONTINUE

        def on_list(em):
            self.lists += 1
            return SE.CONTINUE

        def on_test(em):
            self.tests.append(em.reg(0) & 0xFF)
            return 0

        self.e.hook(A2C8, on_a2c8)
        self.e.hook(FILE_LIST, on_list)
        self.e.hook(TEST_MODE, on_test)

    def frames(self, port, f, ms=40):
        """send one controller frame, let it be handled: (piano frames to every port, handler entries)"""
        n0, a0 = len(self.n.tx), self.a2c8
        self.n.send(port, f)
        self.n.run(ms)
        return [(p, x) for _t, p, x in self.n.tx[n0:]], self.a2c8 - a0

    def close(self):
        self.s.close()


def sides(m):
    H = Rig(rs.Side(m.image, m.syms, m.data), True)
    H.passes = 0
    if getattr(m, "no_gate_at", None):   # O: the gate's verdict function answers KPG_PASS (no_gate)
        def on_kpg(em):
            H.passes += 1
            return 0
        H.e.hook(m.no_gate_at, on_kpg)
    connect(H.n, APP, full=False)
    connect(H.n, PANEL, full=False)
    H.n.run(100)
    return H


def gate_diag(side, port=APP):
    d = rf.diag(side, 0x06, port)
    if d is None:
        return None
    le = rs.le
    return dict(layout=d[0], flags=d[1], bound=d[2], binding=d[3], passed=le(d, 4), dropped=le(d, 8),
                info_own=d[12] | d[13] << 8, file_busy=d[14] | d[15] << 8, file_err=d[16] | d[17] << 8,
                file_ack=d[18] | d[19] << 8, test_drop=d[20] | d[21] << 8, other_drop=d[22] | d[23] << 8)


def to(tx, port):
    return [f for p, f in tx if p == port]


def info(m, mo, rows):
    H, O = sides(m), sides(mo)
    S = Rig(rs.Side(m.image, m.syms, None), False)
    connect(S.n, APP, full=False)
    S.n.run(100)
    for r in (H, O, S):   # the info data as on this piano (model table entry 2 "KP813Q   (CA79 )"), not the emulator's zeros
        for a, v in INFO_RAM.items():
            r.s.poke(a, v.to_bytes(4, "little"))
        r.s.poke(INFO_D, b"\x05")
    ok = True
    g0 = gate_diag(H.s)
    res = []
    for q in INFO:
        tp, ep = H.frames(PANEL, ctl(0x7E, q))
        ta, ea = H.frames(APP, ctl(0x7E, q))
        to_, eo = O.frames(APP, ctl(0x7E, q))
        ts, es = S.frames(APP, ctl(0x7E, q))
        res.append((q, to(tp, PANEL), to(tp, APP), ep, to(ta, APP), to(ta, PANEL), ea, to(to_, APP), eo, to(ts, APP)))
    t42, e42 = H.frames(APP, ctl(0x7E, [0x04, 0x42]))
    g1 = gate_diag(H.s)
    for q, pan, pan_app, ep, app, app_pan, ea, oapp, eo, sapp in res:
        good = (len(pan) == 1 and pan[0].startswith(SYS_HEAD + bytes([q[0], q[1] & 0x3F])) and not pan_app and ep == 1
                and app == pan and not app_pan and ea == 0 and not oapp and eo == 1 and sapp == pan)
        ok &= check(f"1 7E {q[0]:02X} {q[1]:02X}: panel (binding) <- {[f.hex() for f in pan]} (stock, handler {ep}); app <- "
                    f"{[f.hex() for f in app]} (core, handler {ea}), panel extra {len(app_pan)}; O app <- {len(oapp)} (handler "
                    f"{eo}); S app (binding) <- {[f.hex() for f in sapp]}", good)
    ok &= check(f"1 7E 04 42 (flash checksum) from the app: sent {[(p, f.hex()) for p, f in t42]}, handler {e42}; 1B 06 info_own "
                f"{g0 and g0['info_own']} -> {g1 and g1['info_own']}",
                not t42 and e42 == 0 and g0 is not None and g1 is not None and g1["info_own"] - g0["info_own"] == len(INFO))
    rows.append(("1", "정보 `7E 04/05`(패널 = 스톡 바인딩, 앱 = 바운드)",
                 f"패널: 스톡 답 · 앱: 코어가 같은 바이트 {len(INFO)}개(스톡 처리기 0번), O 앱 0 · S 앱(바인딩) 같은 바이트; "
                 f"`7E 04 42`(4 MB 체크섬)는 앱에서 버림"))
    for r in (H, O, S):
        r.close()
    return ok


def refusals(m, mo, rows):
    H, O = sides(m), sides(mo)
    bad_m4 = bytearray(ctl(0x10, [0x55, 0x08, 0x00, 0x04]))
    bad_m4[4] = 0x09
    frames = [ctl(0x08, [0x00]), ctl(0x20, [0x00, 0x00]), ctl(0x09, [], dev=0x11), bytes(bad_m4),
              ctl(0x7E, [0x00, 0x00]), ctl(0x7E, [0x03, 0x00]), ctl(0x7E, [0x04, 0x3F]), ctl(0x7E, [0x05, 0x43]),
              ctl(0x03, [0x55, 0x7F])]
    g0 = gate_diag(H.s, PANEL)
    hs, os_ = [], []
    for f in frames:
        hs.append(H.frames(PANEL, f))
        os_.append(O.frames(PANEL, f))
    g1 = gate_diag(H.s, PANEL)
    app09, e09 = H.frames(APP, ctl(0x09, []))
    app03, e03 = H.frames(APP, ctl(0x03, [0x55, 0x7F, 0x7F]))
    pan03, p03 = H.frames(PANEL, ctl(0x03, [0x55, 0x7F, 0x7F]))
    h_tx = sum(len(t) for t, _e in hs)
    h_in = sum(e for _t, e in hs)
    o_tx = sum(len(t) for t, _e in os_)
    o_in = sum(e for _t, e in os_)
    dropped = (g1["dropped"] - g0["dropped"]) if g0 and g1 else None
    ok = check(f"2 {len(frames)} frames the stock ignores, from the panel (binding): H sent {h_tx}, handler entries {h_in}, 1B 06 "
               f"dropped +{dropped}; O sent {o_tx}, handler entries {o_in}; app fn 09 -> sent {len(app09)} handler {e09}, app "
               f"fn 03 -> {len(app03)} / {e03}; panel fn 03 (n 10) -> handler {p03}, sent {len(pan03)}",
               h_tx == 0 and h_in == 0 and dropped == len(frames) and o_tx == 0 and o_in == len(frames)
               and not app09 and e09 == 0 and not app03 and e03 == 0 and p03 == 1 and not pan03)
    rows.append(("2", "스톡이 무시하는 프레임 9종(패널 = 바인딩) · 앱의 fn 09 / 03",
                 f"H: 보낸 것 0 · 스톡 처리기 0번 · `1B 06` 버림 +{dropped}; O: 보낸 것 0 · 처리기 {o_in}번(같은 결과, 판단만 스톡)"))
    # ---- 3 test mode ----
    cls = H.n.b(MODEL)
    t_def, e_def = H.frames(PANEL, ctl(0x7E, [0x02, 0x03, 0x01]))
    for r in (H, O):
        r.s.poke(MODEL, b"\x01")
    t_pan, e_pan = H.frames(PANEL, ctl(0x7E, [0x02, 0x03, 0x05]))
    t_app, e_app = H.frames(APP, ctl(0x7E, [0x02, 0x03, 0x06]))
    t_o, e_o = O.frames(APP, ctl(0x7E, [0x02, 0x03, 0x06]))
    g2 = gate_diag(H.s, PANEL)
    ok &= check(f"3 test mode 7E 02 03: model class {cls} -> panel handler {e_def} calls {H.tests[:0] or []}; class 1 -> panel "
                f"handler {e_pan}, FUN_4013D614 {H.tests}; app handler {e_app}; O app handler {e_o} calls {O.tests}; 1B 06 test "
                f"drops {g2 and g2['test_drop']}",
                (cls != 0 or (e_def == 0 and not t_def)) and e_pan == 1 and H.tests == ([0x01] if cls else []) + [0x05]
                and e_app == 0 and not t_app and e_o == 1 and not O.tests and not t_pan and not t_o
                and g2 is not None and g2["test_drop"] == (0 if cls else 1) + 1)
    rows.append(("3", "시험 모드 `7E 02 03`", f"모델 클래스 {cls}면 패널 것도 버림, 1이면 패널만 스톡으로(`FUN_4013D614` 1번), "
                                          f"앱 것 버림 · O 앱: 처리기 들어가 스톡이 버림"))
    for r in (H, O):
        r.close()
    return ok


def file_intake(m, rows):
    H = sides(m)
    S = Rig(rs.Side(m.image, m.syms, None), False)
    connect(S.n, APP, full=False)
    S.n.run(100)
    ok = True
    g0 = gate_diag(H.s, PANEL)
    # (name, pokes, frame body, expected)
    cases = [("no USB memory", {MEDIA: 0}, [0x11], [FILE_ACK, NOMEDIA]),
             ("list running", {MEDIA: 1, LIST_BUSY: 1}, [0x11], [FILE_ACK, BUSY]),
             ("job pending", {LIST_BUSY: 0, FILE_OP: 6}, [0x06], [FILE_ACK, BUSY]),
             ("mode 5", {FILE_OP: 0, MODE: 5}, [0x11], [FILE_ACK])]
    mode0 = H.n.b(MODE)
    out = []
    for name, pokes, body, want in cases:
        for a, v in pokes.items():
            H.s.poke(a, bytes([v]))
            S.s.poke(a, bytes([v]))
        l0h, l0s = H.lists, S.lists
        th, eh = H.frames(PANEL, ctl(0x24, body))
        ts, es = S.frames(APP, ctl(0x24, body))
        hp, ha, sa = to(th, PANEL), to(th, APP), to(ts, APP)
        good = hp == want and not ha and eh == 0 and H.lists == l0h and sa == want
        ok &= check(f"4 {name}: H panel <- {[f.hex() for f in hp]} (core; handler {eh}, FUN_4014F018 {H.lists - l0h}), app "
                    f"{len(ha)}; S app (binding) <- {[f.hex() for f in sa]} (stock, handler {es}, FUN_4014F018 {S.lists - l0s})",
                    good)
        out.append(name)
    for r in (H, S):
        r.s.poke(MODE, bytes([mode0]))
    q0, l0 = len(H.n.file_q), H.lists
    th, eh = H.frames(PANEL, ctl(0x24, [0x11]))
    posts = len(H.n.file_q) - q0
    H.s.poke(LIST_BUSY, b"\x00")
    g1 = gate_diag(H.s, PANEL)
    ok &= check(f"4 idle, USB memory in: panel <- {[f.hex() for p, f in th if p == PANEL]} (stock), handler {eh}, FUN_4014F018 "
                f"{H.lists - l0}, file task posts {posts}; 1B 06 busy +{g1 and g0 and g1['file_busy'] - g0['file_busy']} err "
                f"+{g1 and g0 and g1['file_err'] - g0['file_err']} ack-only +{g1 and g0 and g1['file_ack'] - g0['file_ack']}",
                to(th, PANEL) == [FILE_ACK] and eh == 1 and H.lists - l0 == 1 and posts == 1 and g0 is not None
                and g1 is not None and g1["file_busy"] - g0["file_busy"] == 2 and g1["file_err"] - g0["file_err"] == 1
                and g1["file_ack"] - g0["file_ack"] == 1)
    rows.append(("4", "바인딩(패널)의 파일 요청 접수",
                 "USB 없음 · 목록 중 · 작업 대기 · 모드 5 → 코어가 스톡과 같은 바이트(S와 같음), 스톡 처리기 0번; 한가하면 스톡이 목록을 받음"))
    for r in (H, S):
        r.close()
    return ok


def no_gate(image, syms, m, rows):
    """O = ota_main with the intake gate taken out: kpg_rx (the gate's verdict, firmware/src/kproto/kpgate.c) answers
    KPG_PASS at once - every frame goes on to the stock handler as before the gate existed."""
    psyms = rtlib.payload(m.name).psyms
    mo = rss.Machines(image, syms, m.data)
    mo.no_gate_at = psyms["kpg_rx"] & ~1
    O = sides(mo)
    g0 = gate_diag(O.s, PANEL)
    t, e = O.frames(PANEL, ctl(0x08, [0x00]))   # H drops it (check 2); without the gate the stock handler gets it
    g1 = gate_diag(O.s, PANEL)
    ok = check(f"0 O: kpg_rx {mo.no_gate_at:#x} -> KPG_PASS ({O.passes} calls); fn 08 from the panel (binding) enters "
               f"FUN_4014A2C8 {e}x, nothing sent; 1B 06 dropped {g0 and g0['dropped']} -> {g1 and g1['dropped']}",
               O.passes > 0 and e == 1 and not t and g0 is not None and g1 is not None and g1["dropped"] == g0["dropped"])
    O.close()
    rows.append(("0", "O = 접수 판단 뺀 ota_main", f"kpg_rx → KPG_PASS, 스톡이 무시하는 fn 08도 스톡 처리기로"))
    return ok, mo


def counters(m, rows):
    H = sides(m)
    g = gate_diag(H.s)
    c3 = rf.diag(H.s, 0x03)
    c4 = rf.fan_diag(H.s)
    ok = check(f"5 1B 06 layout {g and g['layout']} flags {g and hex(g['flags'])} table {g and hex(g['bound'])} binding "
               f"{g and g['binding']} passed {g and g['passed']} (the identity / CONNECT frames of the two connects); 1B 03 "
               f"{c3 is not None} 1B 04 {c4 is not None}",
               g is not None and g["layout"] == 8 and g["flags"] == 0x05 and g["bound"] == 0x0C and g["binding"] == 4
               and g["passed"] > 0 and g["dropped"] == 0 and c3 is not None and c4 is not None)
    rows.append(("5", "op `1B 06` 기록", f"배치 8 · 켜짐 · 두 연결의 CONNECT · 식별 질의 {g and g['passed']}개를 스톡에 넘김, 버림 0"))
    H.close()
    return ok


def main(argv):
    name = rtlib.first_arg(argv, "ota_main")
    if not images.OTA_PAYLOADS_BY_NAME[name].feat3 & images.FEAT3_GATE:
        raise SystemExit(f"{name}: no intake gate (feat3 0x02)")
    _img, _stock, _manifest, image, syms = rtlib.loader()
    m = rss.Machines(image, syms, (STOCKFW / "build" / f"{name}.c79").read_bytes())
    m.name = name
    rows = []
    ok, mo = no_gate(image, syms, m, rows)
    ok &= counters(m, rows)
    ok &= info(m, mo, rows)
    ok &= refusals(m, mo, rows)
    ok &= file_intake(m, rows)
    print("\n| # | 시나리오 | 결과 |\n|---|---|---|")
    for r in rows:
        print(f"| {r[0]} | {r[1]} | {r[2]} |")
    return rtlib.finish(ok)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
