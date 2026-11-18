# roundtrip_gpio.py - the read-only GPIO diagnostic op 0x17 of ota_main (REMOTE_PROTOCOL §10.11; core result/firmware/src/system/gpiodiag.c, port result/firmware/ports/cave/gpio_cave.c).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_gpio.py [ota_main] [-q] [--loader=ota_loader_v4]
#
# Same machine as roundtrip_power.py (its Rig: the real resident image on StockEmu with the NOR model, the payload
# uploaded over the 0x7D OTA ops and applied live; the stock binding byte 0x413CA3DE makes the app port 2 bound).
# The GPIO / SYSCFG windows are emulator stubs: every value a test puts there (emu.periph, the NOR model's IN_DATA23
# and OUT_DATA67 latches) must come back in the record, and every access the payload makes there is logged.
# Checks (numbered as printed):
#   1  static: identify = the image table's feature bytes (no new bit); gdc_tick / gdc_sysex / gd_* in the payload;
#      the op is answered from the engine loop (a request alone sends nothing)
#   2  snapshot 17 / 17 00 from the unbound panel port: 57 00 + 130 B - DIR / OUT / IN of the pairs 23 / 45 / 67, the
#      five stock bytes, PINMUX0..19 exactly as put; the pass that answers reads exactly those 9 + 20 MMIO words in the
#      GPIO / SYSCFG windows
#   3  change log 17 01: the first pass is the baseline; headphones in (IN_DATA45 bit 30), GP3[8] high (IN_DATA23 bit
#      24), amp off + mute (OUT_DATA67 0x8800 -> 0) -> 4 events in signal order with their pass time; the NOR window bits
#      GP6[10:0] and RY/BY# GP2[10] make none
#   4  notices: 17 02 01 from the unbound panel -> 57 02 02; from the bound app -> 57 02 00 03 02; a GP3[8] edge -> one
#      57 7F to port 2 on that pass (n 1, signal 0, level); edges on the next passes wait GDC_NOTICE_MS (40 ms) and go
#      as one notice (n <= 4); 17 02 00 from the panel -> off, no notice; on again, the app unbound -> no frame, off
#   5  bad forms: 17 03, 17 02 (no argument), 17 01 00 -> silent; 17 02 05 -> rc 1
#   6  read only: in checks 2-5 every payload store lands in payload .data / core RW / the stack, and the payload makes
#      no write at all in the GPIO (0x01E26000-0x01E260FF) and SYSCFG (0x01C14000-0x01C14FFF) windows
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

OP = 0x17
BINDING = rpw.BINDING
APP, PANEL = 2, 0
GPIO = 0x01E26000
DIR = {23: GPIO + 0x38, 45: GPIO + 0x60, 67: GPIO + 0x88}
OUT = {23: GPIO + 0x3C, 45: GPIO + 0x64, 67: GPIO + 0x8C}
IN = {23: GPIO + 0x48, 45: GPIO + 0x70, 67: GPIO + 0x98}
PINMUX0 = 0x01C14120
STOCK_BYTES = {0x4139E7FA: 1, 0x4139E7FF: 0, 0x4139E7FD: 0, 0x4139E80A: 0x21, 0x4139E80B: 0xDD}
GPIO_WIN, SYSCFG_WIN = (GPIO, GPIO + 0x100), (0x01C14000, 0x01C15000)
NOTICE_MS = 40


def in_payload(pc):
    return RUN <= pc < RUN + 0x20000


class Probe:
    """A Rig with the payload applied live and the app bound; logs every payload store / MMIO access."""

    def __init__(self, image, syms, data):
        self.rig = rig = rpw.Rig(image, syms)
        rpw.upload_apply(rig, data)
        rig.emu.write_bytes(BINDING, bytes([APP + 1]))
        self.stores, self.mmio = [], []
        self.pass_mmio = []
        self.tick(2)

    def tick(self, n=1, ms=8):
        rig = self.rig
        out = []
        for _ in range(n):
            rig.clock(ms)
            res = rig.pn.call(rig.pn.syms["cave_tick"])
            self.stores += [w for w in res.writes if in_payload(w.pc)]
            self.pass_mmio = [a for a in res.periph if in_payload(a.pc)]
            self.mmio += self.pass_mmio
            out += list(rig.pn.sent)
            rig.pn.sent.clear()
        return out

    def ask(self, port, body):
        """The frames the request sends at once (none expected) and those of the next pass."""
        direct = self.rig.pn.sysex(port, body)
        return direct, self.tick()

    def reply(self, port, body, sub):
        direct, later = self.ask(port, body)
        mine = [m for p, m in later if p == port and m[6] == OP | 0x40 and m[7] == sub]
        return direct, (mine[0][8:-1] if len(mine) == 1 else None), later

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
    emu, nor = pr.rig.emu, pr.rig.pn.nor
    id_ = rh.identify(pr.rig.pn)
    want_id = bytes([p.ngver, 2, 19, p.feat0, p.feat1] + ([p.feat2] if p.feat2 or p.feat3 else []) + ([p.feat3] if p.feat3 else []))
    syms_in = all(n in psyms and in_payload(psyms[n]) for n in ("gdc_tick", "gdc_sysex", "gd_sample", "gd_encode_snapshot"))
    direct, _r, _l = pr.reply(PANEL, rp.req(OP), 0x00)
    ok &= check(f"1 identify {want_id.hex(' ').upper()} (the image table's bytes - no new feature bit); gdc_tick / gdc_sysex / "
                "gd_sample / gd_encode_snapshot in the payload; a request sends nothing at once (answered from the engine loop)",
                id_ == want_id and syms_in and direct == [], f"{id_ and id_.hex()} {syms_in} {direct}")

    # ---- 2. snapshot ----------------------------------------------------------------------------------
    regs = {DIR[23]: 0xFB2BBC00, OUT[23]: 0x04440000, DIR[45]: 0x51A94F60, OUT[45]: 0x8000A000, IN[45]: 0x40000000,
            DIR[67]: 0x88F80000, IN[67]: 0x1234ABCD}
    for a, v in regs.items():
        emu.periph[a] = v
    nor.in_data23 = (1 << 17) | (1 << 14)            # power switch released, a stray bit
    nor.gpio67 = (nor.gpio67 & 0x7FF) | 0x10008800   # amp on, unmuted, GP7[12] high; the NOR window kept
    for a, v in STOCK_BYTES.items():
        emu.write_bytes(a, bytes([v]))
    mux = [(0x01010101 * i) ^ 0x80000000 * (i & 1) for i in range(20)]
    for i, v in enumerate(mux):
        emu.periph[PINMUX0 + 4 * i] = v
    rows = []
    for body in (rp.req(OP), rp.req(OP, 0x00)):
        _d, r, later = pr.reply(PANEL, body, 0x00)
        d = rp.unpack7(r) if r is not None else b""
        rows.append((d, later, list(pr.pass_mmio)))
    d, later, mm = rows[0]
    in23 = nor.ryby_word()
    want = {5: regs[DIR[23]], 9: regs[OUT[23]], 13: in23, 17: regs[DIR[45]], 21: regs[OUT[45]], 25: regs[IN[45]],
            29: regs[DIR[67]], 33: nor.gpio67, 37: regs[IN[67]]}
    got = {o: le(d, o) for o in want} if len(d) == 130 else {}
    bytes_ok = len(d) == 130 and list(d[41:46]) == list(STOCK_BYTES.values())
    mux_ok = len(d) == 130 and [le(d, 46 + 4 * i) for i in range(20)] == mux
    reads = sorted({a.addr for a in mm if a.op == "R" and (GPIO_WIN[0] <= a.addr < GPIO_WIN[1] or SYSCFG_WIN[0] <= a.addr < SYSCFG_WIN[1])})
    want_reads = sorted(set(DIR.values()) | set(OUT.values()) | set(IN.values()) | {PINMUX0 + 4 * i for i in range(20)})
    ok &= check("2 17 / 17 00 from the unbound panel port -> 57 00 + 130 B: layout 1, DIR / OUT / IN of the pairs 23 / 45 / 67, "
                "the stock bytes 0x4139E7FA / E7FF / E7FD / E80A / E80B and PINMUX0..19 exactly as put, no notice port (0x7F); "
                "the answering pass reads exactly the 9 GPIO words + 20 PINMUX words in those windows",
                got == want and bytes_ok and mux_ok and d[0] == 1 and d[2] == 0x7F and rows[1][0][5:126] == d[5:126]
                and [p_ for p_, _m in later] == [PANEL] and reads == want_reads,
                f"{ {hex(o): (hex(got.get(o, 0)), hex(v)) for o, v in want.items() if got.get(o) != v} } bytes {bytes_ok} "
                f"mux {mux_ok} reads {len(reads)}/{len(want_reads)} {[hex(a) for a in set(reads) ^ set(want_reads)]}")

    # ---- 3. change log ----------------------------------------------------------------------------------
    t0 = le(d, 126)
    _d, r, _l = pr.reply(APP, rp.req(OP, 0x01), 0x01)
    log0 = rp.unpack7(r) if r is not None else b""
    seq0 = le(log0, 2, 2) if len(log0) >= 4 else -1
    emu.periph[IN[45]] = 0                     # headphones out first (GP5[14] 1 -> 0 at the regs above)
    pr.tick(1)
    emu.periph[IN[45]] = 1 << 30               # in again
    nor.in_data23 |= 1 << 24                   # GP3[8] high
    pr.tick(1)
    nor.gpio67 &= ~0x8800                      # the stock's answer: amp off, mute
    nor.gpio67 ^= 0x155                        # the NOR window bits move too: no event
    nor.in_data23 ^= 0x400                     # GP2[10] is RY/BY# (the model ORs it in anyway)
    pr.tick(1)
    _d, r, _l = pr.reply(APP, rp.req(OP, 0x01), 0x01)
    log = rp.unpack7(r) if r is not None else b""
    ev = [(le(log, 4 + 8 * i, 2), le(log, 6 + 8 * i), log[10 + 8 * i], log[11 + 8 * i]) for i in range(log[1])] if len(log) >= 4 else []
    new = [e for e in ev if e[0] > seq0]
    sig = [(s, lv) for _q, _t, s, lv in new]
    times = [t for _q, t, _s, _l in new]
    ok &= check("3 17 01 log: headphones out / in (signal 2), GP3[8] high (signal 0), amp off + mute (3, 4) -> 5 events in pass "
                "and signal order, 8 ms apart by pass, after the snapshot's time; NOR window GP6[10:0] / GP2[10] make none",
                log[:1] == b"\x01" and sig == [(2, 0), (0, 1), (2, 1), (3, 0), (4, 0)]
                and times[0] > t0 and times[1] == times[2] == times[0] + 8 and times[3] == times[4] == times[0] + 16,
                f"seq0 {seq0} {new}")

    # ---- 4. notices -------------------------------------------------------------------------------------
    _d, r_unb, _l = pr.reply(PANEL, rp.req(OP, 0x02, 0x01), 0x02)
    _d, r_on, later_on = pr.reply(APP, rp.req(OP, 0x02, 0x01), 0x02)
    nor.in_data23 &= ~(1 << 24)                # GP3[8] back to 0
    f1 = pr.tick(1)
    notes1 = [(p_, m) for p_, m in f1 if m[6] == OP | 0x40 and m[7] == 0x7F]
    n1 = rp.unpack7(notes1[0][1][8:-1]) if len(notes1) == 1 else b""
    # three edges on the next three passes (24 ms < 40 ms): nothing until 40 ms after the first notice
    burst = []
    for k in range(3):
        emu.periph[IN[45]] ^= 1 << 30
        burst += [(p_, m) for p_, m in pr.tick(1) if m[6] == OP | 0x40 and m[7] == 0x7F]
    later = []
    for _ in range(3):
        later += [(p_, m) for p_, m in pr.tick(1) if m[6] == OP | 0x40 and m[7] == 0x7F]
    n2 = rp.unpack7(later[0][1][8:-1]) if len(later) == 1 else b""
    ok &= check("4 17 02 01 from the unbound panel -> 57 02 02 (not bound); from the bound app -> 57 02 00 03 02; GP3[8] 1 -> 0 "
                "-> one 57 7F to port 2 on that pass (n 1, signal 0, level 0); 3 headphone edges on the next 3 passes -> no "
                f"notice inside {NOTICE_MS} ms, then one with n 3 (2 / 2 / 2, levels alternating)",
                r_unb is not None and list(r_unb[:1]) == [2] and r_on is not None and list(r_on) == [0, 3, 2]
                and len(notes1) == 1 and notes1[0][0] == APP and n1[0] == 1 and (n1[9], n1[10]) == (0, 0)
                and burst == [] and len(later) == 1 and later[0][0] == APP and n2[0] == 3
                and [n2[9 + 8 * i] for i in range(3)] == [2, 2, 2] and [n2[10 + 8 * i] for i in range(3)] == [0, 1, 0],
                f"{r_unb and list(r_unb)} {r_on and list(r_on)} {notes1} {n1.hex()} burst {burst} later {n2.hex()}")
    _d, r_off, _l = pr.reply(PANEL, rp.req(OP, 0x02, 0x00), 0x02)
    nor.in_data23 |= 1 << 24
    quiet_off = [m for _p, m in pr.tick(6) if m[6] == OP | 0x40]
    _d, r_on2, _l = pr.reply(APP, rp.req(OP, 0x02, 0x01), 0x02)
    emu.write_bytes(BINDING, b"\x00")          # the app leaves (stock binding gone)
    pr.tick(2)
    nor.in_data23 &= ~(1 << 24)
    quiet_unb = [m for _p, m in pr.tick(6) if m[6] == OP | 0x40]
    _d, snap, _l = pr.reply(PANEL, rp.req(OP), 0x00)
    s_off = rp.unpack7(snap) if snap is not None else b"\0\0\0"
    ok &= check("4 17 02 00 from the panel -> 57 02 00 00 7F, a GP3[8] edge sends nothing; on again from the app, then the "
                "app unbound: the next edge sends nothing and notices are off (snapshot flags 0x02, port 0x7F)",
                r_off is not None and list(r_off) == [0, 2, 0x7F] and quiet_off == [] and r_on2 is not None
                and list(r_on2) == [0, 3, 2] and quiet_unb == [] and s_off[1] == 0x02 and s_off[2] == 0x7F,
                f"{r_off and list(r_off)} {quiet_off} {r_on2 and list(r_on2)} {quiet_unb} {s_off[:3].hex()}")
    emu.write_bytes(BINDING, bytes([APP + 1]))
    pr.tick(2)

    # ---- 5. bad forms -----------------------------------------------------------------------------------
    bad = []
    for body in (rp.req(OP, 0x03), rp.req(OP, 0x02), rp.req(OP, 0x01, 0x00)):
        direct, later = pr.ask(APP, body)
        bad.append(direct + [m for _p, m in later if m[6] == OP | 0x40])
    _d, r_arg, _l = pr.reply(APP, rp.req(OP, 0x02, 0x05), 0x02)
    ok &= check("5 17 03 / 17 02 (no argument) / 17 01 00 -> silent; 17 02 05 -> 57 02 01 (bad argument)",
                bad == [[], [], []] and r_arg is not None and r_arg[0] == 1, f"{bad} {r_arg and list(r_arg)}")

    # ---- 6. read only -----------------------------------------------------------------------------------
    okw = [(RUN, RUN + 0x20000), (0x40180000, 0x401A0000), ro.STACK]
    stray = [w for w in pr.stores if not any(a <= w.addr < b for a, b in okw)]
    mw = [a for a in pr.mmio if a.op == "W" and (GPIO_WIN[0] <= a.addr < GPIO_WIN[1] or SYSCFG_WIN[0] <= a.addr < SYSCFG_WIN[1])]
    ok &= check(f"6 read only: {len(pr.stores)} payload stores in checks 1-5 all in payload .data / core RW / the stack; "
                f"{sum(1 for a in pr.mmio if a.op == 'R')} payload MMIO reads, no payload write in the GPIO / SYSCFG windows",
                pr.stores and not stray and not mw,
                f"{[(hex(w.addr), hex(w.pc)) for w in stray[:4]]} {[(hex(a.addr), hex(a.pc)) for a in mw[:4]]}")
    pr.close()
    return rtlib.finish(ok)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
