# roundtrip_fanout.py - Phase 3-2 stage 2, change fan-out:
# with the app (BLE, stock port 2) and the panel (port 3) both bound to the session core, a value one of them
# changes - or the stock changes and notifies its binding - reaches the other one too, as the stock's own
# notification frame, and never comes back to the controller that set it.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_fanout.py [ota_main] [--shard i/n]
#   (--shard: the i-th of n cost-balanced groups of the units 1-10, 11, 12, 13, 14-15, 16-18 - rtlib.Unit; each
#   unit runs on fresh machines forked from one build of each kind it takes)
#
# Machines and network as roundtrip_session.py (virtual 2 ms clock, the real request task / reliable sender /
# task 19 end handler, the panel and BLE app link models, pump tail 0x401539AC every 2 ms on H), plus a model
# of the per-port TX ring writers (FUN_4014492C / FUN_401445AC / FUN_40145284 / FUN_40144CD0): every frame a
# writer is given is also stored in that port's 2 KB ring and its write index advanced, as the writer does -
# the core reads the stock binding's ring for the stock's own notifications.  S = the stock (ota_loader_v4
# built-in cave), H = the payload applied live.
# Checks (numbered as printed):
#   1  both bound on H: app CONNECT + sync, panel CONNECT -> op 1B 03 table 0x0C, stock binding 4 (panel);
#      op 1B 04 answers (stage 2 record) and op 1B 03 byte 45 names the asking port
#   2  app set 55 08 00 -> the panel gets ONE frame F0 40 7F 10 04 1E 55 08 00 <v> F7, the app gets its ACK
#      and no value frame; the bytes equal what the stock's request task sends for that key (S: app alone,
#      the same set, then 02 55 7F 7F)
#   3  panel set 55 07 00 / 56 01 7F (tempo) -> the app gets each; nothing back to the panel
#   4  a set that changes nothing (the panel's cache push) -> no frame to anybody
#   5  stock-originated: the headphone notice FUN_4014C048(0x58 51 29) run as the engine loop does (reliable
#      sender, real ACK wait): S sends it to its binding only; H: the panel (stock binding, the stock's own
#      send) and the app (the core, from the ring) once each, same bytes, no duplicate to the panel
#   6  app SongSelect 52 00 7F -> the stock's MaxTime notice (FUN_401694E4 -> FUN_40151A38 -> notify task
#      FUN_40151E08 -> FUN_4014C048, descriptor 0xA4 = 52 18) goes to the panel (stock) and to the app (core):
#      the stock tells its bound controller, so the setter gets it too
#   7  ordering / no storm: 5 quick app sets of one key while the panel is slow (ACK 300 ms) -> the panel gets
#      the frames in order, the last value last, fewer than 5 (merged in the queue), none to the app
#   8  interleave: the panel changes a value during the app's full dump -> the app's dump is complete (239
#      values + End) and the change arrives in between; one frame in flight per port all along (every fan
#      frame ACKed before the next, fan resends 0, fan drops 0, frames <= 40 B for the panel's 511 B buffer)
#   9  op 1B 04 at the end: counters agree (sets fanned, frames sent == ACKed, tap frames fanned)
#   10 the app CONNECTs again (stock binding back to port 2, the panel stays bound): the core follows the new
#      binding's ring - the headphone notice reaches the app (stock) and the panel (core)
#   11 live payload swap (field 2026-11-01): panel + app bound,
#      the app holds the stock binding; "apply now" (op 0x15) of the same slot again -> the new core starts with
#      table 0x04 and sends the panel the stock's invitation F0 40 7F 7E 04 1E 01 01 F7 once (port 3 only, nothing
#      to the app: no 7E 01 00, no invitation); the panel re-CONNECTs as its JS does (CONNECT + identity + full
#      request) -> table 0x0C, an app set reaches the panel again, a panel set the app; the app's own re-CONNECT
#      afterwards displaces nobody.  The first apply (nobody bound yet, as at a boot load) sent no invitation.
# Phase 3-4 S1 + Phase 3-5 stage 1 wired into the cave.  These rigs start from the stock boot settings (FUN_400B1448 defaults -> FUN_4015B384 boot
# copy -> FUN_4016E054 song-task init, as FUN_400AF648 does), put in right after the loader boots and before the
# apply; with `t20` task 20 (FUN_4016DE5C(1)) runs after the pump tail every 2 ms, on both sides:
#   12 S1: the store's first fill by the pump tail (op 1B 05): 239 value frames = 230 store + 8 side table + 1 unknown
#      (55 01 1F), 0 bad / long; a compare pass (1B 05 01) right after: 0 differences; a value the stock changes alone
#      (metronome volume byte poked) -> exactly 1 difference, key 56 03 7F; the next pass 0.  Stack depth and
#      instructions of every pump-tail pass are measured (fill, compare, idle).
#   13 metronome: app + panel bound (the panel holds the stock binding).  The app turns it on, the panel sets 3/4, the
#      app sets a refused 7/4, the app turns it off; S (the app alone, stock) gets the same sets.  The panel gets the
#      stock's BeatCounter frames only (none from the core, none relayed again by the tap); the app gets the core's -
#      the same bytes in the same order as the panel's and as S's app, at most one 2 ms pass later; a value change fans
#      out once to the other side (the refused beat changes nothing: no frame), never back to the setter; op 1B 05:
#      the judgement == the stock block after every apply (miss 0), the store compare afterwards 0 differences (a raw
#      mirror of the refused beat would differ), and the tap relayed no fn 0x32 frame meanwhile.
# Field 2026-11-01 (ota_main 7018a43c): the stock runs fn 0x31 / 0x32
# commands for its binding only (FUN_4014A2C8: FUN_40151BCC(port) == 0 -> return), so a bound controller that is not
# the stock binding was ignored; the BT-audio ops were gated on the stock binding too.
#   14 fn 0x32 command from the app while the panel holds the stock binding: the app's metronome run 32 56 08 7F 01
#      starts it (S: the app alone, the same command) - BeatCounter to the panel (stock) and to the app (core), the
#      app's == S's app; the panel learns the run state from a value frame F0 40 7F 10 04 1E 56 08 7F 01 F7 (the stock
#      sends none for a command - the getter frame, as for a set), the app none; the panel's 32 56 08 7F 00 stops it
#      and the app gets 56 08 7F 00; no ACK for fn 0x32 (as the stock), the stock binding stays 4 all along (no
#      flapping), 1B 04 counts the commands (1 from the non-binding app per app command), 1B 05 judged 2 sets, miss 0.
#      The old build (7018a43c) fails it: the app's command is dropped.
#   15 BT audio gate (op 1D 05 「이 기기로」): the app, bound in the core table but not the stock binding, passes the
#      bound check (rc != 2 - the field's rc 2 was `5D 05 02 01`); a port nobody bound (0, USB) still gets rc 2.
# Phase 3-4 S2: the pump tail runs a compare pass by itself every 5 s
# (paused while a set / fan-out, staging, the startup restore, a flash job, the recorder or standby is in progress) and
# sorts each difference: explained (a known stock path) or unexplained (fixed + fanned out to every bound controller).
# op 1B 05 02 = counters, 1B 05 03 [k] = the ring of the last unexplained ones.  App + panel bound, panel = stock binding.
# S3 / S4 / the MIDI channel judge / the executor gate are always on in ota_main (boot-on 2026-11-16; fold step 3,
# 2026-11-21, removed their off ops), so 16-21 and 25-26 run with all four on, as on the piano.  Where these checks
# compared with "S3 off" before fold step 3 they compare with the stock alone (S) or with the stock getter's own answer
# while the store is untrusted; 19 checks the boot state and that the old off op 1B 05 04 00 is refused:
#   16 stock-originated changes, no set from anybody: the headphone jack (state byte 0x4139E7FA + the engine loop's
#      notice FUN_4014C048(0x58) to the panel) and a MIDI Program Change on USB-MIDI (the stock's own channel path
#      FUN_401475CC -> FUN_4016053C(ctx, 0, 7), no controller told) -> the periodic passes find 51 29 7F (NOTIFY) and
#      55 00 00 (KEY): 2 explained, 0 unexplained (the app has 51 29 once, from the tap); with S3 on the silent key
#      55 00 00 goes once to the app and the panel (before fold step 3, with S3 switched off: no frame)
#   17 a hidden direct RAM write (metronome volume byte, no path at all) -> exactly 1 unexplained difference: the store
#      is fixed, the app and the panel get F0 40 7F 10 04 1E 56 03 7F <new> F7 once each, 1B 05 03 shows the key with
#      the old / new value, the next pass finds nothing
#   18 no false alarms: the metronome running 12 s (the app's 32 56 08 01, task 20 on) -> passes run, 0 unexplained;
#      the staging flag up 3 s -> no pass (postponed, pause bit 0x04), down -> passes again; pump-tail cost of a pass
# Field 2026-11-09 (ota_main e5bb2372): the stock runs fn 0x24 for its binding only
# and sends every file reply to *0x413CA3DE at that send, so the app's USB list got nothing while the panel held the binding.
# App + panel bound, the panel = stock binding, USB memory mounted, a fake directory table for FUN_401282AC, the real
# stop-and-wait FUN_4014B638 / FUN_4014B4E0 (the controllers ACK every piano fn 0x24 packet 15 ms later):
#   22 the app's 24 11: the app gets the stock's 24 7F, the core makes it the stock binding for the job (3), the list is
#      queued; the panel's own 24 11 meanwhile gets the core's 24 7F + Busy 7E 03 01 00 00 and queues nothing; the file
#      task's walk + 24 1F go to the app only (each packet once = the app's ACK reached the stock); 1 s after the last ACK the
#      binding is the panel's again (1B 04 lease 0, leases +1); an app set still reaches the panel, the panel's own list is
#      the stock's (its 24 7F, the app nothing).  The old build (e5bb2372) fails: the app gets nothing at all.
#   23 the app deletes by name (24 06 + FileName, the engine loop's executor FUN_4014F364 run once): its ACKs and the Result
#      32 51 7E 7F 00 00 reach the app, nothing reaches the panel (the tap does not relay 32 51 7E), binding 3 -> 4.
#   24 the app stops ACKing mid-list: the stock's 16 sends then its unbind (FUN_4014B55C, binding 0, as for any non-panel
#      binding) - the core gives the panel its binding back, the app is out of the table as the stock decided.
#   25 Phase 3-4 S4, S3 + S4 on (from boot): a panel set of a pure key (speaker volume)
#      keeps the store trusted - the app's request right after the ACK is served from the store, the new value; the
#      same key one byte short: ACKed, refused, the answer unchanged (the read-back undid the front's mirror)
#   26 a set that is not pure (Sound of part 00): the S3 rule (untrusted until a clean walk); the old off op 1B 05 06 00 ->
#      rc 1, S4 stays on (before fold step 3 it switched S4 off: the pure key's set was the S3 rule again)
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rss = rtlib.load("roundtrip_session")
rs, rt, pa, images, check = rss.rs, rss.rt, rss.pa, rss.images, rss.check
STOCKFW = rss.STOCKFW
APP, PANEL = rss.APP, rss.PANEL
ctl, has, connect = rss.ctl, rss.has, rss.connect
VALUE_HEAD, END_HEAD = rss.VALUE_HEAD, rss.END_HEAD
CTX_SP = rss.CTX_SP
le = rs.le

# per-port SysEx TX ring: (buffer, write index u16) - the writers' literal pools
RINGS = {0: (0x413ACAFC, 0x413AE5A6), 1: (0x413ADAFC, 0x413AE5B2), 2: (0x413A9AFC, 0x413AE584),
         3: (0x413ABAFC, 0x413AE59A)}
NOTIFY = 0x4014C048                 # FUN_4014C048(descriptor id, cc, slot, data): the stock's notify send
NOTIFY_ENQ, NOTIFY_ENQ_END = 0x40151A38, 0x40151B5C   # FUN_40151A38: puts a notice on the notify dtq
DATA = CTX_SP - 0x3000              # scratch for the notice's data bytes (below the injected task's stack)
HEADPHONE = bytes.fromhex("F0407F10041E51297F01F7")
INVITE = bytes.fromhex("F0407F7E041E0101F7")       # FUN_4014C080: "CONNECT again", port 3 only
DISPLACE = bytes.fromhex("F0407F7E041E0100F7")
rh = rs.rpw.rh                                      # roundtrip_ota_hooks: apply now (op 0x15)
# fn 0x24 file access (22-23; addresses as scripts/analysis/stock_fn24.py, REA)
FILE_LIST, FILE_LIST_END = 0x4014F018, 0x4014F07C   # FUN_4014F018: list request -> file task queue
FILE_HEAD = bytes.fromhex("F0407F24041E")
FILE_ACK_US = 15_000                                # the app's ACK, a BLE event later [model]
MEDIA = 0x41359E45                                  # & 0x7F: USB memory mounted (FUN_4012795C)
LIST_BUSY = 0x413CA3EC                              # list running (FUN_4014F018 sets, the file task clears)
FILE_OP, NAME_READY = 0x413CA3EA, 0x413CA3EB        # pending job d0 / name complete (executor due)
WALK, END_OF_LIST, ROOT = 0x40128360, 0x4014F07C, 0x40152278   # the file task's message 0: FUN_40128360("\") + 24 1F
OPEN_DIR, DIR_TABLE, DIR_DEPTH, DIR_ENTRY = 0x401282AC, 0x41045868, 0x41057EB8, 0x11C
EXECUTOR, FAT_DELETE = 0x4014F364, 0x400A94BC       # engine-loop job executor, FAT delete (stubbed: 0)
FAKE_FS = {"\\": [(1, "SONG1.MID"), (1, "A_LONG_FILE_NAME_FOR_TWO_PACKETS.MID"), (2, "DIR")], "\\DIR": [(1, "INNER.MID")]}
BUSY = bytes.fromhex("F0407F7E041E03010000F7")
FILE_ACK = bytes.fromhex("F0407F24041E7FF7")
RESULT_OK = bytes.fromhex("F0407F32041E517E7F0000F7")


class NetF(rss.Net):
    """roundtrip_session.Net + the TX ring model + stock tasks run with the reliable sender's real wait."""

    def __init__(self, side, **kw):
        self.notify_q = []                    # messages FUN_40151A38 posted to the notify task's dtq
        self.file_q = []                      # messages FUN_4014F018 posted to the file-access task's queue
        self.file_ack = set()                 # ports that ACK every piano fn 0x24 packet (fileaccess.ts) - 22-23
        super().__init__(side, **kw)
        self.acks_in = []                     # (t, port, frame) controller ACKs delivered

    def _post(self, em):
        # the emulator leaves every dtq id 0, so the notify dtq looks like the request task's: tell them apart by
        # the poster (FUN_40151A38's tsnd_dtq) - otherwise the request task would take the notice as a request
        if NOTIFY_ENQ <= em.reg(14) < NOTIFY_ENQ_END:
            self.notify_q.append(em.reg(1))
            return 0
        if FILE_LIST <= em.reg(14) < FILE_LIST_END:    # FUN_4014F018's post of message 0 (snd to *0x413CA37C)
            self.file_q.append(em.reg(1))
            return 0
        return super()._post(em)

    def _tx(self, em, port):
        r = super()._tx(em, port)
        if port in RINGS:
            buf, wr = RINGS[port]
            f = bytes(em.read_bytes(em.reg(0), em.reg(1)))
            w = le(em.read_bytes(wr, 2) + b"\0\0") & 0x7FF
            for b in f:
                em.write_bytes(buf + w, bytes([b]))
                w = (w + 1) & 0x7FF
            em.write_bytes(wr, w.to_bytes(2, "little"))
        f = bytes(em.read_bytes(em.reg(0), em.reg(1)))
        # the app's file client (fileaccess.ts): an ACK 24 7F for every FileName 03, end 1F, TopOfData 01, data 02, 7B
        if port in self.file_ack and f.startswith(FILE_HEAD) and len(f) > 7 and f[6] in (0x01, 0x02, 0x03, 0x1F, 0x7B):
            self.pending.append((self.t + FILE_ACK_US, port, ctl(0x24, [0x7F])))
            self.pending.sort(key=lambda x: x[0])
        return r

    def _deliver_due(self):
        due = [x for x in self.pending if x[0] <= self.t]
        for _t, p, f in due:
            if f[3:4] == b"\x07":
                self.acks_in.append((self.t, p, f))
        super()._deliver_due()

    def task(self, addr, *args):
        """Run a stock function as a task (engine loop / notify task): preempted by task 19 deliveries at its
        2 ms counter reads and delays, like the request task in roundtrip_session.Net._run_task."""
        e = self.e
        self.in_task = True
        try:
            res = e.call(addr, *args, regs={"sp": CTX_SP}, max_insns=200_000_000)
            while res.stop == "preempt":
                regs = {n: res.regs[i] for i, n in enumerate(
                    ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr"])}
                self._deliver_due()
                res = e.call(res.regs[15], regs=regs, max_insns=200_000_000)
        finally:
            self.in_task = False
        if res.stop != "returned":
            raise RuntimeError(f"task {addr:#x} stopped: {res}")
        return res

    # ---- task 20 and the pump-tail measurements (12 / 13) ----
    t20 = False
    t20_max = 2_000_000                               # instructions per task-20 pass (an SMF stop + rewind needs more)
    next_t20 = 0
    pump_log = None                                   # [(t, instructions, stack bytes below rs.ISR_SP)] when a list

    def run(self, ms):
        """roundtrip_session.Net.run + task 20 after the pump tail every 2 ms (t20) + per-pass stack / instructions."""
        if not self.t20 and self.pump_log is None:
            return super().run(ms)
        end = self.t + int(ms * 1000)
        if self.t20 and self.next_t20 <= self.t:
            self.next_t20 = self.t - self.t % 2000 + 2000
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
            if self.t20:
                nxt = min(nxt, self.next_t20)
            self.t = max(self.t + 100, min(nxt, end))
            self._sync()
            if self.pump_fn is not None and self.t >= self.next_pump:
                self.next_pump = self.t - self.t % 2000 + 2000
                r = self.e.call(self.pump_fn, regs={"sp": rs.ISR_SP}, max_insns=20_000_000)
                if r.stop != "returned":
                    raise RuntimeError(f"pump tail stopped: {r}")
                if self.pump_log is not None:
                    low = min((w.addr for w in r.writes if STACK_LO <= w.addr < rs.ISR_SP), default=rs.ISR_SP)
                    self.pump_log.append((self.t, r.insns, rs.ISR_SP - low))
            if self.t20 and self.t >= self.next_t20:      # task 20 after task 19 (lower priority, same 2 ms signal)
                self.next_t20 = self.t - self.t % 2000 + 2000
                r = self.e.call(TASK20, 1, regs={"sp": T20_SP}, max_insns=self.t20_max)
                if r.stop != "returned":
                    raise RuntimeError(f"task 20 stopped: {r}")
        if not self.stuck:
            self._deliver_due()

    def notify(self, desc, data, cc=0x7F, slot=0x7F):
        self.e.write_bytes(DATA, bytes(data))
        return self.task(NOTIFY, desc, cc, slot, DATA)

    def run_notify_posts(self):
        """The notify task FUN_40151E08: each message posted to its dtq -> FUN_4014C048(id, m[2], m[3], m + 4)."""
        n = 0
        while self.notify_q:
            m = self.notify_q.pop(0)
            b = self.e.read_bytes(m, 4)
            self.task(NOTIFY, b[0] << 8 | b[1], b[2], b[3], m + 4)
            n += 1
        return n


# stock boot settings (roundtrip_metro.py, REA analysis) and the metronome's RAM (kpmetro.h KPM_A_*)
DEFAULTS, BOOT_COPY, UM, CUR_BLOCK, SONG_INIT = 0x400B1448, 0x4015B384, 0x40400004, 0x4015B4D8, 0x4016E054
TASK20, T20_SP = 0x4016DE5C, 0x40F20000
METRO_CFG = 0x418F1A2D
BEAT_HEAD = bytes.fromhex("F0407F32041E560B7F")
STACK_LO = CTX_SP                     # the pump tail runs on rs.ISR_SP; its stores below that, down to CTX_SP, are stack


def boot_settings(e):
    e.call(DEFAULTS, UM)
    e.call(BOOT_COPY, UM)
    r = e.call(SONG_INIT, e.call(CUR_BLOCK, 0).r0 + 0x12, max_insns=5_000_000)
    assert r.ok, r


def settings_rig(image, syms, _orig=rs.rpw.Rig):
    rig = _orig(image, syms)
    boot = rig.pn.boot

    def boot2(*a, **k):
        r = boot(*a, **k)
        boot_settings(rig.emu)
        return r
    rig.pn.boot = boot2
    return rig


def _plain_side_settings(image, syms, data):
    orig, rs.rpw.Rig = rs.rpw.Rig, settings_rig
    try:
        return rs.Side(image, syms, data)
    finally:
        rs.rpw.Rig = orig


def side_settings(m, data):
    """rs.Side whose loader boot is followed by the stock boot settings (before the payload is applied)."""
    key = "Hs" if data is not None else "Ss"
    if key in rtlib._BUILDERS and data in (None, m.data):
        return rtlib.fresh(key)
    orig, rs.rpw.Rig = rs.rpw.Rig, settings_rig
    try:
        return rs.Side(m.image, m.syms, data)
    finally:
        rs.rpw.Rig = orig


def values(tx, port):
    return [f for _t, p, f in tx if p == port and f.startswith(VALUE_HEAD)]


def ack_frames(tx, port):
    return [f for _t, p, f in tx if p == port and f.startswith(rs.ACK_HEAD)]


def diag(side, sub, port=APP):
    out = side.pn.sysex(port, rs.rp.req(rs.OP_FRONT, sub))
    b = rs.rp.body_of(out, port)
    if not b or b[0] != rs.OP_FRONT | 0x40 or b[1][:2] != bytes([sub, 0]):
        return None
    return rs.rp.unpack7(b[1][2:])


def fan_diag(side, port=APP):
    d = diag(side, 0x04, port)
    if d is None:
        return None
    names = ["sets", "same", "queued", "merged", "sent", "acks", "drop", "fail", "tap_frames", "tap_fanned",
             "tap_long", "snap_over", "relayed"]
    r = dict(layout=d[0], flags=d[1], bound=d[2], binding=d[3], port=d[4], tap_rep=d[5], pending=list(d[6:10]),
             tap_pos=d[10] | d[11] << 8, tap_bytes=le(d, 38), cmds=d[42] | d[43] << 8, cmds_other=d[44] | d[45] << 8,
             lease=d[46], leases=d[47])
    for i, k in enumerate(names):
        r[k] = d[12 + 2 * i] | d[13 + 2 * i] << 8
    return r


def set_and_wait(n, port, body, ms=600):
    n0 = len(n.tx)
    n.send(port, ctl(0x10, body))
    n.until(has(port, "F0407F07041E" + bytes(body[:3]).hex()), 3000)
    n.run(ms)
    return n.tx[n0:]


def state_diag(side, port=APP):
    """op 0x1B 05: the settings-state walk (kpstate) and the metronome module (kpmetro)."""
    d = diag(side, 0x05, port)
    if d is None:
        return None
    w = lambda i: d[i] | d[i + 1] << 8
    r = dict(layout=d[0], flags=d[1], mode=d[2], cursor=d[3], diffs_total=w(20), last_diff=bytes(d[22:25]).hex(),
             last_unknown=bytes(d[25:28]).hex(), metro_sent=w(42), metro_miss=w(44), on=d[46], beat=d[47])
    for i, k in enumerate(["frames", "stored", "unknown", "longval", "bad", "diffs", "indexed", "passes"]):
        r[k] = w(4 + 2 * i)
    for i, k in enumerate(["sets", "ignored", "refused", "beats", "starts", "reloads", "resyncs"]):
        r["m_" + k] = w(28 + 2 * i)
    return r


def compare_pass(n, port=APP):
    """op 0x1B 05 01, then run until the walk is done (4 value reads per pump-tail pass); rc, 1B 05, pump log.
    A walk already running (S2's periodic pass) is waited out first: rc 1 is then only "not filled yet"."""
    for _ in range(40):
        out = n.s.pn.sysex(port, rs.rp.req(rs.OP_FRONT, 0x05, 0x01))
        b = rs.rp.body_of(out, port)
        rc = b[1][1] if b and b[0] == rs.OP_FRONT | 0x40 and b[1][:1] == b"\x05" else None
        if rc != 1 or not rs.rp.unpack7(b[1][2:])[1] & 0x10:
            break
        n.run(50)
    p0 = len(n.pump_log)
    st = None
    for _ in range(40):
        n.run(50)
        st = state_diag(n.s, port)
        if st is None or not st["flags"] & 0x10:
            break
    return rc, st, n.pump_log[p0:]


def beats(tx, port):
    return [(t, f) for t, p, f in tx if p == port and f.startswith(BEAT_HEAD)]


def pump_stats(log):
    """(passes, most instructions in one pass, deepest stack in one pass)"""
    return (len(log), max((x[1] for x in log), default=0), max((x[2] for x in log), default=0))


def state_s1(m, rows):
    n = NetF(side_settings(m, m.data), pump=True)
    n.pump_log = []
    both_bound(n)
    fill = list(n.pump_log)
    st = state_diag(n.s)
    rc, cmp0, log0 = compare_pass(n)
    quiet(n)                                          # S2's periodic pass not running nor due within the next one
    vol = n.b(METRO_CFG + 7)
    n.s.poke(METRO_CFG + 7, bytes([vol ^ 0x10]))     # the stock changes a value by itself (no set, no notice)
    rc1, cmp1, _ = compare_pass(n)
    rc2, cmp2, _ = compare_pass(n)
    p1 = len(n.pump_log)
    n.run(200)
    idle = n.pump_log[p1:]
    fl, cp, il = pump_stats(fill), pump_stats(log0), pump_stats(idle)
    ok = check(f"12 S1 store fill (op 1B 05): flags {st and hex(st['flags'])} frames {st and st['frames']} stored "
               f"{st and st['stored']} (side table {st and st['indexed']}) unknown {st and st['unknown']} "
               f"({st and st['last_unknown']}) long {st and st['longval']} bad {st and st['bad']}; compare pass rc {rc}: "
               f"diffs {cmp0 and cmp0['diffs']} passes {cmp0 and cmp0['passes']}; stock volume byte changed alone -> "
               f"diffs {cmp1 and cmp1['diffs']} key {cmp1 and cmp1['last_diff']}; next pass {cmp2 and cmp2['diffs']} "
               f"(total {cmp2 and cmp2['diffs_total']}); pump tail (passes, max insns, max stack B): fill {fl} compare "
               f"{cp} idle {il}",
               st is not None and st["flags"] & 0x0F == 0x0F and st["frames"] == 239 and st["stored"] == 238
               and st["indexed"] == 8 and st["unknown"] == 1 and st["last_unknown"] == "55011f" and st["longval"] == 0
               and st["bad"] == 0 and st["passes"] >= 1 and rc == 0 and cmp0 is not None and cmp0["diffs"] == 0
               and cmp0["passes"] > st["passes"] and cmp0["frames"] == 239 and rc1 == 0 and cmp1["diffs"] == 1
               and cmp1["last_diff"] == "56037f" and rc2 == 0 and cmp2["diffs"] == 0 and cmp2["diffs_total"] == 1)
    rows.append(("12", "S1 store 첫 채움 + 대조 한 바퀴",
                 f"239 = store 230 + 곁표 8 + 모름 1(`55 01 1F`), 대조 차이 0 · 스톡 혼자 바꾼 값 → 차이 1(`56 03 7F`) → 0; "
                 f"펌프 꼬리 한 차례 최대 명령어 · 스택: 채움 {fl[1]} · {fl[2]} B, 대조 {cp[1]} · {cp[2]} B, 평시 {il[1]} · {il[2]} B"))
    n.s.close()
    return ok


# (port, set body, ms to run after its ACK, value frames the other side gets)
# the waits put every next set mid-beat (250 ms from a beat): S (all sets from the app, BLE timing) and H (one from the
# panel, UART timing) then cut the same beats - a set within a few ms of a beat would land on either side of it
METRO_SETS = [(APP, [0x56, 0x08, 0x7F, 0x01], 2750, 1),       # on (4/4, 120): 5.5 beats
              (PANEL, [0x56, 0x02, 0x7F, 0x03, 0x02], 1750, 1),  # 3/4 while running (restart)
              (APP, [0x56, 0x02, 0x7F, 0x07, 0x02], 1250, 0),    # 7/4: refused - nothing changes, restarts anyway
              (APP, [0x56, 0x08, 0x7F, 0x00], 600, 1)]           # off


def metronome(m, rows):
    # S: the stock, the app alone
    s = NetF(side_settings(m, None))
    connect(s, APP, full=False)
    s.run(50)
    s.t20 = True
    s0 = len(s.tx)
    for _port, body, ms, _v in METRO_SETS:
        set_and_wait(s, APP, body, ms=ms)
    s_app = [f for _t, f in beats(s.tx[s0:], APP)]
    s.s.close()
    # H: app + panel, the panel holds the stock binding
    n = NetF(side_settings(m, m.data), pump=True)
    n.pump_log = []
    both_bound(n)
    n.run(100)
    fd0 = fan_diag(n.s)
    n.t20 = True
    n0, p0 = len(n.tx), len(n.pump_log)
    vals = []
    for port, body, ms, want in METRO_SETS:
        tx = set_and_wait(n, port, body, ms=ms)
        other = PANEL if port == APP else APP
        key = lambda f: f[6:9] == bytes(body[:3])
        vals.append((len([f for f in values(tx, other) if key(f)]), len([f for f in values(tx, port) if key(f)]), want))
    tx = n.tx[n0:]
    run_log = n.pump_log[p0:]
    h_panel, h_app = beats(tx, PANEL), beats(tx, APP)
    st = state_diag(n.s)
    fd1 = fan_diag(n.s)
    rc, cmp, _ = compare_pass(n)
    lag = [ta - tp for (tp, _fp), (ta, _fa) in zip(h_panel, h_app)]
    pv, av, sv = [f[9] for _t, f in h_panel], [f[9] for _t, f in h_app], [f[9] for f in s_app]
    k = min(len(av), len(sv))
    rl = pump_stats(run_log)
    ok = check(f"13 metronome (app on, panel 3/4, app refused 7/4, app off): BeatCounter to the panel (stock) {len(pv)} "
               f"{pv}, to the app (core) {len(av)}, S app (stock) {len(sv)} {sv}; app == panel frames "
               f"{[f for _t, f in h_app] == [f for _t, f in h_panel]}, lag max {max(lag) if lag else None} us; value "
               f"frames (other, setter, want) {vals}; 1B 05: sets {st and st['m_sets']} refused {st and st['m_refused']} "
               f"miss {st and st['metro_miss']} sent {st and st['metro_sent']} beats {st and st['m_beats']} starts "
               f"{st and st['m_starts']}; tap fn 0x32 relays {fd0 and fd0['relayed']} -> {fd1 and fd1['relayed']}; "
               f"compare after: rc {rc} diffs {cmp and cmp['diffs']}; pump tail while running {rl}",
               len(pv) >= 15 and [f for _t, f in h_app] == [f for _t, f in h_panel]
               and all(0 <= x <= 2000 for x in lag) and abs(len(av) - len(sv)) <= 1 and av[:k] == sv[:k]
               and all(o == w and setter == 0 for o, setter, w in vals)
               and st is not None and st["m_sets"] == 4 and st["m_refused"] == 1 and st["metro_miss"] == 0
               and st["metro_sent"] == len(av) and fd0["relayed"] == fd1["relayed"] and rc == 0
               and cmp is not None and cmp["diffs"] == 0)
    rows.append(("13", "메트로놈: 앱 켬 · 패널 3/4 · 앱 7/4(거절) · 앱 끔, 패널이 스톡 바인딩",
                 f"BeatCounter 패널(스톡) {len(pv)} = 앱(코어) {len(av)}, 바이트 · 순서 같음(늦음 ≤ 2 ms), S 앱 {len(sv)}과 같음, "
                 f"중복 0 · 탭 중계 0, 값 프레임 다른 쪽 1(거절은 0) · 설정자 0, 판단 == 스톡 블록, 뒤 대조 차이 0; 펌프 꼬리 "
                 f"최대 {rl[1]} 명령어 · 스택 {rl[2]} B"))
    n.s.close()
    return ok


CMD_RUN, CMD_STOP = [0x56, 0x08, 0x7F, 0x01], [0x56, 0x08, 0x7F, 0x00]
OP_BT = 0x1D


def cmd_and_run(n, port, body, ms):
    n0 = len(n.tx)
    n.send(port, ctl(0x32, body))
    n.run(ms)
    return n.tx[n0:]


def acks32(tx):
    return [(p, f) for _t, p, f in tx if f.startswith(rs.ACK_HEAD) and f[6:8] == b"\x56\x08"]


def remote_cmd(m, rows):
    # S: the stock, the app alone (the stock binding) - what a command does there
    s = NetF(side_settings(m, None))
    connect(s, APP, full=False)
    s.run(50)
    s.t20 = True
    s0 = len(s.tx)
    cmd_and_run(s, APP, CMD_RUN, 2750)
    s_on = s.b(METRO_CFG)
    cmd_and_run(s, APP, CMD_STOP, 600)
    s_tx = s.tx[s0:]
    s_app = [f for _t, f in beats(s_tx, APP)]
    s.s.close()
    # H: app + panel bound, the panel holds the stock binding (the field)
    n = NetF(side_settings(m, m.data), pump=True)
    both_bound(n)
    n.run(100)
    b0 = n.b(rs.BINDING)
    fd0 = fan_diag(n.s)
    st0 = state_diag(n.s)
    n.t20 = True
    tx1 = cmd_and_run(n, APP, CMD_RUN, 2750)
    on1, bind1 = n.b(METRO_CFG), n.b(rs.BINDING)
    tx2 = cmd_and_run(n, PANEL, CMD_STOP, 600)
    on2, bind2 = n.b(METRO_CFG), n.b(rs.BINDING)
    tail = cmd_and_run(n, APP, [0x56, 0x0B, 0x7F, 0x01], 600)   # once stopped: no beats; 32 56 0B is not a command
    fd1 = fan_diag(n.s)
    st1 = state_diag(n.s)
    key = lambda f: f[6:9] == b"\x56\x08\x7F"
    run_p = [f for f in values(tx1, PANEL) if key(f)]
    run_a = [f for f in values(tx1, APP) if key(f)]
    stop_a = [f for f in values(tx2, APP) if key(f)]
    stop_p = [f for f in values(tx2, PANEL) if key(f)]
    hp, ha = beats(tx1, PANEL), beats(tx1, APP)
    av, pv, sv = [f[9] for _t, f in ha], [f[9] for _t, f in hp], [f[9] for f in s_app]
    k = min(len(av), len(sv))
    after = beats(tail, PANEL) + beats(tail, APP)
    no_ack = not acks32(tx1 + tx2 + tail) and not acks32(s_tx)
    cmds = (fd1["cmds"] - fd0["cmds"], fd1["cmds_other"] - fd0["cmds_other"]) if fd1 and fd0 else None
    msets = st1["m_sets"] - st0["m_sets"] if st1 and st0 else None
    ok = check(f"14 app fn 0x32 metronome run 32 56 08 7F 01, the panel holds the stock binding {b0}: on {on1} (S {s_on}); "
               f"BeatCounter panel (stock) {len(pv)} {pv[:6]}.., app (core) {len(av)}, S app {len(sv)} {sv[:6]}..; run "
               f"state value frame to the panel {[f.hex() for f in run_p]}, to the app {len(run_a)}; panel 32 56 08 7F 00: "
               f"on {on2}, the app gets {[f.hex() for f in stop_a]}, the panel {len(stop_p)}; beats after the stop "
               f"{len(after)}; fn 0x32 ACKs {not no_ack}; stock binding {[bind1, bind2]}; 1B 04 commands (all, "
               f"non-binding) {cmds}; 1B 05 metronome sets +{msets} miss {st1 and st1['metro_miss']}",
               b0 == 4 and s_on == 1 and on1 == 1 and on2 == 0 and len(pv) >= 5 and len(av) == len(pv)
               and [f for _t, f in ha] == [f for _t, f in hp] and abs(len(av) - len(sv)) <= 1 and av[:k] == sv[:k]
               and len(run_p) == 1 and run_p[0] == bytes.fromhex("F0407F10041E56087F01F7") and not run_a
               and len(stop_a) == 1 and stop_a[0] == bytes.fromhex("F0407F10041E56087F00F7") and not stop_p
               and not after and no_ack and bind1 == 4 and bind2 == 4 and cmds == (3, 2) and msets == 2
               and st1["metro_miss"] == 0)
    rows.append(("14", "앱 fn 0x32 메트로놈 시작(패널이 스톡 바인딩) · 패널 정지",
                 f"켜짐 {on1}(S {s_on}), BeatCounter 패널(스톡) {len(pv)} = 앱(코어) {len(av)} · S 앱 {len(sv)}, 실행 상태 값 프레임 "
                 f"패널 {len(run_p)}(`56 08 7F 01`) · 앱 {len(run_a)} → 패널 정지(켜짐 {on2}) → 앱 {len(stop_a)}(`56 08 7F 00`), fn 0x32 ACK 0, 스톡 바인딩 {bind1} · {bind2}"))
    # ---- 15 the BT-audio gate: any controller bound in the core table ----
    phone7 = list(rtlib.pack7(bytes.fromhex("6281818111a1")))
    rig = n.s.rig
    r_app = rig.raw_reply(APP, rs.rp.req(OP_BT, 0x05, 0x01, *phone7), OP_BT)
    r_usb = rig.raw_reply(0, rs.rp.req(OP_BT, 0x05, 0x01, *phone7), OP_BT)
    rc_app = r_app[1] if r_app is not None and len(r_app) >= 2 else None
    rc_usb = r_usb[1] if r_usb is not None and len(r_usb) >= 2 else None
    ok &= check(f"15 BT audio 1D 05 01 (「이 기기로」) from the app, bound in the core table (stock binding "
                f"{n.b(rs.BINDING)}): rc {rc_app} (2 = not bound - the field); from port 0 (nobody bound): rc {rc_usb}",
                rc_app is not None and rc_app != 2 and rc_usb == 2)
    rows.append(("15", "BT 오디오 1D 05(앱은 코어 표에만 바운드)", f"앱 rc {rc_app}(바운드 검사 통과) · 포트 0 rc {rc_usb}"))
    n.s.close()
    return ok



# ---- Phase 3-4 S2 (16-18) ----
HP_STATE = 0x4139E7FA                # FUN_400ACC58: headphones present (getter source of 51 29 7F)
MIDI_PC = 0x401475CC                 # FUN_401475CC(port, channel, program): the MIDI parsers' Program Change
STAGE_FLAG = 0x419156A9              # fn 0x09 staging (sysex_stock.h stk_stage_flag)
WHY = ["none", "set", "stock", "notify", "key", "restore", "power", "rec"]


def s2_diag(side, port=APP):
    """op 0x1B 05 02: the periodic compare and the explained / unexplained counters."""
    out = side.pn.sysex(port, rs.rp.req(rs.OP_FRONT, 0x05, 0x02))
    b = rs.rp.body_of(out, port)
    if not b or b[0] != rs.OP_FRONT | 0x40 or b[1][:2] != b"\x05\x00":
        return None
    d = rs.rp.unpack7(b[1][2:])
    w = lambda i: d[i] | d[i + 1] << 8
    r = dict(layout=d[0], flags=d[1], pause=d[2], last_why=d[3], period=w(4) * 100, ev=le(d, 6),
             passes=w(26), aborts=w(28), postponed=w(30), fanned=w(32), ring=d[34], due=w(36) * 100,
             last_key=bytes(d[38:41]).hex())
    for i, k in enumerate(WHY):
        r[k] = w(10 + 2 * i)
    return r


def ring_diag(side, k=None, port=APP):
    """op 0x1B 05 03 [k]: up to three unexplained differences from the k-th newest: [(key, old, new)], held, total."""
    args = (0x05, 0x03) if k is None else (0x05, 0x03, k)
    out = side.pn.sysex(port, rs.rp.req(rs.OP_FRONT, *args))
    b = rs.rp.body_of(out, port)
    if not b or b[0] != rs.OP_FRONT | 0x40 or b[1][:2] != b"\x05\x00":
        return None
    d = rs.rp.unpack7(b[1][2:])
    ents = []
    for e in range(d[3]):
        x = d[4 + 12 * e:16 + 12 * e]
        ents.append((bytes(x[0:3]).hex(), bytes(x[4:4 + (x[3] >> 4)]).hex(), bytes(x[8:8 + (x[3] & 15)]).hex()))
    return dict(layout=d[0], held=d[1], k=d[2], entries=ents, total=le(d, 40))


def quiet(n, room_ms=1500):
    """run until S2's periodic walk is not running and the next one is at least room_ms away."""
    for _ in range(100):
        d = s2_diag(n.s)
        if d and not d["flags"] & 0x02 and d["due"] >= room_ms:
            return d
        n.run(100)
    return None


def wait_passes(n, k, limit_ms=20000):
    """run until the periodic compare has finished k more passes (None if it did not)."""
    s0 = s2_diag(n.s)
    for _ in range(limit_ms // 250):
        n.run(250)
        s = s2_diag(n.s)
        if s and s["passes"] >= s0["passes"] + k:
            return s
    return None


def delta(a, b):
    return {k: b[k] - a[k] for k in WHY} if a and b else None


def state_s2(m, rows):
    n = NetF(side_settings(m, m.data), pump=True)
    n.pump_log = []
    both_bound(n)
    s0 = wait_passes(n, 1)
    # ---- 16 stock-originated changes ----
    n0 = len(n.tx)
    n.s.poke(HP_STATE, b"\x01")
    n.notify(0x58, [0x01, 0xFF])                 # the engine loop's headphone notice, to its binding (the panel)
    n.task(MIDI_PC, 0, 0, 5)                     # USB-MIDI channel 1 Program Change 5 (stock MIDI parser path)
    s1 = wait_passes(n, 2)
    tx = n.tx[n0:]
    d1 = delta(s0, s1)
    hp_app = [f for f in values(tx, APP) if f[6:9] == b"\x51\x29\x7F"]
    pc_any = [(p, f.hex()) for _t, p, f in tx if f.startswith(VALUE_HEAD) and f[6:8] == b"\x55\x00"]
    ok = check(f"16 S2 stock-originated (headphone notice to the panel + USB-MIDI Program Change): periodic passes "
               f"{s0 and s0['passes']} -> {s1 and s1['passes']} (period {s1 and s1['period']} ms), reasons {d1}, fanned "
               f"+{s1 and s0 and s1['fanned'] - s0['fanned']}; 51 29 frames to the app {len(hp_app)} (tap), 55 00 frames "
               f"{pc_any}",
               s0 is not None and s1 is not None and s1["period"] == 5000 and d1["notify"] == 1 and d1["key"] == 1
               and d1["none"] == 0 and s1["fanned"] == s0["fanned"] and len(hp_app) == 1
               and sorted(p for p, _f in pc_any) == [APP, PANEL] and pc_any[0][1] == pc_any[1][1])
    # S3 always on (fold step 3): the silent stock key 55 00 00 (KEY, explained) is fanned once to every bound controller
    # (kst_key_silent - the S3 rule, roundtrip_fanout 21); S2's own "fanned" (unexplained) count stays
    rows.append(("16", "S2: 스톡발 변경(헤드폰 통지 · USB-MIDI 프로그램 체인지)",
                 f"주기 대조가 51 29 7F(통지) · 55 00 00(스톡 자체 키) 설명됨 2, 설명 안 됨 0; 51 29는 탭으로 앱 1, "
                 f"55 00 00은 S3 켬이라 조용한 키 팬아웃으로 앱 · 패널 1프레임씩"))
    # ---- 17 a hidden direct RAM write ----
    vol = n.b(METRO_CFG + 7)
    new = vol ^ 0x10
    n0 = len(n.tx)
    n.s.poke(METRO_CFG + 7, bytes([new]))         # no path: nobody could know
    s2 = wait_passes(n, 1)
    tx = n.tx[n0:]
    want = bytes.fromhex(f"F0407F10041E56037F{new:02X}F7")
    fa, fp = [f for f in values(tx, APP) if f[6:9] == b"\x56\x03\x7F"], [f for f in values(tx, PANEL) if f[6:9] == b"\x56\x03\x7F"]
    rg = ring_diag(n.s)
    rg1 = ring_diag(n.s, 1)
    n0 = len(n.tx)
    s3 = wait_passes(n, 1)
    again = [f for _t, _p, f in n.tx[n0:] if f.startswith(VALUE_HEAD) and f[6:9] == b"\x56\x03\x7F"]
    d2, d3 = delta(s1, s2), delta(s2, s3)
    ok &= check(f"17 S2 hidden RAM write 56 03 7F {vol:#x} -> {new:#x}: reasons {d2}, fanned +{s2 and s2['fanned'] - s1['fanned']}, "
                f"last key {s2 and s2['last_key']}; app {[f.hex() for f in fa]} panel {[f.hex() for f in fp]}; 1B 05 03 "
                f"{rg}; 1B 05 03 01 {rg1 and rg1['entries']}; next pass {d3}, frames {len(again)}",
                d2 is not None and d2["none"] == 1 and sum(d2.values()) == 1 and s2["fanned"] == s1["fanned"] + 1
                and s2["last_key"] == "56037f" and fa == [want] and fp == [want] and rg is not None and rg["layout"] == 7
                and rg["held"] == 1 and rg["total"] == 1 and rg["entries"] == [("56037f", f"{vol:02x}", f"{new:02x}")]
                and rg1 is not None and rg1["entries"] == [] and d3 is not None and sum(d3.values()) == 0 and not again)
    rows.append(("17", "S2: 숨은 RAM 쓰기(메트로놈 음량 바이트)",
                 f"설명 안 됨 정확히 1(`56 03 7F` {vol:#x} → {new:#x}), store 고침, 앱 · 패널에 1프레임씩, 1B 05 03 고리 1, 다음 바퀴 0"))
    # ---- 18 metronome running, staging pause ----
    n.t20 = True
    p0 = len(n.pump_log)
    cmd_and_run(n, APP, CMD_RUN, 100)
    s4 = s2_diag(n.s)
    s5 = wait_passes(n, 2)
    running = n.b(METRO_CFG)
    cmd_and_run(n, APP, CMD_STOP, 600)
    n.t20 = False
    run_log = n.pump_log[p0:]
    d5 = delta(s4, s5)
    n.s.poke(STAGE_FLAG, b"\x01")
    s6 = s2_diag(n.s)
    n.run(7000)
    s7 = s2_diag(n.s)
    n.s.poke(STAGE_FLAG, b"\x00")
    s8 = wait_passes(n, 1)
    rl = pump_stats(run_log)
    ok &= check(f"18 S2 no false alarms: metronome on {running} for 2 passes ({s4 and s4['passes']} -> {s5 and s5['passes']}) "
                f"reasons {d5}; staging flag up 7 s: passes {s6 and s6['passes']} -> {s7 and s7['passes']}, postponed "
                f"+{s7 and s6 and s7['postponed'] - s6['postponed']}, pause {s7 and hex(s7['pause'])}; down -> passes "
                f"{s8 and s8['passes']}; unexplained in all {s8 and s8['none']}; pump tail while running {rl}",
                running == 1 and d5 is not None and d5["none"] == 0 and s7 is not None and s7["passes"] == s6["passes"]
                and s7["postponed"] > s6["postponed"] and s7["pause"] & 0x04 and s8 is not None
                and s8["passes"] > s7["passes"] and s8["none"] == 1 and rl[1] < 20000 and rl[2] <= 512)
    rows.append(("18", "S2: 메트로놈 실행 · 스테이징 멈춤",
                 f"실행 중 대조 {d5 and s5['passes'] - s4['passes']}바퀴 설명 안 됨 0, 스테이징 동안 대조 없음(미룸 "
                 f"{s7 and s6 and s7['postponed'] - s6['postponed']}), 내리면 다시; 펌프 꼬리 최대 {rl[1]} 명령어 · {rl[2]} B"))
    n.s.close()
    return ok

# ---- Phase 3-4 S3: the settings store answers the requests ----------
S3_NAMES = ("served", "stock", "untrusted", "miss", "stale", "fanned_key")


def s3_diag(side, port=APP):
    """op 0x1B 05 05: the read owner's record (layout 8)."""
    out = side.pn.sysex(port, rs.rp.req(rs.OP_FRONT, 0x05, 0x05))
    b = rs.rp.body_of(out, port)
    if not b or b[0] != rs.OP_FRONT | 0x40 or b[1][:2] != b"\x05\x00":
        return None
    d = rs.rp.unpack7(b[1][2:])
    r = dict(layout=d[0], flags=d[1], gen=le(d, 28), clean_gen=le(d, 32), ev=le(d, 36), passes=le(d, 40))
    for i, k in enumerate(S3_NAMES):
        r[k] = le(d, 4 + 4 * i)
    return r


BOOT_ON = False   # main(): the payload's images boot_on


def s3_set(side, on, port=APP):
    """op 0x1B 05 04 <0|1> - the S3 switch before fold step 3; from ngver 3 refused (rc 1): rc."""
    out = side.pn.sysex(port, rs.rp.req(rs.OP_FRONT, 0x05, 0x04, on))
    b = rs.rp.body_of(out, port)
    return b[1][1] if b and b[0] == rs.OP_FRONT | 0x40 and b[1][:1] == b"\x05" else None


def until_trusted(n, limit_ms=8000):
    """run until the store is trusted (op 1B 05 05 flag 0x04, no walk running): ms waited, None if it was not."""
    for k in range(limit_ms // 50 + 1):
        d = s3_diag(n.s)
        if d and d["flags"] & 0x04 and not d["flags"] & 0x20:
            return 50 * k
        n.run(50)
    return None


def s3_delta(a, b):
    return {k: b[k] - a[k] for k in S3_NAMES} if a and b else None


def answer(n, body, port=APP):
    return values(rss.request(n, port, body), port)


# single-key requests, one of each FUN_401526DC / ss_value branch the store answers or hands to the stock: part-specific
# (Sound part 00 = the stock's, part 00 of a settings key, a system part 0x0F, a VT slot 0x30), single 7F keys, the side table
# (54 05 0F, EQ bands 51 1A / 51 1B), the unknown key 55 01 1F, run state (song tempo, metronome run, headphones)
S3_KEYS = [[0x55, 0x00, 0x00], [0x55, 0x08, 0x00], [0x50, 0x0F, 0x00], [0x51, 0x0B, 0x7F], [0x56, 0x01, 0x7F],
           [0x56, 0x02, 0x7F], [0x54, 0x05, 0x0F], [0x51, 0x1A, 0x7F], [0x51, 0x1B, 0x7F], [0x55, 0x01, 0x1F],
           [0x52, 0x02, 0x7F], [0x56, 0x08, 0x7F], [0x51, 0x29, 0x7F], [0x5A, 0x00, 0x30]]


def state_s3(m, rows):
    # S: the stock alone (the app bound): the bytes every answer below must equal
    s = NetF(side_settings(m, None))
    connect(s, APP, full=False)
    s.run(50)
    ref_s = answer(s, [0x5F, 0x7F, 0x7F])
    keys_s = [answer(s, k) for k in S3_KEYS]
    s.s.close()

    n = NetF(side_settings(m, m.data), pump=True)
    n.pump_log = []
    both_bound(n)
    wait_passes(n, 1)
    d0 = s3_diag(n.s)                                      # the boot state: on (always, from fold step 3)
    rc_off = s3_set(n.s, 0)                                # the old off op: refused
    w0 = until_trusted(n)
    a = s3_diag(n.s)
    p0 = len(n.pump_log)
    on = answer(n, [0x5F, 0x7F, 0x7F])
    b = s3_diag(n.s)
    keys_on = [answer(n, k) for k in S3_KEYS]
    c = s3_diag(n.s)
    dump_log = pump_stats(n.pump_log[p0:])
    dd, dk = s3_delta(a, b), s3_delta(b, c)
    # the app CONNECTs again (the stock binds it now: FUN_4015DE98 copies the VT ctx - those values change, the stock's)
    # and syncs: CONNECT + identity change run state only - the store stays trusted and answers; only the VT keys differ
    # from the stock alone's boot dump (the stock's own VT ctx copy)
    n0 = len(n.tx)
    ok_c, _r = connect(n, APP, full=True)
    re_dump = values(n.tx[n0:], APP)
    e = s3_diag(n.s)
    de = s3_delta(c, e)
    moved = sorted({f[6:9].hex() for f, g in zip(re_dump, ref_s) if f != g})
    bad_keys = [i for i, (x, y) in enumerate(zip(keys_on, keys_s)) if x != y]
    ok = check(f"19 S3 read owner: record at boot {d0 and hex(d0['flags'])} (on expected), the old off op 1B 05 04 00 rc "
               f"{rc_off}, trusted after {w0} ms; full dump "
               f"{len(on)} values == the stock alone {on == ref_s} ({len(ref_s)}); per value read "
               f"{dd}; {len(S3_KEYS)} single-key requests == the stock alone {bad_keys or 'all'} "
               f"({[len(k) for k in keys_on]}, {dk}); app re-CONNECT + dump ({len(re_dump)}), "
               f"{de}, flags {e and hex(e['flags'])}, keys that differ from the stock alone's boot dump {moved}; pump tail "
               f"during the dump {dump_log}",
               d0 is not None and d0["layout"] == 8 and d0["flags"] & 0x03 == 0x03
               and rc_off == 1 and w0 is not None
               and len(ref_s) == 239 and on == ref_s and dd is not None and dd["served"] >= 140
               and dd["untrusted"] == 0 and dd["miss"] == 1 and dd["stale"] == 0
               and keys_on == keys_s and not bad_keys and sum(len(k) for k in keys_on) >= 20
               and dk["served"] > 0 and dk["stock"] > 0 and dk["untrusted"] == 0
               and ok_c and len(re_dump) == 239 and de["served"] >= 140 and de["untrusted"] == 0
               and e["flags"] & 0x04 and all(0x30 <= int(k[4:], 16) <= 0x3F for k in moved))
    rows.append(("19", "S3: 요청 답을 store에서",
                 f"전체 덤프 {len(on)}값 = 스톡 단독(바이트 · 순서) — store가 답함 {dd and dd['served']} · 스톡 "
                 f"{dd and dd['stock']} · store에 없음 {dd and dd['miss']}; 키 하나 요청 {len(S3_KEYS)}개 = 스톡; 앱 다시 "
                 f"CONNECT 뒤 덤프도 store {de and de['served']}"))

    # ---- 20 a set with side effects: the stock answers until a clean walk ran after it, then the store again ----
    quiet(n)
    f0 = s3_diag(n.s)
    set_and_wait(n, PANEL, [0x55, 0x00, 0x00, 0x00, 0x05, 0x00], ms=0)   # Sound of part 00: its effects follow it
    g = s3_diag(n.s)
    early = answer(n, [0x55, 0x7F, 0x7F])                  # starts at once: the first values from the stock
    h = s3_diag(n.s)
    w_early = until_trusted(n)
    i_ = s3_diag(n.s)
    late = answer(n, [0x55, 0x7F, 0x7F])
    j_ = s3_diag(n.s)
    quiet(n)
    set_and_wait(n, PANEL, [0x55, 0x00, 0x00, 0x00, 0x00, 0x00], ms=0)   # back: how long until the store answers again
    w1 = until_trusted(n)
    ref55 = [f for f in ref_s if f[6] == 0x55]
    dh, dj = s3_delta(g, h), s3_delta(i_, j_)
    ok &= check(f"20 S3 after a set: flags {g and hex(g['flags'])} (gen {f0 and f0['gen']} -> {g and g['gen']}); 02 55 7F 7F "
                f"at once {len(early)} values {dh}; once trusted again {len(late)} values {dj}; early (the stock getter while "
                f"untrusted) == late (the store) {early == late}, changed from the boot dump {early != ref55}; the next set: trusted again "
                f"{w1} ms after its ACK",
                g is not None and not g["flags"] & 0x04 and g["gen"] > f0["gen"] and dh is not None
                and dh["untrusted"] > 0 and w_early is not None and dj["served"] > 0 and dj["untrusted"] == 0
                and early == late and len(early) == len(ref55) and early != ref55
                and w1 is not None and w1 <= 1500)
    rows.append(("20", "S3: 부수 효과 있는 set 뒤",
                 f"바로 읽기는 스톡이 답함(못 믿음 {dh and dh['untrusted']}, 덤프 도중 깨끗한 바퀴가 끝나면 store {dh and dh['served']}), "
                 f"set ACK 뒤 {w1} ms면 다시 store; 바이트 = 스톡 답"))

    # ---- 21 the stock's own changes: Program Change read through + fanned out; a hidden write = stale, counted ----
    quiet(n)
    k0 = s3_diag(n.s)
    before_pc = answer(n, [0x55, 0x00, 0x00])
    n0 = len(n.tx)
    n.task(MIDI_PC, 0, 0, 7)                                   # USB-MIDI channel 1 Program Change 7: 55 00 00, nobody told
    now_pc = [f for f in answer(n, [0x55, 0x00, 0x00]) if f[6:9] == b"\x55\x00\x00"]
    before_pc = [f for f in before_pc if f[6:9] == b"\x55\x00\x00"]
    wait_passes(n, 1)
    k1 = s3_diag(n.s)
    pc = [(p, f) for _t, p, f in n.tx[n0:] if f.startswith(VALUE_HEAD) and f[6:9] == b"\x55\x00\x00"]
    pc_panel = [f for p, f in pc if p == PANEL]
    fan_app = [f for p, f in pc if p == APP][1:]              # after the request's own answer
    quiet(n)
    vol = n.b(METRO_CFG + 7)
    n.s.poke(METRO_CFG + 7, bytes([vol ^ 0x08]))               # no path at all: the store still holds the old value
    stale_v = answer(n, [0x56, 0x03, 0x7F])
    wait_passes(n, 1)
    k2 = s3_diag(n.s)
    fixed_v = answer(n, [0x56, 0x03, 0x7F])
    d1, d2 = s3_delta(k0, k1), s3_delta(k1, k2)
    ok &= check(f"21 S3 the stock's own changes: Program Change -> 02 55 00 00 at once {[f.hex() for f in now_pc]} (before "
                f"{[f.hex() for f in before_pc]}); the next pass fans it to the app {[f.hex() for f in fan_app]} / panel "
                f"{[f.hex() for f in pc_panel]} ({d1}); hidden write 56 03 7F {vol:#x} -> {vol ^ 8:#x}: answered "
                f"{[f[9] for f in stale_v]} before the pass, {[f[9] for f in fixed_v]} after ({d2})",
                len(now_pc) == 1 and now_pc != before_pc and d1 is not None and d1["fanned_key"] == 1
                and fan_app == now_pc and pc_panel == now_pc and d1["stale"] == 0
                and [f[9] for f in stale_v] == [vol] and [f[9] for f in fixed_v] == [vol ^ 8] and d2["stale"] == 1)
    rows.append(("21", "S3: 스톡 자체 변경",
                 f"프로그램 체인지 → `55 00 00` 바로 스톡 값(통과 읽기), 다음 바퀴에 앱 · 패널 1프레임씩(조용한 키 팬아웃); 숨은 "
                 f"RAM 쓰기는 다음 바퀴까지 옛 값(낡음 {d2 and d2['stale']}로 셈) → 그 뒤 새 값"))
    n.s.close()
    return ok


# ---- Phase 3-4 S4: the core writes first -------------------------------------
S4_NAMES = ("sets", "values", "own", "refused", "passed", "followed", "wmiss", "kept", "side", "dropped")


def s4_diag(side, sub=0x07, arg=None, port=APP):
    """op 0x1B 05 07 (or 06 <arg>): rc + the write owner's record (layout 9)."""
    out = side.pn.sysex(port, rs.rp.req(rs.OP_FRONT, 0x05, sub, *([] if arg is None else [arg])))
    b = rs.rp.body_of(out, port)
    if not b or b[0] != rs.OP_FRONT | 0x40 or b[1][:1] != b"\x05":
        return None
    d = rs.rp.unpack7(b[1][2:])
    r = dict(rc=b[1][1], layout=d[0], flags=d[1], refused_key=bytes(d[24:27]).hex(), miss_key=bytes(d[27:30]).hex(),
             gen=le(d, 40))
    for i, k in enumerate(S4_NAMES):
        r[k] = d[4 + 2 * i] | d[5 + 2 * i] << 8
    return r


def s4_delta(a, b):
    return {k: b[k] - a[k] for k in S4_NAMES if b[k] != a[k]} if a and b else None


def state_s4(m, rows):
    """25: S3 + S4 on (from boot), app and panel bound - a panel set of a pure key leaves the store trusted: the app's
    request right after the ACK is answered from the store with the new value; a value the gate refuses (one byte short)
    is ACKed like the stock and changes nothing.  26: a set of a key that is not pure (Sound of part 00) - the S3 rule
    (untrusted until a clean walk); the old off op 1B 05 06 00 is refused and S4 stays on."""
    n = NetF(side_settings(m, m.data), pump=True)
    both_bound(n)
    wait_passes(n, 1)
    on3 = s3_diag(n.s)
    on = s4_diag(n.s)
    w0 = until_trusted(n)
    quiet(n)
    a, a3 = s4_diag(n.s), s3_diag(n.s)
    tx = set_and_wait(n, PANEL, [0x51, 0x0B, 0x7F, 0x21], ms=0)       # speaker volume 0x21 from the panel (pure)
    b3 = s3_diag(n.s)
    got = answer(n, [0x51, 0x0B, 0x7F])                                # the app asks right after the ACK
    c, c3 = s4_diag(n.s), s3_diag(n.s)
    fan_app = [f for f in values(tx, APP) if f[6:9] == b"\x51\x0B\x7F"]
    d, d3 = s4_delta(a, c), s3_delta(a3, c3)
    # the gate: the same key one byte short - ACKed (the front, like the stock), nothing applied, value unchanged
    until_trusted(n)
    quiet(n)
    e = s4_diag(n.s)
    tx2 = set_and_wait(n, PANEL, [0x51, 0x0B, 0x7F, 0x30, 0x30], ms=50)
    got2 = answer(n, [0x51, 0x0B, 0x7F])
    f_ = s4_diag(n.s)
    d2 = s4_delta(e, f_)
    ok = check(f"25 S4 write owner: from boot S3 flags {on3 and hex(on3['flags'])}, S4 1B 05 07 {on and (on['rc'], hex(on['flags']))}, "
               f"trusted after {w0} ms; "
               f"panel speaker volume 21 -> trust kept (flags {b3 and hex(b3['flags'])}, gen {a3 and a3['gen']} -> {b3 and b3['gen']}), "
               f"the app's 02 51 0B 7F at once {[x.hex() for x in got]} (S3 {d3}), S4 {d}, the app "
               f"was told {len(fan_app)}; short value 30 30: ACK {len(ack_frames(tx2, PANEL))}, answer {[x[9] for x in got2]}, S4 {d2}",
               on3 is not None and on3["flags"] & 0x03 == 0x03
               and on is not None and on["rc"] == 0 and on["flags"] & 0x03 == 0x03 and w0 is not None
               and b3 is not None and b3["flags"] & 0x04 and b3["gen"] == a3["gen"]
               and len(got) == 1 and got[0][9] == 0x21 and d3 is not None and d3["served"] == 1
               and d3["untrusted"] == 0 and d == dict(sets=1, values=1, own=1, followed=1, kept=1) and len(fan_app) == 1
               and len(ack_frames(tx2, PANEL)) == 1 and [x[9] for x in got2] == [0x21]
               and d2 == dict(sets=1, values=1, refused=1, dropped=1))
    rows.append(("25", "S4: 순수 키 set 뒤 바로 읽기",
                 f"패널 스피커 음량 set → 믿음 유지, 앱 요청이 곧바로 store에서(새 값); 한 바이트 짧은 값은 ACK만, 적용 · 값 변화 없음"))

    # ---- 26 a set that is not pure; the old off op refused ----
    until_trusted(n)
    quiet(n)
    g, g3 = s4_diag(n.s), s3_diag(n.s)
    set_and_wait(n, PANEL, [0x55, 0x00, 0x00, 0x00, 0x05, 0x00], ms=0)
    h, h3 = s4_diag(n.s), s3_diag(n.s)
    w1 = until_trusted(n)
    off = s4_diag(n.s, 0x06, 0)
    dg = s4_delta(g, h)
    ok &= check(f"26 S4 Sound of part 00 (not pure): S4 {dg}, trust dropped (flags {h3 and hex(h3['flags'])}, gen "
                f"{g3 and g3['gen']} -> {h3 and h3['gen']}), trusted again after {w1} ms; the old off op 1B 05 06 00 -> rc "
                f"{off and off['rc']}, S4 still on (flags {off and hex(off['flags'])})",
                dg is not None and dg.get("sets") == 1 and dg.get("own") == 1 and "kept" not in dg
                and h3 is not None and not h3["flags"] & 0x04 and h3["gen"] > g3["gen"] and w1 is not None
                and off is not None and off["rc"] == 1 and off["flags"] & 0x03 == 0x03)
    rows.append(("26", "S4: 순수하지 않은 set · 옛 끄기 op",
                 f"Sound 파트 00 set → S3 규칙(못 믿음 → 깨끗한 바퀴 뒤 {w1} ms 믿음); 옛 끄기 op `1B 05 06 00`은 rc 1, S4 켬 그대로"))
    n.s.close()
    return ok


ss = rtlib.load("stock_songs")


def songs_in(side):
    """The synthetic RecorderData (tools/stock_songs.py LAYOUT, never the owner's dump) before the first pump pass: from
    ota_main feat2 0x10 the core judges SongSelect (recctl) and refuses it on a table that fails the
    boot check - the emulator's RAM there is not a song table."""
    side.emu.write_bytes(ss.REC, ss.store(ss.LAYOUT))
    side.emu.write_bytes(ss.REC + ss.REC_END, b"\x5A\xA5")
    return side


def side_plain(m, data):
    """rs.Side (the loader boot, then the payload applied when data): a fresh() copy when main registered "H" / "S"."""
    key = "H" if data is not None else "S"
    if key in rtlib._BUILDERS and data in (None, m.data):
        return rtlib.fresh(key)
    return rs.Side(m.image, m.syms, data)


def h_net(m):
    side = songs_in(side_plain(m, m.data))
    return NetF(side, pump=True)


def s_net(m):
    return NetF(side_plain(m, None))


def both_bound(n):
    connect(n, APP, full=True)
    connect(n, PANEL, full=False)
    n.run(100)


def stock_answer(m, body):
    """S, the app alone: set body, then 02 55 7F 7F - the stock request task's frame for that key."""
    n = s_net(m)
    connect(n, APP, full=False)
    set_and_wait(n, APP, body)
    tx = rss.request(n, APP, [body[0], 0x7F, 0x7F])
    n.s.close()
    return next((f for f in values(tx, APP) if f[6:9] == bytes(body[:3])), None)


def live_swap(m, rows):
    n = h_net(m)
    connect(n, PANEL, full=False)
    connect(n, APP, full=True)            # the app CONNECTs last: it holds the stock binding (field: table 0x04)
    n.run(100)
    boot_inv = [f for _t, p, f in n.tx if f == INVITE]
    before = rss.core_diag(n.s)
    pn = n.s.pn
    rc = rh.apply(pn, rh.T_A)             # apply now, the same slot again: a new copy of the payload, RW zeroed
    st, _ = rh.run_apply(pn)
    pn.tick(2)                            # the new payload's first engine-loop pass: sxc_tick -> init
    n0 = len(n.tx)
    n.run(20)                             # task 19 pump tail: kps_resume
    tx = n.tx[n0:]
    inv_p = [f for _t, p, f in tx if p == PANEL and f == INVITE]
    to_app = [f for _t, p, f in tx if p == APP and f[3:4] == b"\x7E"]
    core0 = rss.core_diag(n.s)
    raw3 = diag(n.s, 0x03, APP)
    # the panel's JS on a piano-sent 7E 01 01: CONNECT + identity + full parameter request
    ok_c, reply = connect(n, PANEL, full=True)
    core1 = rss.core_diag(n.s)
    a = set_and_wait(n, APP, [0x55, 0x08, 0x00, 0x03])
    b = set_and_wait(n, PANEL, [0x55, 0x07, 0x00, 0x01])   # reverb on (off by default)
    ok_a, _r = connect(n, APP, full=False)   # the app re-CONNECTs too (its own reconnect after the apply)
    n.run(100)
    core2 = rss.core_diag(n.s)
    disp = [(p, f.hex()) for _t, p, f in n.tx[n0:] if f == DISPLACE]
    all_inv = [f for _t, p, f in n.tx if f == INVITE]
    ok = check(f"11 live swap: before table {before and hex(before['bound'])} binding {before and before['binding']}, "
               f"invitations at the first apply {len(boot_inv)}; apply rc {rc} state {st['ap_state']}; new core table "
               f"{core0 and hex(core0['bound'])}, invitation to the panel {len(inv_p)}, 7E frames to the app {len(to_app)}, "
               f"1B 03 byte 46 {raw3 and hex(raw3[46])}; panel re-CONNECT reply {reply and reply.hex()} -> table "
               f"{core1 and hex(core1['bound'])} binding {core1 and core1['binding']}; app set -> panel "
               f"{len(values(a, PANEL))}, panel set -> app {len(values(b, APP))}; app re-CONNECT -> table "
               f"{core2 and hex(core2['bound'])} binding {core2 and core2['binding']}; 7E 01 00 sent {disp}, "
               f"invitations in all {len(all_inv)}",
               before is not None and before["bound"] == 0x0C and before["binding"] == 3 and not boot_inv
               and rc == 0 and st["ap_state"] == rh.AP_DONE and core0 is not None and core0["bound"] == 0x04
               and inv_p == [INVITE] and not to_app and raw3 is not None and raw3[46] == 0x81
               and ok_c and core1 is not None and core1["bound"] == 0x0C and core1["binding"] == 4
               and len(values(a, PANEL)) == 1 and not values(a, APP) and len(values(b, APP)) == 1
               and not values(b, PANEL) and ok_a and core2 is not None and core2["bound"] == 0x0C
               and core2["binding"] == 3 and not disp and len(all_inv) == 1)
    rows.append(("11", "「지금 적용」(라이브 교체), 앱이 스톡 바인딩",
                 f"새 코어 표 {core0 and hex(core0['bound'])} → 패널에 `7E 01 01` {len(inv_p)}번(앱 0) → 패널 다시 "
                 f"CONNECT → 표 {core1 and hex(core1['bound'])}, 팬아웃 양쪽 1프레임씩, `7E 01 00` 0"))
    n.s.close()
    return ok


def fan_core(m, name):
    """Checks 1-10 on one H machine (and S references): (ok, table rows)."""
    ok = True
    rows = []

    # ---- 1 both bound ----
    n = h_net(m)
    both_bound(n)
    core = rss.core_diag(n.s)
    raw3 = diag(n.s, 0x03, PANEL)
    fd = fan_diag(n.s)
    ok &= check(f"1 H {name}: app + panel bound - 1B 03 table {core and hex(core['bound'])}, stock binding "
                f"{core and core['binding']} (panel); 1B 04 layout {fd and fd['layout']} flags {fd and hex(fd['flags'])} "
                f"follows binding {fd and fd['tap_rep']}; 1B 03 byte 45 from the panel {raw3 and hex(raw3[45])}",
                core is not None and core["bound"] == 0x0C and core["binding"] == 4 and fd is not None
                and fd["layout"] == 4 and fd["flags"] == 0x07 and fd["tap_rep"] == 4 and fd["port"] == APP
                and raw3 is not None and raw3[45] == 0x80 | PANEL)

    # ---- 2 app set -> panel ----
    body = [0x55, 0x08, 0x00, 0x04]
    tx = set_and_wait(n, APP, body)
    to_panel, to_app = values(tx, PANEL), values(tx, APP)
    ref = stock_answer(m, body)
    ok &= check(f"2 app set {bytes(body).hex()}: panel got {[f.hex() for f in to_panel]}, app got {len(to_app)} value "
                f"frame(s) + {len(ack_frames(tx, APP))} ACK; the stock request task's frame for that key "
                f"{ref and ref.hex()}",
                len(to_panel) == 1 and to_panel[0][6:10] == bytes(body) and to_panel[0] == ref and not to_app
                and len(ack_frames(tx, APP)) == 1)
    rows.append(("2", "앱 set → 패널", f"패널 1프레임 {to_panel[0].hex() if to_panel else '-'} · 앱 0 (스톡 요청 응답과 같은 바이트)"))

    # ---- 3 panel sets -> app ----
    got = []
    for b in ([0x55, 0x07, 0x00, 0x01], [0x56, 0x01, 0x7F, 0x00, 0x64]):
        tx = set_and_wait(n, PANEL, b)
        got.append((values(tx, APP), values(tx, PANEL)))
    ok &= check(f"3 panel sets 55 07 00 01 (reverb on) / 56 01 7F (tempo 100): app got "
                f"{[[f.hex() for f in a] for a, _p in got]}, panel got {[len(p) for _a, p in got]} value frames back",
                all(len(a) == 1 and not p for a, p in got) and got[0][0][0][6:10] == bytes([0x55, 0x07, 0x00, 0x01])
                and got[1][0][0][6:11] == bytes([0x56, 0x01, 0x7F, 0x00, 0x64]))
    rows.append(("3", "패널 set → 앱", "앱 1프레임씩 · 패널 0"))

    # ---- 4 no change ----
    tx = set_and_wait(n, PANEL, [0x55, 0x07, 0x00, 0x01])
    ok &= check(f"4 a set that changes nothing: {len(values(tx, APP))} / {len(values(tx, PANEL))} value frames "
                f"(app / panel)", not values(tx, APP) and not values(tx, PANEL))

    # ---- 5 stock-originated: headphone ----
    n0 = len(n.tx)
    n.notify(0x58, [0x01, 0xFF])
    n.run(600)
    tx = n.tx[n0:]
    hp_h = (values(tx, PANEL), values(tx, APP))
    s = s_net(m)
    connect(s, APP, full=False)
    connect(s, PANEL, full=False)
    s.run(100)
    s0 = len(s.tx)
    s.notify(0x58, [0x01, 0xFF])
    s.run(600)
    stx = s.tx[s0:]
    hp_s = (values(stx, PANEL), values(stx, APP))
    s.s.close()
    ok &= check(f"5 headphone notice (FUN_4014C048 0x58, engine loop): S panel {len(hp_s[0])} app {len(hp_s[1])} "
                f"(binding only); H panel {len(hp_h[0])} app {len(hp_h[1])}, bytes {hp_h[1] and hp_h[1][0].hex()}",
                [f for f in hp_s[0]] == [HEADPHONE] and not hp_s[1] and hp_h[0] == [HEADPHONE]
                and hp_h[1] == [HEADPHONE])
    rows.append(("5", "헤드폰 통지(스톡발)", f"S 패널 {len(hp_s[0])} · 앱 {len(hp_s[1])} / H 패널 {len(hp_h[0])} · 앱 {len(hp_h[1])}"))

    # ---- 6 SongSelect -> MaxTime ----
    n0 = len(n.tx)
    set_and_wait(n, APP, [0x52, 0x00, 0x7F, 0x00, 0x00, 0x00], ms=50)
    posted = n.run_notify_posts()
    n.run(600)
    tx = n.tx[n0:]
    mt_p = [f for f in values(tx, PANEL) if f[6:8] == b"\x52\x18"]
    mt_a = [f for f in values(tx, APP) if f[6:8] == b"\x52\x18"]
    pr = [(p, f.hex()) for _t, p, f in tx if f[:6] == bytes.fromhex("F0407F32041E")]
    ok &= check(f"6 app SongSelect: {posted} notify post(s); MaxTime 52 18 to the panel {len(mt_p)} (stock), to the "
                f"app {len(mt_a)} (core) {mt_a and mt_a[0].hex()}; fn 0x32 progress relayed {pr}; binding "
                f"{n.b(rs.BINDING)}",
                posted >= 1 and len(mt_p) == 1 and len(mt_a) == 1 and mt_p[0] == mt_a[0]
                and sorted(p for p, _f in pr) == [APP, PANEL] and pr[0][1] == pr[1][1] and n.b(rs.BINDING) == 4)
    rows.append(("6", "앱 SongSelect → MaxTime 통지 · 진행 `32 52 03`",
                 f"패널 {len(mt_p)}(스톡) · 앱 {len(mt_a)}(코어 — 스톡도 바인딩된 설정자에게 보냄), 진행은 즉시 중계"))

    # ---- 7 quick sets, slow panel ----
    n.fixed[PANEL] = 300_000
    n0 = len(n.tx)
    vals_ = [1, 2, 3, 4, 5]
    for v in vals_:
        n.send(APP, ctl(0x10, [0x55, 0x08, 0x00, v]))
        n.until(has(APP, "F0407F07041E550800"), 3000)
        n.run(20)
    n.run(3000)
    tx = n.tx[n0:]
    seq = [f[9] for f in values(tx, PANEL) if f[6:9] == b"\x55\x08\x00"]
    ok &= check(f"7 5 app sets of 55 08 00 with the panel ACKing after 300 ms: panel got values {seq} (in order, last "
                f"= 5, merged), app value frames {len(values(tx, APP))}",
                seq and seq == sorted(seq) and seq[-1] == 5 and len(seq) < 5 and not values(tx, APP))
    rows.append(("7", "빠른 set 5번 · 패널 느림", f"패널 {seq} (순서 · 마지막 값 · 합쳐짐) · 앱 0"))
    del n.fixed[PANEL]

    # ---- 8 change during the app's dump ----
    n.fixed[APP] = 20_000
    n0 = len(n.tx)
    n.at(2500, lambda net: net.send(PANEL, ctl(0x10, [0x55, 0x07, 0x00, 0x00])))
    rss.request(n, APP, rss.FULL)
    tx = n.tx[n0:]
    av = values(tx, APP)
    # the dump sent 55 07 00 01 early (descriptor 0x10); the change 55 07 00 00 comes as one more frame
    change = [i for i, f in enumerate(av) if f[6:10] == bytes([0x55, 0x07, 0x00, 0x00])]
    dump_n = len(av) - len(change)
    fdg = fan_diag(n.s)
    core = rss.core_diag(n.s)
    lens = [len(f) for _t, p, f in n.tx if f.startswith(VALUE_HEAD)]
    ok &= check(f"8 panel set during the app's full dump: app got {dump_n} dump values + End "
                f"{len(rss.ends(tx, APP))}, the change at position {change}; fan frames sent {fdg and fdg['sent']} "
                f"ACKed {fdg and fdg['acks']}, drops {fdg and fdg['drop']}, fails {fdg and fdg['fail']}, core resends "
                f"{core and core['resends']}; longest value frame {max(lens)} B",
                dump_n == 239 and len(rss.ends(tx, APP)) == 1 and len(change) == 1 and 0 < change[0] < len(av) - 1
                and fdg is not None and fdg["sent"] == fdg["acks"] and fdg["drop"] == 0 and fdg["fail"] == 0
                and core["resends"] == 0 and max(lens) <= 40)
    rows.append(("8", "앱 덤프 중 패널 set", f"덤프 {dump_n} + End, 변경은 {change}번째에 끼어듦, 재전송 0"))

    # ---- 9 record ----
    ok &= check(f"9 op 1B 04: sets fanned {fdg['sets']}, unchanged {fdg['same']}, queued {fdg['queued']} (merged "
                f"{fdg['merged']}), sent {fdg['sent']} = ACKed {fdg['acks']}, stock notices on the ring "
                f"{fdg['tap_frames']} fanned {fdg['tap_fanned']}, queues {fdg['pending']}",
                fdg["sets"] >= 5 and fdg["same"] >= 1 and fdg["merged"] >= 1 and fdg["tap_frames"] >= 2
                and fdg["tap_fanned"] >= 2 and fdg["pending"] == [0, 0, 0, 0])

    # ---- 10 the app takes the stock binding back: the core follows it ----
    del n.fixed[APP]
    n.send(APP, ctl(0x7E, [0x01, 0x01]))
    n.until(has(APP, "F0407F7E041E02"), 3000)
    n.run(100)
    n0 = len(n.tx)
    n.notify(0x58, [0x00, 0xFF])
    n.run(600)
    tx = n.tx[n0:]
    tx_p = set_and_wait(n, PANEL, [0x55, 0x08, 0x00, 0x02])
    fd2 = fan_diag(n.s, PANEL)
    ok &= check(f"10 app CONNECTs again (stock binding {n.b(rs.BINDING)}, table {fd2 and hex(fd2['bound'])}, ring "
                f"followed {fd2 and fd2['tap_rep']}): headphone off to the app {len(values(tx, APP))} (stock) and the "
                f"panel {len(values(tx, PANEL))} (core); then a panel set reaches the app {len(values(tx_p, APP))}",
                n.b(rs.BINDING) == 3 and fd2 is not None and fd2["bound"] == 0x0C and fd2["tap_rep"] == 3
                and [f[9] for f in values(tx, APP)] == [0] and [f[9] for f in values(tx, PANEL)] == [0]
                and len(values(tx_p, APP)) == 1 and not values(tx_p, PANEL))
    rows.append(("10", "앱이 스톡 바인딩을 다시 가짐", "헤드폰 통지: 앱(스톡) · 패널(코어) 1번씩, 패널 set → 앱"))
    n.s.close()
    return ok, rows


# ---- 22-23 fn 0x24 file access from a controller that is not the stock binding (field 2026-11-09) ----
def install_fs(e, fs):
    """FUN_401282AC(type, path) replaced by a fake directory table (stock_fn24.py): chdir + read into 0x41045868."""
    def open_dir(em):
        path = em.read_cstr(em.reg(1))
        path = path.decode("latin1") if isinstance(path, (bytes, bytearray)) else path
        ents = fs.get(path)
        if ents is None:
            return 0x56
        em.write_bytes(DIR_DEPTH, bytes([path.count("\\") if path != "\\" else 0]))
        em.write_u32(DIR_TABLE, len(ents))
        em.write_u32(DIR_TABLE + 4, 0)
        for i, (typ, nm) in enumerate(ents):
            em.write_bytes(DIR_TABLE + i * DIR_ENTRY + 0xC, bytes([typ]) + nm.encode("latin1") + b"\x00")
        return 0
    e.hook(OPEN_DIR, open_dir)


def name_packets(name):
    """The app's FileName packets (fileaccess.ts namePackets): 21 raw bytes per packet, 8 -> 7, XOR checksum."""
    raw = list(name.encode("latin1")) + [0]
    out = []
    for c, i in enumerate(range(0, len(raw), 21)):
        g = raw[i:i + 21]
        body = []
        for k in range(0, len(g), 7):
            h = g[k:k + 7]
            body.append(sum(((b & 0x80) >> (j + 1)) for j, b in enumerate(h)))
            body += [b & 0x7F for b in h]
        m = [0xF0, 0x40, 0x7F, 0x24, 0x08, 0x02, 0x03, c, *body]
        x = 0
        for b in m[1:]:
            x ^= b
        out.append(bytes(m + [x & 0x7F, 0xF7]))
    return out


def file_frames(tx, port):
    """fn 0x24 frames and file Results / 7E 03 results the piano sent port"""
    return [f for _t, p, f in tx if p == port and (f.startswith(FILE_HEAD) or f.startswith(bytes.fromhex("F0407F7E041E03"))
                                                   or f == RESULT_OK)]


def file_access(m, rows):
    n = h_net(m)
    both_bound(n)                                       # the app CONNECTs first, the panel last: stock binding 4
    n.run(100)
    e = n.e
    e.unhook(rs.p3.ACK_WAIT)                            # the real FUN_4014B4E0: polls the ACK / NAK flags, 250 ticks
    install_fs(e, FAKE_FS)
    e.hook(FAT_DELETE, lambda em: 0)
    n.s.poke(MEDIA, b"\x01")
    n.file_ack = {APP, PANEL}
    b0 = n.b(rs.BINDING)
    fd0 = fan_diag(n.s)
    # ---- 22 list: the app asks while the panel holds the stock binding ----
    n0 = len(n.tx)
    n.send(APP, ctl(0x24, [0x11]))
    n.run(60)
    ack_app = file_frames(n.tx[n0:], APP)
    bind1, busy1, posts1 = n.b(rs.BINDING), n.b(LIST_BUSY), list(n.file_q)
    # the panel asks meanwhile: Busy from the core, the job stays the app's
    n1 = len(n.tx)
    n.send(PANEL, ctl(0x24, [0x11]))
    n.run(60)
    panel_busy = file_frames(n.tx[n1:], PANEL)
    posts2 = list(n.file_q)
    # the file task runs message 0: the walk + 24 1F, each packet stop-and-wait (FUN_4014B638 / FUN_4014B4E0 real)
    n2 = len(n.tx)
    t_walk = n.t
    walk = n.task(WALK, ROOT)
    n.task(END_OF_LIST, 0x7F)
    walk_ms = (n.t - t_walk) / 1000
    n.s.poke(LIST_BUSY, b"\x00")                       # the file task's own clear after 24 1F (0x40151F14..)
    n.run(80)
    to_app = file_frames(n.tx[n2:], APP)
    names = [f for f in to_app if f[6] == 0x03]
    acks_from_app = sum(1 for _t, p, f in n.acks_in if p == APP)   # (fn 07 only - fn 24 ACKs counted below)
    bind2 = n.b(rs.BINDING)
    n.run(1000)                                         # KPS_FILE_QUIET_MS after the last ACK: the panel's binding back
    bind3 = n.b(rs.BINDING)
    fd1 = fan_diag(n.s)
    to_panel = file_frames(n.tx[n0:], PANEL)
    # the panel is unaffected: a set of the app still reaches it, its own list is the stock's again
    tx_set = set_and_wait(n, APP, [0x55, 0x08, 0x00, 0x04])
    fan_p = [f for f in values(tx_set, PANEL) if f[6:9] == b"\x55\x08\x00"]
    n3, q3 = len(n.tx), len(n.file_q)
    n.send(PANEL, ctl(0x24, [0x11]))
    n.run(60)
    panel_own = file_frames(n.tx[n3:], PANEL)
    app_own = file_frames(n.tx[n3:], APP)
    bind4, posts4 = n.b(rs.BINDING), len(n.file_q) - q3
    n.s.poke(LIST_BUSY, b"\x00")
    leases = (fd1["leases"] - fd0["leases"]) if fd1 and fd0 else None
    ok = check(f"22 fn 0x24 list from the app, the panel holds the stock binding {b0}: the app gets "
               f"{[f.hex() for f in ack_app]} (stock ACK), stock binding {bind1} for the job, list flag {busy1}, file task "
               f"posts {posts1}; the panel's list meanwhile -> {[f.hex() for f in panel_busy]} (core Busy), posts {posts2}; "
               f"walk r0 {walk.r0:#x} ({walk_ms:.0f} ms, each packet sent once = ACKed): to the app {len(names)} FileName + {sum(1 for f in to_app if f[6] == 0x1F)} x 24 1F "
               f"({len(to_app)} frames), binding {bind2} -> {bind3} after 1 s; fn 0x24 frames to the panel "
               f"{[f.hex() for f in to_panel]}; 1B 04 lease {fd1 and hex(fd1['lease'])} leases +{leases}; afterwards an app "
               f"set reaches the panel {len(fan_p)}x, the panel's own list: panel {[f.hex() for f in panel_own]} app "
               f"{len(app_own)} binding {bind4} posts {posts4}",
               b0 == 4 and ack_app == [FILE_ACK] and bind1 == 3 and busy1 == 1 and posts1 == [0]
               and panel_busy == [FILE_ACK, BUSY] and posts2 == [0] and walk.r0 == 0 and len(names) == 4
               and len(set(names)) == 4 and 4 * FILE_ACK_US / 1000 <= walk_ms < 500
               and [f for f in to_app if f[6] == 0x1F] == [bytes.fromhex("F0407F24041E1FF7")] and len(to_app) == 5
               and bind2 == 3 and bind3 == 4 and to_panel == [FILE_ACK, BUSY] and fd1 is not None
               and fd1["lease"] == 0 and leases == 1 and len(fan_p) == 1 and panel_own == [FILE_ACK] and not app_own
               and bind4 == 4 and posts4 == 1)
    rows.append(("22", "앱 USB 목록 `24 11`(패널이 스톡 바인딩)",
                 f"앱: `24 7F` · FileName {len(names)} · `24 1F`(각 앱 ACK), 그동안 스톡 바인딩 {bind1} → 1 s 뒤 {bind3}; 패널: 그 사이 "
                 f"제 목록 요청에 코어 `24 7F` + Busy만, 앱 목록 프레임 0; 뒤에 앱 set 팬아웃 {len(fan_p)} · 패널 제 목록은 스톡 그대로"))
    # ---- 23 a job through the executor: delete by name, the Result goes to the app only ----
    n.run(700)                                           # the panel's list just now: a lease starts after 0.6 s quiet
    n4 = len(n.tx)
    n.send(APP, ctl(0x24, [0x06]))
    n.run(60)
    bind5, op5 = n.b(rs.BINDING), n.b(FILE_OP)
    for pk in name_packets("\\SONG1.MID"):
        n.send(APP, pk)
        n.run(40)
    ready = n.b(NAME_READY)
    n.task(EXECUTOR)                                     # the engine loop's FUN_4014F364 (task 24)
    n.run(60)
    del_app = file_frames(n.tx[n4:], APP)
    bind6 = n.b(rs.BINDING)
    n.run(1000)
    bind7 = n.b(rs.BINDING)
    del_panel = [f for _t, p, f in n.tx[n4:] if p == PANEL and (f.startswith(FILE_HEAD) or f == RESULT_OK
                                                               or f[3:4] == b"\x32" and f[6:8] == b"\x51\x7E")]
    ok &= check(f"23 delete 24 06 + FileName from the app (panel = stock binding): binding {bind5} job {op5:#x} name ready "
                f"{ready}; the app gets {[f.hex() for f in del_app]}; the panel {[f.hex() for f in del_panel]} (the tap does "
                f"not relay 32 51 7E); binding {bind6} -> {bind7}",
                bind5 == 3 and op5 == 6 and ready == 1 and del_app == [FILE_ACK, FILE_ACK, RESULT_OK] and not del_panel
                and bind6 == 3 and bind7 == 4)
    rows.append(("23", "앱 삭제 `24 06` + FileName(패널이 스톡 바인딩)",
                 f"앱: `24 7F` 2 · Result `32 51 7E 7F 00 00`, 패널 0(탭이 `51 7E`를 중계하지 않음), 스톡 바인딩 {bind5} → {bind7}"))
    # ---- 24 the app stops answering mid-list: the stock's 16 failures drop the app, the panel gets its binding back ----
    install_fs(e, {"\\": [(1, "A.MID")]})
    n.file_ack = {PANEL}
    n.send(APP, ctl(0x24, [0x11]))
    n.run(60)
    bind8 = n.b(rs.BINDING)
    n5 = len(n.tx)
    walk = n.task(WALK, ROOT)                            # FUN_4014B638: 16 sends 500 ms apart, then unbind (not 4)
    n.s.poke(LIST_BUSY, b"\x00")
    sends = [f for _t, p, f in n.tx[n5:] if p == APP and f.startswith(FILE_HEAD) and f[6] == 0x03]
    bind9 = n.b(rs.BINDING)
    n.run(100)
    bind10 = n.b(rs.BINDING)
    fd2 = fan_diag(n.s, PANEL)
    tx_set = set_and_wait(n, PANEL, [0x55, 0x08, 0x00, 0x00])
    ok &= check(f"24 the app does not ACK its list: binding {bind8}, {len(sends)} FileName sends (walk r0 {walk.r0:#x}), "
                f"stock binding after the 16th {bind9} -> {bind10} (the panel's back); core table {fd2 and hex(fd2['bound'])}; "
                f"the panel's set still ACKed {len(ack_frames(tx_set, PANEL))}",
                bind8 == 3 and len(sends) == 16 and walk.r0 == 0xF0 and bind9 == 0 and bind10 == 4 and fd2 is not None
                and fd2["bound"] == 0x08 and len(ack_frames(tx_set, PANEL)) == 1)
    rows.append(("24", "앱이 목록 패킷에 ACK 안 함",
                 f"스톡 16회 뒤 바인딩 {bind9}(앱 해제, 스톡 규칙) → 코어가 패널에 되돌림 {bind10}, 코어 표 패널만"))
    n.s.close()
    return ok


def main(argv):
    global BOOT_ON
    name = rtlib.first_arg(argv, "ota_main")
    BOOT_ON = images.OTA_PAYLOADS_BY_NAME[name].boot_on
    if not images.OTA_PAYLOADS_BY_NAME[name].feat2 & images.FEAT2_SESSION:
        raise SystemExit(f"{name}: no session core (feat2 0x02)")
    _img, _stock, _manifest, image, syms = rtlib.loader()
    data = (STOCKFW / "build" / f"{name}.c79").read_bytes()
    m = rss.Machines(image, syms, data)
    ok = True
    rows = []

    rtlib.builders(S=lambda: rs.Side(image, syms, None), H=lambda: rs.Side(image, syms, data),
                   Ss=lambda: _plain_side_settings(image, syms, None), Hs=lambda: _plain_side_settings(image, syms, data))

    def one(fn):
        def run():
            r = []
            return fn(m, r), r
        return run
    # cost = seconds measured 2026-11-02 (only the balance of the shards depends on it)
    units = [rtlib.Unit("1-10", lambda: fan_core(m, name), 10.6, ("H", "S")),
             rtlib.Unit("11", one(live_swap), 7.1, ("H",)),           # live payload swap: the panel is invited back
             rtlib.Unit("12", one(state_s1), 5.0, ("Hs",)),           # S1 store fill + compare (Phase 3-4 S1)
             rtlib.Unit("13", one(metronome), 7.3, ("Hs", "Ss")),     # metronome (Phase 3-5 stage 1)
             rtlib.Unit("14-15", one(remote_cmd), 5.7, ("Hs", "Ss")),  # fn 0x32 from a non-stock-bound controller, BT gate
             rtlib.Unit("16-18", one(state_s2), 21.0, ("Hs",)),       # Phase 3-4 S2: periodic compare, differences
             rtlib.Unit("19-21", one(state_s3), 20.0, ("Hs", "Ss")),  # Phase 3-4 S3: the store answers the requests
             rtlib.Unit("22-24", one(file_access), 12.0, ("H",)),      # fn 0x24 from a non-stock-bound controller
             rtlib.Unit("25-26", one(state_s4), 15.0, ("Hs",))]       # Phase 3-4 S4: the core writes first
    for _u, res in rtlib.run_units(units, argv):
        ok &= res is not None and bool(res[0])
        rows += res[1] if res is not None else []

    print("\n| # | 시나리오 | 결과 |\n|---|---|---|")
    for r in rows:
        print(f"| {r[0]} | {r[1]} | {r[2]} |")
    return rtlib.finish(ok, rtlib.shard_note(units, argv))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
