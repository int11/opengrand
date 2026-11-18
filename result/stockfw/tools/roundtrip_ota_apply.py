# roundtrip_ota_apply.py - emulator verification of the ota_loader_v4 (stage-1) "apply now" (op 0x15), the payload
# quiesce entry, the cache maintenance after installing code, the key-state escape and the boot
# diagnostics.  Payloads: the test payloads ota_core_v2 / ota_base_v2 (tools/images.py TEST_PAYLOADS).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_ota_apply.py [ota_loader_v3] [-q]
#
# Same machine as roundtrip_ota.py (its Piano: real image, NOR model with allowed = slot range only,
# stock NOR routines unmodified, dis/ena_dsp hooks, SWI #0 emulated so the real privileged cache body
# runs).  Checks (numbered as printed):
#   1  static: op 0x15 present; cache sync = stock wrapper shape + body (D clean by MVA, drain WB,
#      I-cache invalidate, return through *0x4009D000), its svc / mcr and the runtime-hook poke's
#      (roundtrip_ota_hooks.py) the only ones in the cave; payload entry
#      tables: quiesce inside the payload; the no-quiesce copies of check 9 reserved = 0
#   2  built-in -> slot A live: no power cycle, no boot attempt, no NOR op; copy -> cache sync over
#      exactly the payload -> first instruction in the run region, in that order; identify + core
#      self-test answered by the slot code; after 60 s the LIVE mark (+0x0C), not the confirm mark
#   3  slot A -> newer slot B (target "newest"), and back to A explicitly
#   4  slot -> built-in: built-in core re-initialised (self-test 9/9, guards intact)
#   5  refused: bad slot (bit flipped in NOR) -> CRC/MAC error, old code still running and answering;
#      rejected / empty slot -> 4; bad tag -> 2; during an upload -> 3
#   6  stock flash busy during the slot read -> deferred, then completes
#   7  quiesce: pending self-test is served by the OLD cave before the switch; a cave that never
#      goes idle -> refused after the timeout, old cave still running, SysEx flows again
#   8  a SysEx call still inside the old cave (in_sysex) -> switch deferred, then done
#   9  payloads without a quiesce entry (entry word 0x14 = 0, the first payload format: here ota_core_v2 /
#      ota_base_v2 with that word cleared and signed again) boot and apply
#  10  reboot after live apply: boot rules unchanged (attempt recorded, live mark is no confirm)
#  11  20 applies in a row (built-in / A / B): no leak (dispatch balanced, gate open, stack flat,
#      flags, NOR ops = live marks only), stores confined, stock RAM unchanged
#  12  escape keys from the stock key-scan state: the stock baseline scan FUN_40154D0C run on a
#      scanner model with A0 A#0 B0 down -> built-in (reason escape); other key sets do not
#      escape; the raw mailbox / copy words are reported
from __future__ import annotations

import pathlib
import sys

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

ro = rtlib.load("roundtrip_ota")
rp, rt, op, images, pa, sb = ro.rp, ro.rt, ro.op, ro.images, ro.pa, ro.sb
check, Piano, KEY, le = ro.check, ro.Piano, ro.KEY, ro.le
STOCKFW = ro.STOCKFW
RUN, STAGE, SLOT, J = ro.RUN, ro.STAGE, ro.SLOT, ro.J
ACT_A, ACT_B, ACT_BUILTIN, BR = ro.ACT_A, ro.ACT_B, ro.ACT_BUILTIN, ro.BR
OP_APPLY = 0x15
T_A, T_B, T_BUILTIN, T_NEWEST = 0, 1, 2, 3
AP = dict(idle=0, load=1, verify=2, quiesce=3, switch=4, done=5, error=6)
BR_LIVE = 6
KEY_STATE, KEY_SCAN_ON = 0x413CAE98, 0x413CAF38
SCAN_BASELINE = 0x40154D0C          # stock: initial scan of all 88 keys into the state array
KEYCHIP = 0x62000000


def apply(pn, target, **kw):
    return pn.auth_op(OP_APPLY, bytes([target]), **kw)[0]


def run_apply(pn, limit=200):
    for i in range(limit):
        pn.tick()
        st = pn.status()
        if st["ap_state"] in (AP["done"], AP["error"]):
            return st, i + 1
    return pn.status(), limit


def identify(pn, port=2):
    r = rp.body_of(pn.sysex(port, rp.req(0x01)), port)
    return r[1] if r else None


def selftest(pn):
    """Request the core self-test (op 0x0A), tick once, return (pass mask, TX caller lrs)."""
    calls = []
    pn.emu.hook(rt.TX[2], lambda e: (calls.append(e.reg(14)), pn.sent.append((2, e.read_bytes(e.reg(0), e.reg(1)))))[1])
    pn.sysex(2, rp.req(0x0A))
    pn.sent.clear()
    pn.tick()
    got = [s for s in pn.sent if s[1][6] == 0x4A]
    pn.sent.clear()
    pn.hook_tx()
    if not got:
        return None, calls
    return le(rp.unpack7(got[0][1][7:-1]), 2, 2), calls


def scanner(emu, down):
    """Key-scan chip model for the stock baseline scan: status byte 0x62000004 never busy, 0x6200001A
    (= base register +0x16 in the code, base 0x62000004) returns one byte per key in scan order
    (0xE0 = all three sensors closed)."""
    emu.periph[KEYCHIP + 4] = 0
    emu.periph[KEYCHIP + 0x1A] = [0xE0 if k in down else 0x00 for k in range(88)]


def main(argv):
    img, stock, manifest, image, syms = rtlib.loader(rtlib.first_arg(argv, rtlib.DEFAULT_LOADER))
    elf = STOCKFW / f"cave/build/cave_{img.variant}.elf"
    cave_lo, cave_end = syms["cave_h1"], syms["cave_end"]
    rd = lambda n: (STOCKFW / "build" / f"{n}.c79").read_bytes()
    core2, base2 = rd("ota_core_v2"), rd("ota_base_v2")

    def no_quiesce(data):
        """the same payload with entry word 0x14 (quiesce) = 0 - the first slot payload format - signed again"""
        pay = bytearray(data[64:])
        pay[0x14:0x18] = bytes(4)
        return op.pack(bytes(pay), ro.KEY, op.parse(data)["feat"])
    core1, base1 = no_quiesce(core2), no_quiesce(base2)
    ok = True
    print(f"image   {img.name}  sha256 {pa.sha256(image)}")

    # ---- 1. static ------------------------------------------------------------------------------
    V4 = img.stage0     # ota_loader_v4: cache sync in stage-0 (s0_cache_sync), the hook poke in stage-1
    ins = rtlib.instructions(elf) + (rtlib.instructions(STOCKFW / "cave/build/loader_s1.elf") if V4 else [])
    sync, body = syms["ota_cache_sync"], syms.get("ota_cache_body", syms.get("s0_cache_body"))
    svcs = [a for a, m, _o in ins if m.startswith("svc")]
    mcrs = [(a, o) for a, m, o in ins if m.startswith(("mcr", "mrc", "msr", "cps"))]
    poke, pbody = syms["ota_hook_poke"], syms["ota_poke_body"]   # runtime-hook word write: roundtrip_ota_hooks.py
    ok &= check("1 the loader's (stage-0 + stage-1) only svcs are the cache-sync wrapper's and the hook-poke wrapper's, all CP15 / mode writes "
                "are in their privileged bodies",
                svcs == sorted([sync + 0x10, poke + 0x10]) and len(mcrs) == 6
                and sum(body <= a < body + 0x30 for a, _o in mcrs) == 3 and sum(pbody <= a < pbody + 0x20 for a, _o in mcrs) == 3,
                f"svc {[hex(a) for a in svcs]}, mcr {[(hex(a), o) for a, o in mcrs]}")
    wrap = [m + " " + o for a, m, o in ins if sync <= a < sync + 0x1C]
    ok &= check("1 wrapper = the stock 0x4009D52C shape: push {r0,r1,r4,r5,ip,lr}; r4/r5 = range (v4: first line / end); "
                "r0 = body; svc #0; pop",
                wrap[0].startswith("push") and "r4, r5, ip, lr" in wrap[0] and wrap[3].startswith("ldr r0")
                and wrap[4].startswith("svc") and wrap[5].startswith("pop") and le(image, sync + 0x1C - 0x40060000) == body, str(wrap))
    bod = [(m, o) for a, m, o in ins if body <= a < body + 0x30]
    ok &= check("1 body: clean D-cache by MVA over the range (c7,c10,1), drain write buffer (c7,c10,4) and "
                "invalidate I-cache (c7,c5,0) (v4: I-invalidate before the drain, both before the exception return), return "
                "through *0x4009D000 (= the stock SWI return stub 0x400A88BC)",
                [o.replace(" ", "") for m, o in bod if m == "mcr"] == (
                    ["15,0,r4,cr7,cr10,{1}", "15,0,r0,cr7,cr5,{0}", "15,0,r0,cr7,cr10,{4}"] if V4 else
                    ["15,0,r4,cr7,cr10,{1}", "15,0,r0,cr7,cr10,{4}", "15,0,r0,cr7,cr5,{0}"])
                and le(image, 0x4009D000 - 0x40060000) == ro.SVC_RET, str(bod))
    pn = Piano(image, syms)
    res = pn.emu.call(body, regs={"r4": RUN, "r5": RUN + 0x6000 if V4 else 0x6000}, until=ro.SVC_RET)
    ok &= check("1 the body runs on the ARM926 model (SVC mode, CP15 c7 ops accepted) and ends at the SWI return stub",
                res.stop == "until" and res.pc == ro.SVC_RET and not res.writes, f"{res.stop} pc {res.pc:#x}")
    pn.close()
    for name, data, has_q in (("ota_core_v2", core2, True), ("ota_base_v2", base2, True), ("ota_core_v2 (no quiesce)", core1, False),
                              ("ota_base_v2 (no quiesce)", base1, False)):
        pay = data[64:]
        q = le(pay, 0x14)
        good = (RUN <= q < RUN + len(pay) and not q & 3) if has_q else q == 0
        ok &= check(f"1 {name}: entry table word 0x14 = {'quiesce inside the payload' if has_q else 'reserved 0 (no quiesce)'}, ABI 1",
                    good and le(pay, 4) == 1 and op.parse(data)["abi"] == 1, f"{q:#x}")

    # ---- 2. built-in -> slot A, live -----------------------------------------------------------------
    pn = Piano(image, syms)
    st, _ = pn.boot()
    pn.upload(0, core2, seq=1)
    pn.run_job()
    nor_a = bytes(pn.nor.mem)
    st = pn.status()
    ok &= check(f"2 status layout {3 if V4 else 2}: caps apply + diagnostics + key escape, idle",
                st["layout"] == (3 if V4 else 2) and st["caps"] & 7 == 7
                and st["ap_state"] == AP["idle"] and st["active"] == ACT_BUILTIN)
    ev = []
    lo_c, hi_c = (0x40176000, 0x40180000) if V4 else (cave_lo, cave_end)   # who copies: stage-1 / the resident
    hw = pn.emu.uc.hook_add(UC_HOOK_MEM_WRITE, lambda uc, acc, a, sz, v, d: ev.append(("w", a, uc.reg_read(rt.se.A.UC_ARM_REG_PC))),
                            None, RUN, RUN + 0x20000)
    hx = pn.emu.uc.hook_add(UC_HOOK_CODE, lambda uc, a, sz, d: ev.append(("x", a, 0)) if not ev or ev[-1][0] != "x" else None,
                            None, RUN, RUN + 0x20000)
    n_ops, n_svc = len(pn.nor.ops), len(pn.svc_calls)
    pn.emu.hook(syms["ota_cache_sync"], lambda e: (ev.append(("s", e.reg(0), e.reg(1))), rt.se.StockEmu.CONTINUE)[1])
    rc = apply(pn, T_NEWEST)
    st, n = run_apply(pn)
    pn.emu.unhook(syms["ota_cache_sync"])
    pn.hook_svc()
    pn.emu.uc.hook_del(hw)
    pn.emu.uc.hook_del(hx)
    pn.emu.uc.ctl_flush_tb()
    ok &= check("2 apply 'newest' from the built-in (v4: no main) -> slot A running, reason live, no power cycle",
                rc == 0 and st["ap_state"] == AP["done"] and st["active"] == ACT_A and st["reason"] == BR_LIVE
                and st["live"] == 1 and st["ap_count"] == 1, f"rc {rc} state {st['ap_state']} err {st['ap_err']} after {n} passes")
    ok &= check("2 the run region holds the payload code; no boot attempt spent, no NOR program/erase at all",
                pn.emu.read_bytes(RUN, 0x1000) == core2[64:64 + 0x1000] and st["slots"][0]["tries"] == 0
                and len(pn.nor.ops) == n_ops, f"tries {st['slots'][0]['tries']}, NOR ops +{len(pn.nor.ops) - n_ops}")
    copy_w = [i for i, e in enumerate(ev) if e[0] == "w" and lo_c <= e[2] < hi_c]
    syncs = [i for i, e in enumerate(ev) if e[0] == "s"]
    first_x = next((i for i, e in enumerate(ev) if e[0] == "x" and syncs and i > syncs[0]), None)
    x_between = [e for e in ev[copy_w[0]:syncs[0]] if e[0] == "x"] if copy_w and syncs else ["?"]
    ok &= check("2 order: resident copy (all stores) -> one cache sync over exactly [0x401C0000, +payload) -> "
                "first instruction fetched from the run region; nothing executes there in between",
                len(syncs) == 1 and ev[syncs[0]][1:] == (RUN, len(core2) - 64) and copy_w[-1] < syncs[0] < (first_x or 0)
                and not x_between and len(pn.svc_calls) == n_svc + 1,
                f"{len(copy_w)} copy stores, sync {ev[syncs[0]][1:] if syncs else None}, first fetch after sync at event {first_x}")
    r = identify(pn)
    mask, calls = selftest(pn)
    ok &= check("2 after the switch the slot code answers: identify 3B 07, core self-test 9/9 (TX caller in the run region)",
                r is not None and r[3:5] == bytes([0x3B, 0x07]) and mask == 0x1FF and calls and all(RUN <= c < RUN + 0x20000 for c in calls),
                f"mask {mask}")
    pn.emu.write_u32(syms["ota_alive"], ro.CONFIRM_TICKS - 2)
    pn.tick(3)
    st = pn.status()
    sa = ro.slot_image(pn.nor, 0)
    ok &= check("2 60 s after a LIVE apply: live mark +0x0C programmed (flag 0x80), confirm +0x08 NOT written",
                st["slots"][0]["flags"] & 0x80 and not st["slots"][0]["flags"] & 0x02 and sa[0xC:0xE] == b"\0\0" and sa[8:0xA] == b"\xff\xff",
                f"flags {st['slots'][0]['flags']:#x}")
    nor_a_live = bytes(pn.nor.mem)

    # ---- 3. slot A -> newer slot B -> A ---------------------------------------------------------------
    ok &= check("3 begin to the running slot A is refused (4)", pn.begin(0, base2, 2) == 4)
    pn.upload(1, base2, seq=2)
    st, _ = pn.run_job()
    rc = apply(pn, T_NEWEST)
    st, _ = run_apply(pn)
    r = identify(pn)
    ok &= check("3 upload ota_base_v2 to B while A runs, apply 'newest' -> B (seq 2) runs: identify feat1 0x05 from the slot",
                rc == 0 and st["active"] == ACT_B and st["ap_state"] == AP["done"] and r is not None and r[4] == 0x05)
    rc = apply(pn, T_A)
    st, _ = run_apply(pn)
    r = identify(pn)
    ok &= check("3 apply slot A explicitly -> A again (feat1 0x07), slot A image untouched in NOR",
                rc == 0 and st["active"] == ACT_A and r is not None and r[4] == 0x07
                and ro.slot_image(pn.nor, 0)[0x200:0x200 + len(core2)] == core2)
    nor_ab = bytes(pn.nor.mem)

    # ---- 4. slot -> built-in ------------------------------------------------------------------------------
    rc = apply(pn, T_BUILTIN)
    st, n = run_apply(pn)
    if V4:
        r = identify(pn)
        ok &= check("4 apply target 2 -> no main (reason live): stage-1's minimal responder answers Identify (feat0 0x01, "
                    "feat1 0x04), the slot code is never called again",
                    rc == 0 and st["active"] == ACT_BUILTIN and st["reason"] == BR_LIVE and r is not None
                    and r[:5] == ro.NO_MAIN_ID, f"{r.hex() if r else None} after {n} passes")
    else:
        mask, calls = selftest(pn)
        guards = (pn.emu.read_u32(0x40180000) == 0xC0DE5A17)
        ok &= check("4 apply built-in -> built-in runs (reason live), its core re-initialised: self-test 9/9 from the cave, "
                    "RW guard word rewritten",
                    rc == 0 and st["active"] == ACT_BUILTIN and st["reason"] == BR_LIVE and mask == 0x1FF
                    and calls and all(cave_lo <= c < cave_end for c in calls) and guards, f"mask {mask} after {n} passes")
    pn.close()

    # ---- 5. refusals -----------------------------------------------------------------------------------------
    mem = bytearray(nor_ab)
    mem[0x1C0000 + 0x200 + 64 + 300] ^= 0x04           # slot B payload bit
    pn = Piano(image, syms, nor_mem=bytes(mem))
    pn.boot()                                           # B fails at boot -> A runs
    rc = apply(pn, T_A)
    run_apply(pn)
    before = pn.emu.read_bytes(RUN, 0x2000)
    rc = apply(pn, T_B)
    st, n = run_apply(pn)
    r = identify(pn)
    ok &= check("5 bad slot B (bit flipped in NOR) -> apply refused with CRC error, A still running and answering, "
                "run region untouched, SysEx gate open",
                rc == 0 and st["ap_state"] == AP["error"] and st["ap_err"] in (10, 12) and st["active"] == ACT_A
                and pn.emu.read_bytes(RUN, 0x2000) == before and r is not None and r[4] == 0x07
                and pn.emu.read_bytes(syms["ota_gate"], 1) == b"\0", f"err {st['ap_err']}")
    pn.close()
    pn = Piano(image, syms, nor_mem=nor_a)
    pn.boot()
    ok &= check(f"5 apply to an empty slot -> 4; bad tag -> 2; target {5 if V4 else 4} -> 1",
                (apply(pn, T_B), apply(pn, T_A, key=bytes(32)), apply(pn, 5 if V4 else 4)) == (4, 2, 1))
    pn.auth_op(ro.OP_REJECT, bytes([1]))
    pn.tick(2)
    ok &= check("5 rejected slot A -> 4, 'newest' then means the built-in", apply(pn, T_A) == 4 and apply(pn, T_NEWEST) == 0
                and run_apply(pn)[0]["active"] == ACT_BUILTIN)
    pn.close()
    pn = Piano(image, syms, nor_mem=nor_a)
    pn.boot()
    pn.begin(1, base2, 2)
    ok &= check("5 apply while an upload is receiving -> busy 3 (staging is shared)", apply(pn, T_A) == 3)
    pn.close()

    # ---- 6. flash busy ---------------------------------------------------------------------------------------
    pn = Piano(image, syms, nor_mem=nor_a)
    pn.boot()
    pn.emu.write_bytes(ro.BUSY_FLAG, b"\x01\0\0\0")
    apply(pn, T_A)
    pn.tick(15)
    st = pn.status()
    r = identify(pn)
    ok &= check("6 stock flash busy -> the slot read for a re-apply of A is deferred (15 passes), the running A copy "
                "keeps answering",
                st["ap_state"] == AP["load"] and st["active"] == ACT_A and st["defers"] >= 15 and r is not None)
    pn.emu.write_bytes(ro.BUSY_FLAG, b"\0\0\0\0")
    st, _ = run_apply(pn)
    ok &= check("6 busy cleared -> apply completes", st["ap_state"] == AP["done"] and st["active"] == ACT_A)
    pn.close()

    # ---- 7. quiesce ---------------------------------------------------------------------------------------------
    pn = Piano(image, syms, nor_mem=nor_a)
    pn.boot()
    calls = []
    pn.emu.hook(rt.TX[2], lambda e: (calls.append((e.reg(14), e.read_bytes(e.reg(0), 7)[6])),
                                      pn.sent.append((2, e.read_bytes(e.reg(0), e.reg(1)))))[1])
    pn.sysex(2, rp.req(0x0A))                 # self-test pending in slot A's core (served on its next tick)
    apply(pn, T_BUILTIN)                       # switch to the built-in: quiesce must wait for that request
    pn.sent.clear()
    seen = []
    for _ in range(10):
        pn.tick()
        seen += [s for s in pn.sent if s[1][6] == 0x4A]
        pn.sent.clear()
        if pn.status()["ap_state"] in (AP["done"], AP["error"]):
            break
    pn.hook_tx()
    ok &= check("7 a pending self-test makes quiesce say 'busy': the OLD cave serves it on its tick, then the switch happens",
                len(seen) == 1 and pn.status()["ap_state"] == AP["done"] and [RUN <= c < RUN + 0x20000 for c, o in calls if o == 0x4A] == [True])
    if V4:   # no built-in cave: the old cave of 7 / 8 is the slot payload (no main has nothing to quiesce)
        apply(pn, T_A)
        run_apply(pn)
        q, old_act, new_t, new_act = le(core2[64:], 0x14), ACT_A, T_BUILTIN, ACT_BUILTIN
    else:
        q, old_act, new_t, new_act = syms["builtin_quiesce"], ACT_BUILTIN, T_A, ACT_A
    pn.emu.hook(q, lambda e: 1)                # a cave that never goes idle
    apply(pn, new_t)
    st, n = run_apply(pn, 400)
    pn.emu.unhook(q)
    r = identify(pn)
    ok &= check("7 a cave that never goes idle -> refused after the quiesce timeout (250 passes, error 20), old cave "
                "still running, SysEx reaches it again",
                st["ap_state"] == AP["error"] and st["ap_err"] == 20 and st["active"] == old_act and r is not None
                and n > 250, f"err {st['ap_err']} after {n} passes")

    # ---- 8. SysEx call still inside the old cave -------------------------------------------------------------------
    pn.emu.write_bytes(syms["ota_in_sysex"], b"\x01")
    apply(pn, new_t)
    for _ in range(40):
        pn.tick()
    st = pn.status()
    ok &= check("8 in_sysex set (a call blocked inside the old cave) -> no switch, apply waits in quiesce",
                st["ap_state"] == AP["quiesce"] and st["active"] == old_act)
    pn.emu.write_bytes(syms["ota_in_sysex"], b"\x00")
    st, _ = run_apply(pn)
    ok &= check("8 call returned -> switch done", st["ap_state"] == AP["done"] and st["active"] == new_act and pn.dsp_depth == 0)
    pn.close()

    # ---- 9. backward compatibility ---------------------------------------------------------------------------------
    pn = Piano(image, syms)
    pn.boot()
    pn.upload(0, core1, seq=1)
    pn.run_job()
    pn.upload(1, base1, seq=2)
    st, _ = pn.run_job()
    rc = apply(pn, T_NEWEST)
    st, _ = run_apply(pn)
    r = identify(pn)
    ok3 = rc == 0 and st["active"] == ACT_B and r is not None and r[4] == 0x05
    apply(pn, T_A)
    st, _ = run_apply(pn)
    mask, _c = selftest(pn)
    nor_v1 = bytes(pn.nor.mem)
    pn.close()
    pn = Piano(image, syms, nor_mem=nor_v1)
    st, _ = pn.boot()
    ok &= check("9 payloads without a quiesce entry (word 0x14 = 0): upload, apply B then A live (self-test 9/9), "
                f"and boot (B, newest) on {img.name}",
                ok3 and mask == 0x1FF and st["active"] == ACT_B,
                f"boot active {st['active']}")
    pn.close()

    # ---- 10. reboot after a live apply ---------------------------------------------------------------------------------
    pn = Piano(image, syms, nor_mem=nor_a_live)
    st, _ = pn.boot()
    ok &= check("10 reboot after the live run: slot A boots by the boot rules, attempt 1 recorded (live mark is no confirm)",
                st["active"] == ACT_A and st["reason"] == BR["slot"] and st["slots"][0]["tries"] == 1
                and st["slots"][0]["flags"] & 0x80 and not st["slots"][0]["flags"] & 0x02)
    pn.close()

    # ---- 11. 20 applies ------------------------------------------------------------------------------------------------
    pn = Piano(image, syms, nor_mem=nor_ab)
    lo, hi = rp.STOCK_RAM
    ram0 = pn.emu.read_bytes(lo, hi - lo)
    pn.boot()
    pn.writes.clear()
    n_ops = len(pn.nor.ops)
    seq = [T_BUILTIN, T_A, T_B] * 7
    results, depths = [], []
    for t in seq[:20]:
        pn.emu.write_bytes(0x40FF0000, b"\xCD" * 0x10000)
        rc = apply(pn, t)
        st, _ = run_apply(pn)
        m = pn.emu.read_bytes(0x40FF0000, 0x10000)
        depths.append(0x10000 - next(i for i, b in enumerate(m) if b != 0xCD))
        results.append((rc, st["ap_state"], st["active"]))
    want = [(0, AP["done"], {T_BUILTIN: ACT_BUILTIN, T_A: ACT_A, T_B: ACT_B}[t]) for t in seq[:20]]
    st = pn.status()
    r = identify(pn)
    ok &= check("11 20 applies in a row (built-in / A / B) all done, status counts 20, identify answers",
                results == want and st["ap_count"] == 20 and r is not None, str([x for x, y in zip(results, want) if x != y][:3]))
    ok &= check("11 no leak: dispatch lock balanced, gate open, in_sysex 0, no NOR program/erase, stack depth flat",
                pn.dsp_depth == 0 and not pn.nested and pn.emu.read_bytes(syms["ota_gate"], 1) == b"\0"
                and pn.emu.read_bytes(syms["ota_in_sysex"], 1) == b"\0" and len(pn.nor.ops) == n_ops
                and max(depths) <= 2048 and len(set(depths[3:])) <= 3, f"stack {min(depths)}-{max(depths)} B")
    data_lo = min(lo_ for lo_, _h, sec, kind, _o in sb.layout(img.variant) if sec == ".cave" and kind == "data")
    s1_data = [(l_, h_) for l_, h_, sec, kind, _o in sb.layout("s1") if sec == ".cave" and kind == "data"] if V4 else []
    allowed = s1_data + [(cave_lo, cave_end), (0x40180000, 0x401A0000), (STAGE, STAGE + 0x20000), (RUN, RUN + 0x20000),
               (ro.VBUF, ro.VBUF + 0x1000), ro.STACK, (ro.KERNEL_ERR, ro.KERNEL_ERR + 4), (ro.BUSY_FLAG, ro.BUSY_FLAG + 4),
               (0x40B90000, 0x40B90200)]
    stray = [w for w in pn.writes if not any(a <= w.addr < b for a, b in allowed)]
    code_w = [w for w in pn.writes if cave_lo <= w.addr < data_lo]
    ram1 = pn.emu.read_bytes(lo, hi - lo)
    diff = [lo + i for i in range(len(ram1)) if ram1[i] != ram0[i]] if ram1 != ram0 else []
    diff = [a for a in diff if not (ro.KERNEL_ERR <= a < ro.KERNEL_ERR + 4 or a == ro.DSP_BYTE or a == KEY_SCAN_ON)]
    ok &= check(f"11 {len(pn.writes)} stores of 20 applies: only cave .data (v4: stage-0 / stage-1 .data), core RW, staging, run region, stack, "
                "the test's SysEx buffer; never the cave code; stock RAM 0x41000000-0x41D00000 unchanged",
                not stray and not code_w and not diff, f"stray {stray[0]}" if stray else (f"diff {diff[:2]}" if diff else ""))
    pn.close()

    # ---- 12. escape from the stock key-scan state ------------------------------------------------------------------------
    def boot_with_keys(down, mailbox=None, scan=True):
        p = Piano(image, syms, nor_mem=nor_a, mailbox=mailbox)
        p.emu.write_bytes(KEY_SCAN_ON, b"\0")
        if scan:
            scanner(p.emu, set(down))
            p.emu.hook(0x400AA8F4, lambda e: 0)            # the stock dly_tsk wrapper it calls first
            res = p.emu.call(0x40154D94)                    # stock: baseline scan + scan-on flag
            if not res.ok:
                raise RuntimeError(res)
        st_, n_ = p.boot(600)
        arr = p.emu.read_bytes(KEY_STATE, 88)
        if V4:   # stage-0's own record (also there when E1 left no stage-1 to answer): reason, esc, keys, wait
            b0 = p.emu.read_bytes(syms["s0_boot"], 72)
            s0 = dict(reason=b0[10], flags=b0[11], mbox0=le(b0, 52), mbox1=le(b0, 56), copy=le(b0, 60), esc=b0[64],
                      keys_down=b0[65], keys_low=b0[66], esc_wait=b0[67])
            st_ = dict(st_ or {}, s0=s0)
        p.close()
        return st_, n_, arr

    if V4:
        st, n, arr = boot_with_keys({0, 1, 2})
        s0 = st["s0"]
        ok &= check("12 stock baseline scan with A0 A#0 B0 down -> state bytes 0xE0 at 0..2 -> E1: pure stock (no stage-1 "
                    "answers), stage-0 reason escape, source = key state, scan on, 3 keys down",
                    arr[:3] == b"\xE0" * 3 and not any(arr[3:]) and "active" not in st and s0["reason"] == 3
                    and s0["esc"] == 0x44 and s0["keys_down"] == 3 and s0["keys_low"] == 7, f"esc {s0['esc']:#x}")
        st, n, arr = boot_with_keys({0, 1})
        ok &= check("12 A0 A#0 down -> E2: SAFE - stage-1 runs with no main (reason 7), stage-0 reason SAFE, esc = key state "
                    "E2 + scan on, 2 keys down",
                    st.get("active") == ACT_BUILTIN and st.get("reason") == 7 and st["s0"]["reason"] == 6
                    and st["s0"]["flags"] & 1 and st["esc"] == 0x60 and st["keys_down"] == 2 and st["keys_low"] == 3,
                    f"{st.get('active')}/{st.get('reason')} esc {st.get('esc')}")
        outs = []
        for down in ({0, 1, 2, 3}, {0, 1, 2, 50}, {84, 86, 87, 79}, {0}, set()):
            st, _n, _a = boot_with_keys(down)
            outs.append((st.get("active"), st["s0"]["esc"] & 0x3F))
        ok &= check("12 other key sets (A0+A#0+B0 + C1, + a middle key, E7 A7 B7 C8, A0 alone, none) escape neither way -> "
                    "slot A boots", outs == [(ACT_A, 0)] * 5, str(outs))
        st, n, _a = boot_with_keys(set(), mailbox=(0x55AAA55A, 0x40000007))
        ok &= check("12 mailbox path still escapes (E1); stage-0 keeps the raw mailbox words and *0x40000000",
                    "active" not in st and st["s0"]["reason"] == 3 and st["s0"]["mbox0"] == 0x55AAA55A
                    and st["s0"]["mbox1"] == 0x40000007 and st["s0"]["esc"] & 0x01 and st["s0"]["copy"] == 0)
        st, n, _a = boot_with_keys(set(), mailbox=(0x55AAA55A, 0x40000003))
        ok &= check("12 mailbox code 0x40000003 (A0 A#0) -> E2: SAFE, no main; status shows the mailbox E2 bit",
                    st.get("active") == ACT_BUILTIN and st.get("reason") == 7 and st["esc"] & 0x08 != 0)
        st, n, _a = boot_with_keys({0, 1, 2}, scan=False)
        ok &= check("12 key scan never switched on -> stage-0 waits at most 250 passes, then boots normally (diag: scan off)",
                    st.get("active") == ACT_A and not st["esc"] & 0x40 and st["esc_wait"] == 250 and 250 < n < 400, f"{n} passes")
    else:
        st, n, arr = boot_with_keys({0, 1, 2})
        ok &= check("12 stock baseline scan with A0 A#0 B0 down -> state bytes 0xE0 at 0..2 -> built-in, reason escape, "
                    "source = key state, scan on, 3 keys down",
                    arr[:3] == b"\xE0" * 3 and not any(arr[3:]) and st["active"] == ACT_BUILTIN and st["reason"] == BR["escape"]
                    and st["esc"] == 0x44 and st["keys_down"] == 3 and st["keys_low"] == 7, f"esc {st['esc']:#x}")
        outs = []
        for down in ({0, 1}, {0, 1, 2, 3}, {0, 1, 2, 50}, {84, 86, 87, 79}, set()):
            st, _n, _a = boot_with_keys(down)
            outs.append((st["active"], st["esc"] & 0x07))
        ok &= check("12 other key sets (A0+A#0, + C1, + a middle key, E7 A7 B7 C8, none) do not escape -> slot A boots",
                    outs == [(ACT_A, 0)] * 5, str(outs))
        st, n, _a = boot_with_keys(set(), mailbox=(0x55AAA55A, 0x40000007))
        ok &= check("12 mailbox path still escapes; diagnostics carry the raw mailbox words and *0x40000000",
                    st["reason"] == BR["escape"] and st["mbox0"] == 0x55AAA55A and st["mbox1"] == 0x40000007
                    and st["esc"] & 0x01 and st["copy"] == 0)
        st, n, _a = boot_with_keys({0, 1, 2}, scan=False)
        ok &= check("12 key scan never switched on -> boot waits at most 250 passes, then boots normally (diag: scan off)",
                    st["active"] == ACT_A and not st["esc"] & 0x40 and st["esc_wait"] == 250 and 250 < n < 300, f"{n} passes")

    return rtlib.finish(ok, " - emulation only: no interrupts, no RTOS scheduling, no caches (the CP15 ops execute but do nothing).")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
