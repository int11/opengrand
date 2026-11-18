# roundtrip_bt.py - emulator verification of the BT port in the slot payload ota_main on ota_loader_v4
# (BLE-MIDI port 2 RX latch clear F1, port 2 active-sensing guard F5, BT audio module read-only query F2).  The power part of ota_main is checked by `roundtrip_power.py ota_main`.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_bt.py [ota_main] [-q]
#
# ota_main (every feature): identify answers feat1 0x77 / feat2 0x03,
# and a port 2 frame passes the SysEx front's stub first.  First written for the BT-only payload ota_bt (discarded
# 2026-11-01).
#
# Same machine as roundtrip_power.py (the real ota_loader_v4 image, the payload uploaded over the 0x7D OTA
# ops and applied live, attach() hooks the four sites).  Port 2 bytes go through the REAL stock receive
# chain of tools/sysex_size.py (external UART model -> RX ISR FUN_401465AC/FUN_401464B8 -> ring -> pump
# FUN_40148FB0 -> parser -> SysEx end handler -> resident -> payload); the UART1 module lines are written
# into the stock UART1 RX ring as the UART1 ISR would and read by the REAL line assembler FUN_40145C44.
# The AINTC HIER register (0xFFFEF500) is modelled as a plain register so the IRQ mask is observable.
# Checks (numbered as printed):
#   1  static: the BT bindings at their documented addresses; the payload holds the three read-only module
#      commands and no configuration write form (`SET <x>`) but the AUTOCALL op's two (off / factory restore line, record
#      sent only by op 1D 09, roundtrip_btsrc.py 9), the class op's `SET BT CLASS ` prefix (+ 6 hex digits
#      formed at run time - sent only by op 1D 0A, roundtrip_btsrc.py 17) and the compared `SET BT PAIR ` / `SET BT CLASS`
#      (the line log / the write counter compare against them, never sent), the AVRCP features op's `SET PROFILE AVRCP
#      CONTROLLER ` prefix (+ 2 hex digits, only op 1D 0C - roundtrip_btsrc.py 19) and its compared `SET PROFILE AVRCP`, the
#      audio routing op's `SET CONTROL AUDIO INTERNAL I2S_SLAVE` (+ ` EVENT` / ` KEEPALIVE`, only op 1D 0D - roundtrip_btsrc.py
#      21) and its compared `SET CONTROL AUDIO`: the other `SET ` text is the bare prefix the source
#      manager completes into the run-time link selection `SET <link id> SELECT` - no
#      letter follows it and ` SELECT` is in the payload; one call site each for FUN_40145DFC / FUN_4014512C; run on
#      the image, FUN_40145DFC stores parser byte, wr, flag, rd, AS 0xFF, count in that order, and
#      FUN_4014512C puts the bytes into the UART1 TX ring under the stock IRQ mask
#   2  F1: identify over the real port 2 chain -> 6-byte reply; a UART error mid-frame -> the real pump
#      unbinds and latches 0x80, the port is deaf (control); 300 ms still latched, 600 ms the payload calls
#      the real FUN_40145DFC once with HIER bit 1 clear (restored after), the port hears identify again; a
#      latch right after needs 1 s (back-off)
#   3  F5: 0xFE through the real ISR arms the AS counter; switched off (1D 03 01) the real 20 ms handler
#      FUN_4014911C counts it down and the real pump unbinds port 2 (stock meaning, control); switched on,
#      the next engine-loop pass puts it back to 0xFF and 40 handler ticks later port 2 is still bound
#   4  F2: 1D 01 00 -> the real FUN_4014512C sends SET / LIST / INFO in turn (TX ring = the three commands
#      only); module lines in the UART1 ring are taken by the real assembler without touching any stock BT
#      state (only its read index, line buffer, line length); 1D 02 chunks give back the captured text;
#      ng_quiesce is busy while a command is on the wire
#   5  stores: payload code writes only its .data, the core RW, the stack, the power port's three globals
#      and the port 2 AS counter; its only peripheral writes are HIER; the TX / RX rings are stock-written
#   6  1D 0B: DSP L2 (the emulator's stub window 0x11700000-, made RAM-like) holds a known
#      record-bus capture; the reply from an unbound port equals a Python scan of it (peaks per slot, mean, count over
#      the floor, gain, write position, MOVED on the next scan); payload code only reads DSP L2 - the two buffers and
#      the four DSP words - and stores nothing there
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rpw = rtlib.load("roundtrip_power")
ss = rtlib.load("sysex_size")
rh, ro, rp, rt, images, pa = rpw.rh, rpw.ro, rpw.rp, rpw.rt, rpw.images, rpw.pa
check, le = rpw.check, rpw.le
Rig, upload_apply = rpw.Rig, rpw.upload_apply
STOCKFW, RUN = rpw.STOCKFW, rpw.RUN
BINDING = rpw.BINDING
BT_RING, BT_WR, BT_RD, BT_FLAG, BT_ASSEMBLER = rpw.BT_RING, rpw.BT_WR, rpw.BT_RD, rpw.BT_FLAG, rpw.BT_ASSEMBLER
APO_COUNTER, POWER_REQ, GP3_EVENT = rpw.APO_COUNTER, rpw.POWER_REQ, rpw.GP3_EVENT

OP = 0x1D
HIER = 0xFFFEF500
P2_RESET, BT_TX = 0x40145DFC, 0x4014512C
P2_AS, P2_FLAG, P2_CNT, P2_RD, P2_WR, P2_PARSER = 0x413AE5BA, 0x413AE5BB, 0x413AE580, 0x413AE57C, 0x413AE57E, 0x413B5413
TX_RING, TX_RD, TX_WR, TX_CNT = 0x413AAAFC, 0x413AE58C, 0x413AE58E, 0x413AE590
AS_TICK = 0x4014911C          # 20 ms handler part (0x400AA888): AS count-down when the BLE line is up
BLE_LINE = 0x4139E80F         # GP4[6] state the count-down needs
LINE_BUF, LINE_LEN = 0x413AE2FC, 0x413AE5BE
BT_STATE = (0x413AE5B0, 0x413AE5D0)   # parser flags around the A2DP flag 0x413AE5B8 (READY 0x413AE5C1, AT ...)
BINDINGS = {"stk_p2_rx_reset": P2_RESET, "stk_bt_tx": BT_TX, "stk_p2_as": P2_AS, "stk_p2_rx_flag": P2_FLAG,
            "stk_p2_rx_count": P2_CNT, "stk_p2_rx_rd": P2_RD, "stk_p2_rx_wr": P2_WR, "stk_irq_hier": HIER}
SB_STANDBY = rpw.SB["standby"]


class Hier:
    """AINTC HIER as a register: reads give the last value written (initially IRQ + FIQ enabled)."""

    def __init__(self, emu):
        self.v = 0x3
        self.log = []
        emu.periph[HIER] = lambda e, a, s: self.v
        emu.on_periph_write(HIER, HIER + 4, self._w)

    def _w(self, e, a, s, v):
        self.v = v
        self.log.append(v)


def status(rig):
    r = rig.raw_reply(2, rp.req(OP, 0x00), OP)
    if r is None or len(r) < 2:
        return None
    d = rp.unpack7(r[2:])
    return dict(rc=r[1], layout=d[0], sw=d[1], q=d[2], qf=d[3], qlen=le(d, 4, 2), sent=d[6], which=d[7],
                p2flag=d[8], p2as=d[9], p2cnt=le(d, 10, 2), unlatches=le(d, 12, 2), disarms=le(d, 14, 2),
                streak=d[16], u1flag=d[17], bits=d[18], raw=d)


def op_rc(rig, *args):
    r = rig.raw_reply(2, rp.req(OP, *args), OP)
    return r[1] if r is not None and len(r) >= 2 else None


def read_text(rig):
    out, off = bytearray(), 0
    for _ in range(100):
        r = rig.raw_reply(2, rp.req(OP, 0x02, (off >> 7) & 0x7F, off & 0x7F), OP)
        if r is None or len(r) < 7 or r[1] != 0:
            return None
        total = (r[3] << 7) | r[4]
        chunk = r[7:]
        out += chunk
        off += len(chunk)
        if off >= total or not chunk:
            break
    return bytes(out)


def port2(rig, unbinds):
    """The real port 2 receive chain on this rig (sysex_size.Port2), the rig's TX capture kept, the real
    unbind FUN_4015B494 run (and counted)."""
    p2 = ss.Port2(rig.emu)
    rig.pn.hook_tx()
    rig.emu.hook(ss.UNBIND, lambda e: (unbinds.append(e.reg(0)), rt.se.StockEmu.CONTINUE)[1])
    return p2


def heard_identify(rig, p2):
    rig.pn.sent.clear()
    p2.feed(rp.req(0x01) + b"\xF7")
    out = [m for p, m in rig.pn.sent if p == 2 and m[:7] == b"\xF0\x7D" + rt.TAG + b"\x41"]
    rig.pn.sent.clear()
    return out


def main(argv):
    img, _stock, _manifest, image, syms = rtlib.loader()
    which = rtlib.first_arg(argv, "ota_main")
    p, data, pay, plen, pelf, psyms = rtlib.payload(which)
    if not p.feat2 & images.FEAT2_BT:
        raise SystemExit(f"{which}: no BT port (feat2 0x01)")
    want_id = bytes([p.ngver, 2, 19, p.feat0, p.feat1, p.feat2]) + (bytes([p.feat3]) if p.feat3 else b"")   # feat3: 7th byte
    ok = True
    print(f"payload {which}  {plen} B  sha256 {pa.sha256(data)}  on {img.name}")

    # ---- 1. static ------------------------------------------------------------------------------
    bad = {n: hex(psyms.get(n, 0)) for n, a in BINDINGS.items() if psyms.get(n) != a}
    ok &= check(f"1 stock_bt.ld: all {len(BINDINGS)} BT bindings at their documented addresses", not bad, str(bad))
    cmds = [c for c in (b"SET\r\n", b"LIST\r\n", b"INFO\r\n") if c in pay]
    # a write form = `SET ` + a category word (`SET BT …`, `SET CONTROL …`, `SET PROFILE …`).  The bare `SET ` prefix that
    # btsrc.c q_avrcp completes with a link id and ` SELECT` is not followed by a letter (the compiler keeps it as a
    # literal word)
    writes_form = [i for i in range(len(pay) - 5) if (pay[i:i + 4] == b"SET " and chr(pay[i + 4]).isascii() and chr(pay[i + 4]).isalpha()
                                                      or pay[i:i + 4] == b"SET\t")]
    if any(pay[i:i + 4] == b"SET " for i in range(len(pay) - 4)) and b" SELECT\0" not in pay:
        writes_form.append(-1)
    # allowed: the AUTOCALL op's two writes - off (`SET CONTROL AUTOCALL`, RM 6.68 "Disable") and
    # the stock factory line restore falls back to - sent only from op 1D 09 (roundtrip_btsrc.py 9 checks the bytes on
    # the wire), and `SET BT PAIR ` - a text the line log COMPARES against to cut pairing keys off (never sent)
    # the class-of-device op's write prefix (`SET BT CLASS ` + 6 hex digits formed at run time, only from op 1D 0A -
    # roundtrip_btsrc.py 17) and `SET BT CLASS` the write counter compares a sent command against (never sent itself)
    # the AVRCP features op's write prefix (`SET PROFILE AVRCP CONTROLLER ` + 2 hex digits formed at run time, only from
    # op 1D 0C - roundtrip_btsrc.py 19) and `SET PROFILE AVRCP` its write counter compares against (never sent itself)
    # the audio routing op's line (`SET CONTROL AUDIO INTERNAL I2S_SLAVE` + ` EVENT` / ` KEEPALIVE` formed at run time,
    # only from op 1D 0D - roundtrip_btsrc.py 21) and `SET CONTROL AUDIO` the listing parser compares against (never sent)
    allowed = (b"SET CONTROL AUTOCALL\0", b"SET CONTROL AUTOCALL 19 5000 A2DP\0", b"SET BT PAIR \0",
               b"SET BT CLASS \0", b"SET BT CLASS\0", b"SET PROFILE AVRCP CONTROLLER \0", b"SET PROFILE AVRCP\0",
               b"SET CONTROL AUDIO INTERNAL I2S_SLAVE\0", b"SET CONTROL AUDIO\0")
    writes_form = [i for i in writes_form if not any(pay[i:i + len(a)] == a for a in allowed)]

    def is_text(i: int) -> bool:
        """a NUL-terminated printable string starts here (not the literal word `SET ` followed by a pointer whose low
        byte happens to be a letter - the q_on_link prefix in a literal pool)"""
        end = pay.find(b"\0", i)
        return end > i + 5 and all(0x20 <= c < 0x7F for c in pay[i:end])

    writes_form = [i for i in writes_form if i < 0 or is_text(i)]
    targets, _nl, _ind = rp.static_calls(pelf, RUN, RUN + plen)
    ok &= check("1 the payload carries the three read-only iWRAP queries SET / LIST / INFO and no `SET <x>` write form "
                "(only the bare `SET ` prefix of the run-time `SET <link> SELECT`); "
                "FUN_4014512C (UART1 TX) and FUN_40145DFC (port 2 RX reset) are each called from one site",
                len(cmds) == 3 and not writes_form and len(targets.get(BT_TX, [])) == 1 and len(targets.get(P2_RESET, [])) == 1,
                f"cmds {len(cmds)} write-form at {writes_form[:3]} tx {targets.get(BT_TX)} reset {targets.get(P2_RESET)}")

    rig = Rig(image, syms)
    emu = rig.emu
    for a, v in ((P2_PARSER, b"\x05"), (P2_WR, b"\x10\x00"), (P2_FLAG, b"\x80"), (P2_RD, b"\x10\x00"),
                 (P2_AS, b"\x11"), (P2_CNT, b"\x00\x00")):
        emu.write_bytes(a, v)
    res = emu.call(P2_RESET)
    order = [w_.addr for w_ in res.writes if not ro.STACK[0] <= w_.addr < ro.STACK[1]]
    vals = [emu.read_bytes(P2_PARSER, 1)[0], le(emu.read_bytes(P2_WR, 2), 0, 2), emu.read_bytes(P2_FLAG, 1)[0],
            le(emu.read_bytes(P2_RD, 2), 0, 2), emu.read_bytes(P2_AS, 1)[0], le(emu.read_bytes(P2_CNT, 2), 0, 2)]
    ok &= check("1 run on the image: FUN_40145DFC stores parser byte 0x413B5413, wr, flag, rd, AS counter, count - in that "
                "order, nothing else - to 0, 0, 0, 0, 0xFF, 0",
                res.stop == "returned" and order == [P2_PARSER, P2_WR, P2_FLAG, P2_RD, P2_AS, P2_CNT] and vals == [0, 0, 0, 0, 0xFF, 0],
                f"{[hex(a) for a in order]} {vals}")
    hier = Hier(emu)
    wr0 = le(emu.read_bytes(TX_WR, 2), 0, 2)
    rd0 = le(emu.read_bytes(TX_RD, 2), 0, 2)
    emu.write_bytes(0x40B90100, b"SET\r\n")
    res = emu.call(BT_TX, 0x40B90100, 5, max_insns=2_000_000)
    ring = emu.read_bytes(TX_RING + wr0, 5)
    mem = sorted({w_.addr for w_ in res.writes if not 0x40F00000 <= w_.addr < 0x41000000})
    ok &= check("1 run on the image: FUN_4014512C(\"SET\\r\\n\", 5) puts the 5 bytes into the UART1 TX ring 0x413AAAFC, write "
                "index and count +5, each byte under the stock IRQ mask (HIER bit 1 cleared, then set: 5 pairs)",
                res.stop == "returned" and ring == b"SET\r\n" and le(emu.read_bytes(TX_WR, 2), 0, 2) == wr0 + 5
                and le(emu.read_bytes(TX_CNT, 2), 0, 2) + le(emu.read_bytes(TX_RD, 2), 0, 2) - rd0 == 5 and hier.log[:10] == [0x1, 0x3] * 5 and hier.v == 0x3,
                f"{ring} cnt {le(emu.read_bytes(TX_CNT, 2), 0, 2)} rd {rd0}->{le(emu.read_bytes(TX_RD, 2), 0, 2)} hier {hier.log[:10]} mem {[hex(a) for a in mem[:9]]}")
    rig.close()

    # ---- 2. F1: port 2 latch -------------------------------------------------------------------
    rig = Rig(image, syms)
    pn, emu = rig.pn, rig.emu
    hier = Hier(emu)
    upload_apply(rig, data)
    emu.write_bytes(BINDING, b"\x03")
    rig.tick(2)
    pn.writes.clear()
    unbinds = []
    p2 = port2(rig, unbinds)
    first = heard_identify(rig, p2)
    ok &= check("2 identify over the real port 2 chain (ISR -> ring -> pump -> parser -> end handler -> resident -> payload): "
                f"{want_id.hex(' ').upper()} (feat2 0x01 = BT port)",
                len(first) == 1 and first[0][7:-1] == want_id, str([m.hex() for m in first]))
    resets = []
    emu.hook(P2_RESET, lambda e: (resets.append((hier.v, e.reg(14))), rt.se.StockEmu.CONTINUE)[1])
    rig.pn.sent.clear()
    p2.feed(rp.req(0x01) + b"\xF7", err_at=4)       # UART overrun inside the frame
    latched = emu.read_bytes(P2_FLAG, 1)[0]
    deaf = heard_identify(rig, p2)
    stock_resets = len(resets)
    ok &= check("2 UART error inside a frame: the real pump unbinds port 2 and latches the flag at 0x80; the port is then deaf "
                "(identify gets no reply) - stock behaviour, control",
                latched == 0x80 and 2 in unbinds and not deaf and emu.read_bytes(P2_FLAG, 1)[0] == 0x80,
                f"flag {latched:#x} unbinds {unbinds} deaf {len(deaf)} stock resets {stock_resets}")
    rig.tick(1, ms=300)
    early = emu.read_bytes(P2_FLAG, 1)[0] == 0x80 and len(resets) == stock_resets
    st0 = status(rig)
    rig.tick(1, ms=300)
    mine = resets[stock_resets:]
    st1 = status(rig)
    back = heard_identify(rig, p2)
    ok &= check("2 300 ms: still latched (status bit latched); 600 ms: the payload calls the real FUN_40145DFC once, entered with "
                "HIER bit 1 clear (IRQ masked) and HIER back at 0x3 after; flag 0, unlatches 1; identify answered again",
                early and st0 and st0["bits"] & 0x04 and len(mine) == 1 and mine[0][0] & 0x2 == 0 and hier.v == 0x3
                and emu.read_bytes(P2_FLAG, 1)[0] == 0 and st1 and st1["unlatches"] == 1 and len(back) == 1,
                f"early {early} calls {[(hex(h), hex(l)) for h, l in mine]} hier {hier.v:#x} st1 {st1 and st1['unlatches']} back {len(back)}")
    n_before = len(resets)
    p2.feed(rp.req(0x01) + b"\xF7", err_at=4)
    rig.tick(1, ms=600)
    still = emu.read_bytes(P2_FLAG, 1)[0] == 0x80
    rig.tick(1, ms=500)
    st2 = status(rig)
    ok &= check("2 latched again right after the clear: 600 ms is not enough any more (back-off 1 s), 1.1 s clears it (streak 1)",
                still and emu.read_bytes(P2_FLAG, 1)[0] == 0 and len(resets) >= n_before + 2 and st2 and st2["streak"] == 1
                and st2["unlatches"] == 2, f"still {still} st2 {st2 and (st2['streak'], st2['unlatches'])}")
    emu.unhook(P2_RESET)

    # ---- 3. F5: active sensing ------------------------------------------------------------------
    emu.write_bytes(BLE_LINE, b"\x01")
    emu.write_bytes(BINDING, b"\x03")
    rc_off = op_rc(rig, 0x03, 0x01)                  # F1 only: the stock count-down runs
    unbinds.clear()
    p2.feed(b"\xFE")
    armed = emu.read_bytes(P2_AS, 1)[0]
    rig.tick()
    for _ in range(20):
        emu.call(AS_TICK)
    p2.pump()
    ctrl = (armed, list(unbinds), emu.read_bytes(BINDING, 1)[0], emu.read_bytes(P2_AS, 1)[0])
    ok &= check("3 control (F5 off, 1D 03 01 rc 0): 0xFE through the real ISR arms the counter (0x11); 20 ticks of the real "
                "FUN_4014911C count it out, the real pump unbinds port 2 (binding 0) and resets the counter to 0xFF",
                rc_off == 0 and armed == 0x11 and 2 in unbinds and ctrl[2] == 0 and ctrl[3] == 0xFF, str(ctrl))
    emu.write_bytes(BINDING, b"\x03")
    rc_on = op_rc(rig, 0x03, 0x03)
    unbinds.clear()
    p2.feed(b"\xFE")
    armed = emu.read_bytes(P2_AS, 1)[0]
    rig.tick()
    after = emu.read_bytes(P2_AS, 1)[0]
    for i in range(40):
        emu.call(AS_TICK)
        if i % 3 == 0:
            p2.feed(b"\xFE" if i % 15 == 0 else b"")
            rig.tick()
    p2.pump()
    st3 = status(rig)
    ok &= check("3 F5 on (1D 03 03): 0xFE arms the counter, the next engine-loop pass puts it back to 0xFF; 40 handler ticks "
                "with 0xFE every 300 ms: never counted out, port 2 still bound, no unbind; status counts the disarms",
                rc_on == 0 and armed == 0x11 and after == 0xFF and not unbinds and emu.read_bytes(BINDING, 1)[0] == 3
                and st3 and st3["disarms"] >= 2 and st3["sw"] == 3, f"{armed:#x}->{after:#x} unbinds {unbinds} st {st3 and st3['disarms']}")

    # ---- 4. F2: module query ---------------------------------------------------------------------
    tx_calls = []
    emu.hook(BT_TX, lambda e: (tx_calls.append(e.read_bytes(e.reg(0), e.reg(1))), rt.se.StockEmu.CONTINUE)[1])
    rig.bt_line("A2DP STREAMING START 0")           # before the query: not part of the text
    emu.write_bytes(BT_RD, emu.read_bytes(BT_WR, 2))
    txw0 = le(emu.read_bytes(TX_WR, 2), 0, 2)
    state0 = emu.read_bytes(BT_STATE[0], BT_STATE[1] - BT_STATE[0])
    rc = op_rc(rig, 0x01, 0x00)
    # quiesce through the entry table word (payload + 0x14) as the resident calls it - an ARM entry point in a Thumb payload too
    q_busy = pn.call(le(pay, 0x14)).r0
    rig.tick()
    asm_writes = set()

    def module(*lines):
        for ln in lines:
            rig.bt_line(ln)
            res_ = emu.call(BT_ASSEMBLER, max_insns=2_000_000)
            pn.writes.extend(res_.writes)
            asm_writes.update(w_.addr for w_ in res_.writes if not ro.STACK[0] <= w_.addr < ro.STACK[1])

    module("SET BT BDADDR 02:07:80:12:34:56", "SET BT NAME CA79", "SET BT PAGEMODE 3 2000 1",
           "SET PROFILE A2DP SINK", "SET PROFILE SPP", "SET CONTROL AUTOCALL 19 5000 A2DP", "SET")
    rig.tick(2)
    rig.tick(6, ms=100)                              # 500 ms of silence ends the SET reply -> LIST
    rig.tick(4)
    rig.tick(26, ms=100)                             # no reply to LIST in 2 s -> INFO
    rig.tick(4)
    module("WRAP THOR AI (4.1.0 build 1066)", "Copyright (c) 2003-2011 Bluegiga Technologies Inc.")
    rig.tick(7, ms=100)
    rig.tick()
    q_idle = pn.call(le(pay, 0x14)).r0
    st4 = status(rig)
    text = read_text(rig)
    txring = emu.read_bytes(TX_RING + txw0, 17)
    state1 = emu.read_bytes(BT_STATE[0], BT_STATE[1] - BT_STATE[0])
    want = (b"> SET\r\nSET BT BDADDR 02:07:80:12:34:56\r\nSET BT NAME CA79\r\nSET BT PAGEMODE 3 2000 1\r\n"
            b"SET PROFILE A2DP SINK\r\nSET PROFILE SPP\r\nSET CONTROL AUTOCALL 19 5000 A2DP\r\nSET\r\n> LIST\r\n> INFO\r\n"
            b"WRAP THOR AI (4.1.0 build 1066)\r\nCopyright (c) 2003-2011 Bluegiga Technologies Inc.\r\n")
    ok &= check("4 1D 01 00 -> rc 0; the real FUN_4014512C sends SET, then (500 ms after the last SET line) LIST, then (2 s "
                "without a reply) INFO - every call ONE byte (no kick race), the first byte of a "
                "command 16 ms before the rest: the TX ring holds exactly SET\\r\\nLIST\\r\\nINFO\\r\\n; done, 3 sent, flag 'a command "
                "got no reply'",
                rc == 0 and b"".join(tx_calls).startswith(b"SET\r\nLIST\r\nINFO\r\n") and all(len(c) == 1 for c in tx_calls)
                and set(b"".join(tx_calls)[17:].split(b"\r\n")) <= {b"LIST", b""}   # then only the source manager's LIST
                and txring == b"SET\r\nLIST\r\nINFO\r\n"
                and st4 and st4["q"] == 3 and st4["sent"] == 3 and st4["qf"] == 0x10,
                f"rc {rc} tx {tx_calls} ring {txring} st {st4 and (st4['q'], st4['sent'], st4['qf'])}")
    ok &= check("4 1D 02 chunks (<= 48 B each) give back the captured text: a '> CMD' marker per command and the module lines "
                "after it, not the STREAMING line from before the query",
                text == want and st4["qlen"] == len(want), f"{len(text or b'')} B vs {len(want)} B: {(text or b'')[:60]!r}")
    allowed_asm = {BT_RD, LINE_LEN, BT_FLAG} | set(range(LINE_BUF, LINE_BUF + 0x200))
    ok &= check("4 the real line assembler FUN_40145C44 takes every SET / INFO line and stores only its read index, the line "
                "buffer, the line length and its flag (`and #0x7F`, stays 0): no stock BT state changes (0x413AE5B0-0x413AE5CF identical, A2DP flag, READY)",
                asm_writes and asm_writes <= allowed_asm and state0 == state1 and emu.read_bytes(BT_RD, 2) == emu.read_bytes(BT_WR, 2),
                str(sorted(hex(a) for a in asm_writes - allowed_asm)[:6]))
    ok &= check("4 ng_quiesce = 1 while the query runs (apply-now waits for it), 0 when it is done",
                q_busy == 1 and q_idle == 0, f"{q_busy} {q_idle}")
    emu.unhook(BT_TX)

    # ---- 5. stores -----------------------------------------------------------------------------
    okw = [(RUN, RUN + 0x20000), (0x40180000, 0x401A0000), ro.STACK, (0x40B90000, 0x40B90200),
           (APO_COUNTER, APO_COUNTER + 4), (POWER_REQ, POWER_REQ + 1), (GP3_EVENT, GP3_EVENT + 1), (P2_AS, P2_AS + 1)]
    if p.feat2 & images.FEAT2_SESSION:   # contract (b): the session sender marks the resident's inflight flag
        okw.append((syms["ota_in_sysex"], syms["ota_in_sysex"] + 1))
    mine = [w_ for w_ in pn.writes if RUN <= w_.pc < RUN + plen]
    stray = [w_ for w_ in mine if not any(a <= w_.addr < b_ for a, b_ in okw)]
    stockw = sorted({w_.addr for w_ in mine if 0x41000000 <= w_.addr < 0x41D00000})
    ok &= check(f"5 {len(mine)} stores by payload code (checks 2-4): only payload .data, core RW, stack, the power port's three "
                "globals and the port 2 AS counter 0x413AE5BA - the port 2 ring / indices / flag and the UART1 TX / RX rings are "
                "written only by stock code", mine and not stray and set(stockw) <= {APO_COUNTER, POWER_REQ, GP3_EVENT, P2_AS},
                f"stray {[(hex(w_.addr), hex(w_.pc)) for w_ in stray[:3]]}" if stray else str([hex(a) for a in stockw]))
    hw = [x for x in hier.log]
    ok &= check("5 HIER (0xFFFEF500) ends at 0x3 (IRQ + FIQ enabled) after every sequence: no mask left behind",
                hier.v == 0x3 and bool(hw), f"{hier.v:#x} ({len(hw)} writes)")
    rig.close()

    # ---- 6. 1D 0B: the DSP record-bus capture (read only) -------------------------------------------
    ok &= check_level(image, syms, data, psyms)

    return rtlib.finish(ok, " - emulation only: no interrupts, no RTOS scheduling, no caches; time = the stock timer base we set.")


DSP_L2 = (0x11800000, 0x40000)
REC_BUF, REC_WORDS = 0x11830070, 0x480
REC_WP, REC_N, REC_STATE, REC_GAIN = 0x11837D80, 0x11837D84, 0x11837D70, 0x11837DB2
LVL_BINDINGS = {"stk_dsp_rec_buf": REC_BUF, "stk_dsp_rec_wp": REC_WP, "stk_dsp_rec_n": REC_N,
                "stk_dsp_rec_state": REC_STATE, "stk_dsp_rec_gain": REC_GAIN}


def check_level(image, syms, data, psyms):
    """6: op 1D 0B on the real payload - DSP L2 mapped as RAM (the stock MMU maps 0x11700000-0x11F07FFF uncached; the
    emulator does not map it by default), a known capture written in; the reply's peaks / mean / count / gain /
    position equal a Python scan of the same bytes; payload code only READS DSP L2, and only the capture buffers and
    the four DSP words."""
    import struct

    ok = True
    bad = {n: hex(psyms.get(n, 0)) for n, a in LVL_BINDINGS.items() if psyms.get(n) != a}
    ok &= check(f"6 stock_bt.ld: the {len(LVL_BINDINGS)} DSP capture bindings at their documented addresses", not bad, str(bad))
    rig = Rig(image, syms)
    emu = rig.emu
    upload_apply(rig, data)
    emu.write_bytes(BINDING, b"\x03")
    rig.tick(2)
    # DSP L2 lies in the emulator's peripheral stub window 0x11700000-0x11EFFFFF (the stock MMU region): made RAM-like
    # (readback store), every payload access to it logged through the stub's read value / write hooks
    emu.set_readback(DSP_L2[0])
    stub = next(s for s in emu._stubs if s.base <= DSP_L2[0] < s.base + s.size)
    reads, writes = set(), []
    lo, hi = DSP_L2[0], DSP_L2[0] + DSP_L2[1]
    value0 = emu._periph_value

    def value(stub_, addr, size):
        if lo <= addr < hi:
            reads.add((addr, size))
        return value0(stub_, addr, size)

    emu._periph_value = value
    emu.on_periph_write(lo, hi, lambda e, a, s, v: writes.append(a))

    def put(addr, b):
        stub.store[addr - stub.base:addr - stub.base + len(b)] = b

    samples = [0] * (REC_WORDS * 2)
    for i in range(0, len(samples), 2):
        samples[i] = (1200, -700, 0, 300)[(i // 2) % 4]   # slot 0 (L)
        samples[i + 1] = (-5, 3, 0, 9)[(i // 2) % 4]      # slot 1 (R): under the floor
    samples[100] = -32768
    put(REC_BUF, struct.pack(f"<{len(samples)}h", *samples))
    put(REC_WP, struct.pack("<I", REC_BUF + 0x900 + 0x40))
    put(REC_N, struct.pack("<H", 0x120))
    put(REC_STATE, b"\x00\x00")
    put(REC_GAIN, struct.pack("<H", 0x2D))

    def level(port):
        r = rig.raw_reply(port, rp.req(OP, 0x0B), OP)
        if r is None or len(r) < 2 or r[0] != 0x0B or r[1] != 0:
            return None
        d = rp.unpack7(r[2:])
        return dict(layout=d[0], flags=d[1], pl=le(d, 2, 2), pr=le(d, 4, 2), mean=le(d, 6, 2), over=le(d, 8, 2),
                    gain=le(d, 10, 2), off=le(d, 12, 2), n=le(d, 14, 2)) if len(d) >= 16 else None

    a = [min(abs(x), 0x7FFF) for x in samples]
    want = dict(layout=1, flags=0x02, pl=max(a[0::2]), pr=max(a[1::2]), mean=sum(a) // len(a),
                over=sum(1 for x in a if x >= 16), gain=0x2D, off=0x940, n=0x120)
    before = bytes(stub.store[lo - stub.base:hi - stub.base])
    got1 = level(1)                                   # port 1: not the bound controller (binding 3 = port 2)
    put(REC_N, struct.pack("<H", 0x1A0))           # the DSP moved on (a host write: the payload stores nothing here)
    before = bytes(stub.store[lo - stub.base:hi - stub.base])
    got2 = level(2)
    ok &= check("6 1D 0B from an unbound port -> 5D 0B rc 0, layout 1, flags FIRST; peaks L 0x7FFF (the -32768 sample) / R 9 "
                "(under the floor), mean, samples >= 16, gain 0x2D, write position 0x940, count 0x120 = a Python scan of the "
                "same capture; the next scan after the DSP count moved: MOVED",
                got1 == want and got2 is not None and got2["flags"] == 0x01 and got2["n"] == 0x1A0,
                f"got {got1} want {want} then {got2 and got2['flags']}")
    allowed = set(range(REC_BUF, REC_BUF + REC_WORDS * 4)) | {REC_WP, REC_WP + 2, REC_N, REC_STATE, REC_GAIN}
    stray = sorted({x for x, _sz in reads if x not in allowed})
    words = {x for x, sz in reads if REC_BUF <= x < REC_BUF + REC_WORDS * 4 and sz == 4}
    ok &= check("6 payload code only READS DSP L2: no store into 0x11800000-0x1183FFFF (the capture unchanged), reads only the "
                f"two buffers ({REC_WORDS} 32-bit words, every one) and the write pointer / count / record state / gain",
                not writes and bytes(stub.store[lo - stub.base:hi - stub.base]) == before and not stray and len(words) == REC_WORDS,
                f"writes {[hex(x) for x in writes[:4]]} stray reads {[hex(x) for x in stray[:6]]} words {len(words)}")
    rig.close()
    return ok


if __name__ == "__main__":
    sys.exit(main(sys.argv))
