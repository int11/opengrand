# make_phase2_manifest.py - generate a patch manifest for a cave build.
#
# Computes the hook branch encodings from the cave ELF's own symbols instead of
# hard-coding them, so the manifest can never disagree with the binary it ships.
#
#   cd result/stockfw
#   uv run python -I tools/make_phase2_manifest.py <variant>   # a cave image of tools/images.py CAVE_IMAGES (s0)
#   uv run python -I tools/make_phase2_manifest.py opengrand_install   # an install image (INSTALL_IMAGES): the loader
#                                                  # manifest + one seed blob patch per slot (build/<name>.seed_*.bin)
#
# The variants and their feature bits are the image table in tools/images.py; the cave
# must already be built (tools/build_images.py, or `make image` at the repo root, does both).
from __future__ import annotations

import importlib.util
import json
import pathlib
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent


def _load(name):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


images = _load("images")
STOCKFW = HERE.parent
ROOT = STOCKFW.parents[1]
NM = STOCKFW / ".toolchain/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-nm"

VA_BASE = 0x40060000          # file offset 0
CAVE_VA = 0x40173A14          # cave placement option A
LOADER_END_LITERAL = 0x400AB3EC  # [this] = end VA of loader copy #1; length = end - 0x400C0000
LOADER_END_STOCK = 0x40173A04
V4_LOADER_END = 0x40176000    # ota_loader_v4: copy #1 = stage-0 + the root key at 0x40175FE0 (include/s0.h)

# ota_loader_v4 stage-0: the same two sites, its own entries (src/s0.c)
HOOKS = {
    0x40147E1C: (0x1A00001D, "s0_h1", "bne",
                 "SysEx end handler: unknown manufacturer ID falls through to 0x40147E98. Stage-0 hands 0x7D to the "
                 "running stage-1 and jumps to the same place for everything else (registers as they came)."),
    0x40143AE0: (0xEBFFFC6A, "s0_tick", "bl",
                 "Engine loop tail (task 24, ~8 ms). Stage-0 runs its boot step or the stage-1 tick and tail-calls "
                 "the original 0x40142C90, whose return value the loop ignores."),
}
COND = {"bne": 0x1, "bl": 0xE}
OPC = {"bne": 0xA, "bl": 0xB}


def branch_word(site: int, target: int, kind: str) -> int:
    delta = target - (site + 8)
    if delta % 4:
        raise SystemExit("branch target is not word aligned")
    imm = (delta // 4) & 0xFFFFFF
    if not (-0x800000 <= delta // 4 <= 0x7FFFFF):
        raise SystemExit("branch target out of ARM +-32 MB range")
    return (COND[kind] << 28) | (OPC[kind] << 24) | imm


def symbols(elf: pathlib.Path) -> dict[str, int]:
    out = subprocess.run([str(NM), str(elf)], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def write_manifest(variant: str, *, quiet: bool = False) -> pathlib.Path:
    if variant not in images.CAVE_IMAGES:
        raise SystemExit(f"variant must be one of {', '.join(images.CAVE_IMAGES)} (tools/images.py)")
    img = images.CAVE_IMAGES[variant]
    name, description = img.name, img.description
    elf = STOCKFW / f"cave/build/cave_{variant}.elf"
    binary = STOCKFW / f"cave/build/cave_{variant}.bin"
    if not elf.is_file() or not binary.is_file():
        raise SystemExit(f"build the cave first: cd cave && make V={variant} FEAT0=0x{img.feat0:02X} "
                         f"FEAT1=0x{img.feat1:02X}")

    syms = symbols(elf)
    blob = binary.read_bytes()
    cave_end = CAVE_VA + len(blob)
    entry = "s0_h1"
    if not img.stage0:
        raise SystemExit(f"{variant}: the only cave image is ota_loader_v4's stage-0")
    if syms.get(entry) != CAVE_VA:
        raise SystemExit(f"{entry} is at 0x{syms.get(entry, 0):08x}, expected 0x{CAVE_VA:08x}")
    if syms.get("cave_end") != cave_end:
        raise SystemExit("cave_end symbol disagrees with the binary size")

    patches = [{
        "id": "cave",
        "file_offset": hex(CAVE_VA - VA_BASE),
        "va": hex(CAVE_VA),
        "blob": f"cave/build/cave_{variant}.bin",
        "expect_fill": "ff",
        "note": (f"OpenGrand cave payload, {len(blob)} bytes, ends at va 0x{cave_end:08x}. "
                 "Lands in the erased hole after loader copy #1 (cave placement option A)."),
    }, {
        "id": "loader_end",
        "file_offset": hex(LOADER_END_LITERAL - VA_BASE),
        "va": hex(LOADER_END_LITERAL),
        "orig": struct.pack("<I", LOADER_END_STOCK).hex(),
        "new": struct.pack("<I", cave_end).hex(),
        "note": ("End VA of loader copy #1 (FUN_400AA994: len = this - 0x400C0000). Extended so "
                 f"the cave is copied from NOR to SDRAM too: 0x{LOADER_END_STOCK:08x} -> 0x{cave_end:08x}."),
    }]

    if img.stage0:
        # ota_loader_v4: the root key above stage-0 (s0.ld s0_key) as its own blob patch read from the git-ignored
        # secrets/, loader copy #1 ends at the stage-1 window 0x40176000 (stage-0 + key reach SDRAM, the window is
        # loaded by stage-0 itself).  The tracked stage-0 blob never contains the key.
        key_va = syms["s0_key"]
        if syms["cave_end"] > key_va or key_va + 32 != V4_LOADER_END:
            raise SystemExit("ota_loader_v4: stage-0 runs into the key, or the key is not right below the window")
        patches.append({
            "id": "ota_key",
            "file_offset": hex(key_va - VA_BASE),
            "va": hex(key_va),
            "blob": "secrets/ota_key.bin",
            "expect_fill": "ff",
            "note": "Per-device root key (32 B, git-ignored secrets/ota_key.bin, tools/ota_pack.py). Stage-0 verifies "
                    "loader images with it; stage-1 v4.0 uses it for main images and the authenticated ops "
                    "(REMOTE_PROTOCOL 10.3 / 10.12).",
        })
        patches[0]["note"] = (f"ota_loader_v4 stage-0, {len(blob)} bytes, ends at va 0x{cave_end:08x} (budget "
                              f"0x{key_va:08x}). Lands in the erased hole after loader copy #1.")
        patches[1]["new"] = struct.pack("<I", V4_LOADER_END).hex()
        patches[1]["note"] = ("End VA of loader copy #1 (FUN_400AA994: len = this - 0x400C0000). Extended so "
                              f"stage-0 and the root key are copied to SDRAM: 0x{LOADER_END_STOCK:08x} -> "
                              f"0x{V4_LOADER_END:08x} (the stage-1 window above is loaded by stage-0).")

    for site, (stock_word, sym, kind, note) in HOOKS.items():
        target = syms[sym]
        patches.append({
            "id": f"hook_{sym}",
            "file_offset": hex(site - VA_BASE),
            "va": hex(site),
            "orig": struct.pack("<I", stock_word).hex(),
            "new": struct.pack("<I", branch_word(site, target, kind)).hex(),
            "note": f"{kind} -> {sym} (0x{target:08x}). {note}",
        })

    manifest = {
        "name": name,
        "description": description,
        "base_sha256": "09ad85703e44ad95d82475cfe2dae14c6edfa9f164a0b17b66b45f74c8d933c8",
        "patches": patches,
    }
    out = STOCKFW / "patches" / f"{name}.json"
    out.write_text(json.dumps(manifest, indent=2) + "\n")
    if quiet:
        return out
    print(f"wrote {out}")
    print(f"  cave {len(blob)} bytes, 0x{CAVE_VA:08x}-0x{cave_end:08x}")
    for p in patches:
        if "orig" in p:
            print(f"  {p['id']}: {p['orig']} -> {p['new']} @ va {p['va']}")
    return out


def seed_blob(seed, c79: bytes) -> bytes:
    """The slot contents an install image pre-seeds (images.Seed): the state area exactly as stage-1's own commit
    leaves it - magic ('C79L' loader / 'C79S' main), seq, state version 1 at +0xE, every other halfword erased - plus
    the confirm halfword 0000 for a pre-confirmed seed, followed by the signed .c79 (roundtrip_v4_boot.py checks that
    an upload + commit leaves the same state area)."""
    state = bytearray(b"\xff" * images.SLOT_STATE_SIZE)
    state[0:4] = images.LOADER_STATE_MAGIC if seed.loader else images.SLOT_STATE_MAGIC
    state[4:8] = struct.pack("<I", seed.seq)
    state[0xE:0x10] = struct.pack("<H", 1)
    if seed.confirmed:
        state[8:10] = b"\0\0"
    return bytes(state) + c79


def seed_path(name: str, seed) -> str:
    """The seed blob of one slot of an install image (git-ignored build/, key-dependent)."""
    return f"build/{name}.seed_{seed.tag}.bin"


def write_install_manifest(name: str, *, quiet: bool = False) -> pathlib.Path:
    """patches/<install>.json: the loader image's manifest + one blob patch per seeded slot at the slot's file offset.
    The blobs (build/<install>.seed_<tag>.bin, written by build_images.py) hold key-signed .c79 files, so they are
    referenced from the git-ignored build/, never from a tracked directory."""
    inst = images.INSTALL_IMAGES[name]
    loader = images.CAVE_IMAGES[inst.loader]
    base = json.loads((STOCKFW / "patches" / f"{loader.name}.json").read_text())
    patches = list(base["patches"])
    for seed in inst.seeds:
        off = images.SLOT_FILE_OFFSET[seed.target]
        what = images.SLOT_NAME[seed.target]
        patches.append({
            "id": f"seed_{seed.tag}",
            "file_offset": hex(off),          # no "va": the slot sectors are never copied to SDRAM
            "blob": seed_path(name, seed),
            "expect_fill": "ff",
            "keyed": f"c79@0x{images.SLOT_STATE_SIZE:x}",
            "note": (f"{what} slot (NOR 0x{0x60060000 + off:08x}) pre-seeded with {seed.image}.c79: state "
                     f"'{'C79L' if seed.loader else 'C79S'}' + seq {seed.seq} + state version 1, "
                     f"{'confirmed (+8 = 0000)' if seed.confirmed else 'unconfirmed'}, the rest of the state area "
                     "erased, the signed file at +0x200. The slot sectors lie in the erased hole of MainProgram that "
                     "no stock copy reads (loader copy #1 ends at file 0x116000). Key-dependent (a signed .c79): the "
                     "blob lives under the git-ignored build/."),
        })
    manifest = {"name": name, "description": inst.description, "base_sha256": base["base_sha256"],
                "patches": patches}
    out = STOCKFW / "patches" / f"{name}.json"
    text = json.dumps(manifest, indent=2) + "\n"
    if not out.is_file() or out.read_text() != text:
        out.write_text(text)
    if not quiet:
        print(f"wrote {out}")
    return out


def main(argv: list[str]) -> int:
    arg = argv[1] if len(argv) > 1 else "a"
    if arg in images.INSTALL_IMAGES:
        write_install_manifest(arg)
        return 0
    write_manifest(arg.lower())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
