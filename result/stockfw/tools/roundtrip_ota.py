# roundtrip_ota.py - emulator verification of the remote cave update (image ota_loader_v4 + slot payloads).
#
# Runs the real ota_loader_v4 image (stock loader -> stage-0 + root key in SDRAM; loader slot A seeded with
# ota_stage1, confirmed, as opengrand_install seeds it - rtlib.loader) on StockEmu with the NOR device model
# attached (allowed = the slot sectors 0x180000-0x1FFFFF only; any program/erase elsewhere raises NorGuardError).
# The stock NOR routines run unmodified (only stage-0 calls them); dis_dsp/ena_dsp are replaced by hooks that
# set/clear the NORTi dispatch-disabled byte (so the stock dly_tsk takes its E_CTX path exactly as on hardware), the
# stock engine-loop tail and TX writers are stubbed.  "Reboot" = a fresh emulator on the same image with the NOR array
# carried over.  The loader's own boot choice / rollback / power loss / RESELECT: roundtrip_v4_boot.py.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_ota.py [ota_loader_v4] [-q]
#
# Payloads: the test payloads ota_core_v2 / ota_base_v2 (tools/images.py TEST_PAYLOADS); apply now, the key-state
# escape and the boot diagnostics are roundtrip_ota_apply.py.
#
# Checks (numbered as printed):
#   1  image: declared diffs only, revert = stock, build file; key patched but not in any tracked blob;
#      payload files verify; static audit of stage-0 (stock calls, NOR writers only from its three guarded service
#      functions, NOR literals only in the allowed ranges, indirect calls only to stage-1) and stage-1 (stock calls,
#      no NOR writer, NOR literals only in the slot range, indirect calls only in its dispatch / the service)
#   2  first boot, empty main slots -> stage-1 in "no main" mode; its minimal Identify; the real hook path
#   3  full upload of ota_core_v2 to slot A over the 0x7D ops (stop-and-wait, 200 B chunks) -> commit ->
#      NOR holds the file, state area sealed; every NOR op in slot A, with dispatch disabled
#   4  reboot -> slot A verified and running from 0x401C0000, attempt recorded; identify and core
#      self-test answered by the slot code, its time-independent bytes = the host build's (cave_selftest);
#      confirm after the alive window
#   5  ota_base_v2 to slot B -> reboot -> B (higher seq) runs (identify feat1 0x05)
#   6  rollback (reject B) -> A; reject A too -> no main
#   7  corrupted CRC in the NOR copy -> that slot fails verification -> other slot / no main
#   8  power cut during erase / program / seal (op m of the commit) -> the old slot boots
#   9  bad MAC / bad CRC / wrong key id upload -> commit fails, flash never touched
#  10  auth: wrong tag, replayed tag, wrong slot / seq / offset rejected
#  11  out-of-range erase/program -> refused by stage-0's guard before the stock routine; NOR trap silent
#  12  stock flash busy -> erase/program deferred, then completes; boot waits the same way
#  13  escape keys (A0+A#0+B0 mailbox code / MainProgram copy) -> pure stock (no answer); 3 unconfirmed boots -> skipped
#  14  stores: only stage-0 .data, the stage-1 window, R1 OTA areas, the stack and the NORTi error word 0x4139E778;
#      stock RAM otherwise unchanged; measured stack
from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rp = rtlib.load("roundtrip_probe")
sb = rtlib.load("stack_budget")
op = rtlib.load("ota_pack")
rt, images = rp.rt, rp.images
se, pa = rt.se, rt.pa
check = rt.check
STOCKFW = rtlib.STOCKFW

DIS, ENA, DLY = 0x40089BA0, 0x40088DF8, 0x40089658
DSP_BYTE = 0x4139E79B
BUSY_FLAG = 0x4139DAC4
KERNEL_ERR = 0x4139E778
NOR_ERASE, NOR_BUF, NOR_HW = 0x40133C38, 0x40133D50, 0x401339EC
MBOX = 0x8001F7F8
ESC = 0x40000007
SLOT = {0: 0x60180000, 1: 0x601C0000}
STAGE, RUN, VBUF = 0x401A0000, 0x401C0000, 0x401E0000
CORE_RW = 0x40180000
STACK = (0x40F00000, 0x41000000)
OP_STATUS, OP_BEGIN, OP_DATA, OP_COMMIT, OP_REJECT = 0x10, 0x11, 0x12, 0x13, 0x14
J = dict(idle=0, recv=1, commit_q=2, verify=3, erase=4, prog=5, readback=6, seal=7, done=8, error=9)
ACT_A, ACT_B, ACT_BUILTIN = 0, 1, 2
BR = dict(selecting=0, slot=1, empty=2, escape=3, fallback=4, busy=5)
CONFIRM_TICKS = 7500
KEY = op.ensure_key(quiet=True)
VERBOSE = "-q" not in sys.argv
SVC_RET = 0x400A88BC                  # *0x4009D000: the stock SWI #0 return stub
KEY_SCAN_ON = 0x413CAF38              # stock: 1 once TaskInitial enabled the key scan
# loader image -> its slot payloads (core, base)
PAYLOADS = {"ota_loader_v4": ("ota_core_v2", "ota_base_v2")}
WIN = (0x40176000, 0x40180000)        # the stage-1 window
NO_MAIN_ID = bytes([1, 2, 19, 0x01, 0x04])   # stage-1's minimal Identify (no main)
S0_ALLOWED = {0x400AA030: "stk_flash_copy", 0x4013427C: "stk_flash_busy", 0x40089BA0: "dis_dsp", 0x40088DF8: "ena_dsp",
              0x401342AC: "stk_flash_jobs", 0x4013403C: "stk_flash_busy_set", 0x40133C38: "stk_nor_erase",
              0x40133D50: "stk_nor_prog_buf", 0x401339EC: "stk_nor_prog_hw", 0x40147E98: "stk_end_handler_done",
              0x40142C90: "stk_engine_tail"}
S1_ALLOWED = {0x4014492C: "stk_tx_port0", 0x401445AC: "stk_tx_port1", 0x40145284: "stk_tx_port2", 0x40144CD0: "stk_tx_port3"}


def reg_indirect(indirect):
    """roundtrip_probe.static_calls' indirect list without `blx <address>` (a direct ARM <-> Thumb call)."""
    return [(a, t) for a, t in indirect if not re.match(r"blx\w* [0-9a-f]+\b", t)]


pack7, le = rtlib.pack7, rtlib.le


class PowerCut(Exception):
    pass


class Piano:
    """One power-on of the ota_loader image."""

    def __init__(self, image, syms, nor_mem=None, mailbox=None, sdram_code=None, nor_allowed=None, nor_kw=None):
        self.syms = syms
        # R1 above the cave is our RAM (core RW, staging, run region): not a guard range here;
        # the other never-loaded span (DSP image tail, 0x403C0000-0x40400000) stays guarded
        self.emu = emu = se.StockEmu(image=image, guard=[(0x403C0000, 0x40400000)])
        # nor_allowed: NOR offsets the model lets the code program / erase (default: the update slots;
        # roundtrip_power.py adds unused_3 for the power port's policy store); nor_kw: more NorFlash arguments
        # (roundtrip_v4_boot.py: protected=, chip_size= for the stage-0 NOR gates)
        self.nor = nor = emu.attach_nor(latency={"erase": 20, "buffer": 4, "program": 2},
                                        **({"allowed": nor_allowed} if nor_allowed else {}), **(nor_kw or {}))
        if nor_mem is not None:
            nor.mem[:] = nor_mem
        if mailbox is not None:
            emu.periph[MBOX], emu.periph[MBOX + 4] = mailbox
        if sdram_code is not None:
            emu.write_u32(0x40000000, sdram_code)
        self.sent = []
        self.writes = se.WriteLog()   # every CPU store of every call (stockemu.WriteLog, list-like)
        self.nor_calls = []         # (routine, lr, dsp byte, r0)
        self.dsp_depth = 0
        self.nested = False
        self.svc_calls = []         # (r4 addr, r5 len, lr of the wrapper) per SWI #0 (the loader's cache sync)
        self.svc_ret = None
        # on the piano the stock key scan runs before the engine-loop task exists (TaskInitial
        # 0x400AB170 -> 0x400A9DC8); the emulator never ran TaskInitial, so raise the flag here
        emu.write_bytes(KEY_SCAN_ON, b"\x01")
        if "ota_cache_sync" in syms:
            self.hook_svc()

        def dis(e):
            self.dsp_depth += 1
            self.nested |= self.dsp_depth > 1
            e.write_bytes(DSP_BYTE, b"\x01")

        def ena(e):
            self.dsp_depth -= 1
            e.write_bytes(DSP_BYTE, b"\x00")

        emu.hook(DIS, dis)
        emu.hook(ENA, ena)
        emu.hook(rt.ENGINE_TAIL, lambda e: None)
        self.hook_tx()
        for name, addr in (("erase", NOR_ERASE), ("buf", NOR_BUF), ("hw", NOR_HW)):
            emu.hook(addr, (lambda nm: lambda e: (self.nor_calls.append(   # self.nor_calls looked up per call
                (nm, e.reg(14), e.read_bytes(DSP_BYTE, 1)[0], e.reg(0))), se.StockEmu.CONTINUE)[1])(name))

    def hook_svc(self):
        """Emulate the stock SWI #0 dispatcher (0x400A8884: SWI 0 -> bx r0 in SVC mode) for the
        resident's privileged wrappers (cache sync after installing a payload, and the
        runtime-hook word write ota_hook_poke): run the real privileged body (Unicorn executes the
        CP15 c7 operations), whose return through *0x4009D000 lands on the stock return stub, which we
        turn back into the instruction after the svc.  svc_calls / poke_calls record (r4, r5, lr)."""
        self.poke_calls = []

        def on_svc(lst, svc):
            def f(e):   # by name: rtlib.memo restores the lists as new objects
                getattr(self, lst).append((e.reg(4), e.reg(5), e.reg(14)))
                self.svc_ret = svc + 4
                e.jump(e.reg(0))
            return f

        for name, lst in (("ota_cache_sync", "svc_calls"), ("ota_hook_poke", "poke_calls")):
            if name not in self.syms:
                continue
            svc = self.syms[name] + 0x10
            # (ota_loader_v4: the stage-1 window 0x40176000+ is loaded by stage-0 later - checked when loaded)
            if not 0x40176000 <= svc < 0x40180000 and self.emu.read_u32(svc) != 0xEF000000:
                raise RuntimeError(f"{name}: no `svc #0` where expected")
            self.emu.hook(svc, on_svc(lst, svc))

        def on_ret(e):
            if self.svc_ret is None:
                raise RuntimeError("SWI return stub reached without a SWI")
            e.jump(self.svc_ret)
            self.svc_ret = None

        self.emu.hook(SVC_RET, on_ret)

    def hook_tx(self):
        for p, addr in rt.TX.items():
            self.emu.hook(addr, (lambda pp: lambda e: self.sent.append((pp, e.read_bytes(e.reg(0), e.reg(1)))))(p))

    def close(self):
        self.emu.close()

    def call(self, addr, *args, **kw):
        res = self.emu.call(addr, *args, max_insns=60_000_000, **kw)
        self.writes.extend(res.writes)
        if not res.ok:
            raise RuntimeError(f"call {addr:#x} stopped: {res}")
        return res

    def tick(self, n=1):
        for _ in range(n):
            self.call(self.syms["cave_tick"])

    def sysex(self, port, body):
        """Hand a complete 0x7D message to the resident's ng_sysex (the hook path is checked separately)."""
        frame = body + b"\xF7"
        self.emu.write_bytes(0x40B90000, frame)
        self.sent.clear()
        self.call(self.syms["ng_sysex"], port, 0x40B90000, len(frame))
        out = list(self.sent)
        self.sent.clear()
        return out

    def reply(self, port, body, opc):
        r = rp.body_of(self.sysex(port, body), port)
        return rp.unpack7(r[1]) if r and r[0] == (opc | 0x40) else None

    def status(self, port=2):
        r = self.reply(port, rp.req(OP_STATUS), OP_STATUS)
        if r is None or len(r) < 48:
            return None
        return dict(job=r[1], err=r[2], booted=r[3], active=r[4], reason=r[5], job_slot=r[6], rej=r[7],
                    rx=le(r, 8), total=le(r, 12), challenge=le(r, 16), key_id=le(r, 20),
                    slots=[dict(seq=le(r, 24 + 6 * i), flags=r[28 + 6 * i], tries=r[29 + 6 * i]) for i in range(2)],
                    guard=le(r, 36, 2), defers=le(r, 38, 2), image_max=le(r, 40), abi=r[44], chunk=r[45],
                    layout=r[0], **(dict(caps=r[48], ap_state=r[49], ap_err=r[50], ap_target=r[51], live=r[52],
                                         ap_count=r[53], mbox0=le(r, 54), mbox1=le(r, 58), copy=le(r, 62),
                                         esc=r[66], keys_down=r[67], keys_low=r[68], esc_wait=r[69],
                                         hooks=r[70], hk_refused=r[71])
                                    if len(r) >= 70 else {}))

    def boot(self, limit=400):
        def run():
            for i in range(limit):
                self.tick()
                st = self.status()
                if st and st["booted"]:
                    return st, i + 1
            return self.status(), limit
        # once per process for one machine state (rtlib.memo): many checks boot the same NOR again
        return rtlib.memo(("boot", limit), [self, self.nor, self.emu], run)

    def auth_op(self, opc, args, key=KEY, challenge=None, port=2):
        ch = self.status(port)["challenge"] if challenge is None else challenge
        tag = op.auth_tag(key, opc, ch, args)
        r = self.reply(port, rp.req(opc) + pack7(args + tag), opc)
        return (r[0], le(r, 1)) if r else (None, None)

    def begin(self, slot, data, seq, crc=None, **kw):
        args = bytes([slot]) + len(data).to_bytes(4, "little") + \
            (zlib.crc32(data) if crc is None else crc).to_bytes(4, "little") + seq.to_bytes(4, "little")
        return self.auth_op(OP_BEGIN, args, **kw)[0]

    def data(self, off, chunk, port=2):
        r = self.reply(port, rp.req(OP_DATA, (off >> 14) & 0x7F, (off >> 7) & 0x7F, off & 0x7F) + pack7(chunk), OP_DATA)
        return (r[0], le(r, 1)) if r else (None, None)

    def upload(self, slot, data, seq, chunk=200):
        rc = self.begin(slot, data, seq)
        if rc != 0:
            return ("begin", rc)
        off = 0
        frames = 0
        while off < len(data):          # strict stop-and-wait: next chunk only after the ACK
            c = data[off:off + chunk]
            rc, rx = self.data(off, c)
            frames += 1
            if rc != 0 or rx != off + len(c):
                return ("data", rc)
            off = rx
        self.frames = frames
        rc = self.auth_op(OP_COMMIT, bytes([slot]))[0]
        return ("commit", rc)

    def run_job(self, limit=400):
        for i in range(limit):
            self.tick()
            st = self.status()
            if st["job"] in (J["done"], J["error"]):
                return st, i + 1
        return self.status(), limit


SELFTEST_FIXED = 19   # time-independent raw bytes of the op 0x0A reply (core_cave.h CORE_SELFTEST_FIXED)


def host_selftest() -> bytes:
    """The fixed self-test bytes of the host build (result/firmware tools/cave_selftest.c): the same C code."""
    fw = rtlib.REPO / "result/firmware"
    subprocess.run(["make", "-s", "-C", str(fw), "build/cave_selftest"], check=True, stdout=subprocess.DEVNULL)
    out = subprocess.run([str(fw / "build/cave_selftest")], capture_output=True, text=True, check=True)
    return bytes.fromhex(out.stdout.split()[0])


def slot_image(nor, slot):
    off = SLOT[slot] - 0x60000000
    return nor.read(off, 0x20000)


def main(argv):
    names = [a for a in argv[1:] if not a.startswith("-")]
    img = images.by_name(names[0] if names else rtlib.DEFAULT_LOADER)
    core_name, base_name = PAYLOADS[img.name]
    _img, stock, manifest, image, syms = rtlib.loader(img.name)
    blob = (STOCKFW / f"cave/build/cave_{img.variant}.bin").read_bytes()
    elf = STOCKFW / f"cave/build/cave_{img.variant}.elf"
    s1elf = STOCKFW / "cave/build/loader_s1.elf"
    s1file = (STOCKFW / "build/ota_stage1.c79").read_bytes()
    cave_lo, cave_end = syms["cave_h1"], syms["cave_end"]
    pays = [images.OTA_PAYLOADS_BY_NAME[n] for n in (core_name, base_name)]
    files = {p.name: (STOCKFW / "build" / f"{p.name}.c79").read_bytes() for p in pays}
    core_file, base_file = files[core_name], files[base_name]
    ok = True
    print(f"image   {img.name}  payloads {core_name} / {base_name}  sha256 {pa.sha256(image)}  stage-0 {len(blob)} B  "
          f"stage-1 {len(s1file) - 64} B  key id {op.key_id(KEY):08x}")

    # ---- 1. image + static audit ------------------------------------------------------------
    changed = pa.check_only_declared_diffs(stock, pa.apply_patches(stock, manifest), manifest)
    ok &= check("1 only declared bytes differ, size unchanged", len(image) == len(stock), f"{len(changed)} bytes")
    ok &= check("1 revert reproduces stock sha256",
                pa.sha256(pa.apply_patches(pa.apply_patches(stock, manifest), manifest, reverse=True)) == pa.sha256(stock))
    built = STOCKFW / "build" / f"{img.name}.SYS"
    la = images.SLOT_FILE_OFFSET[0x10]
    ok &= check(f"1 build/{img.name}.SYS is this image outside loader slot A (the round trips seed ota_stage1 there)",
                built.is_file() and built.read_bytes()[:la] == image[:la] and built.read_bytes()[la + 0x20000:] == image[la + 0x20000:]
                and built.read_bytes()[la:la + 0x20000] == b"\xff" * 0x20000)
    kfo = op.KEY_FILE_OFFSET
    tracked = [STOCKFW / f"cave/build/cave_{img.variant}.{x}" for x in ("bin", "elf")] + \
        [STOCKFW / f"cave/build/loader_s1.{x}" for x in ("bin", "elf")] + \
        [STOCKFW / f"cave/build/payload_{p.variant}.{x}" for p in pays for x in ("bin", "elf")]
    ok &= check("1 root key patched at file 0x115FE0 (VA 0x40175FE0), loader copy #1 extended to 0x40176000, "
                "key bytes in no tracked blob/ELF/manifest",
                image[kfo:kfo + 32] == KEY and image[0x4B3EC:0x4B3F0] == (0x40176000).to_bytes(4, "little")
                and cave_end <= op.KEY_VA and all(KEY not in f.read_bytes() and KEY.hex() not in f.read_bytes().hex()
                                                  for f in tracked + [STOCKFW / f"patches/{img.name}.json"]))
    for name, data in {**files, "ota_stage1": s1file}.items():
        bad = op.verify(data, KEY)
        ok &= check(f"1 {name}.c79 verifies (header, CRC, HMAC), {len(data)} B", not bad, ", ".join(bad))

    def sizes(e):
        out = subprocess.run([str(rt.mm.NM), "-S", str(e)], capture_output=True, text=True, check=True).stdout
        return {q[3]: (int(q[0], 16), int(q[0], 16) + int(q[1], 16)) for q in (l.split() for l in out.splitlines()) if len(q) == 4}

    # stage-0
    targets, nor_lits, indirect = rp.static_calls(elf, cave_lo, cave_end)
    size = sizes(elf)
    bad = {hex(t): v for t, v in targets.items() if t not in S0_ALLOWED}
    ok &= check("1 stage-0 branches only to the copier / busy / jobs / dis / ena, the three NOR writers + busy flag and "
                "the two hook returns", not bad, str(bad))
    wrap = {NOR_ERASE: "nor_erase", NOR_BUF: "nor_prog", NOR_HW: "nor_prog_hw"}
    stray = [(hex(t), hex(a)) for t, w in wrap.items() for a in targets.get(t, []) if not size[w][0] <= a < size[w][1]]
    ok &= check("1 stage-0: each stock NOR writer is called from exactly its guarded service function (nor_erase / "
                "nor_prog / nor_prog_hw); the busy flag only inside them (1 -> writer -> 0)",
                not stray and all(len(targets.get(t, [])) == 1 for t in wrap)
                and len(targets.get(0x4013403C, [])) == 6
                and all(any(size[w][0] <= a < size[w][1] for w in wrap.values()) for a in targets.get(0x4013403C, [])),
                str(stray))
    calls_erase = [a for a, m, o in rtlib.instructions(elf) if m.startswith("bl") and o.startswith(f"{size['nor_erase'][0]:x}")]
    ok &= check("1 stage-0 never erases on its own: nor_erase is reached only through the service table (no direct call)",
                not calls_erase, str(calls_erase))
    # include/s0.h range table: slots, kvstore + gate 0 (adjacent), gates 1 + 2 (adjacent); plus the BootProgram bound
    # the guard refuses below (0x60060000, as `> 0x6005FFFF`) - BootProgram's own start is only READ (alias check)
    bad_lit = [(hex(a), hex(w)) for a, w in nor_lits
               if not (0x6017FFFF <= w <= 0x60200000 or 0x6047FFFF <= w <= 0x60500000 or 0x605FFFFF <= w <= 0x61000000
                       or w in (0x60000000, 0x6005FFFF, 0x60060000))]
    ok &= check("1 every NOR literal in stage-0 lies in (or is a bound of) an entry of the range table (slot sectors, "
                "kvstore, the three gates), or is BootProgram's start (read by the alias check) / end bound (the guard's "
                "floor)", not bad_lit, str(bad_lit[:3]))
    disp = [size[n] for n in ("s0_sysex", "s0_step") if n in size]
    stray = [(hex(a), t) for a, t in reg_indirect(indirect) if not any(lo <= a < hi for lo, hi in disp)]
    ok &= check("1 stage-0 indirect calls only in its dispatch (s0_sysex / s0_step -> the stage-1 entries)", not stray, str(stray[:3]))
    # stage-1
    s1sym = rtlib.stage1_symbols()
    t1, l1, i1 = rp.static_calls(s1elf, WIN[0], s1sym["cave_end"])
    size1 = sizes(s1elf)
    bad = {hex(t): v for t, v in t1.items() if t not in {**S1_ALLOWED, 0x400A8340: 1, 0x400AA030: 1, 0x4013427C: 1,
                                                          0x401342AC: 1, DIS: 1, ENA: 1}}
    ok &= check("1 stage-1 branches only to the TX writers, the timer, the copier / busy / jobs, dis / ena: no NOR writer",
                not bad and not rtlib.load("build_images").writer_refs(s1elf), str(bad))
    bad_lit = [(hex(a), hex(w)) for a, w in l1 if not 0x60180000 <= w <= 0x60200000]
    ok &= check("1 every NOR literal in stage-1 lies in the slot sectors", not bad_lit, str(bad_lit[:3]))
    sdisp = [size1[n] for n in ("s1_sysex", "s1_tick", "apply_step", "nor_erase", "nor_program", "nor_program_hw",
                                "install", "v_finish", "s1_release") if n in size1]
    stray = [(hex(a), t) for a, t in reg_indirect(i1) if not any(lo <= a < hi for lo, hi in sdisp)]
    ok &= check("1 stage-1 indirect calls only in its dispatch (-> the main) and the stage-0 service calls", not stray, str(stray[:3]))
    for p in pays:
        pelf = STOCKFW / f"cave/build/payload_{p.variant}.elf"
        psyms = rt.mm.symbols(pelf)
        t2, l2, _i2 = rp.static_calls(pelf, RUN, psyms["cave_end"])
        if sb.have_map(p.variant):
            pmap = (STOCKFW / f"cave/build/core_{p.variant}/cave.map").read_text()
            ptext_hi = min(int(m, 16) for m in re.findall(r"\n \.(?:rodata|data)\S*\s+0x([0-9a-f]{8})", pmap)
                           + re.findall(r"\n \.(?:rodata|data)\S*\n\s+0x([0-9a-f]{8})", pmap) if int(m, 16) >= RUN)
        else:   # no build intermediates (a test payload's ELF is local only): the code end from the ELF (same value)
            ptext_hi = min(lo for lo, _hi, sec, kind, _o in sb.elf_layout(p.variant) if sec == ".cave" and kind != "text")
        l2 = [(a, w) for a, w in l2 if a < ptext_hi]
        badp = {hex(t): v for t, v in t2.items() if t not in {**rp.ALLOWED_CALLS, 0x400A8340: 1}}
        ok &= check(f"1 payload {p.name}: linked at 0x401C0000 (entry table first), stock calls = the core_probe set, "
                    "no NOR writer, no NOR literal",
                    psyms.get("ota_entry") == RUN and not badp and not l2, str(badp or l2[:2]))

    # ---- 2. first boot, empty main slots ----------------------------------------------------
    pn = Piano(image, syms)
    ok &= check("2 loader copied stage-0 + key into SDRAM", pn.emu.read_bytes(cave_lo, len(blob)) == blob
                and pn.emu.read_bytes(op.KEY_VA, 32) == KEY)
    st, n = pn.boot()
    ok &= check("2 empty main slots -> stage-1 from loader slot A, no main (reason: no valid slot)",
                st["booted"] and st["active"] == ACT_BUILTIN and st["reason"] == BR["empty"] and st["layout"] == 3
                and pn.emu.read_bytes(WIN[0], 0x1000) == s1file[64:64 + 0x1000], f"{st['active']}/{st['reason']} after {n} ticks")
    ok &= check("2 status: key id matches the build key, ABI 1, chunk 200, image max 0x1FE00",
                st["key_id"] == op.key_id(KEY) and st["abi"] == 1 and st["chunk"] == 200 and st["image_max"] == 0x1FE00)
    r = rp.body_of(pn.sysex(2, rp.req(0x01)), 2)
    ok &= check("2 identify answered by stage-1's no-main responder: ngver 1, feat0 0x01 feat1 0x04 (OTA)",
                r is not None and r[1] == NO_MAIN_ID, r[1].hex() if r else "-")
    sent = rt.deliver(pn.emu, 1, rp.req(OP_STATUS))
    ok &= check("2 the real stock hook path (end handler -> s0_h1 -> stage-1) answers OTA status on port 1",
                (b := rp.body_of(sent, 1)) is not None and b[0] == 0x50)
    pn.emu.hook(rt.ENGINE_TAIL, lambda e: None)
    pn.hook_tx()                          # deliver() unhooked the TX writers

    # ---- 3. upload ota_core_v2 to slot A-----------------------------------------------------
    res = pn.upload(0, core_file, seq=1)
    ok &= check(f"3 upload {core_name} ({len(core_file)} B) to slot A: begin/data/commit accepted",
                res == ("commit", 0), f"{res}, {getattr(pn, 'frames', 0)} data frames of <= 200 B")
    frame_len = 7 + 3 + len(pack7(bytes(200))) + 1
    ok &= check("3 data frame size <= 256 B (sysex-size finding)", frame_len <= 256, f"{frame_len} B per 200 B chunk")
    ops_before = len(pn.nor.ops)
    st, n = pn.run_job()
    ok &= check("3 commit -> done (verify in RAM, erase, program, read back, seal)", st["job"] == J["done"] and st["err"] == 0,
                f"job {st['job']} err {st['err']} after {n} ticks")
    sa = slot_image(pn.nor, 0)
    ok &= check("3 NOR slot A = state 'C79S' seq 1 + state version 1 + the uploaded file at +0x200, rest erased",
                sa[0:4] == b"C79S" and le(sa, 4) == 1 and sa[0x200:0x200 + len(core_file)] == core_file
                and sa[0x200 + len(core_file):] == b"\xff" * (0x20000 - 0x200 - len(core_file))
                and sa[8:0xE] == b"\xff" * 6 and sa[0xE:0x10] == b"\x01\x00" and sa[0x10:0x200] == b"\xff" * 0x1F0)
    kinds = [o.kind for o in pn.nor.ops[ops_before:]]
    ok &= check("3 NOR ops: 1 erase, write-buffer pages, 5 halfwords (seq, state version, magic) - all inside slot A",
                kinds[0] == "erase" and kinds[-5:] == ["program"] * 5 and set(kinds[1:-5]) == {"buffer"}
                and all(0x180000 <= o.offset and o.offset + o.size <= 0x1A0000 for o in pn.nor.ops), f"{len(kinds)} ops")
    ok &= check("3 every stock NOR writer call: dispatch disabled, called from its stage-0 service function, no nesting",
                all(d == 1 for _n, _lr, d, _a in pn.nor_calls) and not pn.nested and
                all(size[w][0] <= lr < size[w][1] for nm, lr, _d, _a in pn.nor_calls
                    for w in [{"erase": "nor_erase", "buf": "nor_prog", "hw": "nor_prog_hw"}[nm]]),
                f"{len(pn.nor_calls)} calls")
    nor_after_a = bytes(pn.nor.mem)
    writes_upload = list(pn.writes)
    pn.close()

    # ---- 4. reboot -> slot A ------------------------------------------------------------------
    pn = Piano(image, syms, nor_mem=nor_after_a)
    st, n = pn.boot()
    ok &= check("4 reboot: slot A verified (CRC + HMAC), copied to 0x401C0000, active", st["active"] == ACT_A
                and st["reason"] == BR["slot"] and pn.emu.read_bytes(RUN, 0x1000) == core_file[64:64 + 0x1000],
                f"active {st['active']} reason {st['reason']} after {n} ticks")
    ok &= check("4 one boot attempt recorded (unconfirmed slot)", st["slots"][0]["tries"] == 1 and
                slot_image(pn.nor, 0)[0x10:0x12] == b"\0\0" and slot_image(pn.nor, 0)[0x12:0x14] == b"\xff\xff")
    r = rp.body_of(pn.sysex(2, rp.req(0x01)), 2)
    ok &= check("4 identify now answered by the slot code at 0x401C0000", r is not None and r[1] == bytes([1, 2, 19, 0x3B, 0x07]))
    calls = []
    pn.emu.hook(rt.TX[2], lambda e: (calls.append(e.reg(14)), pn.sent.append((2, e.read_bytes(e.reg(0), e.reg(1)))))[1])
    pn.sysex(2, rp.req(0x0A))
    pn.sent.clear()
    pn.tick()
    got = [s for s in pn.sent if s[1][6] == 0x4A]
    raw = rp.unpack7(got[0][1][7:-1]) if got else b""
    ok &= check("4 core self-test runs from the slot (TX caller inside the run region), 9/9 checks",
                bool(got) and le(raw, 2, 2) == 0x1FF and all(RUN <= lr < RUN + 0x20000 for lr in calls),
                f"mask {le(raw, 2, 2):#x}" if raw else "no reply")
    host = host_selftest()
    ok &= check(f"4 self-test: the {SELFTEST_FIXED} time-independent bytes equal the host build of the same core "
                "(result/firmware build/cave_selftest)", bool(raw) and raw[:SELFTEST_FIXED] == host,
                f"{raw[:SELFTEST_FIXED].hex()} vs {host.hex()}")
    pn.emu.write_u32(syms["ota_alive"], CONFIRM_TICKS - 2)
    pn.tick(3)
    st = pn.status()
    ok &= check("4 after the alive window (7500 passes ~60 s) the slot is confirmed in NOR",
                st["slots"][0]["flags"] & 0x02 and slot_image(pn.nor, 0)[8:10] == b"\0\0")
    # ---- 5. ota_base_v2 to slot B--------------------------------------------------------------
    ok &= check("5 begin on the active slot A is refused (slot 4)", pn.begin(0, base_file, 2) == 4)
    res = pn.upload(1, base_file, seq=2)
    st, n = pn.run_job()
    ok &= check(f"5 upload + commit {base_name} to slot B", res == ("commit", 0) and st["job"] == J["done"], f"{res} {st['job']}/{st['err']}")
    nor_ab = bytes(pn.nor.mem)
    pn.close()
    pn = Piano(image, syms, nor_mem=nor_ab)
    st, _ = pn.boot()
    r = rp.body_of(pn.sysex(2, rp.req(0x01)), 2)
    ok &= check("5 reboot -> slot B (seq 2 > 1) runs: identify feat1 0x05 (no core) from the slot",
                st["active"] == ACT_B and r is not None and r[1][3:5] == bytes([0x3B, 0x05]), f"active {st['active']}")
    # ---- 6. rollback ----------------------------------------------------------------------------
    rc, _ = pn.auth_op(OP_REJECT, bytes([2]))
    pn.tick(2)
    st = pn.status()
    ok &= check("6 reject B (rollback) accepted, rejected flag programmed", rc == 0 and st["slots"][1]["flags"] & 0x04
                and slot_image(pn.nor, 1)[0xA:0xC] == b"\0\0")
    nor_rej_b = bytes(pn.nor.mem)
    pn.close()
    pn = Piano(image, syms, nor_mem=nor_rej_b)
    st, _ = pn.boot()
    ok &= check("6 reboot -> slot A again", st["active"] == ACT_A)
    rc, _ = pn.auth_op(OP_REJECT, bytes([1]))
    pn.tick(2)
    nor_rej_ab = bytes(pn.nor.mem)
    pn.close()
    pn = Piano(image, syms, nor_mem=nor_rej_ab)
    st, _ = pn.boot()
    ok &= check("6 reject A as well -> no main (reason: no valid slot)", rc == 0 and st["active"] == ACT_BUILTIN
                and st["reason"] == BR["empty"])
    pn.close()

    # ---- 7. corrupted CRC ----------------------------------------------------------------------
    mem = bytearray(nor_ab)
    mem[0x1C0000 + 0x200 + 64 + 100] ^= 0x01        # flip one payload bit of slot B (the newest)
    pn = Piano(image, syms, nor_mem=bytes(mem))
    st, _ = pn.boot()
    ok &= check("7 slot B payload bit flipped in NOR -> B fails (flag bad), A boots", st["active"] == ACT_A
                and st["slots"][1]["flags"] & 0x10 and st["reason"] == BR["slot"])
    pn.close()
    mem = bytearray(nor_after_a)
    mem[0x180000 + 0x200 + 64 + 7] ^= 0x80
    pn = Piano(image, syms, nor_mem=bytes(mem))
    st, _ = pn.boot()
    ok &= check("7 the only slot corrupted -> no main (reason fallback)", st["active"] == ACT_BUILTIN and st["reason"] == BR["fallback"])
    pn.close()

    # ---- 8. power cut during the commit -----------------------------------------------------------
    pn = Piano(image, syms, nor_mem=nor_after_a)
    pn.boot()
    pn.upload(1, base_file, seq=2)
    st, _ = pn.run_job()
    n_ops = len([o for o in pn.nor.ops if o.offset >= 0x1C0000])
    pn.close()
    cuts = sorted({1, 2, n_ops // 2 + 1, n_ops - 3, n_ops - 1, n_ops})
    for m in cuts:
        pn = Piano(image, syms, nor_mem=nor_after_a)
        pn.boot()
        start = len(pn.nor.ops)
        orig = pn.nor._start
        nor = pn.nor

        def cut_start(kind, data, lo, hi, status_at, pc, _orig=orig, _m=m, _start=start, _nor=nor):
            _orig(kind, data, lo, hi, status_at, pc)
            if len(_nor.ops) - _start == _m:          # the m-th op of this commit has begun: cut now
                if kind == "erase":                    # interrupted erase: half the sector erased
                    _nor.mem[lo:lo + (hi - lo) // 2] = b"\xff" * ((hi - lo) // 2)
                else:                                  # interrupted program: half the words written
                    for a, v in list(data.items())[: max(1, len(data) // 2)]:
                        _nor.word_and(a, v) if len(data) > 1 else None
                raise PowerCut(kind)

        nor._start = cut_start
        what = "?"
        try:
            pn.upload(1, base_file, seq=2)
            pn.run_job()
        except PowerCut as e:
            what = str(e)
        nor._start = orig
        mem = bytes(nor.mem)
        pn.close()
        pn = Piano(image, syms, nor_mem=mem)
        st, _ = pn.boot()
        ok &= check(f"8 power cut during commit op {m}/{n_ops} ({what}) -> reboot boots slot A",
                    what != "?" and st["active"] == ACT_A, f"active {st['active']}, B flags {st['slots'][1]['flags']:#x}")
        pn.close()

    # ---- 9. bad images never reach the flash -------------------------------------------------------
    pn = Piano(image, syms, nor_mem=nor_after_a)
    pn.boot()
    bad_mac = bytearray(base_file)
    bad_mac[0x30] ^= 1
    other = op.pack(base_file[64:], bytes(32), le(base_file, 0x20))           # signed with another key
    for label, data, crc, want in (("MAC flipped", bytes(bad_mac), None, 12),
                                   ("other key (key id mismatch)", other, None, 11),
                                   ("begin CRC wrong", base_file, 0x12345678, 10)):
        n0 = len(pn.nor.ops)
        rc = pn.begin(1, data, 5, crc=crc)
        off = 0
        while off < len(data):
            off = pn.data(off, data[off:off + 200])[1]
        rc2 = pn.auth_op(OP_COMMIT, bytes([1]))[0]
        st, _ = pn.run_job()
        ok &= check(f"9 {label}: commit -> error {want}, no NOR op at all", rc == 0 and rc2 == 0 and st["job"] == J["error"]
                    and st["err"] == want and len(pn.nor.ops) == n0, f"err {st['err']}")

    # ---- 10. auth and argument checks ---------------------------------------------------------------
    st = pn.status()
    ch = st["challenge"]
    ok &= check("10 begin with a tag from the wrong key -> auth error (2)", pn.begin(1, base_file, 9, key=bytes(32)) == 2)
    ok &= check("10 replay: a tag for an already used challenge -> auth error", pn.begin(1, base_file, 9, challenge=ch) == 2)
    ok &= check("10 begin slot 2 / active slot / seq not newer -> slot 4 / slot 4 / seq 6",
                (pn.begin(2, base_file, 9), pn.begin(0, base_file, 9), pn.begin(1, base_file, 1)) == (4, 4, 6))
    ok &= check("10 begin -> data out of order -> offset error 7, ack carries the expected offset",
                pn.begin(1, base_file, 9) == 0 and pn.data(400, base_file[400:600]) == (7, 0))
    ok &= check("10 commit before all bytes -> incomplete 9", pn.auth_op(OP_COMMIT, bytes([1]))[0] == 9)
    ok &= check("10 reject with a bad tag -> auth error, nothing queued",
                pn.auth_op(OP_REJECT, bytes([1]), key=bytes(32))[0] == 2 and pn.status()["rej"] == 0)

    # ---- 11. hard guard ----------------------------------------------------------------------------
    g0 = pn.status()["guard"]
    n0, c0 = len(pn.nor.ops), len(pn.nor_calls)
    probes = [("nor_erase", (0x60460000,)), ("nor_erase", (0x60160000,)), ("nor_erase", (0x60200000,)),
              ("nor_erase", (0x60180200,)), ("nor_erase", (0x60000000,)), ("nor_erase", (0x601A0000,)),
              ("nor_prog", (0x6017FE00, STAGE, 0x200)), ("nor_prog", (0x60200000, STAGE, 0x200)),
              ("nor_prog", (0x601FFF00, STAGE, 0x200)), ("nor_prog", (0x601A0200, STAGE, 0x200)),
              ("nor_prog", (0x60180001, STAGE, 2)), ("nor_prog", (0x604BFE00, STAGE, 0x200)),
              ("nor_prog_hw", (0x60200000, 0)), ("nor_prog_hw", (0x6017FFFE, 0)), ("nor_prog_hw", (0x60500000, 0)),
              ("nor_prog_hw", (0x60180001, 0)), ("nor_prog_hw", (0x40180000, 0))]
    rcs = [pn.call(syms[f], *a).r0 for f, a in probes]
    ok &= check(f"11 {len(probes)} out-of-range / misaligned erase+program calls (and erase / page program of the running "
                "loader sector A) -> stage-0 guard (3), stock routine never entered, "
                "NOR model trap never fired, guard counter counts them",
                rcs == [3] * len(probes) and len(pn.nor_calls) == c0 and len(pn.nor.ops) == n0 and pn.nor.fault is None
                and pn.status()["guard"] == g0 + len(probes), str(rcs))
    pn.close()

    # ---- 12. stock flash busy -------------------------------------------------------------------------
    pn = Piano(image, syms, nor_mem=nor_after_a)
    pn.boot()
    pn.upload(1, base_file, seq=2)
    for _ in range(30):
        pn.tick()
        if pn.status()["job"] == J["erase"]:
            break
    pn.emu.write_bytes(BUSY_FLAG, b"\x01\x00\x00\x00")
    n0 = len(pn.nor.ops)
    pn.tick(20)
    st = pn.status()
    ok &= check("12 stock busy flag 0x4139DAC4 = 1 -> erase deferred for 20 passes, no NOR op",
                st["job"] == J["erase"] and len(pn.nor.ops) == n0 and st["defers"] >= 20, f"defers {st['defers']}")
    pn.emu.write_bytes(BUSY_FLAG, b"\x00\x00\x00\x00")
    st, _ = pn.run_job()
    ok &= check("12 busy cleared -> the commit completes", st["job"] == J["done"])
    nor_ab2 = bytes(pn.nor.mem)
    pn.close()
    pn = Piano(image, syms, nor_mem=nor_ab2)
    pn.emu.write_bytes(BUSY_FLAG, b"\x01\x00\x00\x00")
    pn.tick(10)
    st = pn.status()
    ok &= check("12 at boot: busy -> stage-0's loader slot scan waits (no stage-1 yet), nothing activated",
                st is None and pn.emu.read_bytes(syms["s0_state"], 1)[0] == 1)
    pn.emu.write_bytes(BUSY_FLAG, b"\x00\x00\x00\x00")
    st, _ = pn.boot()
    ok &= check("12 ... then boots slot B", st["active"] == ACT_B)
    pn.close()

    # ---- 13. escape and attempts ------------------------------------------------------------------------
    pn = Piano(image, syms, nor_mem=nor_ab, mailbox=(0x55AAA55A, ESC))
    st, _ = pn.boot()
    s0r = pn.emu.read_bytes(syms["s0_boot"] + 10, 1)[0]
    ok &= check("13 A0+A#0+B0 held (mailbox 0x40000007) -> pure stock: no stage-1 (no 0x7D answer), stage-0 reason "
                "escape, slots untouched", st is None and s0r == 3 and bytes(pn.nor.mem) == nor_ab)
    pn.close()
    pn = Piano(image, syms, nor_mem=nor_ab, sdram_code=ESC)
    st, _ = pn.boot()
    ok &= check("13 ... also via MainProgram's copy *0x40000000", st is None and pn.emu.read_bytes(syms["s0_boot"] + 10, 1)[0] == 3)
    pn.close()
    pn = Piano(image, syms, nor_mem=nor_ab, mailbox=(0x55AAA55A, 0x4000010D))
    st, _ = pn.boot()
    ok &= check("13 another key code (stock A0 B0 C1 F1) does not trigger the escape", st["active"] == ACT_B)
    pn.close()
    mem = nor_ab
    seen = []
    for _ in range(4):                       # B unconfirmed: boots 1..3 record attempts, 4th skips it
        pn = Piano(image, syms, nor_mem=mem)
        st, _ = pn.boot()
        seen.append(st["active"])
        mem = bytes(pn.nor.mem)
        pn.close()
    ok &= check("13 unconfirmed slot B: 3 boot attempts, then given up -> slot A", seen == [ACT_B, ACT_B, ACT_B, ACT_A],
                str(seen))

    # ---- 14. stores + stack -------------------------------------------------------------------------------
    data_lo = min(lo for lo, _hi, sec, kind, _o in sb.layout(img.variant) if sec == ".cave" and kind == "data")
    allowed = [(data_lo, cave_end), WIN, (CORE_RW, 0x401A0000), (STAGE, STAGE + 0x20000), (RUN, RUN + 0x20000),
               (VBUF, VBUF + 0x1000), STACK, (KERNEL_ERR, KERNEL_ERR + 4), (BUSY_FLAG, BUSY_FLAG + 4)]
    stray = [w for w in writes_upload if not any(lo <= w.addr < hi for lo, hi in allowed)]
    code_w = [w for w in writes_upload if cave_lo <= w.addr < data_lo]
    ok &= check(f"14 {len(writes_upload)} CPU stores of boot + upload + commit: only stage-0 .data, the stage-1 window, core RW, staging, "
                "run region, read-back buffer, stack, NORTi error word 0x4139E778, busy flag 0x4139DAC4 (1 -> 0)",
                not stray and not code_w, f"stray {stray[0]}" if stray else (f"code {code_w[0]}" if code_w else ""))
    pn = Piano(image, syms, nor_mem=nor_after_a)
    lo, hi = rp.STOCK_RAM
    before = pn.emu.read_bytes(lo, hi - lo)
    pn.boot()
    pn.upload(1, base_file, seq=2)
    pn.run_job()
    after = pn.emu.read_bytes(lo, hi - lo)
    diff = [lo + i for i in range(len(after)) if after[i] != before[i]] if after != before else []
    diff = [a for a in diff if not (KERNEL_ERR <= a < KERNEL_ERR + 4 or a == DSP_BYTE)]
    bw = [w.value for w in writes_upload if w.addr == BUSY_FLAG]
    ok &= check("14 busy flag writes alternate 1, 0 (one pair per stock writer call) and end at 0",
                bw and bw == [1, 0] * (len(bw) // 2), f"{len(bw)} writes")
    ok &= check("14 stock RAM 0x41000000-0x41D00000 unchanged except 0x4139E778 (E_CTX) and the test's dispatch byte",
                not diff, f"{len(diff)} bytes, first {diff[0]:#x}" if diff else "")
    # measured stack of a commit pass (erase / program through the stock routines) and of a boot pass
    pn.close()
    depth = {}
    for label, prep in (("boot verify pass", None), ("commit passes", "commit")):
        pn = Piano(image, syms, nor_mem=nor_after_a)
        if prep:
            pn.boot()
            pn.upload(1, base_file, seq=2)
        deepest = 0
        for _ in range(60):
            pn.emu.write_bytes(0x40FF0000, b"\xCD" * 0x10000)
            pn.tick()
            m = pn.emu.read_bytes(0x40FF0000, 0x10000)
            deepest = max(deepest, 0x10000 - next(i for i, b in enumerate(m) if b != 0xCD))
            st = pn.status()
            if st and ((prep and st["job"] in (J["done"], J["error"])) or (not prep and st["booted"])):
                break
        depth[label] = deepest
        pn.close()
    ok &= check("14 measured stack of the engine-loop pass <= 2048 B (task 24 has 4 KB)",
                all(v <= 2048 for v in depth.values()), ", ".join(f"{k} {v} B" for k, v in depth.items()))

    return rtlib.finish(ok, " - emulation only: no interrupts, no RTOS scheduling, NOR timing is poll counts.")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
