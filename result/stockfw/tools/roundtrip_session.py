# roundtrip_session.py - virtual-time session harness:
# the stock request task, its reliable sender and task 19 run for real against a 2 ms clock and two modelled
# controllers, the phone app on BLE (stock port 2) and the built-in panel (port 3).  First use: the second
# ota_sysex field failure (2026-10-31).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_session.py [ota_main] [-q] [--t] [--shard i/n]
#
# Runs on the session payload named (default and only one built: ota_main, the all-features slot payload).
#
# Machines as in roundtrip_sysex.py: S = ota_loader_v4 built-in cave (stock decides), H = the payload applied live.
# What runs:
#   task 19   the real Kawai end handler 0x40147DB0 for every controller frame, on its own stack, preempting the
#             request task when a frame falls due (at a 2 ms counter read or a dly_tsk)
#   task 28   the real request task FUN_40152AD8: rcv_dtq 0x40082F94 is fed from the posts of FUN_4014FD94
#             (fn 0x02, busy byte 0x413CA3E0), the reliable sender FUN_4014BD1C and its wait FUN_4014BCAC run
#             unstubbed; the loop's own `strb 0, busy` and remote-change clear run too
#   clock     2 ms counter 0x4139E796 = t / 2 ms (far from the 16-bit wrap: that one is roundtrip_sysex 7)
#   app       port 2: ACKs every piano fn 0x10 / 0x03 (session.ts), link as roundtrip_sysex.DumpReplay [inference]
#   panel     port 3: ACKs every piano fn 0x10 / 0x03 after 2 ms, UART 0.32 ms/B each way [inference]
# Semaphores stay stubbed (one sender at a time here); the engine loop (task 24) does not run.
#
# Scenario = the field order: app CONNECT + identity + full dump -> 13 reverb sets -> get
# 02 52 0D 7F while bound; then the panel takes the binding (CONNECT + identity + full request, the overlay-tap
# path of the original panel app) -> the app's get 52 0D -> the app's manual CONNECT -> set 55 00 00.
# (Checks 0-6 ran it with S == H on the SysEx-only payload ota_sysex - the second field failure;
# retired with that payload 2026-11-01.  Check 11 runs it on H.)
#
# Phase 3-2 stage 1 - the core's session table, CONNECT
# answers, request answers on its own sender and the ACK / Wait assignment, with the pump tail 0x401539AC called
# every 2 ms (H: hk_ss_midi -> ssc_pump):
#   10 the payload applied: identify with feat2 0x02 set (ota_main 0x03), op 0x1B 03 flags (core on, pump hooked, ready)
#   11 the field order above on H: nobody displaced, the app's get answered, the app's CONNECT answered
#   T1-T11 on S and H from fresh machines, printed as a table; T12 is Phase 3-4:
#      T1 dump, ACK after 20 / 80 / 300 ms: H == S bytes and order   T2 one ACK 700 ms late: resent once in both
#      T3 no answer: 16 sends, unbind (H also invites the panel)       T4 panel, no answer: 16, never unbound
#      T5 the 16-bit wrap window: S bursts 16 sends and unbinds, H sends every value once (the stage-1 gain)
#      T6 Wait: S's request task spins for ever holding 0x413CA388; H extends the try, the dump ends
#      T7 Sound / Metronome / SongSelect sets during a dump: no reliable send from task 19, dump intact
#      T8 Cancel: stops before the next value, no End                T9 another port's CONNECT / link down mid-dump
#      T10 the reliable sender's semaphore: held per value in S, never taken by the core sender
#      T11 panel + app both bound: per-port ACKs and dumps            12 the PianoRemote golden trace: S == H
#   (`--t` runs only 10 / T / 12.  `--shard i/n`: the i-th of n cost-balanced groups of the units 11, 10, T1.20 ...
#   12 - rtlib.Unit; each unit runs on fresh machines forked from one S and one H build.)
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rs = rtlib.load("roundtrip_sysex")
p3, rt, ro, pa, images = rs.p3, rs.rt, rs.ro, rs.pa, rs.images
check, SE, kawai = ro.check, rs.SE, rs.kawai
STOCKFW = rs.STOCKFW

REQ_TASK, RCV_DTQ, REQ_DTQ, REQ_BUSY = 0x40152AD8, 0x40082F94, 0x413CA378, 0x413CA3E0
REMOTE_FLAG = 0x413CA3B0
PUMP_SITE, SPIN_AT, SPIN_MAX, SEM_REL = 0x401539AC, 0x4014BCE8, 200_000, 0x413CA388
le = ro.le
APP, PANEL = 2, 3
PIANO_HEAD = bytes.fromhex("F0407F")
# the field's 13 sets: reverb on/off x2, type and back, Cathedral, then 8 type changes
SETS = ([[0x55, 0x07, 0x00, v] for v in (0, 1)] + [[0x55, 0x08, 0x00, v] for v in (4, 0, 4)]
        + [[0x55, 0x08, 0x00, (0, 4)[i & 1]] for i in range(8)])
IDENTITY = [[0x7E, 0x05, 0x40], [0x7E, 0x05, 0x41], [0x7E, 0x05, 0x42], [0x7E, 0x04, 0x41]]


def ctl(fn, body, dev=0x7F):
    """A controller frame as the app / panel send it (device 7F, header 08 02)."""
    return kawai(fn, body, dev=dev)


class Net:
    """One side (S or H) with the virtual clock, the two controller models and the request task."""

    UART_B, EVENT, PANEL_THINK = 320, 15_000, 2_000          # us

    def __init__(self, side, tick0=1000, pump=False):
        self.s, self.e = side, side.emu
        self.t, self.tick0 = 0, tick0
        self.pending = []                                  # (t, port, frame) controller -> piano
        self.tx = []                                       # (t, port, frame) piano -> controller
        self.reqq, self.other_posts, self.dropped_req = [], [], 0
        self.in_isr = self.in_task = False
        self.up_free = {APP: 0, PANEL: 0}
        self.down_free = {APP: 0, PANEL: 0}
        self.ack = {APP: True, PANEL: True}
        self.policy = {}                                   # port -> fn(net, frame, k) -> [(delay us, frame)] | None
        self.fixed = {}                                    # port -> fixed ACK delay (us) instead of the link model
        self.nsent = {}                                    # port -> piano frames that asked for an ACK
        self.sem = []                                      # (t, "take" | "give") of FUN_4014BD1C's semaphore
        self.spins, self.stuck, self.isr_bd1c = 0, False, 0
        self.pump_fn = self.next_pump = None
        self.events = []                                   # (t, fn(net)) run in task 19 / 24 order at t
        e = self.e
        e.unhook(p3.ACK_WAIT_10)                           # the real FUN_4014BCAC
        e.hook(rs.TIME16, self._time)
        e.hook(p3.DELAY, self._delay)
        e.hook(p3.Q_POST, self._post)
        e.hook(RCV_DTQ, self._rcv)
        e.hook(p3.SEM_TAKE, lambda em: self._semop(em, "take"))
        e.hook(p3.SEM_GIVE, lambda em: self._semop(em, "give"))
        e.hook(SPIN_AT, self._spin)
        e.hook(rs.SEND10, lambda em: (self._bd1c(), SE.CONTINUE)[1])
        for p in rt.TX:
            e.hook(rt.TX[p], (lambda pp: lambda em: self._tx(em, pp))(p))
        if pump:   # task 19's pump tail every 2 ms: whatever the bl at 0x401539AC calls now (H: hk_ss_midi)
            w = le(e.read_bytes(PUMP_SITE, 4))
            off = w & 0xFFFFFF
            self.pump_fn = (PUMP_SITE + 8 + ((off - (1 << 24) if off & 0x800000 else off) << 2)) & 0xFFFFFFFF
            self.next_pump = 2000
        self._sync()

    # ---- clock ----
    def tick(self):
        return (self.tick0 + self.t // 2000) & 0xFFFF

    def _sync(self):
        self.e.write_bytes(rs.TICK16, self.tick().to_bytes(2, "little"))

    def _due(self):
        return self.in_task and not self.in_isr and (
            (self.pending and self.pending[0][0] <= self.t) or (self.events and self.events[0][0] <= self.t))

    def at(self, ms_from_now, fn):
        """Run fn(net) in its own context (another task / the BT link) ms from now."""
        self.events.append((self.t + int(ms_from_now * 1000), fn))
        self.events.sort(key=lambda x: x[0])

    def _horizon(self):
        """The next time a frame or an event falls due (the poll skip stops there)."""
        h = self.pending[0][0] if self.pending else 1 << 62
        return min(h, self.events[0][0]) if self.events else h

    def _time(self, em):
        # 10 us per counter read; FUN_4014BCAC's idle passes up to the next tick / frame skipped (rtlib.poll_step)
        self.t += rtlib.poll_step(em, self.t, self._horizon(), lambda t: (self.tick0 + t // 2000) & 0xFFFF)
        self.spins = 0
        self._sync()
        if self._due():
            em.stop("preempt")
        return self.tick()

    def _delay(self, em):
        self.t += em.reg(0) * 1000
        self._sync()
        if self._due():
            em.stop("preempt")
        return 0

    # ---- RTOS ----
    def _semop(self, em, op):
        if em.reg(0) == em.read_u32(SEM_REL):
            self.sem.append((self.t, op))
        return 0

    def _spin(self, em):
        """0x4014BCE8 `bne` of FUN_4014BCAC's Wait loop: past SPIN_MAX passes the sender never leaves it."""
        self.spins += 1
        if self.spins > SPIN_MAX:
            self.stuck = True
            em.stop("stuck")
        return SE.CONTINUE

    def _bd1c(self):
        if self.in_isr:
            self.isr_bd1c += 1

    def _post(self, em):
        q, m = em.reg(0), em.reg(1)
        if q == em.read_u32(REQ_DTQ):
            self.reqq.append(m)
        else:
            self.other_posts.append((self.t, q, m))
        return 0

    def _rcv(self, em):
        if not self.reqq:
            em.stop("idle")
            return 0
        em.write_u32(em.reg(1), self.reqq.pop(0))
        return 0

    # ---- links ----
    def _tx(self, em, port):
        f = bytes(em.read_bytes(em.reg(0), em.reg(1)))
        self.s._ev("tx", port, f)
        self.tx.append((self.t, port, f))
        if port in self.ack and self.ack[port] and f[:3] == PIANO_HEAD and f[3] in (0x10, 0x03) and f[4:6] == b"\x04\x1E":
            k = self.nsent.get(port, 0)
            self.nsent[port] = k + 1
            ack = ctl(0x07, f[6:8] if f[6] == 0x70 else f[6:9])
            if port in self.fixed:
                self.down_free[port] = max(self.down_free[port], self.t) + len(f) * self.UART_B
                at = self.down_free[port] + self.fixed[port]
            elif port == APP:   # piano -> module UART, seen at the next connection event, one app write per event
                self.down_free[port] = max(self.down_free[port], self.t) + len(f) * self.UART_B
                seen = -(-self.down_free[port] // self.EVENT) * self.EVENT
                self.up_free[port] = max(seen + self.EVENT, self.up_free[port] + self.EVENT)
                at = self.up_free[port] + len(ack) * self.UART_B
            else:             # panel: UART both ways, the panel answers after PANEL_THINK
                self.down_free[port] = max(self.down_free[port], self.t) + len(f) * self.UART_B
                at = self.up_free[port] = max(self.down_free[port] + self.PANEL_THINK,
                                              self.up_free[port]) + len(ack) * self.UART_B
            pol = self.policy.get(port)
            plan = pol(self, f, k) if pol else None
            for d, r in ([(0, ack)] if plan is None else plan):
                self.pending.append((at + d, port, r))
            self.pending.sort(key=lambda x: x[0])
        return None

    def send(self, port, frame):
        """A controller frame put on the wire now."""
        at = max(self.t, self.up_free[port]) + len(frame) * self.UART_B
        self.up_free[port] = at
        self.pending.append((at, port, frame))
        self.pending.sort(key=lambda x: x[0])

    # ---- tasks ----
    def _deliver_due(self):
        while self.events and self.events[0][0] <= self.t:
            _at, fn = self.events.pop(0)
            self.in_isr = True
            try:
                fn(self)
            finally:
                self.in_isr = False
        while self.pending and self.pending[0][0] <= self.t:
            _at, port, f = self.pending.pop(0)
            self.in_isr = True
            try:
                # the byte parser stores data bytes only and calls the end handler on the F7
                # (0x40148CA4..0x40148CC4), which appends F7 and counts it: n = wire length
                r = self.s.send(port, f[:-1] if f[-1:] == b"\xF7" else f, sp=rs.ISR_SP)
            finally:
                self.in_isr = False
            if r["res"].stop != "returned":
                raise RuntimeError(f"end handler stopped: {r['res']} on {f.hex()}")

    def _run_task(self):
        e = self.e
        self.in_task = True
        try:
            res = e.call(REQ_TASK, max_insns=200_000_000)
            while res.stop == "preempt":
                regs = {n: res.regs[i] for i, n in enumerate(
                    ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr"])}
                self._deliver_due()
                res = e.call(res.regs[15], regs=regs, max_insns=200_000_000)
        finally:
            self.in_task = False
        if res.stop == "stuck":
            return                                         # T6: the sender spins on its Wait flag for ever
        if res.stop != "idle":
            raise RuntimeError(f"request task stopped: {res}")

    def run(self, ms):
        """Let ms of virtual time pass: task 19 deliveries first, then the request task when it has work."""
        end = self.t + int(ms * 1000)
        while self.t < end and not self.stuck:
            self._deliver_due()
            if self.reqq:
                self._run_task()
                continue
            nxt = self.pending[0][0] if self.pending else end
            if self.events:
                nxt = min(nxt, self.events[0][0])
            if self.pump_fn is not None:
                nxt = min(nxt, self.next_pump)
            self.t = max(self.t + 100, min(nxt, end))
            self._sync()
            if self.pump_fn is not None and self.t >= self.next_pump:
                self.next_pump = self.t - self.t % 2000 + 2000
                r = self.e.call(self.pump_fn, regs={"sp": rs.ISR_SP}, max_insns=20_000_000)
                if r.stop != "returned":
                    raise RuntimeError(f"pump tail stopped: {r}")
        if not self.stuck:
            self._deliver_due()

    def until(self, pred, ms=20_000):
        """Run until pred(new TX list) holds (checked every 1 ms), at most ms."""
        n0 = len(self.tx)
        for _ in range(int(ms)):
            if pred(self.tx[n0:]):
                return True
            self.run(1)
        return pred(self.tx[n0:])

    def b(self, addr):
        return self.e.read_bytes(addr, 1)[0]


def has(port, head):
    head = bytes.fromhex(head)
    return lambda tx: any(p == port and f.startswith(head) for _t, p, f in tx)


def session_diag(side):
    """op 0x1B 02 (the SysEx front): the stock session globals and the front's per-port counts."""
    out = side.pn.sysex(APP, rs.rp.req(rs.OP_FRONT, 0x02))
    b = rs.rp.body_of(out, APP)
    if not b or b[0] != rs.OP_FRONT | 0x40 or b[1][:2] != b"\x02\x00":
        return None
    d = rs.rp.unpack7(b[1][2:])
    le = rs.le
    return dict(layout=d[0], binding=d[1], prev=d[2], busy=d[3], remote=bytes(d[4:8]), flags=bytes(d[8:12]),
                tick=d[12] | d[13] << 8, seen=[le(d, 16 + 4 * i) for i in range(4)],
                connects=[d[32 + 2 * i] | d[33 + 2 * i] << 8 for i in range(4)], disconnects=list(d[40:44]),
                last_connect=d[44], binding_at=d[45])


def connect(n, port, full=True):
    """CONNECT + identity (+ full request), each after the previous reply, as the app (session.ts) and the panel
    (connectPiano / overlay tap) do; returns the CONNECT reply."""
    n0 = len(n.tx)
    n.send(port, ctl(0x7E, [0x01, 0x01]))
    ok = n.until(has(port, "F0407F7E041E02"))
    reply = next((f for _t, p, f in n.tx[n0:] if p == port and f[3:7] == bytes.fromhex("7E041E02")), None)
    for q in IDENTITY:
        n.send(port, ctl(q[0], q[1:]))
        n.until(has(port, "F0407F7E041E" + bytes(q[1:2]).hex() if q[1] == 0x05 else "F0407F7E041E04"), 3000)
    if full:
        n.send(port, ctl(0x02, [0x5F, 0x7F, 0x7F]))
        ok &= n.until(has(port, "F0407F03041E5F7F7F"), 60_000)
        n.run(200)
    return ok, reply


def scenario(side, pump=False):
    n = Net(side, pump=pump)
    out = {}
    diag = (lambda: rs.front_diag(side)) if side.applied else (lambda: None)

    # A. the app connects and syncs (field step 2)
    ok, rep = connect(n, APP)
    vals = [f for _t, p, f in n.tx if p == APP and f[:6] == bytes.fromhex("F0407F10041E")]
    out["A"] = dict(ok=ok, reply=rep, values=len(vals), distinct=len(set(vals)), binding=n.b(rs.BINDING),
                    busy=n.b(REQ_BUSY), diag=diag())

    # C. get 52 0D while bound (the Concert Magic screen's request; before the sets: an answered request clears
    #    the bound port's remote-change flag, 0x40152B5C)
    n0 = len(n.tx)
    n.send(APP, ctl(0x02, [0x52, 0x0D, 0x7F]))
    n.until(has(APP, "F0407F03041E520D7F"), 3000)
    n.run(300)
    out["C"] = dict(tx=[(p, f) for _t, p, f in n.tx[n0:]], busy=n.b(REQ_BUSY), binding=n.b(rs.BINDING))

    # B. 13 sets, each after the previous ACK (field steps 3-4; field order: the get came after them, unanswered)
    acks = 0
    for s_ in SETS:
        n0 = len(n.tx)
        n.send(APP, ctl(0x10, s_))
        n.until(has(APP, "F0407F07041E" + bytes(s_[:3]).hex()), 3000)
        acks += sum(1 for _t, p, f in n.tx[n0:] if p == APP and f[:6] == rs.ACK_HEAD)
        n.run(50)
    out["B"] = dict(acks=acks, flag=n.e.read_bytes(REMOTE_FLAG, 4), posts=list(n.other_posts), diag=diag())

    # D. the panel takes the binding: CONNECT + identity + full request
    d0 = diag()
    n0 = len(n.tx)
    ok, rep = connect(n, PANEL)
    tx = n.tx[n0:]
    out["D"] = dict(ok=ok, reply=rep, to_app=[f for _t, p, f in tx if p == APP],
                    panel_values=sum(1 for _t, p, f in tx if p == PANEL and f[:6] == bytes.fromhex("F0407F10041E")),
                    binding=n.b(rs.BINDING), busy=n.b(REQ_BUSY), d0=d0, d1=diag(),
                    sess=session_diag(side) if side.applied else None)

    # E. the app's get 52 0D from the unbound port (field t = 1743.6)
    n0, q0 = len(n.tx), len(n.reqq)
    n.send(APP, ctl(0x02, [0x52, 0x0D, 0x7F]))
    n.run(3000)
    out["E"] = dict(tx=[(p, f) for _t, p, f in n.tx[n0:]], diag=diag())

    # F. the app's manual CONNECT (field t = 1821), then set 55 00 00 (field step 7)
    n0 = len(n.tx)
    n.send(APP, ctl(0x7E, [0x01, 0x01]))
    n.until(has(APP, "F0407F7E041E02"), 3000)
    n.run(200)
    out["F"] = dict(tx=[(p, f) for _t, p, f in n.tx[n0:]], binding=n.b(rs.BINDING),
                    sess=session_diag(side) if side.applied else None)
    n0 = len(n.tx)
    n.send(APP, ctl(0x10, [0x55, 0x00, 0x00, 0x00]))
    n.until(has(APP, "F0407F07041E550000"), 3000)
    out["G"] = dict(tx=[(p, f) for _t, p, f in n.tx[n0:]], diag=diag())
    # contrast: CONNECT again from the port that is still bound, its remote-change flag set by a reverb set
    n.send(APP, ctl(0x10, SETS[0]))
    n.until(has(APP, "F0407F07041E" + bytes(SETS[0][:3]).hex()), 3000)
    out["G"]["flag"] = n.e.read_bytes(REMOTE_FLAG, 4)
    n0 = len(n.tx)
    n.send(APP, ctl(0x7E, [0x01, 0x01]))
    n.until(has(APP, "F0407F7E041E02"), 3000)
    out["G2"] = dict(tx=[(p, f) for _t, p, f in n.tx[n0:]])
    out["timeline"] = list(n.tx)
    return out


H_NAME = "ota_main"   # the session payload (argv)


def main(argv):
    global H_NAME
    H_NAME = rtlib.first_arg(argv, H_NAME)
    if not images.OTA_PAYLOADS_BY_NAME[H_NAME].feat2 & images.FEAT2_SESSION:
        raise SystemExit(f"{H_NAME}: no session core (feat2 0x02)")
    _img, _stock, _manifest, image, syms = rtlib.loader()
    data = (STOCKFW / "build" / f"{H_NAME}.c79").read_bytes()
    rtlib.builders(S=lambda: rs.Side(image, syms, None), H=lambda: rs.Side(image, syms, data))
    units = session_units(image, syms)
    if "--t" not in argv:
        units = [rtlib.Unit("11", lambda: (field_session(image, syms), []), 6.8, ("H",))] + units
    ok = session_part(units, argv, quiet="-q" in argv)
    return rtlib.finish(ok, " - emulation: task 19 and the request task run for real on a virtual 2 ms clock, "
                        "semaphores recorded, engine loop not run; link and panel timing are models."
                        + rtlib.shard_note(units, argv))


# ==== Phase 3-2: T1-T12 on S and on the session payload ===========================
# S = the stock state machine (the regression base of 3-2), H = the session payload applied live with its pump tail.
# Every run starts from a fresh machine: the app (port 2) CONNECTs (+ identity), then the test's steps.

FULL, GROUP = [0x5F, 0x7F, 0x7F], [0x55, 0x7F, 0x7F]    # full dump, the 55 group (reverb / sound, 12 values)
VALUE_HEAD, END_HEAD = bytes.fromhex("F0407F10041E"), bytes.fromhex("F0407F03041E")
INVITE, DISPLACE = bytes.fromhex("F0407F7E041E0101F7"), bytes.fromhex("F0407F7E041E0100F7")
LINK_DOWN = 0x4015B494
CTX_SP = 0x40F40000                                      # stack of the injected "task 24" calls


def vals(tx, port):
    return [f for _t, p, f in tx if p == port and f.startswith(VALUE_HEAD)]


def ends(tx, port):
    return [f for _t, p, f in tx if p == port and f.startswith(END_HEAD)]


def sends_per_frame(tx, port):
    out = {}
    for _t, p, f in tx:
        if p == port and f.startswith(VALUE_HEAD):
            out[f.hex()] = out.get(f.hex(), 0) + 1     # a whole frame: one key can carry several values
    return out


def core_diag(side, port=APP):
    """op 0x1B 03 (the session core): the session core's record."""
    out = side.pn.sysex(port, rs.rp.req(rs.OP_FRONT, 0x03))
    b = rs.rp.body_of(out, port)
    if not b or b[0] != rs.OP_FRONT | 0x40 or b[1][:2] != b"\x03\x00":
        return None
    d = rs.rp.unpack7(b[1][2:])
    names = ["requests", "dropped", "values", "ends", "resends", "timeouts", "naks", "waits", "acks", "to_stock",
             "ack_drop", "wait_drop", "failed"]
    r = dict(layout=d[0], flags=d[1], bound=d[2], binding=d[3], ports=list(d[4:8]), pumps=d[8] | d[9] << 8,
             conn_own=d[36], conn_pass=d[37], disc_own=d[38], unbinds=d[39], now=le(d, 40), cancels=d[44])
    for i, k in enumerate(names):
        r[k] = d[10 + 2 * i] | d[11 + 2 * i] << 8
    return r


class Machines:
    def __init__(self, image, syms, data):
        self.image, self.syms, self.data = image, syms, data

    def net(self, kind, tick0=1000):
        side = rtlib.fresh(kind) if kind in rtlib._BUILDERS else rs.Side(self.image, self.syms,
                                                                         self.data if kind == "H" else None)
        return Net(side, tick0=tick0, pump=kind == "H")


def start(n, panel=False):
    connect(n, APP, full=False)
    if panel:
        connect(n, PANEL, full=False)
    n.run(50)


def request(n, port, body, until_ms=60_000):
    n0 = len(n.tx)
    n.send(port, ctl(0x02, body))
    n.until(lambda tx: any(p == port and f.startswith(END_HEAD + bytes(body[:2])) for _t, p, f in tx), until_ms)
    n.run(200)
    return n.tx[n0:]


def result(n, tx, **kw):
    e = n.e
    d = dict(tx=tx, binding=n.b(rs.BINDING), stuck=n.stuck, isr_bd1c=n.isr_bd1c, sem=list(n.sem), **kw)
    d["core"] = core_diag(n.s) if n.pump_fn is not None and not n.stuck else None
    n.s.close()
    del e
    return d


def t1(m, kind, delay_ms, body):
    n = m.net(kind)
    start(n)
    n.fixed[APP] = delay_ms * 1000
    tx = request(n, APP, body, until_ms=max(60_000, 400 * (delay_ms + 30)))
    return result(n, tx)


def t2(m, kind):
    n = m.net(kind)
    start(n)
    n.fixed[APP] = 20_000
    n.policy[APP] = lambda net, f, k: [(700_000, ctl(0x07, f[6:9]))] if k == 3 else None
    return result(n, request(n, APP, GROUP))


def t3(m, kind):
    n = m.net(kind)
    start(n)
    n.ack[APP] = False
    n0 = len(n.tx)
    n.send(APP, ctl(0x02, GROUP))
    n.run(10_000)
    return result(n, n.tx[n0:])


def t4(m, kind):
    n = m.net(kind)
    start(n, panel=True)
    n.ack[PANEL] = False
    n0 = len(n.tx)
    n.send(PANEL, ctl(0x02, GROUP))
    n.run(10_000)
    return result(n, n.tx[n0:])


def t5(m, kind):
    n = m.net(kind, tick0=rs.WRAP_FROM - 1500)            # the 500 ms window 3 s after start
    start(n)
    n.fixed[APP] = 150_000
    return result(n, request(n, APP, FULL, until_ms=80_000), tick0=n.tick0)


def t6(m, kind):
    n = m.net(kind)
    start(n)
    n.fixed[APP] = 20_000

    def wait_then_ack(net, f, k):
        if k == 2:
            return [(0, ctl(0x04, f[6:9])), (300_000, ctl(0x07, f[6:9]))]
        return None
    n.policy[APP] = wait_then_ack
    n0 = len(n.tx)
    n.send(APP, ctl(0x02, GROUP))
    n.until(lambda tx: any(p == APP and f.startswith(END_HEAD) for _t, p, f in tx), 5_000)
    n.run(200)
    return result(n, n.tx[n0:])


T7_SETS = [[0x55, 0x00, 0x00, 0x5F, 0x70, 0x00], [0x56, 0x08, 0x7F, 0x01], [0x52, 0x00, 0x7F, 0x00, 0x00, 0x00]]


def t7(m, kind):
    n = m.net(kind)
    start(n)
    n.fixed[APP] = 20_000
    for i, st in enumerate(T7_SETS):
        n.at(300 + 400 * i, lambda net, st=st: net.send(APP, ctl(0x10, st)))
    tx = request(n, APP, FULL)
    acks = [f for _t, p, f in tx if p == APP and f.startswith(rs.ACK_HEAD)]
    return result(n, tx, set_acks=acks)


def t8(m, kind):
    n = m.net(kind)
    start(n)
    n.fixed[APP] = 20_000

    def cancel(net, f, k):
        if k == 2:
            return [(0, ctl(0x05, [])), (100, ctl(0x07, f[6:9]))]
        return None
    n.policy[APP] = cancel
    n0 = len(n.tx)
    n.send(APP, ctl(0x02, GROUP))
    n.run(3_000)
    return result(n, n.tx[n0:])


def t9(m, kind, link):
    n = m.net(kind)
    start(n)
    n.fixed[APP] = 20_000
    if link:   # the switch scan (task 24) sees GP4[6] fall: FUN_4015B494(2)
        n.at(500, lambda net: net.e.call(LINK_DOWN, 2, regs={"sp": CTX_SP}))
    else:      # the panel's overlay tap: CONNECT (+ identity), no request
        n.at(500, lambda net: net.send(PANEL, ctl(0x7E, [0x01, 0x01])))
    n0 = len(n.tx)
    n.send(APP, ctl(0x02, FULL))
    n.run(12_000)
    return result(n, n.tx[n0:])


def t11(m, kind):
    n = m.net(kind)
    start(n, panel=True)
    n0 = len(n.tx)
    n.send(APP, ctl(0x10, SETS[0]))
    n.run(300)
    n.send(PANEL, ctl(0x10, SETS[1]))
    n.run(300)
    n.send(APP, ctl(0x02, GROUP))
    n.send(PANEL, ctl(0x02, [0x56, 0x7F, 0x7F]))
    n.run(6_000)
    return result(n, n.tx[n0:])


def holds(sem):
    """FUN_4014BD1C's semaphore: (count, longest hold ms)."""
    out, t0 = [], None
    for t, op in sem:
        if op == "take":
            t0 = t
        elif t0 is not None:
            out.append(t - t0)
            t0 = None
    return len(out), (max(out) / 1000 if out else 0.0)


def golden(m, kind):
    """The phone app's golden trace (PianoRemote 1.1.5, roundtrip_sysex.TRACE): every phone -> piano Kawai frame in
    order except its ACKs (the BLE link model ACKs each piano frame), 60 ms after each, a request until its End; the
    piano's Kawai TX on port 2."""
    n = m.net(kind)
    for d, f in rs.trace_frames():   # the app's ACKs come from the link model (same keys, the trace's own skipped)
        if d == "PH->PI" and f[:2] == b"\xF0\x40" and f[3] != 0x07:
            n.send(APP, f)
            if f[3] == 0x02:
                n.until(has(APP, "F0407F03041E" + f[6:8].hex()), 30_000)
            n.run(60)
    tx = [f for _t, p, f in n.tx if p == APP and f[:2] == b"\xF0\x40"]
    out = dict(tx=tx, binding=n.b(rs.BINDING), core=core_diag(n.s) if kind == "H" else None)
    n.s.close()
    return out


def session_units(image, syms):
    """The checks of 10 / T1-T12 / 12 as independent units (rtlib.Unit): each returns (ok, table rows)."""
    feat2 = images.OTA_PAYLOADS_BY_NAME[H_NAME].feat2
    m = Machines(image, syms, None)

    def u10():
        H0 = rtlib.fresh("H")
        ident = rs.rp.body_of(H0.pn.sysex(APP, rs.rp.req(0x01)), APP)
        dg = core_diag(H0)
        H0.close()
        return check(f"10 H: {H_NAME} applied live; identify {ident and ident[1].hex()} (feat2 {feat2:#04x}, 0x02 = session); "
                     f"op 1B 03 layout {dg and dg['layout']} flags {dg and hex(dg['flags'])} (core on, pump hooked, ready)",
                     ident is not None and ident[1][5:6] == bytes([feat2])
                     and len(ident[1]) == (7 if images.OTA_PAYLOADS_BY_NAME[H_NAME].feat3 else 6) and dg is not None
                     and dg["layout"] == 3 and dg["flags"] == 0x07), []

    # T1 normal dump, ACK after 20 ms (full), 80 / 300 ms (55 group); T10 from T1 (20 ms)
    def ut1(d, body):
        rows = []
        s, h = t1(m, "S", d, body), t1(m, "H", d, body)
        vs, vh = vals(s["tx"], APP), vals(h["tx"], APP)
        same = vs == vh and ends(s["tx"], APP) == ends(h["tx"], APP)
        ok = check(f"T1 dump {bytes(body).hex()} ACK {d} ms: S {len(vs)} values + End {len(ends(s['tx'], APP))}, "
                   f"H {len(vh)} + {len(ends(h['tx'], APP))}, bytes and order S == H; binding {s['binding']} / "
                   f"{h['binding']}; H core values {h['core']['values']} resends {h['core']['resends']}",
                   same and len(vs) == len(set(vs)) > 0 and len(ends(s["tx"], APP)) == 1 and s["binding"] == 3
                   and h["binding"] == 3 and h["core"]["resends"] == 0)
        if d != 20:
            return ok, rows
        rows.append(("T1", f"{len(vs)} 값 + End, 각 1회", f"같은 바이트 · 순서 {len(vh)} + End", "같음"))
        # T10 from T1 (20 ms): the reliable sender's semaphore
        sn, smax = holds(s["sem"])
        hn, hmax = holds(h["sem"])
        ok &= check(f"T10 FUN_4014BD1C's semaphore 0x413CA388 during the full dump: S taken {sn}x, held up to {smax:.1f} ms "
                    f"(an engine-loop headphone / USB notice waits that long); H taken {hn}x (the core sender never takes it)",
                    sn > 200 and hn == 0)
        rows.append(("T10", f"세마포어 {sn}회 · 최장 {smax:.0f} ms 보유(엔진 루프 통지가 그만큼 대기)", "0회(코어 송신은 안 씀)", "의도"))
        return ok, rows

    def ut2():
        s, h = t2(m, "S"), t2(m, "H")
        ks, kh = sends_per_frame(s["tx"], APP), sends_per_frame(h["tx"], APP)
        late_s = [k for k, c in ks.items() if c == 2]
        late_h = [k for k, c in kh.items() if c == 2]
        ok = check(f"T2 one ACK 700 ms late: S sends {late_s} twice, H {late_h}; others once; both End",
                   late_s == late_h and len(late_s) == 1 and all(c <= 2 for c in kh.values())
                   and len(ends(s["tx"], APP)) == len(ends(h["tx"], APP)) == 1)
        return ok, [("T2", f"늦은 값 2회({late_s[0][12:18] if late_s else '-'}), 계속", "같음", "같음")]

    def ut3():
        s, h = t3(m, "S"), t3(m, "H")
        cs, ch = len(vals(s["tx"], APP)), len(vals(h["tx"], APP))
        ts_ = [t for t, p, f in s["tx"] if p == APP and f.startswith(VALUE_HEAD)]
        th_ = [t for t, p, f in h["tx"] if p == APP and f.startswith(VALUE_HEAD)]
        gs = (ts_[-1] - ts_[0]) / 15000 if len(ts_) > 1 else 0
        gh = (th_[-1] - th_[0]) / 15000 if len(th_) > 1 else 0
        inv_s = [f for _t, p, f in s["tx"] if p == PANEL and f == INVITE]
        inv_h = [f for _t, p, f in h["tx"] if p == PANEL and f == INVITE]
        ok = check(f"T3 no answer: S {cs} sends ~{gs:.0f} ms apart -> binding {s['binding']}, End {len(ends(s['tx'], APP))}, "
                   f"invite {len(inv_s)}; H {ch} sends {gh:.0f} ms apart -> binding {h['binding']}, invite {len(inv_h)} "
                   f"(intended: the panel may take the free binding), failed {h['core']['failed']}",
                   cs == ch == 16 and s["binding"] == 0 and h["binding"] == 0 and not ends(s["tx"], APP)
                   and not ends(h["tx"], APP) and not inv_s and len(inv_h) == 1 and h["core"]["failed"] == 1)
        return ok, [("T3", f"16회(~{gs:.0f} ms) → 해제, 초대 없음", f"16회({gh:.0f} ms) → 해제 + 패널 초대", "의도(초대)")]

    def ut4():
        s, h = t4(m, "S"), t4(m, "H")
        cs, ch = len(vals(s["tx"], PANEL)), len(vals(h["tx"], PANEL))
        ok = check(f"T4 panel bound, no answer: S {cs} sends, binding {s['binding']}; H {ch}, binding {h['binding']}",
                   cs == ch == 16 and s["binding"] == h["binding"] == 4)
        return ok, [("T4", "16회 → −1, 해제 없음(4)", "같음", "같음")]

    def ut5():
        s, h = t5(m, "S"), t5(m, "H")
        ks, kh = sends_per_frame(s["tx"], APP), sends_per_frame(h["tx"], APP)
        ms_, mh = max(ks.values()), max(kh.values())
        ok = check(f"T5 counter wrap window (tick0 {s['tick0']:#x}), ACK 150 ms: S max {ms_} sends of one value, binding "
                   f"{s['binding']}, End {len(ends(s['tx'], APP))}; H every value once ({len(kh)} values, max {mh}), End "
                   f"{len(ends(h['tx'], APP))}, binding {h['binding']} - the stage-1 field gain",
                   ms_ == 16 and s["binding"] == 0 and not ends(s["tx"], APP) and mh == 1 and len(ends(h["tx"], APP)) == 1
                   and h["binding"] == 3 and len(kh) > 200)
        return ok, [("T5", f"감김 창 즉시 재전송 {ms_}회 → 해제(통지 없음)", f"값마다 1회 · End · 바인딩 유지({len(kh)}값)",
                     "의도(1단계 이득)")]

    def ut6():
        s, h = t6(m, "S"), t6(m, "H")
        ok = check(f"T6 Wait (fn 04) during a dump: S request task stuck {s['stuck']} (spins on the Wait flag, sem held: "
                   f"{len([x for x in s['sem'] if x[1] == 'take']) > len([x for x in s['sem'] if x[1] == 'give'])}); H waits "
                   f"{h['core']['waits']}, End {len(ends(h['tx'], APP))}, wait passed to the stock 0",
                   s["stuck"] and not h["stuck"] and h["core"]["waits"] == 1 and len(ends(h["tx"], APP)) == 1)
        return ok, [("T6", "요청 태스크 영구 바쁜 대기(세마포어 쥔 채)", "Wait = 이번 회차 500 ms 연장, 덤프 끝", "의도")]

    def ut7():
        s, h = t7(m, "S"), t7(m, "H")
        ok = check(f"T7 3 sets (Sound / Metronome / SongSelect) during the full dump: S {len(vals(s['tx'], APP))} values, "
                   f"End {len(ends(s['tx'], APP))}, set ACKs {len(s['set_acks'])}, reliable sends from task 19 "
                   f"{s['isr_bd1c']}; H {len(vals(h['tx'], APP))}, End {len(ends(h['tx'], APP))}, ACKs "
                   f"{len(h['set_acks'])}, from task 19 {h['isr_bd1c']}",
                   len(s["set_acks"]) == len(h["set_acks"]) == 3 and len(ends(s["tx"], APP)) == len(ends(h["tx"], APP)) == 1
                   and s["isr_bd1c"] == h["isr_bd1c"] == 0 and not s["stuck"]
                   and len(vals(s["tx"], APP)) == len(vals(h["tx"], APP)))
        return ok, [("T7", f"덤프 계속 · set ACK 3 · 태스크 19 신뢰 송신 {s['isr_bd1c']}", "같음", "같음")]

    def ut8():
        s, h = t8(m, "S"), t8(m, "H")
        ok = check(f"T8 Cancel during a dump: S {len(vals(s['tx'], APP))} values, End {len(ends(s['tx'], APP))}; "
                   f"H {len(vals(h['tx'], APP))}, End {len(ends(h['tx'], APP))}, cancels {h['core']['cancels']}",
                   vals(s["tx"], APP) == vals(h["tx"], APP) and not ends(s["tx"], APP) and not ends(h["tx"], APP)
                   and h["core"]["cancels"] == 1)
        return ok, [("T8", f"다음 값 전 중단({len(vals(s['tx'], APP))}값), End 없음", "같음", "같음")]

    def ut9a():
        s, h = t9(m, "S", False), t9(m, "H", False)
        s_disp = [f for _t, p, f in s["tx"] if p == APP and f == DISPLACE]
        h_disp = [f for _t, p, f in h["tx"] if p == APP and f == DISPLACE]
        s_pv, h_pv = len(vals(s["tx"], PANEL)), len(vals(h["tx"], PANEL))
        ok = check(f"T9 panel CONNECT during the app's dump: S 7E 01 00 to the app {len(s_disp)}, values then to port 3: "
                   f"{s_pv}, binding {s['binding']}; H no 7E 01 00 ({len(h_disp)}), values to port 3 {h_pv}, app values "
                   f"{len(vals(h['tx'], APP))} + End {len(ends(h['tx'], APP))}, binding {h['binding']}, core bound "
                   f"{h['core']['bound']:#x}",
                   len(s_disp) == 1 and s_pv > 0 and not h_disp and h_pv == 0 and len(ends(h["tx"], APP)) == 1
                   and h["binding"] == 4 and h["core"]["bound"] == 0x0C)
        return ok, [("T9a", f"앱 밀려남 · 남은 값 {s_pv}개가 패널 포트로", "앱 덤프 끝까지 · 둘 다 바인딩, 대표 = 패널", "의도")]

    def ut9b():
        s, h = t9(m, "S", True), t9(m, "H", True)
        ok = check(f"T9 link down (FUN_4015B494(2)) during the dump: S binding {s['binding']}, invite "
                   f"{sum(1 for _t, p, f in s['tx'] if f == INVITE)}, End {len(ends(s['tx'], APP))}; H binding "
                   f"{h['binding']}, invite {sum(1 for _t, p, f in h['tx'] if f == INVITE)}, app values after "
                   f"{len(vals(h['tx'], APP))}, core bound {h['core']['bound']:#x}",
                   s["binding"] == h["binding"] == 0 and not ends(h["tx"], APP) and h["core"]["bound"] == 0)
        return ok, [("T9b", "해제 · 패널 초대(스톡 경로)", "같은 스톡 경로 + 코어 덤프 중단", "같음")]

    def ut11():
        s, h = t11(m, "S"), t11(m, "H")
        a_ack = lambda r: [f for _t, p, f in r["tx"] if p == APP and f.startswith(rs.ACK_HEAD)]
        p_ack = lambda r: [f for _t, p, f in r["tx"] if p == PANEL and f.startswith(rs.ACK_HEAD)]
        ok = check(f"T11 panel + app: S app set ACK {len(a_ack(s))}, app dump {len(vals(s['tx'], APP))} (displaced); "
                   f"H app ACK {len(a_ack(h))} panel ACK {len(p_ack(h))}, app dump {len(vals(h['tx'], APP))} + End "
                   f"{len(ends(h['tx'], APP))}, panel dump {len(vals(h['tx'], PANEL))} + End {len(ends(h['tx'], PANEL))}, "
                   f"binding {h['binding']}",
                   len(a_ack(s)) == 0 and len(a_ack(h)) == 1 and len(p_ack(h)) == 1 and len(ends(h["tx"], APP)) == 1
                   and len(ends(h["tx"], PANEL)) == 1 and h["binding"] == 4)
        return ok, [("T11", "패널이 앱을 밀어냄 — 앱 set · 요청 침묵", "포트별 큐 독립, 둘 다 ACK · 덤프(팬아웃은 2단계)", "의도"),
                    ("T12", "—", "3-4(store 첫 채움) 범위 — 이번 단계 해당 없음. 덤프 바이트 S == H(T1)가 그 기준", "해당 없음")]

    def u12():
        if not rs.TRACE.is_file():   # a private capture, not in the public repository
            print(f"SKIP 12 golden trace ({rs.TRACE.relative_to(rs.REPO)} absent: not in the repository)")
            return True, [("골든", "SKIP", "capture absent", "SKIP")]
        gs, gh = golden(m, "S"), golden(m, "H")
        cap = [f for d, f in rs.trace_frames() if d == "PI->PH" and f[:2] == b"\xF0\x40"]
        ok = check(f"12 golden trace (PianoRemote 1.1.5): S {len(gs['tx'])} Kawai frames to the app, H {len(gh['tx'])} - "
                   f"bytes and order S == H; capture {len(cap)} (S == capture: {gs['tx'] == cap}); binding "
                   f"{gs['binding']} / {gh['binding']}; H core requests {gh['core']['requests']} values {gh['core']['values']}",
                   gs["tx"] == gh["tx"] and gs["binding"] == gh["binding"] == 3 and len(gh["tx"]) > 200,
                   next((f"first difference {i}: S {a_.hex()} H {b_.hex()}" for i, (a_, b_) in enumerate(zip(gs["tx"], gh["tx"]))
                         if a_ != b_), ""))
        return ok, [("골든", f"{len(gs['tx'])} 프레임", "바이트 · 순서 같음", "같음")]

    SH = ("S", "H")
    # cost = seconds measured 2026-11-02 (only the balance of the shards depends on it)
    return [rtlib.Unit("10", u10, 0.1, ("H",)),
            rtlib.Unit("T1.20", lambda: ut1(20, FULL), 2.9, SH),
            rtlib.Unit("T1.80", lambda: ut1(80, GROUP), 4.0, SH),
            rtlib.Unit("T1.300", lambda: ut1(300, GROUP), 13.4, SH),
            rtlib.Unit("T2", ut2, 1.7, SH), rtlib.Unit("T3", ut3, 4.6, SH), rtlib.Unit("T4", ut4, 4.3, SH),
            rtlib.Unit("T5", ut5, 16.3, SH), rtlib.Unit("T6", ut6, 1.7, SH), rtlib.Unit("T7", ut7, 3.1, SH),
            rtlib.Unit("T8", ut8, 1.3, SH), rtlib.Unit("T9a", ut9a, 5.7, SH), rtlib.Unit("T9b", ut9b, 5.1, SH),
            rtlib.Unit("T11", ut11, 3.5, SH), rtlib.Unit("12", u12, 5.0, SH)]


def session_part(units, argv, quiet=False):
    ok, rows = True, []
    for _u, res in rtlib.run_units(units, argv):
        ok &= res is not None and bool(res[0])
        rows += res[1] if res is not None else []
    if not quiet and rows:
        print(f"  T  | S (stock) | H ({H_NAME}) | 차이")
        for r in rows:
            print("  " + " | ".join(r))
    return ok


def field_session(image, syms):
    """The field order of the 2026-10-31 failure on the session payload: what stage 1 changes (nobody displaced, the core answers)."""
    H = rtlib.fresh("H")
    o = scenario(H, pump=True)
    dg = core_diag(H)
    H.close()
    ok = True
    d, e_, f_ = o["D"], o["E"], o["F"]
    ok &= check(f"11 field order on {H_NAME}: app sync {o['A']['values']} values; panel CONNECT -> to the app "
                f"{[x.hex() for x in d['to_app']]} (stock: 7E 01 00), panel reply {d['reply'] and d['reply'].hex()}, "
                f"{d['panel_values']} values to the panel, binding {d['binding']}",
                o["A"]["values"] == 239 and d["to_app"] == [] and d["panel_values"] == 239 and d["binding"] == 4)
    ok &= check(f"11 the app's get 52 0D after the panel took the stock binding: {[(p, x.hex()) for p, x in e_['tx']]} "
                f"(stock: silence - the field's unanswered get)",
                e_["tx"] == [(APP, bytes.fromhex("F0407F03041E520D7FF7"))])
    rep = [x for p, x in f_["tx"] if p == APP]
    ok &= check(f"11 the app's CONNECT: {[x.hex() for x in rep]}, to the panel {[x.hex() for p, x in f_['tx'] if p == PANEL]} "
                f"(stock: 7E 01 00), binding {f_['binding']}; set 55 00 00 ACKed; op 1B 03: bound {dg and hex(dg['bound'])}, "
                f"CONNECT own {dg and dg['conn_own']} pass {dg and dg['conn_pass']}, requests {dg and dg['requests']}",
                len(rep) == 1 and rep[0][:7] == bytes.fromhex("F0407F7E041E02") and f_["binding"] == 3
                and not any(p == PANEL for p, _x in f_["tx"]) and (APP, bytes.fromhex("F0407F07041E550000F7")) in o["G"]["tx"]
                and dg is not None and dg["bound"] == 0x0C)
    return ok


if __name__ == "__main__":
    sys.exit(main(sys.argv))
