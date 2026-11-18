# stockemu.py - Unicorn harness for the Kawai CA79 stock MainProgram image (CAx9_P06.SYS).
#
# Purpose: run individual stock ARM functions (or short code snippets) on the raw image so that
# static-analysis claims can be re-checked and patched stock functions can be exercised before
# anything goes near a device. NOT a full-system emulator: no interrupts, no timers, no real
# peripherals, ARM mode only.
#
# usage (isolated mode does not put this directory on sys.path, so add it explicitly):
#   import sys; sys.path.insert(0, "<repo>/result/stockfw/tools")
#   from stockemu import StockEmu
#   emu = StockEmu()                       # default: target/CAx9_P06.SYS
#   res = emu.call(0x400aa030, src, dst, n)  # -> CallResult (r0, insns, writes, periph, guard, stop, ...)
#   uv run python -I tools/stockemu.py [image]   # prints the memory map and the observed loader copies
#   nor = emu.attach_nor(latency={'erase': 20})  # optional NOR device model (commands, window, RY/BY#, guard)
#   # -> nor.ops / nor.cycles logs, NorGuardError outside nor.allowed (default: update slots 0x180000-0x1FFFFF)
from __future__ import annotations

import ctypes
import hashlib
from array import array
import os
import sys
import weakref
from collections import namedtuple
from dataclasses import dataclass, field
from pathlib import Path

from unicorn import (
    UC_ARCH_ARM,
    UC_HOOK_BLOCK,
    UC_HOOK_CODE,
    UC_HOOK_INTR,
    UC_HOOK_MEM_FETCH_PROT,
    UC_HOOK_MEM_FETCH_UNMAPPED,
    UC_HOOK_MEM_READ,
    UC_HOOK_MEM_READ_PROT,
    UC_HOOK_MEM_READ_UNMAPPED,
    UC_HOOK_MEM_WRITE,
    UC_HOOK_MEM_WRITE_PROT,
    UC_HOOK_MEM_WRITE_UNMAPPED,
    UC_MODE_ARM,
    UC_PROT_ALL,
    UC_PROT_EXEC,
    UC_PROT_READ,
    Uc,
    UcError,
)
from unicorn import arm_const as A
from unicorn.unicorn_py3.arch.arm import UcRegCP as A_UcRegCP
from unicorn import unicorn_const as U

try:  # the C entry points behind Uc.reg_read / reg_write, for the per-store / per-hook register accesses
    from unicorn.unicorn_py3.unicorn import uclib as _uclib
except ImportError:  # other binding layout: the plain Uc methods (same values, slower)
    _uclib = None


# --------------------------------------------------------------------------------------------
# The per-block / per-store callbacks in C (tools/emuhooks.c): built on first use into build/native/
# --------------------------------------------------------------------------------------------
_EH_NSIG, _EH_MAXGUARD = 16, 32
_EH_GUARD_CB = ctypes.CFUNCTYPE(None, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64)
_EH_NP_CB = ctypes.CFUNCTYPE(None, ctypes.c_uint64)
_EH_CODE_CB = ctypes.CFUNCTYPE(None, ctypes.c_uint64, ctypes.c_uint64)
_EH_HTAB = 4096
_u64, _i64, _i32 = ctypes.c_uint64, ctypes.c_int64, ctypes.c_int32


class _EhState(ctypes.Structure):
    """eh_state of emuhooks.c, field for field."""
    _fields_ = [("reg_read", ctypes.c_void_p), ("reg_read_batch", ctypes.c_void_p), ("emu_stop", ctypes.c_void_p),
                ("guard_cb", _EH_GUARD_CB), ("np_cb", _EH_NP_CB),
                ("reg_pc", _i32), ("reg_cpsr", _i32), ("sig_regs", _i32 * _EH_NSIG),
                ("log_limit", _u64),
                ("insns", _u64), ("blk_start", _u64), ("blk_len", _u64), ("nblk", _u64), ("epoch", _u64),
                ("dropped", _u64), ("blk_shift", _i64), ("trace_on", _i64),
                ("np_window", _u64), ("np_next", _u64), ("np_armed_at", _u64), ("np_tries", _u64), ("np_armed", _i64),
                ("np_addr", _u64), ("np_epoch", _u64), ("np_gen", _u64), ("np_regs", _u64 * _EH_NSIG),
                ("nguard", _u64), ("glo", _u64 * _EH_MAXGUARD), ("ghi", _u64 * _EH_MAXGUARD),
                ("wbuf", ctypes.POINTER(_u64)), ("wlen", _u64), ("wcap", _u64),
                ("tbuf", ctypes.POINTER(_u64)), ("tlen", _u64), ("tcap", _u64),
                ("gx_cb", _EH_CODE_CB), ("fn_cb", _EH_CODE_CB), ("gx_on", _i64), ("htab", _u64 * _EH_HTAB)]


_EH_LIB = None


def _eh_lib():
    """emuhooks.c as a shared library: build/native/emuhooks-<sha of the source>.so, compiled once (cc -O2; a
    temporary file renamed into place, so parallel round trips never load a half-written one)."""
    global _EH_LIB
    if _EH_LIB is None:
        src = Path(__file__).with_name("emuhooks.c")
        tag = hashlib.sha256(src.read_bytes()).hexdigest()[:16]
        out = Path(__file__).resolve().parents[1] / "build" / "native" / f"emuhooks-{tag}.so"
        if not out.exists():
            import subprocess
            out.parent.mkdir(parents=True, exist_ok=True)
            tmp = out.with_name(f"{out.name}.{os.getpid()}.tmp")
            subprocess.run([os.environ.get("CC", "cc"), "-O2", "-shared", "-fPIC", "-o", str(tmp), str(src)],
                           check=True)
            os.replace(tmp, out)
        lib = ctypes.CDLL(str(out))
        lib.eh_set_hooks.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64]
        _EH_LIB = lib
    return _EH_LIB

# --------------------------------------------------------------------------------------------
# Memory map and stock-firmware constants
# --------------------------------------------------------------------------------------------
IMAGE_BASE = 0x40060000  # SDRAM VA of file offset 0
IMAGE_SIZE = 0x3A0000  # raw MainProgram partition
FLASH_BASE = 0x60060000  # NOR alias of file offset 0
SDRAM_BASE, SDRAM_SIZE = 0x40000000, 0x2000000  # 0x40000000-0x41FFFFFF
NOR_BASE, NOR_SIZE = 0x60000000, 0x600000  # 0x60000000-0x605FFFFF, read-only
VECTOR_PAGE = 0xFFFF0000
VECTOR_SRC, VECTOR_LEN = 0x4009D6D4, 0x40  # startup copies these 0x40 bytes to the vector page
BOOT_PART = 0x60000  # file bytes [0, 0x60000) are put in SDRAM by the boot stage (assumed complete)
STACK_TOP = 0x41000000  # stock reset code: SVC sp
RET_TRAP = 0xFFFFFF00  # unmapped address used as LR of the top-level call; reaching it ends the call
CPSR_SVC = 0xD3  # SVC mode, IRQ+FIQ masked, flags clear
STOCK_SHA256 = "09ad85703e44ad95d82475cfe2dae14c6edfa9f164a0b17b66b45f74c8d933c8"
DEFAULT_IMAGE = Path(__file__).resolve().parents[3] / "target" / "CAx9_P06.SYS"
STOCK_HINT = (f"put Kawai's official V2.19 system file CAx9_P06.SYS (sha256 {STOCK_SHA256}) into target/ "
              "to build the images and run the emulator checks")


def stock_bytes(what: str = "") -> bytes:
    """target/CAx9_P06.SYS, checked (the same rule as patcher.stock_bytes). Absent: print SKIP + where to get it and
    exit 0 - target/ is not in the repository, so the stock-dependent checks skip. Present but not the stock
    V2.19 image: FAIL (exit 1)."""
    if not DEFAULT_IMAGE.is_file():
        print(f"SKIP {what or Path(sys.argv[0]).stem}: target/CAx9_P06.SYS absent - {STOCK_HINT}")
        sys.exit(0)
    data = DEFAULT_IMAGE.read_bytes()
    got = hashlib.sha256(data).hexdigest()
    if got != STOCK_SHA256:
        raise SystemExit(f"FAIL target/CAx9_P06.SYS is not the stock V2.19 image: {len(data)} bytes, sha256 {got} "
                         f"(expected sha256 {STOCK_SHA256})")
    return data

# stock flash->SDRAM loader
FUN_LOADER = 0x400AA994  # copies the five regions, then byte-swaps the voice-DB header table in place
FUN_COPY = 0x400AA030  # copy(src_flash, dst_sdram, len), reads NOR through a 32 KB window
FUN_WINDOW_SELECT = 0x400AA008  # writes the window number to a GPIO latch (0x01E2608C)
LOADER_END_LITERAL = 0x400AB3EC  # literal pool word holding the end of loader copy #1 (0x40173a04)
# (flash src, SDRAM dst, length) numbered #1..#5; file offset = dst - IMAGE_BASE
LOADER_COPIES = (
    (0x600C0000, 0x400C0000, 0xB3A04),
    (0x60200000, 0x40200000, 0x100000),
    (0x60300000, 0x40300000, 0x20000),
    (0x60320000, 0x40320000, 0x40000),
    (0x60360000, 0x40360000, 0x60000),
)

# hookable peripheral stubs: (base, size, name). Reads -> emu.periph value or 0, writes -> logged.
STUB_WINDOWS = (
    (0x01C00000, 0x310000, "soc_periph"),  # 0x01C00000-0x01F0FFFF (GPIO, UART, SYSCFG, ...)
    (0x11700000, 0x800000, "soc_periph2"),  # 0x11700000-0x11EFFFFF
    (0x62000000, 0x8000, "ext_logic"),  # 0x62000000-0x62007FFF (external UARTs, ...)
    (0x68000000, 0x8000, "emifa"),  # 0x68000000-0x68007FFF
    (0x80000000, 0x20000, "shared_ram"),  # 0x80000000-0x8001FFFF
    (0xB0000000, 0x8000, "ddr2_ctl"),  # 0xB0000000-0xB0007FFF
    (0xFFFEE000, 0x2000, "aintc"),  # 0xFFFEE000-0xFFFEFFFF
)

# NOR flash device model (opt-in via emu.attach_nor())
GPIO_OUT_DATA67 = 0x01E2608C  # GP6[10:0] = NOR address bits 25:15 (32 KB window), written by FUN_400aa008
GPIO_IN_DATA23 = 0x01E26048  # bit 0x400 = GP2[10] = RY/BY# (1 = ready), polled by the stock completion checks
NOR_RYBY_BIT = 0x400
NOR_WINDOW = 0x8000  # CPU supplies A[14:0] only (hypothesis: NOR address window)
NOR_CPU_SIZE = 0x2000000  # CPU-visible NOR span 0x60000000-0x61FFFFFF (MMU sections), all decoded through the window
NOR_SECTOR = 0x20000  # 128 KB uniform sectors (mask 0xf1fe0000)
NOR_BUFFER = 0x200  # write buffer 512 B (FUN_40133ec4 splits at 0x200)
NOR_CHIP_SIZE = 0x1000000  # 128 Mbit [inferred]; the stock image never reads ID/CFI
MAIN_OFFSET = FLASH_BASE - NOR_BASE  # 0x60000: MainProgram partition = .SYS file offset 0
# stock partition table 0x400f97cc (NOR offsets)
NOR_PARTITIONS = (
    (0x000000, 0x060000, "BootProgram"),
    (0x060000, 0x400000, "MainProgram"),
    (0x400000, 0x440000, "UserMemory"),
    (0x440000, 0x460000, "TouchAdjust"),
    (0x460000, 0x480000, "unused_1 (config block A)"),
    (0x480000, 0x4A0000, "unused_2 (config block B)"),
    (0x4A0000, 0x4C0000, "TouchCalib"),
    (0x4C0000, 0x500000, "unused_3"),
    (0x500000, 0x600000, "RecorderData"),
)
# remote-update cave slots A/B: the 0xFF hole of MainProgram (file 0x120000-0x1A0000), 4 sectors
NOR_UPDATE_SLOTS = ((0x180000, 0x200000),)
# never programmed/erased by the model, whatever `allowed` says: boot, the used parts of MainProgram,
# settings and the per-device calibration partitions
NOR_PROTECTED = (
    (0x000000, 0x060000, "BootProgram"),
    (0x060000, 0x180000, "MainProgram"),
    (0x200000, 0x400000, "MainProgram"),
    (0x400000, 0x440000, "UserMemory"),
    (0x440000, 0x460000, "TouchAdjust"),
    (0x460000, 0x4A0000, "config blocks A/B"),
    (0x4A0000, 0x4C0000, "TouchCalib"),
)
NOR_DEFAULT_LATENCY = {"program": 3, "buffer": 6, "erase": 20}  # busy polls per operation

DEFAULT_MAX_INSNS = 10_000_000
DEFAULT_LOG_LIMIT = 1_000_000  # entries kept per log (writes, peripheral, guard, trace); more are counted in log_dropped
DEFAULT_NO_PROGRESS = 2048  # basic blocks between no-progress checks; 0/False disables
LOADER_MAX_INSNS = 50_000_000
_NP_TRIES = 8  # how many revisits of the armed block are compared before re-arming

# --------------------------------------------------------------------------------------------
# Result / log types
# --------------------------------------------------------------------------------------------
Write = namedtuple("Write", "addr size value pc")  # CPU store to RAM (SDRAM, vector page, map_ram)


class WriteLog:
    """CallResult.writes: a list of Write records, kept packed (4 words per record) and made into Write tuples only
    when read - a round trip logs millions of stores and looks at few.  Reads like the list it replaces: len,
    index / slice, iteration, ==, repr (the list's); extend / append / clear for a log that collects many calls."""

    __slots__ = ("_a",)

    def __init__(self, records=()):
        self._a = array("Q")
        self.extend(records)

    def __len__(self):
        return len(self._a) >> 2

    def __iter__(self):
        a = self._a
        return map(Write, a[0::4], a[1::4], a[2::4], a[3::4])

    def __getitem__(self, i):
        if isinstance(i, slice):
            start, stop, step = i.indices(len(self))
            if step == 1:
                out = WriteLog()
                out._a = self._a[4 * start:4 * max(start, stop)]
                return out
            return list(self)[i]
        n = len(self)
        if i < 0:
            i += n
        if not 0 <= i < n:
            raise IndexError("WriteLog index out of range")
        a = self._a
        return Write(a[4 * i], a[4 * i + 1], a[4 * i + 2], a[4 * i + 3])

    def __eq__(self, other):
        if isinstance(other, WriteLog):
            return self._a == other._a
        if isinstance(other, list):
            return len(other) == len(self) and list(self) == other
        return NotImplemented

    __hash__ = None

    def __repr__(self):
        return repr(list(self))

    def extend(self, records):
        if isinstance(records, WriteLog):
            self._a.extend(records._a)
        else:
            for w in records:
                self._a.extend(Write(*w))

    def append(self, w):
        self._a.extend(Write(*w))

    def clear(self):
        del self._a[:]

    def copy(self):
        out = WriteLog()
        out._a = array("Q", self._a)
        return out

    def packed(self) -> bytes:
        """The records as bytes (4 little-endian u64 per record): for hashing a machine state."""
        return self._a.tobytes()
PeriphAccess = namedtuple("PeriphAccess", "op addr size value pc")  # op 'R' or 'W'
GuardHit = namedtuple("GuardHit", "op addr size value pc")  # op 'R', 'W' or 'X'


@dataclass
class CallResult:
    stop: str  # see the StockEmu docstring for the reasons
    r0: int
    pc: int
    regs: list  # r0..r12, sp, lr, pc at the stop
    insns: int  # executed instructions (exact; a hooked function counts as one)
    writes: WriteLog = field(default_factory=lambda: WriteLog())  # Write entries, in order (a list-like WriteLog)
    periph: list = field(default_factory=list)  # PeriphAccess entries (reads and writes)
    guard: list = field(default_factory=list)  # GuardHit entries
    trace: list = field(default_factory=list)  # block start addresses (trace=True only)
    info: dict = field(default_factory=dict)  # details for the stop reason
    cpsr: int = 0
    log_dropped: int = 0  # log entries not stored because the log limit was reached

    @property
    def ok(self) -> bool:
        return self.stop in ("returned", "until")

    def __str__(self) -> str:
        skip = {"errno", "error"} if self.stop != "uc_error" else set()
        extra = "".join(
            f" {k}={v:#x}" if isinstance(v, int) else f" {k}={v}"
            for k, v in self.info.items()
            if k not in skip and not (k == "pc" and v == self.pc)
        )
        return (
            f"<CallResult stop={self.stop} r0={self.r0:#x} pc={self.pc:#x} insns={self.insns} "
            f"writes={len(self.writes)} periph={len(self.periph)} guard={len(self.guard)}{extra}>"
        )


class _Stub:
    __slots__ = ("base", "size", "name", "store")

    def __init__(self, base, size, name, store):
        self.base, self.size, self.name, self.store = base, size, name, store


_REG_ALIAS = {"sb": 9, "sl": 10, "fp": 11, "ip": 12, "sp": 13, "lr": 14, "pc": 15}
_REG_IDS = [getattr(A, f"UC_ARM_REG_R{i}") for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR, A.UC_ARM_REG_PC]
_SIG_REGS = _REG_IDS[:15] + [A.UC_ARM_REG_CPSR]  # state signature for the no-progress detector
_ACCESS_NAME = {
    U.UC_MEM_READ_UNMAPPED: "read_unmapped",
    U.UC_MEM_WRITE_UNMAPPED: "write_unmapped",
    U.UC_MEM_FETCH_UNMAPPED: "fetch_unmapped",
    U.UC_MEM_READ_PROT: "read_prot",
    U.UC_MEM_WRITE_PROT: "write_prot",
    U.UC_MEM_FETCH_PROT: "fetch_prot",
}
_ERR_STOP = {
    U.UC_ERR_READ_UNMAPPED: "unmapped",
    U.UC_ERR_WRITE_UNMAPPED: "unmapped",
    U.UC_ERR_FETCH_UNMAPPED: "unmapped",
    U.UC_ERR_WRITE_PROT: "write_prot",
    U.UC_ERR_READ_PROT: "prot",
    U.UC_ERR_FETCH_PROT: "prot",
    U.UC_ERR_INSN_INVALID: "invalid_insn",
    U.UC_ERR_EXCEPTION: "exception",
}


NorCycle = namedtuple("NorCycle", "offset value window cpu pc")  # one bus write to the chip
NorOp = namedtuple("NorOp", "kind offset size data pc")  # embedded operation: 'program' | 'buffer' | 'erase'
WindowMismatch = namedtuple("WindowMismatch", "op cpu window pc")  # CPU A[24:15] != selected window


class NorError(Exception):
    """A NOR access the model refuses. Raised from inside emu.call() (the call ends with the exception)."""


class NorGuardError(NorError):
    """Program/erase outside the allowed ranges, inside a protected range, or a chip erase."""


class NorProtocolError(NorError):
    """Command sequence a stock-compatible driver must not produce (stray write, bad unlock, buffer abort, ...)."""


def _overlaps(lo, hi, ranges):
    return [r for r in ranges if lo < r[1] and r[0] < hi]


def _inside(lo, hi, ranges):
    return any(r[0] <= lo and hi <= r[1] for r in ranges)


class NorFlash:
    """AMD/Spansion-style x16 NOR as the stock routines drive it.

    Byte-addressed by NOR offset. Commands: unlock AA@0xAAA / 55@0x554 (word 0x555/0x2AA, A[10:0] decoded),
    A0 program word, 80+AA+55+30 sector erase, 25/count/data/29 write-buffer program, F0 reset (in read
    mode or inside a command sequence). 80+AA+55+10 chip erase always raises NorGuardError. Everything
    else (autoselect 90, CFI 98, SecSi 88, erase suspend B0/30, unlock bypass 20) raises NorProtocolError,
    as does any write while an embedded operation is running and any stray write in read-array mode.

    While busy, every CPU read of the chip returns the status word (DQ7 = data# for program / 0 for erase,
    DQ6 toggles on each read, DQ3 = 1 and DQ2 toggling on the erasing sector for erase, DQ15-8 = 0) and
    RY/BY# reads as 0. Each such read (or RY/BY# read) is one poll; after latency[kind] polls the operation
    completes and the chip is back in read-array mode on its own (no reset needed, as the stock assumes).

    Guard: program/erase must lie completely inside `allowed` and must not touch `protected`; else
    NorGuardError and the array is left unchanged. Programming a 0 bit back to 1 raises NorProtocolError
    (a real chip fails the operation with DQ5).
    """

    def __init__(self, image=None, *, chip_size=NOR_CHIP_SIZE, sector_size=NOR_SECTOR, buffer_size=NOR_BUFFER,
                 allowed=NOR_UPDATE_SLOTS, protected=NOR_PROTECTED, latency=None, decode="window",
                 log_limit=DEFAULT_LOG_LIMIT):
        if chip_size & (chip_size - 1) or chip_size < MAIN_OFFSET + IMAGE_SIZE:
            raise ValueError("chip_size must be a power of two that holds the MainProgram partition")
        if decode not in ("window", "linear"):
            raise ValueError("decode must be 'window' or 'linear'")
        self.chip_size, self.sector_size, self.buffer_size = chip_size, sector_size, buffer_size
        self.decode = decode
        self.mem = bytearray(b"\xff" * chip_size)
        if image is not None:
            img = StockEmu._load_image(image)
            self.mem[MAIN_OFFSET : MAIN_OFFSET + IMAGE_SIZE] = img
        self.allowed = [(int(r[0]), int(r[1])) for r in allowed]
        self.protected = [(int(r[0]), int(r[1]), *(r[2:] or ("protected",))) for r in protected]
        for lo, hi in self.allowed:
            bad = _overlaps(lo, hi, self.protected)
            if bad:
                raise ValueError(f"allowed range {lo:#x}-{hi:#x} overlaps protected {bad[0][2]} {bad[0][0]:#x}-{bad[0][1]:#x}")
        self.latency = dict(NOR_DEFAULT_LATENCY, **(latency or {}))
        self.gpio67 = 0  # OUT_DATA67 latch (GP6 = bits 15:0, GP7 = bits 31:16)
        self.in_data23 = 0  # other IN_DATA23 bits returned with RY/BY#
        self._log_limit = int(log_limit)
        self.cycles = []  # NorCycle per bus write
        self.ops = []  # NorOp per accepted program/erase (logged when it starts)
        self.window_mismatch = []  # WindowMismatch, informational
        self.polls = 0  # busy observations (status reads + RY/BY# low)
        self.fault = None  # first NorError raised; further writes re-raise it without touching the chip
        self.reset()

    # host-side access (no bus cycles, no polls) --------------------------------------------
    def reset(self):
        """Hardware reset: abort whatever runs (pending data is dropped) and return to read-array mode."""
        self.state, self._sa, self._buf, self._left, self._op = "read", None, None, 0, None
        self._toggle, self._dq2 = 0, 0
        self.fault = None

    def read(self, offset, size):
        return bytes(self.mem[offset : offset + size])

    def load(self, offset, data):
        self.mem[offset : offset + len(data)] = data

    def word(self, offset):
        return self.mem[offset] | self.mem[offset + 1] << 8

    @property
    def busy(self):
        return self.state == "busy"

    @property
    def window(self):
        return self.gpio67 & 0x7FF

    def sector(self, offset):
        return offset - offset % self.sector_size

    # address decode --------------------------------------------------------------------------
    def decode_cpu(self, cpu, op="R", pc=0):
        """NOR byte offset for CPU address cpu in 0x60000000-0x61FFFFFF."""
        low = cpu - NOR_BASE
        if self.decode == "linear":
            off = low
        else:
            win = self.window
            off = (win << 15) | (low & (NOR_WINDOW - 1))
            if (win & 0x3FF) != (low >> 15) & 0x3FF and len(self.window_mismatch) < self._log_limit:
                self.window_mismatch.append(WindowMismatch(op, cpu, win, pc))
        return off & (self.chip_size - 1)

    # bus side --------------------------------------------------------------------------------
    def bus_read(self, offset):
        """One 16-bit CPU read at NOR byte offset (halfword aligned)."""
        offset &= ~1
        if self.state != "busy":
            return self.word(offset)
        op = self._op
        self._toggle ^= 0x40
        st = self._toggle
        if op["kind"] == "erase":
            st |= 0x08
            if op["lo"] <= offset < op["hi"]:
                self._dq2 ^= 0x04
            st |= self._dq2
        else:
            st |= ~op["dq7"] & 0x80
        self._tick()
        return st

    def ryby_word(self):
        """IN_DATA23 as the CPU reads it: RY/BY# bit set when the chip is ready."""
        if self.state == "busy":
            self._tick()
            return self.in_data23 & ~NOR_RYBY_BIT
        return self.in_data23 | NOR_RYBY_BIT

    def _tick(self):
        self.polls += 1
        self._left -= 1
        if self._left <= 0:
            self._complete()

    def _err(self, cls, msg):
        e = cls(msg)
        self.fault = e
        self.state, self._sa, self._buf = "read", None, None
        raise e

    def bus_write(self, offset, value, cpu=0, pc=0):
        """One 16-bit CPU write at NOR byte offset."""
        if self.fault is not None:
            raise self.fault
        offset &= ~1
        value &= 0xFFFF
        if len(self.cycles) < self._log_limit:
            self.cycles.append(NorCycle(offset, value, self.window, cpu, pc))
        w = (offset >> 1) & 0x7FF
        st = self.state
        where = f"{value:#06x} at {offset:#x} (pc={pc:#x}, state {st})"
        if st == "busy":
            self._err(NorProtocolError, f"write during embedded {self._op['kind']}: {where}")
        if value == 0xF0 and st in ("read", "u1", "u2", "e_setup", "e_u1", "e_u2"):
            self.state = "read"
            return
        if st == "read":
            if value == 0xAA and w == 0x555:
                self.state = "u1"
            elif value == 0x98:
                self._err(NorProtocolError, f"CFI query is not modelled (the stock image has none): {where}")
            else:
                self._err(NorProtocolError, f"stray write in read-array mode: {where}")
        elif st == "u1":
            if value == 0x55 and w == 0x2AA:
                self.state = "u2"
            else:
                self._err(NorProtocolError, f"bad second unlock cycle: {where}")
        elif st == "u2":
            if w == 0x555 and value == 0xA0:
                self.state = "prog"
            elif w == 0x555 and value == 0x80:
                self.state = "e_setup"
            elif value == 0x25:
                self.state, self._sa = "buf_count", self.sector(offset)
            else:
                self._err(NorProtocolError, f"unsupported command after unlock: {where}")
        elif st == "prog":
            self._start("program", {offset: value}, offset, offset + 2, offset, pc)
        elif st == "e_setup":
            if value == 0xAA and w == 0x555:
                self.state = "e_u1"
            else:
                self._err(NorProtocolError, f"bad erase unlock cycle: {where}")
        elif st == "e_u1":
            if value == 0x55 and w == 0x2AA:
                self.state = "e_u2"
            else:
                self._err(NorProtocolError, f"bad erase unlock cycle: {where}")
        elif st == "e_u2":
            if value == 0x10 and w == 0x555:
                self._err(NorGuardError, f"chip erase rejected: {where}")
            elif value == 0x30:
                sa = self.sector(offset)
                self._start("erase", None, sa, sa + self.sector_size, offset, pc)
            else:
                self._err(NorProtocolError, f"unsupported erase command: {where}")
        elif st == "buf_count":
            n = value + 1
            if self.sector(offset) != self._sa:
                self._err(NorProtocolError, f"write-buffer count not at the sector address {self._sa:#x}: {where}")
            if n > self.buffer_size // 2:
                self._err(NorProtocolError, f"write-buffer count {n} words > {self.buffer_size // 2}: {where}")
            self.state, self._buf = "buf_data", {"left": n, "page": None, "data": {}}
        elif st == "buf_data":
            b = self._buf
            page = offset - offset % self.buffer_size
            if b["page"] is None:
                b["page"] = page
            if page != b["page"] or self.sector(offset) != self._sa:
                self._err(NorProtocolError, f"write-buffer data outside page {b['page']:#x} (abort): {where}")
            b["data"][offset] = value
            b["last"] = offset
            b["left"] -= 1
            if b["left"] == 0:
                self.state = "buf_confirm"
        elif st == "buf_confirm":
            if value != 0x29 or self.sector(offset) != self._sa:
                self._err(NorProtocolError, f"write-buffer confirm expected 0x29 in sector {self._sa:#x} (abort): {where}")
            data = self._buf["data"]
            lo, hi = min(data), max(data) + 2
            self._start("buffer", data, lo, hi, self._buf["last"], pc)
        else:  # pragma: no cover
            raise AssertionError(st)

    def _guard(self, kind, lo, hi, pc):
        bad = _overlaps(lo, hi, self.protected)
        if bad:
            self._err(NorGuardError, f"{kind} {lo:#x}-{hi:#x} touches protected {bad[0][2]} {bad[0][0]:#x}-{bad[0][1]:#x} (pc={pc:#x})")
        if not _inside(lo, hi, self.allowed):
            self._err(NorGuardError, f"{kind} {lo:#x}-{hi:#x} is outside the allowed ranges (pc={pc:#x})")

    def _start(self, kind, data, lo, hi, status_at, pc):
        self._guard(kind, lo, hi, pc)
        if data is not None:
            for a, v in data.items():
                old = self.word(a)
                if v & ~old & 0xFFFF:
                    self._err(NorProtocolError, f"{kind} would set 0->1 bits at {a:#x}: old {old:#06x} new {v:#06x} (pc={pc:#x})")
            blob = bytearray(b"\xff" * (hi - lo))
            for a, v in data.items():
                blob[a - lo : a - lo + 2] = v.to_bytes(2, "little")
            entry = NorOp(kind, lo, hi - lo, bytes(blob), pc)
        else:
            entry = NorOp(kind, lo, hi - lo, None, pc)
        if len(self.ops) < self._log_limit:
            self.ops.append(entry)
        self._op = {"kind": kind, "lo": lo, "hi": hi, "data": data, "dq7": data[status_at] if data else 0}
        self.state, self._sa, self._buf = "busy", None, None
        self._left = int(self.latency[kind])
        self._toggle = self._dq2 = 0
        if self._left <= 0:
            self._complete()

    def _complete(self):
        op = self._op
        if op["kind"] == "erase":
            self.mem[op["lo"] : op["hi"]] = b"\xff" * (op["hi"] - op["lo"])
        else:
            for a, v in op["data"].items():
                self.word_and(a, v)
        self.state, self._op = "read", None

    def word_and(self, offset, value):
        self.mem[offset] &= value & 0xFF
        self.mem[offset + 1] &= value >> 8


def _merge(ranges):
    out = []
    for lo, hi in sorted(ranges):
        if out and lo <= out[-1][1]:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return [(lo, hi) for lo, hi in out]


class StockEmu:
    """Unicorn ARM926 (ARM mode) machine holding the CA79 stock image the way the stock loader leaves it.

    emu.call(addr, ...) runs one function (or a snippet up to `until`) and returns a CallResult.
    Memory persists between calls; the CPU (r0-r12, sp, lr, CPSR, CP15 control register = MMU/caches off)
    is re-initialised for every call. Logs (writes, peripheral, guard, trace) are per call.

    call() stop reasons (CallResult.stop):
      returned     the function returned to the trap address (normal end)
      until        reached one of the `until` addresses (instruction there not executed)
      max_insns    instruction limit reached
      no_progress  loop with identical CPU state and no memory/peripheral change (info: pc, polled)
      guard_exec   instruction fetch from a guard range (always stops)
      guard        guard read/write with stop_on_guard=True
      unmapped     access to unmapped memory (info: access, addr, size, pc)
      write_prot   write to the read-only NOR alias (info: addr, size, value, pc)
      swi          SWI executed (info: number, pc)
      halted       the core halted (WFI) - nothing can wake it here, there are no interrupts
      invalid_insn / exception / uc_error   other Unicorn errors
      <any str>    reason given to emu.stop() from a hook
    """

    CONTINUE = object()  # hook return value: do not replace the function, run the original code

    # ----------------------------------------------------------------------------------------
    # construction
    # ----------------------------------------------------------------------------------------
    def __init__(self, image=None, *, loader="stock", guard=None, log_limit=DEFAULT_LOG_LIMIT):
        """image: path or bytes-like of the raw 0x3A0000-byte image (default target/CAx9_P06.SYS).

        loader="stock": run the stock loader FUN_400aa994 on the image (with the GPIO window select
          stubbed) so RAM is exactly what the loader leaves, including its in-place byte swap of the
          voice-DB header and any patched boundary literal. loader="none": only the boot-stage part
          (file 0-0x60000) is loaded.
        guard: list of (lo, hi) half-open ranges to guard; default = image span not loaded by the loader.
        log_limit: entries kept per log and call; the rest is only counted (CallResult.log_dropped).
        """
        if loader not in ("stock", "none"):
            raise ValueError("loader must be 'stock' or 'none'")
        self.image = self._load_image(image)
        self.sha256 = hashlib.sha256(self.image).hexdigest()
        self.is_stock = self.sha256 == STOCK_SHA256
        self.periph = {}  # absolute address -> int | callable(emu, addr, size) | list of ints
        self.stop_on_guard = False
        self.loader_copies = []  # (src, dst, len) as observed in the loader run
        self.guard_ranges = []
        self._log_limit = int(log_limit)
        self._hooks = {}  # addr -> (fn, uc hook handle, name)
        self._write_hooks = []  # (lo, hi, fn) peripheral write callbacks
        self._seq = {}  # addr -> (sequence object, next index) of periph sequences
        self._stubs = []
        self._ram = []
        self._exits = ()
        self._running = False
        self._host_depth = 0
        self._jump = None
        self._counting = False  # True once the block hook is installed (exact instruction counts)
        self.nor = None  # NorFlash once attach_nor() replaced the read-only NOR alias
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self._init_native()
        self._reset_run_state(False, 0, None)
        self._rr, self._rw = self._fast_reg_access(uc)
        uc.ctl_set_cpu_model(A.UC_CPU_ARM_926)
        uc.ctl_exits_enabled(True)
        self._sctlr0 = uc.cpr_read(15, 0, 1, 0, 0, 0, False)  # CP15 control register at reset: MMU and caches off
        self._map_memory()
        self._native_hook(UC_HOOK_CODE, self._eh_code)   # every emu.hook address and the guard-range execution check
        uc.hook_add(
            UC_HOOK_MEM_READ_UNMAPPED
            | UC_HOOK_MEM_WRITE_UNMAPPED
            | UC_HOOK_MEM_FETCH_UNMAPPED
            | UC_HOOK_MEM_READ_PROT
            | UC_HOOK_MEM_WRITE_PROT
            | UC_HOOK_MEM_FETCH_PROT,
            self._on_fault,
        )
        uc.hook_add(UC_HOOK_INTR, self._on_intr)
        if loader == "stock":
            self._run_stock_loader()
        self.guard_ranges = [tuple(r) for r in guard] if guard is not None else self._derive_guard()
        self._install_runtime_hooks()

    @staticmethod
    def _fast_reg_access(uc):
        """(read, write) of a 32-bit core register / CPSR by Unicorn register id: the same uc_reg_read / uc_reg_write
        call as Uc.reg_read / reg_write without the binding's per-call type dispatch (the hooks run once per store
        and per hooked call, millions of times in the round trips). Values and errors are the same."""
        if _uclib is None or not hasattr(uc, "_uch"):
            return uc.reg_read, uc.reg_write
        h, rd, wr = uc._uch, _uclib.uc_reg_read, _uclib.uc_reg_write
        rbuf, wbuf = ctypes.c_uint64(), ctypes.c_uint64()
        rref, wref = ctypes.byref(rbuf), ctypes.byref(wbuf)

        def read(reg_id):
            rbuf.value = 0
            st = rd(h, reg_id, rref)
            if st:
                raise UcError(st, reg_id)
            return rbuf.value

        def write(reg_id, value):
            wbuf.value = value
            st = wr(h, reg_id, wref)
            if st:
                raise UcError(st, reg_id)

        return read, write

    def _init_native(self):
        """The C state of the block / store callbacks (emuhooks.c) and its two Python callbacks."""
        lib = self._eh = _eh_lib()
        st = self._st = _EhState()
        st.reg_read = ctypes.cast(_uclib.uc_reg_read, ctypes.c_void_p).value
        st.reg_read_batch = ctypes.cast(_uclib.uc_reg_read_batch, ctypes.c_void_p).value
        st.emu_stop = ctypes.cast(_uclib.uc_emu_stop, ctypes.c_void_p).value
        me = weakref.ref(self)      # st keeps its callbacks alive; they must not keep the emulator alive

        def guarded(f):
            """A Python callback called from C: an exception is kept and stops the run, then emu_start raises it
            (what the Unicorn binding does for its own Python hooks)."""
            def cb(*a):
                try:
                    f(me(), *a)
                except BaseException as e:      # noqa: BLE001 - re-raised by Uc.emu_start
                    uc = me().uc
                    if uc._hook_exception is None:
                        uc._hook_exception = e
                    uc.emu_stop()
            return cb

        st.guard_cb = _EH_GUARD_CB(guarded(lambda emu, a, s, v, p: emu._on_guard_write(a, s, v, p)))
        st.np_cb = _EH_NP_CB(guarded(lambda emu, a: emu._on_no_progress(a)))
        st.gx_cb = _EH_CODE_CB(guarded(lambda emu, a, s: emu._on_guard_exec(emu.uc, a, s, None)))
        st.fn_cb = _EH_CODE_CB(guarded(lambda emu, a, s: emu._on_fn_hook(emu.uc, a, s, None)))
        self._eh_fin = weakref.finalize(self, lib.eh_free, ctypes.byref(st))   # the C log buffers
        st.reg_pc, st.reg_cpsr = A.UC_ARM_REG_PC, A.UC_ARM_REG_CPSR
        for i, r in enumerate(_SIG_REGS):
            st.sig_regs[i] = r
        self._eh_block = ctypes.cast(lib.eh_block, ctypes.c_void_p)
        self._eh_write = ctypes.cast(lib.eh_write, ctypes.c_void_p)
        self._eh_code = ctypes.cast(lib.eh_code, ctypes.c_void_p)
        self._np_gen = -1           # np_gen of the arming the polled set belongs to
        # per call: the 16 register writes of _setup_cpu and the 16 reads of _finish as one batch call each, and the
        # CP15 control register read without the binding's per-call type dispatch (same values, same order)
        h = self.uc._uch
        cp = self._cp_sctlr = A_UcRegCP()
        cp.cp, cp.is64, cp.sec, cp.crn, cp.crm, cp.opc1, cp.opc2 = 15, 0, 0, 1, 0, 0, 0
        cp_ref = ctypes.byref(cp)

        def sctlr():
            cp.val = 0
            e = _uclib.uc_reg_read(h, A.UC_ARM_REG_CP_REG, cp_ref)
            if e:
                raise UcError(e)
            return cp.val

        self._sctlr = sctlr

        def batch(ids):
            n = len(ids)
            ids_a = (ctypes.c_int * n)(*ids)
            vals = (ctypes.c_uint64 * n)()
            ptrs = (ctypes.c_void_p * n)(*[ctypes.addressof(vals) + 8 * i for i in range(n)])
            return ids_a, vals, ptrs, n

        w_ids, w_vals, w_ptrs, w_n = batch([A.UC_ARM_REG_CPSR] + _REG_IDS[:13] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR])
        r_ids, r_vals, r_ptrs, r_n = batch(_REG_IDS + [A.UC_ARM_REG_CPSR])
        wr_batch, rd_batch = _uclib.uc_reg_write_batch, _uclib.uc_reg_read_batch

        def write_setup(values):
            w_vals[:] = values
            e = wr_batch(h, w_ids, w_ptrs, w_n)
            if e:
                raise UcError(e)

        def read_all():
            """r0..r12, sp, lr, pc, cpsr."""
            for i in range(r_n):
                r_vals[i] = 0
            e = rd_batch(h, r_ids, r_ptrs, r_n)
            if e:
                raise UcError(e)
            return r_vals[:]

        self._write_setup, self._read_all = write_setup, read_all

    def _native_hook(self, htype, fn, lo=1, hi=0):
        """uc_hook_add with a C callback of emuhooks.c and the C state as its user data."""
        h = ctypes.c_size_t()
        st = _uclib.uc_hook_add(self.uc._uch, ctypes.byref(h), htype, fn, ctypes.c_void_p(ctypes.addressof(self._st)),
                                ctypes.c_uint64(lo), ctypes.c_uint64(hi))
        if st:
            raise UcError(st)
        return h.value

    def _set_guard_native(self):
        if len(self.guard_ranges) > _EH_MAXGUARD:
            raise ValueError(f"at most {_EH_MAXGUARD} guard ranges")
        st = self._st
        st.nguard = len(self.guard_ranges)
        for i, (lo, hi) in enumerate(self.guard_ranges):
            st.glo[i], st.ghi[i] = lo, hi

    def close(self):
        """Free the Unicorn engine now. Unicorn's Python binding keeps reference cycles, so without this the
        memory (~10 MB RSS per emulator) is only released when the cyclic garbage collector runs."""
        if getattr(self, "_eh_fin", None) is not None:
            self._eh_fin()
        uc, self.uc = self.uc, None
        fin = getattr(uc, "_Uc__finalizer", None)  # weakref.finalize that calls uc_close exactly once
        if fin is not None:
            fin()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    @staticmethod
    def _load_image(image):
        if image is None:
            image = stock_bytes()
        data = Path(image).read_bytes() if isinstance(image, (str, os.PathLike)) else bytes(image)
        if len(data) != IMAGE_SIZE:
            raise ValueError(f"image must be {IMAGE_SIZE:#x} bytes (raw MainProgram partition), got {len(data):#x}")
        return data

    def _map_memory(self):
        uc = self.uc
        uc.mem_map(SDRAM_BASE, SDRAM_SIZE, UC_PROT_ALL)
        uc.mem_write(IMAGE_BASE, self.image[:BOOT_PART])  # boot stage part; the rest comes from the loader
        # NOR alias: boot area and everything after the image is erased flash (0xFF), image mirrored
        uc.mem_map(NOR_BASE, NOR_SIZE, UC_PROT_ALL)
        uc.mem_write(NOR_BASE, b"\xff" * NOR_SIZE)
        uc.mem_write(FLASH_BASE, self.image)
        uc.mem_protect(NOR_BASE, NOR_SIZE, UC_PROT_READ | UC_PROT_EXEC)
        # vector page, filled the way the stock startup code does
        uc.mem_map(VECTOR_PAGE, 0x1000, UC_PROT_ALL)
        off = VECTOR_SRC - IMAGE_BASE
        uc.mem_write(VECTOR_PAGE, self.image[off : off + VECTOR_LEN])
        self._ram = [(SDRAM_BASE, SDRAM_BASE + SDRAM_SIZE), (VECTOR_PAGE, VECTOR_PAGE + 0x1000)]
        for base, size, name in STUB_WINDOWS:
            self.map_stub(base, size, name)

    def _run_stock_loader(self):
        copies = []

        def watch(emu):  # observe only, run the real copy helper
            copies.append((emu.reg(0), emu.reg(1), emu.reg(2)))
            return StockEmu.CONTINUE

        self.hook(FUN_WINDOW_SELECT, lambda emu: None, name="loader_window_select")
        self.hook(FUN_COPY, watch, name="loader_copy_watch")
        try:
            self._setup_cpu(0, 0, 0, 0, (), None)
            err = self._execute(FUN_LOADER, (RET_TRAP,), LOADER_MAX_INSNS)
            pc = self.uc.reg_read(A.UC_ARM_REG_PC)
        finally:
            self.unhook(FUN_WINDOW_SELECT)
            self.unhook(FUN_COPY)
        if err is not None or pc != RET_TRAP:
            raise RuntimeError(f"stock loader {FUN_LOADER:#x} did not return (pc={pc:#x}, error={err}); use loader='none'")
        self.loader_copies = copies

    def _derive_guard(self):
        """Image span [IMAGE_BASE, +IMAGE_SIZE) minus what the boot stage / loader copies filled."""
        end = IMAGE_BASE + IMAGE_SIZE
        loaded = [(IMAGE_BASE, IMAGE_BASE + BOOT_PART)]
        loaded += [(dst, dst + ((n + 3) & ~3)) for _src, dst, n in self.loader_copies]
        out, cur = [], IMAGE_BASE
        for lo, hi in _merge(loaded):
            lo, hi = max(lo, IMAGE_BASE), min(hi, end)
            if lo > cur:
                out.append((cur, lo))
            cur = max(cur, hi)
        if cur < end:
            out.append((cur, end))
        return out

    def _install_runtime_hooks(self):
        uc = self.uc
        self._set_guard_native()
        for lo, hi in self._ram:
            self._native_hook(UC_HOOK_MEM_WRITE, self._eh_write, lo, hi - 1)
        self._guard_read_hooks = []
        for lo, hi in self.guard_ranges:
            self._guard_read_hooks.append(uc.hook_add(UC_HOOK_MEM_READ, self._on_guard_read, None, lo, hi - 1))
        self._st.gx_on = 1          # execution in a guard range: the code hook (emuhooks.c eh_code), before any fn hook
        self._native_hook(UC_HOOK_BLOCK, self._eh_block)
        self._counting = True
        uc.ctl_flush_tb()  # hooks added after translation do not apply to cached blocks

    def guard_reads(self, on):
        """Between calls: switch the guard-range READ hooks off (on=False) or back on. Only for a run whose
        CallResult.guard read entries nobody looks at - each guarded load is a Python callback (e.g. a 6 MB
        buffer walk). Writes to and execution in guard ranges stay hooked."""
        if self._running:
            raise RuntimeError("hooks can only be changed between calls")
        if not on:
            for h in self._guard_read_hooks:
                self.uc.hook_del(h)
            self._guard_read_hooks = []
        elif not self._guard_read_hooks:
            self._guard_read_hooks = [self.uc.hook_add(UC_HOOK_MEM_READ, self._on_guard_read, None, lo, hi - 1)
                                      for lo, hi in self.guard_ranges]
        self.uc.ctl_flush_tb()

    # ----------------------------------------------------------------------------------------
    # extra mappings
    # ----------------------------------------------------------------------------------------
    def map_ram(self, base, size):
        """Map extra zero-filled RAM (e.g. the DDR window 0xC0000000, not mapped by default); writes are logged."""
        self.uc.mem_map(base, size, UC_PROT_ALL)
        self._ram.append((base, base + size))
        if self._counting:
            self._native_hook(UC_HOOK_MEM_WRITE, self._eh_write, base, base + size - 1)
            self.uc.ctl_flush_tb()

    def map_stub(self, base, size, name="stub", readback=False):
        """Map a peripheral stub window. readback=True stores writes and returns them on reads (RAM-like)."""
        stub = _Stub(base, size, name, bytearray(size) if readback else None)
        self._stubs.append(stub)
        self.uc.mmio_map(base, size, self._mmio_read, stub, self._mmio_write, stub)
        return stub

    def attach_nor(self, nor=None, **kw):
        """Replace the read-only NOR alias by a NorFlash device model and return it (emu.nor).

        The CPU span 0x60000000-0x61FFFFFF becomes MMIO decoded through the GPIO window latch
        OUT_DATA67[10:0] (kept by the model, read back by the stock RMW in FUN_400aa008), and
        IN_DATA23 bit 0x400 reads as RY/BY#. Host reads (emu.read_bytes) of the span are linear
        (0x60000000 + NOR offset, array contents, no polls). Backing store: the image at MainProgram offset 0x60000,
        everything else 0xFF (kw go to NorFlash(), e.g. allowed=, latency=, decode=). After this,
        code can no longer execute from the NOR alias (Unicorn cannot fetch from MMIO). NorError raised
        by the model ends emu.call() with that exception.
        """
        if self._running:
            raise RuntimeError("attach_nor() only between calls")
        if self.nor is not None:
            raise RuntimeError("a NOR model is already attached")
        nor = nor if nor is not None else NorFlash(self.image, **kw)
        uc = self.uc
        uc.mem_unmap(NOR_BASE, NOR_SIZE)
        uc.mmio_map(NOR_BASE, NOR_CPU_SIZE, self._nor_read, nor, self._nor_write, nor)

        def gpio_write(emu, addr, size, value):
            sh = 8 * (addr - GPIO_OUT_DATA67)
            mask = ((1 << (8 * size)) - 1) << sh
            nor.gpio67 = (nor.gpio67 & ~mask) | ((value << sh) & mask)

        self.periph[GPIO_OUT_DATA67] = lambda emu, addr, size: nor.gpio67
        self.on_periph_write(GPIO_OUT_DATA67, GPIO_OUT_DATA67 + 4, gpio_write)
        self.periph[GPIO_IN_DATA23] = lambda emu, addr, size: nor.ryby_word()
        uc.ctl_flush_tb()
        self.nor = nor
        return nor

    def _nor_read(self, uc, off, size, nor):
        cpu = NOR_BASE + off
        if not self._running or self._host_depth:  # host read: linear array contents, no window, no polls
            return int.from_bytes(nor.read(off & (nor.chip_size - 1), size), "little")
        pc = uc.reg_read(A.UC_ARM_REG_PC)
        if nor.busy:
            self._st.epoch += 1  # status reads change state (toggle bits, latency)
            if self._st.np_armed:
                self._polled().add(cpu)
        val = 0
        a = cpu & ~1
        for i in range(0, max(size, 2) + (cpu & 1), 2):  # one bus cycle per halfword
            val |= nor.bus_read(nor.decode_cpu(a + i, "R", pc)) << (8 * i)
        return (val >> (8 * (cpu & 1))) & ((1 << (8 * size)) - 1)

    def _nor_write(self, uc, off, size, value, nor):
        cpu = NOR_BASE + off
        pc = uc.reg_read(A.UC_ARM_REG_PC) if self._running else 0
        self._st.epoch += 1
        if size != 2 or cpu & 1:
            nor.fault = NorProtocolError(f"NOR write must be an aligned halfword (16-bit bus): size {size} at {cpu:#x} pc={pc:#x}")
            raise nor.fault
        nor.bus_write(nor.decode_cpu(cpu, "W", pc), value, cpu, pc)

    def set_readback(self, addr, on=True):
        """Switch the stub window containing addr to/from RAM-like readback (e.g. shared RAM 0x80000000)."""
        for s in self._stubs:
            if s.base <= addr < s.base + s.size:
                s.store = (s.store or bytearray(s.size)) if on else None
                return
        raise ValueError(f"{addr:#x} is not in a stub window")

    # ----------------------------------------------------------------------------------------
    # registers and memory helpers (usable from hooks as well)
    # ----------------------------------------------------------------------------------------
    @staticmethod
    def _regid(r):
        if isinstance(r, str):
            r = r.lower()
            if r == "cpsr":
                return A.UC_ARM_REG_CPSR
            r = _REG_ALIAS[r] if r in _REG_ALIAS else int(r[1:])
        return _REG_IDS[r]

    def reg(self, r):
        """Read a register: 0..15 or 'r0'..'r12','sp','lr','pc','ip','fp','sl','sb','cpsr'."""
        return self._rr(self._regid(r))

    def set_reg(self, r, value):
        self._rw(self._regid(r), value & 0xFFFFFFFF)

    pc = property(lambda s: s.reg("pc"), lambda s, v: s.set_reg("pc", v))
    sp = property(lambda s: s.reg("sp"), lambda s, v: s.set_reg("sp", v))
    lr = property(lambda s: s.reg("lr"), lambda s, v: s.set_reg("lr", v))

    def arg(self, n):
        """n-th 32-bit argument at function entry (AAPCS: r0-r3, then the stack). Use inside hooks."""
        return self.reg(n) if n < 4 else self.read_u32(self.sp + 4 * (n - 4))

    def read_bytes(self, addr, size):
        self._host_depth += 1
        try:
            return bytes(self.uc.mem_read(addr, size))
        finally:
            self._host_depth -= 1

    def write_bytes(self, addr, data):
        """Host-side write: not logged, ignores the read-only protection of the NOR alias.

        Between calls it also invalidates translated code, so patched instructions take effect
        (patching code from inside a hook is not supported)."""
        data = bytes(data)
        self._host_depth += 1
        try:
            self.uc.mem_write(addr, data)
        finally:
            self._host_depth -= 1
        if data and not self._running:
            try:
                self.uc.ctl_remove_cache(addr, addr + len(data))  # drop translated blocks of patched code
            except UcError:
                pass

    set_mem = write_bytes  # name used when patching code or rodata

    def read_u32(self, addr):
        return int.from_bytes(self.read_bytes(addr, 4), "little")

    def write_u32(self, addr, value):
        self.write_bytes(addr, (value & 0xFFFFFFFF).to_bytes(4, "little"))

    def read_cstr(self, addr, maxlen=4096):
        out = bytearray()
        while len(out) < maxlen:
            b = self.read_bytes(addr + len(out), 1)
            if b == b"\0":
                break
            out += b
        return bytes(out)

    def write_cstr(self, addr, text):
        data = text.encode() if isinstance(text, str) else bytes(text)
        self.write_bytes(addr, data + b"\0")

    def disasm(self, addr, count=8):
        """Disassemble `count` ARM instructions from the current memory contents (needs capstone)."""
        import capstone

        md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
        return [f"{i.address:08x}: {i.mnemonic} {i.op_str}".rstrip() for i in md.disasm(self.read_bytes(addr, 4 * count), addr)]

    # ----------------------------------------------------------------------------------------
    # hooks
    # ----------------------------------------------------------------------------------------
    def hook(self, addr, fn, name=None):
        """Replace the function at addr with Python code fn(emu).

        fn runs when execution reaches addr. It can use emu.reg/set_reg/arg, the memory helpers,
        emu.stop(reason) and emu.jump(addr). Its return value chooses what happens next:
          None            return to the caller (pc = lr), r0 unchanged
          int             r0 = value, return to the caller
          StockEmu.CONTINUE   do not replace anything, execute the original instruction at addr
        (emu.jump(addr) inside fn overrides the return: execution continues at addr).
        """
        if self._running:
            raise RuntimeError("hooks can only be changed between calls")
        if addr & 3:
            raise ValueError("hook address must be 4-byte aligned (ARM mode only)")
        if addr in self._hooks:
            self.unhook(addr)
        self._hooks[addr] = (fn, None, name or getattr(fn, "__name__", "hook"))
        self._set_hooks_native()
        self.uc.ctl_flush_tb()

    def unhook(self, addr):
        if self._running:
            raise RuntimeError("hooks can only be changed between calls")
        self._hooks.pop(addr)
        self._set_hooks_native()
        self.uc.ctl_flush_tb()

    def _set_hooks_native(self):
        """The hooked addresses into eh_code's table.  eh_code is one code hook over every address (Unicorn's count
        hook already calls the code hooks before every instruction - every call runs with count > 0), so no
        translated block depends on the hook set.  hook / unhook still flush the translated blocks as before: the
        round trips count on it (a site the emulated CPU rewrote - ota_hook_set's poke - runs its new word after
        the next hook change; Unicorn does not see that store as self-modifying code)."""
        if len(self._hooks) > _EH_HTAB // 2:
            raise ValueError(f"at most {_EH_HTAB // 2} hooked addresses")
        arr = (ctypes.c_uint64 * max(1, len(self._hooks)))(*self._hooks)
        self._eh.eh_set_hooks(ctypes.byref(self._st), arr, len(self._hooks))

    def on_periph_write(self, lo, hi, fn):
        """Call fn(emu, addr, size, value) for peripheral-stub writes with lo <= addr < hi (e.g. to change emu.periph)."""
        self._write_hooks.append((lo, hi, fn))

    def jump(self, addr):
        """From inside a hook: continue at addr instead of returning to lr."""
        if not self._running:
            raise RuntimeError("jump() is only valid inside a hook")
        self._jump = addr

    def stop(self, reason="stopped", **info):
        """From inside a hook: end the call; CallResult.stop = reason."""
        if not self._running:
            raise RuntimeError("stop() is only valid inside a hook")
        if self._stop is None:
            self._stop = (reason, info)
        self.uc.emu_stop()

    # ----------------------------------------------------------------------------------------
    # running code
    # ----------------------------------------------------------------------------------------
    def call(
        self,
        addr,
        r0=0,
        r1=0,
        r2=0,
        r3=0,
        stack=(),
        *,
        regs=None,
        until=None,
        max_insns=DEFAULT_MAX_INSNS,
        trace=False,
        no_progress=DEFAULT_NO_PROGRESS,
        stop_on_guard=None,
    ):
        """Run the ARM function at addr with AAPCS arguments r0-r3 + stack words, in SVC mode, sp=0x41000000.

        regs: extra register presets, e.g. {'ip': 5, 'r4': 1}. until: address (or list) where to stop
        instead of/in addition to the function return (for snippets). no_progress: blocks between loop
        checks (0/False = off). trace=True records executed basic-block start addresses.
        Python exceptions raised in hooks propagate to the caller.
        """
        if self.uc is None:
            raise RuntimeError("emulator is closed")
        if self._running:
            raise RuntimeError("StockEmu.call() is not re-entrant")
        if addr & 3:
            raise ValueError("call address must be 4-byte aligned (ARM mode only)")
        exits = [RET_TRAP]
        if until is not None:
            exits += [int(a) for a in (until if isinstance(until, (list, tuple, set)) else [until])]
        self._reset_run_state(trace, int(no_progress or 0), self.stop_on_guard if stop_on_guard is None else stop_on_guard)
        self._setup_cpu(r0, r1, r2, r3, stack, regs)
        self._running = True
        try:
            err = self._execute(addr, exits, int(max_insns))
        finally:
            self._running = False
            self._jump = None
        return self._finish(err, exits, int(max_insns))

    def _reset_run_state(self, trace, np_window, stop_on_guard):
        self._periph_log, self._guard_log = [], []
        self._trace_on = bool(trace)
        # the block / store counters and logs live in the C state (emuhooks.c)
        st = self._st
        st.log_limit = max(0, self._log_limit)
        st.dropped = st.insns = st.blk_start = st.blk_len = st.nblk = st.epoch = 0
        st.blk_shift = 2      # log2 of the instruction size of the current block: 2 ARM, 1 Thumb (halfwords)
        st.trace_on = int(bool(trace))
        st.np_window, st.np_armed, st.np_next, st.np_armed_at, st.np_tries = np_window, 0, np_window, 0, 0
        st.wlen = st.tlen = 0
        self._np_polled = set()
        self._guard_stop = bool(stop_on_guard)
        self._stop = None
        self._fault = None

    def _setup_cpu(self, r0, r1, r2, r3, stack, regs):
        uc = self.uc
        if self._sctlr() != self._sctlr0:  # an earlier call turned the MMU/caches on
            uc.cpr_write(15, 0, 1, 0, 0, 0, False, self._sctlr0)
        sp = STACK_TOP - ((len(stack) * 4 + 7) & ~7)
        for i, v in enumerate(stack):
            self.write_u32(sp + 4 * i, v)
        # CPSR (SVC mode) first, then r0-r12, then the SVC bank's sp and lr - one batch, in this order
        self._write_setup([CPSR_SVC, r0 & 0xFFFFFFFF, r1 & 0xFFFFFFFF, r2 & 0xFFFFFFFF, r3 & 0xFFFFFFFF,
                           0, 0, 0, 0, 0, 0, 0, 0, 0, sp, RET_TRAP])
        for r, v in (regs or {}).items():
            self.set_reg(r, v)

    def _execute(self, entry, exits, max_insns):
        exits = tuple(dict.fromkeys(exits))
        if exits != self._exits:
            self.uc.ctl_set_exits(list(exits))
            # Unicorn checks an exit only when it translates a block starting there: a block already cached at the new
            # exit address (e.g. a function entry run before) would run through it.  Drop the cache with the change.
            self.uc.ctl_flush_tb()
            self._exits = exits
        try:
            # count>0 always: Unicorn drops translated blocks when the count hook is added/removed
            self.uc.emu_start(entry, 0, count=max(1, max_insns))
        except UcError as e:
            return e
        return None

    def _finish(self, err, exits, max_insns):
        uc = self.uc
        regs = self._read_all()
        cpsr = regs.pop()
        pc = regs[15]
        st = self._st
        if st.blk_len:  # block in progress at the stop: count only what ran
            off = pc - st.blk_start
            sh = st.blk_shift
            st.insns += (off >> sh) if 0 <= off < (st.blk_len << sh) else st.blk_len
        insns = st.insns
        info = {}
        if self._stop is not None:
            stop, info = self._stop
        elif err is not None:
            stop = _ERR_STOP.get(err.errno, "uc_error")
            info = dict(self._fault or {})
            info.update(errno=err.errno, error=str(err))
            if stop in ("invalid_insn", "exception"):
                info.setdefault("pc", pc)
        elif pc in exits:
            stop = "returned" if pc == RET_TRAP else "until"
        elif insns >= max_insns:
            stop = "max_insns"
        else:  # Unicorn ended the run quietly, e.g. WFI (mcr p15,0,rX,c7,c0,4) halts the core
            stop, info = "halted", {"pc": pc}
        return CallResult(
            stop=stop,
            r0=regs[0],
            pc=pc,
            regs=regs,
            cpsr=cpsr,
            insns=insns,
            writes=self._native_writes(),
            periph=self._periph_log,
            guard=self._guard_log,
            trace=self._native_trace(),
            info=info,
            log_dropped=st.dropped,
        )

    def writes_so_far(self):
        """From inside a hook: how many Write records this call has logged up to now."""
        return self._st.wlen

    def _native_writes(self):
        """The C store log of this call as Write records (and the C buffer released when it grew large)."""
        st = self._st
        out = WriteLog()
        n = st.wlen
        if n:
            out._a.frombytes(ctypes.string_at(st.wbuf, 32 * n))
        st.wlen = 0
        if st.wcap > 1 << 22:
            self._eh.eh_free(ctypes.byref(st))
        return out

    def _native_trace(self):
        st = self._st
        if not self._trace_on:
            return []
        out = memoryview(ctypes.string_at(st.tbuf, 8 * st.tlen)).cast("Q").tolist() if st.tlen else []
        st.tlen = 0
        return out

    # ----------------------------------------------------------------------------------------
    # Unicorn callbacks
    # ----------------------------------------------------------------------------------------
    def _log(self, lst, entry):
        if len(lst) < self._log_limit:
            lst.append(entry)
        else:
            self._st.dropped += 1

    def _on_fn_hook(self, uc, address, size, _ud):
        ent = self._hooks.get(address)
        if ent is None:
            return
        st = self._st
        st.epoch += 1  # a Python hook may change anything, so it counts as progress
        self._jump = None
        ret = ent[0](self)
        if self._jump is not None:
            target, self._jump = self._jump, None
        elif ret is StockEmu.CONTINUE:
            return
        else:
            if ret is not None:
                self._rw(A.UC_ARM_REG_R0, ret & 0xFFFFFFFF)
            target = self._rr(A.UC_ARM_REG_LR)
        if self._counting:  # instructions of the current block before the hook, plus the replaced call
            st.insns += ((address - st.blk_start) >> st.blk_shift) + 1
            st.blk_len = 0
        self._rw(A.UC_ARM_REG_PC, target)

    # The block hook (instruction count, Thumb state, trace, no-progress detector) and the RAM store hook (Write log,
    # epoch) are C callbacks: emuhooks.c eh_block / eh_write.  Two rare events come back to Python:
    def _on_no_progress(self, addr):
        # Sound loop detector (eh_block): the machine is deterministic, so the same block entered with the same
        # registers/flags and no memory write, hook call or stateful peripheral read in between
        # can never make progress.  eh_block stops the run after this.
        self._stop = ("no_progress", {"pc": addr, "polled": sorted(self._polled())})

    def _polled(self):
        """The stateful peripheral addresses read since the detector armed (empty set after a new arming)."""
        g = self._st.np_gen
        if self._np_gen != g:
            self._np_gen, self._np_polled = g, set()
        return self._np_polled

    def _on_guard_write(self, addr, size, value, pc):
        # a RAM store into a guard range (eh_write logged the Write and found the range)
        self._log(self._guard_log, GuardHit("W", addr, size, value, pc))
        if self._guard_stop:
            self._stop = ("guard", {"op": "W", "addr": addr, "pc": pc})
            self.uc.emu_stop()

    def _on_guard_read(self, uc, access, addr, size, value, _ud):
        pc = self._rr(A.UC_ARM_REG_PC)
        val = int.from_bytes(uc.mem_read(addr, size), "little")
        self._log(self._guard_log, GuardHit("R", addr, size, val, pc))
        if self._guard_stop:
            self._stop = ("guard", {"op": "R", "addr": addr, "pc": pc})
            uc.emu_stop()

    def _on_guard_exec(self, uc, address, size, _ud):
        self._log(self._guard_log, GuardHit("X", address, 4, 0, address))
        self._stop = ("guard_exec", {"pc": address})
        uc.emu_stop()

    def _on_fault(self, uc, access, addr, size, value, _ud):
        self._fault = {
            "access": _ACCESS_NAME.get(access, str(access)),
            "addr": addr,
            "size": size,
            "value": value & ((1 << (8 * size)) - 1) if size else value,
            "pc": uc.reg_read(A.UC_ARM_REG_PC),
        }
        return False  # let Unicorn raise the error

    def _on_intr(self, uc, intno, _ud):
        pc = uc.reg_read(A.UC_ARM_REG_PC)
        if intno == 3 and pc in self._exits:  # code switched the MMU on (FUN_4009d198) and the trap/until address
            uc.emu_stop()  # is not mapped by its tables: the call did reach its end
            return
        if intno == 2:  # SWI; pc is already past the instruction
            word = int.from_bytes(uc.mem_read(pc - 4, 4), "little")
            self._stop = ("swi", {"number": word & 0xFFFFFF, "pc": pc - 4})
        else:
            self._stop = ("exception", {"intno": intno, "pc": pc})
        uc.emu_stop()

    # peripheral stubs ---------------------------------------------------------------------
    def _eval_periph(self, addr, v, size):
        if callable(v):
            self._st.epoch += 1
            return int(v(self, addr, size))
        if isinstance(v, (list, tuple)):
            ent = self._seq.get(addr)  # (sequence object, next index); a new object restarts at 0
            i = ent[1] if ent is not None and ent[0] is v else 0
            if i < len(v) - 1:  # stateful until the last element, which then repeats
                self._seq[addr] = (v, i + 1)
                self._st.epoch += 1
            return int(v[i])
        return int(v)

    def _periph_value(self, stub, addr, size):
        mask = (1 << (8 * size)) - 1
        d = self.periph
        if addr in d:
            return self._eval_periph(addr, d[addr], size) & mask
        base = addr & ~3
        if base != addr and base in d:  # byte/halfword access to a 32-bit register entry
            return (self._eval_periph(base, d[base], 4) >> (8 * (addr - base))) & mask
        if stub.store is not None:
            off = addr - stub.base
            return int.from_bytes(stub.store[off : off + size], "little")
        return 0

    def _mmio_read(self, uc, off, size, stub):
        addr = stub.base + off
        val = self._periph_value(stub, addr, size)
        if self._running and not self._host_depth:
            self._log(self._periph_log, PeriphAccess("R", addr, size, val, self._rr(A.UC_ARM_REG_PC)))
            if self._st.np_armed:
                self._polled().add(addr)
        return val

    def _mmio_write(self, uc, off, size, value, stub):
        addr = stub.base + off
        value &= (1 << (8 * size)) - 1
        if stub.store is not None:
            stub.store[off : off + size] = value.to_bytes(size, "little")
            self._st.epoch += 1
        if self._running and not self._host_depth:
            self._log(self._periph_log, PeriphAccess("W", addr, size, value, self._rr(A.UC_ARM_REG_PC)))
        for lo, hi, fn in self._write_hooks:
            if lo <= addr < hi:
                self._st.epoch += 1
                fn(self, addr, size, value)


def describe(emu):
    """Human-readable memory map summary (used by the command line)."""
    lines = [f"image sha256 {emu.sha256} ({'stock' if emu.is_stock else 'NOT the stock image'})"]
    lines.append(f"SDRAM  {SDRAM_BASE:#010x}-{SDRAM_BASE + SDRAM_SIZE - 1:#010x} rw")
    if emu.nor is None:
        lines.append(f"NOR    {NOR_BASE:#010x}-{NOR_BASE + NOR_SIZE - 1:#010x} ro (image at {FLASH_BASE:#x}, rest 0xFF)")
    else:
        n = emu.nor
        lines.append(f"NOR    {NOR_BASE:#010x}-{NOR_BASE + NOR_CPU_SIZE - 1:#010x} device model ({n.chip_size:#x} B, "
                     f"decode={n.decode}, window={n.window:#x}, state={n.state})")
        lines += [f"  allowed   {lo:#08x}-{hi - 1:#08x}" for lo, hi in n.allowed]
        lines += [f"  protected {r[0]:#08x}-{r[1] - 1:#08x} {r[2]}" for r in n.protected]
    lines.append(f"vector {VECTOR_PAGE:#010x}-{VECTOR_PAGE + 0xFFF:#010x} rw")
    for s in emu._stubs:
        lines.append(f"stub   {s.base:#010x}-{s.base + s.size - 1:#010x} {s.name}")
    lines.append("loader copies (src, dst, len):")
    lines += [f"  {s:#010x} -> {d:#010x}  {n:#x}" for s, d, n in emu.loader_copies]
    lines.append("guard ranges:")
    lines += [f"  {lo:#010x}-{hi - 1:#010x}  ({hi - lo:#x} bytes)" for lo, hi in emu.guard_ranges]
    return "\n".join(lines)


if __name__ == "__main__":
    print(describe(StockEmu(sys.argv[1] if len(sys.argv) > 1 else None)))
