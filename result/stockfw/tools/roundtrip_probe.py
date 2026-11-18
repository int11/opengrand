# roundtrip_probe.py - the OpenGrand request helpers shared by the round trips: req() builds a 0x7D request, body_of()
# finds its reply among the frames a port sent, static_calls() lists a cave's calls out of itself, plus the op
# numbers and the allowed stock calls.  Named after the diagnostic-op cave images it used to verify (phase2c_probe,
# baseline); those images are no longer built.  Library only.
from __future__ import annotations

import pathlib
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rt = rtlib.load("roundtrip_phase2")   # deliver(), check(), constants, cave_symbols()
images = rtlib.load("images")
se, pa = rt.se, rt.pa
StockEmu = se.StockEmu

TAG = rt.TAG
OP_IDENTIFY, OP_DUMP, OP_STATUS = 0x01, 0x02, 0x03
OP_PORT, OP_ECHO, OP_APO, OP_BTLOG, OP_CANARY = 0x05, 0x06, 0x07, 0x08, 0x09
ECHO_MAX = 0x7C0
MAX_TOTAL = 0x800              # largest SysEx the stock receiver assembles
R1_HI = 0x40200000
CANARY_XOR = 0xC79A5A5C

# stock state the probe reports (02_apo_a2dp, 07_sysex_slot)
APO_BLOCK = 0x4139E7E4
A2DP_FLAG = 0x413AE5B8
BT_READY = 0x413AE5C1
SPECIAL_MODE = 0x419156A7
PORT3_FWD = 0x413B5408
BT_RING, BT_RD, BT_WR, BT_LINE = 0x413AA2FC, 0x413AE588, 0x413AE58A, 0x413AE5BE
PINMUX, GPIO_IN = 0x01C14128, 0x01E26098
DIALECT_FN = 0x400ACFF8

# the only stock code the probe cave may branch to
ALLOWED_CALLS = {
    0x40147E98: "stk_end_handler_done", 0x40142C90: "stk_engine_tail",
    0x4014492C: "tx0", 0x401445AC: "tx1", 0x40145284: "tx2", 0x40144CD0: "tx3",
    DIALECT_FN: "stk_bt_dialect",
}
STOCK_RAM = (0x41000000, 0x41D00000)   # zero-filled at boot: every stock global lives here
unpack7, le = rtlib.unpack7, rtlib.le


def req(op: int, *args: int) -> bytes:
    return b"\xF0\x7D" + TAG + bytes([op, *args])


def body_of(sent, port=None):
    """Data bytes of the single reply (after <op|0x40>, before F7), or None."""
    if len(sent) != 1 or (port is not None and sent[0][0] != port):
        return None
    m = sent[0][1]
    if len(m) < 8 or m[:6] != b"\xF0\x7D" + TAG or m[-1] != 0xF7:
        return None
    return m[6], m[7:-1]


def static_calls(elf: pathlib.Path, cave_lo: int, cave_end: int):
    """Every b/bl target outside the cave (code only: objdump honours the $a/$d mapping
    symbols, so literal pools and tables are not mistaken for branches), and every
    literal word pointing into the NOR alias."""
    out = rtlib.objdump(elf)
    targets, nor_lits, indirect = {}, [], []
    rows = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+" + rtlib.ENC + r"\s+(\S+)\s*(.*)", line)
        if m:   # a Thumb instruction (halfword encoding) has no literal word: -1
            rows.append((int(m[1], 16), int(m["enc"], 16) if len(m["enc"]) == 8 else -1, m[3], m[4]))
    words = {a: w for a, w, _m, _o in rows}
    prev = None
    for addr, word, mnem, ops in rows:
        if 0x60000000 <= word < 0x62000000:
            nor_lits.append((addr, word))
        if (mnem.startswith(("blx", "bx")) and ops.strip() != "lr") or re.match(r"(mov|ldr)\w*$", mnem) and ops.startswith("pc"):
            # the one indirect form GCC emits here is a switch table: `cmp rN,#k; ldrls pc,[pc,rN,lsl #2]`
            # followed by k+1 words; accept it only when every entry lands inside the cave
            jt = re.match(r"pc, \[pc, (r\d+), lsl #2\]", ops)
            cm = prev and re.match(r"cmp$", prev[2]) and re.match(r"(r\d+), #(\d+)", prev[3])
            if not (jt and cm and cm[1] == jt[1] and mnem == "ldrls" and all(
                    cave_lo <= words.get(addr + 8 + 4 * i, 0) < cave_end for i in range(int(cm[2]) + 1))):
                indirect.append((addr, f"{mnem} {ops}"))
        prev = (addr, word, mnem, ops)
        b = re.match(r"(b|bl|blx)(eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?(\.n|\.w)?$", mnem)   # .n / .w: Thumb
        if b and re.match(r"[0-9a-f]+\b", ops):
            t = int(ops.split()[0], 16)
            if not cave_lo <= t < cave_end:
                targets.setdefault(t, []).append(addr)
    return targets, nor_lits, indirect
