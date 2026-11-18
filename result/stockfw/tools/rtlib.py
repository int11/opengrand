# rtlib.py - the shared harness of the emulator round trips (tools/roundtrip_*.py).
#
# What every round trip used to repeat, in one place: loading a sibling tool by file name (the tools run
# with `python -I`, so this directory is not on sys.path by itself), the built image / slot payload
# inputs (manifest -> patched image, cave symbols from the ELF, a signed .c79 with its payload ELF),
# the cave's 7-in-8 packing, the objdump listing, the `  [PASS] / [FAIL]` check line and the final
# verdict line.  The machines themselves (StockEmu rigs, MMIO / RTOS stubs, SysEx feed / collect) stay
# in the round trip that introduced them and are reached through its module, as before:
#   roundtrip_phase2  deliver() (end handler on a bare StockEmu)          roundtrip_probe  req() / body_of()
#   roundtrip_ota     Piano (NOR model, OTA upload, apply)                roundtrip_ota_hooks  hook sites
#   roundtrip_power   Rig (clock, power stubs, input pins)                roundtrip_sysex  Side (S / H machines)
#   roundtrip_session Net (virtual-time controllers)                      p3_map  StockRun (stock request task)
# Speed (none of it changes what a check sees): poll_step skips the stock ACK wait's idle passes on the virtual
# clocks; memo runs one machine setup (boot, OTA upload + apply) once per process and restores its state for the
# next identical machine; Unit / run_units / --shard split a long round trip into independent scenario groups
# (one check-runner job each), every scenario on fresh machines forked from one build.
#
# A round trip starts with
#   sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
#   import rtlib
# and loads everything else with rtlib.load("<tool>").
from __future__ import annotations

import importlib.util
import pathlib
import re
import subprocess
import sys
from typing import NamedTuple

sys.dont_write_bytecode = True
TOOLS = pathlib.Path(__file__).resolve().parent
STOCKFW = TOOLS.parent
REPO = TOOLS.parents[2]


def load(name: str):
    """tools/<name>.py as module <name>, once per process (later calls return the same module)."""
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


# Every round trip runs the stock image: without target/CAx9_P06.SYS (not in the repository) it prints SKIP and
# exits 0 here, before it builds or reads anything; a file there that is not the stock image fails.
load("patcher").stock_bytes()


# ---- output -------------------------------------------------------------------------------------------
def check(name: str, ok, detail: str = "", quiet: bool = False):
    """One check line `  [PASS] name - detail` (quiet: printed only when it fails); returns ok unchanged."""
    if not quiet or not ok:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}{(' - ' + detail) if detail else ''}")
    return ok


def finish(ok, note: str = "") -> int:
    """The verdict line every round trip ends with; the exit status."""
    print(("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED") + note)
    return 0 if ok else 1


def first_arg(argv: list[str], default: str) -> str:
    """The first argument that is not an option nor an option's value (`--shard i/n`; argv[0] = the script)."""
    return next((a for k, a in enumerate(argv[1:], 1) if not a.startswith("-") and argv[k - 1] != "--shard"), default)


# ---- bytes --------------------------------------------------------------------------------------------
def le(b, o: int = 0, n: int = 4) -> int:
    """Little-endian unsigned of b[o:o + n]."""
    return int.from_bytes(bytes(b[o:o + n]), "little")


def pack7(raw: bytes) -> bytes:
    """The cave's 7-in-8 packing: per group of up to 7 bytes a prefix byte holding their bit 7."""
    out = bytearray()
    for i in range(0, len(raw), 7):
        g = raw[i:i + 7]
        out.append(sum(((b >> 7) & 1) << k for k, b in enumerate(g)))
        out += bytes(b & 0x7F for b in g)
    return bytes(out)


def unpack7(data: bytes) -> bytes:
    """Inverse of the cave's pack7(): prefix byte with bit 7 of each following byte."""
    out, i = bytearray(), 0
    while i < len(data):
        hi = data[i]
        grp = data[i + 1:i + 8]
        out += bytes(b | (0x80 if hi >> k & 1 else 0) for k, b in enumerate(grp))
        i += 1 + len(grp)
    return bytes(out)


# ---- built inputs (make image) ------------------------------------------------------------------------
def cave_symbols(variant: str) -> dict[str, int]:
    """Cave entry addresses from the built ELF (nm), never hard-coded."""
    return load("make_phase2_manifest").symbols(STOCKFW / f"cave/build/cave_{variant}.elf")


class Loader(NamedTuple):
    img: object          # images.CaveImage
    stock: bytes         # target/CAx9_P06.SYS
    manifest: dict       # patches/<name>.json
    image: bytes         # stock + manifest (= build/<name>.SYS when image-check passes); v4: + the loader seed
    syms: dict           # cave symbols of the image's cave (v4: stage-0 + stage-1, see loader())


DEFAULT_LOADER = "ota_loader_v4"
# loaders with the 17-site allow-list (task-20 flag, svc version >= 2)
ALLOW17 = ("ota_loader_v3", "ota_loader_v4")
STAGE1 = "ota_stage1"


def stage1_symbols(variant: str = "s1") -> dict[str, int]:
    """Symbols of a built stage-1 (cave/build/loader_<variant>.elf)."""
    return load("make_phase2_manifest").symbols(STOCKFW / f"cave/build/loader_{variant}.elf")


def loader_seed(c79: bytes, *, seq: int = 1, confirmed: bool = True, target: int = 0x10) -> bytes:
    """A loader / main slot sector's contents as an install image seeds it (make_phase2_manifest.seed_blob)."""
    images = load("images")
    return load("make_phase2_manifest").seed_blob(images.Seed(target, "-", seq, confirmed), c79)


def with_seed(image: bytes, target: int, blob: bytes) -> bytes:
    """image with one slot sector replaced by blob (FF-padded to the sector)."""
    off = load("images").SLOT_FILE_OFFSET[target]
    b = bytearray(image)
    b[off:off + 0x20000] = blob + b"\xff" * (0x20000 - len(blob))
    return bytes(b)


def loader(name: str = DEFAULT_LOADER) -> Loader:
    """A cave image patched from its manifest (the round trips run the manifest, not the built file).

    ota_loader_v4 (stage-0 alone boots stock): the image is returned with loader slot A seeded with build/ota_stage1.c79
    (seq 1, confirmed - as opengrand_install seeds it), main slots empty: the v4 equivalent of the v3 "loader alone",
    the base the payload round trips start from.  syms = stage-0's symbols + stage-1's (stage-0 wins a name both
    have; every stage-1 name is also there as 's1.<name>'), plus the v3 names the shared harness calls:
    cave_tick = s0_tick, cave_h1 = s0_h1, ng_sysex = s0_sysex (r3 = the frame pointer, unused), ota_cache_sync =
    s0_cache_sync."""
    pa, images = load("patcher"), load("images")
    img = images.by_name(name)
    manifest = pa.load_manifest(STOCKFW / "patches" / f"{img.name}.json")
    stock = pa.stock_bytes()
    image = pa.apply_patches(stock, manifest)
    syms = cave_symbols(img.variant)
    if img.stage0:
        s1 = stage1_symbols()
        syms = {**s1, **syms, **{f"s1.{k}": v for k, v in s1.items()},
                "cave_tick": syms["s0_tick"], "cave_h1": syms["s0_h1"], "ng_sysex": syms["s0_sysex"],
                "ota_cache_sync": syms["s0_cache_sync"]}
        image = with_seed(image, 0x10, loader_seed((STOCKFW / "build" / f"{STAGE1}.c79").read_bytes()))
    return Loader(img, stock, manifest, image, syms)


class Payload(NamedTuple):
    p: object            # images.OtaPayload
    data: bytes          # build/<name>.c79 (64 B header + payload)
    pay: bytes           # data[64:]
    plen: int            # len(pay)
    pelf: pathlib.Path   # cave/build/payload_<variant>.elf
    psyms: dict          # its symbols


def payload(name: str) -> Payload:
    """A signed slot payload from build/ and the symbols of its ELF."""
    images, mm = load("images"), load("make_phase2_manifest")
    p = images.OTA_PAYLOADS_BY_NAME[name]
    data = (STOCKFW / "build" / f"{name}.c79").read_bytes()
    pelf = STOCKFW / f"cave/build/payload_{p.variant}.elf"
    return Payload(p, data, data[64:], len(data) - 64, pelf, mm.symbols(pelf))


def objdump(elf: pathlib.Path) -> str:
    """`arm-none-eabi-objdump -d` of a cave / payload ELF (the pinned toolchain)."""
    od = load("make_phase2_manifest").NM.with_name("arm-none-eabi-objdump")
    return subprocess.run([str(od), "-d", str(elf)], capture_output=True, text=True, check=True).stdout


# objdump's encoding column: one ARM word (8 hex digits) or a Thumb instruction (one or two halfwords, "b5f7" /
# "f6c5 ef1e").  objdump follows the $a / $t / $d mapping symbols, so a Thumb slot payload (cave Makefile THUMB=1) is listed in the right state and literal pools stay `.word`.
ENC = r"(?P<enc>[0-9a-f]{8}|[0-9a-f]{4}(?: [0-9a-f]{4})?)"


def instructions(elf: pathlib.Path, stop: str = ";@") -> list[tuple[int, str, str]]:
    """(address, mnemonic, operands) of every instruction in the objdump listing; the operands end before
    the first character of `stop` (objdump's `; 0x...` / `@ ...` annotations, '<' also cuts `<symbol>`).
    ARM and Thumb alike (instructions_x says which)."""
    return [(a, m, o) for a, m, o, _t in instructions_x(elf, stop)]


def instructions_x(elf: pathlib.Path, stop: str = ";@") -> list[tuple[int, str, str, bool]]:
    """instructions() + whether the instruction is Thumb (its encoding is halfwords, not one ARM word)."""
    rx = re.compile(r"\s*([0-9a-f]+):\s+" + ENC + r"\s+(\S+)\s*([^" + re.escape(stop) + r"]*)")
    return [(int(m[1], 16), m[3], m[4].strip(), len(m["enc"]) != 8)
            for m in (rx.match(l) for l in objdump(elf).splitlines()) if m]


def pc_literal(addr: int, imm: int, thumb: bool) -> int:
    """Address an `ldr rN, [pc, #imm]` at addr reads: ARM pc = addr + 8; Thumb pc = Align(addr + 4, 4)."""
    return ((addr + 4) & ~3) + imm if thumb else addr + 8 + imm


# ---- virtual time: FUN_4014BCAC's poll loop -----------------------------------------------------------
# The stock reliable sender's ACK wait FUN_4014BCAC (REA, CAx9_P06.SYS, Ghidra procedure_assembly 0x4014BCAC):
#   0x4014BCDC cmp r0 (ACK flag), 0 / ldmiane -> 0      0x4014BCE8 cmp r12 (Wait flag), 0 / bne 0x4014BCDC
#   0x4014BCF0 ldrb r12, [r5] (NAK flag) / ldmiane -> 1  0x4014BD00 bl 0x400AB2E8 (2 ms counter)
#   0x4014BD04 cmp r4 (deadline), r0 / ldrbgt the two flags again / bgt 0x4014BCDC / else -> 2
# One pass reads the three flags and the counter and nothing else: no store, no call but the counter read, no
# pass count.  The round-trip clocks advance 10 us per counter read; a pass changes nothing the next pass could
# see unless a controller frame was delivered (the flags) or the 2 ms counter moved (the deadline compare).
WAIT_POLL_LR = 0x4014BD04          # return address of the in-loop counter read


def poll_step(em, t: int, horizon: int, tick, step: int = 10, tick_us: int = 2000) -> int:
    """How far a round-trip clock moves at one 2 ms counter read (the clocks move `step` us per read);
    tick(t) = the counter value the clock shows at virtual time t.

    At FUN_4014BCAC's in-loop read, when this pass stays in the loop (deadline r4 > the counter it reads), the
    passes after it that see the same counter and the same flags are skipped: the clock lands on the first read
    whose time reaches the next counter tick or `horizon` (the next frame / event due), on the same `step` grid
    the one-by-one passes would reach - so every delivery, preemption, counter value and loop exit happens at
    the same virtual time as before; only passes that could not observe anything new are not emulated."""
    if em.reg("lr") != WAIT_POLL_LR or not em.reg("r4") > tick(t + step):
        return step
    target = min(horizon, (t // tick_us + 1) * tick_us)
    return max(step, -(-(target - t) // step) * step)


# ---- machine snapshots: the same setup run once per process ---------------------------------------------
# memo(key, objs, fn) runs fn() (a setup step on a machine: boot + OTA upload + apply ...) and keeps the state it
# left: the emulator RAM (StockEmu._ram ranges, readback stub stores), the CPU context, and every plain-data
# attribute of objs (the Rig / Piano / NorFlash / StockEmu objects of that machine: NOR array, logs, clocks ...).
# The next time fn would run with the same key on a machine whose state before it is the same (a hash over all
# of the above, the hooks, the peripheral stubs and every callable attribute's name), that state is written into
# the machine instead - the bytes and logs the run would have produced, without emulating it again.  Not kept
# when fn added or removed a hook or a non-data attribute (then fn always runs).  Objects the hooks reach (a UART
# model, Hier, the rig) join the machine; a hook reaching a bare container nobody holds -> fn always runs.
# RT_MEMO=0 turns it off; RT_MEMO=verify runs fn on every hit too and fails unless its state hash equals the
# remembered one (all round trips passed that, 2026-11-02).
import hashlib as _hashlib  # noqa: E402
import os as _os            # noqa: E402

_IMM = (int, float, str, bytes, bool, type(None), complex)
_MEMO: dict = {}
_MEMO_MAX = 4
_SKIP = {"uc", "_rr", "_rw", "_exits", "_guard_read_hooks", "_hooks", "_write_hooks", "_stubs", "nor", "emu",
         "pn", "rig", "periph"}


class _NotPlain(Exception):
    pass


def _imm(v) -> bool:
    return type(v) in _IMM or (isinstance(v, tuple) and all(_imm(x) for x in v))


def _copy(v):
    """A copy of plain data (immutable parts shared); _NotPlain for anything else."""
    t = type(v)
    if t in _IMM:
        return v
    if t is bytearray:
        return bytearray(v)
    if isinstance(v, tuple):
        if _imm(v):
            return v
        items = [_copy(x) for x in v]
        return t._make(items) if hasattr(t, "_make") else t(items)
    if t is list:
        # long logs (Write / NorCycle records ...): one record type, immutable records -> share the records
        if len(v) > 256 and isinstance(v[0], tuple) and _imm(v[0]) and len(set(map(type, v))) == 1:
            return list(v)
        return [_copy(x) for x in v]
    if t is dict:
        return {_copy(k): _copy(x) for k, x in v.items()}
    if t is set:
        return {_copy(x) for x in v}
    if t.__name__ == "WriteLog":         # stockemu.WriteLog (a packed list of Write records)
        return v.copy()
    raise _NotPlain(t.__name__)


def _feed(h, v):
    t = type(v)
    if t is bytes or t is bytearray:
        h.update(b"B%d:" % len(v))
        h.update(v)
    elif t in _IMM:
        h.update(repr(v).encode())
    elif isinstance(v, (list, tuple)):
        h.update(b"[%d" % len(v))
        for x in v:
            _feed(h, x)
        h.update(b"]")
    elif t is dict:
        h.update(b"{%d" % len(v))
        for k in sorted(v, key=repr):
            _feed(h, k)
            _feed(h, v[k])
        h.update(b"}")
    elif t is set:
        _feed(h, sorted(v, key=repr))
    elif t.__name__ == "WriteLog":
        h.update(b"W%d:" % len(v))
        h.update(v.packed())
    else:   # a callable or an object: what it is, not its contents
        h.update(("<" + getattr(v, "__qualname__", t.__qualname__) + ">").encode())


def _attrs(o):
    """(plain attributes, {name: kind of every other attribute})."""
    plain, other = {}, {}
    for k, v in vars(o).items():
        if k in _SKIP:
            other[k] = type(v).__qualname__
            continue
        try:
            _copy(v)
            plain[k] = v
        except _NotPlain:
            other[k] = getattr(v, "__qualname__", type(v).__qualname__)
    return plain, other


def _emu_of(objs):
    return next(o for o in objs if hasattr(o, "uc") and hasattr(o, "_ram"))


def _state(objs, digest=True):
    """Hash of the machine state memo() keys on (digest), plus the non-plain shape (hooks, callables)."""
    h = _hashlib.blake2b(digest_size=20)
    emu = _emu_of(objs)
    shape = []
    for o in objs:
        plain, other = _attrs(o)
        shape.append(type(o).__qualname__)
        if digest:
            for k in sorted(plain):
                h.update(k.encode())
                _feed(h, plain[k])
        shape.append(sorted(other.items()))
    shape.append(sorted((a, n, getattr(f, "__qualname__", "?")) for a, (f, _hd, n) in emu._hooks.items()))
    shape.append([(lo, hi, getattr(f, "__qualname__", "?")) for lo, hi, f in emu._write_hooks])
    shape.append(sorted((a, v if _imm(v) else getattr(v, "__qualname__", type(v).__qualname__))
                        for a, v in emu.periph.items()))
    shape.append([(s.base, s.size, s.name, s.store is not None) for s in emu._stubs])
    if not digest:
        return None, repr(shape)
    _feed(h, repr(shape))
    for lo, hi in emu._ram:
        h.update(emu.uc.mem_read(lo, hi - lo))
    for s in emu._stubs:
        if s.store is not None:
            h.update(s.store)
    return h.hexdigest(), repr(shape)


def _reach(objs, emu):
    """The objects the hook / stub callbacks of emu can change besides objs: instances they reach (Hier, a UART
    model, the Rig ...) are added to the machine (their data attributes kept and restored like objs'); None when
    one reaches a bare list / dict / set that no machine object holds - a restore could not reach it."""
    objs = list(objs)
    own = {id(o) for o in objs}
    for o in objs:
        own.update(id(v) for v in vars(o).values())
    seen = set()

    def walk(v, depth):
        if id(v) in own or id(v) in seen or type(v) in _IMM or depth > 4:
            return True
        seen.add(id(v))
        if isinstance(v, type) or type(v).__name__ in ("module", "builtin_function_or_method", "method-wrapper"):
            return True
        if hasattr(v, "__self__") and hasattr(v, "__func__"):               # bound method
            return walk(v.__self__, depth + 1) and walk(v.__func__, depth + 1)
        if hasattr(v, "__code__"):                                           # function: its closure and defaults
            cells = [c.cell_contents for c in (v.__closure__ or ()) if c.cell_contents is not None]
            return all(walk(x, depth + 1) for x in cells + list(v.__defaults__ or ()))
        if isinstance(v, tuple) and _imm(v):
            return True
        if hasattr(v, "__dict__") and not isinstance(v, (list, dict, set, bytearray)):
            objs.append(v)                                                   # an instance: part of the machine
            own.add(id(v))
            own.update(id(x) for x in vars(v).values())
            return all(walk(x, depth + 1) for x in vars(v).values() if callable(x))
        return False

    fns = [f for f, _h, _n in emu._hooks.values()] + [f for _lo, _hi, f in emu._write_hooks] + \
          [v for v in emu.periph.values() if callable(v)]
    return objs if all(walk(f, 0) for f in fns) else None


def memo(key, objs, fn):
    """fn() on the machine objs ([rig, piano, nor, emu] ...), or its remembered outcome (see above)."""
    mode = _os.environ.get("RT_MEMO", "1")
    if mode == "0":
        return fn()
    objs = _reach([o for o in dict.fromkeys(objs) if o is not None], _emu_of([o for o in objs if o is not None]))
    if objs is None:
        return fn()
    sig, shape = _state(objs)
    hit = _MEMO.get((key, sig))
    emu = _emu_of(objs)
    if hit is not None and mode == "verify":            # run it anyway; the state must equal the remembered one
        res = fn()
        if _state(objs)[0] != hit[0]["post"] or _copy(res) != hit[1]:
            raise RuntimeError(f"rtlib.memo {key[0]}: the restored state would differ from a real run")
        print(f"rtlib.memo {key[0]}: verified", file=sys.stderr)
        return res
    if hit is not None:
        snap, res = hit
        for o, attrs in zip(objs, snap["attrs"]):
            for k, v in attrs.items():
                setattr(o, k, _copy(v))
        for a, v in snap["periph"].items():
            emu.periph[a] = _copy(v)
        for (lo, _hi), blob in zip(emu._ram, snap["ram"]):
            emu.uc.mem_write(lo, blob)
        for s, blob in zip(emu._stubs, snap["stubs"]):
            if blob is not None:
                s.store[:] = blob
        emu.uc.context_restore(snap["ctx"])
        emu.uc.ctl_flush_tb()
        return _copy(res)
    res = fn()
    _sig2, shape2 = _state(objs, digest=False)
    if shape2 != shape:                                  # fn changed hooks / callables: not restorable
        return res
    try:
        snap = dict(attrs=[{k: _copy(v) for k, v in _attrs(o)[0].items()} for o in objs],
                    periph={a: _copy(v) for a, v in emu.periph.items() if not callable(v)},
                    ram=[bytes(emu.uc.mem_read(lo, hi - lo)) for lo, hi in emu._ram],
                    stubs=[bytes(s.store) if s.store is not None else None for s in emu._stubs],
                    ctx=emu.uc.context_save(),
                    post=_state(objs)[0] if mode == "verify" else None)
        r = _copy(res)
    except _NotPlain:
        return res
    if len(_MEMO) >= _MEMO_MAX:
        _MEMO.pop(next(iter(_MEMO)))
    _MEMO[(key, sig)] = (snap, r)
    return res


# ---- scenario units: shards and forked fresh machines -------------------------------------------------
# A long round trip lists its independent scenarios as Units.  `--shard i/n` runs the i-th (1-based) of n
# cost-balanced groups (the Makefile lists each group as its own job, so the check runner spreads them over the
# cores); without it every unit runs, in order, in one process.  Each unit runs in a forked child of the round
# trip's process: the machines a unit asks for with fresh(key) are built once in the parent before the first
# fork (`Unit.sides`) and every child gets its own untouched copy - the same state a new build gives, without
# paying the boot / OTA apply again.  A unit returns a picklable value (its verdict, rows of a table ...).
import pickle as _pickle    # noqa: E402
import time as _time        # noqa: E402
import traceback as _tb     # noqa: E402


class Unit(NamedTuple):
    name: str
    fn: object                  # fn(*results of `needs`) -> picklable result
    cost: float = 1.0           # rough seconds, to balance the shards
    sides: tuple = ()           # fresh() keys the unit takes (built once, before the forks)
    needs: tuple = ()           # names of earlier units whose results fn takes (run silently in a shard without them)


_BUILDERS: dict = {}
_BUILT: dict = {}


def builders(**kw):
    """Register the machine builders fresh(key) uses: builders(S=lambda: ..., H=lambda: ...)."""
    _BUILDERS.update(kw)


def fresh(key: str):
    """A new machine of kind `key`: the copy built before this unit's fork (once per unit), else a new build."""
    obj = _BUILT.pop(key, None)
    return obj if obj is not None else _BUILDERS[key]()


def shard_of(argv: list[str]):
    """(i, n) from `--shard i/n` (1 <= i <= n), else None."""
    for k, a in enumerate(argv):
        if a == "--shard" and k + 1 < len(argv):
            i, n = (int(x) for x in argv[k + 1].split("/"))
            if not 1 <= i <= n:
                raise SystemExit(f"--shard {i}/{n}: need 1 <= i <= n")
            return i, n
    return None


def partition(units: list, n: int) -> list[list]:
    """n groups of units: longest first onto the group it makes the lightest (deterministic), each group in list
    order.  A group's load counts the units it runs silently for `needs` too."""
    by_name = {u.name: u for u in units}

    def load(ks):
        names, todo = set(), [units[k].name for k in ks]
        while todo:
            x = todo.pop()
            if x not in names:
                names.add(x)
                todo += list(by_name[x].needs)
        return sum(by_name[x].cost for x in names)

    groups = [[] for _ in range(n)]
    for k, u in sorted(enumerate(units), key=lambda x: (-x[1].cost, x[0])):
        g = min(range(n), key=lambda j: (load(groups[j] + [k]), load(groups[j]), j))
        groups[g].append(k)
    return [[units[k] for k in sorted(g)] for g in groups]


def select(units: list, argv: list[str]) -> list:
    """The units this process runs: all, or the `--shard i/n` group."""
    sh = shard_of(argv)
    return list(units) if sh is None else partition(units, sh[1])[sh[0] - 1]


def _fork_run(u, args, silent=False):
    """u.fn(*args) in a forked child, its picklable result (None when it raised or died); silent = its output
    discarded (a prerequisite of another shard's unit: its own checks count where it is selected)."""
    sys.stdout.flush()
    sys.stderr.flush()
    rfd, wfd = _os.pipe()
    t0 = _time.monotonic()
    pid = _os.fork()
    if pid == 0:                                         # child: the unit on its own copy of the machines
        _os.close(rfd)
        if silent:
            sink = _os.open(_os.devnull, _os.O_WRONLY)
            _os.dup2(sink, 1)
            _os.dup2(sink, 2)
        code = 0
        try:
            res = u.fn(*args)
        except BaseException:                            # noqa: BLE001 - reported, the unit fails
            _tb.print_exc()
            res, code = None, 1
        sys.stdout.flush()
        sys.stderr.flush()
        with _os.fdopen(wfd, "wb") as fh:
            _pickle.dump(res, fh)
        _os._exit(code)
    _os.close(wfd)
    with _os.fdopen(rfd, "rb") as fh:
        blob = fh.read()
    _pid, status = _os.waitpid(pid, 0)
    if _os.environ.get("RT_UNIT_TIMES"):
        print(f"unit {u.name}{' (prerequisite)' if silent else ''} {_time.monotonic() - t0:.1f}s", file=sys.stderr,
              flush=True)
    if not blob:
        print(f"  [FAIL] unit {u.name}: child died (exit {_os.waitstatus_to_exitcode(status)})")
        return None
    return _pickle.loads(blob)


def run_units(units: list, argv: list[str]) -> list:
    """Run the selected units, each in a forked child (one after the other, so the output keeps its order);
    returns [(unit, result)], result None when the unit raised (its traceback printed) or its child died.
    Units a selected unit `needs` that this shard does not select run first, silently, for their results."""
    sel = select(units, argv)
    by_name = {u.name: u for u in units}
    chosen = {u.name for u in sel}
    pre, todo = [], [n for u in sel for n in u.needs]
    while todo:
        n = todo.pop()
        if n not in chosen and n not in pre:
            pre.append(n)
            todo += list(by_name[n].needs)
    for key in dict.fromkeys(k for u in sel + [by_name[n] for n in pre] for k in u.sides):
        _BUILT[key] = _BUILDERS[key]()
    results, out = {}, []
    for u in units:
        if u.name not in chosen and u.name not in pre:
            continue
        args = [results.get(n) for n in u.needs]
        if any(a is None for a in args):
            res = None
            print(f"  [FAIL] unit {u.name}: a unit it needs failed ({', '.join(u.needs)})")
        else:
            res = _fork_run(u, args, silent=u.name not in chosen)
        results[u.name] = res
        if u.name in chosen:
            out.append((u, res))
    for key in list(_BUILT):
        obj = _BUILT.pop(key)
        if hasattr(obj, "close"):
            obj.close()
    return out


def shard_note(units: list, argv: list[str]) -> str:
    """' - shard i/n: names' for the verdict line ('' when every unit ran)."""
    sh = shard_of(argv)
    return "" if sh is None else f" - shard {sh[0]}/{sh[1]}: {' '.join(u.name for u in select(units, argv))}"
