# roundtrip_sysex.py - emulator verification of Phase 3-1 in the slot payload ota_main on ota_loader_v4:
# the core's SysEx front owns the Kawai fn 0x10 set at the runtime hook 0x40147E94 and must answer and
# apply byte-for-byte like the stock handler FUN_4014A2C8.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_sysex.py [ota_main] [-q]
#
# H = ota_main (every feature): it hooks five sites (the session
# core's pump tail too) and feat1 is 0x77.  First written for the SysEx-only payload ota_sysex (four sites,
# feat1 0x37; discarded 2026-11-01).
#
# Two machines, each the real ota_loader_v4 image on StockEmu (roundtrip_power.Rig: NOR model, timer):
#   S  the built-in cave, every hook site stock          = the stock handler decides
#   H  ota_main uploaded over the 0x7D OTA ops and applied live (attach() hooks the power sites;
#      0x40147E94 -> hk_sx_kawai)                          = the core front decides
# The same frames go through the real Kawai end handler 0x40147DB0 of both, one by one, and per frame the
# test compares: every TX frame (port, bytes), the stock apply path FUN_4014EBFC (ctx, fn, body, len), the
# apply switch FUN_4016053C (ctx, part, id, data), every CPU store into stock RAM (>= 0x41000000, in
# order), the stop reason and the end handler's state byte.  RTOS calls are stubbed as in p3_map.py
# (queue post, semaphores, delay, the two stop-and-wait ACK waits) so the request / file tasks are not run -
# except in 7, which runs the fn 0x10 stop-and-wait for real against a modelled clock and link.
# A boot-on payload (images boot_on) starts with S3 / S4 / the MIDI channel judge / the executor gate
# on: check 1 reads that, then H switches all four off (switches_off)
# so 2-6 measure the front alone as before; 8 switches S4 on by itself.
# Checks (numbered as printed):
#   1  static: the payload verifies (feat 0x37 + attach); stock_sysex.ld bindings and the stock words
#      they rest on (fn 0x10 path 0x4014AA6C..0x4014AAA0, ctx table 0x400A710E = 01 02 03 04, device
#      literal, ACK key length FUN_4014BB54); hk_sx_kawai = peek, front, `bxne lr`, `b FUN_4014A2C8`
#   2  every fn 0x10 descriptor of the stock table (p3_map cases) on the bound BLE port: S == H
#   3  edge frames: unbound port, other / 0x7F device byte, unknown key, packed 5F 00, length error, key 70,
#      short frame, VT edit 5A / 50 0C, header 71, staging on, panel bound with port 3 forwarding,
#      other functions (02 / 31 / 32 / 7E): S == H; owned ones never enter FUN_4014A2C8, passed ones enter it
#      with the registers of S
#   4  the phone app's golden trace (PianoRemote 1.1.5, every phone -> piano frame in order, from
#      unbound): S == H for every frame, and H's ACK for each of the 52 sets == the real piano's ACK in
#      the capture
#   5  contract (b): the resident's inflight flag is 1 at every TX / stock apply call the front makes and
#      0 after each frame; stack at FUN_4014EBFC vs S; stores by payload code only in its .data, the core
#      RW, the stack and the inflight flag
#   6  op 0x1B: counters = what 2-4 sent; 1B 01 00 from the bound port turns the front off (H then enters
#      FUN_4014A2C8 for a set, S == H), 1B 01 01 back on; unbound -> rc 2
#   8  (payloads with feat3 0x20, Phase 3-4 S4) op 1B 05 06 01 turns the write
#      owner on in H: check 2's 164 owned descriptors again S == H (the gate lets them through, the stock applies the same
#      bytes, the read-back only reads), extra stack at FUN_4014EBFC; a packed set with a short entry reaches FUN_4014EBFC
#      without it (S applies both); 1B 05 06 00: S == H again, counters still
#   7  field failure replay (2026-10-31): the
#      logged full-sync values through the real stock sender FUN_4014BD1C and its real wait FUN_4014BCAC (no
#      stub here), the app's ACKs back through the end handler while the sender waits (DumpReplay).  Far from
#      the 2 ms counter wrap every value goes once; with the counter at 0xFF06 when 55 04 38 goes out, the
#      stock's 16-bit deadline makes every wait time out at once, values are resent every ~5 ms and one is sent
#      16 times -> unbind, like the field log - in S and H alike.  Then the app's 7E 01 00 + 7E 01 01: CONNECT
#      answered in both (the field silence was before the handler)
from __future__ import annotations

import pathlib
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rpw = rtlib.load("roundtrip_power")
p3 = rtlib.load("p3_map")
rh, ro, rp, rt, op, images, pa = rpw.rh, rpw.ro, rpw.rp, rpw.rt, rpw.op, rpw.images, rpw.pa
check, le = ro.check, ro.le
STOCKFW, RUN = ro.STOCKFW, ro.RUN
REPO = STOCKFW.parents[1]
# Checks 4 and 7 replay two BLE captures (original app <-> stock piano) that are not in the public repository:
# without them those two checks print SKIP and the rest runs.
TRACE = REPO / "target/captures/phone_pianoremote_1.1.5.txt"   # optional, your own capture
SE = rt.se.StockEmu

END_HANDLER, A2C8, EBFC, APPLY = 0x40147DB0, 0x4014A2C8, 0x4014EBFC, 0x4016053C
STATE_BASE, LEN_BASE, BUF_BASE = 0x413B5400, 0x413B53F0, 0x413B29D0
BINDING, DEV_ID, FWD, STAGE = 0x413CA3DE, 0x413B5404, 0x413B5408, 0x419156A9
OUT_DATA45 = 0x01E26064                     # GPIO bank 4/5 output data (GP5[4] = bit 20)
RTOS_STUBS = {p3.Q_POST: 0, p3.SEM_TAKE: 0, p3.SEM_GIVE: 0, p3.DELAY: None, p3.ACK_WAIT: 0, p3.ACK_WAIT_10: 0}
OP_FRONT = 0x1B
BINDINGS = {"stk_param_set": EBFC, "stk_dev_id": DEV_ID, "stk_stage_flag": STAGE, "stk_binding": BINDING,
            "stk_port3_fwd": FWD}


class Side:
    """One rig (S or H) with the recorders of this test."""

    def __init__(self, image, syms, data=None):
        self.rig = rig = rpw.Rig(image, syms)
        self.pn, self.emu = rig.pn, rig.emu
        self.inflight_at = syms["ota_in_sysex"]
        if data is not None:
            rc, st = rpw.upload_apply(rig, data)
            self.applied = rc == 0 and st["ap_state"] == rh.AP_DONE
            rig.tick(2)                                   # sxc_tick: the front's mirror store is set up
        else:
            self.pn.boot()
            self.applied = None
        e = self.emu
        # LineInLevel's apply waits (dly_tsk(1) loop, 0x400AB490-0x400AB4A0) for OUT_DATA45 bit 20 (GP5[4]) that
        # another task sets on the piano: shown set here, on both sides
        e.periph[OUT_DATA45] = 0x100000
        # the re-voicing sets run ~12 M instructions and > 1 M stores (stack included): keep every entry (the default 1 M cut S and H at
        # different points - H's payload frames add stack stores - and the stock store lists looked different)
        e._log_limit = 20_000_000
        for a, v in RTOS_STUBS.items():
            e.hook(a, (lambda vv: lambda em: vv)(v))
        self.log = []                                     # per call: TX / EBFC / APPLY / A2C8 events in order
        for p, a in rt.TX.items():
            e.hook(a, (lambda pp: lambda em: self._ev("tx", pp, bytes(em.read_bytes(em.reg(0), em.reg(1)))))(p))
        e.hook(EBFC, lambda em: (self._ev("ebfc", em.reg(0), em.reg(1), em.reg(2),
                                          bytes(em.read_bytes(em.reg(2), max(0, em.reg(3) - 6))), em.reg(3), em.reg(13)),
                                 SE.CONTINUE)[1])
        e.hook(APPLY, lambda em: (self._ev("apply", em.reg(0), em.reg(1), em.reg(2), bytes(em.read_bytes(em.reg(3), 4))),
                                  SE.CONTINUE)[1])
        e.hook(A2C8, lambda em: (self._ev("a2c8", *[em.reg(i) for i in range(16)]), SE.CONTINUE)[1])

    def _ev(self, kind, *a):
        self.log.append((kind, a, self.emu.read_bytes(self.inflight_at, 1)[0]))
        if kind == "tx":
            self.pn.sent.append(a)                       # Piano.sysex / reply read these

    # KeyboardMode / TransposeValue / OctaveShift re-voice every part: ~10^7 instructions once the frame reaches
    # the apply with its real length (the old 5 M budget stopped them at different points on S and H)
    def send(self, port, frame, max_insns=200_000_000, sp=None):
        e = self.emu
        self.log = []
        # as the stock byte parser leaves it (0x40148CA4-0x40148CC4): the bytes before F7 in the buffer, the
        # length without F7 - the end handler appends F7 and counts it (the old harness wrote
        # the F7 too, so every frame reached the handlers one byte long)
        body = frame[:-1] if frame[-1:] == b"\xF7" else frame
        e.write_bytes(BUF_BASE + port * 0x800, body)
        e.write_bytes(LEN_BASE + port * 2, len(body).to_bytes(2, "little"))
        e.write_bytes(STATE_BASE + port, b"\xF0")
        res = e.call(END_HANDLER, port, max_insns=max_insns, regs={"sp": sp} if sp else None)
        self.pn.writes.extend(res.writes)
        stock_w = [(w.addr, w.size, w.value) for w in res.writes if w.addr >= 0x41000000]
        return dict(res=res, log=list(self.log), stock_w=stock_w, state=e.read_bytes(STATE_BASE + port, 1)[0],
                    inflight=e.read_bytes(self.inflight_at, 1)[0])

    def poke(self, addr, data):
        self.emu.write_bytes(addr, data)

    def close(self):
        self.rig.close()


def view(r, regs=True):
    """What must be equal between S and H for one frame."""
    ev = []
    for kind, a, _f in r["log"]:
        if kind == "ebfc":
            ev.append((kind, a[0], a[1], a[3], a[4]))            # ctx, fn, body, len (pointer and sp aside)
        elif kind == "a2c8":
            ev.append((kind,) + ((tuple(a[:12]) + (a[13], a[14])) if regs else ()))   # r0-r11, sp, lr (ip aside)
        else:
            ev.append((kind,) + tuple(a))
    return dict(ev=ev, stock_w=r["stock_w"], stop=r["res"].stop, state=r["state"])


def compare(S, H, port, frame, regs=True, owned=None):
    """Send to both; returns (equal, H result, detail)."""
    rs, rh_ = S.send(port, frame), H.send(port, frame)
    vs, vh = view(rs), view(rh_)
    if owned:   # an owned frame never reaches FUN_4014A2C8 in H; S's entry into it is the only difference
        vs = dict(vs, ev=[x for x in vs["ev"] if x[0] != "a2c8"])
    eq = vs == vh
    detail = ""
    if not eq:
        for k in vs:
            if vs[k] != vh[k]:
                detail = f"{k}: S {str(vs[k])[:300]} | H {str(vh[k])[:300]}"
                break
    return eq, rs, rh_, detail


ACK_HEAD = bytes.fromhex("F0407F07041E")


def is_ack(k, a):
    return k == "tx" and a[1][:6] == ACK_HEAD


def vt_prefix(v1, v2):
    return v1 == 0x5A or (v1 == 0x50 and v2 in (0x0C, 0x0E, 0x10, 0x17))


def gate_drops(frame, port, dev):
    """Phase 4 A intake gate (kpgate.c kpg_rx, feat3 0x02 - in ota_main since the 2026-11-16 fold): the frames of these checks the stock handler enters only to ignore them - another device
    byte (0x4014A304), header 71 not 71 02 from port 0 (0x4014AC50), a header other than 08 02 (0x4014A35C) - the gate
    drops before FUN_4014A2C8, so H owns them (nothing sent, no stock call) where S enters the handler and does nothing."""
    if len(frame) < 7 or frame[0] != 0xF0 or frame[1] != 0x40 or frame[3] in (0x70, 0x60):
        return False
    if frame[2] != dev and frame[2] != 0x7F:
        return True
    if frame[4] == 0x71:
        return not (frame[5] == 0x02 and port == 0)
    return frame[4] != 0x08 or frame[5] != 0x02


def h_entered_a2c8(r):
    return any(k == "a2c8" for k, _a, _f in r["log"])


def kawai(fn, body, dev=0x00):
    return bytes([0xF0, 0x40, dev, fn, 0x08, 0x02]) + bytes(body) + b"\xF7"


def front_diag(side, port=2, req=(0x00,)):
    out = side.pn.sysex(port, rp.req(OP_FRONT, *req))
    b = rp.body_of(out, port)
    if not b or b[0] != OP_FRONT | 0x40:
        return None
    d = rp.unpack7(b[1][2:])
    return dict(sub=b[1][0], rc=b[1][1], flags=d[1], key=d[2:5], seen=le(d, 6), passed=le(d, 10), owned=le(d, 14),
                silent=le(d, 18), mirrored=le(d, 22), unknown=le(d, 26), staged=le(d, 30), packed=le(d, 34),
                acks=le(d, 38), crc=le(d, 42), raw=d)


def trace_frames():
    out = []
    for line in TRACE.read_text().splitlines():
        m = re.match(r"\s*([0-9.]+)\s+(PH->PI|PI->PH)\s+((?:[0-9a-f]{2}\s*)+)$", line)
        if m:
            out.append((m[2], bytes.fromhex(m[3].replace(" ", ""))))
    return out


FAIL_LOG = REPO / "target/captures/ota_sysex_dump_fail.txt"   # optional, your own capture
SEND10, WAIT10, TIME16, TICK16 = 0x4014BD1C, 0x4014BCAC, 0x400AB2E8, 0x4139E796
ACK_FLAG, NAK_FLAG, HOLD_FLAG, PREV_BINDING = 0x413CA3EE, 0x413CA3F0, 0x413CA3F1, 0x413CA3DF
MSG_AT, ISR_SP, WAIT_RET = 0x40B90100, 0x40F80000, 0x4014BD64
WRAP_FROM = 0x10000 - 250          # FUN_4014BCAC: deadline = (start + 250) & 0xFFFF, `cmp; bgt` -> no wait


def log_dump(path=FAIL_LOG):
    """The piano's dump frames of the field log in order (a run of identical frames = one value resent), and the
    resent ones: {key hex: times the app saw it}."""
    frames, n = [], []
    for line in path.read_text().splitlines():
        m = re.match(r"\s*([0-9.]+)\s+RX\s+(f0 40 7f 10 04 1e (?:[0-9a-f]{2}\s*)+)$", line)
        if not m:
            continue
        f = bytes.fromhex(m[2].replace(" ", ""))
        if frames and frames[-1] == f:
            n[-1] += 1
        else:
            frames.append(f)
            n.append(1)
    return frames, {f[6:9].hex(): c for f, c in zip(frames, n) if c > 1}


class DumpReplay:
    """The stock dump sender FUN_4014BD1C (real stop-and-wait FUN_4014BCAC, 2 ms counter 0x4139E796) with the
    app's ACK fed back through the real end handler 0x40147DB0 (H: through hk_sx_kawai) while the sender waits.

    Task 19 preempts the sender: when an ACK is due at a counter read or a dly_tsk, the sender run stops,
    the ACK frame goes through the end handler on its own stack, and the sender resumes with its registers.
    Link model [inference, fitted to the field log's 30 ms per value]: piano -> module UART 31.25 kbaud
    (0.32 ms/B, serial), the app sees a frame at the next 15 ms connection event and ACKs it at once, one
    app write per connection event (queue), module -> piano UART 0.32 ms/B."""

    EVENT, UART_B, UP_PER_EVENT = 15_000, 320, 1      # us, us, app writes per connection event

    def __init__(self, side, tick0, port=2):
        self.s, self.e, self.port = side, side.emu, port
        self.tick0, self.t = tick0, 0
        self.pending, self.sends, self.acks_in = [], [], []
        self.down_free = self.up_free = 0
        self.active = False
        self.waits = []                                   # FUN_4014BCAC results
        self.per_value = []                               # sends per FUN_4014BD1C call
        e = self.e
        e.unhook(p3.ACK_WAIT_10)
        e.hook(TIME16, self._time)
        e.hook(p3.DELAY, self._delay)
        e.hook(rt.TX[port], self._tx)
        # 0x4014BD64 `sub ip, r0, #1`: the sender right after FUN_4014BCAC returned (0 ACK, 1 NAK, 2 timeout)
        e.hook(WAIT_RET, lambda em: (self.waits.append(em.reg(0)), SE.CONTINUE)[1])
        self._sync_tick()

    def tick(self):
        return (self.tick0 + self.t // 2000) & 0xFFFF

    def _sync_tick(self):
        self.e.write_bytes(TICK16, self.tick().to_bytes(2, "little"))

    def _due(self):
        return self.active and self.pending and self.pending[0][0] <= self.t

    def _time(self, em):
        # one pass of the poll loop: 10 us (FUN_4014BCAC's idle passes up to the next tick / ACK skipped, rtlib.poll_step)
        self.t += rtlib.poll_step(em, self.t, self.pending[0][0] if self.pending else 1 << 62,
                                  lambda t: (self.tick0 + t // 2000) & 0xFFFF)
        self._sync_tick()
        if self._due():
            em.stop("preempt")
        return self.tick()

    def _delay(self, em):
        self.t += em.reg(0) * 1000                        # dly_tsk(ms)
        self._sync_tick()
        if self._due():
            em.stop("preempt")
        return 0

    def _tx(self, em):
        f = bytes(em.read_bytes(em.reg(0), em.reg(1)))
        self.s._ev("tx", self.port, f)
        if not self.active or f[:6] != bytes.fromhex("F0407F10041E"):
            return None
        self.sends.append((self.t, f[6:9]))
        self.down_free = max(self.down_free, self.t) + len(f) * self.UART_B
        seen = -(-self.down_free // self.EVENT) * self.EVENT
        ack = kawai(0x07, f[6:9], dev=0x7F)
        self.up_free = max(seen + self.EVENT, self.up_free + self.EVENT // self.UP_PER_EVENT)
        self.pending.append((self.up_free + len(ack) * self.UART_B, ack))
        self.pending.sort(key=lambda x: x[0])
        return None

    def deliver(self):
        while self.pending and self.pending[0][0] <= self.t:
            _at, ack = self.pending.pop(0)
            self.acks_in.append((self.t, ack[6:9]))
            r = self.s.send(self.port, ack, sp=ISR_SP)
            if r["res"].stop != "returned":
                raise RuntimeError(f"end handler stopped: {r['res']}")

    def send_value(self, frame):
        """One FUN_4014BD1C call (r0 = frame, r1 = len) to its return; ACKs delivered as they fall due."""
        e = self.e
        e.write_bytes(MSG_AT, frame)
        n0 = len(self.sends)
        self.active = True
        res = e.call(SEND10, MSG_AT, len(frame), max_insns=50_000_000)
        while res.stop == "preempt":
            regs = {n: res.regs[i] for i, n in enumerate(
                ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr"])}
            self.deliver()
            res = e.call(res.regs[15], regs=regs, max_insns=50_000_000)
        self.active = False
        if res.stop != "returned":
            raise RuntimeError(f"FUN_4014BD1C stopped: {res}")
        self.per_value.append(len(self.sends) - n0)
        return res.r0 if res.r0 < 0x80000000 else res.r0 - 0x100000000


def dump_replay(image, syms, data, frames, tick0):
    """S and H through the same dump: per side the sends (t, key), the sender results, binding, prev binding."""
    out = []
    for side in (Side(image, syms), Side(image, syms, data)):
        side.poke(BINDING, b"\x03")
        r = DumpReplay(side, tick0)
        rcs = []
        for f in frames:
            rc = r.send_value(f)
            rcs.append(rc)
            if rc != 0:
                break
        e = side.emu
        out.append(dict(side=side, rp=r, rcs=rcs, sends=list(r.sends), waits=list(r.waits), per=list(r.per_value),
                        binding=e.read_bytes(BINDING, 1)[0], prev=e.read_bytes(PREV_BINDING, 1)[0],
                        nak=e.read_bytes(NAK_FLAG, 1)[0], hold=e.read_bytes(HOLD_FLAG, 1)[0]))
    return out


# Kawai functions the session core (feat2 0x02) takes before the front on the bound app port: the request fn 0x02
# and the ACK family 07 / 06 / 04 / 05 that answers its own value frames
SESSION_FNS = {0x02, 0x04, 0x05, 0x06, 0x07}


BOOT_ON = False   # main(): the payload's images boot_on


def switches_off(side, port=2):
    """the four runtime switches off as the app turns them off - 1B 05 04 00 (S3), 1B 05 06 00 (S4), 1B 07 00 (MIDI channel
    judge), 1F 01 00 (executor gate) - and one pass: what a default-off payload boots with, the state checks 2-6 were
    written for (the front alone).  rc of each."""
    rcs = [(front_diag(side, port, req) or {}).get("rc") for req in ((0x05, 0x04, 0x00), (0x05, 0x06, 0x00), (0x07, 0x00))]
    b = rp.body_of(side.pn.sysex(port, rp.req(0x1F, 0x01, 0x00)), port)
    rcs.append(b[1][1] if b and len(b[1]) > 1 else None)
    side.rig.tick(1)
    return rcs


def s4_record(side, req=(0x05, 0x07)):
    """op 1B 05 06 / 07 (layout 9): rc, flags and the S4 counters."""
    d = front_diag(side, req=req)
    if d is None:
        return None
    r = d["raw"]
    k = ["sets", "values", "own", "refused", "passed", "followed", "wmiss", "kept", "side", "dropped"]
    out = dict(rc=d["rc"], layout=r[0], flags=r[1], refused_key=bytes(r[24:27]), miss_key=bytes(r[27:30]),
               staged=le(r, 33, 2), big=le(r, 35, 2), unread=r[37])
    out.update({n: le(r, 4 + 2 * i, 2) for i, n in enumerate(k)})
    return out


def s4_checks(image, syms, data):
    """8: S4 on in H (op 1B 05 06 01) - every fn 0x10 descriptor of check 2 again: S == H wherever the gate lets the value
    through (the stock applies the very same bytes, the read-back only reads); the gate's refusals never reach
    FUN_4014EBFC; a packed set with a short entry reaches it without that entry; 1B 05 06 00 = check 2 again."""
    ok = True
    S, H = Side(image, syms), Side(image, syms, data)
    if BOOT_ON:   # from the front alone, as before the boot-on change: S4 is switched on below by itself
        assert switches_off(H) == [0, 0, 0, 0]
    for s in (S, H):
        s.poke(BINDING, b"\x03")
    on = s4_record(H, req=(0x05, 0x06, 0x01))
    ok &= check("8 S4: op 1B 05 06 01 -> rc 0, record layout 9, built + on (flags 0x03)",
                on is not None and on["rc"] == 0 and on["layout"] == 9 and on["flags"] & 0x03 == 0x03, str(on))
    cases = [c for c in p3.load_cases() if c["fn"] == 0x10 and not vt_prefix(c["v1"], c["v2"])]
    fails, refused, same, depth, stopped, extra = [], [], 0, [], 0, []
    for c in cases:
        frame = kawai(0x10, [c["v1"], c["v2"], c["part"]] + c["probe"])
        for s in (S, H):   # check 2's sets change the receive device number (51 ..): keep it 0 so every set applies
            s.poke(DEV_ID, b"\x00\x00\x00\x00")
        eq, rs, rh_, det = compare(S, H, 2, frame, owned=True)
        h_ebfc = [a for k, a, _f in rh_["log"] if k == "ebfc"]
        s_ebfc = [a for k, a, _f in rs["log"] if k == "ebfc"]
        stopped += rs["res"].stop != "returned"
        if eq:
            same += 1
            if h_ebfc and s_ebfc:
                depth.append(s_ebfc[0][5] - h_ebfc[0][5])
                extra.append(rh_["res"].insns - rs["res"].insns)
        elif not h_ebfc and s_ebfc:
            refused.append(f"{c['iid']:#04x} {p3.hexs(frame[6:-1])}")   # (S and H differ from here: a failure below)
        else:
            fails.append((hex(c["iid"]), c["name"], det))
    rec = s4_record(H)
    ok &= check(f"8 S4 on, the {len(cases)} owned fn 0x10 descriptors with check 2's values: S == H for {same} (TX, FUN_4014EBFC / "
                f"FUN_4016053C arguments, stock RAM stores in order, stop, state byte - the read-back adds getter reads "
                f"only); {len(refused)} refused by the gate (no FUN_4014EBFC in H): {refused}; {stopped} stop inside the "
                f"stock apply in both; record sets {rec and rec['sets']} own {rec and rec['own']} refused "
                f"{rec and rec['refused']} followed {rec and rec['followed']} write miss {rec and rec['wmiss']}",
                not fails and not refused and rec is not None and rec["sets"] == len(cases) and same == len(cases)
                and rec["own"] + rec["refused"] + rec["passed"] == rec["values"],
                str(fails[:3]))
    ok &= check(f"8 S4: stack at FUN_4014EBFC: H uses {sorted(set(depth))[:1]}..{sorted(set(depth))[-1:]} B more than S "
                f"(io_apply -> s4_apply noinline; task 19 stack 0x4000); instructions per set H - S (front, gate, read-back): "
                f"median {sorted(extra)[len(extra) // 2] if extra else None}, max {max(extra, default=None)}", depth and max(depth) <= 192, str(sorted(set(depth))))
    # 192: the Thumb payload (Thumb-1 has r0-r7 only, more spills) measures 168 B here where the ARM build stayed <= 160
    # against the 16 KB task-19 stack the margin is the same
    # a packed set with one entry a byte short: S applies both, H only the valid one (the rebuilt frame)
    pk = kawai(0x10, [0x5F, 0x00, 0x7F, 0x05, 0x51, 0x0B, 0x7F, 0x20, 0x05, 0x56, 0x01, 0x7F, 0x00])
    for s in (S, H):
        s.poke(DEV_ID, b"\x00\x00\x00\x00")
    rs, rh_ = S.send(2, pk), H.send(2, pk)
    s_ap = [a[2] for k, a, _f in rs["log"] if k == "apply"]
    h_ap = [a[2] for k, a, _f in rh_["log"] if k == "apply"]
    h_eb = [a for k, a, _f in rh_["log"] if k == "ebfc"]
    acks = ([a for k, a, _f in rs["log"] if is_ack(k, a)], [a for k, a, _f in rh_["log"] if is_ack(k, a)])
    ok &= check("8 S4: packed 5F 00 (speaker volume 20 + tempo one byte short): the stock applies both (no length check in "
                "its packed loop), H applies the speaker volume only - FUN_4014EBFC gets the frame without the short entry; "
                "same ACK",
                len(s_ap) == 2 and len(h_ap) == 1 and h_ap[0] == s_ap[0] and h_eb
                and h_eb[0][3] == bytes([0x5F, 0x00, 0x7F, 0x05, 0x51, 0x0B, 0x7F, 0x20, 0xF7]) and acks[0] == acks[1],
                f"S {s_ap} H {h_ap} {h_eb and h_eb[0][3].hex()}")
    # switched off: the S3 path (check 2's frames S == H again)
    off = s4_record(H, req=(0x05, 0x06, 0x00))
    for s in (S, H):
        s.poke(DEV_ID, b"\x00\x00\x00\x00")
    eq_off = all(compare(S, H, 2, kawai(0x10, [c["v1"], c["v2"], c["part"]] + c["probe"]), owned=True)[0]
                 for c in cases[:12])
    rec2 = s4_record(H)
    ok &= check("8 S4: op 1B 05 06 00 -> off (flags bit1 0); 12 sets after it: S == H, the S4 counters stand still",
                off is not None and off["rc"] == 0 and not off["flags"] & 0x02 and eq_off and rec2 is not None
                and rec2["sets"] == off["sets"], f"{off} {rec2 and rec2['sets']}")
    S.close(); H.close()
    return ok


def main(argv):
    quiet = "-q" in argv
    img, stock, _manifest, image, syms = rtlib.loader()
    which = rtlib.first_arg(argv, "ota_main")
    p, data, pay, plen, pelf, psyms = rtlib.payload(which)
    session = bool(p.feat2 & images.FEAT2_SESSION)
    gate = bool(p.feat3 & images.FEAT3_GATE)
    ok = True
    print(f"payload {which}  {plen} B  sha256 {pa.sha256(data)}  on {img.name}")

    # ---- 1. static -------------------------------------------------------------------------------
    feat = le(pay, 0x10)
    # the power port's feat1 bits (BT RX log, core, OTA, power)
    POWER_BITS = images.FEAT1_BTLOG | images.FEAT1_CORE | images.FEAT1_OTA | images.FEAT1_POWER
    ok &= check(f"1 {which}.c79 verifies (header, CRC, HMAC); feat {p.feat1:#04x}3B + OTA_EF_ATTACH; superset of the power port's bits",
                not op.verify(data, ro.KEY) and feat == (p.feat0 | p.feat1 << 8 | 0x10000) and p.feat1 & 0x37 == 0x37
                and (p.feat1 & POWER_BITS) == POWER_BITS, f"feat {feat:#x}")
    bad = {n: hex(psyms.get(n, 0)) for n, a in BINDINGS.items() if psyms.get(n) != a}
    ok &= check(f"1 stock_sysex.ld / stock.ld: {len(BINDINGS)} bindings at their documented addresses", not bad, str(bad))

    def w(va):
        return le(stock, va - 0x40060000)

    path = [w(0x4014AA6C + 4 * i) for i in range(14)]
    want_path = [0xE1A00006, rh.bl_word(0x4014AA70, 0xEB000000, 0x40151BCC), 0xE3500000, 0x0A000084, 0xE3A0007F,
                 0xE1A01005, rh.bl_word(0x4014AA84, 0xEB000000, 0x4014BE98), 0xE1A00006,
                 rh.bl_word(0x4014AA8C, 0xEB000000, 0x4015B470), 0xE1A03007, 0xE3A01010, 0xE1A02005,
                 rh.bl_word(0x4014AA9C, 0xEB000000, EBFC), 0xEA00007A]
    ok &= check("1 the stock fn 0x10 path the front replaces: bound FUN_40151BCC -> ACK FUN_4014BE98(0x7F, msg + 6) -> "
                "FUN_4014EBFC(FUN_4015B470(port), 0x10, msg + 6, len); ctx table 0x400A710E = 01 02 03 04; device literal "
                "0x413B5404; ACK key length FUN_4014BB54 = 2 for v1 0x70 else 3",
                path == want_path and stock[0x400A710E - 0x40060000:][:4] == bytes([1, 2, 3, 4])
                and w(0x40147A18) == DEV_ID and w(0x4014BB58) == 0xE35C0070 and w(0x4014BB5C) == 0x13A00003
                and w(0x4014BB60) == 0x03A00002 and w(0x4014BCA8) == BINDING and w(0x40147A14) == FWD,
                str([hex(x) for x in path]) if path != want_path else "")
    lo = psyms["hk_sx_kawai"]
    stub = [i for i in rtlib.instructions(pelf, stop=";@<") if lo <= i[0] < lo + 0x20]
    # bl / blx <imm>: the C bodies are Thumb in a Thumb payload (cave Makefile THUMB=1), reached by the ARMv5 BLX
    shape = [{"blx": "bl"}.get(s[1], s[1]) for s in stub]
    ok &= check("1 hk_sx_kawai: push {r0-r3,ip,lr}; bl(x) pwc_kawai_peek; ldm sp,{r0-r2}; bl(x) sxc_kawai; cmp r0,#0; "
                "pop {r0-r3,ip,lr}; bxne lr; b FUN_4014A2C8",
                shape == ["push", "bl", "ldm", "bl", "cmp", "pop", "bxne", "b"]
                and int(stub[1][2].split()[0], 16) == psyms["pwc_kawai_peek"] and int(stub[3][2].split()[0], 16) == psyms["sxc_kawai"]
                and int(stub[7][2].split()[0], 16) == A2C8, str(shape))

    # ---- machines --------------------------------------------------------------------------------
    global BOOT_ON
    BOOT_ON = p.boot_on
    S, H = Side(image, syms), Side(image, syms, data)
    # boot state of the four runtime switches, then all off: checks 2-6 measure the front alone
    b3 = (front_diag(H, req=(0x05, 0x05)) or {}).get("raw", b"\0\0")[1] & 0x02
    b4 = (front_diag(H, req=(0x05, 0x07)) or {}).get("raw", b"\0\0")[1] & 0x02
    bm = (front_diag(H, req=(0x07,)) or {}).get("raw", b"\0\0")[1] & 0x02
    bg = rp.body_of(H.pn.sysex(2, rp.req(0x1F)), 2)
    bg = rp.unpack7(bg[1][2:])[1] & 0x02 if bg else None
    rc_off = switches_off(H) if BOOT_ON else [0, 0, 0, 0]
    want_b = (2, 2, 2, 2) if BOOT_ON else (0, 0, 0, 0)
    ok &= check(f"1 H boot state: S3 / S4 / MIDI channel / executor gate on bits {(b3, b4, bm, bg)} (want {want_b} - "
                f"{'boot-on payload' if BOOT_ON else 'all off'}); switched off {rc_off}",
                (b3, b4, bm, bg) == want_b and rc_off == [0, 0, 0, 0])
    hk = rh.hooks(H.pn)
    words = rh.words(H.pn)
    # the power sites (+ the pump tail with the session core) + the three MIDI channel sites (the judge switched off keeps
    # them, the stock path) + the fn 0x24 Read site; the executor gate's two sites go with the gate switched off
    n_hk = (5 if session else 4) + (3 if p.feat3 & images.FEAT3_MIDICH else 0) + (1 if "hk_fr_file" in psyms else 0)
    ok &= check(f"1 H: {which} applied live, attach() hooked {n_hk} sites, 0x40147E94 = bl hk_sx_kawai; front ready + hooked "
                "(op 0x1B flags)",
                H.applied and hk["hooked"] == n_hk and words[0] == rh.bl_word(0x40147E94, 0xEB00090B, lo)
                and (front_diag(H) or {}).get("flags") == 0x0F, f"{hk and hk['hooked']} {hex(words[0])}")
    for s in (S, H):
        s.poke(BINDING, b"\x03")                            # the app on BLE (stock port 2) is bound

    stats = dict(frames=0, owned=0, silent=0, passed=0, acks=0, mirrored_expect=0)
    flag_bad, depth = [], []

    def run(port, frame, owned=None, regs=True):
        eq, rs, rh_, det = compare(S, H, port, frame, regs=regs, owned=owned)
        ent = h_entered_a2c8(rh_)
        own = not ent
        stats["frames"] += 1
        if own:
            stats["acks"] += sum(1 for k, a, _f in rh_["log"] if is_ack(k, a))
            if any(k == "ebfc" for k, _a, _f in rh_["log"]):
                stats["owned"] += 1
            else:
                stats["silent"] += 1
            for k, a, f in rh_["log"]:
                if (is_ack(k, a) or k == "ebfc") and f != 1:   # the end handler's own port 3 forwarding is before
                    flag_bad.append((frame.hex(), k, f))
            for k, a, _f in rh_["log"]:
                if k == "ebfc":
                    sd = next(x for x in rs["log"] if x[0] == "ebfc")[1][5]
                    depth.append(sd - a[5])
        else:
            stats["passed"] += 1
        if rh_["inflight"] != 0:
            flag_bad.append((frame.hex(), "after", rh_["inflight"]))
        return eq, own, det, rs, rh_

    # ---- 2. every fn 0x10 descriptor -------------------------------------------------------------
    cases = [c for c in p3.load_cases() if c["fn"] == 0x10]
    fails, owned2, vt2, stopped, gated2 = [], 0, 0, [], 0
    for c in cases:
        frame = kawai(0x10, [c["v1"], c["v2"], c["part"]] + c["probe"])
        want = not vt_prefix(c["v1"], c["v2"])
        dropped = gate and not want and gate_drops(frame, 2, le(H.emu.read_bytes(DEV_ID, 4), 0))
        eq, own, det, rs, rh_ = run(2, frame, owned=want or dropped)
        owned2 += own and want
        vt2 += not want
        gated2 += dropped
        if dropped:
            own = want   # the gate's drop: counted with the VT edits, S == H checked above with S's handler entry aside
        if rs["res"].stop != "returned":
            stopped.append((hex(c["iid"]), rs["res"].stop))
        if not eq or own != want:
            fails.append((hex(c["iid"]), c["name"], det or f"owned {own}"))
    ok &= check(f"2 {len(cases)} fn 0x10 descriptors (stock table 0x400FAA2C, p3_map probe values) on the bound port: "
                f"{owned2} owned by the front, the {vt2} VT edit ones (5A / 50 0C 0E 10 17) passed to the stock; S == H for "
                f"every frame (TX bytes and ports, FUN_4014EBFC and FUN_4016053C arguments, stock RAM stores in order, "
                f"stop, state byte); {len(stopped)} stop inside the stock apply in both"
                + (f"; intake gate: {gated2} VT edit frames with another device byte (check 2's sets move it) dropped "
                   f"before FUN_4014A2C8 - the stock enters it and ignores them" if gate else ""),
                not fails and owned2 + vt2 == len(cases), str(fails[:3]) + f" stopped {stopped}")

    # ---- 3. edge frames --------------------------------------------------------------------------
    apo = [0x51, 0x00, 0x7F, 0x02]
    edge = [
        ("unbound port 1", 1, kawai(0x10, apo), True),
        ("device 0x05 (receive number 0)", 2, kawai(0x10, apo, dev=0x05), True),
        ("device 0x7F", 2, kawai(0x10, apo, dev=0x7F), True),
        ("unknown key 51 7E", 2, kawai(0x10, [0x51, 0x7E, 0x7F, 0x00]), True),
        ("packed 5F 00 x2", 2, kawai(0x10, [0x5F, 0x00, 0x7F, 0x05, 0x51, 0x00, 0x7F, 0x03, 0x05, 0x51, 0x03, 0x7F, 0x40]), True),
        ("length error 51 00 7F 01 01", 2, kawai(0x10, [0x51, 0x00, 0x7F, 0x01, 0x01]), True),
        ("key 70 (2-byte ACK)", 2, kawai(0x10, [0x70, 0x01, 0x7F, 0x00]), True),
        # the end handler appends F7 and counts it: n = the length on the wire; the front owns n >= 10 only
        ("10-byte frame (n = 10, no value)", 2, bytes([0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x51, 0x00, 0x7F, 0xF7]), True),
        ("9-byte frame (n = 9)", 2, bytes([0xF0, 0x40, 0x00, 0x10, 0x08, 0x02, 0x51, 0x00, 0xF7]), False),
        ("VT edit 5A", 2, kawai(0x10, [0x5A, 0x00, 0x7F, 0x00]), False),
        ("VT edit 50 0C", 2, kawai(0x10, [0x50, 0x0C, 0x00, 0x00]), False),
        ("50 0D (an ordinary set)", 2, kawai(0x10, [0x50, 0x0D, 0x00, 0x01]), True),
        ("header 71", 2, bytes([0xF0, 0x40, 0x00, 0x10, 0x71, 0x02, 0x51, 0x00, 0x7F, 0x02, 0xF7]), False),
        ("header 08 03", 2, bytes([0xF0, 0x40, 0x00, 0x10, 0x08, 0x03, 0x51, 0x00, 0x7F, 0x02, 0xF7]), False),
        ("fn 02 request 51 03", 2, kawai(0x02, [0x51, 0x03, 0x7F]), False),
        ("fn 31", 2, kawai(0x31, [0x51, 0x00, 0x7F, 0x00]), False),
        ("fn 32 touch notice", 2, kawai(0x32, [0x51, 0x39, 0x7F, 0x00]), False),
        ("7E 04 41 version", 2, kawai(0x7E, [0x04, 0x41]), False),
    ]
    efail = []
    gated3 = []
    for name, port, frame, want_own in edge:
        if gate and not want_own and gate_drops(frame, port, le(H.emu.read_bytes(DEV_ID, 4), 0)):
            want_own = True   # the intake gate drops what the stock handler would enter only to ignore
            gated3.append(name)
        eq, own, det, rs, rh_ = run(port, frame, owned=want_own)
        if not eq or own != want_own:
            efail.append((name, own, det))
    for s in (S, H):
        s.poke(STAGE, b"\x01")
    eq, own, det, rs, rh_ = run(2, kawai(0x10, [0x51, 0x00, 0x7F, 0x03]), owned=True)
    stage_ok = eq and own and not any(k == "apply" for k, _a, _f in rh_["log"])
    dev_after2 = H.emu.read_bytes(DEV_ID, 4)
    for s in (S, H):
        s.poke(STAGE, b"\x00")
        s.poke(DEV_ID, b"\x00\x00\x00\x00")       # check 2's sets really apply now (frame length fixed): back
        s.poke(BINDING, b"\x04")                      # to receive number 0 for the panel frames (device 0x00)
        s.poke(FWD, b"\x01\x00\x00\x00")
    eq, own, det, rs, rh_ = run(3, kawai(0x10, apo), owned=True)
    fwd_ports = [a[0] for k, a, _f in rh_["log"] if is_ack(k, a)]   # (the end handler forwards the frame itself first)
    fwd_ok = eq and own and fwd_ports == [0, 1, 2, 3]
    for s in (S, H):
        s.poke(FWD, b"\x00\x00\x00\x00")
    eq, own, det2, rs, rh_ = run(3, kawai(0x10, apo), owned=True)
    fwd_ok &= eq and [a[0] for k, a, _f in rh_["log"] if is_ack(k, a)] == [3]
    for s in (S, H):
        s.poke(BINDING, b"\x03")
    ok &= check(f"3 {len(edge)} edge frames: S == H; owned exactly the fn 0x10 sets with header 08 02 outside the VT edit "
                "prefixes (silent ones included), passed frames enter FUN_4014A2C8 with S's r0-r11, sp, lr"
                + (f"; intake gate: {gated3} dropped before FUN_4014A2C8 (the stock enters it and ignores them)" if gate else ""),
                not efail, str(efail[:3]))
    ok &= check("3 staging flag 0x419156A9 = 1: S == H (ACK, FUN_4014EBFC, no apply switch call - the stock gate holds it)",
                stage_ok, det)
    ok &= check("3 panel bound (4) on port 3: forwarding on -> ACK on ports 0, 1, 2, 3 (FUN_4014AD04); off -> port 3 only; "
                "S == H", fwd_ok, f"{fwd_ports} {det2} (device word after check 2: {dev_after2.hex()})")

    # ---- 4. golden trace -------------------------------------------------------------------------
    S.close(); H.close()
    S, H = Side(image, syms), Side(image, syms, data)
    if BOOT_ON:
        assert switches_off(H) == [0, 0, 0, 0]
    if not TRACE.is_file():
        print(f"SKIP 4 golden trace ({TRACE.relative_to(REPO)} absent: a private capture, not in the repository)")
        for s in (S, H):   # the trace's CONNECT would have bound the app on port 2 (check 6 starts from that)
            s.poke(BINDING, b"\x03")
    else:
        frames = trace_frames()
        sets = [f for d, f in frames if d == "PH->PI" and len(f) > 3 and f[1] == 0x40 and f[3] == 0x10]
        real_acks = [f for d, f in frames if d == "PI->PH" and f[:6] == bytes.fromhex("F0407F07041E")]
        tfail, my_acks, n_ph, n_kawai, n_sess, n_cmd = [], [], 0, 0, 0, 0
        for d, f in frames:
            if d != "PH->PI":
                continue
            n_ph += 1
            n_kawai += f[1] == 0x40          # the BLE module's own F0 00 02 08 frames never reach the Kawai site
            # a session payload runs the bound controllers' fn 0x31 / 0x32 commands itself through the same stock apply
            # FUN_4014EBFC: S's entry into FUN_4014A2C8 is the only
            # difference, as for an owned set
            cmd = (session and f[1] == 0x40 and f[3] in (0x31, 0x32) and len(f) >= 10 and f[4:6] == b"\x08\x02"
                   and not vt_prefix(f[6], f[7]))
            n_cmd += bool(cmd)
            eq, own, det, rs, rh_ = run(2, f, owned=(f[1] == 0x40 and f[3] == 0x10) or cmd)
            if session and f[1] == 0x40 and f[3] in SESSION_FNS:
                # a session payload (ota_main): the core answers the request itself and takes the ACKs of its own
                # values - by design, and S's request task is stubbed here; S == H for these is roundtrip_session.py 12
                n_sess += 1
                continue
            if not eq:
                tfail.append((f.hex(), det))
            if f[1] == 0x40 and f[3] == 0x10:
                my_acks += [a[1] for k, a, _f in rh_["log"] if is_ack(k, a)]
        bound = H.emu.read_bytes(BINDING, 1)[0]
        ok &= check(f"4 golden trace: {n_ph} phone -> piano frames in order from unbound (CONNECT, identity, full request, "
                    f"{len(sets)} sets, ACKs): S == H for every frame"
                    f"{f' but the {n_sess} the session core takes (request, ACKs of its values)' if session else ''}"
                    f"{f'; its {n_cmd} fn 0x31 / 0x32 commands run through FUN_4014EBFC without FUN_4014A2C8' if session else ''}; "
                    "binding after = 3 in both",
                    not tfail and bound == 3 and S.emu.read_bytes(BINDING, 1)[0] == 3, str(tfail[:2]))
        ok &= check(f"4 H's ACK for each of the {len(sets)} sets == the real piano's ACK in the capture "
                    f"({len(real_acks)} PI->PH 07 frames)", my_acks == real_acks and len(my_acks) == len(sets),
                    f"{len(my_acks)} vs {len(real_acks)}")

    # ---- 5. contract (b), stack, stores ----------------------------------------------------------
    ok &= check("5 inflight (resident ota_in_sysex) = 1 at every TX writer / FUN_4014EBFC call of the front, 0 after every "
                f"frame ({stats['owned']} owned, {stats['silent']} silent, {stats['passed']} passed, {stats['acks']} ACK frames)",
                not flag_bad and stats["owned"] > 0, str(flag_bad[:3]))
    extra = sorted(set(depth))
    ok &= check(f"5 stack at FUN_4014EBFC: H uses {extra} B more than S (stock FUN_4014A2C8 frame 48 B; task 19 stack 0x4000)",
                depth and max(depth) <= 128, str(extra))
    okw = [(RUN, RUN + 0x20000), (0x40180000, 0x401A0000), ro.STACK, (0x40B90000, 0x40B90200),
           (syms["ota_in_sysex"], syms["ota_in_sysex"] + 1)]
    mine = [w_ for w_ in H.pn.writes if RUN <= w_.pc < RUN + plen]
    stray = [(hex(w_.addr), hex(w_.pc)) for w_ in mine if not any(a <= w_.addr < b for a, b in okw)]
    ok &= check(f"5 {len(mine)} stores by payload code: only payload .data, core RW, stack and the inflight flag",
                mine and not stray, str(stray[:3]))

    # ---- 6. op 0x1B ------------------------------------------------------------------------------
    d = front_diag(H)
    if not TRACE.is_file():
        print("SKIP 6 op 1B 00 counters after the golden trace (check 4 skipped: capture absent)")
    else:
        exp_owned = len(sets)
        n_front = n_kawai - n_sess - n_cmd   # a session payload: the frames its core takes never reach the front
        ok &= check(f"6 op 1B 00 (trace machine): seen {d and d['seen']} = {n_front} Kawai frames, owned {d and d['owned']} = {exp_owned} sets, "
                    f"mirrored {d and d['mirrored']}, unknown {d and d['unknown']}, ACK frames {d and d['acks']}; last key 56 03 7F",
                    d is not None and d["rc"] == 0 and d["owned"] == exp_owned and d["seen"] == n_front
                    and d["acks"] == exp_owned and d["key"] == bytes([0x56, 0x03, 0x7F])
                    and d["mirrored"] + d["unknown"] == exp_owned and d["flags"] == 0x0F, str(d and {k: v for k, v in d.items() if k != "raw"}))
    r1 = front_diag(H, port=1, req=(0x01, 0x00))
    r2 = front_diag(H, port=2, req=(0x01, 0x00))
    eq, own, det, rs, rh_ = run(2, kawai(0x10, apo), owned=False)
    off_ok = eq and not own
    r3 = front_diag(H, port=2, req=(0x01, 0x01))
    eq, own, det, rs, rh_ = run(2, kawai(0x10, apo), owned=True)
    ok &= check("6 1B 01 00 from unbound port 1 -> rc 2 (still on); from the bound port -> off: a set enters FUN_4014A2C8, "
                "S == H; 1B 01 01 -> on again, owned",
                r1 and r1["rc"] == 2 and r1["flags"] & 1 and r2 and r2["rc"] == 0 and not r2["flags"] & 1 and off_ok
                and r3 and r3["rc"] == 0 and r3["flags"] & 1 and eq and own, f"{r1 and r1['rc']} {r2 and r2['rc']} {off_ok}")
    S.close(); H.close()

    # ---- 8. S4 write owner ----------------------------------------------
    if p.feat3 & images.FEAT3_S4:
        ok &= s4_checks(image, syms, data)

    # ---- 7. field failure replay: full sync stop-and-wait (real FUN_4014BD1C / FUN_4014BCAC) -------
    ok &= check("7 stock FUN_4014BCAC: deadline = (FUN_400AB2E8() + 250) & 0xFFFF (lsl/lsr #16), `cmp r4, r0; bgt` - "
                "no wrap handling; FUN_400AB2E8 = ldrh 0x4139E796 (2 ms counter); FUN_4014BD1C clears the ACK/NAK/hold "
                "flags once (bl FUN_4014BC88) before its 16 sends, dly_tsk(5) between",
                w(0x4014BCCC) == 0xE1A0480C and w(0x4014BCD4) == 0xE1A04824 and w(0x4014BD04) == 0xE1540000
                and w(0x4014BD10) == 0xCAFFFFF1 and w(0x400AB2EC) == 0xE1DC00B0 and w(0x400AB3D8) == TICK16
                and w(0x4014BD3C) == rh.bl_word(0x4014BD3C, 0xEB000000, 0x4014BC88) and w(0x4014BD40) == 0xE3A07010
                and w(0x4014BD48) == 0xE3A00005)
    if not FAIL_LOG.is_file():
        print(f"SKIP 7 field log replay ({FAIL_LOG.relative_to(REPO)} absent: a private capture, not in the repository)")
    else:
        frames, log_multi = log_dump()
        k38 = bytes.fromhex("550438")
        far = dump_replay(image, syms, data, frames, 1000)
        fs, fh = far
        d7 = front_diag(fh["side"])
        acks_far = len(fh["rp"].acks_in)
        ok &= check(f"7 field log replay, counter far from the wrap: the {len(frames)} logged dump values (51 0F 7F .. 55 04 3B) "
                    f"each sent once, ACKed through the end handler, FUN_4014BD1C 0 every time, binding stays 3; S == H "
                    f"(send times, keys, results); H's front saw the {acks_far} ACKs and passed every one (owned 0)",
                    fs["sends"] == fh["sends"] and fs["rcs"] == fh["rcs"] and len(fs["sends"]) == len(frames)
                    and all(r == 0 for r in fs["rcs"]) and fs["binding"] == fh["binding"] == 3
                    and d7 and d7["seen"] == d7["passed"] == acks_far and d7["owned"] == 0 and d7["silent"] == 0,
                    str(d7 and {k: v for k, v in d7.items() if k != "raw"}))
        t38 = next(t for t, k in fs["sends"] if k == k38)
        for o in far:
            o["side"].close()
        tick0 = (WRAP_FROM - t38 // 2000) & 0xFFFF
        wrap = dump_replay(image, syms, data, frames, tick0)
        ws, wh = wrap
        multi = {frames[i][6:9].hex(): n for i, n in enumerate(ws["per"]) if n > 1}
        last = ws["sends"][-1][1]
        ok &= check(f"7 same replay, the 2 ms counter at {WRAP_FROM:#06x} when 55 04 38 goes out (as in the log): every wait "
                    f"times out at once (FUN_4014BCAC 2, NAK flag 0) until an ACK is already in, values resent every ~5 ms - "
                    f"sent {multi} (log {log_multi}); {last.hex()} sent 16 times, FUN_4014BD1C -1, binding 3 -> 0 "
                    f"(0x413CA3DF = 3) - the field failure; S == H",
                    ws["sends"] == wh["sends"] and ws["rcs"] == wh["rcs"] and ws["waits"] == wh["waits"]
                    and ws["rcs"][-1] == -1 and ws["per"][-1] == 16 and 2 in ws["waits"] and 1 not in ws["waits"]
                    and ws["nak"] == 0 and ws["binding"] == wh["binding"] == 0 and ws["prev"] == wh["prev"] == 3
                    and multi.get(k38.hex(), 1) > 1 and set(multi) <= set(log_multi), f"rcs {ws['rcs'][-3:]} binding {ws['binding']} waits {set(ws['waits'])}")
        rep = []
        for o in wrap:
            sd = o["side"]
            r0 = sd.send(2, kawai(0x7E, [0x01, 0x00], dev=0x7F))
            r1_ = sd.send(2, kawai(0x7E, [0x01, 0x01], dev=0x7F))
            rep.append(([a[1] for k, a, _f in r0["log"] if k == "tx"], [(a[0], a[1]) for k, a, _f in r1_["log"] if k == "tx"],
                        sd.emu.read_bytes(BINDING, 1)[0]))
            sd.close()
        ok &= check("7 after that unbind, the app's 7E 01 00 then 7E 01 01 through the end handler: no reply to the first, "
                    "CONNECT answered F0 40 7F 7E 04 1E 02 00 40 F7 on port 2 and binding 3 again; S == H - the piano side "
                    "takes a CONNECT, so the field silence came before the handler (one of each pair never reached it: op 1B "
                    "seen +1 per pair)",
                    rep[0] == rep[1] and rep[0][0] == [] and rep[0][1] == [(2, bytes.fromhex("F0407F7E041E020040F7"))]
                    and rep[0][2] == 3, str(rep))

    return rtlib.finish(ok, " - emulation only: RTOS stubbed (queue post / semaphores / delay / ACK waits; 7 runs the "
                        "fn 0x10 wait against a modelled clock and link), no other task runs.")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
