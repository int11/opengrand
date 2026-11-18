# Smoke tests for tools/stockemu.py (Unicorn harness for the CA79 stock image).
#
#   cd result/stockfw && uv run python -I tools/test_stockemu.py [-v] [test_name ...]
#
# Exit code 0 = all passed. Needs target/CAx9_P06.SYS (stock sha256 09ad8570...d933c8).
# Expected values come from the raw image file or from independent Python references, not from
# the emulator itself, so a pass means the emulated stock code really does what the file says.
import importlib.util
import pathlib
import random
import struct
import sys
import time
import traceback

sys.dont_write_bytecode = True  # keep tools/ free of __pycache__
_HERE = pathlib.Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("stockemu", _HERE / "stockemu.py")
se = importlib.util.module_from_spec(_spec)
sys.modules["stockemu"] = se  # dataclasses needs the module registered before exec
_spec.loader.exec_module(se)

from keystone import KS_ARCH_ARM, KS_MODE_ARM, Ks  # noqa: E402
from unicorn import UC_HOOK_CODE, UcError  # noqa: E402

StockEmu = se.StockEmu
IMG = se.stock_bytes()
SCRATCH = 0x40800000  # zero RAM above the image (BSS area): scratch code and buffers
_ks = Ks(KS_ARCH_ARM, KS_MODE_ARM)


def asm(src, addr=SCRATCH):
    enc, _n = _ks.asm(src, addr)
    return bytes(enc)


def run_code(emu, src, *args, **kw):
    """Assemble `src` into scratch RAM and call it like a function."""
    emu.write_bytes(SCRATCH, asm(src))
    return emu.call(SCRATCH, *args, **kw)


def bswap32(x):
    return int.from_bytes(x.to_bytes(4, "little"), "big")


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def fresh(image=IMG, **kw):
    return StockEmu(image, **kw)


def raises(exc, fn, *a, **k):
    try:
        fn(*a, **k)
    except exc:
        return
    raise AssertionError(f"{exc.__name__} not raised")


_SHARED = []


def shared():
    """One read-mostly emulator for tests that do not patch memory."""
    if not _SHARED:
        _SHARED.append(fresh())
    return _SHARED[0]


# --------------------------------------------------------------------------------------------
# stock functions used below (all addresses from own disassembly)
# --------------------------------------------------------------------------------------------
FUN_COPY = se.FUN_COPY  # 0x400aa030 copy(src_flash, dst, len)
FUN_WINSEL = se.FUN_WINDOW_SELECT  # 0x400aa008
FUN_IN_RAM = 0x400A86E0  # r0 = 1 if 0x40000000 <= r0 < literal(0x40400000 @0x400a87b4) else 0
IN_RAM_LITERAL = 0x400A87B4
FUN_FMUL = 0x401567A4  # soft-float32 multiply (r0 * r1), flush-to-zero, overflow -> inf
FUN_DSP_INIT_WRITE = 0x400AAADC  # *(0x8001f87c) = 'INIT'
FUN_DSP_INIT_WAIT = 0x400AAAEC  # while (*(0x8001f87c) != 'IEND');
SHARED_HANDSHAKE = 0x8001F87C


# --------------------------------------------------------------------------------------------
# memory map and loader state
# --------------------------------------------------------------------------------------------
def test_memory_map():
    emu = shared()
    assert emu.is_stock and emu.sha256 == se.STOCK_SHA256
    # NOR alias: boot area and the tail after the image are erased flash, the image is mirrored
    assert emu.read_bytes(0x60000000, 0x60000) == b"\xff" * 0x60000
    assert emu.read_bytes(0x60060000, len(IMG)) == IMG
    assert emu.read_bytes(0x60400000, 0x200000) == b"\xff" * 0x200000
    # vector page = the 0x40 bytes the stock startup copies there
    off = se.VECTOR_SRC - se.IMAGE_BASE
    assert emu.read_bytes(0xFFFF0000, 0x40) == IMG[off : off + 0x40]
    assert emu.read_bytes(0xFFFF0040, 0x40) == bytes(0x40)
    # every stub window answers at its first and last word
    for base, size, _name in se.STUB_WINDOWS:
        assert emu.read_u32(base) == 0 and emu.read_u32(base + size - 4) == 0
    # outside the map: host read fails, CPU access stops the call
    raises(UcError, emu.read_bytes, 0xC0000000, 4)
    res = emu.call(0x70000000)
    assert res.stop == "unmapped" and res.info["access"] == "fetch_unmapped", res
    res = run_code(emu, "ldr r1, =0xC0000000\n ldr r0, [r1]\n bx lr")
    assert res.stop == "unmapped" and res.info["addr"] == 0xC0000000 and res.info["access"] == "read_unmapped", res
    assert res.info["pc"] == SCRATCH + 4, res.info


def test_nor_alias_is_read_only():
    emu = fresh()
    res = run_code(emu, "ldr r1, =0x60060000\n mov r2, #5\n str r2, [r1]\n bx lr")
    assert res.stop == "write_prot", res
    assert res.info["addr"] == 0x60060000 and res.info["size"] == 4 and res.info["value"] == 5, res.info
    assert emu.read_bytes(0x60060000, 4) == IMG[:4]
    # the host can still patch it (set_mem), and code may execute from the alias (XIP)
    xip = se.FLASH_BASE + (FUN_IN_RAM - se.IMAGE_BASE)
    assert emu.call(xip, 0x40300000).r0 == 1
    emu.set_mem(se.FLASH_BASE, b"\x11\x22\x33\x44")
    assert emu.read_bytes(se.FLASH_BASE, 4) == b"\x11\x22\x33\x44"
    emu.close()


def test_loader_state_matches_address_map():
    """The stock loader FUN_400aa994 really leaves SDRAM as the loader-copy table describes."""
    emu = shared()
    # 1. the five copies the stock code makes are exactly the documented table
    assert sorted(emu.loader_copies) == sorted(se.LOADER_COPIES), emu.loader_copies
    # 2. independent Python reference of the RAM image: boot part + five copies + in-place header swap
    ref = bytearray(0x400000)  # SDRAM 0x40000000-0x403FFFFF
    base = se.IMAGE_BASE - 0x40000000
    ref[base : base + 0x60000] = IMG[:0x60000]
    for _src, dst, n in se.LOADER_COPIES:
        fo = dst - se.IMAGE_BASE
        ref[dst - 0x40000000 : dst - 0x40000000 + n] = IMG[fo : fo + n]
    hdr = bswap32(u32(IMG, 0x1A0000))  # first word of the voice-DB table (big endian)
    nconv = bswap32(u32(IMG, 0x1A0000 + hdr))  # byte length of the table the loader swaps in place
    assert (hdr, nconv) == (0x100, 0x1CD4), (hdr, nconv)
    for off in range(0, nconv, 4):
        a = 0x200000 + off
        ref[a : a + 4] = ref[a : a + 4][::-1]
    got = emu.read_bytes(0x40000000, 0x400000)
    if got != bytes(ref):
        bad = next(i for i in range(len(ref)) if got[i] != ref[i])
        raise AssertionError(f"SDRAM differs from reference at {0x40000000 + bad:#x}")
    # 3. the converted header is visibly different from the file bytes
    assert emu.read_bytes(0x40200000, 4) != IMG[0x1A0000 : 0x1A0004]
    assert emu.read_bytes(0x40200000 + nconv, 64) == IMG[0x1A0000 + nconv : 0x1A0000 + nconv + 64]
    # 4. guard ranges = the 0xFF gaps the loader does not copy; zero-filled
    assert emu.guard_ranges == [(0x40173A04, 0x40200000), (0x403C0000, 0x40400000)], emu.guard_ranges
    for lo, hi in emu.guard_ranges:
        assert emu.read_bytes(lo, hi - lo) == bytes(hi - lo)
    # what the gap holds in flash: 16 bytes of non-erased data (words 0x45,0x6e,0x64,0x20 = 'E','n','d',' ') that the
    # loader does not copy (it stops at 0x40173a04), then erased 0xFF up to 0x1A0000
    assert IMG[0x113A04 : 0x113A14] == struct.pack("<4I", 0x45, 0x6E, 0x64, 0x20)
    assert IMG[0x113A14 : 0x1A0000] == b"\xff" * (0x1A0000 - 0x113A14)
    # 5. RAM outside the image is plain zero (no stock init has run)
    assert emu.read_bytes(0x40000000, 0x60000) == bytes(0x60000)
    assert emu.read_bytes(0x40400000, 0x1000) == bytes(0x1000)


def test_loader_write_footprint():
    """Run the stock loader under the write log: it writes only the five copies (+ its stack), never the guard gaps."""
    guard = [(0x40173A04, 0x40200000), (0x403C0000, 0x40400000)]
    emu = fresh(loader="none", guard=guard)  # bare machine, loader not run yet
    assert emu.read_bytes(0x40200000, 16) == bytes(16)
    emu.hook(FUN_WINSEL, lambda e: None)
    res = emu.call(se.FUN_LOADER, max_insns=50_000_000)
    assert res.stop == "returned" and res.log_dropped == 0, res
    spans = []
    for lo, hi in sorted((w.addr, w.addr + w.size) for w in res.writes):
        if spans and lo <= spans[-1][1]:
            spans[-1][1] = max(spans[-1][1], hi)
        else:
            spans.append([lo, hi])
    assert spans == [[0x400C0000, 0x40173A04], [0x40200000, 0x403C0000], [0x40FFFFE8, 0x41000000]], [(hex(a), hex(b)) for a, b in spans]
    assert res.guard == [] and {w.size for w in res.writes} == {4}
    # the loaded ranges equal those of a normal construction (which runs the same loader at start-up)
    ref = shared()
    for lo, hi in ((0x400C0000, 0x40173A04), (0x40200000, 0x403C0000)):
        assert emu.read_bytes(lo, hi - lo) == ref.read_bytes(lo, hi - lo)
    emu.close()


def test_design_a_boundary_literal():
    """Cave placement option A: one literal (0x400ab3ec) decides how far loader copy #1 reaches."""
    assert u32(IMG, se.LOADER_END_LITERAL - se.IMAGE_BASE) == 0x40173A04
    patched = bytearray(IMG)
    struct.pack_into("<I", patched, se.LOADER_END_LITERAL - se.IMAGE_BASE, 0x40174A04)  # +0x1000
    emu = fresh(bytes(patched))
    want = [(0x600C0000, 0x400C0000, 0xB4A04)] + [c for c in se.LOADER_COPIES if c[1] != 0x400C0000]  # only #1 changes
    assert sorted(emu.loader_copies) == sorted(want), emu.loader_copies
    fo = 0x40173A04 - se.IMAGE_BASE
    assert emu.read_bytes(0x40173A04, 0x1000) == IMG[fo : fo + 0x1000]  # now loaded: 16 bytes of data, then 0xFF
    assert IMG[fo + 16 : fo + 0x1000] == b"\xff" * (0x1000 - 16)
    assert emu.guard_ranges[0] == (0x40174A04, 0x40200000), emu.guard_ranges
    assert shared().read_bytes(0x40173A04, 0x1000) == bytes(0x1000)  # unpatched run: still zero
    # the extended range is no longer a guard range: reading it is silent, the old boundary is not
    emu.hook(FUN_WINSEL, lambda e: None)
    assert emu.call(FUN_COPY, 0x40173A04, SCRATCH, 16).guard == []
    assert [h.addr for h in emu.call(FUN_COPY, 0x40174A04, SCRATCH, 4).guard] == [0x40174A04]
    emu.close()


def test_image_arguments():
    emu = fresh(bytearray(IMG), loader="none")  # bytes-like accepted
    assert emu.loader_copies == []
    assert emu.read_bytes(0x400C0000, 0x1000) == bytes(0x1000)  # nothing but the boot part loaded
    assert emu.read_bytes(0x40060000, 0x60000) == IMG[:0x60000]
    assert emu.guard_ranges == [(0x400C0000, 0x40400000)]
    emu.close()
    raises(ValueError, StockEmu, IMG[:-4])
    raises(ValueError, StockEmu, IMG, loader="bogus")
    emu = StockEmu(str(se.DEFAULT_IMAGE))  # path accepted
    assert emu.read_bytes(0x400C0000, 16) == IMG[0x60000:0x60010]
    emu.close()
    with StockEmu(IMG) as emu:
        pass
    raises(RuntimeError, emu.call, FUN_IN_RAM)  # closed


# --------------------------------------------------------------------------------------------
# smoke test 1: pure code vs Python references
# --------------------------------------------------------------------------------------------
def test_pure_byteswap_snippet():
    """Byte-swap sequence of the loader tail (0x400aa9f4-0x400aaa30), run as a snippet with `until`."""
    emu = shared()
    rnd = random.Random(1)
    words = [0, 1, 0xFF, 0x12345678, 0x80000001, 0xFFFFFFFF, 0xDEADBEEF] + [rnd.getrandbits(32) for _ in range(64)]
    for w in words:
        # ip = bswap(ip): registers only, stop before the table load at 0x400aaa14
        res = emu.call(0x400AA9F4, regs={"ip": w}, until=0x400AAA14)
        assert res.stop == "until" and res.pc == 0x400AAA14, res
        assert res.regs[12] == bswap32(w), (hex(w), hex(res.regs[12]))
        assert res.writes == [] and res.periph == [] and res.guard == []
        # second swap at 0x400aaa18-0x400aaa30 acts on r0
        res = emu.call(0x400AAA18, r0=w, until=0x400AAA34)
        assert res.stop == "until" and res.r0 == bswap32(w), (hex(w), res)
    # whole sequence incl. the table read: the loader first sizes the table from the raw header word
    # (which the finished loader has already converted in RAM, so restore the raw bytes first).
    emu = fresh()
    emu.set_mem(0x40200000, IMG[0x1A0000 : 0x1A0000 + 0x1CD4 + 0x10])
    first = u32(IMG, 0x1A0000)  # what `ldr ip,[ip]` at 0x400aa9f0 loads
    res = emu.call(0x400AA9F4, regs={"ip": first}, until=0x400AAA34)
    ref = bswap32(u32(IMG, 0x1A0000 + bswap32(first)))  # table length read from the file
    assert res.stop == "until" and res.r0 == ref == 0x1CD4, (res, hex(ref))
    assert res.writes == [] and res.periph == [] and res.guard == []  # the sequence has no stores
    emu.close()


def _f2b(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def _b2f(b):
    return struct.unpack("<f", struct.pack("<I", b))[0]


def _fmul_ref(ba, bb):
    """float32 multiply as the stock soft-float does it: denormal in/out flushes to +0, overflow -> +-inf."""
    ea, eb = (ba >> 23) & 0xFF, (bb >> 23) & 0xFF
    sign = (ba ^ bb) & 0x80000000
    if ea == 0 or eb == 0:
        return 0
    if ea == 0xFF or eb == 0xFF:
        return sign | 0x7F800000
    p = _b2f(ba) * _b2f(bb)  # exact in double (24x24 bit), single rounding to float32 below
    if abs(p) >= 3.4028235677973366e38:
        return sign | 0x7F800000
    bp = _f2b(p)
    return 0 if ((bp >> 23) & 0xFF) == 0 else bp


def test_pure_function_float_multiply():
    emu = shared()
    res = emu.call(FUN_FMUL, _f2b(1.5), _f2b(2.0))
    assert res.stop == "returned" and _b2f(res.r0) == 3.0, res
    insns = res.insns
    sp0 = res.regs[13]
    assert sp0 == se.STACK_TOP  # balanced stack after the return
    rnd = random.Random(7)
    for i in range(400):
        ba, bb = rnd.getrandbits(32), rnd.getrandbits(32)
        if i < 40:  # exact small integers and special exponents first
            ba, bb = _f2b(float(rnd.randint(-300, 300))), _f2b(float(rnd.randint(-300, 300)))
        res = emu.call(FUN_FMUL, ba, bb)
        assert res.stop == "returned"
        assert res.r0 == _fmul_ref(ba, bb), (hex(ba), hex(bb), hex(res.r0), hex(_fmul_ref(ba, bb)))
        # side-effect free: the only stores are the push of the saved registers on the stack
        assert all(se.STACK_TOP - 0x40 <= w.addr < se.STACK_TOP for w in res.writes), res.writes[:3]
        assert res.periph == [] and res.guard == []
    assert emu.call(FUN_FMUL, _f2b(1.5), _f2b(2.0)).insns == insns  # deterministic


# --------------------------------------------------------------------------------------------
# smoke test 2: loader copy helper FUN_400aa030 with FUN_400aa008 hooked
# --------------------------------------------------------------------------------------------
def test_loader_copy_helper():
    emu = fresh()
    sel = []
    emu.hook(FUN_WINSEL, lambda e: sel.append(e.reg(0)))
    src, dst, n = 0x601A7000, SCRATCH, 0x3000  # crosses the 32 KB window edge at 0x601a8000
    res = emu.call(FUN_COPY, src, dst, n)
    fo = src - se.FLASH_BASE
    assert res.stop == "returned", res
    assert emu.read_bytes(dst, n) == IMG[fo : fo + n]
    assert emu.read_bytes(dst + n, 16) == bytes(16)  # nothing written past the end
    assert sel == [src, 0x601A8000], [hex(x) for x in sel]  # window select once per 32 KB window
    # write log: 4 pushed registers, then n/4 word stores to dst by the four unrolled `str r1,[r3]`
    stores = [w for w in res.writes if dst <= w.addr < dst + n]
    assert len(res.writes) == n // 4 + 4 and len(stores) == n // 4
    assert [(w.addr, w.size, w.value) for w in stores] == [(dst + 4 * i, 4, u32(IMG, fo + 4 * i)) for i in range(n // 4)]
    assert {w.pc for w in stores} == {0x400AA080, 0x400AA0AC, 0x400AA0D4, 0x400AA100}
    assert res.periph == [] and res.guard == []
    # length is rounded up to whole words
    res = emu.call(FUN_COPY, 0x60200000, SCRATCH + 0x10000, 0x2FFE)
    assert emu.read_bytes(SCRATCH + 0x10000, 0x3000) == IMG[0x1A0000 : 0x1A3000]
    assert emu.read_bytes(SCRATCH + 0x13000, 4) == bytes(4)
    # zero length returns at once
    res = emu.call(FUN_COPY, 0x60200000, SCRATCH + 0x20000, 0)
    assert res.stop == "returned" and res.insns <= 6 and emu.read_bytes(SCRATCH + 0x20000, 4) == bytes(4)
    emu.close()


def test_window_select_stub_and_readback():
    """Un-hooked FUN_400aa008 writes the NOR window number to the GPIO latch 0x01E2608C: logged by the stub."""
    emu = fresh()
    res = emu.call(FUN_WINSEL, 0x601A8000)
    P = se.PeriphAccess
    assert res.periph == [
        P("R", 0x01E2608C, 2, 0, 0x400AA010),
        P("W", 0x01E2608C, 2, 0, 0x400AA018),
        P("R", 0x01E2608C, 2, 0, 0x400AA01C),
        P("W", 0x01E2608C, 2, 0x35, 0x400AA028),  # (0x601a8000 >> 15) & 0x7ff
    ], res.periph
    assert res.r0 == 0x35 and res.writes == []
    # the real register keeps what was written; with readback the stub does the same
    emu.set_readback(0x01E2608C)
    emu.write_bytes(0x01E2608C, struct.pack("<H", 0xABCD))
    emu.call(FUN_WINSEL, 0x601A8000)
    assert emu.read_bytes(0x01E2608C, 2) == struct.pack("<H", 0xA800 | 0x35)  # bits 15-11 kept, low 11 replaced
    emu.close()


# --------------------------------------------------------------------------------------------
# smoke test 3: guard pages
# --------------------------------------------------------------------------------------------
def test_guard_read_is_logged():
    emu = fresh()
    emu.hook(FUN_WINSEL, lambda e: None)
    res = emu.call(FUN_COPY, 0x40180000, SCRATCH, 16)  # 4 word reads inside 0x40173a04-0x401fffff
    assert res.stop == "returned"
    assert [(h.op, h.addr, h.size, h.value) for h in res.guard] == [("R", 0x40180000 + 4 * i, 4, 0) for i in range(4)], res.guard
    assert {h.pc for h in res.guard} == {0x400AA078, 0x400AA0A4, 0x400AA0CC, 0x400AA0F8}  # the four `ldr r1,[r0..]`
    # byte-exact boundary (0x40173a04 is not page aligned): the word below is loaded data, the word at it is guard
    res = emu.call(FUN_COPY, 0x40173A00, SCRATCH, 8)
    assert [h.addr for h in res.guard] == [0x40173A04], res.guard
    res = emu.call(FUN_COPY, 0x401FFFFC, SCRATCH, 8)  # last guard word, then 0x40200000 (loaded)
    assert [h.addr for h in res.guard] == [0x401FFFFC], res.guard
    res = emu.call(FUN_COPY, 0x403BFFFC, SCRATCH, 8)  # last loaded word, then 0x403c0000 (guard #2)
    assert [h.addr for h in res.guard] == [0x403C0000], res.guard
    # nothing in a loaded region, and host-side reads never log
    emu.read_bytes(0x40180000, 16)
    assert emu.call(FUN_COPY, 0x40200000, SCRATCH, 64).guard == []
    # writes into a guard range are guard hits as well as normal write-log entries
    res = emu.call(FUN_COPY, SCRATCH, 0x40180100, 8)
    assert [(h.op, h.addr) for h in res.guard] == [("W", 0x40180100), ("W", 0x40180104)], res.guard
    assert [w for w in res.writes if w.addr == 0x40180100]
    # optional hard stop at the first guard access
    res = emu.call(FUN_COPY, 0x40180000, SCRATCH, 16, stop_on_guard=True)
    assert res.stop == "guard" and res.info["addr"] == 0x40180000 and res.info["op"] == "R", res
    assert len(res.guard) == 1 and res.insns < 50
    # executing in a guard range always stops before the first instruction runs
    res = emu.call(0x40180000)
    assert res.stop == "guard_exec" and res.pc == 0x40180000 and res.insns == 0, res
    assert [(h.op, h.addr) for h in res.guard] == [("X", 0x40180000)]
    emu.close()


# --------------------------------------------------------------------------------------------
# smoke test 4: patch a copy of the image (one literal-pool word)
# --------------------------------------------------------------------------------------------
def test_patch_literal_changes_result():
    assert u32(IMG, IN_RAM_LITERAL - se.IMAGE_BASE) == 0x40400000
    patched = bytearray(IMG)  # a copy; IMG itself must stay untouched
    struct.pack_into("<I", patched, IN_RAM_LITERAL - se.IMAGE_BASE, 0x40200000)
    orig, pat = fresh(IMG), fresh(bytes(patched))
    probes = [0x3FFFFFFF, 0x40000000, 0x401FFFFF, 0x40200000, 0x40300000, 0x403FFFFF, 0x40400000]
    for a in probes:
        e_orig = int(0x40000000 <= a < 0x40400000)
        e_pat = int(0x40000000 <= a < 0x40200000)
        assert orig.call(FUN_IN_RAM, a).r0 == e_orig, hex(a)
        assert pat.call(FUN_IN_RAM, a).r0 == e_pat, hex(a)
    assert orig.call(FUN_IN_RAM, 0x40300000).r0 == 1 and pat.call(FUN_IN_RAM, 0x40300000).r0 == 0  # they differ
    assert u32(IMG, IN_RAM_LITERAL - se.IMAGE_BASE) == 0x40400000  # source bytes unchanged
    assert orig.read_u32(IN_RAM_LITERAL) == 0x40400000 and pat.read_u32(IN_RAM_LITERAL) == 0x40200000
    # patching a live emulator after the function already ran (translated code must be invalidated)
    live = fresh()
    assert live.call(FUN_IN_RAM, 0x40300000).r0 == 1
    live.set_mem(IN_RAM_LITERAL, struct.pack("<I", 0x40200000))
    assert live.call(FUN_IN_RAM, 0x40300000).r0 == 0
    live.set_mem(IN_RAM_LITERAL, struct.pack("<I", 0x40400000))
    assert live.call(FUN_IN_RAM, 0x40300000).r0 == 1
    # patching an instruction: `cmp r1,r0` (0xe1510000) -> `cmp r0,r0` makes movhi never fire
    assert live.read_u32(0x400A86F0) == 0xE1510000
    live.write_u32(0x400A86F0, 0xE1500000)
    assert live.call(FUN_IN_RAM, 0x40300000).r0 == 0
    for e in (orig, pat, live):
        e.close()


# --------------------------------------------------------------------------------------------
# hooks, stop reasons, no-progress detector, counting
# --------------------------------------------------------------------------------------------
def test_hook_semantics():
    emu = fresh()
    F = FUN_IN_RAM
    base = emu.call(F, 0x40300000)
    assert base.r0 == 1 and base.insns == 8
    # None: return to caller, r0 untouched (= first argument)
    emu.hook(F, lambda e: None)
    res = emu.call(F, 0x1234)
    assert res.stop == "returned" and res.r0 == 0x1234 and res.insns == 1
    # int: r0 = value
    emu.hook(F, lambda e: 0x1_0000_0077)  # truncated to 32 bits
    assert emu.call(F, 0x40300000).r0 == 0x77
    # CONTINUE: observe only, the original code runs
    seen = []
    emu.hook(F, lambda e: (seen.append(e.arg(0)), StockEmu.CONTINUE)[1])
    res = emu.call(F, 0x40300000)
    assert seen == [0x40300000] and res.r0 == 1 and res.insns == base.insns
    # jump: continue elsewhere (the `mov r0, ip; bx lr` tail) instead of returning
    emu.hook(F, lambda e: e.jump(0x400A86F8))
    res = emu.call(F, 0x40300000, regs={"ip": 9})
    assert res.r0 == 9 and res.insns == 1 + 2, res
    # arguments beyond r3 come from the stack, sp is 8-byte aligned at entry
    got = []
    emu.hook(F, lambda e: got.append(([e.arg(i) for i in range(6)], e.sp)))
    emu.call(F, 1, 2, 3, 4, stack=[0x55, 0x66])
    assert got == [([1, 2, 3, 4, 0x55, 0x66], se.STACK_TOP - 8)], got
    for words in ([7], [7, 8, 9], [1, 2, 3, 4, 5]):  # odd word counts still give an 8-byte aligned sp
        got.clear()
        emu.call(F, 0, 0, 0, 0, stack=words)
        assert got[0][1] % 8 == 0 and got[0][0][4] == words[0] and got[0][1] <= se.STACK_TOP - 4 * len(words), got
    # a hook placed in the middle of an already translated block takes effect
    emu.unhook(F)
    emu.call(F, 0x40300000)  # translate the whole function first
    mid = []
    emu.hook(0x400A86F0, lambda e: (mid.append(e.reg(1)), StockEmu.CONTINUE)[1])  # `cmp r1, r0`
    assert emu.call(F, 0x40300000).r0 == 1 and mid == [0x40400000], mid
    emu.unhook(0x400A86F0)
    emu.hook(F, lambda e: 0)
    # hooks work in the middle of a call tree (copy helper -> window select)
    calls = []
    emu.hook(FUN_WINSEL, lambda e: calls.append(e.reg(0)) or 0x1234)
    res = emu.call(FUN_COPY, 0x60200000, SCRATCH, 8)
    assert calls == [0x60200000] and res.stop == "returned"
    # unhook restores the stock behaviour
    emu.unhook(F)
    emu.unhook(FUN_WINSEL)
    assert emu.call(F, 0x40300000).r0 == 1
    # misuse
    raises(ValueError, emu.hook, F + 2, lambda e: None)
    raises(ValueError, emu.call, F + 1)
    raises(KeyError, emu.unhook, F)
    emu.hook(F, lambda e: e.hook(F, None))
    raises(RuntimeError, emu.call, F)  # hooks cannot be changed while running; the error reaches the caller
    emu.hook(F, lambda e: e.call(F))
    raises(RuntimeError, emu.call, F)  # call() is not re-entrant
    emu.hook(F, lambda e: 1 // 0)
    raises(ZeroDivisionError, emu.call, F)  # hook exceptions propagate
    emu.unhook(F)
    assert emu.call(F, 0x40300000).r0 == 1  # and the emulator is still usable
    emu.close()


def test_stop_reasons():
    emu = fresh()
    # until: first reached address wins, list accepted, the instruction there is not executed
    res = emu.call(FUN_FMUL, _f2b(1.5), _f2b(2.0), until=[0x401567CC, 0x40156838])
    assert res.stop == "until" and res.pc in (0x401567CC, 0x40156838), res
    # instruction limit is exact
    full = emu.call(FUN_FMUL, _f2b(1.5), _f2b(2.0)).insns
    for lim in (1, 2, 5, full - 1):
        res = emu.call(FUN_FMUL, _f2b(1.5), _f2b(2.0), max_insns=lim)
        assert res.stop == "max_insns" and res.insns == lim, (lim, res)
    assert emu.call(FUN_FMUL, _f2b(1.5), _f2b(2.0), max_insns=full).stop == "returned"
    # endless loops: self branch and a 2-cycle of register states are both caught quickly
    res = run_code(emu, "b .")
    assert res.stop == "no_progress" and res.info["pc"] == SCRATCH and res.insns < 10_000, res
    res = run_code(emu, "add r0, r0, #1\n sub r0, r0, #1\n b .-8")
    assert res.stop == "no_progress" and res.insns < 20_000, res
    # a long but finite loop and a state-changing endless loop are not no-progress
    res = run_code(emu, "mov r1, #0x20000\n l: subs r1, r1, #1\n bne l\n bx lr")
    assert res.stop == "returned" and res.insns == 1 + 2 * 0x20000 + 1, res
    res = run_code(emu, "add r0, r0, #1\n b .-4", max_insns=100_000)
    assert res.stop == "max_insns" and res.insns == 100_000 and res.r0 == 50_000, res
    res = run_code(emu, "b .", max_insns=5000, no_progress=0)  # detector off -> only the limit stops it
    assert res.stop == "max_insns" and res.insns == 5000
    # memory-changing loops count as progress (documented limit: idempotent stores are not detected)
    res = run_code(emu, "ldr r1, =0x40801000\n mov r2, #5\n l: str r2, [r1]\n b l", max_insns=20_000)
    assert res.stop == "max_insns" and res.insns == 20_000
    # faults and exceptions carry details
    res = run_code(emu, "swi #0x123456\n bx lr")
    assert res.stop == "swi" and res.info["number"] == 0x123456 and res.info["pc"] == SCRATCH, res
    res = run_code(emu, ".long 0xe7f000f0\n bx lr")
    assert res.stop == "invalid_insn" and res.info["pc"] == SCRATCH, res
    res = run_code(emu, "mcr p15, 0, r0, c7, c0, 4\n bx lr")  # WFI: no interrupt can ever wake the core
    assert res.stop == "halted", res
    res = run_code(emu, "mrc p15, 0, r0, c0, c0, 0\n bx lr")  # ARM926EJ-S main ID register
    assert res.stop == "returned" and res.r0 == 0x41069265, res
    # hook asks to stop with its own reason
    emu.hook(FUN_IN_RAM, lambda e: e.stop("reached_in_ram", arg=e.arg(0)))
    res = emu.call(FUN_IN_RAM, 0x4321)
    assert res.stop == "reached_in_ram" and res.info == {"arg": 0x4321}, res
    emu.close()


def test_stock_swi_wrapper():
    """Stock 0x4009d324 runs a function in supervisor mode via `svc #0` (r0 = routine): the harness stops there."""
    emu = fresh()
    res = emu.call(0x4009D324)
    assert res.stop == "swi" and res.info == {"number": 0, "pc": 0x4009D32C} and res.r0 == 0x4009D338, res
    emu.hook(0x4009D324, lambda e: 0)  # hook the wrapper instead
    assert emu.call(0x4009D324).stop == "returned"
    emu.close()


def test_mmu_table_builder_and_cp15_reset():
    """Run the stock MMU setup FUN_4009d198: it builds the tables at 0x41ef8000 from the region list at 0x4009d038
    and switches the MMU on. The call still ends normally and the next call starts with the MMU off again."""
    emu = fresh()
    res = emu.call(0x4009D198)
    assert res.stop == "returned" and res.insns > 20_000, res
    cp15 = lambda crn: emu.uc.cpr_read(15, 0, crn, 0, 0, 0, False)  # noqa: E731
    sctlr, ttbr, dacr = cp15(1), cp15(2), cp15(3)
    assert (ttbr, dacr) == (0x41EF8000, 0x55555555) and sctlr & 0x2005 == 0x2005, (hex(sctlr), hex(ttbr), hex(dacr))
    l1 = struct.unpack("<4096I", emu.read_bytes(0x41EF8000, 0x4000))
    off, n = 0x4009D038 - se.IMAGE_BASE, 0
    while True:
        size, va, pa, attr = struct.unpack_from("<4I", IMG, off + 16 * n)
        if size == 0:
            break
        for k in range(size >> 20):  # whole megabytes become section descriptors
            assert l1[(va >> 20) + k] == (((pa + (k << 20)) & 0xFFF00000) | attr | 0x12), (hex(va), k)
        if size & 0xFFFFF:  # the rest goes through a coarse page table
            assert l1[(va + size - 1) >> 20] & 3 == 1, hex(va)
        n += 1
    assert n == 13  # 10 of them are in the harness map; DDR 0xC0000000/0xC3800000 and 0xFFFD0000 (64 KB) are not mapped
    assert l1[0x40000000 >> 20] == 0x40000C1E and l1[0x60000000 >> 20] == 0x60000C12  # SDRAM C+B, NOR uncached
    res = emu.call(FUN_IN_RAM, 0x40300000)
    assert res.stop == "returned" and res.r0 == 1 and cp15(1) == emu._sctlr0, res
    emu.close()


def test_hook_with_state_is_progress():
    emu = fresh()
    emu.write_bytes(SCRATCH + 0x100, asm("bx lr", SCRATCH + 0x100))  # callee, replaced by a Python hook below
    calls = []
    emu.hook(SCRATCH + 0x100, lambda e: 1 if len(calls.append(1) or calls) > 5000 else 0)
    # identical CPU state at every iteration; only the hook's own counter changes
    res = run_code(emu, "push {lr}\n l: bl 0x40800100\n cmp r0, #1\n bne l\n pop {pc}")
    assert res.stop == "returned" and len(calls) == 5001, (res, len(calls))
    emu.close()


def test_write_log_sizes_and_values():
    emu = fresh()
    res = run_code(emu, "ldr r1, =0x40801000\n ldr r2, =0x11223344\n strb r2, [r1]\n strh r2, [r1, #4]\n str r2, [r1, #8]\n bx lr")
    W = se.Write
    assert res.writes == [
        W(0x40801000, 1, 0x44, SCRATCH + 8),
        W(0x40801004, 2, 0x3344, SCRATCH + 12),
        W(0x40801008, 4, 0x11223344, SCRATCH + 16),
    ], res.writes
    assert emu.read_bytes(0x40801000, 12) == bytes.fromhex("44000000" "44330000" "44332211")
    # push stores the registers lowest address first; stm of two registers = two word writes
    res = run_code(emu, "mov r4, #4\n mov r5, #5\n push {r4, r5}\n pop {r4, r5}\n bx lr")
    assert [(w.addr, w.value) for w in res.writes] == [(se.STACK_TOP - 8, 4), (se.STACK_TOP - 4, 5)], res.writes
    emu.close()


def test_instruction_count_is_exact():
    """CallResult.insns (block based) equals a brute-force per-instruction hook count."""
    emu = fresh()
    emu.hook(FUN_WINSEL, lambda e: None)
    cases = [
        (FUN_COPY, (0x601A0000, SCRATCH, 0x40)),
        (FUN_COPY, (0x601A7FF0, SCRATCH, 0x40)),  # crosses a window edge
        (FUN_FMUL, (_f2b(1.5), _f2b(2.0))),
        (FUN_FMUL, (0, _f2b(2.0))),  # early return path
        (FUN_IN_RAM, (0x10,)),
        (FUN_IN_RAM, (0x40300000,)),
    ]
    for addr, args in cases:
        n = [0]
        h = emu.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, ud: n.__setitem__(0, n[0] + 1))
        emu.uc.ctl_flush_tb()
        try:
            res = emu.call(addr, *args)
        finally:
            emu.uc.hook_del(h)
            emu.uc.ctl_flush_tb()
        assert res.stop == "returned" and res.insns == n[0], (hex(addr), res.insns, n[0])
    # limit hit in the middle of a block and a fault in the middle of a block
    res = emu.call(FUN_COPY, 0x601A0000, SCRATCH, 0x40, max_insns=7)
    assert res.stop == "max_insns" and res.insns == 7
    res = run_code(emu, "mov r0, #1\n mov r1, #2\n mov r2, #3\n ldr r3, [r4, #0]\n bx lr", regs={"r4": 0x70000000})
    assert res.stop == "unmapped" and res.insns == 3, res


def test_trace_and_logs_limit():
    emu = shared()
    res = emu.call(FUN_FMUL, _f2b(1.5), _f2b(2.0), trace=True)
    assert res.trace[0] == FUN_FMUL and len(res.trace) >= 4
    assert all(FUN_FMUL <= a < 0x4015684C for a in res.trace), [hex(a) for a in res.trace]
    assert emu.call(FUN_FMUL, _f2b(1.5), _f2b(2.0)).trace == []
    small = fresh(log_limit=10)
    small.hook(FUN_WINSEL, lambda e: None)
    res = small.call(FUN_COPY, 0x601A0000, SCRATCH, 0x100)
    assert len(res.writes) == 10 and res.log_dropped == 0x100 // 4 + 4 - 10, (len(res.writes), res.log_dropped)
    small.close()


# --------------------------------------------------------------------------------------------
# peripheral stubs
# --------------------------------------------------------------------------------------------
def test_peripheral_values_and_logs():
    emu = fresh()
    A = 0x62000000
    P = se.PeriphAccess
    # unset address reads 0; dict value is returned (and sub-word accesses cut it out little-endian)
    res = run_code(emu, "ldr r1, =0x62000000\n ldr r0, [r1]\n bx lr")
    assert res.r0 == 0 and res.periph == [P("R", A, 4, 0, SCRATCH + 4)], res.periph
    emu.periph[A] = 0xAABBCCDD
    assert run_code(emu, "ldr r1, =0x62000000\n ldr r0, [r1]\n bx lr").r0 == 0xAABBCCDD
    assert run_code(emu, "ldr r1, =0x62000000\n ldrb r0, [r1, #1]\n bx lr").r0 == 0xCC
    assert run_code(emu, "ldr r1, =0x62000000\n ldrh r0, [r1, #2]\n bx lr").r0 == 0xAABB
    # callable (gets emu, addr, size) and a sequence whose last value repeats
    emu.periph[A] = lambda e, addr, size: 0x40 + size
    assert run_code(emu, "ldr r1, =0x62000000\n ldr r0, [r1]\n bx lr").r0 == 0x44
    emu.periph[A] = [1, 2, 3]
    res = run_code(emu, "ldr r1, =0x62000000\n ldr r2, [r1]\n ldr r3, [r1]\n ldr r0, [r1]\n ldr r4, [r1]\n bx lr")
    assert [res.regs[i] for i in (2, 3, 0, 4)] == [1, 2, 3, 3], res.regs
    emu.periph[A] = [10, 20]  # a new sequence starts from its first element again
    res = run_code(emu, "ldr r1, =0x62000000\n ldr r2, [r1]\n ldr r3, [r1]\n ldr r0, [r1]\n bx lr")
    assert [res.regs[i] for i in (2, 3, 0)] == [10, 20, 20], res.regs
    # writes: logged with size/value/pc, stay out of the RAM write log, do not change reads
    emu.periph[A] = 7
    res = run_code(emu, "ldr r1, =0x62000000\n mov r2, #0x5a\n strb r2, [r1, #3]\n strh r2, [r1, #4]\n str r2, [r1, #8]\n bx lr")
    assert res.periph == [P("W", A + 3, 1, 0x5A, SCRATCH + 8), P("W", A + 4, 2, 0x5A, SCRATCH + 12), P("W", A + 8, 4, 0x5A, SCRATCH + 16)], res.periph
    assert [w for w in res.writes if w.addr >= 0x60000000] == []
    assert run_code(emu, "ldr r1, =0x62000000\n ldr r0, [r1]\n bx lr").r0 == 7
    # write hook can model the device (here: a status register that mirrors the last command + 1)
    emu.on_periph_write(A + 0x10, A + 0x14, lambda e, addr, size, value: e.periph.__setitem__(A + 0x14, value + 1))
    res = run_code(emu, "ldr r1, =0x62000000\n mov r2, #0x20\n str r2, [r1, #0x10]\n ldr r0, [r1, #0x14]\n bx lr")
    assert res.r0 == 0x21, res
    # RAM-like readback for one window (e.g. the shared RAM) when wanted
    S = 0x80000010
    assert run_code(emu, "ldr r1, =0x80000010\n mov r2, #9\n str r2, [r1]\n ldr r0, [r1]\n bx lr").r0 == 0  # default: stub
    emu.set_readback(S)
    assert run_code(emu, "ldr r1, =0x80000010\n mov r2, #9\n str r2, [r1]\n ldr r0, [r1]\n bx lr").r0 == 9
    assert run_code(emu, "ldr r1, =0x80000010\n ldrb r0, [r1]\n bx lr").r0 == 9
    raises(ValueError, emu.set_readback, 0x12345678)
    emu.close()


def test_polling_loop_on_stock_handshake():
    """Stock code 0x400aaadc/0x400aaaec: ARM writes 'INIT' to shared RAM and waits for the other side to answer 'IEND'."""
    emu = fresh()
    P = se.PeriphAccess
    res = emu.call(FUN_DSP_INIT_WRITE)
    assert res.stop == "returned" and res.periph == [P("W", SHARED_HANDSHAKE, 4, 0x494E4954, 0x400AAAE4)], res.periph
    assert struct.pack(">I", 0x494E4954) == b"INIT" and struct.pack(">I", 0x49454E44) == b"IEND"
    # nobody answers: the loop is reported with the register it polls instead of burning the budget
    res = emu.call(FUN_DSP_INIT_WAIT)
    assert res.stop == "no_progress" and res.info["polled"] == [SHARED_HANDSHAKE], res
    assert res.info["pc"] == 0x400AAB04 and res.insns < 10_000
    # stub answers after two polls
    emu.periph[SHARED_HANDSHAKE] = [0, 0, 0x49454E44]
    res = emu.call(FUN_DSP_INIT_WAIT)
    assert res.stop == "returned" and [(p.op, p.value, p.pc) for p in res.periph] == [
        ("R", 0, 0x400AAAF4),
        ("R", 0, 0x400AAB00),
        ("R", 0x49454E44, 0x400AAB08),
    ], res.periph
    # a stateful stub is progress even when the CPU state repeats for thousands of polls (list and callable)
    emu.periph[SHARED_HANDSHAKE] = [0] * 6000 + [0x49454E44]
    res = emu.call(FUN_DSP_INIT_WAIT)
    assert res.stop == "returned" and len(res.periph) == 6001, (res.stop, len(res.periph))
    polls = []
    emu.periph[SHARED_HANDSHAKE] = lambda e, addr, size: 0x49454E44 if len(polls.append(1) or polls) > 5000 else 0
    res = emu.call(FUN_DSP_INIT_WAIT)
    assert res.stop == "returned" and len(polls) == 5001, (res.stop, len(polls))
    # or model the other side: it answers when it sees INIT
    emu.periph.clear()

    def other_side(e, addr, size, value):
        if value == 0x494E4954:
            e.periph[addr] = 0x49454E44

    emu.on_periph_write(SHARED_HANDSHAKE, SHARED_HANDSHAKE + 4, other_side)
    assert emu.call(FUN_DSP_INIT_WAIT).stop == "no_progress"
    emu.call(FUN_DSP_INIT_WRITE)
    assert emu.call(FUN_DSP_INIT_WAIT).stop == "returned"
    emu.close()


# --------------------------------------------------------------------------------------------
# memory helpers
# --------------------------------------------------------------------------------------------
def test_memory_and_register_helpers():
    emu = fresh()
    emu.write_bytes(SCRATCH, b"\x01\x02\x03\x04\x05")
    assert emu.read_bytes(SCRATCH, 5) == b"\x01\x02\x03\x04\x05" and emu.read_u32(SCRATCH) == 0x04030201
    emu.write_u32(SCRATCH + 8, 0xCAFEF00D)
    assert emu.read_bytes(SCRATCH + 8, 4) == bytes.fromhex("0df0feca")
    emu.write_cstr(SCRATCH + 0x20, "KAWAI CA79")
    assert emu.read_cstr(SCRATCH + 0x20) == b"KAWAI CA79" and emu.read_bytes(SCRATCH + 0x2A, 2) == b"\0\0"
    assert emu.read_cstr(SCRATCH + 0x20, maxlen=5) == b"KAWAI"
    # strings of the image are visible at their stock addresses (file offset + 0x40060000)
    assert emu.read_cstr(0x400F96AC) == b"TaskProgramCheckSum" == IMG[0x996AC : 0x996AC + 19]
    # registers: names, numbers, presets, results
    res = run_code(emu, "add r0, r4, r5\n mov r6, sp\n bx lr", regs={"r4": 3, "r5": 4})
    assert res.r0 == 7 and res.regs[6] == se.STACK_TOP and res.regs[13] == se.STACK_TOP and res.cpsr & 0x1F == 0x13
    res = run_code(emu, "ldr r0, [sp]\n ldr r1, [sp, #4]\n bx lr", stack=[0xAA, 0xBB])
    assert (res.r0, res.regs[1]) == (0xAA, 0xBB)
    emu.write_u32(SCRATCH + 0x100, 0x11)
    assert run_code(emu, "ldr r1, =0x40800100\n ldr r0, [r1]\n bx lr").r0 == 0x11  # memory persists between calls
    assert emu.disasm(FUN_IN_RAM, 3) == [
        "400a86e0: cmn r0, #0xc0000000",
        "400a86e4: mov ip, #0",
        "400a86e8: blo #0x400a86f8",
    ]
    emu.close()


def test_describe_lists_map_and_guards():
    text = se.describe(shared())
    for needle in ("stock", "0x40000000-0x41ffffff", "0x60000000-0x605fffff", "0x80000000-0x8001ffff", "0xfffee000-0xfffeffff",
                   "0x600c0000 -> 0x400c0000  0xb3a04", "0x40173a04-0x401fffff", "0x403c0000-0x403fffff"):
        assert needle in text, needle


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
