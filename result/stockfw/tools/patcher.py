# patcher.py - build a patched MainProgram image from the stock CAx9_P06.SYS and a patch manifest.
#
# Rules this tool enforces:
#   * output is a raw .SYS of exactly the stock size and layout (no header, no compression);
#   * every patch declares its original bytes, which must match the stock image byte for byte;
#   * the ONLY bytes that may differ from stock are the ones the manifest declares;
#   * no patch may touch a protected region (boot data, voice DB, DSP image, flash-write code...);
#   * revert(apply(stock)) must hash back to the stock sha256, or the build fails.
#
#   cd result/stockfw
#   uv run python -I tools/patcher.py build  patches/phase1_version.json [-o build/out.SYS]
#   uv run python -I tools/patcher.py verify patches/phase1_version.json build/out.SYS
#   uv run python -I tools/patcher.py revert patches/phase1_version.json build/out.SYS -o build/back.SYS
#   uv run python -I tools/patcher.py info   patches/phase1_version.json
#
# Nothing here writes to a device.  Flashing is always done by the user, after approval.
from __future__ import annotations

import hashlib
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
DEFAULT_IMAGE = ROOT / "target" / "CAx9_P06.SYS"
STOCK_SHA256 = "09ad85703e44ad95d82475cfe2dae14c6edfa9f164a0b17b66b45f74c8d933c8"
IMAGE_SIZE = 0x3A0000
VA_BASE = 0x40060000  # file offset 0 runs here in SDRAM
# target/ is not in the repository (Kawai's file is not ours to distribute): without it every stock-dependent step
# prints this and skips; a file that is there but is not the stock image fails.
STOCK_HINT = (f"put Kawai's official V2.19 system file CAx9_P06.SYS (sha256 {STOCK_SHA256}) into target/ "
              "to build the images and run the emulator checks")


def stock_present() -> bool:
    return DEFAULT_IMAGE.is_file()


def skip_no_stock(what: str = "") -> None:
    """target/CAx9_P06.SYS absent: print SKIP + where to get it, exit 0."""
    print(f"SKIP {what or pathlib.Path(sys.argv[0]).stem}: target/CAx9_P06.SYS absent - {STOCK_HINT}")
    sys.exit(0)


def stock_bytes(what: str = "") -> bytes:
    """The stock image target/CAx9_P06.SYS, checked. Absent: SKIP (exit 0). Wrong size / sha256: FAIL (exit 1)."""
    if not stock_present():
        skip_no_stock(what)
    img = DEFAULT_IMAGE.read_bytes()
    got = hashlib.sha256(img).hexdigest()
    if len(img) != IMAGE_SIZE or got != STOCK_SHA256:
        raise SystemExit(f"FAIL target/CAx9_P06.SYS is not the stock V2.19 image: {len(img)} bytes, sha256 {got} "
                         f"(expected {IMAGE_SIZE} bytes, sha256 {STOCK_SHA256})")
    return img

# File ranges that a patch may never touch.  Derived from the stock
# loader copies and the stock image's boundaries.
PROTECTED = [
    (0x1A0000, 0x2A0000, "TG voice DB (loader copy #2)"),
    (0x2A0000, 0x2C0000, "drum rhythms (loader copy #3)"),
    (0x2C0000, 0x300000, "touch curves + external DSP parameters (loader copy #4)"),
    (0x300000, 0x360000, "bitmaps (loader copy #5)"),
    (0x360000, 0x394180, "C674x DSP image"),
]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_manifest(path: pathlib.Path) -> dict:
    """Read a manifest. A patch is either literal bytes (orig/new hex) or a blob:

       {"id": "cave", "file_offset": "0x113A14", "blob": "cave/build/cave_a.bin",
        "expect_fill": "ff"}

    A blob patch drops a built binary into a run of erased flash; `expect_fill` is the
    byte that must currently fill the whole target range, which is what proves we are
    writing into an unused hole and not over stock code or data.
    """
    m = json.loads(path.read_text())
    for p in m["patches"]:
        p["file_offset"] = int(str(p["file_offset"]), 0)
        if "blob" in p:
            blob_path = (ROOT / "result/stockfw" / p["blob"]).resolve()
            if not blob_path.is_file():
                raise SystemExit(f"patch {p['id']}: blob {blob_path} not found (build it first)")
            p["new"] = blob_path.read_bytes()
            fill = bytes.fromhex(p.get("expect_fill", "ff"))
            if len(fill) != 1:
                raise SystemExit(f"patch {p['id']}: expect_fill must be one byte")
            p["orig"] = fill * len(p["new"])
            p["blob_path"] = blob_path
        else:
            p["orig"] = bytes.fromhex(p["orig"])
            p["new"] = bytes.fromhex(p["new"])
        if len(p["orig"]) != len(p["new"]):
            raise SystemExit(f"patch {p['id']}: orig/new length differ - the layout must not change")
        if "va" in p:
            p["va"] = int(str(p["va"]), 0)
            if p["va"] != VA_BASE + p["file_offset"]:
                raise SystemExit(f"patch {p['id']}: va does not match file_offset")
    return m


def protected_hit(start: int, end: int):
    for lo, hi, name in PROTECTED:
        if start < hi and lo < end:
            return name
    return None


def apply_patches(stock: bytes, manifest: dict, *, reverse: bool = False) -> bytes:
    out = bytearray(stock)
    for p in manifest["patches"]:
        src, dst = (p["new"], p["orig"]) if reverse else (p["orig"], p["new"])
        off, n = p["file_offset"], len(src)
        if off + n > IMAGE_SIZE:
            raise SystemExit(f"patch {p['id']}: runs past the end of the partition")
        hit = protected_hit(off, off + n)
        if hit:
            raise SystemExit(f"patch {p['id']}: touches a protected region ({hit})")
        have = bytes(out[off:off + n])
        if have != src:
            raise SystemExit(
                f"patch {p['id']}: image has {have.hex()} at 0x{off:06x}, manifest expects {src.hex()}")
        out[off:off + n] = dst
    return bytes(out)


def diff_offsets(a: bytes, b: bytes):
    return [i for i in range(len(a)) if a[i] != b[i]]


def check_only_declared_diffs(stock: bytes, patched: bytes, manifest: dict) -> list[int]:
    declared = set()
    for p in manifest["patches"]:
        declared.update(range(p["file_offset"], p["file_offset"] + len(p["new"])))
    actual = set(diff_offsets(stock, patched))
    stray = sorted(actual - declared)
    if stray:
        raise SystemExit(f"image differs from stock at undeclared offsets: {[hex(o) for o in stray[:16]]}")
    return sorted(actual)


def read_stock(manifest: dict) -> bytes:
    img = stock_bytes()
    got = sha256(img)
    want = manifest.get("base_sha256", STOCK_SHA256)
    if got != want:
        raise SystemExit(f"stock image sha256 {got} != expected {want}")
    return img


def cmd_build(manifest: dict, out_path: pathlib.Path) -> int:
    stock = read_stock(manifest)
    patched = apply_patches(stock, manifest)
    if len(patched) != IMAGE_SIZE:
        raise SystemExit("output size changed")
    changed = check_only_declared_diffs(stock, patched, manifest)
    back = apply_patches(patched, manifest, reverse=True)
    if sha256(back) != sha256(stock):
        raise SystemExit("revert does not reproduce the stock image - refusing to write the build")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(patched)
    print(f"{manifest['name']}: wrote {out_path} ({len(patched)} bytes)")
    print(f"  sha256(stock)   = {sha256(stock)}")
    print(f"  sha256(patched) = {sha256(patched)}")
    if len(changed) <= 16:
        print(f"  changed bytes   = {len(changed)} at {[hex(o) for o in changed]}")
    else:
        runs = []
        for o in changed:
            if runs and o == runs[-1][1] + 1:
                runs[-1][1] = o
            else:
                runs.append([o, o])
        print(f"  changed bytes   = {len(changed)} in {len(runs)} run(s): "
              + ", ".join(f"0x{a:06x}-0x{b:06x}" for a, b in runs))
    print("  revert -> stock sha256: OK")
    return 0


def cmd_verify(manifest: dict, image_path: pathlib.Path) -> int:
    stock = read_stock(manifest)
    patched = image_path.read_bytes()
    if len(patched) != IMAGE_SIZE:
        raise SystemExit(f"{image_path} is {len(patched)} bytes, expected {IMAGE_SIZE}")
    changed = check_only_declared_diffs(stock, patched, manifest)
    for p in manifest["patches"]:
        off, n = p["file_offset"], len(p["new"])
        if bytes(patched[off:off + n]) != p["new"]:
            raise SystemExit(f"patch {p['id']} is not present in {image_path}")
    back = apply_patches(patched, manifest, reverse=True)
    if sha256(back) != sha256(stock):
        raise SystemExit("revert does not reproduce the stock image")
    print(f"{image_path}: {len(changed)} declared changes, revert -> stock sha256 OK")
    print(f"  sha256 = {sha256(patched)}")
    return 0


def cmd_revert(manifest: dict, image_path: pathlib.Path, out_path: pathlib.Path) -> int:
    patched = image_path.read_bytes()
    back = apply_patches(patched, manifest, reverse=True)
    got = sha256(back)
    want = manifest.get("base_sha256", STOCK_SHA256)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(back)
    print(f"wrote {out_path}, sha256 = {got} ({'matches stock' if got == want else 'DOES NOT MATCH STOCK'})")
    return 0 if got == want else 1


def cmd_info(manifest: dict) -> int:
    print(f"{manifest['name']}: {manifest.get('description','')}")
    for p in manifest["patches"]:
        va = p.get("va", VA_BASE + p["file_offset"])
        if "blob" in p:
            what = (f"{len(p['new'])} bytes from {p['blob']} "
                    f"over 0x{p['orig'][0]:02x} fill, ends at va 0x{va + len(p['new']):08x}")
        else:
            what = f"{p['orig'].hex()} -> {p['new'].hex()}"
        print(f"  [{p['id']}] file 0x{p['file_offset']:06x} / va 0x{va:08x}: {what}")
        if p.get("note"):
            print(f"        {p['note']}")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__ or "", file=sys.stderr)
        print("usage: patcher.py {build|verify|revert|info} <manifest.json> [image] [-o out]", file=sys.stderr)
        return 2
    cmd, manifest_path = argv[1], pathlib.Path(argv[2])
    rest = argv[3:]
    out = None
    if "-o" in rest:
        i = rest.index("-o")
        out = pathlib.Path(rest[i + 1])
        del rest[i:i + 2]
    manifest = load_manifest(manifest_path)
    if cmd == "info":
        return cmd_info(manifest)
    if cmd == "build":
        return cmd_build(manifest, out or ROOT / "result/stockfw/build" / f"{manifest['name']}.SYS")
    if cmd == "verify":
        return cmd_verify(manifest, pathlib.Path(rest[0]))
    if cmd == "revert":
        return cmd_revert(manifest, pathlib.Path(rest[0]),
                          out or ROOT / "result/stockfw/build" / "reverted.SYS")
    print(f"unknown command {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
