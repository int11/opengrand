# roundtrip_m4.py - the ota_main M4 field failure on the emulator:
# after the panel took the binding, every BLE request came back with the reply of the PREVIOUS frame (1B -> 5A),
# then nothing; a power cycle cured it.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_m4.py [ota_main | path/to/payload.c79]
#
# One machine: the real ota_loader_v4 with the payload applied live (roundtrip_sysex.Side) in the virtual-time
# network of roundtrip_session.Net (request task, panel model, pump tail 0x401539AC every 2 ms), plus
#   - the app's frames go through the REAL port 2 receive chain (tools/sysex_size.py model of the external UART
#     0x62000080 -> RX ISR FUN_401465AC / FUN_401464B8 -> ring 0x413A92FC -> pump FUN_40148FB0 -> byte parser
#     FUN_40148B24 -> end handler FUN_40147DB0 -> hooks / resident -> payload);
#   - the engine loop tail (task 24: cave_tick -> ng_tick -> power / front / BT ticks) every 8 ms.
# The emulator has no interrupts: the one IRQ that matters is modelled where it lands - inside the stock port 2
# RX reset FUN_40145DFC (stores wr 0x40145E1C, flag 0x40145E20, rd 0x40145E28, AS 0x40145E2C, count 0x40145E30,
# IRQs enabled when the caller is a task).  A byte the ISR takes between the flag store and the count store is
# written to the ring and counted (the ISR's own stores, 0x40146524..0x40146554), then the reset zeroes the
# count: the ring holds a byte the pump never reads, and from then on the pump runs one byte behind - every
# frame is dispatched only when the next one arrives.
# Checks (numbered as printed):
#   1  baseline: app CONNECT through the real chain, 1A / 01 / 1B 03 / 1D 00 / 16 each answered with its own op;
#      the ring is consistent (count == wr - rd)
#   2  the panel takes the binding (CONNECT + identity + full request): FUN_4015DE98 -> FUN_40147A20(2) ->
#      FUN_40145DFC runs from task 19 for port 2, the app's port (the stock does the same call); the app's 1A
#      poll is on the wire and its first byte lands inside that reset (the modelled IRQ)
#   3  the field sequence after it: 01, 19 00, 1D 01 00, 1A, 1B, 1B 03, 16, 1A - each request must get its own
#      reply op within 300 ms (the bug: the previous frame's reply, 1B -> 5A as in the field)
#   4  a whole frame counted away (the stock watchdog FUN_40145E88's reset at 0x40145EDC preempted with a frame
#      arriving) and a replay state (read index ahead of the write index): the same requests answered afterwards
#   5  the ring is consistent at the end and the payload counted its repairs (1D 00 status) - fixed builds only
from __future__ import annotations

import pathlib
import sys
from collections import deque

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rss = rtlib.load("roundtrip_session")
ss = rtlib.load("sysex_size")
rs, rt, pa, images, check = rss.rs, rss.rt, rss.pa, rss.images, rss.check
STOCKFW = rss.STOCKFW
APP, PANEL = rss.APP, rss.PANEL
P2_FLAG, P2_CNT, P2_RD, P2_WR, P2_RING = 0x413AE5BB, 0x413AE580, 0x413AE57C, 0x413AE57E, 0x413A92FC
P2_RESET, RESET_MID = 0x40145DFC, 0x40145E24
CALLER_PORT_RESET = 0x40147A5C          # return address of FUN_40147A20's `bl FUN_40145DFC` (port 2)


class Net2(rss.Net):
    """roundtrip_session.Net with the app on the real port 2 chain and the engine loop every 8 ms."""

    def __init__(self, side, **kw):
        super().__init__(side, **kw)
        e = self.e
        self.fifo = deque()
        e.periph[ss.UART2_RBR] = lambda em, a, s: self.fifo.popleft() if self.fifo else 0
        e.periph[ss.UART2_LSR] = lambda em, a, s: 1 if self.fifo else 0
        e.write_bytes(ss.GATE, b"\x01")
        self.next_tick = 8000
        self.inject = []          # bytes the modelled IRQ delivers inside the next FUN_40145DFC
        self.resets = []          # (ms, caller lr) of every FUN_40145DFC
        e.hook(P2_RESET, lambda em: (self.resets.append((self.t // 1000, em.reg(14))), rss.SE.CONTINUE)[1])
        e.hook(RESET_MID, self._irq_in_reset)

    def _irq_in_reset(self, em):
        """The RX ISR's stores for each injected byte (0x40146524..0x40146554): ring[wr] = b, wr + 1, count + 1."""
        while self.inject:
            b = self.inject.pop(0)
            wr = int.from_bytes(em.read_bytes(P2_WR, 2), "little")
            cnt = int.from_bytes(em.read_bytes(P2_CNT, 2), "little")
            em.write_bytes(P2_RING + wr, bytes([b]))
            em.write_bytes(P2_WR, ((wr + 1) & 0x7FF).to_bytes(2, "little"))
            em.write_bytes(P2_CNT, (cnt + 1).to_bytes(2, "little"))
        return rss.SE.CONTINUE

    def _deliver_due(self):
        while self.events and self.events[0][0] <= self.t:
            _at, fn = self.events.pop(0)
            self.in_isr = True
            try:
                fn(self)
            finally:
                self.in_isr = False
        while self.pending and self.pending[0][0] <= self.t:
            _at, port, f = self.pending.pop(0)
            self.in_isr = True
            try:
                if port == APP:
                    self.fifo.extend(f)
                    self.e.call(ss.RX_ISR, max_insns=1_000_000)
                    r = self.e.call(ss.PUMP2, regs={"sp": rs.ISR_SP}, max_insns=200_000_000)
                    if r.stop != "returned":
                        raise RuntimeError(f"pump 2 stopped: {r}")
                else:
                    r = self.s.send(port, f[:-1] if f[-1:] == b"\xF7" else f, sp=rs.ISR_SP)
                    if r["res"].stop != "returned":
                        raise RuntimeError(f"end handler stopped: {r['res']} on {f.hex()}")
            finally:
                self.in_isr = False

    def run(self, ms):
        end = self.t + int(ms * 1000)
        while self.t < end:
            step = min(end, self.next_tick)
            super().run((step - self.t) / 1000 if step > self.t else 0)
            if self.t >= self.next_tick:
                self.next_tick += 8000
                self.s.rig.clock(8)
                self.s.pn.tick()

    def ring(self):
        e = self.e
        rd = int.from_bytes(e.read_bytes(P2_RD, 2), "little")
        wr = int.from_bytes(e.read_bytes(P2_WR, 2), "little")
        cnt = int.from_bytes(e.read_bytes(P2_CNT, 2), "little")
        return dict(flag=e.read_bytes(P2_FLAG, 1)[0], cnt=cnt, rd=rd, wr=wr, ok=cnt == ((wr - rd) & 0x7FF))


def ng(n, body, wait_ms=300):
    """An 0x7D request from the app on the wire; the ops of the 0x7D replies to the app within wait_ms."""
    n0 = len(n.tx)
    n.send(APP, rs.rp.req(*body) + b"\xF7")
    n.run(wait_ms)
    return [f[6] for _t, p, f in n.tx[n0:] if p == APP and f[:6] == b"\xF0\x7D" + rt.TAG]


FIELD = ([0x01], [0x19, 0x00], [0x1D, 0x01, 0x00], [0x1A], [0x1B], [0x1B, 0x03], [0x16], [0x1A])


def answered(n, seq):
    """Each request of seq with the ops that came back; good when every one got exactly its own op | 0x40."""
    out, good = [], True
    for b in seq:
        ops = ng(n, b)
        out.append(f"{bytes(b).hex()}->{'/'.join(f'{o:02x}' for o in ops) or '-'}")
        good &= ops == [b[0] | 0x40]
        if b[:2] == [0x1D, 0x01]:
            n.run(4200)                      # the module query (no module here: 1.5 s silence per command)
    return good, out


def bt_status(n):
    """1D 00 through the resident (the reply leaves on the next engine pass)."""
    out = n.s.pn.sysex(APP, rs.rp.req(0x1D, 0x00))
    n.s.rig.clock(8)
    n.s.pn.tick()
    out += list(n.s.pn.sent)
    n.s.pn.sent.clear()
    b = [m for _p, m in out if m[:7] == b"\xF0\x7D" + rt.TAG + b"\x5D"]
    if not b or b[0][7:9] != b"\x00\x00":
        return None
    return rs.rp.unpack7(b[0][9:-1])


def main(argv):
    arg = rtlib.first_arg(argv, "ota_main")
    path = pathlib.Path(arg) if arg.endswith(".c79") else STOCKFW / "build" / f"{arg}.c79"
    _img, _stock, _manifest, image, syms = rtlib.loader()
    data = path.read_bytes()
    print(f"payload {path.name}  sha256 {pa.sha256(data)}")
    side = rs.Side(image, syms, data)
    n = Net2(side, pump=True)
    ok = True
    try:
        # 1. baseline
        c_ok, rep = rss.connect(n, APP, full=False)
        good, out = answered(n, ([0x1A], [0x01], [0x1B, 0x03], [0x1D, 0x00], [0x16]))
        ok &= check(f"1 app CONNECT on the real port 2 chain ({rep and rep.hex()}); each 0x7D op answered with its own "
                    f"reply: {out}; ring {n.ring()}", bool(side.applied and c_ok and good and n.ring()["ok"]))

        # 2. the panel takes the binding; the app's 1A poll starts on the wire as port 2 is reset
        poll = rs.rp.req(0x1A) + b"\xF7"
        n.inject = [poll[0]]
        r0 = len(n.resets)
        c_ok, rep = rss.connect(n, PANEL, full=True)
        mine = [(t, hex(lr)) for t, lr in n.resets[r0:]]
        n.send(APP, poll[1:])                # the rest of that poll, after the reset
        n.run(4)
        st = n.ring()
        ok &= check(f"2 panel CONNECT (reply {rep and rep.hex()}, binding {n.b(rs.BINDING)}): FUN_40145DFC from "
                    f"FUN_40147A20 / FUN_4015DE98 (ms, lr) {mine}; the IRQ inside it leaves count {st['cnt']} for "
                    f"{(st['wr'] - st['rd']) & 0x7FF} bytes in the ring",
                    c_ok and n.b(rs.BINDING) == 4 and any(lr == CALLER_PORT_RESET for _t, lr in n.resets[r0:])
                    and not n.inject)

        # 3. the field sequence
        n.run(100)
        good, out = answered(n, FIELD)
        ok &= check(f"3 after the panel took the binding, each request answered with its own op (field: the previous "
                    f"frame's, 1B -> 5A): {out}; ring {n.ring()}", good)

        # 4. a whole frame counted away, then a replay state
        e = n.e
        n.run(50)
        rd = n.ring()["rd"]
        for i, b in enumerate(poll):
            e.write_bytes(P2_RING + ((rd + i) & 0x7FF), bytes([b]))
        e.write_bytes(P2_WR, ((rd + len(poll)) & 0x7FF).to_bytes(2, "little"))    # 8 bytes in, count stays 0
        n.run(40)
        good9, out9 = answered(n, FIELD[3:7])
        st = n.ring()
        e.write_bytes(P2_RD, ((st["wr"] + 0x10) & 0x7FF).to_bytes(2, "little"))   # read index 16 ahead, count 0
        n.run(40)
        goodr, outr = answered(n, FIELD[3:7])
        ok &= check(f"4 a whole frame counted away -> {out9}; read index ahead (replay) -> {outr}", good9 and goodr)

        # 5. consistent ring, repairs counted
        st = bt_status(n)
        rep_n = st[24] | st[25] << 8 if st is not None and len(st) >= 28 else None
        ok &= check(f"5 ring consistent {n.ring()}; 1D 00 port 2 repairs {rep_n}", n.ring()["ok"] and rep_n == 3)
    finally:
        side.close()
    return rtlib.finish(ok)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
