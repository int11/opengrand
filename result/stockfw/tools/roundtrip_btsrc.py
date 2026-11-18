# roundtrip_btsrc.py - emulator verification of the UART1 send discipline (F6) and the BT audio source manager (F7)
# of the BT port (slot payload ota_main).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_btsrc.py [ota_main] [--old=<payload.c79>] [-q]
#
# Same machine as roundtrip_bt.py (the real ota_loader_v4 image, the payload uploaded and applied live).  The UART1
# transmitter is modelled around the REAL stock code: IER (0x01D0C004) is a register (the stock kick FUN_401450A0
# and THRE handler 0x401444E0 write it), LSR (0x01D0C014) reads THRE = 1, no RX data; THR writes are recorded.
# "The transmitter finishes" = the real UART1 handler 0x40144550 runs (between engine-loop passes).
# Checks (numbered as printed):
#   1  the stock hazard on the real code: FUN_4014512C(1 byte) stopped right before its kick (0x401451C0) while the
#      THRE interrupt is on; the real UART1 handler runs there (= a task switch at that point) and empties the ring;
#      the real kick then sends a stale byte and the TX count wraps to 0xFFFF; the handler sends the stale ring and
#      the count stays at 0xF800; the next FUN_4014512C never returns (busy-wait 0x40145160-0x40145174)
#   2  the payload under the same model, with the task switch injected at EVERY kick point where the THRE
#      interrupt is on: the fixed payload never reaches such a point (every FUN_4014512C call is one byte made with
#      IER bit 1 off, count 0, rd == wr) and its module query completes; --old=<c79>: the 2026-11-01 field payload
#      reaches it during `1D 01 00` and its engine-loop pass never returns (the field stall, reproduced)
#   3  heartbeat 1D 0F is answered inside ng_sysex (task 19, no engine-loop pass) with the pass count
#   4  the opt-in policy "newest wins" (1D 06 mode 1): phone streaming, the PC streams too (= no sound on the module) -
#      module lines through the UART1 RX ring and the real line assembler; 1.5 s later the payload sends CLOSE for the
#      phone's links (A2DP first) through the real FUN_4014512C, nothing else; never `SET `; 1D 04 lists the PC as
#      the playing source
#   5  the default (mode SELECT, the one switch) on the 2026-11-02 field sequence:
#      PC (Windows) streaming, the phone connected -> only the AUTOCALL read `SET`; 「이 기기로 듣기」 for the phone
#      (1D 05 01) -> `SET 5 SELECT` + `AVRCP PAUSE`, no STREAMING STOP (Windows keeps its stream) -> the sink never
#      stops it (2026-11-10: a sink STOP left the Windows audio device silent until its PnP restart; never a
#      CLOSE either: field 2026-11-03); no report 1 s after the PAUSE -> `SET 5 SELECT` + `AVRCP PDU 31 1`; its own
#      STOP -> `SET 2 SELECT` + `AVRCP PLAY`; every byte one FUN_4014512C call made
#      idle; 1D 04 layout 2: the PC still listed with all its links, the phone selected, PLAYING, the codec event
#      (`A2DP CODEC SBC …`) kept with the phone's slot; 1D 06 answers strategy 0, the late stop's defaults (on, wait
#      BTS_XWAIT_DEFAULT 1, D76), 1D 04 byte 5 bits 4-6 = 1
#   6  1D 08 (module line log, developer view): every module line and every command sent, in order, with sequence
#      numbers; read from a sequence number on, reply within the BLE frame budget
#   7  a PC without AVRCP (field 2026-11-02 #2): 4 s after its A2DP (field 2026-11-09; 1.5 s
#      up to a6fdc5a2) the sink opens AVRCP to it (`CALL <pc> 17 AVRCP`, once); `CALL 6` with no CONNECT -> `CLOSE 6`
#      1.5 s later (given up), never called again; 「이 기기로 듣기」 for the PC while the phone plays -> phone PAUSE, its
#      STOP -> 0.5 s -> `SET 3 SELECT` + `A2DP STREAMING START` (the sink starts the PC's stream - field #4 stayed
#      silent here); START 3 -> PLAYING, tries = START
#   8  GUARD: the selected phone paused, the PC starts by itself -> `AVRCP PAUSE` for the PC; still streaming -> never a
#      sink STOP (2026-11-10); 1D 04 marks it refused
#  10  module reboots (field 2026-11-02 #1): the PC's lone AVRCP browsing channel (`RING 6 … 1b AVRCP`) then the iWRAP
#      banner / READY. while powered -> 1D 04 tail: 1 reboot, health SUSPECT; a second one 10 s later -> health LOOP;
#      while it holds the PC (A2DP, no AVRCP) gets no `CALL`; nothing but LIST goes out
#  20  field 2026-11-10: the module's echo of our `A2DP STREAMING START` is no stream event; a PC without
#      AVRCP is never stopped by the sink (the switch waits for its own STOP); the phone the piano paused gets PLAY
#      whatever its stale report, no sink START before it
#  11  field 2026-11-02 afternoon: phone -> a parked Windows PC (its A2DP closed by the user's act 6): phone PAUSE, its
#      stream suspended (links kept), `CALL <pc> 19 A2DP`, the fresh A2DP link streams at once while its player is
#      paused (silence) -> `SET <its AVRCP> SELECT` + `AVRCP PLAY` after the stream is up; back to the phone: PC PAUSE,
#      suspended; the phone (suspended by the sink, paused by us) gets PLAY first - no sink resume (D31, 11-09 evening)
#  12  field 2026-11-02 night: the phone streaming, a Windows PC playing but ignored; 「이 기기로 듣기」 on the PC -> phone
#      PAUSE, suspended, the PC's stream at once -> no AVRCP PLAY (PLAY is play/pause on Windows): with the play status
#      (`AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING`) and without one (never paused by us)
#  13  field 2026-11-03 (1afe1978, test 2-가): #1 as 12; the user's act 6 on the phone (its
#      AVRCP goes with its A2DP - the §9 state); PC -> phone: PC suspended, CALL, the phone's fresh link START + STOP
#      10 ms later is not the end of the switch; the sink opens its AVRCP, INTERIM PAUSED (paused by the piano in #1)
#      -> exactly one `AVRCP PLAY` (1afe1978: ended on the START, no PLAY - the field silence)
#  15  field 2026-11-03 (a12c870a), the one switch (the strategies 1-4 removed): 1D 06
#      with strategy 0 (14 B) accepted; 1 / 4 (an old app's write) refused (rc 1), the policy and the switch as before.
#      D27 PC (YouTube, PLAYING) -> phone (PAUSED): PC PAUSE, the PC keeps its paused stream - never stopped by the sink,
#      the switch waits; its own STOP -> no PLAY (the phone plays), KEPT.  D25: back to the PC - phone PAUSE, its own STOP
#      -> the PC PLAY first.  Never `CLOSE`, never `CALL … 19 A2DP` (every link kept)
#  14  the BT hold follows the play status (owner decision 2026-11-03): a Windows PC
#      streams alone (A2DP 3 / 5, AVRCP 4); the manager registers its play status (`SET 4 SELECT` + `AVRCP PDU 31 1`),
#      INTERIM PLAYING -> the power port holds the APO through the real 20 ms site; CHANGED PAUSED with no STREAMING STOP
#      (Windows keeps its stream for minutes) -> registered again, INTERIM PAUSED -> the hold ends like a STOP (counter 1),
#      14 min later still ON, at 15 min standby; woken, CHANGED PLAYING -> held again; the piano sends the PC no PLAY /
#      PAUSE in all of it
#  16  field 2026-11-09: a - the exact interleaved lines (`SAVRCP 6 … PLAYING` / `ET 1
#      SELECT`, `SETA2DP STREAMING STOP 4`) through the real ring and assembler are read, the guard PAUSEs the phone, an
#      unreadable status line asks again; b - the reconnect choice (1D 06 byte 14) goes to the kvstore in unused_3, comes
#      back at the next power-on on the same NOR, and drives the piano's own CALL with AUTOCALL off (off: none)
#  18  field 2026-11-09 evening: a status line without its link id asks every registered
#      device again; a stream newer than its PAUSED report gets PAUSE and the suspend; another source's late STOP
#      re-sends the sink's START on the target once; picking the selected, streaming device again sends that START
#   9  1D 09 AUTOCALL: the only module-setting writes, exactly `SET CONTROL AUTOCALL` (off) and the line read before /
#      the stock factory line (restore), each followed by a `SET` read back; refused before a read and from a port
#      without the binding
#  17  1D 0A class of device (field 2026-11-09): the only other module-setting writes, exactly
#      `SET BT CLASS 240414` (write) and the class read before (restore, `SET BT CLASS 001f00`), each followed by a `SET`
#      read back; refused before a read, with a class that is not an audio sink one, from a port without the binding and
#      while the 1D 09 read-back is pending; every byte one idle FUN_4014512C call
#  19  1D 0C AVRCP controller features (field 2026-11-10): exactly
#      `SET PROFILE AVRCP CONTROLLER 0f` (write, browsing bit off) and the features read before (restore,
#      `SET PROFILE AVRCP CONTROLLER 1f`), each followed by a `SET` read back; refused before a read, with the browsing
#      bit, from a port without the binding, while the 1D 0A read-back is pending and for a listing with a TARGET part;
#      every byte one idle FUN_4014512C call
#  21  1D 0D audio routing (field 2026-11-11): exactly
#      `SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT` (write, KEEPALIVE off), `… EVENT KEEPALIVE` (write 3) and the line
#      read before (restore, `… KEEPALIVE`), each followed by a `SET` read back; refused before a read, with KEEPALIVE
#      alone, from a port without the binding, while the 1D 0C read-back is pending and for another listed form; the
#      EVENT lines (`A2DP CODEC …`, `AUDIO ROUTE <l> …`) around a stream change no stream and send nothing; every byte one
#      idle FUN_4014512C call
#  22  the opt-in late stop (field 2026-11-11): 1D 06 options byte 1 ->
#      the left PC that reports PAUSED and keeps its stream gets `SET 0 SELECT` + `A2DP STREAMING STOP` 4.5 s after the
#      PAUSE (nothing at 3.2 s), then the phone its PLAY; back to the PC: PLAY, no sink START, no STOP to the phone that
#      let go by itself; options 0 -> 1D 04 byte 116 bit 7 clear (the wait 4.5 s set explicitly since then - it
#      counts from the PAUSED report now)
#  23  the late stop's wait: 1D 06 byte 16 (wait 10 = 1.0 s) -> 19 B
#      reply, 1D 04 byte 119; an old 16 B write keeps it, 101 refused.  PC -> phone: the PC reports PAUSED and holds ->
#      `SET 0 SELECT` + `A2DP STREAMING STOP` 1.0 s after the report (not at 0.8 s), STOP 0 -> `SET 5 SELECT` + `AVRCP
#      PLAY`; the phone's START 1.4 s after the PLAY -> PLAYING (not READY).  Wait 5: phone -> PC: the phone reports
#      PAUSED and holds (Android ~3 s) -> `SET 3 SELECT` + `A2DP STREAMING STOP` 0.5 s after the report, STOP 3 ->
#      `SET 2 SELECT` + `AVRCP PLAY`; no START within 2.5 s -> READY, its START then -> PLAYING; never a sink START
#  24  live apply over two connected sources (field 2026-11-11): the payload applied from the
#      built-in finds no handover block (a cold start of the BT port); with the PC and the phone connected, the phone
#      selected and streaming, the same payload is uploaded to slot B and applied live through the real resident:
#      nothing reaches UART1 during the apply and no stock module power routine (FUN_400AD030 / FUN_400AD098 /
#      FUN_40146810) runs, the block at 0x4019FFC0 is taken and cleared; a - the module kept its links (LIST shows them):
#      only LIST, the play-status registrations and the AUTOCALL read go out, the phone is still selected and streaming,
#      no CALL / CLOSE; b - the links dropped (LIST 0, reconnect setting off): `CALL <phone> 19 A2DP` first, then
#      `CALL <PC> 19 A2DP` once the phone is back, both connected again and the phone still selected, no CLOSE
#  25  the decided module values kept at power-on (user decision 2026-11-13): the module's
#      listing with the decided lines -> the one `SET` read, nothing written, no stock module power routine; the factory
#      listing -> `SET BT CLASS 240414`, `SET PROFILE AVRCP CONTROLLER 0f`, `SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT`
#      (each + its `SET` read back), ONE restart through the real FUN_400AD030 / FUN_400AD098, LIST + the listing read
#      back, nothing more.  Since §27 the manager reads the listing at every power-on: 2 allows that read after the
#      query; 17 / 19 / 21 answer it with the decided lines first (the pin done) and then test their ops on the factory
#      listings as before
#  26  standby that switched the module off, then the wake (user decision 2026-11-14): the
#      power policy's 「대기 중 BT 오디오 켜 두기」 off, the phone connected and picked, reconnect 「선호 · 마지막」, AUTOCALL
#      read off; standby by 19 01 / 19 03 / the APO (the real 20 ms site) -> the real FUN_400AD030; wake 19 02 -> the real
#      FUN_400AD098 + FUN_40146810, LIST, `CALL <phone> 19 A2DP` 4 s after the module start (as at power-on, 16b);
#      reconnect off / AUTOCALL on -> no CALL; the module kept on (TV wake) -> no power routine, no LIST, no CALL
#  27  a live apply with no handover block: the payload keeps the decided module values, then the
#      same payload is applied live to slot B with btc_handover skipped (as from the built-in, a payload before §25 or a
#      stale block) while the app holds the binding; the module lists the factory lines -> the three lines written, but
#      no stock module power routine and nothing after (§27 「a live apply writes but never restarts」 - before: a restart)
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rbt = rtlib.load("roundtrip_bt")
rpw = rbt.rpw
rp, rt, images, pa = rbt.rp, rbt.rt, rbt.images, rbt.pa
check, le, Rig, upload_apply = rbt.check, rbt.le, rbt.Rig, rbt.upload_apply
STOCKFW, BINDING = rbt.STOCKFW, rbt.BINDING
TX_RING, TX_RD, TX_WR, TX_CNT, BT_TX = rbt.TX_RING, rbt.TX_RD, rbt.TX_WR, rbt.TX_CNT, rbt.BT_TX
OP = rbt.OP
IER, LSR, THR = 0x01D0C004, 0x01D0C014, 0x01D0C000
KICK, KICK_SITE, UART1_HANDLER = 0x401450A0, 0x401451C0, 0x40144550
SPIN = (0x40145160, 0x40145178)


def late_off(rig):
    """the policy as at power-on but the late stop off (on by default since D76): the
    scenarios that check the switch waiting for a source's own STOP - the late stop itself is 22 / 23"""
    rig.raw_reply(2, rp.req(OP, 0x06, 2, 0x04, 15, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0), OP)


class Uart1Tx:
    """UART1 transmitter model: IER a register, LSR = THRE, THR writes recorded."""

    def __init__(self, emu):
        self.emu = emu
        self.ier = 0
        self.thr = bytearray()
        emu.periph[IER] = lambda e, a, s: self.ier
        emu.periph[LSR] = 0x20
        emu.on_periph_write(IER, IER + 1, self._w_ier)
        emu.on_periph_write(THR, THR + 1, self._w_thr)

    def _w_ier(self, e, a, s, v):
        self.ier = v & 0xFF

    def _w_thr(self, e, a, s, v):
        self.thr.append(v & 0xFF)

    def finish(self):
        """The transmitter drains: the real UART1 handler (THRE part) runs."""
        res = self.emu.call(UART1_HANDLER, max_insns=2_000_000)
        return res

    def state(self):
        e = self.emu
        return le(e.read_bytes(TX_CNT, 2), 0, 2), le(e.read_bytes(TX_RD, 2), 0, 2), le(e.read_bytes(TX_WR, 2), 0, 2), self.ier

    def model_isr(self):
        """0x401444E0 as Python (check 1 shows the real one does exactly this): send while rd != wr, then IER bit 1
        off.  Used inside a hook, where the emulator cannot run a second call."""
        e = self.emu
        cnt, rd, wr, _ = self.state()
        while rd != wr:
            self.thr.append(e.read_bytes(TX_RING + rd, 1)[0])
            rd = (rd + 1) & 0x7FF
            cnt = (cnt - 1) & 0xFFFF
        e.write_bytes(TX_RD, rd.to_bytes(2, "little"))
        e.write_bytes(TX_CNT, cnt.to_bytes(2, "little"))
        self.ier &= ~0x02


def run_query(rig, uart, inject):
    """1D 01 00 with module answers; the transmitter drains after every pass.  inject: at every kick point with the
    THRE interrupt on, the THRE handler runs first (a task switch there).  Returns (completed, hazards, calls)."""
    emu = rig.emu
    hazards, calls = [], []

    def at_kick(e):
        if uart.ier & 0x02:
            hazards.append(uart.state())
            if inject:
                uart.model_isr()
        return rt.se.StockEmu.CONTINUE

    def at_tx(e):
        calls.append((e.reg(1), uart.state()))

    emu.hook(KICK_SITE, at_kick)
    emu.hook(BT_TX, lambda e: (at_tx(e), rt.se.StockEmu.CONTINUE)[1])
    done = True
    try:
        rc = rbt.op_rc(rig, 0x01, 0x00)
        for i in range(900):
            rig.tick()
            uart.finish()
            if i == 3:
                rig.bt_line("SET BT NAME CA79 Audio")
                rig.bt_line("SET")
            if i == 120:
                rig.bt_line("LIST 0")
        st = rbt.status(rig)
        done = rc == 0 and st is not None and st["q"] == 3
    except RuntimeError as ex:
        done = False
        calls.append(("stopped", str(ex)[:80]))
    finally:
        emu.unhook(KICK_SITE)
        emu.unhook(BT_TX)
    return done, hazards, calls


def fresh_rig(image, syms, data):
    rig = Rig(image, syms)
    uart = Uart1Tx(rig.emu)
    rbt.Hier(rig.emu)
    upload_apply(rig, data)
    rig.emu.write_bytes(BINDING, b"\x03")
    rig.tick(2)
    return rig, uart


def main(argv):
    img, _stock, _manifest, image, syms = rtlib.loader()
    which = rtlib.first_arg(argv, "ota_main")
    old = next((a.split("=", 1)[1] for a in argv[1:] if a.startswith("--old=")), None)
    data = rtlib.payload(which).data
    ok = True
    print(f"payload {which}  {len(data) - 64} B  sha256 {pa.sha256(data)}  on {img.name}")

    # ---- 1. the stock hazard ---------------------------------------------------------------------
    rig = Rig(image, syms)
    emu = rig.emu
    uart = Uart1Tx(emu)
    rbt.Hier(emu)
    for a in (TX_RD, TX_WR, TX_CNT):
        emu.write_bytes(a, b"\x00\x00")
    emu.write_bytes(TX_RING, b"OLDRING-" * 256)      # what earlier commands left in the ring
    emu.write_bytes(0x40B90100, b"X")
    uart.ier = 0x02                                  # the previous byte is still going out: THRE interrupt on
    r1 = emu.call(BT_TX, 0x40B90100, 1, until=KICK_SITE, max_insns=200_000)
    put = uart.state()
    h1 = uart.finish()                               # the task switch at 0x401451C0: the real UART1 handler runs
    after_isr = uart.state()
    k = emu.call(KICK, max_insns=200_000)            # the real kick, with the ring already empty
    after_kick = uart.state()
    uart.finish()                                    # the transmitter goes on: the stale ring
    after_drain = uart.state()
    r2 = emu.call(BT_TX, 0x40B90100, 1, max_insns=3_000_000)
    pcs = r2.stop
    ok &= check("1 stock hazard on the real code: put (count 1) -> UART1 handler empties the ring, IER bit 1 off -> kick "
                "sends a stale byte, count 0xFFFF, rd = wr + 1 -> the handler sends 2047 stale bytes, count 0xF800 -> the "
                "next FUN_4014512C never returns (busy-wait)",
                put[0] == 1 and h1.ok and after_isr[0] == 0 and after_isr[1] == after_isr[2] and not after_isr[3] & 2
                and k.ok and after_kick[0] == 0xFFFF and after_kick[1] == (after_kick[2] + 1) & 0x7FF
                and after_drain[0] == 0xF800 and pcs in ("max_insns", "no_progress") and SPIN[0] <= r2.pc < SPIN[1],
                f"put {put} isr {after_isr} kick {after_kick} drain {after_drain} next {pcs} r1 {r1.stop}")
    rig.close()

    # ---- 2. the payload under the model -----------------------------------------------------------
    rig, uart = fresh_rig(image, syms, data)
    done, hz, calls = run_query(rig, uart, inject=True)
    one = [c for c in calls if c[0] != "stopped"]
    clean = all(n == 1 and s[0] == 0 and s[1] == s[2] and not s[3] & 2 for n, s in one)
    ok &= check(f"2 {which}: the module query completes with the task switch injected at every risky kick point - there is "
                f"none: {len(one)} FUN_4014512C calls, each 1 byte with count 0, rd == wr, IER bit 1 off; the THR got "
                "SET / LIST / INFO (then only the source manager's LIST and its power-on `SET` read)",
                done and not hz and clean and bool(one) and bytes(uart.thr).startswith(b"SET\r\nLIST\r\nINFO\r\n")
                and set(bytes(uart.thr)[17:].split(b"\r\n")) <= {b"LIST", b"SET", b""},   # + the power-on read (§27)
                f"done {done} hazards {len(hz)} calls {len(one)} clean {clean} thr {bytes(uart.thr)!r}")
    rig.close()
    if old:
        odata = pathlib.Path(old).read_bytes()
        rig, uart = fresh_rig(image, syms, odata)
        done_o, hz_o, calls_o = run_query(rig, uart, inject=True)
        stopped = [c for c in calls_o if c[0] == "stopped"]
        ok &= check(f"2 old payload {pa.sha256(odata)[:8]} (--old): it reaches a kick with the THRE interrupt on "
                    "(multi-byte FUN_4014512C call) and, with the task switch there, its engine-loop pass never returns - "
                    "the 2026-11-01 field stall",
                    bool(hz_o) and not done_o and bool(stopped), f"hazards {len(hz_o)} done {done_o} stopped {stopped[:1]}")
        rig.close()

    # ---- 3. heartbeat ------------------------------------------------------------------------------
    rig, uart = fresh_rig(image, syms, data)
    rig.tick(10)
    direct = rig.pn.sysex(2, rp.req(OP, 0x0F))       # ng_sysex only: no engine-loop pass
    body = rp.body_of(direct, 2)
    hb = rp.unpack7(body[1][2:]) if body and body[0] == (OP | 0x40) and body[1][:2] == b"\x0F\x00" else None
    ok &= check("3 1D 0F answered inside ng_sysex (task 19) without an engine-loop pass: layout 1, pass count > 0, TX "
                "state idle", hb is not None and hb[0] == 1 and le(hb, 2, 2) > 0 and le(hb, 6, 2) == 0,
                str(hb and list(hb)))

    # ---- 4. the legacy mode NEWEST is not in the payload: refused; two streams close nothing --
    # up to f2056ae8 this check opted in to NEWEST (1D 06 mode 1) and saw the older source's links closed - the CLOSE the
    # switch / guard must never send.  Now the write is refused (rc 1, the policy stays mode 2) and the
    # default SELECT guard pauses the second source (AVRCP) - no CLOSE, no sink STOP.
    emu = rig.emu
    asm = rpw.BT_ASSEMBLER

    def module(*lines):
        for ln in lines:
            rig.bt_line(ln)
            emu.call(asm, max_insns=2_000_000)

    def ticks(n):
        for _ in range(n):
            rig.tick()
            uart.finish()

    def pin_kept():
        """The manager reads the module listing once BTS_SET_READ_MS after module on and keeps
        the decided values.  The op scenarios 17 / 19 / 21 answer that read with the decided lines (nothing written,
        the pin done for this power-on) and then run on their own factory listings as before - the pin itself is 25.
        True = that read was the only command and nothing followed it."""
        for _ in range(500):
            if bytes(uart.thr).endswith(b"SET\r\n"):
                break
            ticks(1)
        got = bytes(uart.thr)
        module("SET", "SET BT BDADDR 02:07:80:aa:bb:cc", "SET BT NAME CA79 Audio", "SET BT CLASS 240414",
               "SET PROFILE A2DP SINK", "SET PROFILE AVRCP CONTROLLER 0f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT",
               "SET")
        ticks(60)
        same = bytes(uart.thr) == got
        uart.thr.clear()
        return got == b"SET\r\n" and same

    ticks(60)                                        # the first LIST (learning the links) goes out
    module("LIST", "LIST 0")
    pol_rc = rbt.op_rc(rig, 0x06, 1, 0x0D, 15, 5, 0, 0, 0, 0, 0, 0, 0, 0)   # the legacy NEWEST: refused since §20
    pol = rig.raw_reply(2, rp.req(OP, 0x06), OP)
    uart.thr.clear()
    module("RING 0 62:81:81:81:11:a1 19 A2DP", "RING 1 62:81:81:81:11:a1 19 A2DP",
           "RING 2 62:81:81:81:11:a1 17 AVRCP", "A2DP STREAMING START 1")
    ticks(400)                                       # 3.2 s: one source (adopted) - its play-status registration only
    quiet = bytes(uart.thr)
    module("RING 3 22:82:02:02:92:22 19 A2DP", "RING 4 22:82:02:02:92:22 19 A2DP", "RING 5 22:82:02:02:92:22 17 AVRCP",
           "A2DP STREAMING START 4")
    ticks(300)                                       # 2.4 s: the guard (0.3 s) - PAUSE to the PC, never a CLOSE
    sent = bytes(uart.thr)[len(quiet):]
    ok &= check("4 the legacy mode NEWEST is not in the payload: 1D 06 mode 1 -> rc 1, the policy stays "
                "mode 2; phone streaming (adopted) -> its play-status registration only; the PC streams too -> the guard "
                "`SET 5 SELECT` + `AVRCP PAUSE`; no CLOSE, no sink STOP (f2056ae8 with NEWEST: CLOSE 0 / 1 / 2)",
                pol_rc == 1 and pol is not None and pol[2] == 2 and quiet.replace(b"SET\r\n", b"") == b"SET 2 SELECT\r\nAVRCP PDU 31 1\r\n"
                and b"SET 5 SELECT\r\nAVRCP PAUSE\r\n" in sent and b"CLOSE" not in quiet + sent
                and b"STREAMING STOP" not in quiet + sent,
                f"policy rc {pol_rc} {pol and pol[:5].hex()} quiet {quiet!r} sent {sent!r}")
    rig.close()

    # ---- 5. default SELECT: the 2026-11-02 field case - 「이 기기로 듣기」 on the phone while Windows keeps its stream ----
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    calls = []
    emu.hook(BT_TX, lambda e: (calls.append((e.reg(1), uart.state())), rt.se.StockEmu.CONTINUE)[1])
    ticks(60)
    module("LIST", "LIST 0")
    pol = rig.raw_reply(2, rp.req(OP, 0x06), OP)
    uart.thr.clear()
    module("RING 3 22:82:02:02:92:22 19 A2DP", "RING 4 22:82:02:02:92:22 19 A2DP", "RING 5 22:82:02:02:92:22 17 AVRCP",
           "A2DP STREAMING START 3",
           "RING 0 62:81:81:81:11:a1 19 A2DP", "RING 1 62:81:81:81:11:a1 19 A2DP", "RING 2 62:81:81:81:11:a1 17 AVRCP")
    ticks(120)                                       # the play-status registrations go out (RM 6.10: PDU 31, event 1)
    module("AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING",
           "AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(280)
    auto = bytes(uart.thr)
    phone7 = list(rtlib.pack7(bytes.fromhex("6281818111a1")))
    use_rc = rbt.op_rc(rig, 0x05, 0x01, *phone7)
    ticks(60)
    paused = bytes(uart.thr)
    ticks(150)                                       # Windows: no STREAMING STOP after the pause
    suspended = bytes(uart.thr)                      # the sink never stops it (2026-11-10); no report 1 s
    module("AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")   # after the PAUSE: asked again
    module("A2DP STREAMING STOP 3")                  # Windows lets its stream go by itself (field: 9-10 s)
    ticks(60)
    sent = bytes(uart.thr)
    module("A2DP CODEC SBC JOINT_STEREO 44100 BITPOOL 2-53", "A2DP STREAMING START 0")   # the phone plays
    ticks(10)
    emu.unhook(BT_TX)
    src = src_list(rig)
    devs = src_devs(src)
    clean = bool(calls) and all(n == 1 and st[0] == 0 and st[1] == st[2] and not st[3] & 2 for n, st in calls)
    tail = src[84:116] if src else b""
    regs = b"SET 5 SELECT\r\nAVRCP PDU 31 1\r\nSET 2 SELECT\r\nAVRCP PDU 31 1\r\n"
    slot = {a: i for i in range(4) for a in [bytes(src[29 + 14 * i: 35 + 14 * i]).hex()]} if src else {}
    ok &= check("5 default SELECT, the one switch: PC streaming (adopted) + phone connected -> the play-status "
                "registrations (`SET <l> SELECT` + `AVRCP PDU 31 1`, PC answers PLAYING, phone PAUSED) and the AUTOCALL "
                "read `SET`; 1D 06 answers 19 B (options 1, wait 1 = BTS_LATE_STOP_DEFAULT / BTS_XWAIT_DEFAULT since D76), strategy 0, reconnect 2, NOR bits 1; 1D 05 01 phone -> `SET 5 SELECT` + `AVRCP PAUSE` (it "
                "plays); no STOP and no report -> no sink STOP ever (2026-11-10), `SET 5 SELECT` + `AVRCP PDU 31 1` once; its own STOP -> "
                "`SET 2 SELECT` + `AVRCP PLAY` (it reported paused); every byte one idle FUN_4014512C call; 1D 04 layout 2 "
                "(120 B, byte 5 bits 4-6 = 1, byte 116 = reconnect 2 + 1 | NOR-kept 0x04 | options known 0x40 | late stop on 0x80): the PC listed with all its links 3 / 4 / 5, the phone selected, PLAYING, "
                "tries PLAY, codec SBC 44.1 kHz on the phone's slot, play status PC PAUSED | LIVE | OURS, phone PAUSED | "
                "LIVE",
                pol is not None and pol[:4] == b"\x06\x00\x02\x04" and len(pol) == 19 and pol[17] == 1 and pol[18] == 1 and pol[14] == 0 and pol[15] == 2 and pol[16] == 1
                and auto == regs + b"SET\r\n" and use_rc == 0
                and paused == auto + b"SET 5 SELECT\r\nAVRCP PAUSE\r\n"
                and suspended == paused + b"SET 5 SELECT\r\nAVRCP PDU 31 1\r\n"
                and sent == suspended + b"SET 2 SELECT\r\nAVRCP PLAY\r\n" and clean and b"CLOSE" not in sent
                and b"STREAMING STOP" not in sent
                and src[5] >> 4 == 1
                and devs.get("228202029222", (0, 0, 0))[1] == 0x38 and devs.get("6281818111a1", (0, 0, 0))[1] == 0x07
                and bool(devs["6281818111a1"][2] & 0x01) and not devs["228202029222"][2] & 0x01
                and src[0] == 2 and len(src) == 120 and src[116] == 0xC7 and tail[1] == 0 and tail[2] == 1 and 5 <= tail[3] <= 60
                and tail[18] == 0 and tail[22] == 0x01 and tail[23] == 1 and tail[25] == 0
                and tail[28 + slot.get("228202029222", 0)] == 0x2B and tail[28 + slot.get("6281818111a1", 0)] == 0x0B
                and tail[24] == next(i for i in range(4) if src[29 + 14 * i: 35 + 14 * i] == bytes.fromhex("6281818111a1")),
                f"policy {pol and pol[:5].hex()} auto {auto!r} rc {use_rc} sent {sent!r} calls {len(calls)} clean {clean} "
                f"devs {devs} tail {tail.hex()}")

    # ---- 6. 1D 08 module line log -------------------------------------------------------------------------------
    def log_read(frm):
        r = rig.raw_reply(2, rp.req(OP, 0x08, (frm >> 7) & 0x7F, frm & 0x7F), OP)
        if not r or r[0] != 0x08 or r[1] != 0 or len(r) < 10:
            return None
        recs, o = [], 10
        while o + 8 <= len(r):
            n = r[o + 7]
            recs.append(((r[o] << 7) | r[o + 1], r[o + 6], bytes(r[o + 8:o + 8 + n]).decode()))
            o += 8 + n
        return (r[2] << 7) | r[3], (r[4] << 7) | r[5], recs, len(r)

    lines, frm, sizes = [], 0, []
    for _ in range(40):
        got = log_read(frm)
        if not got or not got[2]:
            break
        lines += got[2]
        sizes.append(got[3])
        frm = got[2][-1][0] + 1
    texts = [(f, t) for _, f, t in lines]
    want = [(4, "module on"), (1, "LIST"), (0, "A2DP STREAMING START 3"), (1, "SET"), (1, "SET 5 SELECT"),
            (1, "AVRCP PAUSE"), (4, "PAUSE unreported: ask"),
            (1, "AVRCP PDU 31 1"), (0, "A2DP STREAMING STOP 3"),
            (1, "SET 2 SELECT"), (1, "AVRCP PLAY"), (0, "A2DP CODEC SBC JOINT_STEREO 44100 BITPOOL 2-53"),
            (0, "A2DP STREAMING START 0")]
    pos, it = [], iter(enumerate(texts))
    for w in want:
        pos.append(next((i for i, x in it if x == w), None))
    seqs = [s_ for s_, _, _ in lines]
    ok &= check("6 1D 08: the module lines (RING / STREAMING ...) and the commands sent (flag 1) in order, sequence "
                "numbers consecutive, every reply <= 148 B",
                None not in pos and seqs == list(range(seqs[0], seqs[0] + len(seqs))) and max(sizes) <= 140,
                f"{len(lines)} records, sizes {sizes[:6]}, missing {[w for w, p_ in zip(want, pos) if p_ is None]}")
    rig.close()

    # ---- 7. a PC without AVRCP: the sink opens AVRCP once; never the sink's START to it ---------------
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    ticks(60)
    module("LIST", "LIST 0")
    module("RING 0 62:81:81:81:11:a1 19 A2DP", "RING 1 62:81:81:81:11:a1 19 A2DP", "RING 2 62:81:81:81:11:a1 17 AVRCP",
           "A2DP STREAMING START 0", "RING 3 22:82:02:02:92:22 19 A2DP", "RING 4 22:82:02:02:92:22 19 A2DP")
    uart.thr.clear()
    ticks(480)                                       # 3.84 s: not yet (only the phone's play-status registration,
    early = bytes(uart.thr)                          # never answered here: the "paused by us" rule decides - and the
    ticks(80)                                        # AUTOCALL read `SET`); 4.5 s: the sink's AVRCP CALL (sent)
    called = bytes(uart.thr)
    reg2 = b"SET 2 SELECT\r\nAVRCP PDU 31 1\r\n"
    av = b"CALL 22:82:02:02:92:22 17 AVRCP\r\n"
    module("CALL 6")                                 # the PC leaves it unanswered (field 2026-11-09)
    ticks(175)                                       # 1.4 s: still pending
    pend = bytes(uart.thr)
    ticks(25)                                        # 1.6 s: given up - `CLOSE 6`
    closed = bytes(uart.thr)
    module("NO CARRIER 6 ERROR 0")
    ticks(500)                                       # 4 s: never again
    once = bytes(uart.thr)
    uart.thr.clear()
    pc7 = list(rtlib.pack7(bytes.fromhex("228202029222")))
    use_rc = rbt.op_rc(rig, 0x05, 0x01, *pc7)
    ticks(40)
    paused = bytes(uart.thr)
    module("A2DP STREAMING STOP 0")                  # the phone stops on the pause
    ticks(120)
    started = bytes(uart.thr)                        # f2056ae8: `SET 3 SELECT` + `A2DP STREAMING START` (the sink)
    src = src_list(rig)
    tail = src[84:116] if src else b""
    ready = tail[:3]                                 # READY at once: the app says "press play on the PC"
    module("A2DP STREAMING START 3")                 # the user plays on the PC: its own stream
    ticks(135)
    after = bytes(uart.thr)
    src = src_list(rig)
    tail = src[84:116] if src else b""
    ns = lambda x: x.replace(b"SET\r\n", b"")      # the AUTOCALL read, whenever it went
    ok &= check("7 PC without AVRCP (field 2026-11-02 #2, #4; 2026-11-09): nothing within 3.8 s but the phone's play-status "
                "registration (no answer), then at 4 s `CALL 22:82:02:02:92:22 17 AVRCP` once; `CALL 6` and no CONNECT -> "
                "nothing at 1.4 s, `CLOSE 6` by 1.6 s (given up; a LIST), not called again; 1D 05 01 PC "
                "while the phone plays -> `SET 2 SELECT` + `AVRCP PAUSE` (status unknown, it streams); the phone's STOP "
                "-> nothing: no sink START to a source without AVRCP (f2056ae8: `SET 3 SELECT` + `A2DP STREAMING "
                "START`) - 1D 04 result 2 (READY), tries 0; the PC's own START 3 -> nothing sent, the PC streaming; "
                "no sink STOP, no CLOSE; the phone's play status NONE | OURS",
                ns(early) == reg2 and ns(called) == reg2 + av and ns(pend) == ns(called)
                and ns(closed) == ns(called) + b"CLOSE 6\r\n" and ns(once) in (ns(closed), ns(closed) + b"LIST\r\n") and use_rc == 0
                and paused == b"SET 2 SELECT\r\nAVRCP PAUSE\r\n"
                and started == paused and after == paused and ready[1:3] == b"\x00\x02"
                and len(src) == 120 and tail[1] == 0 and tail[2] == 2 and tail[22] == 0 and 0x30 in tail[28:32]
                and any(src[28 + 14 * i] & 0x81 == 0x81 and src[29 + 14 * i: 35 + 14 * i] == bytes.fromhex("228202029222")
                        for i in range(4)),
                f"early {early!r} called {called!r} pend {pend!r} closed {closed!r} once {once!r} rc {use_rc} "
                f"started {started!r} after {after!r} ready {ready.hex()} tail {tail.hex()}")
    rig.close()

    # ---- 8. GUARD: the selected phone is paused, the PC starts by itself -> refused ----------------------------------
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    ticks(60)
    module("LIST", "LIST 0")
    module("RING 0 62:81:81:81:11:a1 19 A2DP", "RING 1 62:81:81:81:11:a1 19 A2DP", "RING 2 62:81:81:81:11:a1 17 AVRCP",
           "A2DP STREAMING START 0",
           "RING 3 22:82:02:02:92:22 19 A2DP", "RING 4 22:82:02:02:92:22 19 A2DP", "RING 5 22:82:02:02:92:22 17 AVRCP")
    ticks(400)
    module("A2DP STREAMING STOP 0", "A2DP STREAMING START 3")
    uart.thr.clear()
    ticks(80)
    paused = bytes(uart.thr)
    ticks(120)
    sent = bytes(uart.thr)
    src = src_list(rig)
    devs = src_devs(src)
    ok &= check("8 GUARD (mode SELECT, SUSPEND): the phone (selected) paused, the PC starts streaming -> 0.3 s later "
                "`SET 5 SELECT` + `AVRCP PAUSE` for the PC; it keeps streaming -> never a sink STOP (2026-11-10, record "
                "§19; its registration is still unanswered here: not asked again); 1D 04: the phone still selected, the "
                "PC marked refused",
                paused == b"SET 5 SELECT\r\nAVRCP PAUSE\r\n"
                and sent == paused
                and bool(devs.get("6281818111a1", (0, 0, 0))[2] & 0x01)
                and bool(devs.get("228202029222", (0, 0, 0))[2] & 0x02) and le(src, 91, 2) == 1,
                f"sent {sent!r} devs {devs}")
    rig.close()

    # ---- 9. AUTOCALL: the module setting is written only by op 1D 09, the exact lines ---------------------------------
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    calls = []
    emu.hook(BT_TX, lambda e: (calls.append((e.reg(1), uart.state())), rt.se.StockEmu.CONTINUE)[1])
    ticks(60)
    module("LIST", "LIST 0")
    uart.thr.clear()

    def ac(o):
        return rig.raw_reply(2, rp.req(OP, 0x09, o), OP)

    def listing(autocall):
        module("SET", "SET BT BDADDR 02:07:80:aa:bb:cc", "SET BT NAME CA79 Audio",
               "SET BT PAIR 62:81:81:81:11:a1 00112233445566778899aabbccddeeff",
               *(["SET CONTROL AUTOCALL 19 5000 A2DP"] if autocall else []), "SET PROFILE A2DP SINK", "SET")

    s0 = ac(0)
    early_off = ac(2)
    rd = ac(1)
    ticks(40)
    read_thr = bytes(uart.thr)
    listing(True)
    ticks(2)
    s1 = ac(0)
    emu.write_bytes(BINDING, b"\x04")                # another port holds the binding
    nb = ac(2)
    emu.write_bytes(BINDING, b"\x03")
    off = ac(2)
    ticks(60)
    off_thr = bytes(uart.thr)
    listing(False)
    ticks(2)
    s2 = ac(0)
    rs = ac(3)
    ticks(80)
    rs_thr = bytes(uart.thr)
    listing(True)
    ticks(2)
    s3 = ac(0)
    emu.unhook(BT_TX)
    clean = bool(calls) and all(n == 1 and st[0] == 0 and st[1] == st[2] and not st[3] & 2 for n, st in calls)
    factory = b"SET CONTROL AUTOCALL 19 5000 A2DP"
    ok &= check("9 1D 09 AUTOCALL: state 0 (unknown) with the restore line = the stock factory line; off before a read "
                "-> rc 10; read -> `SET`; the listing -> ON; off from a port without the binding -> rc 2; off -> "
                "`SET CONTROL AUTOCALL` + `SET`, the listing without it -> OFF rc 0; restore -> `SET CONTROL AUTOCALL "
                "19 5000 A2DP` + `SET` -> ON; every byte one idle FUN_4014512C call",
                s0 is not None and s0[1] == 0 and s0[3] == 0 and bytes(s0[8:8 + s0[7]]) == factory
                and early_off is not None and early_off[1] == 10 and rd is not None and rd[1] == 0
                and read_thr == b"SET\r\n" and s1 is not None and s1[3] == 1 and s1[6] == 1
                and nb is not None and nb[1] == 2 and off is not None and off[1] == 0
                and off_thr == read_thr + b"SET CONTROL AUTOCALL\r\nSET\r\n"
                and s2 is not None and s2[3] == 2 and s2[5] == 0 and rs is not None and rs[1] == 0
                and rs_thr == off_thr + factory + b"\r\nSET\r\n" and s3 is not None and s3[3] == 1 and s3[5] == 0
                and clean,
                f"s0 {s0 and s0[:8].hex()} early {early_off and early_off[:2].hex()} read {read_thr!r} s1 "
                f"{s1 and s1[:8].hex()} nb {nb and nb[:2].hex()} off {off_thr!r} s2 {s2 and s2[:8].hex()} restore "
                f"{rs_thr!r} s3 {s3 and s3[:8].hex()} clean {clean}")
    rig.close()

    # ---- 17. class of device: written only by op 1D 0A, the exact lines -------------------------------------------------
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    calls = []
    emu.hook(BT_TX, lambda e: (calls.append((e.reg(1), uart.state())), rt.se.StockEmu.CONTINUE)[1])
    ticks(60)
    module("LIST", "LIST 0")
    uart.thr.clear()

    def co(o, cls=None):
        args = (o,) if cls is None else (o, cls >> 21 & 0x7F, cls >> 14 & 0x7F, cls >> 7 & 0x7F, cls & 0x7F)
        return rig.raw_reply(2, rp.req(OP, 0x0A, *args), OP)

    def cval(r, i):
        return r[i] << 21 | r[i + 1] << 14 | r[i + 2] << 7 | r[i + 3]

    def clisting(cls):
        module("SET", "SET BT BDADDR 02:07:80:aa:bb:cc", "SET BT NAME CA79 Audio", f"SET BT CLASS {cls}",
               "SET BT PAGEMODE 3 2000 1", "SET BT PAIR 62:81:81:81:11:a1 00112233445566778899aabbccddeeff",
               "SET CONTROL AUTOCALL 19 5000 A2DP", "SET PROFILE A2DP SINK", "SET")

    c0 = co(0)
    early = co(2)
    badc = co(2, 0x001F00)
    pk17 = pin_kept()                                # the power-on read, decided values (§27) - then the factory listing
    rd = co(1)
    busy_ac = rig.raw_reply(2, rp.req(OP, 0x09, 1), OP)
    ticks(40)
    read_thr = bytes(uart.thr)
    clisting("001f00")
    ticks(2)
    c1 = co(0)
    emu.write_bytes(BINDING, b"\x04")                # another port holds the binding
    nb = co(2)
    emu.write_bytes(BINDING, b"\x03")
    wr = co(2)
    ticks(60)
    wr_thr = bytes(uart.thr)
    clisting("240414")
    ticks(2)
    c2 = co(0)
    rs = co(3)
    ticks(60)
    rs_thr = bytes(uart.thr)
    clisting("001f00")
    ticks(2)
    c3 = co(0)
    emu.unhook(BT_TX)
    clean = bool(calls) and all(n == 1 and st[0] == 0 and st[1] == st[2] and not st[3] & 2 for n, st in calls)
    ok &= check("17 1D 0A class of device: state 0 (unknown), restore 001f00, write 240414; write before a read -> rc 10; "
                "a class without the Rendering bit / A-V major -> rc 1; read -> `SET` (1D 09 meanwhile -> rc 3); the "
                "listing -> known 001f00, saved; write from a port without the binding -> rc 2; write -> `SET BT CLASS "
                "240414` + `SET`, the listing -> 240414 rc 0; restore -> `SET BT CLASS 001f00` + `SET` -> 001f00 rc 0; "
                "every byte one idle FUN_4014512C call",
                c0 is not None and len(c0) == 19 and c0[1] == 0 and c0[3] == 0 and cval(c0, 11) == 0x001F00
                and cval(c0, 15) == 0x240414 and early is not None and early[1] == 10 and badc is not None
                and badc[1] == 1 and rd is not None and rd[1] == 0 and busy_ac is not None and busy_ac[1] == 3
                and read_thr == b"SET\r\n" and c1 is not None and c1[3] == 1 and c1[6] == 1
                and cval(c1, 7) == 0x001F00 and nb is not None and nb[1] == 2 and wr is not None and wr[1] == 0
                and wr_thr == read_thr + b"SET BT CLASS 240414\r\nSET\r\n" and c2 is not None and c2[5] == 0
                and cval(c2, 7) == 0x240414 and rs is not None and rs[1] == 0
                and rs_thr == wr_thr + b"SET BT CLASS 001f00\r\nSET\r\n" and c3 is not None and c3[5] == 0
                and cval(c3, 7) == 0x001F00 and clean and pk17,
                f"c0 {c0 and c0.hex()} early {early and early[:2].hex()} bad {badc and badc[:2].hex()} read {read_thr!r} "
                f"ac {busy_ac and busy_ac[:2].hex()} c1 {c1 and c1.hex()} nb {nb and nb[:2].hex()} write {wr_thr!r} "
                f"c2 {c2 and c2.hex()} restore {rs_thr!r} c3 {c3 and c3.hex()} clean {clean} pin {pk17}")
    rig.close()

    # ---- 19. AVRCP controller features: written only by op 1D 0C, the exact lines --------------------------------------
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    calls = []
    emu.hook(BT_TX, lambda e: (calls.append((e.reg(1), uart.state())), rt.se.StockEmu.CONTINUE)[1])
    ticks(60)
    module("LIST", "LIST 0")
    uart.thr.clear()

    def av(o, f=None):
        args = (o,) if f is None else (o, f)
        return rig.raw_reply(2, rp.req(OP, 0x0C, *args), OP)

    def aval(r, i):
        return r[i] << 7 | r[i + 1]

    def alisting(line):
        module("SET", "SET BT BDADDR 02:07:80:aa:bb:cc", "SET BT NAME CA79 Audio", "SET BT CLASS 240414",
               "SET BT PAGEMODE 3 2000 1", "SET BT PAIR 62:81:81:81:11:a1 00112233445566778899aabbccddeeff",
               "SET PROFILE A2DP SINK", line, "SET")

    a0 = av(0)
    aearly = av(2)
    abad = av(2, 0x1F)
    pk19 = pin_kept()                                # the power-on read, decided values (§27)
    ard = av(1)
    busy_co = rig.raw_reply(2, rp.req(OP, 0x0A, 1), OP)
    ticks(40)
    aread_thr = bytes(uart.thr)
    alisting("SET PROFILE AVRCP CONTROLLER 1f")
    ticks(2)
    a1 = av(0)
    emu.write_bytes(BINDING, b"\x04")                # another port holds the binding
    anb = av(2)
    emu.write_bytes(BINDING, b"\x03")
    awr = av(2)
    ticks(60)
    awr_thr = bytes(uart.thr)
    alisting("SET PROFILE AVRCP CONTROLLER 0f")
    ticks(2)
    a2 = av(0)
    ars = av(3)
    ticks(60)
    ars_thr = bytes(uart.thr)
    alisting("SET PROFILE AVRCP CONTROLLER 1f")
    ticks(2)
    a3 = av(0)
    av(1)
    ticks(40)
    alisting("SET PROFILE AVRCP CONTROLLER 1f TARGET 3")
    ticks(2)
    atg = av(2)
    atg_thr = bytes(uart.thr)
    emu.unhook(BT_TX)
    clean = bool(calls) and all(n == 1 and st[0] == 0 and st[1] == st[2] and not st[3] & 2 for n, st in calls)
    ok &= check("19 1D 0C AVRCP features: state 0 (unknown), restore 1f, write 0f; write before a read -> rc 10; the "
                "browsing bit -> rc 1; read -> `SET` (1D 0A meanwhile -> rc 3); the listing -> controller 1f, saved; write "
                "from a port without the binding -> rc 2; write -> `SET PROFILE AVRCP CONTROLLER 0f` + `SET`, the listing "
                "-> 0f rc 0; restore -> `SET PROFILE AVRCP CONTROLLER 1f` + `SET` -> 1f rc 0; a TARGET part -> rc 10, "
                "nothing sent; every byte one idle FUN_4014512C call",
                a0 is not None and len(a0) == 13 and a0[1] == 0 and a0[3] == 0 and aval(a0, 9) == 0x1F
                and aval(a0, 11) == 0x0F and aearly is not None and aearly[1] == 10 and abad is not None
                and abad[1] == 1 and ard is not None and ard[1] == 0 and busy_co is not None and busy_co[1] == 3
                and aread_thr == b"SET\r\n" and a1 is not None and a1[3] == 1 and a1[6] == 1
                and aval(a1, 7) == 0x1F and anb is not None and anb[1] == 2 and awr is not None and awr[1] == 0
                and awr_thr == aread_thr + b"SET PROFILE AVRCP CONTROLLER 0f\r\nSET\r\n" and a2 is not None
                and a2[5] == 0 and aval(a2, 7) == 0x0F and ars is not None and ars[1] == 0
                and ars_thr == awr_thr + b"SET PROFILE AVRCP CONTROLLER 1f\r\nSET\r\n" and a3 is not None
                and a3[5] == 0 and aval(a3, 7) == 0x1F and atg is not None and atg[1] == 10 and atg[3] == 2
                and atg_thr == ars_thr + b"SET\r\n" and clean and pk19,
                f"a0 {a0 and a0.hex()} early {aearly and aearly[:2].hex()} bad {abad and abad[:2].hex()} read "
                f"{aread_thr!r} co {busy_co and busy_co[:2].hex()} a1 {a1 and a1.hex()} nb {anb and anb[:2].hex()} "
                f"write {awr_thr!r} a2 {a2 and a2.hex()} restore {ars_thr!r} a3 {a3 and a3.hex()} target "
                f"{atg and atg.hex()} {atg_thr!r} clean {clean} pin {pk19}")
    rig.close()

    # ---- 21. audio routing: written only by op 1D 0D, the exact lines; the EVENT lines are no stream events ------------
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    calls = []
    emu.hook(BT_TX, lambda e: (calls.append((e.reg(1), uart.state())), rt.se.StockEmu.CONTINUE)[1])
    ticks(60)
    module("LIST", "LIST 0")
    uart.thr.clear()

    def au(o, f=None):
        args = (o,) if f is None else (o, f)
        return rig.raw_reply(2, rp.req(OP, 0x0D, *args), OP)

    def ulisting(line):
        # one engine pass (8 ms) per line - the UART pace (11.5 B/ms): since the power-on listing read moves
        # the RX ring position, and this ~330 B listing fed at once across the ring end reads as a stock ring reset
        # (bt_cave.c rdr_left: the write index wrapped by more than 32 + 16 B/ms) - no module can send that fast
        for ln in ("SET", "SET BT BDADDR 02:07:80:aa:bb:cc", "SET BT NAME CA79 Audio", "SET BT CLASS 240414",
                   "SET BT PAGEMODE 3 2000 1", "SET BT PAIR 62:81:81:81:11:a1 00112233445566778899aabbccddeeff",
                   line, "SET CONTROL CODEC APT-X JOINT_STEREO 44100 0", "SET PROFILE A2DP SINK",
                   "SET PROFILE AVRCP CONTROLLER 1f", "SET"):
            module(ln)
            ticks(1)

    u0 = au(0)
    uearly = au(2)
    ubad = au(2, 2)                                  # KEEPALIVE alone: not a write variant
    pk21 = pin_kept()                                # the power-on read, decided values (§27)
    urd = au(1)
    busy_av = rig.raw_reply(2, rp.req(OP, 0x0C, 1), OP)
    ticks(40)
    uread_thr = bytes(uart.thr)
    ulisting("SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE")
    ticks(2)
    u1 = au(0)
    emu.write_bytes(BINDING, b"\x04")                # another port holds the binding
    unb = au(2)
    emu.write_bytes(BINDING, b"\x03")
    uwr = au(2)                                      # variant (1): KEEPALIVE off, EVENT on
    ticks(90)
    uwr_thr = bytes(uart.thr)
    ulisting("SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT")
    ticks(2)
    u2 = au(0)
    uwr2 = au(2, 3)                                  # variant (2): EVENT added
    ticks(90)
    uwr2_thr = bytes(uart.thr)
    ulisting("SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT KEEPALIVE")
    ticks(2)
    u3 = au(0)
    # the EVENT lines around a phone stream: the codec / route before its START, a route naming another link after
    uart.thr.clear()
    module("RING 0 62:81:81:81:11:a1 19 A2DP", "RING 1 62:81:81:81:11:a1 19 A2DP", "RING 2 62:81:81:81:11:a1 17 AVRCP",
           "RING 4 a2:83:83:83:13:23 19 A2DP", "RING 5 a2:83:83:83:13:23 19 A2DP")
    ticks(40)
    module("A2DP CODEC APT-X JOINT_STEREO 44100", "AUDIO ROUTE 1 A2DP LEFT RIGHT", "A2DP STREAMING START 1")
    ticks(40)
    ev_raw = src_list(rig)
    ev_devs = src_devs(ev_raw)
    ue = au(0)
    module("AUDIO ROUTE 4 A2DP LEFT RIGHT", "AUDIO ROUTE", "AUDIO ROUTE 1 SCO LEFT")
    ticks(40)
    ue2 = au(0)
    ev_devs2 = src_devs(src_list(rig))
    ev_thr = bytes(uart.thr)
    uart.thr.clear()
    urs = au(3)
    ticks(90)
    urs_thr = bytes(uart.thr)
    ulisting("SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE")
    ticks(2)
    u4 = au(0)
    au(1)
    ticks(40)
    ulisting("SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT 32")
    ticks(2)
    uot = au(2)
    uot_thr = bytes(uart.thr)
    emu.unhook(BT_TX)
    clean = bool(calls) and all(n == 1 and st[0] == 0 and st[1] == st[2] and not st[3] & 2 for n, st in calls)
    phone = ev_devs.get("6281818111a1")
    phone2 = ev_devs2.get("6281818111a1")
    pc2 = ev_devs2.get("a28383831323")
    ok &= check("21 1D 0D audio routing: state 0, restore KEEPALIVE, write EVENT; write before a read -> rc 10; KEEPALIVE "
                "alone -> rc 1; read -> `SET` (1D 0C meanwhile -> rc 3); the listing -> plain, KEEPALIVE, saved; write "
                "from a port without the binding -> rc 2; write -> `SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT` + `SET` "
                "-> EVENT rc 0; write 3 -> `… EVENT KEEPALIVE` + `SET`; `A2DP CODEC` / `AUDIO ROUTE 1` before the phone's "
                "START 1 -> the phone streams, route 1 A2DP on the phone; route lines naming the PC / none / SCO -> the "
                "phone still the only stream, nothing sent; restore -> `… KEEPALIVE` + `SET`; another form -> rc 10, "
                "nothing sent; every byte one idle FUN_4014512C call",
                u0 is not None and len(u0) == 12 and u0[1] == 0 and u0[3] == 0 and u0[8] == 2 and u0[9] == 1
                and u0[10] == 0 and u0[11] == 0x7F
                and uearly is not None and uearly[1] == 10 and ubad is not None and ubad[1] == 1
                and urd is not None and urd[1] == 0 and busy_av is not None and busy_av[1] == 3
                and uread_thr == b"SET\r\n" and u1 is not None and u1[3] == 1 and u1[6] == 1 and u1[7] == 2
                and unb is not None and unb[1] == 2 and uwr is not None and uwr[1] == 0
                and uwr_thr == uread_thr + b"SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT\r\nSET\r\n"
                and u2 is not None and u2[5] == 0 and u2[7] == 1 and u2[8] == 2
                and uwr2 is not None and uwr2[1] == 0
                and uwr2_thr == uwr_thr + b"SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT KEEPALIVE\r\nSET\r\n"
                and u3 is not None and u3[5] == 0 and u3[7] == 3
                and phone is not None and phone[0] & 0x01 and ue is not None and ue[10] == 0x40 | 2
                and ue[11] != 0x7F
                and ue2 is not None and ue2[10] == 2 and phone2 is not None and phone2[0] & 0x01
                and pc2 is not None and not pc2[0] & 0x01 and b"STREAMING" not in ev_thr and b"CLOSE" not in ev_thr
                and urs is not None and urs[1] == 0
                and urs_thr == b"SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE\r\nSET\r\n"
                and u4 is not None and u4[5] == 0 and u4[7] == 2
                and uot is not None and uot[1] == 10 and uot[3] == 2 and uot_thr == urs_thr + b"SET\r\n" and clean and pk21,
                f"u0 {u0 and u0.hex()} early {uearly and uearly[:2].hex()} bad {ubad and ubad[:2].hex()} read "
                f"{uread_thr!r} av {busy_av and busy_av[:2].hex()} u1 {u1 and u1.hex()} nb {unb and unb[:2].hex()} "
                f"write {uwr_thr!r} u2 {u2 and u2.hex()} write2 {uwr2_thr!r} u3 {u3 and u3.hex()} devs {ev_devs} "
                f"ue {ue and ue.hex()} ue2 {ue2 and ue2.hex()} devs2 {ev_devs2} ev {ev_thr!r} restore {urs_thr!r} "
                f"u4 {u4 and u4.hex()} other {uot and uot.hex()} {uot_thr!r} clean {clean} pin {pk21}")
    rig.close()

    # ---- 10. module reboots: counted, suspect, loop guard ----------------------------------------------------------------
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    ticks(60)
    module("LIST", "LIST 0")
    ticks(700)                                       # past the boot grace (5 s)
    uart.thr.clear()
    module("RING 0 22:82:02:02:92:22 19 A2DP", "RING 1 22:82:02:02:92:22 19 A2DP", "RING 2 22:82:02:02:92:22 1b AVRCP",
           "AVRCP 5103 ERROR_NO_CONTROL_CHANNEL")
    ticks(60)
    module("WRAP THOR AI (6.2.0 build 1135)", "READY.")
    ticks(10)
    s1 = src_list(rig)
    ticks(1250)                                      # 10 s: AUTOCALL links the PC again (OUTGOING, no address)
    module("CONNECT 0 A2DP 19", "CONNECT 1 A2DP 19")
    ticks(60)
    module("LIST", "LIST 2",
           "LIST 0 CONNECTED A2DP 1024 0 0 24 0 0 22:82:02:02:92:22 19 OUTGOING ACTIVE MASTER ENCRYPTED 0",
           "LIST 1 CONNECTED A2DP 1024 0 0 24 0 0 22:82:02:02:92:22 19 OUTGOING ACTIVE MASTER ENCRYPTED 0",
           "RING 2 22:82:02:02:92:22 1b AVRCP")
    module("WRAP THOR AI (6.2.0 build 1135)", "READY.")
    ticks(10)
    s2 = src_list(rig)
    module("RING 0 22:82:02:02:92:22 19 A2DP", "RING 1 22:82:02:02:92:22 19 A2DP")
    ticks(500)                                       # 4 s: no CALL in the loop
    thr = bytes(uart.thr)
    t1 = s1[84:112] if s1 else b""
    t2 = s2[84:112] if s2 else b""
    ok &= check("10 module reboots (field 2026-11-02 #1): a lone browsing channel + the banner / READY. while powered -> "
                "1D 04: 1 reboot, health SUSPECT; the second 10 s later -> 2, health LOOP | SUSPECT; then the PC (A2DP, "
                "no AVRCP) gets no CALL within 4 s; only LIST went out - and (2026-11-09) `CLOSE 2` for each lone browsing "
                "channel (`RING 2 … 1b AVRCP` without the control channel), ahead of the queue",
                len(t1) == 28 and t1[18] == 0x02 and le(t1, 19, 2) == 1 and t1[21] == 0
                and len(t2) == 28 and t2[18] == 0x03 and le(t2, 19, 2) == 2
                and set(thr.split(b"\r\n")) <= {b"LIST", b"CLOSE 2", b""} and thr.startswith(b"CLOSE 2\r\n")
                and thr.count(b"CLOSE 2") >= 1,
                f"t1 {t1.hex()} t2 {t2.hex()} thr {thr!r}")
    rig.close()

    # ---- 11. the CALL path: back to a parked Windows PC (act 6); its fresh stream is silent until PLAY -------------
    def cmds():
        done = bytes(uart.thr).rsplit(b"\r\n", 1)[0]   # whole lines only (a command may be half out)
        return [c for c in done.split(b"\r\n") if c and c not in (b"LIST", b"SET")]

    def nopdu(cs):
        """the commands without the play-status registrations (`SET <l> SELECT` + `AVRCP PDU 31 1` - again after each
        CHANGED, AN986 4.3)"""
        out = []
        for c in cs:
            if c == b"AVRCP PDU 31 1" and out and out[-1].startswith(b"SET "):
                out.pop()
            else:
                out.append(c)
        return out

    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    ticks(60)
    late_off(rig)
    module("LIST", "LIST 0")
    # the PC parked (the user's act 6 earlier): AVRCP control 1 + browsing 3 only; the phone plays on A2DP 0/2, AVRCP 4
    module("RING 1 a2:83:83:83:13:23 17 AVRCP", "RING 3 a2:83:83:83:13:23 1b AVRCP",
           "RING 0 62:81:81:81:11:a1 19 A2DP", "RING 2 62:81:81:81:11:a1 19 A2DP", "RING 4 62:81:81:81:11:a1 17 AVRCP",
           "A2DP STREAMING START 0")
    ticks(120)                                       # play status: the PC paused, the phone plays
    module("AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED",
           "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING")
    ticks(280)
    uart.thr.clear()
    pc7 = list(rtlib.pack7(bytes.fromhex("a28383831323")))
    use_pc = rbt.op_rc(rig, 0x05, 0x01, *pc7)
    ticks(60)
    a_pause = cmds()
    module("AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(60)                                        # registered again
    module("AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(60)                                        # the phone does not stop within 0.7 s (field: ~3 s)
    a_susp = nopdu(cmds())
    module("A2DP STREAMING STOP 0")                  # the phone stops by itself (~3 s); the sink never stops it
    ticks(40)
    a_call = nopdu(cmds())
    module("CALL 5", "CONNECT 5 A2DP 19", "CONNECT 6 A2DP 19")
    ticks(20)
    module("A2DP STREAMING START 5")                 # Windows: the fresh link streams at once, its player paused
    ticks(60)
    a_play = nopdu(cmds())
    module("AVRCP 1 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING")   # the PC plays (PLAY = play)
    ticks(60)
    module("AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING")   # registered again
    s_pc = src_list(rig)
    t_pc = s_pc[84:112] if s_pc else b""
    uart.thr.clear()
    phone7 = list(rtlib.pack7(bytes.fromhex("6281818111a1")))
    use_ph = rbt.op_rc(rig, 0x05, 0x01, *phone7)
    ticks(60)
    module("AVRCP 1 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")   # Windows: paused, stream kept
    ticks(60)
    module("AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(60)
    b_susp = nopdu(cmds())
    module("A2DP STREAMING STOP 5")                  # Windows lets its stream go by itself (field: 9-10 s)
    ticks(60)
    b_resume = nopdu(cmds())
    module("A2DP STREAMING START 0")                 # the phone resumed (its player paused by us)
    ticks(60)
    b_play = nopdu(cmds())
    s_ph = src_list(rig)
    t_ph = s_ph[84:112] if s_ph else b""
    thr11 = bytes(uart.thr)
    rig.close()
    call_pc = b"CALL a2:83:83:83:13:23 19 A2DP"
    ok &= check("11 the CALL path (field 2026-11-02 afternoon, play status PC PAUSED / phone PLAYING): phone -> a parked PC "
                "(AVRCP 1 only - the user's act 6): `SET 4 SELECT` + `AVRCP PAUSE`; the phone reports PAUSED and keeps "
                "its stream past 0.7 s - never a sink STOP (2026-11-10); its own STOP -> "
                "`CALL <pc> 19 A2DP`; the fresh link streams at once (Windows, paused) -> `SET 1 SELECT` + `AVRCP PLAY`; "
                "tail PLAYING, tries FRESH | PLAY | PLAY_UP.  Back to the phone: `SET 1 SELECT` + `AVRCP PAUSE`; the PC "
                "reports PAUSED, keeps its stream - not stopped; its own STOP -> the "
                "phone (PAUSED, paused by us): PLAY first, `SET 4 SELECT` + `AVRCP PLAY` - no sink "
                "resume (D31); its own START -> tail PLAYING, tries PLAY (0x01); no sink START, no CLOSE, no CALL for the "
                "phone",
                use_pc == 0 and a_pause == [b"SET 4 SELECT", b"AVRCP PAUSE"]
                and a_susp == a_pause
                and a_call == a_susp + [call_pc] and a_play == a_call + [b"SET 1 SELECT", b"AVRCP PLAY"]
                and len(t_pc) == 28 and t_pc[1] == 0 and t_pc[2] == 1 and t_pc[22] == 0x19
                and use_ph == 0 and b_susp == [b"SET 1 SELECT", b"AVRCP PAUSE"]
                and b_resume == b_susp + [b"SET 4 SELECT", b"AVRCP PLAY"] and b_play == b_resume
                and len(t_ph) == 28 and t_ph[1] == 0 and t_ph[2] == 1 and t_ph[22] == 0x01
                and b"CLOSE" not in thr11 and b"CALL" not in thr11 and b"STREAMING START" not in thr11,
                f"rc {use_pc}/{use_ph} a {a_pause} {a_susp} {a_call} {a_play} t_pc {t_pc.hex()} b {b_susp} {b_resume} "
                f"{b_play} t_ph {t_ph.hex()}")

    # ---- 12. field 2026-11-02 night (9e6c8c32): a playing Windows PC must not get PLAY (it is play/pause there) --------
    # the field lines: phone A2DP 0/1 + AVRCP 2 streams (selected); the PC (a2:..:13:23) A2DP 3/5 + AVRCP 4 (psm 17), its
    # YouTube playing but ignored by the module; 「이 기기로 듣기」 on the PC -> phone PAUSE, suspended (the field build
    # closed it), `START 3` at once.  a: with play status (PC PLAYING) -> no PLAY, tries KEPT; b: no answers (the "paused
    # by us" rule) -> no PLAY, tries NOT_OURS.  The old build sent `SET 4 SELECT` + `AVRCP PLAY` here and the PC paused.
    res12 = {}
    for variant in ("a", "b"):
        rig, uart = fresh_rig(image, syms, data)
        emu = rig.emu
        ticks(60)
        late_off(rig)
        module("LIST", "LIST 0")
        module("RING 0 62:81:81:81:11:a1 19 A2DP", "RING 1 62:81:81:81:11:a1 19 A2DP", "RING 2 62:81:81:81:11:a1 17 AVRCP",
               "A2DP STREAMING START 0",
               "RING 3 a2:83:83:83:13:23 19 A2DP", "RING 5 a2:83:83:83:13:23 19 A2DP", "RING 4 a2:83:83:83:13:23 17 AVRCP")
        ticks(120)
        if variant == "a":
            module("AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING",
                   "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING")
        ticks(400)
        uart.thr.clear()
        pc7 = list(rtlib.pack7(bytes.fromhex("a28383831323")))
        rc12 = rbt.op_rc(rig, 0x05, 0x01, *pc7)
        ticks(60)
        p12 = cmds()
        if variant == "a":
            module("AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")
        ticks(140)                                   # the phone does not stop within 0.7 s
        c12 = nopdu(cmds())
        module("A2DP STREAMING STOP 1", "A2DP STREAMING START 3")   # the phone's own STOP; the PC's stream at once
        ticks(250)                                   # 2 s
        after = nopdu(cmds())[len(c12):]
        s12 = src_list(rig)
        res12[variant] = (rc12, p12, c12, after, s12[84:116] if s12 else b"", bytes(uart.thr))
        rig.close()
    ra, rb = res12["a"], res12["b"]

    def no_play(r):
        return b"AVRCP PLAY" not in r[3] and b"A2DP STREAMING START" not in r[3]

    ok &= check("12 field 2026-11-02 night: phone streaming (selected), the Windows PC playing but ignored; 1D 05 01 PC -> "
                "`SET 2 SELECT` + `AVRCP PAUSE` (phone), 0.7 s -> a (reported PAUSED, stream kept) and b (no status): "
                "never a sink STOP (2026-11-10) - its own STOP; the PC's `START 3` "
                "-> NO `AVRCP PLAY` (a: it reported PLAYING - tries KEPT 0x20, its status byte PLAYING | LIVE; b: the "
                "piano never paused it - tries "
                "NOT_OURS 0x40); tail PLAYING; no CLOSE",
                all(r[0] == 0 and r[1][:2] == [b"SET 2 SELECT", b"AVRCP PAUSE"] and no_play(r) and b"CLOSE" not in r[5]
                    and len(r[4]) == 32 and r[4][1] == 0 and r[4][2] == 1 for r in (ra, rb))
                and ra[2] == [b"SET 2 SELECT", b"AVRCP PAUSE"] and rb[2] == [b"SET 2 SELECT", b"AVRCP PAUSE"]
                and b"STREAMING STOP" not in ra[5] and b"STREAMING STOP" not in rb[5]
                and ra[4][22] == 0x20 and rb[4][22] == 0x40 and 0x0A in ra[4][28:32],
                f"a {ra[1]} {ra[2]} {ra[3]} tail {ra[4].hex()} | b {rb[1]} {rb[2]} {rb[3]} tail {rb[4].hex()}")

    # ---- 13. field 2026-11-03 (1afe1978, test 2-가): #1 phone -> playing PC, act 6 on the phone, #2 PC -> phone ------
    # #1 as 12a: PC -> phone PAUSE, suspended, `START 3` -> no PLAY; the phone stays listed, paused by the piano.  The
    # user's act 6 on the phone (`CLOSE 0`; its AVRCP goes too: NO CARRIER 1/0/2) - the §9 state, which a switch no longer
    # makes.  #2 the phone: PC PAUSE (CHANGED PAUSED), 0.7 s, `SET 3 SELECT` + `A2DP STREAMING STOP` -> its STOP ->
    # `CALL <phone> 19 A2DP` -> CALL 0, CONNECT 0/1 -> 0.4 s later `A2DP STREAMING START 0` and 10 ms later `A2DP
    # STREAMING STOP 0` (its player paused); the sink opens its AVRCP (`CALL <phone> 17 AVRCP` -> CALL 2, CONNECT 2
    # AVRCP 17) -> registration -> INTERIM PAUSED -> `SET 2 SELECT` + `AVRCP PLAY` once -> START 0.
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    ticks(60)
    late_off(rig)
    module("LIST", "LIST 0")
    module("RING 0 62:81:81:81:11:a1 19 A2DP", "RING 1 62:81:81:81:11:a1 19 A2DP", "RING 2 62:81:81:81:11:a1 17 AVRCP",
           "A2DP STREAMING START 0",
           "RING 3 a2:83:83:83:13:23 19 A2DP", "RING 5 a2:83:83:83:13:23 19 A2DP", "RING 4 a2:83:83:83:13:23 17 AVRCP")
    ticks(120)
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING",
           "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING")
    ticks(400)
    uart.thr.clear()
    pc7 = list(rtlib.pack7(bytes.fromhex("a28383831323")))
    phone7 = list(rtlib.pack7(bytes.fromhex("6281818111a1")))
    rc1 = rbt.op_rc(rig, 0x05, 0x01, *pc7)
    ticks(60)
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(140)
    module("A2DP STREAMING STOP 0", "A2DP STREAMING START 3")
    ticks(250)
    one = cmds()
    s1 = src_list(rig)
    t1 = s1[84:116] if s1 else b""
    ph_slot = next((i for i in range(4) if s1 and bytes(s1[29 + 14 * i: 35 + 14 * i]).hex() == "6281818111a1"), -1)
    ps1 = t1[28 + ph_slot] if len(t1) == 32 and ph_slot >= 0 else -1
    mute_rc = rbt.op_rc(rig, 0x05, 0x06, *phone7)   # the user's act 6 on the phone
    ticks(40)
    mute = cmds()[len(one):]
    module("NO CARRIER 1 ERROR 0", "NO CARRIER 0 ERROR 0", "NO CARRIER 2 ERROR 0")
    ticks(40)
    uart.thr.clear()
    rc2 = rbt.op_rc(rig, 0x05, 0x01, *phone7)
    ticks(60)
    two_pause = cmds()
    module("AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(140)                                       # Windows keeps its stream after the pause - never stopped by the sink
    two_susp = cmds()
    module("A2DP STREAMING STOP 3")                  # it lets go by itself
    ticks(40)
    two_call = cmds()
    module("CALL 0", "CONNECT 0 A2DP 19", "CONNECT 1 A2DP 19")
    ticks(50)                                        # 0.4 s
    module("A2DP STREAMING START 0")
    ticks(1)
    module("A2DP STREAMING STOP 0")                  # 10 ms later: the phone's player is paused
    ticks(30)
    s_blip = src_list(rig)
    t_blip = s_blip[84:116] if s_blip else b""
    ticks(170)                                       # 1.6 s after the STOP: the sink's AVRCP CALL is out
    two_av = cmds()
    module("CALL 2", "CONNECT 2 AVRCP 17")
    ticks(40)
    two_reg = cmds()
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(60)
    two_play = cmds()
    module("A2DP STREAMING START 0")
    ticks(20)
    s2 = src_list(rig)
    t2 = s2[84:116] if s2 else b""
    thr13 = bytes(uart.thr)
    rig.close()
    call_ph = b"CALL 62:81:81:81:11:a1 19 A2DP"
    av_ph = b"CALL 62:81:81:81:11:a1 17 AVRCP"
    new = two_play[len(two_reg):]
    reg_tail = two_reg[-2:]
    one, two_pause, two_susp, two_call, two_av, two_reg = map(nopdu, (one, two_pause, two_susp, two_call, two_av,
                                                                      two_reg))
    ok &= check("13 field 2026-11-03 (test 2-가): #1 phone -> playing PC: `SET 2 SELECT` + `AVRCP PAUSE`, the phone "
                "reports PAUSED with its stream open - never a sink STOP (2026-11-10), its own STOP, "
                "`START 3` -> no PLAY (KEPT); its status byte has OURS 0x20; act 6 on the phone -> `CLOSE 0` only.  "
                "#2 PC -> phone: `SET 4 SELECT` + `AVRCP PAUSE`, the PC paused with its stream open: not "
                "stopped, its own STOP -> `CALL <phone> 19 A2DP`; its START 0 + STOP 0 10 ms later is NOT the end (tail phase still on, result 0); "
                "`CALL <phone> 17 AVRCP` once; CONNECT 2 -> `SET 2 SELECT` + `AVRCP PDU 31 1`; INTERIM PAUSED -> "
                "`SET 2 SELECT` + `AVRCP PLAY` exactly once; START 0 -> tail result 1, tries FRESH | PLAY (0x09); no PLAY "
                "ever to the PC; no sink START, no CLOSE in #2",
                rc1 == 0 and one[:2] == [b"SET 2 SELECT", b"AVRCP PAUSE"]
                and b"A2DP STREAMING STOP" not in one and b"CLOSE 0" not in one
                and b"AVRCP PLAY" not in one and len(t1) == 32 and t1[22] == 0x20 and ps1 >= 0 and ps1 & 0x20
                and mute_rc == 0 and mute == [b"CLOSE 0"]
                and rc2 == 0 and two_pause == [b"SET 4 SELECT", b"AVRCP PAUSE"]
                and two_susp == two_pause and two_call == two_susp + [call_ph]
                and len(t_blip) == 32 and t_blip[1] != 0 and t_blip[2] == 0
                and two_av == two_call + [av_ph]
                and two_reg == two_av and reg_tail == [b"SET 2 SELECT", b"AVRCP PDU 31 1"]
                and new == [b"SET 2 SELECT", b"AVRCP PLAY"]
                and len(t2) == 32 and t2[1] == 0 and t2[2] == 1 and t2[22] == 0x09
                and b"STREAMING START\r\n" not in thr13 and b"CLOSE" not in thr13,
                f"rc {rc1}/{rc2} #1 {one} tail {t1.hex()} phone ps {ps1} mute {mute_rc} {mute} | #2 {two_pause} "
                f"{two_susp} {two_call} blip tail {t_blip.hex()} av {two_av[len(two_call):]} reg {reg_tail} play {new} "
                f"tail {t2.hex()}")

    # ---- 14. the BT hold follows the play status: Windows paused, its stream left open -------------
    psyms = rtlib.payload(which)[5]
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    ticks(60)
    module("LIST", "LIST 0")
    uart.thr.clear()
    module("RING 3 a2:83:83:83:13:23 19 A2DP", "RING 5 a2:83:83:83:13:23 19 A2DP", "RING 4 a2:83:83:83:13:23 17 AVRCP",
           "A2DP STREAMING START 3")
    ticks(120)
    reg1 = cmds()
    module("AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING")
    ticks(10)
    rig.tick(1, ms=60 * 60 * 1000)
    h_play = rpw.expire(rig)
    ticks(1)
    d_play = rig.diag()
    module("AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")   # no STREAMING STOP (Windows)
    ticks(60)
    reg2 = cmds()
    module("AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(5)
    rig.site(0x400AA874)
    c_rel = emu.read_u32(rpw.APO_COUNTER)
    rpw.ticks(rig, psyms, 14 * 60 * 50)              # 14 min of 20 ms ticks, the last one real
    rig.tick(1, ms=14 * 60 * 1000)
    d14 = rig.diag()
    rpw.ticks(rig, psyms, 60 * 50)                   # the 15th minute
    ticks(2)
    d15 = rig.diag()
    rig.raw_reply(2, rp.req(rpw.OP_STANDBY, 0x02), rpw.OP_STANDBY)
    ticks(15)
    module("AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING")
    ticks(60)
    module("AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING")
    ticks(5)
    rig.tick(1, ms=30 * 60 * 1000)
    h_again = rpw.expire(rig)
    ticks(1)
    d_again = rig.diag()
    sent14 = bytes(uart.thr)
    rig.close()
    reg = [b"SET 4 SELECT", b"AVRCP PDU 31 1"]
    ok &= check("14 BT hold and play status: the Windows PC streams alone -> `SET 4 SELECT` + `AVRCP PDU 31 "
                "1`; INTERIM PLAYING -> after 60 virtual minutes the real 20 ms site holds (counter at the limit, flag 0x08); "
                "CHANGED PAUSED, no STREAMING STOP -> registered again, INTERIM PAUSED -> the next site resets the counter "
                "(1, +1 = 2), not held, still streaming; 14 min later ON, at 15 min standby; woken, CHANGED / INTERIM "
                "PLAYING -> held again (30 min: counter at the limit, flag 0x08); never `AVRCP PLAY` / `AVRCP PAUSE`",
                reg1 == reg and reg2 == reg + reg and h_play == (45000, 0) and d_play["flags"] & 0x08
                and c_rel == 2 and d14["state"] == rpw.SB["on"] and not d14["flags"] & 0x08 and d14["bt"] == 2
                and d15["state"] == rpw.SB["standby"] and h_again == (45000, 0) and d_again["flags"] & 0x08
                and d_again["state"] == rpw.SB["on"] and b"AVRCP PLAY" not in sent14 and b"AVRCP PAUSE" not in sent14,
                f"reg {reg1} {reg2} held {h_play} {d_play['flags']:#x} rel {c_rel} 14 min {d14['state']} "
                f"{d14['flags']:#x} bt {d14['bt']} 15 min {d15['state']} again {h_again} {d_again['flags']:#x}")

    # ---- 15. field 2026-11-03 (a12c870a): the one switch; old strategy writes refused ---------
    # the §10 links: the PC (a2:..:13:23, Windows + Chrome YouTube) A2DP 0/2 + AVRCP 1 streaming, PLAYING; the phone
    # (62:..:11:a1) A2DP 3/4 + AVRCP 5, PAUSED.  D27 = 「이 기기로 듣기」 on the phone; D25 = back to the PC (SUSPEND).
    def d27(strat, module_switches=False, d25=False):
        r_, u_ = fresh_rig(image, syms, data)
        res = {}

        def mod(*lines):
            for ln in lines:
                r_.bt_line(ln)
                r_.emu.call(asm, max_insns=2_000_000)

        def tk(n):
            for _ in range(n):
                r_.tick()
                u_.finish()

        def cm():
            done = bytes(u_.thr).rsplit(b"\r\n", 1)[0]
            return nopdu([c for c in done.split(b"\r\n") if c and c not in (b"LIST", b"SET")])

        tk(60)
        late_off(r_)
        mod("LIST", "LIST 0")
        res["pol_rc"] = rbt.op_rc(r_, 0x06, 2, 0x04, 15, 5, 0, 0, 0, 0, 0, 0, 0, 0, strat)
        pr = r_.raw_reply(2, rp.req(OP, 0x06), OP)
        res["pol"] = pr[14] if pr is not None and len(pr) == 19 else None
        mod("RING 0 a2:83:83:83:13:23 19 A2DP", "RING 2 a2:83:83:83:13:23 19 A2DP", "RING 1 a2:83:83:83:13:23 17 AVRCP",
            "A2DP STREAMING START 0",
            "RING 3 62:81:81:81:11:a1 19 A2DP", "RING 4 62:81:81:81:11:a1 19 A2DP", "RING 5 62:81:81:81:11:a1 17 AVRCP")
        tk(120)
        mod("AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING",
            "AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
        tk(400)
        src0 = src_list(r_)
        res["b5"] = src0[5] >> 4 if src0 else None
        u_.thr.clear()
        res["rc"] = rbt.op_rc(r_, 0x05, 0x01, *list(rtlib.pack7(bytes.fromhex("6281818111a1"))))
        tk(60)
        res["first"] = cm()
        if b"AVRCP PAUSE" in res["first"]:
            mod("AVRCP 1 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")   # Windows: stream kept
            tk(60)
            mod("AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
        if module_switches:
            tk(120)                                  # the paused PC keeps its stream: never stopped (2026-11-10)
            mod("A2DP STREAMING STOP 0", "A2DP STREAMING START 3")   # its own STOP / the module takes the sink's START
            tk(60)
            mod("AVRCP 5 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING")   # it plays
            tk(60)
            mod("AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING")
        else:
            tk(120)
        res["mid"] = cm()
        if b"A2DP STREAMING STOP" in res["mid"]:
            mod("A2DP STREAMING STOP 0")             # the sink's suspend honoured (field 2026-11-02 #3)
            tk(60)
            mod("A2DP STREAMING START 3")            # the phone plays after its PLAY
            mod("AVRCP 5 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING")
            tk(60)
            mod("AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING")
        tk(40)
        res["all"] = cm()
        s_ = src_list(r_)
        res["tail"] = s_[84:116] if s_ else b""
        if d25:
            u_.thr.clear()
            rbt.op_rc(r_, 0x05, 0x01, *list(rtlib.pack7(bytes.fromhex("a28383831323"))))
            tk(60)
            mod("AVRCP 5 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")
            tk(60)
            mod("AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
            tk(60)                                   # Android stops ~3 s after a pause: never stopped by the sink
            b_s = cm()
            mod("A2DP STREAMING STOP 3")
            tk(60)
            b_r = cm()
            mod("A2DP STREAMING START 0")            # the PC resumed (its player paused by us)
            tk(60)
            res["d25"] = (b_s, b_r, cm())
            s_ = src_list(r_)
            res["tail25"] = s_[84:116] if s_ else b""
        res["thr"] = bytes(u_.thr)
        r_.close()
        return res

    pause_pc = [b"SET 1 SELECT", b"AVRCP PAUSE"]
    r0 = d27(0)
    r0s = d27(0, module_switches=True, d25=True)
    r1 = d27(1)                                      # an old app's 「일시정지 후 기다림」: refused
    r4 = d27(4, module_switches=True)                # ... 「선택만」: refused
    rs = (r0, r0s, r1, r4)
    ok &= check("15 field 2026-11-03, the one switch: 1D 06 strategy 0 (14 B) rc 0, strategies 1 / 4 "
                "(an old app) rc 1 - the policy stays (read back byte 14 of 18 = 0, 1D 04 byte 5 bits 4-6 = 1), the switch "
                "as strategy 0.  The sink never stops a source's stream (2026-11-10).  D27 PC -> phone: "
                "`SET 1 SELECT` + `AVRCP PAUSE`; the PC reports PAUSED, keeps its stream - the switch waits (phase PAUSE, "
                "no result yet); its own STOP with the phone's START: no PLAY, KEPT, result 1.  D25 back to the PC: "
                "`SET 5 SELECT` + `AVRCP PAUSE`, the phone not stopped, its own STOP -> PLAY first `SET 1 SELECT` + "
                "`AVRCP PLAY` (no sink resume - D31), tries 0x01.  Never CLOSE, never `CALL … 19 A2DP`",
                [r["pol_rc"] for r in rs] == [0, 0, 1, 1] and all(r["rc"] == 0 for r in rs)
                and all(b"CLOSE" not in r["thr"] and b"19 A2DP" not in r["thr"] for r in rs)
                and [r["pol"] for r in rs] == [0, 0, 0, 0] and [r["b5"] for r in rs] == [1, 1, 1, 1]
                and r0["all"] == pause_pc and r0["tail"][1] == 1 and r0["tail"][2] == 0
                and r1["all"] == pause_pc and r1["tail"][1] == 1 and r1["tail"][2] == 0
                and r0s["all"] == pause_pc and r0s["tail"][2] == 1 and r0s["tail"][22] == 0x20
                and r4["all"] == pause_pc and r4["tail"][2] == 1 and r4["tail"][22] == 0x20
                and all(b"STREAMING STOP" not in r["thr"] for r in rs)
                and r0s["d25"][0] == [b"SET 5 SELECT", b"AVRCP PAUSE"]
                and r0s["d25"][1] == r0s["d25"][0] + [b"SET 1 SELECT", b"AVRCP PLAY"]
                and r0s["d25"][2] == r0s["d25"][1]
                and r0s["tail25"][2] == 1 and r0s["tail25"][22] == 0x01,
                " | ".join(f"s{i} rc {r['pol_rc']} pol {r['pol']} b5 {r['b5']} {r['all']} tail {r['tail'].hex()}"
                           for i, r in enumerate(rs)) + f" | d25 {r0s.get('d25')} tail {r0s.get('tail25', b'').hex()}")

    # ---- 16. field 2026-11-09 (e5bb2372): interleaved module lines; the reconnect choice on NOR -------
    # a: the exact garbled lines through the UART1 ring and the real line assembler: the PC (a2:..:13:23, A2DP 0, AVRCP
    # 1) plays and is selected, the phone (A2DP 4, AVRCP 6) paused; the PC pauses -> registered again (`SET 1 SELECT` +
    # `AVRCP PDU 31 1`) - the module's echo runs into the phone's CHANGED: `SAVRCP 6 … PLAYING` / `ET 1 SELECT`.  The
    # phone's PLAYING is read (1D 04 byte 112+slot, recovered count byte 118); STOP 0 + START 4 -> the guard PAUSEs the
    # phone (`SET 6 SELECT` + `AVRCP PAUSE` - the field build skipped it: "no PAUSE: it is paused").  D31's `SETA2DP
    # STREAMING STOP 4` / ` 0 SELECT` is read as the phone's STOP.  `… PLAYINGET 1 SELECT` (unreadable) -> `SET 6 SELECT`
    # + `AVRCP PDU 31 1` (asked again), garbled count byte 117.
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    ticks(60)
    module("LIST", "LIST 0")
    module("RING 0 a2:83:83:83:13:23 19 A2DP", "RING 1 a2:83:83:83:13:23 17 AVRCP", "A2DP STREAMING START 0",
           "RING 4 62:81:81:81:11:a1 19 A2DP", "RING 6 62:81:81:81:11:a1 17 AVRCP")
    ticks(120)
    module("AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING",
           "AVRCP 6 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED",
           "AVRCP 1 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")
    uart.thr.clear()
    ticks(60)
    rereg = cmds()
    module("SAVRCP 6 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING", "ET 1 SELECT")
    ticks(2)
    s_a = src_list(rig)
    ph = next((i for i in range(4) if s_a and bytes(s_a[29 + 14 * i: 35 + 14 * i]).hex() == "6281818111a1"), -1)
    ps_a = s_a[112 + ph] if len(s_a) == 120 and ph >= 0 else -1
    rec_a = s_a[118] if len(s_a) == 120 else -1
    module("AVRCP 1 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(30)
    uart.thr.clear()
    module("A2DP STREAMING STOP 0", "A2DP STREAMING START 4")
    ticks(120)
    guard = bytes(uart.thr)
    module("AVRCP 6 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(30)
    module("AVRCP 6 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
    module("SETA2DP STREAMING STOP 4", " 0 SELECT")
    ticks(2)
    s_b = src_list(rig)
    ph_streams = bool(s_b[28 + 14 * ph] & 0x01) if len(s_b) == 120 and ph >= 0 else None
    rec_b = s_b[118] if len(s_b) == 120 else -1
    ticks(60)
    uart.thr.clear()
    module("AVRCP 6 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYINGET 1 SELECT")
    ticks(60)
    asked = cmds()
    s_c = src_list(rig)
    ps_c = s_c[112 + ph] if len(s_c) == 120 and ph >= 0 else -1
    garb = s_c[117] if len(s_c) == 120 else -1
    rig.close()
    ok &= check("16a field 2026-11-09 interleaved lines (real UART ring + assembler): `SAVRCP 6 … CHANGED … PLAYING` / `ET 1 "
                "SELECT` -> the phone PLAYING (status byte 0x02 | LIVE 0), lines read after a prefix 1; STOP 0 + START 4 "
                "-> `SET 6 SELECT` + `AVRCP PAUSE` (not skipped); `SETA2DP STREAMING STOP 4` -> the phone not streaming, "
                "read after a prefix 2; `… PLAYINGET 1 SELECT` -> status unknown, `SET 6 SELECT` + `AVRCP PDU 31 1` "
                "again, garbled 1",
                rereg == [b"SET 1 SELECT", b"AVRCP PDU 31 1"] and ps_a >= 0 and ps_a & 7 == 2 and rec_a == 1
                and b"SET 6 SELECT\r\nAVRCP PAUSE\r\n" in guard and ph_streams is False and rec_b == 2
                and asked == [b"SET 6 SELECT", b"AVRCP PDU 31 1"] and ps_c >= 0 and ps_c & 7 == 0 and garb == 1,
                f"rereg {rereg} ps {ps_a} rec {rec_a} guard {guard} streams {ph_streams} rec {rec_b} asked {asked} ps "
                f"{ps_c} garbled {garb}")

    # b: 1D 06 with the reconnect byte (15 B): preferred only + the PC preferred -> 18 B reply, written to the kvstore in
    # unused_3 (every NOR write inside it); power-on again on the same NOR: read back (NOR bits KEPT | LOADED); AUTOCALL
    # read OFF -> the piano calls the PC (`CALL a2:…:23 19 A2DP`).  Then off -> power-on: AUTOCALL OFF, nothing called.
    def power_on(nor):
        r_ = Rig(image, syms, nor_mem=nor)
        u_ = Uart1Tx(r_.emu)
        rbt.Hier(r_.emu)
        st_, _ = r_.pn.boot()
        r_.emu.write_bytes(BINDING, b"\x03")
        r_.tick(3)
        return r_, u_, st_

    def setlist(r_):
        for ln in ("SET", "SET BT BDADDR 02:07:80:aa:bb:cc", "SET BT NAME CA79 Audio", "SET PROFILE A2DP SINK", "SET"):
            r_.bt_line(ln)
            r_.emu.call(asm, max_insns=2_000_000)

    pc7 = list(rtlib.pack7(bytes.fromhex("a28383831323")))
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    ticks(60)
    module("LIST", "LIST 0")
    n0 = len(rig.pn.nor_calls)
    w1 = rig.raw_reply(2, rp.req(OP, 0x06, 2, 0x04, 15, 5, 1, *pc7, 0, 1), OP)
    ticks(5)
    r1 = rig.raw_reply(2, rp.req(OP, 0x06), OP)
    calls = rig.pn.nor_calls[n0:]
    nor_in = bool(calls) and all(c[0] == "buf" and rpw.KV_LO <= c[3] < rpw.KV_HI for c in calls)
    nor1 = bytes(rig.pn.nor.mem)
    rig.close()
    rig, uart, st1 = power_on(nor1)
    r2 = rig.raw_reply(2, rp.req(OP, 0x06), OP)
    for _ in range(420):                             # > 3 s: the AUTOCALL read (someone to reconnect)
        rig.tick()
        uart.finish()
    read1 = bytes(uart.thr)
    setlist(rig)                                     # no `SET CONTROL AUTOCALL` line: OFF
    for _ in range(700):                             # 5.6 s: the first reconnect (4 s)
        rig.tick()
        uart.finish()
    thr1 = bytes(uart.thr)
    w2 = rig.raw_reply(2, rp.req(OP, 0x06, 2, 0x04, 15, 5, 1, *pc7, 0, 0), OP)
    rig.tick(5)
    nor2 = bytes(rig.pn.nor.mem)
    rig.close()
    rig, uart, st2 = power_on(nor2)
    r3 = rig.raw_reply(2, rp.req(OP, 0x06), OP)
    for _ in range(420):
        rig.tick()
        uart.finish()
    setlist(rig)
    for _ in range(1500):                            # 12 s
        rig.tick()
        uart.finish()
    thr2 = bytes(uart.thr)
    s_r = src_list(rig)
    rig.close()
    pc_call = b"CALL a2:83:83:83:13:23 19 A2DP"
    ok &= check("16b reconnect choice (1D 06 byte 14) + NOR: write 15 B (preferred only, PC preferred) -> rc 0, 19 B reply "
                "with 1, NOR writes only in unused_3 (write buffer, dispatch off); power-on on that NOR (slot A) -> 1D 06 "
                "answers reconnect 1, the PC preferred, NOR bits KEPT | LOADED (3); AUTOCALL read, listed OFF -> `CALL "
                "a2:83:83:83:13:23 19 A2DP`; set off, power-on -> reconnect 0 (1D 04 byte 116 = 1 | (3 << 2) | 0x40 | 0x80 - the late stop on), no CALL in "
                "12 s",
                w1 is not None and w1[1] == 0 and len(w1) == 19 and w1[15] == 1 and r1 is not None and r1[16] == 1
                and nor_in and st1["active"] == rpw.ro.ACT_A and r2 is not None and r2[15] == 1 and r2[6] == 1
                and r2[16] == 3 and b"SET\r\n" in read1
                and pc_call in thr1 and w2 is not None and w2[1] == 0 and w2[15] == 0
                and st2["active"] == rpw.ro.ACT_A and r3 is not None and r3[15] == 0 and r3[16] == 3
                and b"CALL" not in thr2 and len(s_r) == 120 and s_r[116] == (1 | 3 << 2 | 0x40 | 0x80),
                f"w1 {w1 and w1.hex()} r1 {r1 and r1.hex()} nor {len(calls)} in {nor_in} r2 {r2 and r2.hex()} read "
                f"{read1[:40]!r} thr1 {thr1[-80:]!r} w2 {w2 and w2.hex()} r3 {r3 and r3.hex()} thr2 {thr2[-80:]!r} "
                f"b116 {s_r[116] if len(s_r) == 120 else None}")


    # ---- 18. field 2026-11-09 evening (a6fdc5a2): a status line without its link id; a stale "paused" -----
    # the phone (62:..:11:a1, A2DP 0, AVRCP 2) streams and is selected, the PC (a2:..:13:23, A2DP 3, AVRCP 4) paused.
    # `AVRCP  REGISTER_NOTIFICATION_RSP CHANGED … PLAYING` (no link id) -> both registered devices asked again; the phone
    # does not answer, pauses and plays by itself (STOP 0 / START 0) - its last report PAUSED is older than its stream.
    # 1D 05 01 PC -> `SET 2 SELECT` + `AVRCP PAUSE` (the field build: "no PAUSE: it is paused"); never a sink STOP
    # (2026-11-10), its own STOP -> `SET 4 SELECT` + `AVRCP PLAY` (PC paused).  A late STOP
    # of the phone -> `SET 3 SELECT` + `A2DP STREAMING START` once.  1D 05 01 PC again (selected, streaming, playing) ->
    # `SET 3 SELECT` + `A2DP STREAMING START`, no PLAY (the field build sent nothing).
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    ticks(60)
    module("LIST", "LIST 0")
    module("RING 0 62:81:81:81:11:a1 19 A2DP", "RING 2 62:81:81:81:11:a1 17 AVRCP", "A2DP STREAMING START 0",
           "RING 3 a2:83:83:83:13:23 19 A2DP", "RING 4 a2:83:83:83:13:23 17 AVRCP")
    ticks(120)
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED",
           "AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(60)
    uart.thr.clear()
    module("AVRCP  REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING")
    ticks(120)
    rereg18 = cmds()
    module("AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
    s18 = src_list(rig)
    garb18 = s18[117] if len(s18) == 120 else -1
    module("A2DP STREAMING STOP 0")
    ticks(30)
    module("A2DP STREAMING START 0")
    ticks(200)
    uart.thr.clear()
    rc18 = rbt.op_rc(rig, 0x05, 0x01, *list(rtlib.pack7(bytes.fromhex("a28383831323"))))
    ticks(60)
    pause18 = nopdu(cmds())
    ticks(100)
    susp18 = nopdu(cmds())
    module("A2DP STREAMING STOP 0")                  # the phone's own STOP (the sink never stops it - 2026-11-10)
    ticks(60)
    play18 = nopdu(cmds())
    module("A2DP STREAMING START 3", "AVRCP 4 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING")
    ticks(60)
    module("AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING")
    ticks(20)
    uart.thr.clear()
    module("A2DP STREAMING START 0")
    ticks(5)
    module("A2DP STREAMING STOP 0")
    ticks(60)
    late18 = bytes(uart.thr)
    ticks(200)
    uart.thr.clear()
    rc18b = rbt.op_rc(rig, 0x05, 0x01, *list(rtlib.pack7(bytes.fromhex("a28383831323"))))
    ticks(60)
    again18 = nopdu(cmds())
    rig.close()
    ok &= check("18 field 2026-11-09 evening: `AVRCP  REGISTER_NOTIFICATION_RSP CHANGED … PLAYING` (no link id) -> `SET 2 "
                "SELECT` + `AVRCP PDU 31 1` and `SET 4 SELECT` + `AVRCP PDU 31 1`, garbled 1; the phone's stream newer than "
                "its PAUSED: 1D 05 01 PC -> `SET 2 SELECT` + `AVRCP PAUSE`, no sink STOP (2026-11-10), "
                "its own STOP -> `SET 4 SELECT` + `AVRCP PLAY`; the phone's late STOP -> `SET 3 SELECT` + `A2DP STREAMING "
                "START`; the PC picked again -> `SET 3 SELECT` + `A2DP STREAMING START`, no PLAY",
                b"SET 2 SELECT" in rereg18 and b"SET 4 SELECT" in rereg18 and rereg18.count(b"AVRCP PDU 31 1") == 2
                and garb18 == 1 and rc18 == 0 and pause18 == [b"SET 2 SELECT", b"AVRCP PAUSE"]
                and susp18 == pause18
                and play18 == susp18 + [b"SET 4 SELECT", b"AVRCP PLAY"]
                and b"SET 3 SELECT\r\nA2DP STREAMING START\r\n" in late18
                and rc18b == 0 and again18 == [b"SET 3 SELECT", b"A2DP STREAMING START"],
                f"rereg {rereg18} garbled {garb18} rc {rc18}/{rc18b} pause {pause18} susp {susp18} play {play18} late "
                f"{late18!r} again {again18}")

    # ---- 20. field 2026-11-10 (c0b89c60 / f2056ae8): a PC without AVRCP; paused by us; let go late ----
    # the phone (62:..:11:a1, A2DP 0/1, AVRCP 2) plays and is selected; the PC (a2:..:13:23, A2DP 4/5) has no AVRCP (its
    # service disabled on Windows).  1D 05 01 PC -> `SET 2 SELECT` + `AVRCP PAUSE`; the phone sends no CHANGED -> 1 s later
    # `SET 2 SELECT` + `AVRCP PDU 31 1` once; no sink STOP; its own STOP -> nothing: no sink START to a source without
    # AVRCP (f2056ae8 sent `SET 4 SELECT` + `A2DP STREAMING START` here and the PC's stream stayed silent
    # when it played, §19.6 #2): 1D 04 result 2 (READY, "press play on the PC"); the PC's own `START 4` -> nothing.  1D 05
    # 01 phone while the PC plays: nothing to the PC (no AVRCP - never a sink STOP), the switch waits (phase PAUSE) past
    # 12.5 s (f2056ae8: BUSY at 12 s) up to 30 s -> BUSY; the PC's own STOP 31 s after the pick (§19.6 #3) -> the switch
    # goes on: `SET 2 SELECT` + `AVRCP PLAY` (paused by us, report PLAYING stale); its START 0 -> tail PLAYING, tries PLAY.
    # The piano's own `CALL <PC> 17 AVRCP` (4 s after its A2DP, unanswered here) is left out of the comparisons.
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    ticks(60)
    module("LIST", "LIST 0")
    module("RING 0 62:81:81:81:11:a1 19 A2DP", "RING 1 62:81:81:81:11:a1 19 A2DP", "RING 2 62:81:81:81:11:a1 17 AVRCP",
           "A2DP STREAMING START 0", "RING 4 a2:83:83:83:13:23 19 A2DP", "RING 5 a2:83:83:83:13:23 19 A2DP")
    ticks(120)
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING")
    ticks(300)
    uart.thr.clear()
    pc7 = list(rtlib.pack7(bytes.fromhex("a28383831323")))
    ph7 = list(rtlib.pack7(bytes.fromhex("6281818111a1")))
    nocall = lambda cs: [c for c in cs if not c.startswith(b"CALL ")]
    rc19 = rbt.op_rc(rig, 0x05, 0x01, *pc7)
    ticks(60)
    p19 = nocall(cmds())
    ticks(150)                                       # no CHANGED from the phone
    r19 = nocall(cmds())
    # (no answer to it either - its last report PLAYING stays, as in the field)
    module("A2DP STREAMING STOP 0")                  # Android lets go ~3 s after the pause
    ticks(80)
    s19 = nocall(cmds())
    t19r = (src_list(rig) or b"")[84:116]
    module("A2DP STREAMING START 4")                 # the user plays on the PC: its own stream
    ticks(150)
    d19 = src_devs(src_list(rig))
    pc_ok = not d19.get("6281818111a1", (0, 0, 0))[0] & 0x01 and bool(d19.get("a28383831323", (0, 0, 0))[0] & 0x01)
    n19 = nocall(cmds())
    uart.thr.clear()
    rc19b = rbt.op_rc(rig, 0x05, 0x01, *ph7)
    ticks(1563)                                      # 12.5 s: the PC keeps its stream - still waiting
    w19 = nocall(cmds())
    t19w = (src_list(rig) or b"")[84:116]
    ticks(2313)                                      # 31 s after the pick: BUSY (30 s) a moment before
    t19b = (src_list(rig) or b"")[84:116]
    module("A2DP STREAMING STOP 4")                  # Windows lets go ~20 s after the user's pause (§19.6 #3)
    ticks(100)
    b19 = nopdu(nocall(cmds()))
    module("A2DP STREAMING START 0")
    ticks(20)
    t19 = (src_list(rig) or b"")[84:116]
    thr19 = bytes(uart.thr)
    rig.close()
    ok &= check("20 field 2026-11-10: 1D 05 01 PC (no AVRCP) while the phone plays -> `SET 2 SELECT` + "
                "`AVRCP PAUSE`; no CHANGED -> `SET 2 SELECT` + `AVRCP PDU 31 1` once; no sink STOP; its own STOP -> no "
                "sink START (f2056ae8: `SET 4 SELECT` + `A2DP STREAMING START`), 1D 04 result 2 READY, tries 0; the PC's "
                "own START 4 -> nothing sent, the PC streaming.  1D 05 01 phone: nothing sent while the PC streams, phase "
                "PAUSE at 12.5 s (f2056ae8: BUSY at 12 s), BUSY at 31 s; the PC's own STOP after that -> the switch goes "
                "on: `SET 2 SELECT` + `AVRCP PLAY` (paused by us, report PLAYING stale), no sink START; tail PLAYING, "
                "tries PLAY",
                rc19 == 0 and p19 == [b"SET 2 SELECT", b"AVRCP PAUSE"]
                and r19 == p19 + [b"SET 2 SELECT", b"AVRCP PDU 31 1"]
                and s19 == r19 and len(t19r) == 32 and t19r[1] == 0 and t19r[2] == 2 and t19r[22] == 0
                and pc_ok and n19 == r19
                and rc19b == 0 and w19 == [] and len(t19w) == 32 and t19w[1] == 1
                and len(t19b) == 32 and t19b[1] == 0 and t19b[2] == 3
                and b19 == [b"SET 2 SELECT", b"AVRCP PLAY"] and b"STREAMING" not in thr19
                and len(t19) == 32 and t19[1] == 0 and t19[2] == 1 and t19[22] == 0x01,
                f"rc {rc19}/{rc19b} pause {p19} rereg {r19} stop {s19} ready {t19r.hex()} devs {d19} own {n19} "
                f"wait {w19} {t19w.hex()} busy {t19b.hex()} back {b19} tail {t19.hex()}")

    # ---- 22. field 2026-11-11 (87579f5c): the opt-in late stop (1D 06 byte 15 bit 0) ----------
    # the PC (a2:..:13:23, A2DP 0/1, AVRCP 2 - re-paired, AVRCP back) plays and is selected; the phone (62:..:11:a1,
    # A2DP 3/4, AVRCP 5) paused.  1D 06 with 15 B (options 1) -> rc 0, 18 B reply, 1D 04 byte 116 bits 6-7 set.  1D 05 01
    # phone -> `SET 2 SELECT` + `AVRCP PAUSE`; the PC reports PAUSED and keeps its stream (Windows ~11 s): nothing at
    # 3.2 s, at 4.5 s after the PAUSE `SET 0 SELECT` + `A2DP STREAMING STOP`; its `STOP 0` -> `SET 5 SELECT` + `AVRCP
    # PLAY`, never a sink START.  Back: 1D 05 01 PC -> `SET 5 SELECT` + `AVRCP PAUSE`, the phone's own STOP at 3.2 s (no
    # STOP to it) -> `SET 2 SELECT` + `AVRCP PLAY` (the PC the sink stopped).  Options 0 -> byte 116 bit 7 clear.
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    ticks(60)
    module("LIST", "LIST 0")
    module("RING 0 a2:83:83:83:13:23 19 A2DP", "RING 1 a2:83:83:83:13:23 19 A2DP", "RING 2 a2:83:83:83:13:23 17 AVRCP",
           "RING 3 62:81:81:81:11:a1 19 A2DP", "RING 4 62:81:81:81:11:a1 19 A2DP", "RING 5 62:81:81:81:11:a1 17 AVRCP")
    ticks(120)
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING",
           "AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED", "A2DP STREAMING START 0")
    ticks(300)
    z7 = [0] * 7
    w22 = rig.raw_reply(2, rp.req(OP, 0x06, 2, 0x04, 15, 5, 0, *z7, 0, 2, 1, 45), OP)   # + wait 4.5 s (default 1.0 s)
    s22 = src_list(rig)
    uart.thr.clear()
    pc7 = list(rtlib.pack7(bytes.fromhex("a28383831323")))
    ph7 = list(rtlib.pack7(bytes.fromhex("6281818111a1")))
    rc22 = rbt.op_rc(rig, 0x05, 0x01, *ph7)
    ticks(60)
    p22 = nopdu(cmds())
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(40)
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(340)                                       # ~3.2 s after the PAUSE: Windows still holds its stream
    e22 = nopdu(cmds())
    ticks(260)                                       # past 4.5 s after the PAUSED report (counted from it)
    st22 = nopdu(cmds())
    module("A2DP STREAMING STOP 0")
    ticks(120)
    pl22 = nopdu(cmds())
    module("AVRCP 5 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING", "A2DP STREAMING START 3")
    ticks(60)
    module("AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING")
    ticks(200)
    t22 = (src_list(rig) or b"")[84:116]
    thr22a = bytes(uart.thr)
    uart.thr.clear()
    rc22b = rbt.op_rc(rig, 0x05, 0x01, *pc7)
    ticks(60)
    module("AVRCP 5 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(40)
    module("AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(300)
    module("A2DP STREAMING STOP 3")                  # the phone's own STOP, 3.2 s after the PAUSE
    ticks(120)
    b22 = nopdu(cmds())
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING", "A2DP STREAMING START 0")
    ticks(60)
    thr22b = bytes(uart.thr)
    w22b = rig.raw_reply(2, rp.req(OP, 0x06, 2, 0x04, 15, 5, 0, *z7, 0, 2, 0), OP)
    s22b = src_list(rig)
    rig.close()
    ok &= check("22 field 2026-11-11: 1D 06 options 1 + wait 45 (16 B) -> rc 0, 19 B reply with 1, 1D 04 byte 116 "
                "bits 6-7; 1D 05 01 phone -> `SET 2 SELECT` + `AVRCP PAUSE`; the PC reports PAUSED, keeps its stream: "
                "nothing at 3.2 s, `SET 0 SELECT` + `A2DP STREAMING STOP` after 4.5 s; its STOP 0 -> `SET 5 SELECT` + "
                "`AVRCP PLAY`, tail PLAYING; back to the PC -> `SET 5 SELECT` + `AVRCP PAUSE`, the phone's own STOP -> "
                "`SET 2 SELECT` + `AVRCP PLAY` (no STOP to the phone, no sink START); options 0 -> bit 7 clear",
                w22 is not None and w22[1] == 0 and len(w22) == 19 and w22[17] == 1 and w22[18] == 45
                and len(s22) == 120 and s22[116] & 0xC0 == 0xC0
                and rc22 == 0 and p22 == [b"SET 2 SELECT", b"AVRCP PAUSE"] and e22 == p22
                and st22 == p22 + [b"SET 0 SELECT", b"A2DP STREAMING STOP"]
                and pl22 == st22 + [b"SET 5 SELECT", b"AVRCP PLAY"]
                and b"STREAMING START" not in thr22a
                and len(t22) == 32 and t22[1] == 0 and t22[2] == 1
                and rc22b == 0 and b22 == [b"SET 5 SELECT", b"AVRCP PAUSE", b"SET 2 SELECT", b"AVRCP PLAY"]
                and b"STREAMING" not in thr22b
                and w22b is not None and w22b[1] == 0 and w22b[17] == 0 and len(s22b) == 120 and s22b[116] & 0xC0 == 0x40,
                f"w {w22 and w22.hex()} b116 {s22[116] if len(s22) == 120 else None} rc {rc22}/{rc22b} pause {p22} "
                f"early {e22} stop {st22} play {pl22} tail {t22.hex()} back {b22} thr {thr22b[-120:]!r} "
                f"w0 {w22b and w22b.hex()} b116 {s22b[116] if len(s22b) == 120 else None}")

    # ---- 23. the late stop's wait (1D 06 byte 16) + the late START result ---------
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    ticks(60)
    module("LIST", "LIST 0")
    module("RING 0 a2:83:83:83:13:23 19 A2DP", "RING 1 a2:83:83:83:13:23 19 A2DP", "RING 2 a2:83:83:83:13:23 17 AVRCP",
           "RING 3 62:81:81:81:11:a1 19 A2DP", "RING 4 62:81:81:81:11:a1 19 A2DP", "RING 5 62:81:81:81:11:a1 17 AVRCP")
    ticks(120)
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING",
           "AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED", "A2DP STREAMING START 0")
    ticks(300)
    z7 = [0] * 7
    pol = lambda *tail: rig.raw_reply(2, rp.req(OP, 0x06, 2, 0x04, 15, 5, 0, *z7, *tail), OP)
    w23 = pol(0, 2, 1, 10)                           # options 1, wait 1.0 s (17 B: sub + 16)
    s23 = src_list(rig)
    w23o = pol(0, 2, 1)                              # the old 16 B write: the wait stays
    w23x = pol(0, 2, 1, 101)                         # 10.1 s: refused, nothing changes
    uart.thr.clear()
    pc7 = list(rtlib.pack7(bytes.fromhex("a28383831323")))
    ph7 = list(rtlib.pack7(bytes.fromhex("6281818111a1")))
    rc23 = rbt.op_rc(rig, 0x05, 0x01, *ph7)
    ticks(60)
    p23 = nopdu(cmds())
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(40)
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(60)                                        # 100 ticks of 8 ms = 0.8 s after the report
    e23 = nopdu(cmds())
    n23 = 100
    while n23 < 250 and b"A2DP STREAMING STOP" not in nopdu(cmds()):
        ticks(1)
        n23 += 1
    st23 = nopdu(cmds())
    module("A2DP STREAMING STOP 0")
    ticks(60)
    pl23 = nopdu(cmds())
    ticks(175)                                       # the phone opens its stream 1.4 s after the PLAY (field D75 run 1)
    module("AVRCP 5 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING", "A2DP STREAMING START 3")
    ticks(40)
    t23 = (src_list(rig) or b"")[84:116]
    module("AVRCP 5 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED PLAYING")
    ticks(200)
    thr23a = bytes(uart.thr)
    w23b = pol(0, 2, 1, 5)                           # wait 0.5 s
    uart.thr.clear()
    rc23b = rbt.op_rc(rig, 0x05, 0x01, *pc7)
    ticks(30)
    module("AVRCP 5 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PAUSED")
    ticks(40)                                        # 0.32 s after the report
    e23b = nopdu(cmds())
    m23 = 40
    while m23 < 200 and b"A2DP STREAMING STOP" not in nopdu(cmds()):
        ticks(1)
        m23 += 1
    st23b = nopdu(cmds())
    module("A2DP STREAMING STOP 3")                  # the phone answers the sink's STOP
    ticks(60)
    b23 = nopdu(cmds())
    ticks(340)                                       # no START from the PC within 2.5 s of the PLAY
    t23r = (src_list(rig) or b"")[84:116]
    module("AVRCP 2 REGISTER_NOTIFICATION_RSP CHANGED PLAYBACK_STATUS_CHANGED PLAYING", "A2DP STREAMING START 0")
    ticks(40)
    t23p = (src_list(rig) or b"")[84:116]
    thr23b = bytes(uart.thr)
    w23z = pol(0, 2, 0, 0)
    s23z = src_list(rig)
    rig.close()
    ok &= check("23 the late stop's wait: 1D 06 options 1 + wait 10 (sub + 16 B) -> rc 0, 19 B reply, byte 18 = 10, 1D 04 byte 119 = "
                "10; the old write without the wait keeps 10, wait 101 rc 1; 1D 05 01 phone -> `SET 2 SELECT` + `AVRCP "
                "PAUSE`; the PC reports PAUSED and holds: nothing 0.8 s after the report, `SET 0 SELECT` + `A2DP STREAMING "
                "STOP` 1.0-1.45 s after it (the wait, then the queued re-registration and the SET / STOP gap); STOP 0 -> `SET 5 SELECT` + `AVRCP PLAY`; the phone's START 1.4 s after the PLAY -> "
                "PLAYING; wait 5: 1D 05 01 PC -> `SET 5 SELECT` + `AVRCP PAUSE`, the phone reports PAUSED and holds -> "
                "`SET 3 SELECT` + `A2DP STREAMING STOP` 0.5-0.95 s after it; STOP 3 -> `SET 2 SELECT` + `AVRCP PLAY`; no START "
                "-> READY at 2.5 s, its START then -> PLAYING; never a sink START; options 0 wait 0 -> byte 119 0",
                w23 is not None and w23[1] == 0 and len(w23) == 19 and w23[17] == 1 and w23[18] == 10
                and len(s23) == 120 and s23[119] == 10 and s23[116] & 0xC0 == 0xC0
                and w23o is not None and w23o[1] == 0 and w23o[18] == 10
                and w23x is not None and w23x[1] == 1 and w23x[18] == 10
                and rc23 == 0 and p23 == [b"SET 2 SELECT", b"AVRCP PAUSE"] and e23 == p23
                and st23 == p23 + [b"SET 0 SELECT", b"A2DP STREAMING STOP"] and 125 <= n23 <= 180
                and pl23 == st23 + [b"SET 5 SELECT", b"AVRCP PLAY"]
                and len(t23) == 32 and t23[1] == 0 and t23[2] == 1
                and b"STREAMING START" not in thr23a
                and w23b is not None and w23b[1] == 0 and w23b[18] == 5
                and rc23b == 0 and e23b[:2] == [b"SET 5 SELECT", b"AVRCP PAUSE"] and b"A2DP STREAMING STOP" not in e23b
                and st23b == [b"SET 5 SELECT", b"AVRCP PAUSE", b"SET 3 SELECT", b"A2DP STREAMING STOP"] and 62 <= m23 <= 120
                and b23 == st23b + [b"SET 2 SELECT", b"AVRCP PLAY"]
                and len(t23r) == 32 and t23r[1] == 0 and t23r[2] == 2
                and len(t23p) == 32 and t23p[1] == 0 and t23p[2] == 1
                and b"STREAMING START" not in thr23b
                and w23z is not None and w23z[1] == 0 and w23z[17] == 0 and w23z[18] == 0 and len(s23z) == 120
                and s23z[119] == 0 and s23z[116] & 0xC0 == 0x40,
                f"w {w23 and w23.hex()} old {w23o and w23o.hex()} bad {w23x and w23x.hex()} b119 "
                f"{s23[119] if len(s23) == 120 else None} rc {rc23}/{rc23b} pause {p23} early {e23} stop {st23} at "
                f"{n23} ticks play {pl23} tail {t23.hex()} | w5 {w23b and w23b.hex()} early {e23b} stop {st23b} at "
                f"{m23} ticks back {b23} ready {t23r.hex()} late {t23p.hex()} | w0 {w23z and w23z.hex()}")

    # ---- 24. live apply over two connected sources: the handover ----------------------------------------
    ok &= check_live_apply(image, syms, data)

    # ---- 25. the decided module values kept at power-on ----------------------------------------------------
    ok &= check_pin(image, syms, data)

    # ---- 26. standby that switched the module off, then the wake: the reconnect choice as at power-on ----
    ok &= check_standby_wake(image, syms, data)

    # ---- 27. a live apply with no handover block: the pin writes, never restarts -------------
    ok &= check_pin_live(image, syms, data)

    return rtlib.finish(ok, " - emulation only: interrupts and task switches are modelled at named points; no caches.")


HO_BLOCK = 0x4019FFC0   # ota_payload.ld bt_handover
STOCK_BT_POWER = {"bt_off": 0x400AD030, "bt_on": 0x400AD098, "bt_flags_clear": 0x40146810}
HO_LIST = ["LIST 6",
           "LIST 0 CONNECTED A2DP 1024 0 0 24 0 0 22:82:02:02:92:22 19 INCOMING ACTIVE SLAVE ENCRYPTED 0",
           "LIST 1 CONNECTED A2DP 1024 0 0 24 0 0 22:82:02:02:92:22 19 INCOMING ACTIVE SLAVE ENCRYPTED 0",
           "LIST 2 CONNECTED AVRCP 512 0 0 21 0 0 22:82:02:02:92:22 17 INCOMING ACTIVE SLAVE ENCRYPTED 0",
           "LIST 3 CONNECTED A2DP 1024 0 0 24 0 0 62:81:81:81:11:a1 19 INCOMING ACTIVE SLAVE ENCRYPTED 0",
           "LIST 4 CONNECTED A2DP 1024 0 0 24 0 0 62:81:81:81:11:a1 19 INCOMING ACTIVE SLAVE ENCRYPTED 0",
           "LIST 5 CONNECTED AVRCP 512 0 0 21 0 0 62:81:81:81:11:a1 17 INCOMING ACTIVE SLAVE ENCRYPTED 0"]


def live_apply_case(image, syms, data, kept):
    """One rig: the payload from the built-in, two sources, the phone selected and streaming, then the same payload
    applied live to slot B.  Returns a dict of what was seen."""
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    asm = rpw.BT_ASSEMBLER
    calls = {}
    for name, a in STOCK_BT_POWER.items():
        emu.hook(a, (lambda n: lambda e: (calls.__setitem__(n, calls.get(n, 0) + 1), rt.se.StockEmu.CONTINUE)[1])(name))

    def module(*lines):
        for ln in lines:
            rig.bt_line(ln)
            emu.call(asm, max_insns=2_000_000)

    def ticks(n):
        for _ in range(n):
            rig.tick()
            uart.finish()

    r = {"first_block": bytes(emu.read_bytes(HO_BLOCK, 4))}
    ticks(60)
    module("LIST", "LIST 0")
    # the user's setting: reconnect off (1D 06 byte 14 = 0, no preferred device) - on NOR, read back after the apply
    r["pol_w"] = rig.raw_reply(2, rp.req(OP, 0x06, 2, 0x04, 15, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), OP)
    ticks(5)
    module("RING 0 22:82:02:02:92:22 19 A2DP", "RING 1 22:82:02:02:92:22 19 A2DP", "RING 2 22:82:02:02:92:22 17 AVRCP",
           "RING 3 62:81:81:81:11:a1 19 A2DP", "RING 4 62:81:81:81:11:a1 19 A2DP", "RING 5 62:81:81:81:11:a1 17 AVRCP",
           "A2DP STREAMING START 3")
    ticks(400)
    r["before"] = src_devs(src_list(rig))
    uart.thr.clear()
    calls.clear()
    pn = rig.pn
    r["upload"] = pn.upload(1, data, seq=2)
    r["job"] = pn.run_job()[0]["job"]
    r["apply_rc"] = rbt.rh.apply(pn, rbt.rh.T_B)
    st, _n = rbt.rh.run_apply(pn)
    uart.finish()
    r["ap_state"] = st["ap_state"]
    r["during"] = bytes(uart.thr)
    r["power_calls"] = dict(calls)
    r["block_after"] = bytes(emu.read_bytes(HO_BLOCK, 52))
    uart.thr.clear()
    ticks(60)
    module(*(HO_LIST if kept else ["LIST 0"]))
    ticks(400)
    r["early"] = bytes(uart.thr)
    if not kept:
        module("CALL 0", "CONNECT 0 A2DP 19", "CONNECT 1 A2DP 19", "RING 2 62:81:81:81:11:a1 17 AVRCP")
        ticks(300)
        r["phone_back"] = bytes(uart.thr)
        module("CALL 3", "CONNECT 3 A2DP 19", "CONNECT 4 A2DP 19", "RING 5 22:82:02:02:92:22 17 AVRCP")
    ticks(2000)
    r["sent"] = bytes(uart.thr)
    r["power_calls_after"] = dict(calls)
    r["pol"] = rig.raw_reply(2, rp.req(OP, 0x06), OP)
    r["after"] = src_devs(src_list(rig))
    rig.close()
    return r


def check_live_apply(image, syms, data):
    PHONE, PC = "6281818111a1", "228202029222"
    a = live_apply_case(image, syms, data, kept=True)
    b = live_apply_case(image, syms, data, kept=False)

    def common(r):
        return (r["first_block"] == b"\0\0\0\0" and r["upload"] == ("commit", 0) and r["apply_rc"] == 0
                and r["ap_state"] == 5 and r["during"] == b"" and not r["power_calls"] and not r["power_calls_after"]
                and r["block_after"] == bytes(52)
                and r["before"].get(PHONE, (0, 0, 0))[2] & 0x01 and r["before"].get(PC, (0, 0, 0))[0] & 0x80
                and b"CLOSE" not in r["sent"] and b"KILL" not in r["sent"] and b"RESET" not in r["sent"]
                and b"STREAMING" not in r["sent"]
                and r["pol_w"] is not None and r["pol_w"][1] == 0 and r["pol"] is not None and r["pol"][15] == 0
                and r["pol"][6] == 0)

    cmds = [c for c in a["sent"].split(b"\r\n") if c]
    ok_a = (common(a) and all(c in (b"LIST", b"SET", b"AVRCP PDU 31 1") or (c.startswith(b"SET ") and c.endswith(b" SELECT"))
                              for c in cmds)
            and b"CALL" not in a["sent"] and a["after"].get(PHONE, (0, 0, 0))[2] & 0x01
            and a["after"][PHONE][0] & 0x01 and not a["after"].get(PC, (0, 0, 0))[2] & 0x01 and a["after"][PC][1] == 0x07)
    calls_b = [c for c in b["sent"].split(b"\r\n") if c.startswith(b"CALL")]
    ok_b = (common(b) and calls_b == [b"CALL 62:81:81:81:11:a1 19 A2DP", b"CALL 22:82:02:02:92:22 19 A2DP"]
            and b"CALL 22:82" not in b["early"] and b"CALL 62:81" in b["early"]
            and b"CALL 22:82:02:02:92:22 19 A2DP" in b["phone_back"]
            and b["after"].get(PHONE, (0, 0, 0))[2] & 0x01 and b["after"].get(PHONE, (0, 0, 0))[1]
            and b["after"].get(PC, (0, 0, 0))[1] and not b["after"][PC][2] & 0x01)
    return check("24 live apply over two connected sources: from the built-in no block; PC + phone "
                 "connected, the phone selected and streaming; the same payload to slot B, applied live: nothing on "
                 "UART1 during the apply, no stock module power routine, the block taken and cleared; a - links kept: "
                 "only LIST / registrations / `SET`, the phone still selected and streaming, no CALL / CLOSE; b - links "
                 "dropped (LIST 0, reconnect off): `CALL <phone> 19 A2DP` then `CALL <PC> 19 A2DP` once the phone is "
                 "back, both connected, the phone selected, no CLOSE",
                 ok_a and ok_b,
                 f"a: during {a['during']!r} power {a['power_calls']}/{a['power_calls_after']} block {a['block_after'][:4].hex()} "
                 f"sent {a['sent']!r} after {a['after']} pol {a['pol'] and a['pol'].hex()} | b: sent {b['sent']!r} after {b['after']}")


PIN_DECIDED = ("SET BT CLASS 240414", "SET PROFILE AVRCP CONTROLLER 0f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT")
PIN_FACTORY = ("SET BT CLASS 001f00", "SET PROFILE AVRCP CONTROLLER 1f", "SET CONTROL AUDIO INTERNAL I2S_SLAVE KEEPALIVE")


def pin_case(image, syms, data, factory, live=False):
    """One rig: the payload from the built-in (a power-on of the BT port), no source; the module answers the manager's
    `SET` with its listing - the factory lines (factory) or the decided ones.  The module model
    takes every setting write; the stock module power routines run for real and are counted.  live: that payload keeps the decided values, then the same payload is applied live to slot B with NO
    handover block (btc_handover skipped - as from the built-in / a payload before §25 / a stale block) while the app
    holds the binding, and the module lists the factory lines to the new payload's power-on read"""
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    asm = rpw.BT_ASSEMBLER
    calls = {}
    for name, a in STOCK_BT_POWER.items():
        emu.hook(a, (lambda n: lambda e: (calls.__setitem__(n, calls.get(n, 0) + 1), rt.se.StockEmu.CONTINUE)[1])(name))
    now = list(PIN_FACTORY if factory else PIN_DECIDED)

    def module(*lines):
        for ln in lines:
            rig.bt_line(ln)
            emu.call(asm, max_insns=2_000_000)

    def ticks(n):
        for _ in range(n):
            rig.tick()
            uart.finish()

    def listing():
        # one engine pass per line - the UART pace (see 21's ulisting: a long listing fed at once across the RX ring end
        # reads as a stock ring reset)
        for ln in ("SET", "SET BT BDADDR 02:07:80:aa:bb:cc", "SET BT NAME CA79 Audio", now[0], "SET BT PAGEMODE 3 2000 1",
                   "SET BT PAIR 62:81:81:81:11:a1 00112233445566778899aabbccddeeff", "SET CONTROL AUTOCALL 19 5000 A2DP",
                   now[2], "SET PROFILE A2DP SINK", now[1]):
            module(ln)
            ticks(1)
        module("SET")                                   # the end: what follows is counted from here by the caller

    def until(tail, limit):
        """ticks until the THR ends with tail (the module answers what came before); the ticks taken, None if not"""
        for i in range(limit):
            if bytes(uart.thr).endswith(tail):
                return i
            ticks(1)
        return None

    r = {"steps": []}
    if live:
        ticks(60)
        module("LIST", "LIST 0")
        r["old_read_at"] = until(b"SET\r\n", 600)
        now = list(PIN_DECIDED)
        listing()                                       # the old payload: the decided values, kept
        ticks(60)
        ho = rtlib.payload("ota_main")[5]["btc_handover"]
        emu.hook(ho, lambda e: None)                    # the old payload hands nothing over
        pn = rig.pn
        r["upload"] = pn.upload(1, data, seq=2)
        pn.run_job()
        r["apply_rc"] = rbt.rh.apply(pn, rbt.rh.T_B)
        r["ap_state"] = rbt.rh.run_apply(pn)[0]["ap_state"]
        emu.unhook(ho)
        uart.finish()
        r["block_after"] = bytes(emu.read_bytes(HO_BLOCK, 4))
        r["binding"] = emu.read_bytes(BINDING, 1)[0]
        now = list(PIN_FACTORY if factory else PIN_DECIDED)
        uart.thr.clear()
    ticks(60)
    r["power_pre"] = dict(calls)
    module("LIST", "LIST 0")
    uart.thr.clear()
    r["first_read_at"] = until(b"SET\r\n", 600)        # BTS_SET_READ_MS after module on, no source needed
    r["first"] = bytes(uart.thr)
    listing()
    for _ in range(4):                                  # each write the module takes: the op's `SET` read back
        mark = len(uart.thr)
        ticks(60)
        cmds = [c for c in bytes(uart.thr)[mark:].split(b"\r\n") if c]
        if len(cmds) != 2 or cmds[1] != b"SET":
            r["steps"].append(cmds)
            break
        w = cmds[0].decode()
        for k, pfx in enumerate(("SET BT CLASS ", "SET PROFILE AVRCP CONTROLLER ", "SET CONTROL AUDIO ")):
            if w.startswith(pfx):
                now[k] = w
        r["steps"].append(cmds)
        listing()
    mark = len(uart.thr)
    r["bt_power_low"] = False
    for _ in range(150):                                # the restart (if any): stock off, 0.6 s, on
        ticks(1)
        r["bt_power_low"] |= emu.read_u32(rpw.BT_POWER) == 0
    r["power"] = dict(calls)
    if calls.get("bt_on"):
        r["list_at"] = until(b"LIST\r\n", 200)          # the module is on again: the port learns its links
        module("LIST", "LIST 0")
        r["back_read_at"] = until(b"SET\r\n", 500)      # ... and the manager reads the listing back
        listing()
    ticks(2500)                                         # 20 s more: nothing else, once per power-on
    r["after"] = [c for c in bytes(uart.thr)[mark:].split(b"\r\n") if c]
    r["power_after"] = dict(calls)
    r["avf"] = rig.raw_reply(2, rp.req(OP, 0x0C, 0), OP)
    r["aud"] = rig.raw_reply(2, rp.req(OP, 0x0D, 0), OP)
    r["cod"] = rig.raw_reply(2, rp.req(OP, 0x0A, 0), OP)
    raw = src_list(rig)
    r["health"] = raw[102] if len(raw) > 102 else None
    r["now"] = list(now)
    rig.close()
    return r


def check_pin_live(image, syms, data):
    lv = pin_case(image, syms, data, factory=True, live=True)
    ok = (lv.get("old_read_at") is not None and lv["upload"] == ("commit", 0) and lv["apply_rc"] == 0
          and lv["ap_state"] == 5 and lv["block_after"] == bytes(4) and lv["binding"] == 3
          and lv["first"] == b"SET\r\n"
          and lv["steps"] == [[b"SET BT CLASS 240414", b"SET"], [b"SET PROFILE AVRCP CONTROLLER 0f", b"SET"],
                              [b"SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT", b"SET"], []]
          and not lv["power_pre"] and not lv["power"] and not lv["power_after"] and not lv["bt_power_low"]
          and lv["after"] == [] and lv["now"] == list(PIN_DECIDED))
    return check("27 a live apply with no handover block: the payload keeps the decided values; "
                 "the same payload applied live to slot B with btc_handover skipped (as from the built-in / a payload "
                 "before §25 / a stale block), the app's binding held; the module lists the factory lines -> the three "
                 "lines written (each + `SET`), NO stock module power routine (FUN_400AD030 / FUN_400AD098), the power "
                 "word never 0, no LIST / `SET` after - the links are not dropped (before: one restart)",
                 ok,
                 f"old read {lv.get('old_read_at')} upload {lv['upload']} apply {lv['apply_rc']}/{lv['ap_state']} block "
                 f"{lv['block_after'].hex()} binding {lv['binding']} first {lv['first']!r} steps {lv['steps']} power "
                 f"{lv['power_pre']}/{lv['power']}/{lv['power_after']} low {lv['bt_power_low']} after {lv['after']}")


def check_pin(image, syms, data):
    d = pin_case(image, syms, data, factory=False)
    f = pin_case(image, syms, data, factory=True)
    ok_d = (d["first"] == b"SET\r\n" and d["first_read_at"] is not None and d["steps"] == [[]]
            and not d["power"] and not d["power_after"] and not d["bt_power_low"] and d["after"] == []
            and d["health"] is not None and not d["health"] & 0x04
            and d["avf"] is not None and d["avf"][3] == 1 and (d["avf"][7] << 7 | d["avf"][8]) == 0x0F
            and d["aud"] is not None and d["aud"][3] == 1 and d["aud"][7] == 1)
    ok_f = (f["first"] == b"SET\r\n"
            and f["steps"] == [[b"SET BT CLASS 240414", b"SET"], [b"SET PROFILE AVRCP CONTROLLER 0f", b"SET"],
                               [b"SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT", b"SET"], []]
            and not f["power_pre"] and f["power"].get("bt_off") == 1 and f["power"].get("bt_on") == 1
            and f["bt_power_low"] and f.get("list_at") is not None and f.get("back_read_at") is not None
            and f["after"] == [b"LIST", b"SET"] and f["power_after"] == f["power"]
            and f["now"] == list(PIN_DECIDED) and f["health"] is not None and not f["health"] & 0x04
            and f["avf"] is not None and f["avf"][5] == 0 and (f["avf"][7] << 7 | f["avf"][8]) == 0x0F
            and (f["avf"][9] << 7 | f["avf"][10]) == 0x1F
            and f["aud"] is not None and f["aud"][5] == 0 and f["aud"][7] == 1 and f["aud"][8] == 2
            and f["cod"] is not None and f["cod"][5] == 0)
    return check("25 the decided module values at power-on: the decided listing -> the one "
                 "`SET` read only, no write, no stock module power routine, health bit 0x04 clear; the factory listing -> "
                 "`SET BT CLASS 240414` / `SET PROFILE AVRCP CONTROLLER 0f` / `SET CONTROL AUDIO INTERNAL I2S_SLAVE EVENT`, "
                 "each + its `SET` read back, then ONE restart through the real stock routines (FUN_400AD030 once, the "
                 "power word 0, FUN_400AD098 once), LIST + the listing read back, nothing more in 20 s; 1D 0A / 0C / 0D "
                 "rc 0 with the restore values the module's own (1f, KEEPALIVE)",
                 ok_d and ok_f,
                 f"decided: first {d['first']!r} at {d['first_read_at']} steps {d['steps']} power {d['power']}/"
                 f"{d['power_after']} after {d['after']} health {d['health']} | factory: steps {f['steps']} power "
                 f"{f['power_pre']}/{f['power']}/{f['power_after']} low {f['bt_power_low']} list {f.get('list_at')} "
                 f"read {f.get('back_read_at')} after {f['after']} now {f['now']} health {f['health']} avf "
                 f"{f['avf'] and f['avf'].hex()} aud {f['aud'] and f['aud'].hex()} cod {f['cod'] and f['cod'].hex()}")


PHONE_ADDR = bytes.fromhex("6281818111a1")
CALL_PHONE = b"CALL 62:81:81:81:11:a1 19 A2DP"


def standby_wake_case(image, syms, data, path, recon, bt_off=True, autocall=False):
    """One rig: the payload from the built-in, the power policy's 「대기 중 BT 오디오 켜 두기」 off (bt_off) or on, the
    reconnect choice recon, nothing preferred; the phone connects, streams and is picked (1D 05 01 - the "last selected"
    device); the power-on listing read answered (AUTOCALL as given).  Standby by path ("enter" 19 01 = 「지금 대기」,
    "remote" 19 03, "apo" the real 20 ms site at the menu limit), 10 virtual minutes, wake 19 02.  The stock module power
    routines run for real and are counted."""
    rig, uart = fresh_rig(image, syms, data)
    emu = rig.emu
    asm = rpw.BT_ASSEMBLER
    calls = {}
    for name, a in STOCK_BT_POWER.items():
        emu.hook(a, (lambda n: lambda e: (calls.__setitem__(n, calls.get(n, 0) + 1), rt.se.StockEmu.CONTINUE)[1])(name))

    def module(*lines):
        for ln in lines:
            rig.bt_line(ln)
            emu.call(asm, max_insns=2_000_000)

    def ticks(n):
        for _ in range(n):
            rig.tick()
            uart.finish()

    def until(tail, limit):
        for i in range(limit):
            if bytes(uart.thr).endswith(tail):
                return i
            ticks(1)
        return None

    r = {}
    ticks(60)
    module("LIST", "LIST 0")
    r["pw"] = rig.raw_reply(2, rp.req(rpw.OP_POLICY, 0x01, *rpw.pack7(rpw.policy_raw(bt_off=bt_off))), rpw.OP_POLICY)
    r["pol"] = rig.raw_reply(2, rp.req(OP, 0x06, 2, 0x04, 15, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, recon), OP)
    module("RING 0 62:81:81:81:11:a1 19 A2DP", "RING 1 62:81:81:81:11:a1 19 A2DP", "RING 2 62:81:81:81:11:a1 17 AVRCP",
           "A2DP STREAMING START 0")
    ticks(60)
    r["use"] = rbt.op_rc(rig, 0x05, 0x01, *rtlib.pack7(PHONE_ADDR))
    uart.thr.clear()
    r["read_at"] = until(b"SET\r\n", 600)           # the power-on listing read
    for ln in ("SET", "SET BT BDADDR 02:07:80:aa:bb:cc", "SET BT NAME CA79 Audio", PIN_DECIDED[0],
               "SET BT PAIR 62:81:81:81:11:a1 00112233445566778899aabbccddeeff",
               *(["SET CONTROL AUTOCALL 19 5000 A2DP"] if autocall else []), PIN_DECIDED[2], "SET PROFILE A2DP SINK",
               PIN_DECIDED[1], "SET"):
        module(ln)
        ticks(1)
    ticks(200)
    r["before"] = src_devs(src_list(rig))
    r["calls_on"] = b"CALL" in bytes(uart.thr)
    calls.clear()
    if path == "enter":
        r["rc"] = rig.raw_reply(2, rp.req(rpw.OP_STANDBY, 0x01), rpw.OP_STANDBY)
    elif path == "remote":
        r["rc"] = rig.raw_reply(2, rp.req(rpw.OP_STANDBY, 0x03), rpw.OP_STANDBY)
    else:
        module("A2DP STREAMING STOP 0")             # a stream holds the APO
        ticks(60)
        rig.site(0x400AA874)                         # the hold's release: the menu time counts from here
        r["rc"] = rpw.expire(rig)
    ticks(2)
    d = rig.diag()
    r["sb_state"] = d and d["state"]
    r["power_low"] = emu.read_u32(rpw.BT_POWER) == 0
    r["sb_calls"] = dict(calls)
    rig.tick(1, ms=10 * 60 * 1000)                   # 10 min in standby
    ticks(5)
    uart.thr.clear()
    r["wake"] = rig.raw_reply(2, rp.req(rpw.OP_STANDBY, 0x02), rpw.OP_STANDBY)
    on_at = None
    for i in range(100):
        ticks(1)
        if emu.read_u32(rpw.BT_POWER) == 1 and on_at is None:
            on_at = i
            break
    r["on_at"] = on_at
    r["wake_calls"] = dict(calls)
    r["list_at"] = until(b"LIST\r\n", 100) if r["power_low"] else None
    if r["list_at"] is not None:
        module("LIST", "LIST 0")
    mark = len(uart.thr)
    r["call_at"] = None
    for i in range(1500):                            # 12 s after the module start
        ticks(1)
        if r["call_at"] is None and b"CALL" in bytes(uart.thr)[mark:]:
            r["call_at"] = i + (r["list_at"] or 0)
    r["after"] = bytes(uart.thr)
    r["state"] = rig.diag()["state"]
    r["links"] = src_devs(src_list(rig))
    rig.close()
    return r


def check_standby_wake(image, syms, data):
    ok = True
    ph = PHONE_ADDR.hex()
    out = []
    for path in ("enter", "remote", "apo"):
        c = standby_wake_case(image, syms, data, path, recon=2)
        good = (c["pw"] is not None and c["pw"][1] == 0 and c["pol"] is not None and c["pol"][1] == 0
                and c["pol"][15] == 2 and c["use"] == 0 and c["read_at"] is not None and not c["calls_on"]
                and c["before"].get(ph, (0, 0, 0))[2] & 0x01
                and c["sb_state"] == rpw.SB["standby"] and c["power_low"] and c["sb_calls"].get("bt_off") == 1
                and c["wake"] is not None and c["wake"][1] == 0 and c["on_at"] is not None
                and c["wake_calls"].get("bt_on") == 1 and c["wake_calls"].get("bt_flags_clear") == 1
                and c["list_at"] is not None and c["call_at"] is not None and 495 <= c["call_at"] <= 515
                and [x for x in c["after"].split(b"\r\n") if x.startswith(b"CALL")] == [CALL_PHONE]
                and b"CLOSE" not in c["after"] and c["state"] == rpw.SB["on"])
        ok &= good
        out.append(f"{path}: sb {c['sb_state']} low {c['power_low']} {c['sb_calls']} wake {c['wake_calls']} on "
                   f"{c['on_at']} list {c['list_at']} call at {c['call_at']} sent {c['after']!r}")
    off = standby_wake_case(image, syms, data, "enter", recon=0)
    good_off = (off["pol"] is not None and off["pol"][15] == 0 and off["power_low"] and off["wake_calls"].get("bt_on") == 1
                and off["list_at"] is not None and b"CALL" not in off["after"])
    ac = standby_wake_case(image, syms, data, "remote", recon=2, autocall=True)
    good_ac = ac["power_low"] and ac["wake_calls"].get("bt_on") == 1 and b"CALL" not in ac["after"]
    kept = standby_wake_case(image, syms, data, "enter", recon=2, bt_off=False)
    good_kept = (kept["sb_state"] == rpw.SB["standby"] and not kept["power_low"] and not kept["sb_calls"]
                 and not kept["wake_calls"] and kept["list_at"] is None and b"CALL" not in kept["after"]
                 and b"LIST" not in kept["after"] and kept["links"].get(ph, (0, 0, 0))[1])
    return check("26 standby with 「대기 중 BT 오디오 켜 두기」 off, then the wake (user decision 2026-11-14): the "
                 "phone connected, streaming and picked (1D 05 01), reconnect 「선호 · 마지막」, nothing preferred, AUTOCALL "
                 "read off; standby by 19 01 / 19 03 / the APO (the real 20 ms site) -> the real FUN_400AD030 once (power "
                 "word 0); 10 min; 19 02 -> FUN_400AD098 + FUN_40146810 once each, LIST, `CALL <phone> 19 A2DP` 4 s after "
                 "the module start (as at power-on) and nothing else; reconnect off -> no CALL; AUTOCALL on -> no CALL; "
                 "the module kept on (TV wake) -> no stock power routine, no LIST, no CALL, the phone's links kept",
                 ok and good_off and good_ac and good_kept,
                 " | ".join(out) + f" | off: {off['after']!r} | autocall: {ac['after']!r} | kept: calls "
                 f"{kept['sb_calls']}/{kept['wake_calls']} sent {kept['after']!r} links {kept['links']}")


def src_list(rig):
    r = rig.raw_reply(2, rp.req(OP, 0x04), OP)
    raw = rp.unpack7(r[2:]) if r and r[1] == 0 else b""
    return raw if len(raw) >= 102 else b""


def src_devs(raw):
    """1D 04 layout 2: {addr hex: (state, links, state2)} for the used slots (14 B per device from 28)."""
    return {bytes(raw[29 + 14 * i: 35 + 14 * i]).hex(): (raw[28 + 14 * i], le(raw, 35 + 14 * i, 2), raw[40 + 14 * i])
            for i in range(4) if raw and raw[28 + 14 * i] & 0x80}


if __name__ == "__main__":
    sys.exit(main(sys.argv))
