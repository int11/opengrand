# stack_budget.py - static worst-case stack depth and size budget of a CORE cave build.
#
# Reads what the cave Makefile writes for CORE images (cave/build/core_<variant>/):
#   *.ci       GCC -fcallgraph-info=su call graphs (per-function frame size + call edges)
#   cave.map   linker map (which input sections survived --gc-sections, and their sizes)
# and prints, for each hook entry, the deepest call chain inside the cave, plus the size
# of the cave by kind (text / rodata / data) and by origin (cave framework / core / port).
#
#   cd result/stockfw && uv run python -I tools/stack_budget.py [ota3]
#
# Before `make image` (a fresh checkout) there is no cave/build/core_<variant>/: layout() falls back to the tracked ELF
# (elf_layout: exact code end, the rest of .cave counted as data, no per-object split) and the checks that need the
# map per object or the call graphs skip (have_map() False).
#
# Rules the graph needs from outside:
#   - entry.S has no .ci: cave_h1 pushes 7 words, cave_tick 2 (read off entry.S).
#   - every indirect call in the core is a kproto kp_io_t callback, and the cave port sets
#     only the io_* functions of core_cave.c as callbacks (core_reset), so an indirect call
#     may reach any of them.
#   - calls that leave the cave (stk_*) are stock code: listed with their frame as read from
#     the stock prologue (STOCK_FRAMES), which is a lower bound - the emulator round trips measure
#     the real depth by running the stock code.
from __future__ import annotations

import pathlib
import re
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
CAVE = HERE.parent / "cave"

ENTRY_FRAMES = {"cave_h1": 28, "cave_tick": 8}          # entry.S: push {r4-r9,lr} / push {r4,lr}
ROOT_CALLS = {"cave_h1": ["ng_sysex"], "cave_tick": ["ng_tick"]}
ROOT_TASK = {"cave_h1": "task 19 (MIDI, stack 0x4000)", "cave_tick": "task 24 (engine loop, stack 0x1000)"}

# stock routines the core port may call: own frame from the prologue (+ what they call, as
# far as statically known).  TX writer: push 8 regs, then twai_sem (push 6 regs) / sig_sem.
STOCK_FRAMES = {
    "stk_tx_port0": 32 + 24, "stk_tx_port1": 32 + 24, "stk_tx_port2": 32 + 24, "stk_tx_port3": 32 + 24,
    "stk_time_us": 8 + 8,          # push {r4,lr} + the divide helper's push {r2,lr}
    "stk_bt_dialect": 8,
    "stk_param_apply": 36 + 0x54,  # push 9 regs, sub sp #0x54 (its callees not followed)
}
LIBC_ALIASES = {"__builtin_memset": "memset", "__builtin_memcpy": "memcpy"}


def parse_ci(folder: pathlib.Path):
    nodes, edges = {}, []          # title -> (name, bytes, qualifier, defined)
    for ci in sorted(folder.glob("*.ci")):
        for line in ci.read_text().splitlines():
            m = re.match(r'node: \{ title: "([^"]+)" label: "([^"\\]+)\\n([^"]*)"', line)
            if m:
                title, name, rest = m[1], m[2], m[3]
                st = re.search(r"(\d+) bytes \(([^)]+)\)", rest)
                if st:
                    nodes[title] = (name, int(st[1]), st[2], True)
                else:
                    nodes.setdefault(title, (name, 0, "extern", False))
                continue
            m = re.match(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"', line)
            if m:
                edges.append((m[1], m[2]))
    return nodes, edges


def build_graph(variant: str):
    folder = CAVE / "build" / f"core_{variant}"
    nodes, edges = parse_ci(folder)
    defined = {t: v for t, v in nodes.items() if v[3]}
    by_name: dict[str, list[str]] = {}
    for t, v in defined.items():
        by_name.setdefault(v[0], []).append(t)

    def resolve(target: str) -> list[str]:
        if target in defined:                       # same-TU call: full "file:name" title
            return [target]
        if target == "__indirect_call":
            return [t for t in defined if "ports/cave/core_cave.c:io_" in t]
        name = nodes.get(target, (target,))[0]
        name = LIBC_ALIASES.get(name, name)
        cands = by_name.get(name, [])
        if len(cands) == 1:                         # bare name = external symbol
            return cands
        if len(cands) > 1:
            raise SystemExit(f"ambiguous call target {name}: {cands}")
        return [f"stock:{name}"] if name.startswith("stk_") else [f"extern:{name}"]

    graph: dict[str, list[str]] = {t: [] for t in defined}
    for s, t in edges:
        graph.setdefault(s, []).extend(resolve(t))
    return defined, graph


def worst(defined, graph, start: str, stack=()):
    """(bytes, chain) of the deepest path from `start`; raises on recursion."""
    if start in stack:
        raise SystemExit("recursion: " + " -> ".join(stack + (start,)))
    if start.startswith("stock:"):
        name = start[6:]
        if name not in STOCK_FRAMES:
            raise SystemExit(f"no stock frame for {name}")
        return STOCK_FRAMES[name], [start]
    if start.startswith("extern:"):
        raise SystemExit(f"unresolved call {start[7:]} (from {stack[-1] if stack else '?'})")
    own = defined[start][1]
    best = (0, [])
    for callee in graph.get(start, []):
        b = worst(defined, graph, callee, stack + (start,))
        if b[0] > best[0]:
            best = b
    return own + best[0], [start] + best[1]


def root_depth(variant: str, root: str):
    defined, graph = build_graph(variant)
    best = (0, [])
    for call in ROOT_CALLS[root]:
        t = [x for x in defined if x == call]   # global functions are titled by bare name
        if len(t) != 1:
            raise SystemExit(f"{call}: {t}")
        b = worst(defined, graph, t[0])
        if b[0] > best[0]:
            best = b
    return ENTRY_FRAMES[root] + best[0], [root] + best[1]


def have_map(variant: str) -> bool:
    """The build intermediates (linker map, call graphs) of this variant are there."""
    return (CAVE / "build" / f"core_{variant}" / "cave.map").is_file()


def _elf_path(variant: str) -> pathlib.Path:
    cave = CAVE / "build" / f"cave_{variant}.elf"
    return cave if cave.is_file() else CAVE / "build" / f"payload_{variant}.elf"


def elf_layout(variant: str) -> list[tuple[int, int, str, str, str]]:
    """layout() without the linker map, from the tracked ELF (ELF32 little endian): .cave = code up to the end
    of the last function (= the first rodata input section: functions carry their literal pools), then the rest
    as one "data" span; .core_rw as one span. Object = "?" (unknown)."""
    d = _elf_path(variant).read_bytes()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    stroff = secs[shstrndx][4]
    name = lambda off, base: d[base + off:d.index(b"\0", base + off)].decode()
    by = {name(sh[0], stroff): (i, sh) for i, sh in enumerate(secs)}
    cave_i, cave = by[".cave"]
    sym_i, symtab = by[".symtab"]
    text_hi = cave[3]
    for o in range(symtab[4], symtab[4] + symtab[5], 16):
        _n, value, size, info, _other, shndx = struct.unpack_from("<IIIBBH", d, o)
        if info & 0xF == 2 and shndx == cave_i:          # STT_FUNC in .cave
            text_hi = max(text_hi, (value & ~1) + size)
    out = [(cave[3], text_hi, ".cave", "text", "?"), (text_hi, cave[3] + cave[5], ".cave", "data", "?")]
    if ".core_rw" in by:
        rw = by[".core_rw"][1]
        out.append((rw[3], rw[3] + rw[5], ".core_rw", "core_rw", "?"))
    return out


def layout(variant: str) -> list[tuple[int, int, str, str, str]]:
    """Input sections that survived the link, from the linker map, as
    (start, end, output section, kind, object) sorted by address; `end` is where the next
    input section starts, so merged string sections are not counted twice.
    No map (fresh checkout, before make image): elf_layout()."""
    if not have_map(variant):
        return elf_layout(variant)
    lines = (CAVE / "build" / f"core_{variant}" / "cave.map").read_text().splitlines()
    lines = lines[next(i for i, l in enumerate(lines) if l.startswith("Linker script and memory map")):]
    starts, section, name, end = [], None, None, {}
    for line in lines:
        m = re.match(r"^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", line)
        if m:
            section = m[1]
            end[section] = int(m[2], 16) + int(m[3], 16)
            continue
        m = re.match(r"^ (\.\S+)\s*$", line)
        if m:
            name = m[1]
            continue
        m = re.match(r"^ (\.\S+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o)", line)
        if m and section in (".cave", ".core_rw"):
            kind = re.match(r"\.(text|rodata|data|bss|core_rw)", m[1] or name)[1]
            starts.append((int(m[2], 16), section, kind, pathlib.Path(m[4]).stem))
        name = None
    starts.sort()
    out = []
    for i, (addr, sec, kind, obj) in enumerate(starts):
        nxt = starts[i + 1][0] if i + 1 < len(starts) and starts[i + 1][1] == sec else end[sec]
        out.append((addr, nxt, sec, kind, obj))
    return out


def origin_of(obj: str) -> str:
    return ("cave framework" if obj in ("entry", "ca79ng") else
            "port (ports/cave)" if obj in ("hal_cave", "core_cave", "cave_rt") else "core (src/)")


def sizes(variant: str):
    """Bytes of the cave by kind (text / rodata / data) and by origin, plus the NOLOAD core RW."""
    kinds = {"text": 0, "rodata": 0, "data": 0, "core_rw": 0}
    by_origin: dict[str, int] = {}
    for lo, hi, sec, kind, obj in layout(variant):
        kinds[kind if sec == ".cave" else "core_rw"] += hi - lo
        if sec == ".cave":
            by_origin[origin_of(obj)] = by_origin.get(origin_of(obj), 0) + hi - lo
    return kinds, by_origin


def report(variant: str = "ota3") -> dict:
    out = {}
    for root in ("cave_tick", "cave_h1"):
        n, chain = root_depth(variant, root)
        out[root] = (n, chain)
        names = [c.split(":")[-1] for c in chain]
        print(f"  {root:<10} {ROOT_TASK[root]:<36} worst {n:5d} B  " + " > ".join(names))
    kinds, origin = sizes(variant)
    print("  size     " + "  ".join(f"{k} {v}" for k, v in kinds.items()))
    print("  origin   " + "  ".join(f"{k} {v}" for k, v in origin.items()))
    out["sizes"], out["origin"] = kinds, origin
    return out


if __name__ == "__main__":
    v = sys.argv[1] if len(sys.argv) > 1 else "ota3"
    print(f"stack / size budget of cave_{v} (static, GCC -fcallgraph-info=su + linker map)")
    report(v)
