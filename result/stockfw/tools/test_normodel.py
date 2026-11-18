# Tests for the NOR flash device model in tools/stockemu.py (StockEmu.attach_nor / NorFlash).
#
#   cd result/stockfw && uv run python -I tools/test_normodel.py [-v] [test_name ...]
#
# The program/erase routines exercised here are the STOCK ones, run
# unmodified against the model on a scratch sector inside the remote-update slots (NOR 0x180000-0x1FFFFF).
# Expected bytes come from Python references, never from the model itself.
import importlib.util
import pathlib
import sys
import time
import traceback

sys.dont_write_bytecode = True
_HERE = pathlib.Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("stockemu", _HERE / "stockemu.py")
se = importlib.util.module_from_spec(_spec)
sys.modules["stockemu"] = se
_spec.loader.exec_module(se)

from keystone import KS_ARCH_ARM, KS_MODE_ARM, Ks  # noqa: E402

IMG = se.stock_bytes()
SCRATCH = 0x40800000  # code
BUF = 0x40810000  # data buffers
SECTOR = 0x1C0000  # scratch sector (NOR offset) inside NOR_UPDATE_SLOTS
CPU = se.NOR_BASE + SECTOR
_ks = Ks(KS_ARCH_ARM, KS_MODE_ARM)

# stock routines
FUN_WINSEL = se.FUN_WINDOW_SELECT  # 0x400aa008
FUN_COPY = se.FUN_COPY  # 0x400aa030
FUN_PROG_HW = 0x401339EC  # (addr, hw) unlock + A0 + data, wait FUN_401339a0 -> 0 ok / 1 timeout
FUN_PROG_LOOP = 0x40133B0C  # (addr, src, len) halfword loop, odd tail padded with 0xFF
FUN_PROG_BUF = 0x40133D50  # (addr, src, len<=512) write-buffer program, wait FUN_40133cfc
FUN_PROG_SPLIT = 0x40133F60  # (addr, src, len) head halfwords up to a 512 B boundary, then buffers
FUN_ERASE = 0x40133C38  # (addr) sector erase, wait FUN_40133b68
FUN_RESET = 0x40133CA8  # F0 at 0xAAA (window 0)
FUN_RESET_UNLOCK = 0x40133CC4  # AA, 55, F0
FUN_DLY_TSK = 0x40089658  # RTOS dly_tsk(n): the erase / buffer waits yield through it
FUN_BUSY_SET = 0x4013403C  # *0x4139dac4 = r0
FUN_TOUCHADJ_SAVE = 0x400B0BFC  # stock caller #6: busy=1, erase 0x60440000, program 0x84 B, busy=0
BUSY_FLAG = 0x4139DAC4


def asm(src, addr=SCRATCH):
    enc, _n = _ks.asm(src, addr)
    return bytes(enc)


def run_code(emu, src, *args, **kw):
    emu.write_bytes(SCRATCH, asm(src))
    return emu.call(SCRATCH, *args, **kw)


def raises(exc, fn, *a, **k):
    try:
        fn(*a, **k)
    except exc as e:
        return e
    raise AssertionError(f"{exc.__name__} not raised")


def pattern(n, seed=1):
    return bytes((i * 37 + seed * 11 + (i >> 8)) & 0xFF for i in range(n))


def make(**kw):
    emu = se.StockEmu()
    nor = emu.attach_nor(**kw)
    emu.hook(FUN_DLY_TSK, lambda e: 0, name="dly_tsk")  # no RTOS: a yield returns at once
    return emu, nor


def ryby_reads(res):
    return [p for p in res.periph if p.op == "R" and p.addr == se.GPIO_IN_DATA23]


# --------------------------------------------------------------------------------------------
# backing store, window decode
# --------------------------------------------------------------------------------------------
def test_backing_store_and_window_read():
    emu, nor = make()
    assert nor.read(0, 0x60000) == b"\xff" * 0x60000  # BootProgram: not loaded (CA79NOR.BIN is never used)
    assert nor.read(se.MAIN_OFFSET, len(IMG)) == IMG
    assert nor.read(se.MAIN_OFFSET + len(IMG), nor.chip_size - se.MAIN_OFFSET - len(IMG)).count(0xFF) == \
        nor.chip_size - se.MAIN_OFFSET - len(IMG)
    assert emu.read_bytes(se.FLASH_BASE, 64) == IMG[:64]  # host reads are linear
    # the stock copier reads through the window and crosses window boundaries correctly
    res = emu.call(FUN_COPY, se.FLASH_BASE + 0x7FF0, BUF, 0x10020)
    assert res.ok, res
    assert emu.read_bytes(BUF, 0x10020) == IMG[0x7FF0:0x18010]
    assert nor.window == ((0x60000 + 0x18000) >> 15) and nor.window_mismatch == []
    win = [p.value for p in res.periph if p.op == "W" and p.addr == se.GPIO_OUT_DATA67]
    assert win == [0, 0xC, 0, 0xD, 0, 0xE, 0, 0xF], win  # clear, set per 32 KB
    # window decode: with window 0xD selected, CPU 0x60000000 reads NOR 0x68000 (alias) - NOR address window hypothesis
    assert emu.call(FUN_WINSEL, se.FLASH_BASE + 0x8000).ok and nor.window == 0xD
    res = run_code(emu, "ldr r1, =0x60000000\n ldr r0, [r1]\n bx lr")
    assert res.r0 == int.from_bytes(IMG[0x8000:0x8004], "little"), hex(res.r0)
    assert [m.cpu for m in nor.window_mismatch] == [0x60000000, 0x60000002]
    # GP6[15:11] / GP7 bits survive the stock RMW of the window select
    nor.gpio67 = 0xABCD0000 | 0xF800 | 0x123
    assert emu.call(FUN_WINSEL, se.NOR_BASE + 0x50 * 0x8000).ok
    assert nor.gpio67 == 0xABCD0000 | 0xF800 | 0x50, hex(nor.gpio67)
    # code cannot run from the modelled NOR (MMIO); it was possible from the plain alias
    assert emu.call(se.FLASH_BASE + (0x400A86E0 - se.IMAGE_BASE)).stop == "prot"  # fetch_prot
    emu.close()


# --------------------------------------------------------------------------------------------
# stock program / erase routines on the scratch sector
# --------------------------------------------------------------------------------------------
def test_stock_sector_erase():
    emu, nor = make(latency={"erase": 20})
    junk = pattern(se.NOR_SECTOR, 3)
    nor.load(SECTOR, junk)
    before = bytes(nor.mem)
    res = emu.call(FUN_ERASE, CPU + 0x1234)
    assert res.ok and res.r0 == 0, res
    assert nor.read(SECTOR, se.NOR_SECTOR) == b"\xff" * se.NOR_SECTOR
    assert nor.mem[:SECTOR] == before[:SECTOR] and nor.mem[SECTOR + se.NOR_SECTOR :] == before[SECTOR + se.NOR_SECTOR :]
    assert nor.ops == [se.NorOp("erase", SECTOR, se.NOR_SECTOR, None, nor.ops[0].pc)]
    assert [(c.offset, c.value, c.window) for c in nor.cycles] == [
        (0xAAA, 0xAA, 0), (0x554, 0x55, 0), (0xAAA, 0x80, 0), (0xAAA, 0xAA, 0), (0x554, 0x55, 0),
        (SECTOR + 0x1234, 0x30, (SECTOR + 0x1234) >> 15)]
    assert nor.ops[0].pc == 0x40133C90  # the 0x30 store in FUN_40133c38
    # completion is judged by RY/BY# (IN_DATA23 bit 0x400) and then reading 0xFFFF back
    rb = ryby_reads(res)
    assert len(rb) == 21 and all(p.value & se.NOR_RYBY_BIT == 0 for p in rb[:20]) and rb[20].value & se.NOR_RYBY_BIT
    assert nor.state == "read" and nor.polls == 20
    emu.close()


def test_stock_halfword_program():
    emu, nor = make(latency={"program": 3})
    res = emu.call(FUN_PROG_HW, CPU + 0x10, 0x1234)
    assert res.ok and res.r0 == 0, res
    assert nor.read(SECTOR + 0x10, 2) == b"\x34\x12" and nor.read(SECTOR, 0x10) == b"\xff" * 0x10
    assert [(c.offset, c.value) for c in nor.cycles] == [(0xAAA, 0xAA), (0x554, 0x55), (0xAAA, 0xA0), (SECTOR + 0x10, 0x1234)]
    assert len(ryby_reads(res)) == 4 and nor.polls == 3
    # halfword loop: odd length, the tail byte is padded with 0xFF
    data = pattern(0x41, 5)
    emu.write_bytes(BUF, data)
    res = emu.call(FUN_PROG_LOOP, CPU + 0x100, BUF, len(data))
    assert res.ok and res.r0 == 0, res
    assert nor.read(SECTOR + 0x100, 0x44) == data + b"\xff\xff\xff"
    assert len(nor.ops) == 1 + 0x21 and {o.kind for o in nor.ops} == {"program"}
    # 0 -> 1 is refused (a real chip fails with DQ5)
    e = raises(se.NorProtocolError, emu.call, FUN_PROG_HW, CPU + 0x10, 0x00FF | 0x1200 | 0x2000)
    assert "0->1" in str(e) and nor.read(SECTOR + 0x10, 2) == b"\x34\x12"
    emu.close()


def test_stock_buffer_program():
    emu, nor = make(latency={"buffer": 6, "program": 2})
    data = pattern(0x200, 7)
    emu.write_bytes(BUF, data)
    res = emu.call(FUN_PROG_BUF, CPU + 0x400, BUF, 0x200)
    assert res.ok and res.r0 == 0, res
    assert nor.read(SECTOR + 0x400, 0x200) == data
    assert nor.ops == [se.NorOp("buffer", SECTOR + 0x400, 0x200, data, nor.ops[0].pc)]
    cyc = [(c.offset, c.value) for c in nor.cycles]
    assert cyc[:4] == [(0xAAA, 0xAA), (0x554, 0x55), (SECTOR, 0x25), (SECTOR, 0xFF)]  # 25 + count-1 at the sector address
    assert len(cyc) == 4 + 0x100 + 1 and cyc[-1] == (SECTOR, 0x29)
    assert nor.window_mismatch == []  # data in the sector's first 32 KB: pointer and window agree
    # split routine: halfword head up to a 512 B boundary, then buffer programs
    data = pattern(0x500, 9)
    emu.write_bytes(BUF, data)
    nor.ops.clear()
    res = emu.call(FUN_PROG_SPLIT, CPU + 0x8000 - 0x100, BUF, len(data))
    assert res.ok, res
    assert nor.read(SECTOR + 0x8000 - 0x100, len(data)) == data
    kinds = [(o.kind, o.offset - SECTOR, o.size) for o in nor.ops]
    assert kinds[:0x80] == [("program", 0x7F00 + 2 * i, 2) for i in range(0x80)]
    assert kinds[0x80:] == [("buffer", 0x8000, 0x200), ("buffer", 0x8200, 0x200)], kinds[0x80:]
    # odd length: last word padded with 0xFF
    data = pattern(0x1FF, 11)
    emu.write_bytes(BUF, data)
    res = emu.call(FUN_PROG_BUF, CPU + 0x10000, BUF, len(data))
    assert res.ok and res.r0 == 0, res
    assert nor.read(SECTOR + 0x10000, 0x200) == data + b"\xff"
    emu.close()


def test_buffer_program_window_vs_linear_decode():
    """Both address decodes give the same result for the stock routines; only the window decode can see
    where CPU A[24:15] disagree with the selected window."""
    data = pattern(0x200, 13)
    out = {}
    for mode in ("window", "linear"):
        emu, nor = make(decode=mode)
        emu.write_bytes(BUF, data)
        assert emu.call(FUN_PROG_BUF, CPU + 0x18200, BUF, 0x200).r0 == 0
        assert emu.call(FUN_ERASE, CPU).r0 == 0
        assert emu.call(FUN_PROG_BUF, CPU + 0x18200, BUF, 0x200).r0 == 0
        out[mode] = (bytes(nor.mem), [o[:4] for o in nor.ops], [c.offset for c in nor.cycles if c.value == 0x29],
                     list(nor.window_mismatch))
        emu.close()
    assert out["window"][0] == out["linear"][0] and out["window"][1] == out["linear"][1]
    # finding: FUN_40133d50 writes the 0x29 confirm through the sector-base pointer (A[24:15] = 0x38) while the
    # window still selects the data's 32 KB window (0x3B). Under the window decode it lands at 0x1D8000, under a
    # linear decode at 0x1C0000 - both inside the same 128 KB sector, so the chip accepts it either way.
    assert out["linear"][2] == [SECTOR] * 2 and out["window"][2] == [SECTOR + 0x18000] * 2, out["window"][2]
    assert out["linear"][3] == []
    mm = out["window"][3]
    assert [(m.op, m.cpu, m.window) for m in mm] == [("W", CPU, 0x3B)] * 2, mm


# --------------------------------------------------------------------------------------------
# status polling (DQ7 / DQ6 / DQ3 / DQ2) through CPU reads
# --------------------------------------------------------------------------------------------
_CMD = """
push {r4-r8, lr}
mov r4, r0
mov r5, r1
mov r6, r2
mov r7, r3
ldr r0, =0x60000aaa
bl 0x400aa008
ldr r1, =0x60000aaa
ldr r2, =0x60000554
%s
mov r0, r4
bl 0x400aa008
%s
loop:
ldrh r3, [r4]
strh r3, [r6], #2
subs r7, r7, #1
bne loop
pop {r4-r8, pc}
"""
_PROGRAM = _CMD % ("mov r3,#0xaa\n strh r3,[r1]\n mov r3,#0x55\n strh r3,[r2]\n mov r3,#0xa0\n strh r3,[r1]",
                   "strh r5, [r4]")
_ERASE = _CMD % ("mov r3,#0xaa\n strh r3,[r1]\n mov r3,#0x55\n strh r3,[r2]\n mov r3,#0x80\n strh r3,[r1]\n"
                 " mov r3,#0xaa\n strh r3,[r1]\n mov r3,#0x55\n strh r3,[r2]", "mov r3,#0x30\n strh r3,[r4]")


def test_status_polling_bits():
    n = 5
    emu, nor = make(latency={"program": n, "erase": n})
    res = run_code(emu, _PROGRAM, CPU + 0x20, 0x00C3, BUF, n + 2)
    assert res.ok, res
    reads = [int.from_bytes(emu.read_bytes(BUF + 2 * i, 2), "little") for i in range(n + 2)]
    st = reads[:n]
    assert all(s & 0x80 == (~0xC3 & 0x80) for s in st)  # DQ7 = complement of data bit 7
    assert [s & 0x40 for s in st] == [0x40, 0, 0x40, 0, 0x40]  # DQ6 toggles on every read
    assert all(s & ~0xC0 == 0 for s in st)  # DQ5/DQ3/DQ2/DQ1 = 0, upper byte 0
    assert reads[n:] == [0x00C3, 0x00C3]  # then read-array data, on its own
    # erase: DQ7 = 0, DQ3 = 1, DQ6 and DQ2 toggle on the erasing sector
    res = run_code(emu, _ERASE, CPU + 0x40, 0, BUF, n + 1)
    assert res.ok, res
    reads = [int.from_bytes(emu.read_bytes(BUF + 2 * i, 2), "little") for i in range(n + 1)]
    assert [s & 0xCC for s in reads[:n]] == [0x4C, 0x08, 0x4C, 0x08, 0x4C], [hex(s) for s in reads]
    assert reads[n] == 0xFFFF and nor.read(SECTOR, se.NOR_SECTOR) == b"\xff" * se.NOR_SECTOR
    # latency 0: no busy phase at all
    emu2, nor2 = make(latency={"program": 0})
    res = run_code(emu2, _PROGRAM, CPU + 0x20, 0x1234, BUF, 1)
    assert res.ok and emu2.read_bytes(BUF, 2) == b"\x34\x12" and nor2.polls == 0
    emu.close()
    emu2.close()


# --------------------------------------------------------------------------------------------
# guard
# --------------------------------------------------------------------------------------------
def test_out_of_range_erase_is_trapped():
    emu, nor = make()
    before = bytes(nor.mem)
    for addr, what in ((0x60400000, "UserMemory"), (0x60000000, "BootProgram"), (0x60060000, "MainProgram"),
                       (0x604A0000, "TouchCalib"), (0x60460000, "config blocks"), (0x60500000, "outside the allowed")):
        nor.reset()
        e = raises(se.NorGuardError, emu.call, FUN_ERASE, addr)
        assert what in str(e), (addr, str(e))
        assert nor.fault is e and nor.state == "read"
        raises(se.NorGuardError, nor.bus_write, 0xAAA, 0xAA)  # faulted chip refuses further cycles
    assert nor.ops == [] and bytes(nor.mem) == before
    # a real stock caller: TouchAdjust save erases 0x60440000 -> trapped
    nor.reset()
    e = raises(se.NorGuardError, emu.call, FUN_TOUCHADJ_SAVE)
    assert "TouchAdjust" in str(e) and "pc=0x40133c90" in str(e)
    assert emu.read_u32(BUSY_FLAG) == 1 and bytes(nor.mem) == before  # it raised between busy=1 and busy=0
    # program outside the allowed ranges
    nor.reset()
    e = raises(se.NorGuardError, emu.call, FUN_PROG_HW, 0x60060000, 0)
    assert "MainProgram" in str(e)
    # a sector straddling the slot end: the slot is erasable, the next sector is not
    nor.reset()
    assert emu.call(FUN_ERASE, se.NOR_BASE + 0x1E0000).r0 == 0
    raises(se.NorGuardError, emu.call, FUN_ERASE, se.NOR_BASE + 0x200000)
    # chip erase is always rejected
    nor.reset()
    chip = "mov r3,#0xaa\n strh r3,[r1]\n mov r3,#0x55\n strh r3,[r2]\n mov r3,#0x80\n strh r3,[r1]\n" \
           " mov r3,#0xaa\n strh r3,[r1]\n mov r3,#0x55\n strh r3,[r2]\n mov r3,#0x10\n strh r3,[r1]"
    e = raises(se.NorGuardError, run_code, emu, _CMD % (chip, ""), CPU, 0, BUF, 1)
    assert "chip erase" in str(e)
    assert bytes(nor.mem[: se.NOR_SECTOR * 0x0F]) == before[: se.NOR_SECTOR * 0x0F]
    emu.close()


def test_guard_configuration():
    raises(ValueError, se.NorFlash, IMG, allowed=[(0x3E0000, 0x420000)])  # overlaps MainProgram / UserMemory
    raises(ValueError, se.NorFlash, IMG, allowed=[(0x000000, 0x020000)])
    emu, nor = make(allowed=[(0x500000, 0x600000)])  # RecorderData only
    assert emu.call(FUN_ERASE, 0x60520000).r0 == 0
    raises(se.NorGuardError, emu.call, FUN_ERASE, CPU)  # the update slot is not allowed now
    emu.close()
    # explicit protected list replaces the default
    nor = se.NorFlash(IMG, allowed=[(0x400000, 0x440000)], protected=[(0, 0x60000, "BootProgram")])
    assert nor.allowed == [(0x400000, 0x440000)]


def test_protocol_errors():
    emu, nor = make(latency={"program": 4})
    e = raises(se.NorProtocolError, run_code, emu, "ldr r1, =0x601c0000\n mov r0, #0\n strh r0, [r1]\n bx lr")
    assert "stray write" in str(e)
    nor.reset()
    e = raises(se.NorProtocolError, run_code, emu, "ldr r1, =0x601c0000\n mov r0, #0\n str r0, [r1]\n bx lr")
    assert "halfword" in str(e)
    nor.reset()
    raises(se.NorProtocolError, run_code, emu, "ldr r1, =0x60000aaa\n mov r0, #0xaa\n strh r0, [r1]\n"
           " ldr r1, =0x60000556\n mov r0, #0x55\n strh r0, [r1]\n bx lr")  # bad second unlock address
    nor.reset()
    raises(se.NorProtocolError, run_code, emu, "ldr r1, =0x60000aaa\n mov r0, #0x98\n strh r0, [r1]\n bx lr")  # CFI
    emu.close()


# --------------------------------------------------------------------------------------------
# read-array return
# --------------------------------------------------------------------------------------------
def test_read_array_return():
    emu, nor = make(latency={"program": 3})
    # 1. after an embedded operation the chip is back in read-array mode without a reset (stock assumption)
    assert emu.call(FUN_PROG_HW, CPU + 0x30, 0x5AA5).r0 == 0
    assert nor.state == "read"
    assert emu.call(FUN_COPY, CPU + 0x30, BUF, 4).ok and emu.read_bytes(BUF, 4) == b"\xa5\x5a\xff\xff"
    # 2. the two stock reset routines (no static callers) are accepted in read mode
    n = len(nor.cycles)
    assert emu.call(FUN_RESET).ok and emu.call(FUN_RESET_UNLOCK).ok
    assert [(c.offset, c.value) for c in nor.cycles[n:]] == [(0xAAA, 0xF0), (0xAAA, 0xAA), (0x554, 0x55), (0xAAA, 0xF0)]
    assert nor.state == "read" and nor.ops[-1].kind == "program"
    # 3. F0 inside a command sequence aborts it: unlock, unlock, then FUN_40133ca8; reads give array data
    run_code(emu, "ldr r1, =0x60000aaa\n mov r0, #0xaa\n strh r0, [r1]\n ldr r1, =0x60000554\n mov r0, #0x55\n"
             " strh r0, [r1]\n bx lr")
    assert nor.state == "u2"
    assert emu.call(FUN_RESET).ok and nor.state == "read"
    assert emu.call(FUN_COPY, CPU + 0x30, BUF, 4).ok and emu.read_bytes(BUF, 4) == b"\xa5\x5a\xff\xff"
    # 4. a reset during an embedded operation is a driver bug -> refused
    res = run_code(emu, _PROGRAM, CPU + 0x40, 0x1111, BUF, 1)
    assert res.ok and nor.busy
    e = raises(se.NorProtocolError, emu.call, FUN_RESET)
    assert "during embedded program" in str(e)
    emu.close()


TESTS = [(n, f) for n, f in list(globals().items()) if n.startswith("test_") and callable(f)]


def main(argv):
    verbose = "-v" in argv
    names = [a for a in argv if not a.startswith("-")]
    todo = [(n, f) for n, f in TESTS if not names or n in names]
    failed = 0
    t_all = time.time()
    for n, f in todo:
        t = time.time()
        try:
            f()
        except Exception:
            failed += 1
            print(f"FAIL {n}")
            traceback.print_exc()
        else:
            print(f"PASS {n}" + (f" ({time.time() - t:.2f}s)" if verbose else ""))
    print(f"{len(todo) - failed}/{len(todo)} passed in {time.time() - t_all:.1f}s")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
