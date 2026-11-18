# roundtrip_xgate.py - Phase 4 executor gate of ota_main (feat3 0x08):
# the core decides WHEN the stock media executor FUN_40127594 (ota_loader_v4 site 13, 0x40143AC4) and the audio-file
# transport executor FUN_40123E34 (site 16, 0x40143AD8) run; the stock executors do the work.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_xgate.py [ota_main] [-q]
#
# Same machine as roundtrip_power.py (its Rig: the real ota_loader_v4 image on StockEmu with the NOR model, SWI #0
# emulated so the resident's real poke body writes the hook words, the payload uploaded over the 0x7D OTA ops and
# applied live).  The engine-loop sites are run from their stock `bl` (Rig.site) - the stock executors run for real.
# Every rule check runs a gated machine G and a stock machine S (gate never switched on) from the same stock RAM.
# Checks (numbered as printed):
#   1  static: Identify 41 01 02 13 3B 77 7F 28 (feat3 0x08 + S4 0x20); hk_xg_media / hk_xg_xport = push / bl body / pop /
#      bxeq lr / `b` the stock target of their site (bl_target of 0xEBFF8EB2 / 0xEBFF80D5); the pass bodies call only
#      the core module (no stock routine, no svc)
#   2  boot state: applied, 20 engine passes.  A boot-on payload (images boot_on - ota_main since 2026-11-16): the first pass after attach() hooked both sites (bl -> stub, op 0x16
#      seven hooked), 1F 00 flags 0x3F, rc 0 / 0; then 1F 01 00 from the unbound panel -> the next pass unhooks both (stock
#      words, five hooked, flags 0x21).  Default-off payload: both site words stock, op 0x16 five hooked (+ the three MIDI
#      channel sites and the fn 0x24 Read site 0x40143ACC, hooked at attach), 1F 00 = built + service, rc 0xFF.
#      From here on every machine starts with the gate off (1F 01 00 first when it booted on)
#   3  1F 01 01 from an unbound port -> rc 2, nothing; from the bound app -> rc 0, the next pass hooks both sites
#      (bl -> stub, op 0x16 seven hooked), 1F 00 flags on + hooked + wanted, rc 0 / 0
#   4  media rule: a USB removal (0x41359E45 = 0x80) while the selected type is KSO (0x413CA3F5 = 3) and the recorder
#      records (0x4191EF79 = 3 / 4 / 5 / 6): S's stock executor forces the state to 2 at once (the take is gone);
#      G holds it 50 passes - pending byte, state unchanged, no stock store, no frame; the state goes to 2 (the take
#      ends) -> G's next pass runs the stock executor: the same stores and frames as S running from that state
#   5  media pass-through: removal with type 5 (MP3) / KSO not recording, mount (0x81): G's pass = S's pass (stock
#      stores, frames), counted as run
#   6  transport rule: a pending stop (0x41058ACE = 0x80) while a file operation runs (0x413CA3EA = 4): S drops it
#      (byte cleared, Busy F0 40 7F 7E 04 1E 03 01 00 00 F7 to the binding); G holds it (byte kept, no frame) until the
#      file operation ends, then its stock stop = S's stop from that state; a held stop that never gets its turn runs
#      at the cap (625 passes) - the stock's Busy; play (0x81) during a file operation: G = S (Busy at once)
#   7  stores: every store made by payload code in checks 3-6 lands in payload .data / core RW / the stack - the gate
#      never writes stock RAM
#   8  1F 01 00 from any port -> the next pass unhooks both (stock words), the media case of 4 behaves as S
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rpw = rtlib.load("roundtrip_power")
rh, rp, ro, images = rpw.rh, rpw.rp, rpw.ro, rpw.images
check, le = rtlib.check, rtlib.le
RUN = ro.RUN

OP = 0x1F
SITE_MEDIA, WORD_MEDIA, EXEC_MEDIA = 0x40143AC4, 0xEBFF8EB2, 0x40127594
SITE_XPORT, WORD_XPORT, EXEC_XPORT = 0x40143AD8, 0xEBFF80D5, 0x40123E34
MEDIA, SEL_TYPE, REC_STATE = 0x41359E45, 0x413CA3F5, 0x4191EF79
XCMD, FILEOP = 0x41058ACE, 0x413CA3EA
BINDING = rpw.BINDING
APP, PANEL = 2, 0
CAP = 625
BUSY = bytes.fromhex("F0407F7E041E03010000F7")
ACK_WAIT_10 = 0x4014BCAC   # FUN_4014BCAC: the stock stop-and-wait of the reliable sender (0 = ACK)
STOCK_RAM = (0x41000000, 0x41D00000)
BOOT_ON = False   # main(): the payload's images boot_on


def status(rig, port=APP, body=None):
    """op 0x1F -> (sub, rc, raw record dict) or None."""
    r = rp.body_of(rig.ask(port, body or rp.req(OP)), port)
    if not r or r[0] != OP | 0x40:
        return None
    b = r[1]
    d = rp.unpack7(b[2:])
    return b[0], b[1], dict(layout=d[0], flags=d[1], rc_m=d[2], rc_x=d[3], last_m=d[4], last_x=d[5], why_m=d[6],
                            why_x=d[7], held=d[8], audio=d[9], wait=le(d, 10, 2), m_run=le(d, 12), m_def=le(d, 16),
                            x_run=le(d, 20), x_def=le(d, 24), m_rel=le(d, 28, 2), x_rel=le(d, 30, 2), x_cap=le(d, 32, 2),
                            n=len(d))


def rig_of(image, syms):
    """roundtrip_power's Rig; the controller ACKs every reliable notice at once (the stock stop-and-wait
    FUN_4014BCAC -> 0, as p3_map does) so a stock executor that sends the 0x86 notice runs to its end."""
    rig = rpw.Rig(image, syms)
    rig.emu.hook(ACK_WAIT_10, lambda em: 0)
    rig.emu.hook(ro.DLY, lambda em: None)   # dly_tsk (FUN_40089658, an SWI): no scheduler here - returns at once
    return rig


def machine(image, syms, data, gate):
    """A Rig with ota_main applied live, the app (port 2) bound; gate=True: 1F 01 01 and one pass; gate=False: the gate
    off - 1F 01 00 when the payload boots with it on (S = the stock path), else a plain 1F 00 (same passes / requests)."""
    rig = rig_of(image, syms)
    rc, _st = rpw.upload_apply(rig, data)
    assert rc == 0
    rig.emu.write_bytes(BINDING, bytes([APP + 1]))
    rig.tick(2)
    if gate:
        rig.ask(APP, rp.req(OP, 0x01, 0x01))
    elif BOOT_ON:
        rig.ask(APP, rp.req(OP, 0x01, 0x00))
    else:
        rig.ask(APP, rp.req(OP))   # the same number of passes / requests
    rig.tick(1)
    return rig


def put(rig, **kv):
    addr = dict(media=MEDIA, sel=SEL_TYPE, rec=REC_STATE, xcmd=XCMD, fileop=FILEOP)
    for k, v in kv.items():
        rig.emu.write_bytes(addr[k], bytes([v]))


def get(rig, *ks):
    addr = dict(media=MEDIA, sel=SEL_TYPE, rec=REC_STATE, xcmd=XCMD, fileop=FILEOP)
    return tuple(rig.emu.read_bytes(addr[k], 1)[0] for k in ks)


def site_pass(rig, site):
    """One engine-loop call through `site`: (stock-RAM stores by stock code, frames sent, payload stores)."""
    rig.pn.sent.clear()
    res = rig.site(site)
    if res.stop != "until":   # the executor (or the stub) did not come back to site + 4
        raise AssertionError(f"site {site:#x}: {res}")
    stock =[(w.addr, w.size, w.value) for w in res.writes
             if STOCK_RAM[0] <= w.addr < STOCK_RAM[1] and not RUN <= w.pc < RUN + 0x20000]
    mine = [w for w in res.writes if RUN <= w.pc < RUN + 0x20000]
    frames = list(rig.pn.sent)
    rig.pn.sent.clear()
    return stock, frames, mine


def main(argv):
    name = rtlib.first_arg(argv, "ota_main")
    global BOOT_ON
    p, data, pay, plen, pelf, psyms = rtlib.payload(name)
    BOOT_ON = p.boot_on
    if not p.feat3 & images.FEAT3_XGATE:
        raise SystemExit(f"{name}: no executor gate (feat3 0x08)")
    # the MIDI channel port (feat3 0x10) hooks its three v3 sites at attach - counted on top
    mc = 3 if p.feat3 & images.FEAT3_MIDICH else 0
    mc += 1 if "hk_fr_file" in psyms else 0   # + the fn 0x24 Read site 0x40143ACC at attach
    _i3, _s3, _m3, image3, syms3 = rtlib.loader()
    ok = True
    print(f"payload {name}  {plen} B  sha256 {ro.pa.sha256(data)}  on ota_loader_v4")
    okw = [(RUN, RUN + 0x20000), (0x40180000, 0x401A0000), ro.STACK]
    allw = []

    # ---- 1. static ------------------------------------------------------------------------------------
    ins = rtlib.instructions(pelf)
    nmS = rtlib.load("make_phase2_manifest").NM
    import subprocess
    out = subprocess.run([str(nmS), "-S", str(pelf)], capture_output=True, text=True, check=True).stdout
    size = {q[3]: (int(q[0], 16), int(q[0], 16) + int(q[1], 16)) for q in (l.split() for l in out.splitlines()) if len(q) == 4}

    def body(n):
        lo, hi = size[n]
        return [(a, m, o) for a, m, o in ins if lo <= a < hi]

    stubs = []
    for stub, pass_, site, word, ex in (("hk_xg_media", "xgc_media_pass", SITE_MEDIA, WORD_MEDIA, EXEC_MEDIA),
                                        ("hk_xg_xport", "xgc_xport_pass", SITE_XPORT, WORD_XPORT, EXEC_XPORT)):
        b = [(m, o) for _a, m, o in body(stub)]
        stubs.append(len(b) == 6 and b[0][0] == "push" and b[1][0] in ("bl", "blx") and int(b[1][1].split()[0], 16) == psyms[pass_]
                     and b[4][0] == "bxeq" and b[5][0] == "b" and int(b[5][1].split()[0], 16) == ex == rh.bl_target(site, word))
    calls = {}
    for n in ("xgc_media_pass", "xgc_xport_pass", "xg_media", "xg_xport"):
        calls[n] = sorted({int(o.split()[0], 16) for _a, m, o in body(n) if m.startswith("bl") or m in ("svc", "blx")})
    leaf = (calls["xg_media"] == [] and calls["xg_xport"] == [] and calls["xgc_media_pass"] in ([], [psyms["xg_media"]])
            and calls["xgc_xport_pass"] in ([], [psyms["xg_xport"]]))
    rig = rig_of(image3, syms3)   # check 2 reads the boot state: no switching here
    assert rpw.upload_apply(rig, data)[0] == 0
    rig.emu.write_bytes(BINDING, bytes([APP + 1]))
    rig.tick(2)
    rig.ask(APP, rp.req(OP))
    rig.tick(1)
    id_ = rh.identify(rig.pn)
    want_id = bytes([p.ngver, 2, 19, p.feat0, p.feat1, p.feat2, p.feat3])
    ok &= check(f"1 identify {want_id.hex(' ').upper()} (feat3 0x08 = executor gate); hk_xg_media / hk_xg_xport: push, bl pass, "
                "pop, bxeq lr, `b` FUN_40127594 / FUN_40123E34 (= the bl target of the site words); the pass bodies call only "
                "the core module", id_ == want_id and all(stubs) and leaf, f"{id_ and id_.hex()} {stubs} {calls}")

    # ---- 2. boot state ------------------------------------------------------------------------------
    rig.tick(20)
    hk = rh.hooks(rig.pn)
    st = status(rig)
    words = (rig.emu.read_u32(SITE_MEDIA), rig.emu.read_u32(SITE_XPORT))
    want_w = (rh.bl_word(SITE_MEDIA, WORD_MEDIA, psyms["hk_xg_media"]), rh.bl_word(SITE_XPORT, WORD_XPORT, psyms["hk_xg_xport"]))
    if BOOT_ON:
        r_off = status(rig, PANEL, rp.req(OP, 0x01, 0x00))
        rig.tick(1)
        w_off = (rig.emu.read_u32(SITE_MEDIA), rig.emu.read_u32(SITE_XPORT))
        hk_off = rh.hooks(rig.pn)
        st_off = status(rig)
        ok &= check("2 boot on: 20 passes after the apply both sites hooked (bl -> hk_xg_media / hk_xg_xport), op 0x16 seven "
                    "hooked; 1F 00 -> 5F 00 00, flags 0x3F (built, on, both hooked, wanted, service), rc 0 / 0, 34 B; "
                    "1F 01 00 from the unbound panel -> 5F 01 00, the next pass unhooks both (stock words, five hooked), flags 0x21",
                    words == want_w and hk["hooked"] == 7 + mc and st is not None and st[:2] == (0, 0)
                    and st[2]["flags"] == 0x3F and (st[2]["rc_m"], st[2]["rc_x"]) == (0, 0) and st[2]["n"] == 34
                    and r_off is not None and r_off[:2] == (1, 0) and w_off == (WORD_MEDIA, WORD_XPORT)
                    and hk_off["hooked"] == 5 + mc and st_off is not None and st_off[2]["flags"] == 0x21,
                    f"{[hex(w) for w in words]} {hk['hooked']} {st and (hex(st[2]['flags']), st[2]['rc_m'], st[2]['rc_x'])} "
                    f"off {r_off and r_off[:2]} {[hex(w) for w in w_off]} {hk_off['hooked']} {st_off and hex(st_off[2]['flags'])}")
    else:
        ok &= check("2 default off: 20 passes after the apply both site words stock, op 0x16 five hooked; 1F 00 -> 5F 00 00, "
                    "flags built + service (0x21), rc 0xFF / 0xFF, 34 B",
                    words == (WORD_MEDIA, WORD_XPORT) and hk["hooked"] == 5 + mc and st is not None and st[:2] == (0, 0)
                    and st[2]["flags"] == 0x21 and (st[2]["rc_m"], st[2]["rc_x"]) == (0xFF, 0xFF) and st[2]["n"] == 34,
                    f"{[hex(w) for w in words]} {hk['hooked']} {st}")
    rig.close()

    # ---- 3. switch on -------------------------------------------------------------------------------
    g = rig_of(image3, syms3)
    rpw.upload_apply(g, data)
    g.emu.write_bytes(BINDING, bytes([APP + 1]))
    g.tick(2)
    if BOOT_ON:   # start from off, as a default-off payload does
        g.ask(APP, rp.req(OP, 0x01, 0x00))
        g.tick(1)
    r_unb = status(g, PANEL, rp.req(OP, 0x01, 0x01))
    g.tick(1)
    w_unb = (g.emu.read_u32(SITE_MEDIA), g.emu.read_u32(SITE_XPORT))
    r_on = status(g, APP, rp.req(OP, 0x01, 0x01))
    g.pn.writes.clear()
    g.tick(1)
    allw += [w for w in g.pn.writes if RUN <= w.pc < RUN + 0x20000]
    w_on = (g.emu.read_u32(SITE_MEDIA), g.emu.read_u32(SITE_XPORT))
    hk = rh.hooks(g.pn)
    st = status(g)
    ok &= check("3 1F 01 01 from the unbound panel port -> 5F 01 02, nothing hooked; from the bound app -> 5F 01 00 (flags "
                "wanted, not yet on), the next pass hooks both sites (bl -> hk_xg_media / hk_xg_xport), op 0x16 seven hooked, "
                "1F 00 flags 0x3F (built, on, both hooked, wanted, service), rc 0 / 0",
                r_unb is not None and r_unb[:2] == (1, 2) and w_unb == (WORD_MEDIA, WORD_XPORT)
                and r_on is not None and r_on[:2] == (1, 0) and r_on[2]["flags"] == 0x31 and w_on == want_w
                and hk["hooked"] == 7 + mc and st is not None and st[2]["flags"] == 0x3F and (st[2]["rc_m"], st[2]["rc_x"]) == (0, 0),
                f"{r_unb and r_unb[:2]} {r_on and (r_on[:2], hex(r_on[2]['flags']))} {[hex(w) for w in w_on]} "
                f"{hk['hooked']} {st and hex(st[2]['flags'])}")
    s = machine(image3, syms3, data, gate=False)

    # ---- 4. media rule --------------------------------------------------------------------------------
    rows4 = []
    for rec in (3, 4, 5, 6):
        for m in (g, s):
            put(m, sel=3, rec=rec, media=0x80)
        s_st, s_fr, _ = site_pass(s, SITE_MEDIA)
        s_after = get(s, "media", "rec")
        held = []
        for _ in range(50):
            st_, fr_, mine_ = site_pass(g, SITE_MEDIA)
            allw.extend(mine_)
            held.append((st_, fr_))
        g_after = get(g, "media", "rec")
        # the take ends: both from state 2 (S re-armed with the same pending byte)
        put(g, rec=2)
        put(s, rec=2, media=0x80)
        g_st, g_fr, mine_ = site_pass(g, SITE_MEDIA)
        allw.extend(mine_)
        s2_st, s2_fr, _ = site_pass(s, SITE_MEDIA)
        rows4.append(dict(rec=rec, s_after=s_after, s_frames=len(s_fr), g_after=g_after,
                          held_quiet=all(not a and not b for a, b in held), same=(g_st == s2_st and g_fr == s2_fr),
                          rel=get(g, "media", "rec"), n=len(g_st), nf=len(g_fr)))
    st = status(g)
    ok &= check("4 media rule: removal (0x80) with type KSO and the recorder in 3 / 4 / 5 / 6 - S forces state 2 at once "
                "(pending cleared); G holds it 50 passes (byte 0x80, state kept, no stock store, no frame); after the take "
                "ends (state 2) G's pass = S's pass from that state (same stock stores, same frames); 1F 00 counts "
                "200 held, 4 released",
                all(r["s_after"] == (0x00, 2) and r["g_after"] == (0x80, r["rec"]) and r["held_quiet"] and r["same"]
                    and r["rel"] == (0x00, 2) for r in rows4)
                and st is not None and st[2]["m_def"] == 200 and st[2]["m_rel"] == 4 and st[2]["why_m"] == 0,
                str([(r["rec"], r["s_after"], r["g_after"], r["held_quiet"], r["same"], r["n"], r["nf"]) for r in rows4])
                + f" {st and (st[2]['m_def'], st[2]['m_rel'])}")

    # ---- 5. media pass-through ------------------------------------------------------------------------
    rows5 = []
    m_run0 = st[2]["m_run"] if st else 0
    for sel, rec, media in ((5, 4, 0x80), (6, 0, 0x80), (4, 1, 0x80), (3, 1, 0x80), (3, 2, 0x80), (3, 4, 0x81)):
        for m in (g, s):
            put(m, sel=sel, rec=rec, media=media)
        g_st, g_fr, mine_ = site_pass(g, SITE_MEDIA)
        allw.extend(mine_)
        s_st, s_fr, _ = site_pass(s, SITE_MEDIA)
        rows5.append((sel, rec, hex(media), g_st == s_st and g_fr == s_fr, get(g, "media", "rec") == get(s, "media", "rec"),
                      len(g_st), len(g_fr)))
    st = status(g)
    ok &= check("5 media pass-through: removal with type MP3 / WAV / MID or KSO not recording (1, 2), mount 0x81 during a "
                "take - G's pass = S's pass (stock stores, frames, the bytes after); 6 counted as run",
                all(r[3] and r[4] for r in rows5) and st is not None and st[2]["m_run"] - m_run0 == 6, str(rows5))

    # ---- 6. transport rule ----------------------------------------------------------------------------
    for m in (g, s):
        put(m, xcmd=0x80, fileop=4)
    s_st, s_fr, _ = site_pass(s, SITE_XPORT)
    s_after = get(s, "xcmd")
    held = []
    for _ in range(20):
        st_, fr_, mine_ = site_pass(g, SITE_XPORT)
        allw.extend(mine_)
        held.append(not st_ and not fr_)
    g_after = get(g, "xcmd")
    put(g, fileop=0)
    put(s, fileop=0, xcmd=0x80)
    g_st, g_fr, mine_ = site_pass(g, SITE_XPORT)
    allw.extend(mine_)
    s2_st, s2_fr, _ = site_pass(s, SITE_XPORT)
    st = status(g)
    ok &= check("6 transport rule: a pending stop (0x80) during a file operation - S clears it and sends Busy "
                "F0 40 7F 7E 04 1E 03 01 00 00 F7 to the binding; G holds it 20 passes (byte kept, nothing sent, no stock "
                "store); the file operation ends -> G's stop = S's stop from that state; 1F 00 20 held, 1 released",
                s_after == (0,) and [f for _p, f in s_fr] == [BUSY] and s_fr[0][0] == APP and g_after == (0x80,) and all(held)
                and g_st == s2_st and g_fr == s2_fr and get(g, "xcmd") == (0,)
                and st is not None and st[2]["x_def"] == 20 and st[2]["x_rel"] == 1,
                f"S {s_after} {[(q, f.hex()) for q, f in s_fr]} G {g_after} {all(held)} same {g_st == s2_st and g_fr == s2_fr} "
                f"({len(g_st)} stores, {len(g_fr)} frames) {st and (st[2]['x_def'], st[2]['x_rel'])}")
    # cap
    put(g, xcmd=0x80, fileop=4)
    quiet = True
    for _ in range(CAP):
        st_, fr_, mine_ = site_pass(g, SITE_XPORT)
        quiet &= not st_ and not fr_
    g_after = get(g, "xcmd")
    c_st, c_fr, _ = site_pass(g, SITE_XPORT)
    st = status(g)
    # play during a file operation: no rule
    for m in (g, s):
        put(m, xcmd=0x81, fileop=4)
    p_st, p_fr, _ = site_pass(g, SITE_XPORT)
    q_st, q_fr, _ = site_pass(s, SITE_XPORT)
    ok &= check(f"6 cap: a stop held {CAP} passes (byte kept, nothing sent) runs at pass {CAP + 1} - the stock's Busy, byte "
                "cleared, 1F 00 capped 1; play (0x81) during a file operation: G = S (Busy at once)",
                quiet and g_after == (0x80,) and [f for _p, f in c_fr] == [BUSY] and get(g, "xcmd") == (0,)
                and st is not None and st[2]["x_cap"] == 1 and p_st == q_st and p_fr == q_fr and [f for _p, f in p_fr] == [BUSY],
                f"{quiet} {g_after} {[f.hex() for _p, f in c_fr]} {st and st[2]['x_cap']} {[f.hex() for _p, f in p_fr]}")
    put(g, fileop=0)
    put(s, fileop=0)

    # ---- 7. stores ------------------------------------------------------------------------------------
    stray = [w for w in allw if not any(a <= w.addr < b for a, b in okw)]
    ok &= check(f"7 {len(allw)} stores by payload code in checks 3-6: only payload .data, core RW and the stack - no stock RAM",
                allw and not stray, str([(hex(w.addr), hex(w.pc)) for w in stray[:4]]))

    # ---- 8. switch off --------------------------------------------------------------------------------
    r_off = status(g, PANEL, rp.req(OP, 0x01, 0x00))
    g.tick(1)
    w_off = (g.emu.read_u32(SITE_MEDIA), g.emu.read_u32(SITE_XPORT))
    hk = rh.hooks(g.pn)
    put(g, sel=3, rec=4, media=0x80)
    put(s, sel=3, rec=4, media=0x80)
    g_st, g_fr, _ = site_pass(g, SITE_MEDIA)
    s_st, s_fr, _ = site_pass(s, SITE_MEDIA)
    st = status(g)
    ok &= check("8 1F 01 00 from the (unbound) panel port -> 5F 01 00; the next pass unhooks both (stock words, op 0x16 five "
                "hooked), flags 0x21; the media case of 4 now = S (state 2 at once)",
                r_off is not None and r_off[:2] == (1, 0) and w_off == (WORD_MEDIA, WORD_XPORT) and hk["hooked"] == 5 + mc
                and g_st == s_st and g_fr == s_fr and get(g, "media", "rec") == (0, 2)
                and st is not None and st[2]["flags"] == 0x21,
                f"{r_off and r_off[:2]} {[hex(w) for w in w_off]} {hk['hooked']} {get(g, 'media', 'rec')} {st and hex(st[2]['flags'])}")
    g.close()
    s.close()

    return rtlib.finish(ok)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
