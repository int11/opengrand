# sysex_size.py - where can a long BLE (port 2) SysEx get lost on the stock receive path?
#
# Port2 models the external UART in front of the REAL stock port-2 receive chain (roundtrip_bt.py and roundtrip_m4.py
# feed the app's frames through it).  Its own run once used the diagnostic image phase2c_probe, which is no longer
# built; the findings below are kept as the model's documentation.  Library only.
# The chain under Unicorn:
#   ext UART @0x62000080 (modelled RBR/LSR) -> RX ISR FUN_401465ac/FUN_401464b8 -> 2 KB RX ring
#   0x413A92FC -> pump FUN_40148FB0 -> byte parser FUN_40148B24 -> SysEx start/data/end handlers
#   -> sysex-mfr hook -> cave probe op 0x06 (sink: count + CRC-32) -> TX FUN_40145284.
# and shows what the piano answers for each way the bytes can arrive (or not):
#   paced      bytes at 31.25 kbaud (6 per 2 ms MIDI-task tick)      -> correct reply up to total 0x800
#   stall      all bytes reach the ring before the pump runs        -> ring limit 0x7FF, then silent drop
#   mid-loss   bytes missing in the middle, F7 arrives              -> reply with a short count (not silent)
#   tail-loss  the end incl. F7 never arrives                       -> silent; the next F0 flushes it
#   head-loss  F0 / header never arrives                            -> silent, nothing pending
#   uart-err   LSR error bit during the message                      -> silent and port 2 stays deaf

from __future__ import annotations

import importlib.util
import pathlib
import sys
from collections import deque

sys.dont_write_bytecode = True
_HERE = pathlib.Path(__file__).resolve().parent


def _load(name):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, _HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


rp = _load("roundtrip_probe")          # req(), unpack7(), body_of(), images, rt, se, pa
rt, se, pa, images = rp.rt, rp.se, rp.pa, rp.images

# port 2 receive chain (V2.19), literals resolved from the image
UART2_RBR, UART2_LSR = 0x62000080, 0x62000094
RX_ISR = 0x401465AC        # loops FUN_401464b8 while LSR bit0
PUMP2 = 0x40148FB0
UNBIND = 0x4015B494        # FUN_4015b494(port): unbinds the controller if it is this port
GATE = 0x413A92F8          # *FUN_40143af4(): ISR drops bytes while 0
RING, RING_WR, RING_RD, RING_CNT, RX_FLAG = 0x413A92FC, 0x413AE57E, 0x413AE57C, 0x413AE580, 0x413AE5BB
STATE2, LEN2, PSTATE2 = 0x413B5402, 0x413B53F4, 0x413B5413
OP_ECHO = 0x06
BYTES_PER_TICK = 6         # 31250 baud / 10 bits = 3125 B/s -> 6.25 B per 2 ms


class Port2:
    def __init__(self, emu):
        self.emu = emu
        self.fifo = deque()
        self.lsr_err = 0            # LSR error bits (1-4) shown on the next two LSR reads
        self.err_reads = 0
        self.sent = []
        self.unbinds = 0
        emu.periph[UART2_RBR] = lambda e, a, s: self.fifo.popleft() if self.fifo else 0
        emu.periph[UART2_LSR] = self._lsr
        emu.write_bytes(GATE, b"\x01")
        for p, addr in rt.TX.items():
            emu.hook(addr, (lambda pp: lambda e: self.sent.append((pp, e.read_bytes(e.reg(0), e.reg(1)))))(p))
        emu.hook(UNBIND, self._unbind)

    def _lsr(self, e, a, s):
        # The error byte is fed alone. Read 1 (FUN_401465ac, bit0) and read 2 (FUN_401464b8,
        # bits 1-4) both see the error; the byte is then lost, so bit0 drops and the
        # ISR's wait-while-bit0 loop ends.
        if self.err_reads:
            self.err_reads -= 1
            if self.err_reads == 0:
                self.fifo.clear()
                return self.lsr_err
            return 1 | self.lsr_err
        return 1 if self.fifo else 0

    def _unbind(self, e):
        self.unbinds += 1
        return None

    def isr(self):
        self.emu.call(RX_ISR)

    def pump(self):
        self.emu.call(PUMP2, max_insns=200_000_000)

    def feed(self, data: bytes, *, per_tick=BYTES_PER_TICK, stall=False, err_at=None):
        """UART receives `data`; ISR per byte batch, pump every tick (or only at the end if stall)."""
        for i in range(0, len(data), per_tick):
            chunk = data[i:i + per_tick]
            if err_at is not None and i <= err_at < i + per_tick:
                self.fifo.extend(chunk[:err_at - i])
                self.isr()
                self.fifo.append(chunk[err_at - i])
                self.lsr_err, self.err_reads = 0x02, 2      # overrun on this byte
                self.isr()
                chunk = chunk[err_at - i + 1:]
            self.fifo.extend(chunk)
            self.isr()
            if not stall:
                self.pump()
        self.pump()

    def take(self):
        s, self.sent = self.sent, []
        return s

    def regs(self):
        e = self.emu
        return dict(flag=e.read_bytes(RX_FLAG, 1)[0], cnt=int.from_bytes(e.read_bytes(RING_CNT, 2), "little"),
                    state=e.read_bytes(STATE2, 1)[0], len=int.from_bytes(e.read_bytes(LEN2, 2), "little"),
                    parser=e.read_bytes(PSTATE2, 1)[0])
