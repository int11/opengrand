# roundtrip_ota_hooks.py - emulator verification of the ota_loader_v4 (stage-1) runtime hook service (17-site allow-list)
# with the demo test payload ota_hookdemo3 (tools/images.py TEST_PAYLOADS: one pass-through counting stub per site).
# (The checks were written for the first 5-site allow-list; SITES_V2 below are those five, the first five of v3's.)
#
#   cd result/stockfw && uv run python -I tools/roundtrip_ota_hooks.py [ota_loader_v3] [-q]
#
# Same machine as roundtrip_ota.py (its Piano: the real ota_loader_v3 image, NOR model limited to
# the slot range, stock NOR routines unmodified, SWI #0 emulated so the real privileged bodies run).
# Checks (numbered as printed):
#   1  static: the allow-list in the built cave = the five planned sites, each original word = the
#      stock image word (`bl`), the resident's own hooks (0x40143AE0, 0x40147E1C) not in it; the poke
#      body = one str + D clean line + drain + I invalidate line + SWI return, runs on the ARM926
#      model; the demo payload: attach flag + attach() inside it, five stubs that only count and
#      tail-call (`b`) exactly the stock target of their site (no bl / svc / blocking call)
#   2  demo in slot A, applied live: attach() hooks 0x40147E94 (bl -> hk_kawai), op 0x16 and status
#      report it, the poke ran once with exactly that word
#   3  Kawai SysEx through the real stock end handler: FUN_4014A2C8 entered with r0-r11, sp, lr
#      identical to the unhooked path (lr = 0x40147E98), handler state cleared after the return,
#      op 0x0B counts every message
#   4  every allowed site: register (a demo stub) -> the word is the expected bl, the stock path
#      through the site reaches the stub and then the stock target with r0-r11 / sp / lr = site+4
#      intact, returns to site+4; unregister -> stock word, the path skips the stub
#   5  refusals (no poke, word unchanged): wrong original, not allow-listed / resident's own hooks /
#      misaligned, target outside the payload / misaligned, kernel-timer site without NOBLOCK,
#      unknown flag, already hooked / not hooked, outside attach()/tick() or another task, built-in
#      active, gate closed (apply in progress), RAM word not the original
#   6  apply to another payload: all hooks restored under dis_dsp BEFORE the old payload is dropped
#      and the new one copied; the new payload (no attach) leaves them stock; back to the demo ->
#      attach() re-registers (fresh counters)
#   7  apply to the built-in restores every stock word; quiesce timeout / SysEx in flight -> the old
#      payload keeps running WITH its hooks; escape at boot -> built-in, no hooks
#   8  stores: hook / unhook / apply touch only cave .data, R1 OTA areas, the stack and the site
#      words; stock code region identical to the boot copy after removal
#   9  100 x register / unregister on all five sites: counters, table empty, words stock, stack flat,
#      no stray store
#  10  (dropped with the first loader: the demo on a loader without the hook service)
#  11  the new sites from real stock paths: program change / control change bytes through the
#      stock byte parser FUN_40148B24 reach the stub and then the stock handler with the unhooked registers;
#      the task-20 in-flight flag (svc->inflight20) holds an apply like the task-19 one
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

ro = rtlib.load("roundtrip_ota")
rp, rt, op, images, pa, sb = ro.rp, ro.rt, ro.op, ro.images, ro.pa, ro.sb
check, Piano, le = ro.check, ro.Piano, ro.le
STOCKFW = ro.STOCKFW
RUN, STAGE = ro.RUN, ro.STAGE
ACT_A, ACT_B, ACT_BUILTIN = ro.ACT_A, ro.ACT_B, ro.ACT_BUILTIN
OP_APPLY, OP_HOOKS, OP_DEMO = 0x15, 0x16, 0x0B
T_A, T_B, T_BUILTIN, T_NEWEST = 0, 1, 2, 3
AP_DONE, AP_ERROR, AP_QUIESCE = 5, 6, 3
NOBLOCK = 0x01
E = dict(ok=0, ctx=1, site=2, orig=3, target=4, flags=5, state=6, busy=7, verify=8)
CUR_TASK = 0x4139E668
STOCK_CODE = (0x40060000, 0x40173A14)     # stock image as copied by the loader (below the cave)
# (site, original word, stock target, context 1 task19 / 2 task24 / 3 kernel timer, demo stub)
SITES_V2 = [
    (0x40147E94, 0xEB00090B, 0x4014A2C8, 1, "hk_kawai"),
    (0x401539AC, 0xEBFF5B88, 0x4012A7D4, 1, "hk_midi"),
    (0x40143AB4, 0xEBFDA539, 0x400ACFA0, 2, "hk_hp"),
    (0x40143AB8, 0xEBFDA49A, 0x400ACD28, 2, "hk_exec"),
    (0x400AA874, 0xEB0005DB, 0x400ABFE8, 3, "hk_apo"),
]
# ota_loader_v3 adds (context 4 = task 20)
SITES_V3 = SITES_V2 + [
    (0x40148C4C, 0xEBFFFA5E, 0x401475CC, 1, "hk_pc"),       # MIDI program change
    (0x40148C60, 0xEBFFFD6C, 0x40148218, 1, "hk_cc"),       # MIDI control change
    (0x40153998, 0xEB003FD0, 0x401638E0, 1, "hk_dly"),      # delayed parameter timers
    (0x401739E4, 0xEBFFE91C, 0x4016DE5C, 4, "hk_t20"),      # task-20 pass
    (0x401739E8, 0xEBFEDB79, 0x4012A7D4, 4, "hk_t20tail"),  # task-20 loop tail
    (0x4016DE80, 0xEBFFBFE1, 0x4015DE0C, 4, "hk_mq"),       # Music queue
    (0x4016DE94, 0xEB000749, 0x4016FBC0, 4, "hk_metro"),    # metronome
    (0x40143AC4, 0xEBFF8EB2, 0x40127594, 2, "hk_rmode"),    # remote-mode executor
    (0x40143AC8, 0xEBFF8E62, 0x40127458, 2, "hk_media"),    # media state Result
    (0x40143ACC, 0xEB002E24, 0x4014F364, 2, "hk_file"),     # fn 0x24 file-op executor
    (0x40143AD8, 0xEBFF80D5, 0x40123E34, 2, "hk_xport"),    # audio transport
    (0x40143ADC, 0xEBFF8161, 0x40124068, 2, "hk_notify"),   # mode-3 notify
]
SITES = SITES_V2          # main() switches to SITES_V3 for ota_loader_v3
KAWAI = SITES_V2[0]
V3 = False


def bl_target(site, word):
    off = word & 0xFFFFFF
    off -= (off & 0x800000) << 1
    return site + 8 + 4 * off


def bl_word(site, orig, target):
    return (orig & 0xFF000000) | (((target - site - 8) >> 2) & 0xFFFFFF)


def apply(pn, target):
    return pn.auth_op(OP_APPLY, bytes([target]))[0]


def run_apply(pn, limit=400):
    for i in range(limit):
        pn.tick()
        st = pn.status()
        if st["ap_state"] in (AP_DONE, AP_ERROR):
            return st, i + 1
    return pn.status(), limit


def hooks(pn):
    """op 0x16.  Layout 1 (v2): one reply, 9 B per site from byte 12.  Layout 2 (v3): pages of up to 7 sites
    (request argument = first site index; byte 12 first, 13 count, sites from byte 14) - all pages read."""
    r = pn.reply(2, rp.req(OP_HOOKS), OP_HOOKS)
    if r is None:
        return None
    pages = [r]
    if r[0] == 1:
        sites = [dict(site=le(r, 12 + 9 * i), state=r[16 + 9 * i], word=le(r, 17 + 9 * i)) for i in range(r[1])]
    else:
        sites = []
        while True:
            q = pages[-1]
            sites += [dict(site=le(q, 14 + 9 * i), state=q[18 + 9 * i], word=le(q, 19 + 9 * i)) for i in range(q[13])]
            if len(sites) >= r[1] or q[13] == 0:
                break
            pages.append(pn.reply(2, rp.req(OP_HOOKS, len(sites)), OP_HOOKS))
    return dict(layout=r[0], n=r[1], hooked=r[2], last=r[3], refused=le(r, 4, 2), sets=le(r, 6, 2),
                clears=le(r, 8, 2), anomaly=le(r, 10, 2), sites=sites, raw_len=len(r),
                pages=[(q[12], q[13], len(q)) for q in pages] if r[0] == 2 else None)


def demo(pn):
    r = pn.reply(2, rp.req(OP_DEMO), OP_DEMO)
    if r is None:
        return None
    return dict(layout=r[0], attach_rc=r[1], svc_ver=r[2], counts=[le(r, 4 + 4 * i) for i in range((len(r) - 4) // 4)])


def identify(pn):
    r = rp.body_of(pn.sysex(2, rp.req(0x01)), 2)
    return r[1] if r else None


def words(pn):
    return [pn.emu.read_u32(s[0]) for s in SITES]


STOCK_WORDS = [s[1] for s in SITES]


def call_svc(pn, name, *args, open_=True):
    """Call ota_hook_set / ota_hook_clear as the payload would from attach()/tick(): the resident's
    ota_hk_open flag up, same NORTi task id as the last engine-loop pass."""
    pn.emu.write_bytes(pn.syms["ota_hk_open"], b"\x01" if open_ else b"\x00")
    res = pn.call(pn.syms[name], *args)
    pn.emu.write_bytes(pn.syms["ota_hk_open"], b"\x00")
    rc = res.r0
    return rc - (1 << 32) if rc & 0x80000000 else rc


def through(pn, site, target, regs_in):
    """Run the stock instruction at `site` (and whatever it branches to) until it returns to site+4,
    with the stock target replaced by a recorder.  -> (registers at the target, CallResult)."""
    seen = []
    pn.emu.hook(target, lambda e: seen.append([e.reg(i) for i in range(16)]))
    res = pn.emu.call(site, *regs_in[:4], regs={f"r{i}": regs_in[i] for i in range(4, 12)}, until=site + 4)
    pn.emu.unhook(target)
    return (seen[0] if len(seen) == 1 else None), res


def deliver(pn, port, body):
    """rt.deliver, but keep the Piano's TX hooks and return the end handler's CallResult."""
    pn.emu.write_bytes(rt.BUF_BASE + port * 0x800, body)
    pn.emu.write_bytes(rt.LEN_BASE + port * 2, len(body).to_bytes(2, "little"))
    pn.emu.write_bytes(rt.STATE_BASE + port, b"\xF0")
    return pn.emu.call(rt.END_HANDLER, port, max_insns=5_000_000)


def boot_with(image, syms, files, apply_to=None):
    """Fresh Piano; upload files {slot: (data, seq)}; optionally apply one live."""
    pn = Piano(image, syms)
    pn.boot()
    for slot, (data, seq) in files.items():
        pn.upload(slot, data, seq=seq)
        pn.run_job()
    if apply_to is not None:
        apply(pn, apply_to)
        run_apply(pn)
    return pn


def main(argv):
    global SITES, V3, STOCK_WORDS
    which = rtlib.first_arg(argv, rtlib.DEFAULT_LOADER)
    V3 = which in rtlib.ALLOW17
    SITES = SITES_V3 if V3 else SITES_V2
    STOCK_WORDS = [s[1] for s in SITES]
    NS = len(SITES)
    CAPS = 0x1F if V3 else 0x0F
    SVC_VER = (3 if which == "ota_loader_v4" else 2) if V3 else 1
    img, stock, manifest, image, syms = rtlib.loader(which)
    elf = STOCKFW / f"cave/build/cave_{img.variant}.elf"
    cave_lo, cave_end = syms["cave_h1"], syms["cave_end"]
    if not V3:
        raise SystemExit(f"{which}: only ota_loader_v4 is built")
    _p, hook_demo, _pay, plen, pelf, psyms = rtlib.payload("ota_hookdemo3")
    base2 = rtlib.payload("ota_base_v2").data
    ok = True
    print(f"image   {img.name}  sha256 {pa.sha256(image)}  demo payload {plen} B")

    # ---- 1. static ------------------------------------------------------------------------------
    tbl = syms["hk_sites"]
    # ota_loader_v4: the allow-list and the poke body are in stage-1 (the window 0x40176000, loaded by stage-0 from a
    # loader slot): read them from the stage-1 binary that the round trips seed into loader slot A
    s1bin = (STOCKFW / "cave/build/loader_s1.bin").read_bytes()
    view = bytearray(image)
    if 0x40176000 <= tbl < 0x40180000:
        view[0x40176000 - 0x40060000:0x40176000 - 0x40060000 + len(s1bin)] = s1bin
        elf = STOCKFW / "cave/build/loader_s1.elf"
    got = [(le(view, tbl - 0x40060000 + 12 * i), le(view, tbl - 0x40060000 + 12 * i + 4), view[tbl - 0x40060000 + 12 * i + 8])
           for i in range(len(SITES))]
    stock_w = [le(stock, s[0] - 0x40060000) for s in SITES]
    ok &= check(f"1 allow-list in the cave = the {NS} sites with their contexts; every original word is the stock image's "
                "`bl` (cond AL) to the documented target",
                got == [(s[0], s[1], s[3]) for s in SITES] and stock_w == STOCK_WORDS
                and all(bl_target(s[0], s[1]) == s[2] and s[1] >> 24 == 0xEB for s in SITES),
                str([(hex(a), hex(w), c) for a, w, c in got]))
    nxt = le(view, tbl - 0x40060000 + 12 * len(SITES))
    ok &= check("1 the resident's own USB hooks (engine tail 0x40143AE0, manufacturer branch 0x40147E1C) are not in it",
                all(a not in (0x40143AE0, 0x40147E1C) for a, _w, _c in got) and nxt not in (0x40143AE0, 0x40147E1C))
    ins = rtlib.instructions(elf)
    pbody = syms["ota_poke_body"]
    body = [(m, o.replace(" ", "")) for a, m, o in ins if pbody <= a < pbody + 0x24]
    ok &= check("1 poke body: str word -> [site], clean D line by MVA (c7,c10,1), drain WB (c7,c10,4), invalidate I line by "
                "MVA (c7,c5,1), return through *0x4009D000 - nothing else",
                [x for x in body[:8]] == [("str", "r5,[r4]"), ("mcr", "15,0,r4,cr7,cr10,{1}"), ("mov", "r0,#0"),
                                          ("mcr", "15,0,r0,cr7,cr10,{4}"), ("mcr", "15,0,r4,cr7,cr5,{1}"),
                                          ("ldr", "lr,[pc,#4]"), ("ldr", "lr,[lr]"), ("bx", "lr")], str(body))
    pn = Piano(image, syms)
    pn.boot()                             # stage-0 loads stage-1 (and its poke body) into the window
    pn.emu.unhook(ro.SVC_RET)             # the body alone, up to the stock return stub
    res = pn.emu.call(pbody, regs={"r4": 0x40FE0000, "r5": 0xEB123456}, until=ro.SVC_RET)
    ok &= check("1 the poke body runs on the ARM926 model (SVC mode, CP15 c7 line ops accepted): one store, ends at the SWI "
                "return stub", res.stop == "until" and [(w.addr, w.size) for w in res.writes] == [(0x40FE0000, 4)]
                and pn.emu.read_u32(0x40FE0000) == 0xEB123456, f"{res.stop} {res.writes}")
    pn.close()
    pay = hook_demo[64:]
    feat, attach = le(pay, 0x10), le(pay, 0x18)
    ok &= check(f"1 demo payload ({_p.name}): entry table + OTA_EF_ATTACH (feat bit 16), attach() and quiesce inside it, "
                "ABI 1, verifies",
                pay[:4] == b"C79E" and le(pay, 4) == 1 and feat & 0x10000 and feat & 0xFFFF == 0x0C01
                and RUN <= attach < RUN + plen and RUN <= le(pay, 0x14) < RUN + plen and not op.verify(hook_demo, ro.KEY),
                f"feat {feat:#x} attach {attach:#x}")
    pins = rtlib.instructions(pelf)
    stubs_ok = []
    for site, word, target, _c, name in SITES:
        a0 = psyms[name]
        st = [(m, o) for a, m, o in pins if a0 <= a < a0 + 0x1C]
        stubs_ok.append(len(st) == 7 and st[0][0] == "push" and st[5][0] == "pop" and st[0][1] == st[5][1] == "{r0}"
                        and st[6][0] == "b" and int(st[6][1].split()[0], 16) == target
                        and not any(m.startswith(("bl", "svc", "blx", "mcr", "msr")) for m, _o in st))
    ok &= check(f"1 demo stubs ({NS}): push {{r0}}; count via ip; pop {{r0}}; `b` = the stock target of the site - no bl / svc / "
                "privileged op, nothing that can block (kernel-timer safe, no payload frame survives)",
                all(stubs_ok), str(stubs_ok))

    # ---- 2. demo applied live ---------------------------------------------------------------------
    pn = Piano(image, syms)
    pn.boot()
    lo, hi = STOCK_CODE
    code0 = pn.emu.read_bytes(lo, hi - lo)
    base_regs = {}
    for name, body_ in (("kawai", bytes([0xF0, 0x40, 0x00, 0x10, 0x00, 0x08, 0x02, 0x00, 0x00, 0x00])),):
        seen = []
        pn.emu.hook(KAWAI[2], lambda e: seen.append([e.reg(i) for i in range(16)]))
        r_ = deliver(pn, 2, body_)
        pn.emu.unhook(KAWAI[2])
        base_regs[name] = seen
    pn.upload(0, hook_demo, seq=1)
    pn.run_job()
    pn.writes.clear()
    n_poke = len(pn.poke_calls)
    rc = apply(pn, T_A)
    st, n = run_apply(pn)
    hk = hooks(pn)
    dm = demo(pn)
    w = words(pn)
    want = bl_word(KAWAI[0], KAWAI[1], psyms["hk_kawai"])
    ok &= check(f"2 apply the demo (slot A) live -> attach() registers 0x40147E94: word = bl hk_kawai, the other {NS - 1} stock; "
                f"svc version {SVC_VER}",
                rc == 0 and st["ap_state"] == AP_DONE and st["active"] == ACT_A and w == [want] + STOCK_WORDS[1:]
                and dm is not None and dm["attach_rc"] == 0 and dm["svc_ver"] == SVC_VER, f"{[hex(x) for x in w]} demo {dm}")
    st = pn.status()
    pk = pn.poke_calls[n_poke:]
    ok &= check(f"2 exactly one poke (site, word) for it; status caps {CAPS:#04x} (bit 3 = runtime hooks"
                f"{', bit 4 = allow-list v3' if V3 else ''}), 1 hook, 0 refusals",
                [(a, v) for a, v, _l in pk] == [(KAWAI[0], want)] and st["caps"] == CAPS and st["hooks"] == 1
                and st["hk_refused"] == 0, str(pk))
    pages_ok = (hk is not None and hk["layout"] == 1 and hk["raw_len"] == 57) if not V3 else \
        (hk is not None and hk["layout"] == 2 and hk["pages"] == [(0, 7, 77), (7, 7, 77), (14, 3, 14 + 27)])
    ok &= check(f"2 op 0x16: {'57 B' if not V3 else 'layout 2, pages from 0 / 7 / 14 with 7 / 7 / 3 sites (77 / 77 / 41 B)'}, "
                f"{NS} sites, 1 hooked (state 0x11 = hooked, task 19), the others free with their context (1 task 19, "
                f"2 task 24, 3 kernel timer{', 4 task 20' if V3 else ''}), words as in RAM",
                pages_ok and hk["n"] == NS and hk["hooked"] == 1 and hk["sets"] == 1
                and [s_["site"] for s_ in hk["sites"]] == [s[0] for s in SITES]
                and [s_["state"] for s_ in hk["sites"]] == [0x11] + [s[3] << 4 for s in SITES[1:]]
                and [s_["word"] for s_ in hk["sites"]] == w and st["caps"] == CAPS,
                str(hk and (hk["pages"], hk["raw_len"])))
    id_ = identify(pn)
    ok &= check("2 the demo answers identify (feat0 0x01, feat1 0x0C)", id_ is not None and id_[3:5] == bytes([0x01, 0x0C]))

    # ---- 3. Kawai SysEx through the patched site --------------------------------------------------
    seen = []
    pn.emu.hook(KAWAI[2], lambda e: seen.append([e.reg(i) for i in range(16)]))
    results = []
    for k in range(4):
        r_ = deliver(pn, 2, bytes([0xF0, 0x40, 0x00, 0x10, 0x00, 0x08, 0x02, 0x00, 0x00, 0x00]))
        results.append((r_.stop, pn.emu.read_bytes(rt.STATE_BASE + 2, 1)))
    pn.emu.unhook(KAWAI[2])
    dm = demo(pn)
    b = base_regs["kawai"][0] if base_regs["kawai"] else None
    same = b is not None and len(seen) == 4 and all(s_[:12] == b[:12] and s_[13:15] == b[13:15] for s_ in seen)
    ok &= check("3 Kawai SysEx via the stock end handler -> stub -> FUN_4014A2C8 entered with r0-r11, sp and lr identical to "
                "the unhooked path (r0 port 2, r1 buffer, r2 len 11, lr 0x40147E98); only ip differs (AAPCS scratch)",
                same and b[0] == 2 and b[2] == 11 and b[14] == 0x40147E98,
                f"base r0-r2 {b[:3] if b else None} lr {b[14] if b else 0:#x}; hooked {[hex(x) for x in seen[0][:3]] if seen else None}")
    ok &= check("3 the end handler returns normally after FUN_4014A2C8 (state byte cleared by the stock epilogue), "
                "op 0x0B counts 4 messages, other counters 0",
                all(s_ == "returned" and st_ == b"\0" for s_, st_ in results) and dm["counts"] == [4] + [0] * (NS - 1),
                f"{results} {dm['counts']}")

    # ---- 4. every allowed site: register, run through, unregister -----------------------------------
    regs_in = [0x11111111 * (i + 1) & 0xFFFFFFFF for i in range(12)]
    rc0 = call_svc(pn, "ota_hook_clear", KAWAI[0])
    per = []
    for site, orig, target, ctx, name in SITES:
        stub = psyms[name]
        idx = [s[0] for s in SITES].index(site)
        cnt0 = demo(pn)["counts"][idx]
        np0 = len(pn.poke_calls)
        rc = call_svc(pn, "ota_hook_set", site, orig, stub, NOBLOCK if ctx == 3 else 0)
        wset = pn.emu.read_u32(site)
        r_hook, res_h = through(pn, site, target, regs_in)
        cnt1 = demo(pn)["counts"][idx]
        rc2 = call_svc(pn, "ota_hook_clear", site)
        wclr = pn.emu.read_u32(site)
        r_plain, res_p = through(pn, site, target, regs_in)
        cnt2 = demo(pn)["counts"][idx]
        good = (rc == 0 and wset == bl_word(site, orig, stub) and rc2 == 0 and wclr == orig
                and r_hook is not None and r_plain is not None and r_hook[:12] == regs_in and r_hook[13:15] == r_plain[13:15]
                and r_hook[14] == site + 4 and res_h.stop == res_p.stop == "until" and res_h.pc == site + 4
                and cnt1 == cnt0 + 1 and cnt2 == cnt1 and pn.poke_calls[np0:] == [(site, wset, pn.poke_calls[np0][2]),
                                                                                   (site, orig, pn.poke_calls[np0 + 1][2])])
        per.append((hex(site), good))
    ok &= check(f"4 each of the {NS} sites: set -> bl to the demo stub; the stock instruction at the site reaches the stub, which "
                "reaches the stock target with r0-r11 / sp intact and lr = site+4, and returns to site+4; clear -> stock word, "
                "path no longer counts; one poke each way",
                rc0 == 0 and all(g for _s, g in per), str(per))

    # ---- 5. refusals ------------------------------------------------------------------------------------
    stub_k = psyms["hk_kawai"]
    hk0 = hooks(pn)
    np0 = len(pn.poke_calls)
    w0 = words(pn)
    cases = []
    cases.append(("wrong original", call_svc(pn, "ota_hook_set", KAWAI[0], KAWAI[1] ^ 1, stub_k, 0), E["orig"]))
    for a in (0x40143AE0, 0x40147E1C, 0x400AA864, 0x40147E98, 0x40147E96, 0x401C0000) + \
            ((0x40143AD0, 0x40143AD4, 0x40148C9C, 0x4016DEB8) if V3 else ()):
        cases.append((f"site {a:#x}", call_svc(pn, "ota_hook_set", a, le(image, a - 0x40060000) if a & 3 == 0 and a < 0x40173A14 else 0, stub_k, 0), E["site"]))
    for t in (cave_lo, KAWAI[2], RUN + plen, RUN + plen + 0x100, stub_k + 2, 0x401E0000):
        cases.append((f"target {t:#x}", call_svc(pn, "ota_hook_set", KAWAI[0], KAWAI[1], t, 0), E["target"]))
    apo = SITES[4]
    cases.append(("kernel-timer site without NOBLOCK", call_svc(pn, "ota_hook_set", apo[0], apo[1], psyms["hk_apo"], 0), E["flags"]))
    cases.append(("unknown flag", call_svc(pn, "ota_hook_set", KAWAI[0], KAWAI[1], stub_k, 0x02), E["flags"]))
    cases.append(("clear, not hooked", call_svc(pn, "ota_hook_clear", KAWAI[0]), E["state"]))
    cases.append(("clear, not allow-listed", call_svc(pn, "ota_hook_clear", 0x40143AE0), E["site"]))
    cases.append(("outside attach/tick (open = 0)", call_svc(pn, "ota_hook_set", KAWAI[0], KAWAI[1], stub_k, 0, open_=False), E["ctx"]))
    task = pn.emu.read_bytes(CUR_TASK, 1)
    pn.emu.write_bytes(CUR_TASK, bytes([task[0] ^ 0x05]))
    cases.append(("another task (MIDI task 19 calling)", call_svc(pn, "ota_hook_set", KAWAI[0], KAWAI[1], stub_k, 0), E["ctx"]))
    pn.emu.write_bytes(CUR_TASK, task)
    pn.emu.write_bytes(syms["ota_gate"], b"\x01")
    cases.append(("gate closed (apply in progress)", call_svc(pn, "ota_hook_set", KAWAI[0], KAWAI[1], stub_k, 0), E["busy"]))
    pn.emu.write_bytes(syms["ota_gate"], b"\x00")
    pn.emu.write_u32(KAWAI[0], 0xE1A00000)               # someone else changed the word (never happens)
    cases.append(("RAM word is not the original", call_svc(pn, "ota_hook_set", KAWAI[0], KAWAI[1], stub_k, 0), E["orig"]))
    tampered = pn.emu.read_u32(KAWAI[0])
    pn.emu.write_u32(KAWAI[0], KAWAI[1])
    words_after = words(pn)
    np1 = len(pn.poke_calls)
    rc_a = call_svc(pn, "ota_hook_set", KAWAI[0], KAWAI[1], stub_k, 0)
    cases.append(("already hooked", call_svc(pn, "ota_hook_set", KAWAI[0], KAWAI[1], psyms["hk_midi"], 0), E["state"]))
    np2 = len(pn.poke_calls)
    hk1 = hooks(pn)
    bad = [(n_, rc_, w_) for n_, rc_, w_ in cases if rc_ != w_]
    ok &= check(f"5 {len(cases)} refusals with the right code: wrong original 3; sites 0x40143AE0 / 0x40147E1C / 0x400AA864 / "
                "0x40147E98 / misaligned / payload address"
                f"{' / v3 neighbours not listed (engine loop 0x40143AD0 / 0x40143AD4, alternate parser 0x40148C9C, task 20 0x4016DEB8)' if V3 else ''} 2; targets resident / stock / past the payload / misaligned / "
                "other R1 4; kernel timer without NOBLOCK, unknown flag 5; hooked / not hooked 6; open = 0, other task 1; "
                "gate closed 7; RAM word changed 3",
                not bad, str(bad))
    ok &= check("5 no refused call wrote anything: no poke, site words unchanged; refusal counter + one per case, last rc, anomaly +1",
                np1 == np0 and words_after == w0 and tampered == 0xE1A00000 and rc_a == 0 and np2 == np1 + 1
                and hk1["refused"] == hk0["refused"] + len(cases) and hk1["last"] == E["state"] and hk1["anomaly"] == hk0["anomaly"] + 1,
                f"refused {hk0['refused']} -> {hk1['refused']}, anomaly {hk0['anomaly']} -> {hk1['anomaly']}")
    pn.close()
    pn = Piano(image, syms)
    pn.boot()
    pn.emu.write_bytes(syms["ota_hk_open"], b"\x01")
    rc_b = pn.call(syms["ota_hook_set"], KAWAI[0], KAWAI[1], RUN + 0x1C, 0).r0
    ok &= check("5 built-in active (no slot payload) -> refused 1", rc_b == E["ctx"] and pn.emu.read_u32(KAWAI[0]) == KAWAI[1])
    pn.close()

    # ---- 6. apply to another payload removes the hooks first --------------------------------------------
    pn = boot_with(image, syms, {0: (hook_demo, 1), 1: (base2, 2)}, apply_to=T_A)
    for site, orig, target, ctx, name in SITES[1:]:
        call_svc(pn, "ota_hook_set", site, orig, psyms[name], NOBLOCK if ctx == 3 else 0)
    w_all = words(pn)
    ev = []
    pn.emu.hook(syms["ota_hook_poke"], lambda e: (ev.append(("poke", e.reg(0), e.reg(1), e.read_bytes(ro.DSP_BYTE, 1)[0],
                                                             pn.emu.read_u32(syms["active"]))), rt.se.StockEmu.CONTINUE)[1])
    pn.emu.hook(syms["ota_cache_sync"], lambda e: (ev.append(("sync", e.reg(0), e.reg(1), 0, 0)), rt.se.StockEmu.CONTINUE)[1])
    rc = apply(pn, T_B)
    st, n = run_apply(pn)
    pn.emu.unhook(syms["ota_hook_poke"])
    pn.emu.unhook(syms["ota_cache_sync"])
    id_ = identify(pn)
    pokes = [e for e in ev if e[0] == "poke"]
    ok &= check(f"6 {NS} hooks in place -> apply slot B (ota_base_v2): all {NS} restored to their stock words, each by one "
                "poke with dispatch disabled while the old payload was still the active cave, all before the copy's cache "
                "sync; B runs (identify feat1 0x05) and has no hooks",
                all(x == bl_word(s[0], s[1], psyms[s[4]]) for x, s in zip(w_all, SITES)) and rc == 0 and st["active"] == ACT_B
                and words(pn) == STOCK_WORDS and sorted((p[1], p[2]) for p in pokes) == sorted((s[0], s[1]) for s in SITES)
                and all(p[3] == 1 and p[4] == RUN for p in pokes) and ev[-1][0] == "sync" and len(ev) == NS + 1
                and id_ is not None and id_[4] == 0x05 and hooks(pn)["hooked"] == 0,
                f"events {[(e[0], hex(e[1])) for e in ev]}")
    rc = apply(pn, T_A)
    st, n = run_apply(pn)
    dm = demo(pn)
    ok &= check("6 back to the demo (slot A): attach() runs again on the fresh copy -> 0x40147E94 hooked, counters 0",
                rc == 0 and st["active"] == ACT_A and words(pn) == [bl_word(KAWAI[0], KAWAI[1], psyms["hk_kawai"])] + STOCK_WORDS[1:]
                and dm["attach_rc"] == 0 and dm["counts"] == [0] * NS)

    # ---- 7. built-in, quiesce refusal, SysEx in flight, escape -------------------------------------------
    q = psyms["demo_quiesce"]
    pn.emu.hook(q, lambda e: 1)
    apply(pn, T_BUILTIN)
    st, n = run_apply(pn)
    pn.emu.unhook(q)
    deliver(pn, 2, bytes([0xF0, 0x40, 0x00, 0x60]))
    dm = demo(pn)
    ok &= check("7 demo never goes idle -> apply refused (20); the demo keeps running WITH its hook (Kawai SysEx still counted)",
                st["ap_state"] == AP_ERROR and st["ap_err"] == 20 and st["active"] == ACT_A and words(pn)[0] != KAWAI[1]
                and dm["counts"][0] == 1)
    pn.emu.write_bytes(syms["ota_in_sysex"], b"\x01")
    apply(pn, T_BUILTIN)
    pn.tick(30)
    st = pn.status()
    w_wait = words(pn)
    pn.emu.write_bytes(syms["ota_in_sysex"], b"\x00")
    st2, _ = run_apply(pn)
    ok &= check("7 a call in flight (svc->inflight / 0x7D SysEx) -> no switch and the hook stays; then -> built-in, every "
                "stock word back, hook table empty",
                st["ap_state"] == AP_QUIESCE and w_wait[0] != KAWAI[1] and st2["ap_state"] == AP_DONE
                and st2["active"] == ACT_BUILTIN and words(pn) == STOCK_WORDS and hooks(pn)["hooked"] == 0
                and pn.status()["hooks"] == 0)
    nor_ab = bytes(pn.nor.mem)
    pn.close()
    pn = Piano(image, syms, nor_mem=nor_ab, mailbox=(0x55AAA55A, 0x40000007))
    st, _ = pn.boot()
    pn.tick(3)
    ok &= check("7 escape keys (E1) at boot with the demo in a slot -> pure stock: no stage-1, no attach, stock words",
                st is None and pn.emu.read_bytes(syms["s0_boot"] + 10, 1)[0] == 3 and words(pn) == STOCK_WORDS
                and hooks(pn) is None)
    pn.close()

    # ---- 8 / 9. stores, 100 x register / unregister ------------------------------------------------------
    pn = boot_with(image, syms, {0: (hook_demo, 1)})
    code0 = pn.emu.read_bytes(lo, hi - lo)
    ram_lo, ram_hi = rp.STOCK_RAM
    ram0 = pn.emu.read_bytes(ram_lo, ram_hi - ram_lo)
    pn.writes.clear()
    apply(pn, T_A)
    run_apply(pn)
    for site, orig, target, ctx, name in SITES[1:]:
        call_svc(pn, "ota_hook_set", site, orig, psyms[name], NOBLOCK if ctx == 3 else 0)
    code_h = pn.emu.read_bytes(lo, hi - lo)
    diff_h = sorted({(lo + i) & ~3 for i in range(len(code_h)) if code_h[i] != code0[i]})
    hk0 = hooks(pn)
    depths = []
    for i in range(100):
        pn.emu.write_bytes(0x40FF0000, b"\xCD" * 0x10000)
        for site, orig, target, ctx, name in SITES:
            call_svc(pn, "ota_hook_clear", site)
            call_svc(pn, "ota_hook_set", site, orig, psyms[name], NOBLOCK if ctx == 3 else 0)
        m = pn.emu.read_bytes(0x40FF0000, 0x10000)
        depths.append(0x10000 - next(j for j, b_ in enumerate(m) if b_ != 0xCD))
    hk1 = hooks(pn)
    w_100 = words(pn)
    apply(pn, T_BUILTIN)
    run_apply(pn)
    code1 = pn.emu.read_bytes(lo, hi - lo)
    ram1 = pn.emu.read_bytes(ram_lo, ram_hi - ram_lo)
    hk2 = hooks(pn)
    ok &= check(f"9 100 x (clear + set) on all {NS} sites: {200 * NS:,} calls all ok, table full the whole time, words = the "
                f"planned bl, set/clear counters +{100 * NS} each, no refusal, stack depth flat",
                hk1["hooked"] == NS and hk1["sets"] == hk0["sets"] + 100 * NS and hk1["clears"] == hk0["clears"] + 100 * NS
                and hk1["refused"] == hk0["refused"] and w_100 == [bl_word(s[0], s[1], psyms[s[4]]) for s in SITES]
                and len(set(depths)) == 1 and depths[0] <= 512, f"stack {min(depths)}-{max(depths)} B")
    ok &= check(f"8 while hooked, the stock code region differs from the boot copy only in the {NS} site words; after the "
                "switch to the built-in it is identical again (and the table empty)",
                diff_h == sorted(s[0] for s in SITES) and code1 == code0 and hk2["hooked"] == 0, str([hex(a) for a in diff_h]))
    data_lo = min(l for l, _h, sec, kind, _o in sb.layout(img.variant) if sec == ".cave" and kind == "data")
    s1_data = [(l, h) for l, h, sec, kind, _o in sb.layout("s1") if sec == ".cave" and kind == "data"] \
        if 0x40176000 <= syms["hk_sites"] < 0x40180000 else []        # ota_loader_v4: stage-1's .data in the window
    allowed = s1_data + [(data_lo, cave_end), (0x40180000, 0x401A0000), (STAGE, STAGE + 0x20000), (RUN, RUN + 0x20000),
               (ro.VBUF, ro.VBUF + 0x1000), ro.STACK, (ro.KERNEL_ERR, ro.KERNEL_ERR + 4), (0x40B90000, 0x40B90200)] + \
              [(s[0], s[0] + 4) for s in SITES]
    stray = [w_ for w_ in pn.writes if not any(a <= w_.addr < b_ for a, b_ in allowed)]
    site_w = [w_ for w_ in pn.writes if any(s[0] <= w_.addr < s[0] + 4 for s in SITES)]
    rdiff = [ram_lo + i for i in range(len(ram1)) if ram1[i] != ram0[i]] if ram1 != ram0 else []
    rdiff = [a for a in rdiff if not (ro.KERNEL_ERR <= a < ro.KERNEL_ERR + 4 or a == ro.DSP_BYTE)]
    ok &= check(f"8 {len(pn.writes)} stores of apply + attach + {NS - 1} sets + 100 x {NS} clear/set + apply back: only stage-0 / stage-1 .data, R1 OTA "
                f"areas, stack, the test buffer and the {NS} site words (each a single word store from the poke body); stock RAM "
                "0x41000000-0x41D00000 unchanged",
                not stray and not rdiff and site_w and all(w_.size == 4 and pbody <= w_.pc < pbody + 0x24 for w_ in site_w),
                f"stray {stray[0]}" if stray else (f"ram diff {[hex(a) for a in rdiff[:3]]}" if rdiff else f"{len(site_w)} site stores"))
    pn.close()

    if V3:
        ok &= v3_checks(image, syms, hook_demo, psyms)
    return rtlib.finish(ok, " - emulation only: no interrupts, no RTOS scheduling, no caches (the CP15 ops execute but do nothing).")


PARSER = 0x40148B24        # FUN_40148B24(port, byte): the stock MIDI byte parser every port pump calls (task 19)


def parse(pn, port, data, target):
    """Feed `data` byte by byte to the stock parser with the stock handler `target` replaced by a recorder that
    returns at once.  -> list of register snapshots at `target` (one per message)."""
    seen = []
    pn.emu.hook(target, lambda e: (seen.append([e.reg(i) for i in range(16)]), 0)[1])
    for b in data:
        pn.emu.call(PARSER, port, b, regs={f"r{i}": 0x01010101 * i for i in range(4, 12)}, max_insns=200_000)
    pn.emu.unhook(target)
    return seen


def v3_checks(image, syms, hook_demo, psyms):
    """11: the new sites from real stock paths, the task-20 in-flight flag."""
    ok = True
    by = {s[4]: s for s in SITES_V3}
    pn = boot_with(image, syms, {0: (hook_demo, 1)}, apply_to=T_A)
    idx = {s[4]: i for i, s in enumerate(SITES_V3)}
    msgs = (("hk_pc", 0, bytes([0xC0, 0x05]), "program change ch 1 -> 5"),
            ("hk_cc", 1, bytes([0xB3, 0x7A, 0x00]), "CC 0x7A local control off, ch 4"),
            ("hk_cc", 3, bytes([0xB0, 0x07, 0x64, 0x0A, 0x40]), "CC 7 volume + running-status CC 10"))
    res = []
    for name, port, data, what in msgs:
        site, orig, target, _c, _n = by[name]
        plain = parse(pn, port, data, target)
        c0 = demo(pn)["counts"][idx[name]]
        rc = call_svc(pn, "ota_hook_set", site, orig, psyms[name], 0)
        hooked = parse(pn, port, data, target)
        c1 = demo(pn)["counts"][idx[name]]
        rc2 = call_svc(pn, "ota_hook_clear", site)
        n_msg = len(plain)
        same = (n_msg >= 1 and len(hooked) == n_msg
                and all(h[:12] == q[:12] and h[13] == q[13] and h[14] == q[14] == site + 4 for h, q in zip(hooked, plain)))
        res.append((what, rc == 0 and rc2 == 0 and same and c1 == c0 + n_msg and pn.emu.read_u32(site) == orig,
                    f"{n_msg} msg, r0-r3 {[hex(x) for x in (plain[0][:4] if plain else [])]}"))
    ok &= check("11 channel messages through the stock byte parser FUN_40148B24 (program change -> 0x40148C4C, control change -> "
                "0x40148C60, running status too): hooked, each message reaches the stub once and the stock handler with r0-r11, "
                "sp and lr (= site + 4) identical to the unhooked run; cleared -> stock word",
                all(g for _w, g, _d in res), str(res))
    # task-20 in-flight flag holds an apply like the task-19 one
    t20 = by["hk_t20"]
    rc = call_svc(pn, "ota_hook_set", t20[0], t20[1], psyms["hk_t20"], 0)
    pn.emu.write_bytes(syms["ota_in_task20"], b"\x01")
    apply(pn, T_BUILTIN)
    pn.tick(30)
    st = pn.status()
    w_wait = pn.emu.read_u32(t20[0])
    pn.emu.write_bytes(syms["ota_in_task20"], b"\x00")
    st2, _ = run_apply(pn)
    ok &= check("11 svc->inflight20 (ota_in_task20) = 1 with a task-20 hook in place -> apply to the built-in waits (quiesce state, "
                "hook kept); 0 -> the switch, every stock word back",
                rc == 0 and st["ap_state"] == AP_QUIESCE and w_wait == bl_word(t20[0], t20[1], psyms["hk_t20"])
                and st2["ap_state"] == AP_DONE and st2["active"] == ACT_BUILTIN and words(pn) == [s[1] for s in SITES_V3],
                f"ap {st['ap_state']} -> {st2['ap_state']}")
    pn.close()
    return ok


if __name__ == "__main__":
    sys.exit(main(sys.argv))
