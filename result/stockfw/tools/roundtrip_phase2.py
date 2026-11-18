# roundtrip_phase2.py - the stock SysEx receive path shared by the round trips: deliver() stages a received
# SysEx in the stock buffers and runs the real end handler, plus the stock addresses it and the later round trips
# use (TX writers, hook sites, receive state).  Named after the first cave images (Phase 2), which it used to verify;
# those images are no longer built - the loader cave is checked by roundtrip_ota.py.  Library only.
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

se = rtlib.load("stockemu")
pa = rtlib.load("patcher")
mm = rtlib.load("make_phase2_manifest")
images = sys.modules["images"]   # loaded by make_phase2_manifest: the image table
StockEmu = se.StockEmu
cave_symbols = rtlib.cave_symbols   # the later round trips reach these two through this module (rt.*)
check = rtlib.check

CAVE_VA = 0x40173A14
HOOK_H1 = 0x40147E1C
HOOK_TICK = 0x40143AE0
END_HANDLER = 0x40147DB0
ENGINE_TAIL = 0x40142C90
SYSEX_HANDLER = 0x4014A2C8
TX = {0: 0x4014492C, 1: 0x401445AC, 2: 0x40145284, 3: 0x40144CD0}
STATE_BASE = 0x413B5400   # state[port], must be 0xF0 for the end handler to act
LEN_BASE = 0x413B53F0     # u16 len[port]
BUF_BASE = 0x413B29D0     # buf[port] = BUF_BASE + port * 0x800
BINDING = 0x413CA3DE
TAG = b"\x43\x41\x37\x39"  # "CA79"


def deliver(emu, port: int, body: bytes):
    """Stage `body` (without the trailing F7) as a received SysEx and run the end handler.

    Returns the list of frames handed to any port transmitter."""
    sent = []
    for p, addr in TX.items():
        emu.hook(addr, (lambda pp: lambda e: sent.append((pp, e.read_bytes(e.reg(0), e.reg(1)))))(p))
    emu.write_bytes(BUF_BASE + port * 0x800, body)
    emu.write_bytes(LEN_BASE + port * 2, len(body).to_bytes(2, "little"))
    emu.write_bytes(STATE_BASE + port, b"\xF0")
    emu.call(END_HANDLER, port)
    for addr in TX.values():
        emu.unhook(addr)
    return sent
