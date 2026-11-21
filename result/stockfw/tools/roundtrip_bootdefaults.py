# roundtrip_bootdefaults.py - the four former runtime switches of ota_main start ON (owner decision 2026-11-16) and, from
# fold step 3 (2026-11-21, ngver 3), stay on: S3 read owner, S4 write owner, the Phase 4 MIDI channel judge and the
# executor gate.  Their old off / on ops (1B 05 04 <x>, 1B 05 06 <x>, 1B 07 <x>, 1F 01 <x>) answer rc 1 and change
# nothing; the records (1B 05 05, 1B 05 07, 1B 07, 1F 00) stay.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_bootdefaults.py [ota_main] [-q]
#
# Machine: roundtrip_power's Rig (the real loader image on StockEmu with the NOR model; the payload uploaded over the 0x7D
# OTA ops, committed and applied live).  "Power-on" = a new Rig on the NOR the upload left (slot A active): the loader
# boots, attach() hooks, the payload's .data is fresh - the field's power cycle.
# Checks (numbered as printed):
#   1  ota_loader_v4, live apply: after 3 engine passes 1B 05 05 / 1B 05 07 / 1B 07 / 1F 00 say on (S3 flags bit 1, S4 bit
#      1, MIDI channel bit 1 with its three sites attached 0x70, gate 0x3F = built, on, both hooked, wanted, service), the
#      two gate site words are the stubs' bl
#   2  ota_loader_v4, power-on on that NOR (slot A boots): the same four on; Identify unchanged
#   3  the old off ops (1B 05 04 00, 1B 05 06 00, 1B 07 00, 1F 01 00 + one pass): rc 1 each, all four still on, the gate
#      site words still the stubs' bl; the old on ops (01 each): rc 1, all on
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rx = rtlib.load("roundtrip_xgate")
rpw, rh, rp, ro, images = rx.rpw, rx.rh, rx.rp, rx.ro, rx.images
check = rtlib.check
APP, PANEL, BINDING = rx.APP, rx.PANEL, rx.BINDING
SITE_MEDIA, WORD_MEDIA, SITE_XPORT, WORD_XPORT = rx.SITE_MEDIA, rx.WORD_MEDIA, rx.SITE_XPORT, rx.WORD_XPORT
OP_FRONT, OP_GATE = 0x1B, 0x1F


def front(rig, *req):
    """op 1B <req>: (rc, unpacked record) or None."""
    r = rp.body_of(rig.ask(APP, rp.req(OP_FRONT, *req)), APP)
    if not r or r[0] != OP_FRONT | 0x40 or r[1][:1] != bytes([req[0]]):
        return None
    return r[1][1], rp.unpack7(r[1][2:])


def state(rig):
    """the four switches as their records say: dict s3 / s4 / mc / gate (bools), mc_sites (bits 4-6), gate flags, rcs."""
    s3, s4, mc = front(rig, 0x05, 0x05), front(rig, 0x05, 0x07), front(rig, 0x07)
    g = rx.status(rig)
    return dict(s3=bool(s3 and s3[1][1] & 0x02), s4=bool(s4 and s4[1][1] & 0x02), mc=bool(mc and mc[1][1] & 0x02),
                mc_sites=mc[1][1] & 0x70 if mc else None, gate=bool(g and g[2]["flags"] & 0x02),
                gflags=g and g[2]["flags"], grc=g and (g[2]["rc_m"], g[2]["rc_x"]),
                words=(rig.emu.read_u32(SITE_MEDIA), rig.emu.read_u32(SITE_XPORT)))


def setall(rig, on):
    """the old off / on ops of all four (before fold step 3 the gate applied on the next pass): rc of each."""
    rcs = [front(rig, 0x05, 0x04, on), front(rig, 0x05, 0x06, on), front(rig, 0x07, on)]
    rcs = [r and r[0] for r in rcs]
    g = rx.status(rig, APP, rp.req(OP_GATE, 0x01, on))
    rcs.append(g and g[1])
    rig.tick(1)
    return rcs


def power_on(image, syms, data):
    """upload + apply on one rig, then a new rig on that NOR: the loader boots slot A.  (live rig, booted rig, boot status)"""
    live = rx.rig_of(image, syms)
    rc, _st = rpw.upload_apply(live, data)
    assert rc == 0
    live.emu.write_bytes(BINDING, bytes([APP + 1]))
    live.tick(3)
    nor = bytes(live.pn.nor.mem)
    rig = rpw.Rig(image, syms, nor_mem=nor)
    rig.emu.hook(rx.ACK_WAIT_10, lambda em: 0)
    rig.emu.hook(ro.DLY, lambda em: None)
    st, _ = rig.pn.boot()
    rig.emu.write_bytes(BINDING, bytes([APP + 1]))
    rig.tick(3)
    return live, rig, st


def main(argv):
    name = rtlib.first_arg(argv, "ota_main")
    p, data, _pay, plen, _pelf, psyms = rtlib.payload(name)
    if not p.boot_on:
        raise SystemExit(f"{name}: not a boot-on payload (images boot_on)")
    _i3, _s3, _m3, image3, syms3 = rtlib.loader()
    ok = True
    print(f"payload {name}  {plen} B  sha256 {ro.pa.sha256(data)}")
    hooked = (rh.bl_word(SITE_MEDIA, WORD_MEDIA, psyms["hk_xg_media"]), rh.bl_word(SITE_XPORT, WORD_XPORT, psyms["hk_xg_xport"]))
    stock = (WORD_MEDIA, WORD_XPORT)   # show() names them

    def all_on(s):
        return (s["s3"] and s["s4"] and s["gate"] and s["mc"] and s["mc_sites"] == 0x70
                and s["gflags"] == 0x3F and s["grc"] == (0, 0) and s["words"] == hooked)

    def show(s):
        return (f"S3 {int(s['s3'])} S4 {int(s['s4'])} MIDI {int(s['mc'])} (sites {s['mc_sites'] and hex(s['mc_sites'])}) "
                f"gate {int(s['gate'])} (flags {s['gflags'] and hex(s['gflags'])}, rc {s['grc']}, words "
                f"{'hooked' if s['words'] == hooked else 'stock' if s['words'] == stock else [hex(w) for w in s['words']]})")

    # ---- 1 / 2 ota_loader_v4: live apply, power-on ----------------------------------------------------
    live, rig, st = power_on(image3, syms3, data)
    s_live = state(live)
    id_live = rh.identify(live.pn)
    live.close()
    ok &= check(f"1 ota_loader_v4, live apply + 3 passes: {show(s_live)}", all_on(s_live))
    s_boot = state(rig)
    id_boot = rh.identify(rig.pn)
    ok &= check(f"2 ota_loader_v4, power-on (slot A {'active' if st['active'] == ro.ACT_A else st['active']}): "
                f"{show(s_boot)}; Identify {id_boot and id_boot.hex(' ').upper()} = live",
                st["active"] == ro.ACT_A and all_on(s_boot) and id_boot is not None and id_boot == id_live
                and id_boot[0] == p.ngver)

    # ---- 3 the old off / on ops: refused, nothing changes ---------------------------------------------
    rc_off = setall(rig, 0)
    s_off = state(rig)
    rc_on = setall(rig, 1)
    s_on = state(rig)
    ok &= check(f"3 old off ops: rc {rc_off} -> {show(s_off)}; old on ops: rc {rc_on} -> {show(s_on)}",
                rc_off == [1, 1, 1, 1] and all_on(s_off) and rc_on == [1, 1, 1, 1] and all_on(s_on))
    rig.close()

    return rtlib.finish(ok)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
