# roundtrip_canary_t1.py - the RAM canary op 0x20 of ota_main over the unreferenced SDRAM blocks T1 / T2
# (REMOTE_PROTOCOL §10.13; core result/firmware/src/system/ramcanary.c, port result/firmware/ports/cave/canary_cave.c).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_canary_t1.py [ota_main] [-q] [--loader=ota_loader_v4]
#
# Same machine as roundtrip_gpio.py (roundtrip_power's Rig: the real resident image on StockEmu with the NOR model, the
# payload uploaded over the 0x7D OTA ops and applied live; the stock binding byte 0x413CA3DE makes the app port 2
# bound).  Every engine-loop pass is one emu.call of cave_tick; its payload stores and instruction count are looked at
# per pass (not kept: a full fill is ~2 M stores).
# Checks (numbered as printed):
#   1  static: identify = the image table's bytes with ngver 4; cnc_tick / cnc_sysex / rcn_step in the payload; the
#      region table in the payload = the eight blocks with a 4 KB guard at each edge, none overlapping R1
#      (0x40173A14-0x401FFFFF) or each other; a request sends nothing at once (answered from the engine loop)
#   2  nothing before an arm: 20 / 20 00 -> phase 0, nothing armed; 40 passes make no payload store in any region
#   3  arm rules: 20 01 03 00 from the unbound panel -> rc 2, nothing armed; 20 01 00 00 and 20 01 00 02 (region 8)
#      -> rc 1; from the bound app -> 60 01 00 03 00
#   4  fill A + B: complete in ceil(words / 2048) passes (read one pass later), at most 2048 words a pass; every payload store in those
#      passes lands in A / B / payload .data / core RW / the stack; both blocks hold addr ^ 0xC79A5A5C word for word;
#      the 4 KB guards and the six other blocks unchanged
#   5  verify: both blocks swept at least once with 0 changed words, status dirty 0; the verify passes make no store
#      in any block
#   6  injected changes (three words of B, one of A, between passes) -> region records: changed_last 3 / 1, first
#      address + value, min / max address; the changed words keep the injected values (never written back); one
#      put back -> changed_last 2, changed_max stays 3
#   7  bad forms: 20 02 08, 20 04, 20 01 03 (no high byte), 20 03 00 -> silent; 20 02 07 -> T2-c record, not armed
#   8  disarm 20 03 from the unbound panel -> 60 03 00 00 00; then no payload store in any block and the pass
#      cost back to the idle cost; the records stay (B changed_max 3)
#   9  re-arm C + E (two small blocks): fill in ceil(words / 2048) passes, A / B untouched; arm all eight -> rc 0 mask
#      7F 01; disarm
#  10  per-pass cost (instructions of one cave_tick): idle, fill, verify, after disarm (printed for the record); fill
#      <= 6 and verify <= 8 instructions a word of the 2048-word budget (the hot loops) + 4096 a pass
from __future__ import annotations

import array
import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rpw = rtlib.load("roundtrip_power")
rh, rp, ro, images = rpw.rh, rpw.rp, rpw.ro, rpw.images
check, le = rtlib.check, rtlib.le
RUN = ro.RUN

OP = 0x20
BINDING = rpw.BINDING
APP, PANEL = 2, 0
XOR = 0xC79A5A5C
BUDGET = 2048
G = 0x1000
# the hardware baseline study's blocks (§2.2) and the 4 KB guard at each edge
BLOCKS = [("T1-A", 0x40600000, 0x40A00000), ("T1-B", 0x41943000, 0x41D00000), ("T1-C", 0x403C0000, 0x40400000),
          ("T1-D", 0x40001000, 0x40060000), ("T1-E", 0x400B4000, 0x400C0000), ("T2-a", 0x40C00000, 0x40E00000),
          ("T2-b", 0x41D00000, 0x41EF8000), ("T2-c", 0x41EFE000, 0x42000000)]
REGIONS = [(n, lo + G, hi - G) for n, lo, hi in BLOCKS]
R1 = (0x40173A14, 0x40200000)
CORE_RW = (0x40180000, 0x401A0000)


def in_payload(pc):
    return RUN <= pc < RUN + 0x20000


def words(i):
    return (REGIONS[i][2] - REGIONS[i][1]) // 4


def pattern(lo, hi):
    a = array.array("I", range(lo, hi, 4))
    return array.array("I", (x ^ XOR for x in a)).tobytes()


class Probe:
    """A Rig with the payload applied live and the app bound.  pass_() runs one engine-loop pass and keeps only what
    the checks need: frames sent, payload stores per region (count), stray payload stores, instruction count."""

    def __init__(self, image, syms, data):
        self.rig = rig = rpw.Rig(image, syms)
        rpw.upload_apply(rig, data)
        rig.emu.write_bytes(BINDING, bytes([APP + 1]))
        self.tick_addr = rig.pn.syms["cave_tick"]
        self.ok_ranges = [(RUN, RUN + 0x20000), CORE_RW, ro.STACK]
        self.reset_counts()
        self.pass_(2)

    def reset_counts(self):
        self.region_stores = [0] * len(REGIONS)
        self.stray = []
        self.max_pass_stores = 0
        self.insns = []

    def pass_(self, n=1, ms=8):
        rig = self.rig
        out = []
        for _ in range(n):
            rig.clock(ms)
            res = rig.emu.call(self.tick_addr, max_insns=60_000_000)
            if not res.ok:
                raise RuntimeError(f"cave_tick stopped: {res}")
            self.insns.append(res.insns)
            k = 0
            for w in res.writes:
                if not in_payload(w.pc):
                    continue
                a = w.addr
                hit = next((i for i, (_n, lo, hi) in enumerate(REGIONS) if lo <= a < hi), None)
                if hit is not None:
                    self.region_stores[hit] += 1
                    k += 1
                elif not any(lo <= a < hi for lo, hi in self.ok_ranges):
                    self.stray.append((a, w.pc))
            self.max_pass_stores = max(self.max_pass_stores, k)
            out += list(rig.pn.sent)
            rig.pn.sent.clear()
        return out

    def ask(self, port, body):
        direct = self.rig.pn.sysex(port, body)
        return direct, self.pass_()

    def reply(self, port, body, sub):
        direct, later = self.ask(port, body)
        mine = [m for p, m in later if p == port and m[6] == OP | 0x40 and m[7] == sub]
        return direct, (mine[0][8:-1] if len(mine) == 1 else None), later

    def status(self, port=APP):
        _d, r, _l = self.reply(port, rp.req(OP, 0x00), 0x00)
        d = rp.unpack7(r) if r is not None else b""
        if len(d) != 32:
            return None
        return dict(layout=d[0], n=d[1], phase=d[2], armed=le(d, 4, 2), filled=le(d, 6, 2), dirty=le(d, 8, 2),
                    budget=le(d, 10, 2), passes=le(d, 12), fw=le(d, 16), vw=le(d, 20), last_us=le(d, 24), max_us=le(d, 28))

    def region(self, i, port=APP):
        _d, r, _l = self.reply(port, rp.req(OP, 0x02, i), 0x02)
        if r is None or r[0] != i:
            return None
        d = rp.unpack7(r[1:])
        if len(d) != 48:
            return None
        return dict(state=d[0], armed=d[1], lo=le(d, 4), hi=le(d, 8), cur=le(d, 12), sweeps=le(d, 16), last=le(d, 20),
                    max=le(d, 24), min_a=le(d, 28), max_a=le(d, 32), first_a=le(d, 36), first_v=le(d, 40),
                    first_s=le(d, 44))

    def arm(self, port, lo7, hi7):
        _d, r, _l = self.reply(port, rp.req(OP, 0x01, lo7, hi7), 0x01)
        return list(r) if r is not None else None

    def close(self):
        self.rig.close()


def main(argv):
    name = rtlib.first_arg(argv, "ota_main")
    p, data, pay, plen, pelf, psyms = rtlib.payload(name)
    resident = next((a.split("=", 1)[1] for a in argv[1:] if a.startswith("--loader=")), rtlib.DEFAULT_LOADER)
    _img, _stock, _man, image, syms = rtlib.loader(resident)
    ok = True
    print(f"payload {name}  {plen} B  sha256 {ro.pa.sha256(data)}  on {resident}")

    # ---- 1. static ------------------------------------------------------------------------------------
    pr = Probe(image, syms, data)
    emu = pr.rig.emu
    id_ = rh.identify(pr.rig.pn)
    want_id = bytes([p.ngver, 2, 19, p.feat0, p.feat1] + ([p.feat2] if p.feat2 or p.feat3 else []) + ([p.feat3] if p.feat3 else []))
    syms_in = all(n in psyms and in_payload(psyms[n]) for n in ("cnc_tick", "cnc_sysex", "rcn_step", "rcn_arm"))
    tab = psyms.get("CNC_TAB")
    got_tab = [(le(emu.read_bytes(tab + 12 * i, 12), 0), le(emu.read_bytes(tab + 12 * i, 12), 4),
                le(emu.read_bytes(tab + 12 * i, 12), 8)) for i in range(8)] if tab else []
    want_tab = [(lo, hi, lo) for _n, lo, hi in REGIONS]
    spans = sorted([(lo - G, hi + G) for _n, lo, hi in REGIONS] + [R1])
    disjoint = all(a[1] <= b[0] for a, b in zip(spans, spans[1:]))
    direct, _r, _l = pr.reply(PANEL, rp.req(OP), 0x00)
    ok &= check(f"1 identify {want_id.hex(' ').upper()} (ngver {p.ngver} >= 4); cnc_tick / cnc_sysex / rcn_step / rcn_arm in "
                "the payload; CNC_TAB = the eight blocks with a 4 KB guard at each edge (lo, hi, base = lo), disjoint from "
                "each other and from R1; a request sends nothing at once",
                id_ == want_id and p.ngver >= 4 and syms_in and got_tab == want_tab and disjoint and direct == [],
                f"{id_ and id_.hex()} {syms_in} tab {[tuple(hex(x) for x in t) for t in got_tab] if got_tab != want_tab else 'ok'} "
                f"disjoint {disjoint} {direct}")

    # ---- 2. nothing before an arm -------------------------------------------------------------------------
    pr.reset_counts()
    st0 = pr.status(PANEL)
    pr.pass_(40)
    idle = sorted(pr.insns)[len(pr.insns) // 2]
    ok &= check("2 before an arm: 20 00 -> layout 1, 8 regions, phase 0, nothing armed / filled / dirty, budget 2048; 40 more "
                "passes: no payload store in any region",
                st0 is not None and (st0["layout"], st0["n"], st0["phase"], st0["armed"], st0["filled"], st0["dirty"],
                                     st0["budget"]) == (1, 8, 0, 0, 0, 0, BUDGET)
                and sum(pr.region_stores) == 0 and not pr.stray, f"{st0} {pr.region_stores} {pr.stray[:3]}")

    # ---- 3. arm rules ---------------------------------------------------------------------------------------
    before = {i: emu.read_bytes(REGIONS[i][1], 64) for i in range(8)}
    pr.reset_counts()
    r_unb = pr.arm(PANEL, 0x03, 0x00)
    r_zero = pr.arm(APP, 0x00, 0x00)
    r_r8 = pr.arm(APP, 0x00, 0x02)
    st_n = pr.status()
    untouched = sum(pr.region_stores) == 0 and all(emu.read_bytes(REGIONS[i][1], 64) == before[i] for i in range(8))
    ok &= check("3 20 01 03 00 from the unbound panel -> 60 01 02 00 00; 20 01 00 00 and 20 01 00 02 (region 8) -> rc 1; "
                "nothing armed, nothing written",
                r_unb == [2, 0, 0] and r_zero == [1, 0, 0] and r_r8 == [1, 0, 0] and st_n and st_n["armed"] == 0 and untouched,
                f"{r_unb} {r_zero} {r_r8} {st_n and st_n['armed']} {untouched}")

    # ---- 4. fill A + B -----------------------------------------------------------------------------------
    edges = {i: (emu.read_bytes(REGIONS[i][1] - G, G), emu.read_bytes(REGIONS[i][2], G)) for i in range(8)}
    others = {i: emu.read_bytes(REGIONS[i][1], min(0x10000, REGIONS[i][2] - REGIONS[i][1])) for i in range(2, 8)}
    pr.reset_counts()
    r_arm = pr.arm(APP, 0x03, 0x00)                 # the arm pass already fills its first 2048 words
    need = -(-(words(0) + words(1)) // BUDGET)
    n = 1
    st = None
    while n < need + 5:
        pr.pass_()
        n += 1
        st = pr.status()                            # (this read is one more pass: its step counts too)
        n += 1
        if st and st["filled"] == 0x03:
            break
    fill_passes = st["passes"] if st else -1
    fill_insns = sorted(pr.insns)[len(pr.insns) // 2]
    full = all(emu.read_bytes(REGIONS[i][1], REGIONS[i][2] - REGIONS[i][1]) == pattern(REGIONS[i][1], REGIONS[i][2])
               for i in (0, 1))
    edges_ok = all((emu.read_bytes(REGIONS[i][1] - G, G), emu.read_bytes(REGIONS[i][2], G)) == edges[i] for i in range(8))
    others_ok = all(emu.read_bytes(REGIONS[i][1], len(v)) == v for i, v in others.items())
    stores_ok = pr.region_stores[0] == words(0) and pr.region_stores[1] == words(1) and sum(pr.region_stores[2:]) == 0
    ok &= check(f"4 arm A + B from the bound app -> 60 01 00 03 00; filled ({words(0) + words(1)} words) seen after {fill_passes} "
                f"passes = ceil(words / {BUDGET}) = {need} (+1: the status read is a pass of its own); <= {BUDGET} region stores a pass, each word stored once, nothing "
                "stray (payload .data / core RW / stack only); both blocks = addr ^ 0xC79A5A5C; the 4 KB guards and the "
                "six other blocks unchanged",
                r_arm == [0, 3, 0] and st is not None and fill_passes in (need, need + 1) and st['fw'] == words(0) + words(1) and pr.max_pass_stores <= BUDGET and stores_ok
                and not pr.stray and full and edges_ok and others_ok,
                f"{r_arm} {st} max/pass {pr.max_pass_stores} stores {pr.region_stores} stray {pr.stray[:3]} full {full} "
                f"edges {edges_ok} others {others_ok}")

    # ---- 5. verify ----------------------------------------------------------------------------------------
    pr.reset_counts()
    for _ in range(need + 4):
        pr.pass_()
    verify_insns = sorted(pr.insns)[len(pr.insns) // 2]
    ra, rb, st = pr.region(0), pr.region(1), pr.status()
    ok &= check("5 verify: A and B swept at least once with 0 changed words (state 2, no first address), status phase 2 / "
                "dirty 0, verified words >= both blocks; no payload store in any block while verifying",
                ra and rb and st and ra["state"] == rb["state"] == 2 and ra["sweeps"] >= 1 and rb["sweeps"] >= 1
                and ra["last"] == rb["last"] == ra["max"] == rb["max"] == 0 and ra["first_s"] == rb["first_s"] == 0
                and st["phase"] == 2 and st["dirty"] == 0 and st["vw"] >= words(0) + words(1)
                and sum(pr.region_stores) == 0 and not pr.stray,
                f"{ra} {rb} {st} {pr.region_stores}")

    # ---- 6. injected changes ------------------------------------------------------------------------------
    b0, a0 = REGIONS[1][1], REGIONS[0][1]
    inj = {b0 + 0x100: 0x12345678, b0 + 0x104: 0, b0 + 0x20000: (b0 + 0x100) ^ XOR, a0 + 4 * (words(0) - 1): 0xDEADBEEF}
    for a, v in inj.items():
        emu.write_u32(a, v)
    s_a, s_b = ra["sweeps"], rb["sweeps"]
    for _ in range(3 * need):
        pr.pass_()
        if (pr.insns and len(pr.insns) % 64 == 0):
            ra, rb = pr.region(0), pr.region(1)
            if ra["sweeps"] >= s_a + 2 and rb["sweeps"] >= s_b + 2:
                break
    ra, rb, st = pr.region(0), pr.region(1), pr.status()
    kept = all(emu.read_u32(a) == v for a, v in inj.items())
    ok &= check("6 injected three words of B (one = a pattern word moved 128 KB) and A's last word -> B changed_last 3, max 3, "
                f"first {b0 + 0x100:#x} = 0x12345678, min {b0 + 0x100:#x} / max {b0 + 0x20000:#x}; A changed_last 1 at "
                f"{a0 + 4 * (words(0) - 1):#x} = 0xDEADBEEF; status dirty 0x03; the changed words keep the injected values",
                rb["last"] == 3 and rb["max"] == 3 and rb["first_a"] == b0 + 0x100 and rb["first_v"] == 0x12345678
                and rb["min_a"] == b0 + 0x100 and rb["max_a"] == b0 + 0x20000 and rb["first_s"] in (s_b + 1, s_b + 2)
                and ra["last"] == 1 and ra["first_a"] == a0 + 4 * (words(0) - 1) and ra["first_v"] == 0xDEADBEEF
                and st["dirty"] == 0x03 and kept,
                f"A {ra} B {rb} dirty {st['dirty']} kept {kept}")
    emu.write_u32(b0 + 0x104, (b0 + 0x104) ^ XOR)
    s_b = rb["sweeps"]
    for _ in range(3 * need):
        pr.pass_()
        if len(pr.insns) % 64 == 0 and pr.region(1)["sweeps"] >= s_b + 2:
            break
    rb = pr.region(1)
    ok &= check("6 one word put back -> B changed_last 2, changed_max stays 3, first address / value unchanged",
                rb["last"] == 2 and rb["max"] == 3 and rb["first_a"] == b0 + 0x100 and rb["first_v"] == 0x12345678, f"{rb}")

    # ---- 7. bad forms ---------------------------------------------------------------------------------------
    bad = []
    for body in (rp.req(OP, 0x02, 0x08), rp.req(OP, 0x04), rp.req(OP, 0x01, 0x03), rp.req(OP, 0x03, 0x00)):
        direct, later = pr.ask(APP, body)
        bad.append(direct + [m for _p, m in later if m[6] == OP | 0x40])
    r7 = pr.region(7)
    ok &= check("7 20 02 08 / 20 04 / 20 01 03 (no high byte) / 20 03 00 -> silent; 20 02 07 -> the T2-c record, not armed "
                f"({REGIONS[7][1]:#x}-{REGIONS[7][2]:#x})",
                bad == [[], [], [], []] and r7 and (r7["state"], r7["armed"], r7["lo"], r7["hi"]) == (0, 0, REGIONS[7][1], REGIONS[7][2]),
                f"{bad} {r7}")

    # ---- 8. disarm ----------------------------------------------------------------------------------------
    _d, r_off, _l = pr.reply(PANEL, rp.req(OP, 0x03), 0x03)
    pr.reset_counts()
    pr.pass_(20)
    after_insns = sorted(pr.insns)[len(pr.insns) // 2]
    st, rb = pr.status(), pr.region(1)
    ok &= check("8 20 03 from the unbound panel -> 60 03 00 00 00; 20 passes after: no payload store in any block, the pass "
                f"cost back to idle ({after_insns} vs {idle} instructions); status phase 0 / armed 0; B's record stays "
                "(changed_max 3, state 0)",
                r_off is not None and list(r_off) == [0, 0, 0] and sum(pr.region_stores) == 0 and not pr.stray
                and abs(after_insns - idle) <= 64 and st["phase"] == 0 and st["armed"] == 0 and rb["max"] == 3
                and rb["state"] == 0,
                f"{r_off and list(r_off)} {pr.region_stores} insns {after_insns}/{idle} {st} {rb}")

    # ---- 9. re-arm two small blocks, then all eight -------------------------------------------------------
    a_snap = emu.read_bytes(REGIONS[0][1], 0x1000)
    pr.reset_counts()
    r_ce = pr.arm(APP, 0x14, 0x00)
    need_ce = -(-(words(2) + words(4)) // BUDGET)
    for _ in range(need_ce + 4):
        pr.pass_()
    st = pr.status()
    full_ce = all(emu.read_bytes(REGIONS[i][1], REGIONS[i][2] - REGIONS[i][1]) == pattern(REGIONS[i][1], REGIONS[i][2])
                  for i in (2, 4))
    ok &= check(f"9 arm C + E (20 01 14 00) -> rc 0; filled after {st['passes'] if st else '?'} passes "
                f"(words {words(2) + words(4)}, bound {need_ce + 5} incl. the reads); both = the pattern; only C / E stored, A unchanged",
                r_ce == [0, 0x14, 0] and st and st["filled"] == 0x14 and full_ce and pr.region_stores[2] == words(2)
                and pr.region_stores[4] == words(4) and sum(pr.region_stores) == words(2) + words(4)
                and emu.read_bytes(REGIONS[0][1], 0x1000) == a_snap and not pr.stray,
                f"{r_ce} {st} {pr.region_stores} {full_ce}")
    r_all = pr.arm(APP, 0x7F, 0x01)
    st_all = pr.status()
    _d, r_off2, _l = pr.reply(APP, rp.req(OP, 0x03), 0x03)
    ok &= check("9 arm all eight (20 01 7F 01) -> 60 01 00 7F 01, status armed 0xFF phase 1; disarm -> 60 03 00 00 00",
                r_all == [0, 0x7F, 0x01] and st_all and st_all["armed"] == 0xFF and st_all["phase"] == 1
                and r_off2 is not None and list(r_off2) == [0, 0, 0], f"{r_all} {st_all} {r_off2 and list(r_off2)}")

    # ---- 10. per-pass cost ----------------------------------------------------------------------------------
    print(f"     per-pass cost (instructions of one cave_tick, median): idle {idle}, fill {fill_insns} "
          f"(+{fill_insns - idle}), verify {verify_insns} (+{verify_insns - idle}), after disarm {after_insns}; "
          f"{BUDGET} words a pass")
    fix = 4096   # per pass: the step's set-up, the region switch, the two hal_time_us reads
    ok &= check(f"10 per-pass cost bounded by the budget: fill +{fill_insns - idle} <= 6 instructions a word + {fix} "
                f"({6 * BUDGET + fix}), verify +{verify_insns - idle} <= 8 a word + {fix} ({8 * BUDGET + fix})",
                0 < fill_insns - idle <= 6 * BUDGET + fix and 0 < verify_insns - idle <= 8 * BUDGET + fix)
    pr.close()
    return rtlib.finish(ok)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
