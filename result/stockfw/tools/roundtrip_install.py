# roundtrip_install.py - emulator verification of the one-file first install opengrand_install.SYS
# (ota_loader_v4 stage-0 + loader slot A seeded with ota_stage1 (confirmed) + main slot A seeded with ota_main
# (unconfirmed), tools/images.py INSTALL_IMAGES).
#
# Runs the real image patched from patches/opengrand_install.json (= build/opengrand_install.SYS) on StockEmu with the
# NOR device model built from the whole file - exactly what the USB updater leaves in MainProgram: stage-0 + the root
# key in copy #1, the two seeds in their slot sectors.  The machine is roundtrip_ota.Piano (stock NOR routines, guarded
# slot range 0x180000-0x1FFFFF: any program / erase elsewhere raises NorGuardError).  "Reboot" = a fresh emulator on the
# same image with the NOR array carried over.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_install.py [-q]
#
# Checks (numbered as printed):
#   1  image: = build/opengrand_install.SYS, = ota_loader_v4.SYS outside the two seeded sectors, revert = stock; loader
#      slot A = 'C79L' + seq 1 + state version 1 + confirmed + build/ota_stage1.c79; main slot A = 'C79S' + seq 1 + state
#      version 1 + unconfirmed + build/ota_main.c79; the other sectors erased; both verify with the local key; the
#      key-independent hash ignores the key (root key + both seeds' key id / HMAC)
#   2  first power-on: stage-0 boots loader A without a try mark (confirmed), stage-1 boots main A (one attempt),
#      Identify answered by ota_main; status layout 3 (loader A seq 1 confirmed + running, root key id = op key id)
#   3  main not confirmed one pass before the 7500-pass alive window, confirmed at it; the next boot spends no attempt
#   4  E1 (A0+A#0+B0) -> pure stock, slots untouched; E2 (A0+A#0) -> loader A in SAFE mode, no main, slots untouched
#   5  one main seed payload byte flipped -> no main (fallback); one loader seed payload byte flipped -> no loader
#      left -> stock for that boot, no NOR write at all
#   6  reject main A (op 0x14) -> reboot -> no main; Bluetooth update of the main to slot B (seq 2) -> B runs
#   7  three unconfirmed boots of the main seed (power cut before 60 s) -> the 4th boot gives main A up -> no main;
#      the confirmed loader never spends an attempt
#   8  Bluetooth update of stage-1 to loader slot B (seq 2): its state area = what the install image would seed for it
#      (the seed format IS the commit output), reboot -> loader B with one attempt, loader confirm op -> confirmed,
#      reboot -> no attempt
#   9  every NOR program / erase of every machine above lies inside the slot range, none in loader slot A (the seeded,
#      confirmed loader is never written after the install)
from __future__ import annotations

import hashlib
import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

ro = rtlib.load("roundtrip_ota")
bi = rtlib.load("build_images")
mm = rtlib.load("make_phase2_manifest")
op, pa, images, rp = ro.op, ro.pa, ro.images, ro.rp


def check(*a, **kw) -> bool:
    return bool(rtlib.check(*a, **kw))


STOCKFW = rtlib.STOCKFW
KEY = ro.KEY
QUIET = "-q" in sys.argv
NAME = "opengrand_install"
SLOT_LO, SLOT_HI = 0x180000, 0x200000      # NOR offsets of the slot sectors
LA = 0x1A0000                              # loader slot A (NOR offset)
OP_CONFIRM = 0x0F


def identify(pn):
    r = rp.body_of(pn.sysex(2, rp.req(0x01)), 2)
    return r[1] if r else None


def status3(pn):
    """ro.Piano.status + the layout-3 loader fields."""
    r = pn.reply(2, rp.req(ro.OP_STATUS), ro.OP_STATUS)
    if r is None or len(r) < 112:
        return None
    le = rtlib.le
    st = pn.status()
    st.update(lslots=[dict(seq=le(r, 72 + 6 * i), flags=r[76 + 6 * i], tries=r[77 + 6 * i]) for i in range(2)],
              lrun=r[84], s0reason=r[85], s0flags=r[86], s0ver=r[87], api=r[88], caps2=r[89], s1ver=le(r, 90),
              root_id=le(r, 94), op_id=le(r, 98), ldr_max=le(r, 104), gates=r[111])
    return st


def s0_reason(pn):
    return pn.emu.read_bytes(pn.syms["s0_boot"] + 10, 1)[0]


def main(argv):
    inst = images.INSTALL_IMAGES[NAME]
    loader_img = images.CAVE_IMAGES[inst.loader]
    seeds = {s.target: s for s in inst.seeds}
    lseed, mseed = seeds[0x10], seeds[0x00]
    p = images.OTA_PAYLOADS_BY_NAME[mseed.image]
    manifest = pa.load_manifest(STOCKFW / "patches" / f"{NAME}.json")
    stock = pa.stock_bytes()
    image = pa.apply_patches(stock, manifest)
    syms = rtlib.loader(loader_img.name).syms
    c79 = (STOCKFW / "build" / f"{mseed.image}.c79").read_bytes()
    s1 = (STOCKFW / "build" / f"{lseed.image}.c79").read_bytes()
    loader_sys = (STOCKFW / "build" / f"{loader_img.name}.SYS").read_bytes()
    ok = True
    nor_ops = []                              # (machine, op) of every NOR program / erase
    print(f"image   {NAME}  seeds {lseed.image} -> loader A (confirmed), {mseed.image} -> main A  sha256 "
          f"{pa.sha256(image)}  key id {op.key_id(KEY):08x}")

    def piano(mem=None, **kw):
        return ro.Piano(image, syms, nor_mem=mem, **kw)

    def done(pn, label):
        nor_ops.extend((label, o) for o in pn.nor.ops)
        mem = bytes(pn.nor.mem)
        pn.close()
        return mem

    want_id = bytes([p.ngver, 2, 19, p.feat0, p.feat1]) + (bytes([p.feat2]) if p.feat2 else b"") + \
        (bytes([p.feat3]) if p.feat3 else b"")

    # ---- 1. image ------------------------------------------------------------------------------------
    built = STOCKFW / "build" / f"{NAME}.SYS"
    ok &= check(f"1 build/{NAME}.SYS is this image (stock + patches/{NAME}.json), size unchanged",
                built.is_file() and built.read_bytes() == image and len(image) == pa.IMAGE_SIZE, quiet=QUIET)
    ok &= check("1 revert reproduces the stock sha256",
                pa.sha256(pa.apply_patches(image, manifest, reverse=True)) == pa.STOCK_SHA256, quiet=QUIET)
    sect = {t: images.SLOT_FILE_OFFSET[t] for t in seeds}
    diff = [i for i in range(len(image)) if image[i] != loader_sys[i]] if image != loader_sys else []
    ok &= check(f"1 = {loader_img.name}.SYS outside loader slot A and main slot A",
                diff and all(any(o <= i < o + op.SECTOR for o in sect.values()) for i in diff), f"{len(diff)} bytes differ",
                quiet=QUIET)
    la = image[sect[0x10]:sect[0x10] + op.SECTOR]
    ma = image[sect[0x00]:sect[0x00] + op.SECTOR]

    def st_ok(sa, magic, confirmed):
        return (sa[:4] == magic and rtlib.le(sa, 4) == 1 and sa[8:10] == (b"\0\0" if confirmed else b"\xff\xff")
                and sa[0xA:0xE] == b"\xff" * 4 and sa[0xE:0x10] == b"\x01\x00" and sa[0x10:0x200] == b"\xff" * 0x1F0)
    ok &= check(f"1 loader slot A = 'C79L' + seq 1 + state version 1 + CONFIRMED + build/{lseed.image}.c79 ({len(s1)} B) + "
                "erased tail", st_ok(la, b"C79L", True) and la[0x200:0x200 + len(s1)] == s1
                and la[0x200 + len(s1):] == b"\xff" * (op.SECTOR - 0x200 - len(s1)), quiet=QUIET)
    ok &= check(f"1 main slot A = 'C79S' + seq 1 + state version 1 + unconfirmed + build/{mseed.image}.c79 ({len(c79)} B) "
                "+ erased tail", st_ok(ma, b"C79S", False) and ma[0x200:0x200 + len(c79)] == c79
                and ma[0x200 + len(c79):] == b"\xff" * (op.SECTOR - 0x200 - len(c79)), quiet=QUIET)
    ok &= check("1 main slot B and loader slot B erased",
                all(image[images.SLOT_FILE_OFFSET[t]:images.SLOT_FILE_OFFSET[t] + op.SECTOR] == b"\xff" * op.SECTOR
                    for t in (0x01, 0x11)), quiet=QUIET)
    ok &= check(f"1 both seeds verify with the local key (header, key id, CRC, HMAC; the loader as format 2 type 2) and "
                f"fit ({len(s1) - 64} <= {op.WINDOW_SIZE}, {len(c79)} <= {op.IMAGE_MAX})",
                not op.verify(s1, KEY) and op.is_loader(s1) and not op.verify(c79, KEY) and not op.is_loader(c79)
                and len(s1) - 64 <= op.WINDOW_SIZE and len(c79) <= op.IMAGE_MAX, quiet=QUIET)
    other = bytes(range(32))
    alt = bytearray(image)
    alt[op.KEY_FILE_OFFSET:op.KEY_FILE_OFFSET + 32] = other
    alt_c79 = op.pack(c79[64:], other, rtlib.le(c79, 0x20))
    alt_s1 = op.pack_loader(s1[64:], other, op.parse(s1)["version"])
    alt[sect[0x00] + 0x200:sect[0x00] + 0x200 + len(c79)] = alt_c79
    alt[sect[0x10] + 0x200:sect[0x10] + 0x200 + len(s1)] = alt_s1
    ok &= check("1 key-independent hash: the same with another key (root key area FF, both seeds' key id + HMAC zeroed), "
                "and the key problems are reported for it",
                bi.key_free(bytes(alt), built.name) == bi.key_free(image, built.name) and alt_c79 != c79 and alt_s1 != s1
                and not bi.key_region_ok(image, built.name, KEY) and len(bi.key_region_ok(bytes(alt), built.name, KEY)) >= 3,
                hashlib.sha256(bi.key_free(image, built.name)).hexdigest(), quiet=QUIET)

    # ---- 2. first power-on ---------------------------------------------------------------------------
    pn = piano()
    st, nt = pn.boot()
    ok &= check("2 first power-on: stage-0 runs loader A (confirmed: no try mark), stage-1 verifies main A (CRC + HMAC) and "
                "runs it from 0x401C0000 (reason slot)",
                st["booted"] and st["active"] == ro.ACT_A and st["reason"] == ro.BR["slot"]
                and pn.emu.read_bytes(ro.RUN, 0x1000) == c79[64:64 + 0x1000]
                and ro.slot_image(pn.nor, 0)[0] == 0x43 and pn.nor.read(LA + 0x10, 2) == b"\xff\xff",
                f"active {st['active']} reason {st['reason']} after {nt} ticks")
    ok &= check("2 one main boot attempt recorded at main slot A +0x10, not confirmed",
                st["slots"][0]["tries"] == 1 and not st["slots"][0]["flags"] & 0x02
                and ro.slot_image(pn.nor, 0)[0x10:0x14] == b"\0\0\xff\xff" and ro.slot_image(pn.nor, 0)[8:10] == b"\xff\xff")
    id_ = identify(pn)
    ok &= check(f"2 Identify answered by {mseed.image}: {want_id.hex(' ').upper()}", id_ == want_id,
                id_.hex(" ") if id_ else "-")
    s3 = status3(pn)
    ok &= check("2 status layout 3: loader A seq 1 confirmed + running (flags 0x4B), loader B empty, running loader A, "
                "stage-0 reason slot, stage-0 v2, no NOR gate on (status 111 = 0), stage-1 API 1, root key id = op key "
                "id = the build key, main slot B empty, max loader file 0xA040",
                s3 is not None and s3["layout"] == 3 and s3["lslots"][0]["seq"] == 1 and s3["lslots"][0]["flags"] == 0x4B
                and s3["lslots"][0]["tries"] == 0 and s3["lslots"][1]["flags"] == 0x40 and s3["lrun"] == 0
                and s3["s0reason"] == 1 and s3["s0ver"] == 2 and s3["gates"] == 0 and s3["api"] == 1
                and s3["root_id"] == op.key_id(KEY)
                and s3["op_id"] == op.key_id(KEY) and s3["key_id"] == op.key_id(KEY) and s3["ldr_max"] == 0xA040
                and s3["s1ver"] == images.LOADER_IMAGES["s1"].version and not s3["slots"][1]["flags"] & 1,
                str({k: s3[k] for k in ("lslots", "lrun", "s0reason", "caps2")}) if s3 else "-")

    # ---- 3. confirm -----------------------------------------------------------------------------------
    pn.emu.write_u32(syms["ota_alive"], 0)
    pn.tick(ro.CONFIRM_TICKS)
    st = pn.status()
    early = not st["slots"][0]["flags"] & 0x02 and ro.slot_image(pn.nor, 0)[8:10] == b"\xff\xff"
    pn.tick(1)
    st = pn.status()
    ok &= check(f"3 main not yet confirmed after {ro.CONFIRM_TICKS} engine-loop passes (the ~60 s alive window), confirmed "
                f"(NOR +0x08 = 0000) on pass {ro.CONFIRM_TICKS + 1}",
                early and st["slots"][0]["flags"] & 0x02 != 0 and ro.slot_image(pn.nor, 0)[8:10] == b"\0\0",
                f"flags {st['slots'][0]['flags']:#x}")
    nor_conf = done(pn, "confirm")
    pn = piano(nor_conf)
    st, _ = pn.boot()
    ok &= check("3 next power-on: confirmed main A runs, no further attempt spent",
                st["active"] == ro.ACT_A and ro.slot_image(pn.nor, 0)[0x12:0x14] == b"\xff\xff" and identify(pn) == want_id)
    done(pn, "confirmed boot")

    # ---- 4. escape keys ----------------------------------------------------------------------------------
    pn = piano(mailbox=(0x55AAA55A, ro.ESC))
    nor0 = bytes(pn.nor.mem)
    st, _ = pn.boot()
    ok &= check("4 E1: A0+A#0+B0 held -> pure stock (nothing answers 0x7D, stage-0 reason escape), slots untouched",
                st is None and s0_reason(pn) == 3 and identify(pn) is None and bytes(pn.nor.mem) == nor0)
    done(pn, "escape E1")
    pn = piano(mailbox=(0x55AAA55A, 0x40000003))
    st, _ = pn.boot()
    id_ = identify(pn)
    s3 = status3(pn)
    ok &= check("4 E2: A0+A#0 held -> loader A (the only one) in SAFE mode: stage-1 answers, no main (reason 7), Identify = "
                "the minimal responder, slots untouched",
                st is not None and st["active"] == ro.ACT_BUILTIN and st["reason"] == 7 and id_ == ro.NO_MAIN_ID
                and s3["s0reason"] == 6 and s3["s0flags"] & 1 != 0 and bytes(pn.nor.mem) == nor0, f"{st}")
    done(pn, "escape E2")

    # ---- 5. corrupted seeds ---------------------------------------------------------------------------------
    pn0 = piano()
    mem0 = bytes(pn0.nor.mem)
    pn0.close()
    mem = bytearray(mem0)
    mem[SLOT_LO + 0x200 + 64 + 1000] ^= 0x01
    pn = piano(bytes(mem))
    st, _ = pn.boot()
    ok &= check("5 one main seed payload byte flipped in NOR -> main A fails verification (flag bad) -> no main "
                "(reason fallback); loader unaffected", st["active"] == ro.ACT_BUILTIN and st["reason"] == ro.BR["fallback"]
                and st["slots"][0]["flags"] & 0x10 != 0, f"active {st['active']} reason {st['reason']}")
    done(pn, "corrupt main")
    mem = bytearray(mem0)
    mem[LA + 0x200 + 64 + 500] ^= 0x01
    pn = piano(bytes(mem))
    st, _ = pn.boot()
    ok &= check("5 one loader seed payload byte flipped -> its CRC fails, no other loader -> stock for this boot (stage-0 "
                "reason failed), no NOR write at all (confirmed: no try mark)",
                st is None and s0_reason(pn) == 4 and not pn.nor.ops)
    done(pn, "corrupt loader")

    # ---- 6. reject + Bluetooth update of the main ---------------------------------------------------------------
    pn = piano()
    pn.boot()
    rc, _ = pn.auth_op(ro.OP_REJECT, bytes([1]))
    pn.tick(2)
    st = pn.status()
    rej_ok = rc == 0 and st["slots"][0]["flags"] & 0x04 and ro.slot_image(pn.nor, 0)[0xA:0xC] == b"\0\0"
    mem = done(pn, "reject")
    pn = piano(mem)
    st, _ = pn.boot()
    ok &= check("6 reject main A (op 0x14, authenticated) -> rejected flag programmed -> reboot -> no main (no valid slot)",
                rej_ok and st["active"] == ro.ACT_BUILTIN and st["reason"] == ro.BR["empty"],
                f"rc {rc} active {st['active']} reason {st['reason']}")
    done(pn, "rejected boot")
    pn = piano()
    pn.boot()
    st = pn.status()
    seq = max(s["seq"] for s in st["slots"] if s["flags"] & 1) + 1
    res = pn.upload(1, c79, seq=seq)
    st, _ = pn.run_job()
    sb_ = ro.slot_image(pn.nor, 1)
    up_ok = seq == 2 and res == ("commit", 0) and st["job"] == ro.J["done"] and sb_[:0x200] == mm.seed_blob(
        images.Seed(0x01, "-", 2, False), b"") and sb_[0x200:0x200 + len(c79)] == c79
    mem = done(pn, "upload main B")
    pn = piano(mem)
    st, _ = pn.boot()
    ok &= check(f"6 upload {mseed.image} to main slot B (the non-active slot) with seq {seq} = max + 1: committed (state "
                "area = the seed format), reboot -> main B runs, main A kept as the fallback",
                up_ok and st["active"] == ro.ACT_B and st["reason"] == ro.BR["slot"] and identify(pn) == want_id
                and ro.slot_image(pn.nor, 0)[:4] == b"C79S", f"{res} active {st['active']}")
    done(pn, "boot main B")

    # ---- 7. three unconfirmed main boots ---------------------------------------------------------------------------
    mem, seen, tries = None, [], []
    for k in range(4):
        pn = piano(mem)
        st, _ = pn.boot()
        seen.append(st["active"])
        tries.append(st["slots"][0]["tries"])
        mem = done(pn, f"short boot {k + 1}")
    ok &= check("7 main seed never confirmed (power cut before 60 s): boots 1-3 run main A (attempts 1, 2, 3), the 4th "
                "gives it up -> no main; the confirmed loader spent no attempt",
                seen == [ro.ACT_A] * 3 + [ro.ACT_BUILTIN] and tries[:3] == [1, 2, 3]
                and mem[LA + 0x10:LA + 0x30] == b"\xff" * 0x20, f"active {seen} tries {tries}")

    # ---- 8. Bluetooth update of stage-1 ------------------------------------------------------------------------------
    pn = piano()
    pn.boot()
    res = pn.upload(0x11, s1, seq=2)
    st, _ = pn.run_job()
    lb = pn.nor.read(0x1E0000, op.SECTOR)
    ok &= check(f"8 upload {lseed.image} to loader slot B (target 0x11, seq 2): committed; NOR = 'C79L' + seq 2 + state "
                "version 1, unconfirmed = exactly the state area the install image would seed + the file",
                res == ("commit", 0) and st["job"] == ro.J["done"]
                and lb[:0x200 + len(s1)] == mm.seed_blob(images.Seed(0x11, "-", 2, False), s1), f"{res} job {st['job']}")
    mem = done(pn, "upload loader B")
    pn = piano(mem)
    st, _ = pn.boot()
    s3 = status3(pn)
    ok &= check("8 reboot -> stage-0 runs loader B (seq 2) with one attempt recorded first (+0x10 = 0000), main A as before",
                s3["lrun"] == 1 and s3["lslots"][1]["tries"] == 1 and s3["lslots"][1]["flags"] & 0x0A == 0x08
                and pn.nor.read(0x1E0000 + 0x10, 4) == b"\0\0\xff\xff" and st["active"] == ro.ACT_A, str(s3["lslots"]))
    rc, _ = pn.auth_op(OP_CONFIRM, bytes([0x11]))
    pn.tick(2)
    s3 = status3(pn)
    ok &= check("8 loader confirm op 0x0F (target 0x11, authenticated) -> loader B +0x08 = 0000, flag confirmed",
                rc == 0 and s3["lslots"][1]["flags"] & 0x02 != 0 and pn.nor.read(0x1E0000 + 8, 2) == b"\0\0", f"rc {rc}")
    mem = done(pn, "confirm loader B")
    pn = piano(mem)
    pn.boot()
    s3 = status3(pn)
    ok &= check("8 next power-on: confirmed loader B runs, no further attempt", s3["lrun"] == 1
                and pn.nor.read(0x1E0000 + 0x12, 2) == b"\xff\xff" and s3["lslots"][1]["tries"] == 1)
    done(pn, "boot loader B")

    # ---- 9. NOR writes ---------------------------------------------------------------------------------------------
    stray = [(lbl, o.kind, hex(o.offset)) for lbl, o in nor_ops
             if not (SLOT_LO <= o.offset and o.offset + o.size <= SLOT_HI) or LA <= o.offset < LA + op.SECTOR]
    ok &= check(f"9 every NOR program / erase lies inside the slot range 0x{SLOT_LO:06X}-0x{SLOT_HI - 1:06X} and none in "
                "loader slot A", not stray and bool(nor_ops), f"{len(nor_ops)} ops" if not stray else str(stray[:3]))

    return rtlib.finish(ok, " - emulation only: no interrupts, no RTOS scheduling, NOR timing is poll counts.")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
