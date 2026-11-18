# build_images.py - build every image from source, or check the built images against ARTIFACTS.md.
#
#   cd result/stockfw
#   uv run python -I tools/build_images.py           # build: toolchain check -> cave -> manifest -> patch
#   uv run python -I tools/build_images.py --check   # every image in tools/images.py has an ARTIFACTS row,
#                                                    # build/<file> hashes to it (key-independent hash, below),
#                                                    # reverts to stock, and a payload verifies with the key
#   uv run python -I tools/build_images.py baseline  # build only the named image(s)
#
# Repo root: `make image` (build) and `make image-check` (build, then check; part of `make check`).
# Inputs: target/CAx9_P06.SYS (stock, read only), cave/src.
# Outputs: cave/build/cave_s0.{bin,elf}, cave/build/loader_s1.{bin,elf}, patches/<name>.json, build/<name>.SYS,
# build/<name>.c79, build/<install>.seed_<tag>.bin.
# The images are opengrand_install (.SYS, the one-file first install), ota_stage1 (.c79, the stage-1 loader image),
# ota_main (.c79, Bluetooth updates) and ota_loader_v4 (.SYS, stage-0 alone: the build step opengrand_install is made
# from); the round trips' test payloads (images.TEST_PAYLOADS) are built and signed the same way into build/
# (git-ignored), with no ARTIFACTS row and no hash check.  Build order: stage-0 image -> stage-1 images -> slot payloads
# -> install images (the stage-0 image's patches + the seeded slots, images.INSTALL_IMAGES).  Every stage-1 and slot
# payload ELF is audited: no branch to / literal of a stock NOR writer (writer_refs) - only stage-0 writes NOR.
# Without target/CAx9_P06.SYS (it is not in the repository) both modes print SKIP and exit 0; a file there that is
# not the stock V2.19 image fails.
#
# The OTA key (secrets/ota_key.bin, created on first use, never committed) is patched into the .SYS images
# and signs every .c79, so those files differ per key. ARTIFACTS.md records a KEY-INDEPENDENT sha256 instead:
#   .SYS  the image with every key blob patch (manifest blob secrets/ota_key.bin) put back to its erased fill (0xFF),
#         and in every seeded slot (manifest "keyed": "c79@<offset in the blob>") the embedded .c79's key id and HMAC
#         zeroed as for a .c79
#   .c79  the file with the key id (header 0x24..0x27) and the HMAC (header 0x30..0x3F) zeroed
# For an image without the key that is simply the file's sha256. --check also verifies that the key region / the
# signature matches the local key, and that a seeded slot holds exactly build/<payload>.c79 and verifies with it.
# The seed blob build/<install>.seed.bin carries a signed .c79 (key-dependent): it exists only under the git-ignored
# build/.
# Nothing here writes to a device.  Flashing is always done by the user, after approval.
from __future__ import annotations

import importlib.util
import json
import pathlib
import re
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = pathlib.Path(__file__).resolve().parent
STOCKFW = HERE.parent
ARTIFACTS = STOCKFW / "ARTIFACTS.md"


def _load(name):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


images = _load("images")
pa = _load("patcher")
mm = _load("make_phase2_manifest")
op = _load("ota_pack")


def artifact_rows() -> dict[str, tuple[str, str]]:
    """ARTIFACTS.md 「만든 이미지」 table: name -> (file under build/, sha256).
    Rows without a file are skipped."""
    rows, in_built = {}, False
    for line in ARTIFACTS.read_text().splitlines():
        if line.startswith("## "):
            in_built = "result/stockfw/build/" in line
            continue
        if not in_built or not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 3:
            continue
        f = re.match(r"`([^`]+\.(?:SYS|c79))`", cells[1])
        h = re.fullmatch(r"`([0-9a-f]{64})`", cells[2])
        if f and h:
            rows[cells[0]] = (f[1], h[1])
    return rows


def all_images() -> list[tuple[str, str]]:
    """(artifact name, manifest name) of every .SYS image in build order: loader images, then install images."""
    return [(i.artifact, i.name) for i in images.CAVE_IMAGES.values()] + \
        [(i.artifact, i.name) for i in images.INSTALL_IMAGES.values()]


KEY_BLOB = "secrets/ota_key.bin"


def _c79_key_free(b: bytearray, at: int) -> None:
    """Zero the key id and the HMAC of the .c79 that starts at b[at] (the .c79 rule of the header)."""
    b[at + 0x24:at + 0x28] = bytes(4)
    b[at + 0x30:at + 0x40] = bytes(16)


def seeded_c79_at(p: dict) -> int | None:
    """File offset of the signed .c79 inside a seed blob patch ("keyed": "c79@<offset in the blob>"), else None."""
    k = p.get("keyed", "")
    return int(p["file_offset"], 0) + int(k[4:], 0) if k.startswith("c79@") else None


def key_free(data: bytes, filename: str) -> bytes:
    """`data` with the bytes that depend on the OTA key neutralised (see the header): the key-independent form
    whose sha256 ARTIFACTS.md records."""
    b = bytearray(data)
    if filename.endswith(".c79"):
        _c79_key_free(b, 0)
        return bytes(b)
    manifest = json.loads((STOCKFW / "patches" / f"{pathlib.Path(filename).stem}.json").read_text())
    for p in manifest["patches"]:
        if p.get("blob") == KEY_BLOB:
            off = int(p["file_offset"], 0)
            b[off:off + 32] = bytes.fromhex(p.get("expect_fill", "ff")) * 32
        at = seeded_c79_at(p)
        if at is not None:
            _c79_key_free(b, at)
    return bytes(b)


def key_region_ok(data: bytes, filename: str, key: bytes) -> list[str]:
    """Problems with the key-dependent bytes of a .SYS (empty = fine; always empty for an image without the key):
    every key blob patch holds the local key; every seeded slot holds its install image's state area and after it
    exactly build/<payload>.c79, which verifies (header, key id, CRC, HMAC) with the local key."""
    stem = pathlib.Path(filename).stem
    manifest = json.loads((STOCKFW / "patches" / f"{stem}.json").read_text())
    bad = []
    for p in manifest["patches"]:
        off = int(p["file_offset"], 0)
        if p.get("blob") == KEY_BLOB and data[off:off + 32] != key:
            bad.append("key region != local key")
        at = seeded_c79_at(p)
        if at is None:
            continue
        inst = images.INSTALL_IMAGES[stem]
        seed = next(x for x in inst.seeds if p["id"] == f"seed_{x.tag}")
        ref = STOCKFW / "build" / f"{seed.image}.c79"
        if not ref.is_file():
            bad.append(f"build/{ref.name} missing")
            continue
        want = ref.read_bytes()
        state = mm.seed_blob(seed, b"")
        if data[off:off + len(state)] != state:
            bad.append(f"seed {seed.tag} state area")
        got = data[at:at + len(want)]
        if got != want:
            bad.append(f"seed {seed.tag} != build/{ref.name}")
        why = op.verify(got, key)
        if why:
            bad.append(f"seed {seed.tag} does not verify with the local key ({', '.join(why)})")
    return bad


# The stock NOR writers and the busy flag setter (cave/stock_norw.ld): only stage-0 may reach them.
NOR_WRITERS = {0x40133C38: "stk_nor_erase", 0x40133D50: "stk_nor_prog_buf", 0x401339EC: "stk_nor_prog_hw",
               0x4013403C: "stk_flash_busy_set"}


def writer_refs(elf: pathlib.Path) -> list[str]:
    """Every branch to, or literal word holding (ARM or Thumb), a stock NOR writer in an ELF's code + data:
    the build check that stage-1 and every slot payload write NOR only through stage-0's service (they do not link
    stock_norw.ld; this also catches a hard-coded address)."""
    od = mm.NM.with_name("arm-none-eabi-objdump")
    out = subprocess.run([str(od), "-D", "-j", ".cave", str(elf)], capture_output=True, text=True, check=True).stdout
    hits = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+([0-9a-f]{8}|[0-9a-f]{4}(?: [0-9a-f]{4})?)\s+(\S+)\s*(.*)", line)
        if not m:
            continue
        words = [int(m[2].replace(" ", ""), 16)] if len(m[2]) == 8 else []
        targets = [int(t, 16) for t in re.findall(r"\b([0-9a-f]{8})\b", m[4])] if m[3].startswith("b") else []
        for v in words + targets:
            if v in NOR_WRITERS or (v & ~1) in NOR_WRITERS:
                hits.append(f"0x{int(m[1], 16):08x} {m[3]} {NOR_WRITERS.get(v & ~1, NOR_WRITERS.get(v))}")
    return hits


def audit_writers(elf: pathlib.Path) -> None:
    hits = writer_refs(elf)
    if hits:
        raise SystemExit(f"{elf.name}: references a stock NOR writer (only stage-0 may): {'; '.join(hits[:4])}")


def build_loaders(only: list[str]) -> None:
    """ota_loader_v4 stage-1 images: link into the stage-1 window, audit, sign into build/<name>.c79 (format 2)."""
    key = op.ensure_key()
    for li in images.LOADER_IMAGES.values():
        if only and li.artifact not in only and li.name not in only:
            continue
        subprocess.run(["make", "-s", "-C", str(STOCKFW / "cave"), "STAGE=1", f"V={li.variant}"], check=True)
        audit_writers(STOCKFW / "cave" / "build" / f"loader_{li.variant}.elf")
        data = op.pack_loader((STOCKFW / "cave" / "build" / f"loader_{li.variant}.bin").read_bytes(), key, li.version)
        out = STOCKFW / "build" / f"{li.name}.c79"
        out.parent.mkdir(parents=True, exist_ok=True)
        same = out.is_file() and out.read_bytes() == data
        if not same:
            out.write_bytes(data)
        print(f"  {li.artifact:<17} build/{out.name:<24} {pa.sha256(key_free(data, out.name))}"
              f"{'' if same else '  (written)'}")


def build_payloads(only: list[str]) -> None:
    """Slot payloads (the image ota_main and the round trips' test payloads): link at OTA_RUN_BASE, then sign into
    build/<name>.c79."""
    key = op.ensure_key()
    for p in (*images.OTA_PAYLOADS.values(), *images.TEST_PAYLOADS.values()):
        if only and p.artifact not in only and p.name not in only:
            continue
        subprocess.run(["make", "-s", "-C", str(STOCKFW / "cave"), "PAYLOAD=1", f"V={p.variant}",
                        f"FEAT0=0x{p.feat0:02X}", f"FEAT1=0x{p.feat1:02X}"]
                       + ([f"FEAT2=0x{p.feat2:02X}"] if p.feat2 else [])
                       + ([f"FEAT3=0x{p.feat3:02X}"] if p.feat3 else []), check=True)
        audit_writers(STOCKFW / "cave" / "build" / f"payload_{p.variant}.elf")
        payload = (STOCKFW / "cave" / "build" / f"payload_{p.variant}.bin").read_bytes()
        data = op.pack(payload, key, p.feat0 | p.feat1 << 8)
        out = STOCKFW / "build" / f"{p.name}.c79"
        out.parent.mkdir(parents=True, exist_ok=True)
        same = out.is_file() and out.read_bytes() == data
        if not same:
            out.write_bytes(data)
        print(f"  {p.artifact:<17} build/{out.name:<24} {pa.sha256(key_free(data, out.name))}"
              f"{'' if same else '  (written)'}")


def build_install(inst, key: bytes) -> None:
    """build/<install>.seed_<tag>.bin from build/<image>.c79 for every seed, then patches/<install>.json (the loader's
    manifest + one seed blob patch per slot)."""
    for seed in inst.seeds:
        c79 = (STOCKFW / "build" / f"{seed.image}.c79").read_bytes()
        why = op.verify(c79, key)
        if why:
            raise SystemExit(f"{inst.name}: build/{seed.image}.c79 does not verify with the local key ({', '.join(why)})")
        if op.is_loader(c79) != seed.loader:
            raise SystemExit(f"{inst.name}: build/{seed.image}.c79 is not a {'loader' if seed.loader else 'main'} image")
        blob_data = mm.seed_blob(seed, c79)
        if len(blob_data) > op.SECTOR:
            raise SystemExit(f"{inst.name}: seed {seed.tag} {len(blob_data)} B does not fit one slot sector")
        blob = STOCKFW / mm.seed_path(inst.name, seed)
        if not blob.is_file() or blob.read_bytes() != blob_data:
            blob.write_bytes(blob_data)
    mm.write_install_manifest(inst.name, quiet=True)


def write_image(artifact: str, name: str) -> None:
    """patches/<name>.json -> build/<name>.SYS (declared diffs only, revert = stock)."""
    manifest = pa.load_manifest(STOCKFW / "patches" / f"{name}.json")
    stock = pa.read_stock(manifest)
    patched = pa.apply_patches(stock, manifest)
    pa.check_only_declared_diffs(stock, patched, manifest)
    if pa.sha256(pa.apply_patches(patched, manifest, reverse=True)) != pa.sha256(stock):
        raise SystemExit(f"{name}: revert does not reproduce the stock image")
    out = STOCKFW / "build" / f"{name}.SYS"
    out.parent.mkdir(parents=True, exist_ok=True)
    # leave an identical file alone: it may be on its way to a USB stick right now
    same = out.is_file() and out.read_bytes() == patched
    if not same:
        out.write_bytes(patched)
    print(f"  {artifact:<17} build/{out.name:<24} {pa.sha256(key_free(patched, out.name))}"
          f"{'' if same else '  (written)'}")


def build(only: list[str]) -> int:
    if not pa.stock_present():
        pa.skip_no_stock("make image")
    pa.stock_bytes()   # present: must be the stock image (fails otherwise) before anything is built
    subprocess.run(["make", "-s", "-C", str(STOCKFW / "cave"), "toolchain-check"], check=True)
    key = op.ensure_key()   # ota_loader patches it in; created on first use (git-ignored secrets/)
    only = list(only)
    for inst in images.INSTALL_IMAGES.values():   # an install image needs its loader image and its seeds first
        if only and (inst.artifact in only or inst.name in only):
            only += [images.CAVE_IMAGES[inst.loader].name] + [x.image for x in inst.seeds]

    def wanted(artifact: str, name: str) -> bool:
        return not only or artifact in only or name in only

    for cave in images.CAVE_IMAGES.values():
        if not wanted(cave.artifact, cave.name):
            continue
        subprocess.run(["make", "-s", "-C", str(STOCKFW / "cave"), f"V={cave.variant}"]
                       + (["STAGE=0"] if cave.stage0 else [f"FEAT0=0x{cave.feat0:02X}", f"FEAT1=0x{cave.feat1:02X}"]),
                       check=True)
        mm.write_manifest(cave.variant, quiet=True)
        write_image(cave.artifact, cave.name)
    build_loaders(only)
    build_payloads(only)
    for inst in images.INSTALL_IMAGES.values():
        if not wanted(inst.artifact, inst.name):
            continue
        build_install(inst, key)
        write_image(inst.artifact, inst.name)
    return 0


def check() -> int:
    if not pa.stock_present():
        pa.skip_no_stock("make image-check")
    rows = artifact_rows()
    key = op.ensure_key(quiet=True)
    ok = True
    for artifact, name in all_images():
        path = STOCKFW / "build" / f"{name}.SYS"
        row = rows.get(artifact)
        if row is None:
            print(f"  [FAIL] {artifact}: no row in ARTIFACTS.md")
            ok = False
            continue
        if row[0] != path.name:
            print(f"  [FAIL] {artifact}: ARTIFACTS names {row[0]}, the image table builds {path.name}")
            ok = False
            continue
        if not path.is_file():
            print(f"  [FAIL] {artifact}: {path} missing (make image)")
            ok = False
            continue
        data = path.read_bytes()
        got = pa.sha256(key_free(data, path.name))
        manifest = pa.load_manifest(STOCKFW / "patches" / f"{name}.json")
        stock = pa.read_stock(manifest)
        reverts = pa.sha256(pa.apply_patches(data, manifest, reverse=True)) == pa.sha256(stock)
        keyed = key_region_ok(data, path.name, key)
        good = got == row[1] and len(data) == pa.IMAGE_SIZE and reverts and not keyed
        ok &= good
        print(f"  [{'PASS' if good else 'FAIL'}] {artifact:<17} {got}"
              + ("" if got == row[1] else f" != ARTIFACTS {row[1]}")
              + ("" if reverts else " (revert != stock)") + (f" ({'; '.join(keyed)})" if keyed else ""))
    for p in (*images.LOADER_IMAGES.values(), *images.OTA_PAYLOADS.values()):
        path = STOCKFW / "build" / f"{p.name}.c79"
        row = rows.get(p.artifact)
        if row is None or row[0] != path.name or not path.is_file():
            print(f"  [FAIL] {p.artifact}: ARTIFACTS row / {path.name} missing or misnamed")
            ok = False
            continue
        data = path.read_bytes()
        got = pa.sha256(key_free(data, path.name))
        bad = op.verify(data, key)
        good = got == row[1] and not bad
        ok &= good
        print(f"  [{'PASS' if good else 'FAIL'}] {p.artifact:<17} {got}"
              + ("" if got == row[1] else f" != ARTIFACTS {row[1]}") + (f" (bad: {', '.join(bad)})" if bad else ""))
    print("ALL IMAGES MATCH ARTIFACTS" if ok else "IMAGE CHECK FAILED")
    return 0 if ok else 1


def main(argv: list[str]) -> int:
    args = argv[1:]
    if "--check" in args:
        return check()
    return build(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
