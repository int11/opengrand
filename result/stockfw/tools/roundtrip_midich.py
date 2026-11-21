# roundtrip_midich.py - Phase 4 (A plan): the MIDI channel messages that change settings and the delayed parameter apply,
# judged by the core at the ota_loader_v4 sites (ota_main feat3 0x10).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_midich.py [ota_main]
#
# Machines as roundtrip_fanout.py (virtual 2 ms clock, the stock boot settings, the app (port 2) and the panel (port 3) on
# the link models, the payload's pump tail every 2 ms), but on the real ota_loader_v4 image: S = its built-in cave (no
# runtime hook, the stock decides), H = ota_main applied live.  MIDI bytes go into the real stock parser FUN_40148B24(port,
# byte) - the hooked sites 0x40148C4C (Program Change) / 0x40148C60 (Control Change) are reached the way the pumps reach
# them.  Every stock-RAM byte (>= 0x41000000) a message writes is recorded on both sides.
# Checks (numbered as printed):
#   1  attach on v3: op 1B 07 = layout 9, built, three sites hooked (flags 0x70), ready, on from boot (flags 0x77 - always
#      on since fold step 3, 2026-11-21); the three words are no longer the stock's; the old off op 1B 07 00 -> rc 1, still
#      0x77.  (Before fold step 3 this check switched the judge off and compared a Program Change with S - the off path is
#      gone; 2 compares the on path with S.)  S3 / S4 / the executor gate are on all along, as on the piano.
#   2  on from boot; a matrix of channel messages on the USB port (unbound) and the panel port (the stock binding),
#      in the settings path, with omni on / off and in the part path (the mode byte): every message writes exactly the
#      stock bytes S writes (same addresses, same values); each judgement agrees with what S did - drop = S wrote nothing
#      beyond the parser's own bytes, setting = the read-back key; the in-flight flag is 0 after every message
#   3  same tick: a Program Change -> 1B 07 shows the read-back key 55 00 00 and the store changed in that very parser
#      call (no pump ran); the next S2 compare pass finds no difference of 55 00 (S2 counted it KEY = 1 when off -
#      roundtrip_fanout 16); Local Control CC 7A -> 58 01 7F the same way
#   4  with S3 on (from boot): the Program Change reaches the app and the panel as one value frame each right after the call (before
#      any compare pass), and the next pass fans nothing more
#   5  delayed apply: timers armed through the stock arm FUN_401604E8 and a pending value poked; H's hooked site (the core's
#      walk) and S's FUN_401638E0 step 2 ms at a time - the tables (9 x 5 countdowns, values, armed word) and the applies
#      FUN_4016053C(c, x, 0x46, v) are the same after every step
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402

rf = rtlib.load("roundtrip_fanout")
rss, rs, images = rf.rss, rf.rs, rf.images
check, le = rtlib.check, rtlib.le
APP, PANEL, USB = rf.APP, rf.PANEL, 0
CTX_SP = rss.CTX_SP
STOCKFW = rss.STOCKFW

PARSER = 0x40148B24                                      # FUN_40148B24(port, byte): the pumps' per-byte MIDI parser
SITES = {"pc": (0x40148C4C, 0xEBFFFA5E), "cc": (0x40148C60, 0xEBFFFD6C), "defer": (0x40153998, 0xEB003FD0)}
DEFER, ARM, APPLY = 0x401638E0, 0x401604E8, 0x4016053C
TM, VAL, ANY = 0x41033850, 0x41033B24, 0x41033C40       # FUN_401638E0's tables (stock_chan.ld)
MODE = 0x413B540C                                        # path byte of an unbound port (FUN_40146D04)
VALUE_HEAD = rf.VALUE_HEAD


def bl_target(e, site):
    w = le(e.read_bytes(site, 4))
    off = w & 0xFFFFFF
    return (site + 8 + ((off - (1 << 24) if off & 0x800000 else off) << 2)) & 0xFFFFFFFF


def chan_diag(side, port=APP):
    """op 0x1B 07: the Phase 4 record (layout 9)."""
    out = side.pn.sysex(port, rs.rp.req(rs.OP_FRONT, 0x07))
    b = rs.rp.body_of(out, port)
    if not b or b[0] != rs.OP_FRONT | 0x40 or b[1][:1] != b"\x07":
        return None
    d = rs.rp.unpack7(b[1][2:])
    return dict(layout=d[0], flags=d[1], last=bytes(d[2:5]).hex(), msg=bytes(d[5:8]).hex(), act=d[8], passes=d[9],
                bad=d[10], pc=[le(d, 12 + 4 * i) for i in range(3)], cc=[le(d, 24 + 4 * i) for i in range(3)],
                applies=le(d, 36), changed=le(d, 40), fanned=le(d, 44))


def chan_set(side, on, port=APP):
    out = side.pn.sysex(port, rs.rp.req(rs.OP_FRONT, 0x07, on))
    b = rs.rp.body_of(out, port)
    return b[1][1] if b and b[0] == rs.OP_FRONT | 0x40 and b[1][:1] == b"\x07" else None


def feed(n, port, msg):
    """the bytes through the real parser, as a pump does (task 19, no preemption): stock-RAM bytes written {addr: value}"""
    e, w = n.e, {}
    for b in msg:
        r = e.call(PARSER, port, b, regs={"sp": CTX_SP}, max_insns=50_000_000)
        if r.stop != "returned":
            raise RuntimeError(f"parser stopped at {bytes(msg).hex()}: {r}")
        for x in r.writes:
            if x.addr >= 0x41000000:
                for i in range(x.size):
                    w[x.addr + i] = (x.value >> (8 * i)) & 0xFF
    return w


def sides(image, syms, data):
    s = rf.NetF(rf._plain_side_settings(image, syms, None))
    rf.both_bound(s)                                       # the same history as H: app CONNECT + sync, then the panel (binding 4)
    s.run(50)
    h = rf.NetF(rf._plain_side_settings(image, syms, data), pump=True)
    rf.both_bound(h)
    return s, h


# (port, message, what it exercises) - USB = unbound, the receive channel 0 (MIDIChannel default), omni off at boot
MATRIX = [
    (USB, [0xC0, 0x05], "PC ch1"), (USB, [0xC0, 0x05], "PC same"), (USB, [0xC1, 0x07], "PC ch2 (omni at boot)"),
    (USB, [0xB0, 0x7A, 0x00], "Local off"), (USB, [0xB0, 0x7A, 0x7F], "Local on"), (USB, [0xB0, 0x7A, 0x40], "Local 40"),
    (USB, [0xB1, 0x7A, 0x00], "Local ch2"), (USB, [0xB0, 0x07, 0x64], "volume"), (USB, [0xB0, 0x0A, 0x30], "pan"),
    (USB, [0xB0, 0x0B, 0x7F], "expression"), (USB, [0xB0, 0x40, 0x7F], "damper on"), (USB, [0xB0, 0x40, 0x00], "damper off"),
    (USB, [0xB0, 0x42, 0x7F], "sostenuto"), (USB, [0xB0, 0x43, 0x7F], "soft"), (USB, [0xB0, 0x00, 0x79], "bank MSB"),
    (USB, [0xB0, 0x20, 0x01], "bank LSB"), (USB, [0xC0, 0x03], "PC after bank"), (USB, [0xB0, 0x5C, 0x7F], "CC 5C 7F"),
    (USB, [0xB0, 0x5C, 0x00], "CC 5C 0"), (USB, [0xB0, 0x50, 0x01], "CC 50"), (USB, [0xB0, 0x51, 0x01], "CC 51"),
    (USB, [0xB0, 0x52, 0x01], "CC 52"), (USB, [0xB0, 0x53, 0x01], "CC 53"), (USB, [0xB0, 0x01, 0x40], "mod (none)"),
    (USB, [0xB0, 0x65, 0x00], "RPN"), (USB, [0xB0, 0x06, 0x02], "data entry"), (USB, [0xB0, 0x79, 0x00], "reset ctl"),
    (USB, [0xB0, 0x78, 0x00], "sound off"), (USB, [0xB0, 0x7B, 0x00], "notes off"), (USB, [0xB0, 0x7E, 0x00], "mono"),
    (USB, [0xB0, 0x7F, 0x00], "poly"), (USB, [0xB0, 0x7D, 0x00], "omni on"), (USB, [0xC3, 0x09], "PC ch4 omni"),
    (USB, [0xB3, 0x07, 0x50], "volume ch4 omni"), (USB, [0xB3, 0x7A, 0x00], "Local ch4 omni"),
    (USB, [0xB0, 0x7C, 0x00], "omni off"), (USB, [0xC3, 0x0A], "PC ch4 again"),
    (PANEL, [0xC0, 0x0B], "binding PC ch1"), (PANEL, [0xC2, 0x0C], "binding PC ch3 (part)"),
    (PANEL, [0xB0, 0x7A, 0x7F], "binding Local"), (PANEL, [0xB2, 0x07, 0x40], "binding vol ch3 (part)"),
    (PANEL, [0xB2, 0x0A, 0x40], "binding pan ch3 (part)"),
    ("mode", 1, "mode byte 1"), (USB, [0xC0, 0x0D], "PC part path"), (USB, [0xB0, 0x07, 0x22], "vol part path"),
    (USB, [0xB0, 0x7A, 0x7F], "Local part path"), ("mode", None, "mode byte back"),
]


def target_of(msg):
    return 0x401475CC if msg[0] >> 4 == 0xC else 0x40148218


def matrix(s, h):
    """per message: S once with the channel routine stubbed (the parser's own bytes), S for real, then H"""
    rows, ok_all, acts = [], True, {}
    mode0 = s.e.read_bytes(MODE, 1)[0]
    for port, msg, what in MATRIX:
        if port == "mode":
            v = mode0 if msg is None else msg
            s.e.write_bytes(MODE, bytes([v]))
            h.e.write_bytes(MODE, bytes([v]))
            continue
        d0 = chan_diag(h.s)
        s.e.hook(target_of(msg), lambda em: 0)
        w0 = feed(s, port, msg)
        s.e.unhook(target_of(msg))
        ws, wh = feed(s, port, msg), feed(h, port, msg)
        d1 = chan_diag(h.s)
        kind = "pc" if msg[0] >> 4 == 0xC else "cc"
        act = next((i for i in range(3) if d1[kind][i] != d0[kind][i]), None)
        acts[what] = act
        flag = h.e.read_bytes(h.s.inflight_at, 1)[0]
        same = ws == wh
        routine = ws != w0                       # S's channel routine wrote stock RAM
        ok = same and act is not None and flag == 0 and (act != 0 or not routine)
        if not ok:
            diff = sorted(set(ws.items()) ^ set(wh.items()))[:8]
            print(f"    {what} {bytes(msg).hex()} port {port}: same {same} act {act} routine {routine} inflight {flag} "
                  f"diff {[(hex(a), v) for a, v in diff]}")
        ok_all &= ok
        rows.append((what, bytes(msg).hex(), port, act, len(set(ws) - set(w0)), d1["last"] if act == 2 else ""))
    return ok_all, rows, acts


def main(argv):
    name = rtlib.first_arg(argv, "ota_main")
    p = images.OTA_PAYLOADS_BY_NAME[name]
    if not p.feat3 & images.FEAT3_MIDICH:
        raise SystemExit(f"{name}: no MIDI channel port (feat3 0x10)")
    data = (STOCKFW / "build" / f"{name}.c79").read_bytes()
    _img, _stock, _m, image, syms = rtlib.loader()
    ok = True
    table = []

    if not p.boot_on:
        raise SystemExit(f"{name}: the judge is always on from fold step 3 (images boot_on)")
    # ---- 1 attach, on from boot ----
    s, h = sides(image, syms, data)
    d = chan_diag(h.s)
    words = {k: le(h.e.read_bytes(a, 4)) for k, (a, _w) in SITES.items()}
    rc_off = chan_set(h.s, 0)
    d0 = chan_diag(h.s)
    ok &= check(f"1 attach on ota_loader_v4: 1B 07 at boot {d} (on expected); site words "
                f"{{{', '.join(f'{k} {v:#010x}' for k, v in words.items())}}}; the old off op 1B 07 00 -> rc {rc_off}, flags "
                f"{d0 and hex(d0['flags'])}",
                d is not None and d["layout"] == 9 and d["flags"] == 0x77
                and all(words[k] != w for k, (_a, w) in SITES.items()) and rc_off == 1
                and d0 is not None and d0["flags"] == 0x77)
    table.append(("1", "v3 붙이기 · 켬", f"자리 셋 걸림 · 부팅 때 켬(flags {d and hex(d['flags'])}), 옛 끄기 op rc {rc_off}"))

    # ---- 2 matrix ----
    m_ok, rows, acts = matrix(s, h)
    d2 = chan_diag(h.s)
    ok &= check(f"2 on from boot; {len(rows)} channel messages: every one writes the stock's bytes, judged "
                f"PC {d2['pc']} CC {d2['cc']} (drop / stock / setting)", m_ok and (d2["flags"] & 0x02) != 0)
    for r in rows:
        print(f"    {r[0]:<24} {r[1]:<8} port {r[2]}  act {r[3]}  stock bytes {r[4]:>4}  {r[5]}")
    want = {"PC ch1": 2, "PC ch2 (omni at boot)": 2, "Local off": 2, "Local 40": 0, "Local ch2": 0, "volume": 1, "mod (none)": 0,
            "CC 52": 0, "PC ch4 omni": 2, "Local ch4 omni": 0, "PC ch4 again": 0, "binding PC ch1": 2,
            "binding PC ch3 (part)": 1, "binding vol ch3 (part)": 1, "PC part path": 1, "Local part path": 2}
    bad = {k: (acts.get(k), v) for k, v in want.items() if acts.get(k) != v}
    ok &= check(f"2b judgements as the stock branches say: {bad or 'all ' + str(len(want))}", not bad)
    table.append(("2", "채널 메시지 행렬", f"{len(rows)}개 — 스톡 RAM 바이트 = S, 판단 PC {d2['pc']} · CC {d2['cc']}"))
    s.s.close()

    # ---- 3 same tick: the store holds the new value before any pump ran ----
    rf.quiet(h)
    a = chan_diag(h.s)
    s0 = rf.s2_diag(h.s)
    feed(h, USB, [0xC0, 0x01])
    b = chan_diag(h.s)
    feed(h, USB, [0xB0, 0x7A, 0x00])
    c = chan_diag(h.s)
    s1 = rf.wait_passes(h, 1)
    dd = rf.delta(s0, s1)
    ok &= check(f"3 same tick: PC -> read back {b['last']} (store changed +{b['changed'] - a['changed']}) in the parser call; "
                f"Local off -> {c['last']} (+{c['changed'] - b['changed']}); the next S2 pass {dd}",
                b["last"] == "550000" and b["changed"] == a["changed"] + 1 and c["last"] == "58017f"
                and c["changed"] == b["changed"] + 1 and dd is not None and dd["key"] == 0 and dd["none"] == 0)
    table.append(("3", "같은 차례 store", f"PC → `55 00 00`, CC 7A → `58 01 7F` 파서 호출 안에서 store 고침, 다음 대조 차이 {dd}"))

    # ---- 4 S3 on (from boot): fanned at once ----
    # in the emulator FUN_4013B644 maps every program to index 0 (its model getters FUN_400B39FC / FUN_400B3A74 answer
    # nothing here), so every Program Change gives 55 00 00 = 79 00 00: the panel sets another sound first
    rf.set_and_wait(h, PANEL, [0x55, 0x00, 0x00, 0x00, 0x05, 0x00])
    rf.until_trusted(h)
    rf.quiet(h)
    n0 = len(h.tx)
    k0 = rf.s3_diag(h.s)
    c = chan_diag(h.s)
    feed(h, USB, [0xC0, 0x04])
    e = chan_diag(h.s)
    h.run(200)
    pc = [(pp, f.hex()) for _t, pp, f in h.tx[n0:] if f.startswith(VALUE_HEAD) and f[6:9] == b"\x55\x00\x00"]
    s2 = rf.wait_passes(h, 1)
    k1 = rf.s3_diag(h.s)
    pc_all = [(pp, f.hex()) for _t, pp, f in h.tx[n0:] if f.startswith(VALUE_HEAD) and f[6:9] == b"\x55\x00\x00"]
    ok &= check(f"4 S3 on (flags {k0 and hex(k0['flags'])}): PC -> store changed +{e['changed'] - c['changed']}, "
                f"fanned {e['fanned'] - c['fanned']}, frames within 200 ms {pc}; after the next pass {len(pc_all)} "
                f"(fanned_key +{k1['fanned_key'] - k0['fanned_key']})",
                k0 is not None and k0["flags"] & 0x02 != 0 and e["changed"] == c["changed"] + 1 and e["fanned"] == c["fanned"] + 1 and sorted(pp for pp, _f in pc) == [APP, PANEL] and len(pc_all) == 2
                and pc[0][1] == pc[1][1] and k1["fanned_key"] == k0["fanned_key"] and s2 is not None)
    table.append(("4", "S3 켬 팬아웃", "PC 직후 앱 · 패널에 `55 00 00` 1프레임씩(대조 바퀴 전), 다음 바퀴 추가 없음"))

    # ---- 5 delayed apply ----
    h.s.close()
    s, h = sides(image, syms, data)                        # a fresh pair: the matrix armed timers on H only
    f5_0 = chan_diag(h.s)
    step_ok, steps = True, 0
    for side in (s, h):
        side.s.log = []                                    # rs.Side records every FUN_4016053C call ("apply")
        for c_, x_ in ((0, 0), (1, 3), (4, 2), (8, 4)):
            side.e.call(ARM, c_, x_, regs={"sp": CTX_SP})
        side.e.write_bytes(TM + 4 * (1 * 5 + 3), (150).to_bytes(4, "little"))
        side.e.write_bytes(VAL + 1 * 5 + 3, b"\x10")      # a pending value (no stock writer < 0x80 is known: §1.3)
        side.e.write_bytes(VAL + 4 * 5 + 2, b"\x7F")
    tgt = bl_target(h.e, SITES["defer"][0])
    for k in range(220):
        el = 2 if k % 7 else 3
        for side, fn in ((s, DEFER), (h, tgt)):
            r = side.e.call(fn, el, regs={"sp": CTX_SP}, max_insns=20_000_000)
            step_ok &= r.stop == "returned"
        tab = [bytes(side.e.read_bytes(TM, 180)) + bytes(side.e.read_bytes(VAL, 45)) + bytes(side.e.read_bytes(ANY, 4))
               for side in (s, h)]
        if tab[0] != tab[1]:
            step_ok = False
            print(f"    step {k}: tables differ")
            break
        steps += 1
    applies = {k: [(a[0], a[1], a[2], a[3][0]) for kind, a, _f in side.s.log if kind == "apply"]
               for k, side in (("S", s), ("H", h))}
    top = {k: [x for x in v if x[2] == 0x46] for k, v in applies.items()}
    f5 = chan_diag(h.s)
    ok &= check(f"5 delayed apply: {steps} steps, tables equal every step {step_ok}; applies (0x46) S {top['S']} H {top['H']}, "
                f"every apply call equal {applies['S'] == applies['H']}; record applies {f5['applies']} passes {f5['passes']}; "
                f"H's site -> {tgt:#x}",
                f5_0 is not None and f5_0["flags"] & 0x02 != 0 and step_ok and steps == 220 and applies["S"] == applies["H"] and len(top["S"]) == 2
                and f5["applies"] == 2 and le(h.e.read_bytes(ANY, 4)) == 0)
    table.append(("5", "지연 적용 타이머", f"{steps} 차례 표 = 스톡, 적용 {top['H']}"))
    s.s.close()
    h.s.close()

    print("\n| # | 시나리오 | 결과 |\n|---|---|---|")
    for r in table:
        print(f"| {r[0]} | {r[1]} | {r[2]} |")
    return rtlib.finish(ok)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
