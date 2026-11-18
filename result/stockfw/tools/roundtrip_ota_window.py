# roundtrip_ota_window.py - emulator verification of the fast OTA upload of the ota_loader_v4 stage-1 (REMOTE_PROTOCOL
# 10.3 "Data v2", cave/include/ota.h "fast upload"): v2 data frames (CRC-32 each, up to 1536 B, optional LZ tokens) with
# several frames in flight, cumulative ACK and go-back on a gap / CRC error / timeout - and the old 200 B stop-and-wait
# frames still taken.  Plus a throughput model on the same machine with 31.25 kbaud timing.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_ota_window.py [-q] [--full]
#
# Machine: roundtrip_ota.Piano on the real ota_loader_v4 image (stage-1 seeded in loader slot A, no main).  Every data
# frame goes through the REAL stock port 2 receive chain (tools/sysex_size.py: external UART model -> RX ISR -> 2 KB ring
# -> pump FUN_40148FB0 -> parser -> SysEx assembly 0x800 -> end handler -> stage-0 s0_h1 -> stage-1), in virtual time:
#   - BLE uplink: a frame becomes available `lat_up` after the sender sends it and crosses BLE at `ble` B/s into the
#     module's buffer (unbounded here; its peak is reported);
#   - module -> piano UART at 31.25 kbaud = 3125 B/s, the ISR + pump every 2 ms MIDI-task tick (as the stock);
#   - the engine loop (stage-1 tick) every 8 ms; a reply reaches the sender `lat_down` after the piano sends it.
# The sender is a Python copy of the app's (result/app/src/protocol/ota.ts OtaClient.update): window W, cumulative ACK
# (`received`), go back to `received` on rc 7 / 10 from a frame of the current round or on a timeout.
# Profiles: "win" = calibrated on the 2026-11-15 field echo test (Electron/Windows Web MIDI: 256 B 170 ms, 1024 B
# 455 ms, 2048 B 900 ms -> 0.41 ms/B = BLE ~2.46 KB/s plus ~66 ms round trip); "fast" = a link faster than the UART
# (BLE 6 KB/s, 30 ms round trip - e.g. Android with short connection intervals), UART-bound.
# Checks (numbered as printed):
#   1  Begin's reply: 9 raw bytes (rc, challenge, caps = v2 | LZ, window 4, max 1536); Commit's still 5
#   2  old 200 B stop-and-wait frames over the real chain -> commit done, slot = file (what older apps send)
#   3  v2 raw frames, 1024 B, 2 in flight -> done, slot = file, no resend; every frame <= 0x800 B total; ring and module
#      buffer peaks; stage-1 stores while receiving only in the staging area, VBUF, its window and the stack
#   4  v2 LZ frames (the app's encoder rules, tools/ota_frames.py) -> done, slot = file
#   5  limits: a 1536 B frame is taken (1773 B SysEx); 1537 B -> rc 1, nothing stored; a raw frame past the end -> 5;
#      not receiving -> 8; malformed LZ with a good CRC (distance beyond the received bytes, output past the end,
#      truncated token) -> 1 and received unchanged; unknown flags -> 1
#   6  faults on the way, each upload completes with the slot = file: frame lost (gap -> 7 -> go back), byte changed
#      (-> 10), tail lost (F7 never comes; the next F0 flushes the cut frame -> 10), frame duplicated (-> 0, received
#      unchanged), two frames swapped (-> 7, go back), a reply lost (timeout -> go back, the resent frame -> 0)
#   7  throughput model (ota_core_v2, or ota_main with --full): old 200 B stop-and-wait vs v2 raw / LZ, 1-3 in flight,
#      both profiles; the old protocol on "win" must land near the field 1.25-1.3 KB/s (calibration)
from __future__ import annotations

import pathlib
import sys
import zlib
from collections import deque

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

ro = rtlib.load("roundtrip_ota")
ss = rtlib.load("sysex_size")
of = rtlib.load("ota_frames")
op, rp = ro.op, ro.rp
le, pack7, unpack7 = rtlib.le, rtlib.pack7, rtlib.unpack7
STOCKFW = rtlib.STOCKFW
KEY = ro.KEY
QUIET = "-q" in sys.argv
FULL = "--full" in sys.argv
STAGE, VBUF, WIN = 0x401A0000, 0x401E0000, (0x40176000, 0x40180000)
STACK = ro.STACK
OP_BEGIN, OP_DATA, OP_COMMIT = 0x11, 0x12, 0x13
J_DONE, J_ERROR = 8, 9
UART_BPS = 3125.0
TICK_US = 2000
LZ_VECTOR = "22f6d521b3baf8b1"   # sha256[:8] of the LZ frames of the synthetic 20000 B vector (also in the app test)
PROFILES = {"win": dict(ble=2460.0, lat_up=36_000, lat_down=30_000),
            "fast": dict(ble=6000.0, lat_up=15_000, lat_down=15_000)}


def check(*a, **kw) -> bool:
    return bool(rtlib.check(*a, quiet=QUIET, **kw))


# ---- machine -------------------------------------------------------------------------------------------------------
_, _stock, _manifest, IMAGE, SYMS = rtlib.loader()
_BOOTED = None


def machine():
    """a booted ota_loader_v4 (stage-1, no main) with the real port 2 chain attached: (piano, port2)"""
    global _BOOTED
    pn = ro.Piano(IMAGE, SYMS)
    st, _n = pn.boot()
    assert st and st["booted"], "stage-1 did not boot"
    p2 = ss.Port2(pn.emu)
    pn.hook_tx()
    return pn, p2


def begin_raw(pn, slot, data, seq):
    args = bytes([slot]) + len(data).to_bytes(4, "little") + zlib.crc32(data).to_bytes(4, "little") + seq.to_bytes(4, "little")
    ch = pn.status()["challenge"]
    return pn.reply(2, rp.req(OP_BEGIN) + pack7(args + op.auth_tag(KEY, OP_BEGIN, ch, args)), OP_BEGIN)


def commit(pn, slot):
    ch = pn.status()["challenge"]
    return pn.reply(2, rp.req(OP_COMMIT) + pack7(bytes([slot]) + op.auth_tag(KEY, OP_COMMIT, ch, bytes([slot]))), OP_COMMIT)


def v1_frame(off, chunk):
    return rp.req(OP_DATA, (off >> 14) & 0x7F, (off >> 7) & 0x7F, off & 0x7F) + pack7(chunk) + b"\xF7"


# ---- virtual-time link + the app's sender ---------------------------------------------------------------------------
class Link:
    def __init__(self, pn, p2, prof, log_writes=False):
        self.pn, self.p2, self.prof = pn, p2, prof
        self.t = 0
        self.uplink = deque()          # [available at, bytearray]
        self.module = bytearray()      # bytes in the BLE module waiting for the UART
        self.ble_acc = self.uart_acc = 0.0
        self.replies = deque()         # (deliver at, reply body after the op byte)
        self.drop_reply = set()        # reply numbers (1-based) lost on the way back
        self.nreply = 0
        self.module_peak = self.ring_peak = self.frame_max = 0
        self.max_insns = 0
        self.log_writes = log_writes
        self.writes = rtlib.load("stockemu").WriteLog() if log_writes else None

    def send(self, msg):
        self.frame_max = max(self.frame_max, len(msg))
        self.uplink.append([self.t + self.prof["lat_up"], bytearray(msg)])

    def _call(self, addr):
        res = self.pn.emu.call(addr, max_insns=200_000_000)
        if not res.ok:
            raise RuntimeError(f"call {addr:#x} stopped: {res}")
        if self.log_writes:
            self.writes.extend(res.writes)
        return res

    def step(self):
        self.t += TICK_US
        self.ble_acc += self.prof["ble"] * TICK_US / 1e6
        while self.uplink and self.uplink[0][0] <= self.t and self.ble_acc >= 1:
            m = self.uplink[0][1]
            k = min(len(m), int(self.ble_acc))
            self.module += m[:k]
            del m[:k]
            self.ble_acc -= k
            if not m:
                self.uplink.popleft()
        if not self.uplink or self.uplink[0][0] > self.t:
            self.ble_acc = min(self.ble_acc, 1.0)       # idle air time is not banked
        self.module_peak = max(self.module_peak, len(self.module))
        self.uart_acc += UART_BPS * TICK_US / 1e6
        k = min(len(self.module), int(self.uart_acc))
        if k:
            self.uart_acc -= k
            self.p2.fifo.extend(self.module[:k])
            del self.module[:k]
            self._call(ss.RX_ISR)
            cnt = int.from_bytes(self.pn.emu.read_bytes(ss.RING_CNT, 2), "little")
            self.ring_peak = max(self.ring_peak, cnt)
            res = self._call(ss.PUMP2)
            if self.pn.sent or self.p2.sent:
                self.max_insns = max(self.max_insns, res.insns)
        else:
            self.uart_acc = min(self.uart_acc, 1.0)
        if self.t % 8000 == 0:
            res = self.pn.emu.call(SYMS["cave_tick"], max_insns=60_000_000)
            if self.log_writes:
                self.writes.extend(res.writes)
        for p, m in self.pn.sent + self.p2.sent:
            if p == 2 and m[:6] == b"\xF0\x7D" + rp.TAG and len(m) >= 8:
                self.nreply += 1
                if self.nreply not in self.drop_reply:
                    self.replies.append((self.t + self.prof["lat_down"], m[6], unpack7(m[7:-1])))
        self.pn.sent.clear()
        self.p2.sent.clear()

    def take(self):
        out = []
        while self.replies and self.replies[0][0] <= self.t:
            out.append(self.replies.popleft()[1:])
        return out


class Result:
    def __init__(self, **kw):
        self.__dict__.update(kw)


def send_v2(link, frames, window, mangle=None, timeout_us=None, limit_us=600_000_000):
    """the app's windowed sender (ota.ts WindowSender) over `link`: frames = [(off, end, flags, data)]; returns a Result.
    Every frame sent is an instance (send number s); replies come back in send order, so a reply answers the oldest
    outstanding instance of its `off`, and every older instance without a reply is lost.  A NAK (7 / 10) goes back to
    `received` unless an instance of that frame sent AFTER the NAKed one is still on its way."""
    index = {f[0]: i for i, f in enumerate(frames)}
    total = frames[-1][1]
    wire = [of.frame(o, fl, d) for o, _e, fl, d in frames]
    biggest = max(len(w) for w in wire)
    timeout_us = timeout_us or 1_500_000 + window * biggest * 400
    base, nxt, seq, sent, resends, timeouts, nak = 0, 0, 0, 0, 0, 0, {7: 0, 10: 0}
    out = []                           # outstanding instances [seq, off, sent at], in send order
    while base < total:
        if link.t > limit_us:
            return Result(ok=False, why="time limit", t=link.t, base=base)
        for opc, r in link.take():
            if opc != (OP_DATA | 0x40) or len(r) < 9:
                continue
            rc, rx, off = r[0], le(r, 1), le(r, 5)
            k = next((i for i, x in enumerate(out) if x[1] == off), None)
            s_nak = out[k][0] if k is not None else -1
            if k is not None:
                del out[:k + 1]        # this instance answered, the older ones lost
            if rx > base:
                if rx not in index and rx != total:
                    return Result(ok=False, why=f"received {rx} not on a frame start", t=link.t, base=base)
                base = rx
                out = [x for x in out if x[1] >= base]
                nxt = max(nxt, index.get(base, len(frames)))
            if rc == 0:
                continue
            if rc in (7, 10):
                nak[rc] += 1
                if k is not None and not any(x[1] == base and x[0] > s_nak for x in out):
                    nxt = index.get(base, len(frames))
                    resends += 1
                continue
            return Result(ok=False, why=f"rc {rc}", t=link.t, base=base)
        if base >= total:
            break
        if out and link.t - out[0][2] > timeout_us:
            timeouts += 1
            if timeouts > 8:
                return Result(ok=False, why="timeouts", t=link.t, base=base)
            out = []
            nxt = index.get(base, len(frames))
        while len(out) < window and nxt < len(frames):
            seq += 1
            sent += 1
            for m in (mangle(sent, wire[nxt]) if mangle else [wire[nxt]]):
                link.send(m)
            out.append([seq, frames[nxt][0], link.t])
            nxt += 1
        link.step()
    return Result(ok=True, t=link.t, sent=sent, resends=resends, timeouts=timeouts, nak=nak, total=total)


def send_v1(link, data, chunk=200, limit_us=900_000_000):
    """the old app: 200 B frames, strict stop-and-wait (ota.ts before the fast upload)"""
    off, sent = 0, 0
    while off < len(data):
        if link.t > limit_us:
            return Result(ok=False, why="time limit", t=link.t)
        link.send(v1_frame(off, data[off:off + chunk]))
        sent += 1
        got = None
        t0 = link.t
        while got is None:
            link.step()
            for opc, r in link.take():
                if opc == (OP_DATA | 0x40) and len(r) >= 5:
                    got = r
            if link.t - t0 > 3_000_000:
                return Result(ok=False, why="no reply", t=link.t)
        if got[0] != 0:
            return Result(ok=False, why=f"rc {got[0]}", t=link.t)
        off = le(got, 1)
    return Result(ok=True, t=link.t, sent=sent, resends=0, timeouts=0)


def finish(pn, slot, data):
    """commit + run the job; True when the slot holds exactly `data`"""
    r = commit(pn, slot)
    if not r or r[0] != 0:
        return False, f"commit rc {r[0] if r else None}"
    st, n = pn.run_job(limit=2000)
    img = ro.slot_image(pn.nor, slot)
    good = st["job"] == J_DONE and img[0x200:0x200 + len(data)] == data
    return good, f"job {st['job']} err {st['err']} after {n} ticks"


def upload(data, mode, window=1, chunk=1024, prof="fast", mangle=None, drop_reply=(), log_writes=False, slot=0):
    pn, p2 = machine()
    r = begin_raw(pn, slot, data, seq=1)
    assert r and r[0] == 0, f"begin {r}"
    link = Link(pn, p2, PROFILES[prof], log_writes=log_writes)
    link.drop_reply = set(drop_reply)
    if mode == "v1":
        res = send_v1(link, data)
    else:
        frames = of.lz_frames(data, chunk) if mode == "lz" else of.raw_frames(data, chunk)
        res = send_v2(link, frames, window, mangle=mangle)
    res.link = link
    res.slot_ok, res.job = finish(pn, slot, data) if res.ok else (False, res.why)
    pn.close()
    return res


# ---- checks ---------------------------------------------------------------------------------------------------------
def main(argv):
    ok = True
    core = (STOCKFW / "build/ota_core_v2.c79").read_bytes()
    base = (STOCKFW / "build/ota_base_v2.c79").read_bytes()
    main_file = (STOCKFW / "build/ota_main.c79").read_bytes()

    # 1 Begin reply
    pn, _p2 = machine()
    r = begin_raw(pn, 0, base, 1)
    ok &= check("1 Begin reply: 9 raw bytes, rc 0, caps v2 | LZ, window 4, max 1536",
                r is not None and len(r) == 9 and r[0] == 0 and r[5] == of.BC_DATA2 | of.BC_LZ and r[6] == of.D2_WINDOW
                and le(r, 7, 2) == of.D2_MAX, r.hex() if r else "-")
    for o in range(0, len(base), 200):
        pn.data(o, base[o:o + 200])
    c = commit(pn, 0)
    ok &= check("1 Commit's reply stays 5 raw bytes", c is not None and len(c) == 5 and c[0] == 0, c.hex() if c else "-")
    pn.close()

    # 2 old frames over the real chain
    res = upload(base, "v1", prof="fast")
    ok &= check("2 old 200 B stop-and-wait frames over the real port 2 chain -> commit done, slot = file",
                res.ok and res.slot_ok, f"{res.sent if res.ok else res.why} frames, {res.job}")

    # 3 v2 raw, 2 in flight
    res = upload(core, "raw", window=2, chunk=1024, prof="fast", log_writes=True)
    ln = res.link
    ok &= check("3 v2 raw 1024 B frames, 2 in flight -> done, slot = file, no resend / timeout",
                res.ok and res.slot_ok and res.resends == 0 and res.timeouts == 0,
                f"{getattr(res, 'sent', res.__dict__.get('why'))} frames, {res.job}")
    ok &= check("3 every SysEx <= 0x800 B (stock assembly buffer), ring peak < 0x7FF",
                ln.frame_max <= 0x800 and ln.ring_peak < 0x7FF,
                f"frame max {ln.frame_max} B, ring peak {ln.ring_peak} B, module buffer peak {ln.module_peak} B, "
                f"handler max {ln.max_insns} instructions per pump call with a reply")
    s1end = SYMS["s1.cave_end"] if "s1.cave_end" in SYMS else WIN[1]
    stray = sorted({hex(w.addr) for w in ln.writes if WIN[0] <= w.pc < s1end and not (
        STAGE <= w.addr < STAGE + 0x20000 or VBUF <= w.addr < VBUF + 0x1000 or WIN[0] <= w.addr < WIN[1]
        or STACK[0] <= w.addr < STACK[1])})
    ok &= check("3 stage-1 code stores while receiving only into staging / VBUF / its window / the stack",
                not stray, ", ".join(stray[:6]))

    # 4 LZ
    res = upload(core, "lz", window=2, chunk=1024, prof="fast")
    lzb = sum(len(f[3]) for f in of.lz_frames(core, 1024))
    ok &= check("4 v2 LZ frames, 2 in flight -> done, slot = file",
                res.ok and res.slot_ok and res.resends == 0, f"{len(core)} B as {lzb} B of tokens, {res.job}")

    import hashlib
    syn = bytes(((i * i * 7) >> 3 ^ (i >> 6)) & 0xFF for i in range(20000))
    h = hashlib.sha256()
    for o, e, _fl, t in of.lz_frames(syn, 1024):
        h.update(o.to_bytes(4, "little") + e.to_bytes(4, "little") + t)
    ok &= check("4 the encoder = the app's lzFrames byte for byte (shared vector, app test ota.window.test.ts)",
                h.hexdigest()[:16] == LZ_VECTOR, h.hexdigest()[:16])

    # 5 limits (direct calls into the end-handler path, no timing)
    pn, _p2 = machine()

    def d2(frame_bytes):
        r = rp.body_of(pn.sysex(2, frame_bytes[:-1]), 2)
        x = unpack7(r[1]) if r and r[0] == (OP_DATA | 0x40) else None
        return (x[0], le(x, 1), le(x, 5)) if x and len(x) >= 9 else None

    big = bytes((i * 7 + 3) & 0xFF for i in range(4000))
    begin_raw(pn, 0, big, 1)
    f1536 = of.frame(0, 0, big[:1536])
    got = d2(f1536)
    ok &= check("5 a 1536 B frame is taken (1773 B SysEx incl. F0/F7)", got == (0, 1536, 0) and len(f1536) == 1773,
                f"{got}, {len(f1536)} B")
    got = d2(of.frame(1536, 0, big[1536:3073]))
    ok &= check("5 a 1537 B frame -> rc 1, nothing stored", got == (1, 1536, 1536), str(got))
    got = d2(of.frame(1536, 0, big[1536:3500] + b"\0" * 0))
    ok &= check("5 (1964 B -> rc 1 too)", got == (1, 1536, 1536), str(got))
    got = d2(of.frame(3000, 0, big[3000:3100]))
    ok &= check("5 a frame beyond received -> rc 7 (gap), received unchanged", got == (7, 1536, 3000), str(got))
    got = d2(of.frame(0, 0, big[:100]))
    ok &= check("5 a frame below received (already held) -> rc 0, received unchanged", got == (0, 1536, 0), str(got))
    keep = pn.emu.read_bytes(STAGE, 1536)
    bad_lz = {"distance beyond the received bytes": bytes([0x10, 0xAA, 0x01, 0x07]),   # 1 literal, then dist 0x701
              "output past the end": bytes([0x0F]) + b"\x01\x00" + b"\xFF" * 12,       # match of 3000+ B
              "truncated token": bytes([0xF0, 0xFF]),
              "distance 0": bytes([0x10, 0x55, 0x00, 0x00])}
    for name, toks in bad_lz.items():
        got = d2(of.frame(1536, of.D2_LZ, toks))
        ok &= check(f"5 malformed LZ, good CRC ({name}) -> rc 1, received unchanged", got == (1, 1536, 1536), str(got))
    ok &= check("5 the received bytes are untouched by the refused frames", pn.emu.read_bytes(STAGE, 1536) == keep)
    got = d2(of.frame(1536, 0x02, big[1536:1600]))
    ok &= check("5 unknown flags -> rc 1", got == (1, 1536, 1536), str(got))
    got = d2(of.frame(1536, 0, big[1536:]))           # 2464 B: rc 1 (too long), then fill to the end in 2 frames
    d2(of.frame(1536, 0, big[1536:3072]))
    got = d2(of.frame(3072, 0, big[3072:] + b"\x00\x00"))
    ok &= check("5 a raw frame past the end of the file -> rc 5", got == (5, 3072, 3072), str(got))
    got = d2(of.frame(3072, 0, big[3072:]))
    ok &= check("5 the last frame exactly to the end -> rc 0", got == (0, 4000, 3072), str(got))
    commit(pn, 0)
    got = d2(of.frame(4000, 0, b"x"))
    ok &= check("5 after Commit (not receiving) -> rc 8", got is not None and got[0] == 8, str(got))
    pn.close()

    # 6 faults
    def lose(k0):
        return lambda k, m: [] if k == k0 else [m]

    def flip(k0, at):
        return lambda k, m: [bytes(b ^ 1 if i == at else b for i, b in enumerate(m))] if k == k0 else [m]

    def tail(k0):
        return lambda k, m: [m[:300]] if k == k0 else [m]

    def dup(k0):
        return lambda k, m: [m, m] if k == k0 else [m]

    held = {}

    def swap(k0):
        def f(k, m):
            if k == k0:
                held["m"] = m
                return []
            if k == k0 + 1:
                return [m, held.pop("m")]
            return [m]
        return f

    cases = [("frame lost", dict(mangle=lose(5)), lambda r: r.nak[7] >= 1 and r.resends == 1),
             ("byte changed", dict(mangle=flip(5, 400)), lambda r: r.nak[10] == 1 and r.resends == 1),
             ("tail lost (no F7)", dict(mangle=tail(5)), lambda r: r.nak[10] + r.nak[7] >= 1 and r.resends == 1),
             ("frame duplicated", dict(mangle=dup(5)), lambda r: r.resends == 0 and r.timeouts == 0),
             # (cannot happen on this serial path - BLE writes and the UART keep order - but must still complete;
             # the sender takes the held-back frame for lost, so it goes back twice)
             ("two frames swapped", dict(mangle=swap(5)), lambda r: r.nak[7] >= 1 and 1 <= r.resends <= 2),
             ("reply lost", dict(drop_reply={5}), lambda r: r.timeouts == 0 or r.timeouts == 1)]
    for name, kw, cond in cases:
        for mode in ("raw", "lz"):
            res = upload(core, mode, window=3, chunk=1024, prof="fast", **kw)
            ok &= check(f"6 {name} ({mode}, 3 in flight) -> completes, slot = file",
                        res.ok and res.slot_ok and cond(res),
                        f"sent {getattr(res, 'sent', '-')} resends {getattr(res, 'resends', '-')} timeouts "
                        f"{getattr(res, 'timeouts', '-')} nak {getattr(res, 'nak', '-')} {res.job}")

    # 7 throughput model
    data = main_file if FULL else core
    name = "ota_main" if FULL else "ota_core_v2"
    print(f"7 throughput model, {name} {len(data)} B (upload phase only; Begin / Commit / write add ~3 s)")
    rows = []
    for prof in ("win", "fast"):
        base_t = None
        for label, mode, w, ch in (("old 200 B stop-and-wait", "v1", 1, 200), ("v2 raw 1024 B x1", "raw", 1, 1024),
                                   ("v2 raw 1024 B x2", "raw", 2, 1024), ("v2 raw 1536 B x2", "raw", 2, 1536),
                                   ("v2 raw 1024 B x3", "raw", 3, 1024), ("v2 LZ 1024 B x2", "lz", 2, 1024),
                                   ("v2 LZ 1536 B x2", "lz", 2, 1536)):
            res = upload(data, mode, window=w, chunk=ch, prof=prof)
            t = res.t / 1e6
            base_t = base_t or t
            rows.append((prof, label, res.ok and res.slot_ok, t, len(data) / t / 1000, base_t / t, res.link.module_peak))
            print(f"   {prof:4} {label:24} {'ok' if res.ok and res.slot_ok else 'FAIL':4} {t:7.1f} s  "
                  f"{len(data) / t / 1000:5.2f} KB/s  x{base_t / t:4.2f}  module peak {res.link.module_peak} B")
    ok &= check("7 every modelled upload completes with the slot = file", all(r[2] for r in rows))
    v1win = next(r for r in rows if r[0] == "win" and r[1].startswith("old"))
    ok &= check("7 calibration: the old protocol on 'win' lands at the field 1.0-1.4 KB/s", 1.0 <= v1win[4] <= 1.4,
                f"{v1win[4]:.2f} KB/s")
    return rtlib.finish(ok)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
