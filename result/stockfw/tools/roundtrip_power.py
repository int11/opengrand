# roundtrip_power.py - emulator verification of the power port of the slot payload ota_main on ota_loader_v4
# (power policy, partial standby, BT audio APO rule).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_power.py [ota_main] [-q] [--loader=ota_loader_v4]
#   (--loader: the resident the payload runs on, default ota_loader_v4)
#
# The power port with TV wake: the BT audio module kept on in standby (check 4), plus check 11 (STREAMING START in the
# UART1 ring wakes, the UART1 0x80 latch is cleared through the real FUN_40145C00, a stock ring reset is never re-read).
# (The earlier power-only slot payloads ota_power / ota_power2 / ota_power3 were checked here too until they were
# removed from the repository.)
# ota_main = every feature: the same checks with the 0x40147E94
# stub hk_sx_kawai (the panel-touch peek, then the Phase 3-1 SysEx front - roundtrip_sysex.py), the session
# core's fifth hook 0x401539AC (roundtrip_session.py) and the BT port's stock calls (roundtrip_bt.py); identify
# answers feat2 0x03 as a sixth byte.  (The per-feature payloads ota_sysex / ota_bt / ota_session were checked
# here too until they were discarded.)
#
# Same machine as roundtrip_ota_hooks.py: the real ota_loader_v4 image on StockEmu with the NOR model
# (allowed = the update slots + unused_3 0x4C0000-0x4FFFFF; anything else raises), SWI #0 emulated so
# the resident's real poke body writes the hook words, the payload uploaded over the 0x7D OTA ops and
# applied live.  The stock routines the power port binds to run for real (amp / mute on OUT_DATA67,
# BT module power, headphone sample, NOR erase / program / copier, the 20 ms APO body FUN_400ABFE8,
# the headphone poll, the executor, the Kawai SysEx end handler); stubbed are only the stock
# cleanup FUN_400B0EA8 and the DSP-path halves FUN_4013BC90 / FUN_4013BD94 (recorded calls).
# Checks (numbered as printed):
#   1  static: the payload verifies; attach flag; every stock binding of stock_power.ld at its
#      documented address and doing what the analysis says when run (amp / mute bits, BT power, the
#      headphone sample, the flag clear); the four stubs end in `b` = the stock target of their site;
#      the kernel-timer and task-19 bodies are leaves; branches out of the payload only to the allowed
#      stock set; the NOR writers only from the two guarded wrappers; NOR literals only in unused_3
#   2  applied live: attach() hooks exactly the four sites (0x401539AC stays stock), op 0x16 / identify
#   3  op 0x18: replies byte-equal to the host core; unbound set refused; bound set -> the kvstore in
#      unused_3 (blank: no erase; every NOR op inside unused_3 with dispatch disabled); stack depth of
#      that tick; reboot on the same NOR -> the saved policy is back
#   4  APO through the real 20 ms site: the expiry tick does not write the stock state byte 2; the
#      engine loop enters standby (amp off, muted, BT audio off, notice 59 7F 00 01 to the app port);
#      the same tick on stock words writes 2 (control)
#   5  BT streaming (lines in the stock UART1 ring) holds the stock counter at 1 through the site (frozen payloads); ota_main
#      the counter runs and rests at the limit, the hold only stops the expiry; STOP resets it to 1
#   6  standby: the headphone-poll site skips the poll, the executor site drops the GP3[8] event
#      (control: the stock executor re-lights the amp), a panel touch through the real Kawai end
#      handler wakes (the stock handler still runs with its own registers), 100 ms later amp + unmute
#   7  the standby cap -> state byte 2 (the stock executor's shutdown request), also from the timer site
#   8  stores: every store made by payload code lands in its .data, the core RW, the stack or the three
#      stock globals of the analysis (counter, state byte, GP3[8] event)
#   9  apply-now to the built-in while in standby: woken first, then every site word stock
#  10  (dropped with the first loader: the payload on a loader without the hook service)
#  11  TV wake: standby never enters FUN_400AD030; CONNECT in the ring does not wake, STREAMING
#      START does (cause 5) and the amp comes back; a latched UART1 flag (control: the real assembler
#      FUN_40145C44 leaves it latched) is cleared by the payload through the real FUN_40145C00 and the
#      stock assembler reads lines again; after a ring reset by the real FUN_40145C00 the old START of
#      the previous lap is not read again; payload stores still only the analysed set
#  12  policy cap 0xFFFF = no cap: ota_main
#      saves it, answers it after a power cycle and stays in standby for 7 virtual days (the timer site holds
#      the stock counter, never state byte 2), then 19 02 wakes
#  13  (ota_main, feat2 0x20) feature 1: an A2DP stream holds the APO through
#      the real 20 ms site for 100 virtual minutes until STOP (a STOP of another link does not end it); a stream left
#      open (TV / Windows paused) holds only up to the policy's max hold (30 min here): an idle piano goes to standby
#      at the cap right away (owner decision 2026-11-02); activity right before the cap -> still ON at
#      the cap, the normal APO from that activity.  Feature 2: 19 03 from the app while the PANEL holds the stock binding (the session core keeps the
#      app bound) -> standby the way the APO goes; from an unbound port rc 2; refused (rc 6) while the stock recorder
#      state 0x4191EF79 is 3..6 (reason 1), a file operation 0x413CA3EA (2) or a RecorderData job 0x4139DAB4 (3) - no
#      cleanup, still ON; policy "power off": 19 03 -> the stock executor's request (state byte 2).  The standby / awake
#      notices 59 7F 00 <state> reach the core-bound app (port 2) while the panel holds the stock binding, never the
#      panel
#  14  (ota_main) polls are not activity: each frame through the REAL byte
#      parser FUN_40148B24 resets the stock counter; at the pump tail 0x401539AC (hk_ss_midi -> pwc_midi_tail) the polls /
#      reads (1B 03, 1D 04, 1A, 1D 0F, 19 00, Kawai fn 02 / 07, CONNECT) get it back, a fn 10 set / the panel touch / 19 02 /
#      a note-on keep the reset; an app polling every few seconds -> APO 15 min still expires -> standby; a real set resets;
#      the BT stream hold with polls holds (the counter keeps counting - the polls are undone - and rests at the limit),
#      after STOP the APO runs out at the menu time
#  15  (ota_main) APO test shortcut 19 04 <s_hi7> <s_lo7> through the real parser:
#      the 20 ms site puts the stock counter at menu limit - s x 50; with an app polling 1D 04 every 2 s the real site
#      expires at s = 60 -> standby; a fn 10 set during the countdown gives the full menu time back; a BT stream holds it
#      (a 19 04 while held moves the counter, the hold still stops the expiry), after STOP the menu time; APO Off -> rc 7; of the stock globals only the counter is stored, no NOR op
#  15b (ota_main) 19 04 answered and moved inside its own pump pass, a poll
#      later in that pass resets the counter: the tail keeps the move (standby at 60 s); a set in that place still wins
#  16  (ota_main, owner decision 2026-11-03) the hold follows the streaming
#      source's AVRCP play status (module lines in the UART1 ring, read by the BT port's source manager): a Windows-like PC
#      (A2DP 3 / 5, AVRCP 4) streams and reports PLAYING -> held through the real 20 ms site; CHANGED PAUSED with the
#      stream left open -> the hold ends like a STOP (counter 1 at the next site) and the piano goes to standby the menu
#      time (15 min) after the pause, with an app polling; PLAYING again with the stream still open -> a new max-hold
#      window (30 min policy: paused at 20, played at 30 -> still held at 59, used up at 61); no play status (REJ) -> the
#      stream alone holds up to the cap (the old rule)
#  17  (ota_main, F-A1) the POWER switch GP3[1] (IN_DATA23 bit 17 low =
#      pressed) through the REAL 20 ms site (stub -> pwc_apo_timer -> FUN_400ABFE8 -> FUN_400ABD74's press block), 20 ms per
#      site with an engine-loop pass after each: from ON 0.2 s -> nothing; 1.5 s -> the counter 0x4139E7F4 never above 15,
#      no state byte, standby (cleanup, amp off); in standby a 0.2 s tap wakes (cause 3); 1.5 s in standby -> woken, the
#      release changes nothing; 5 s from ON and from standby -> the clamp lets go at 4 s, the REAL release block writes
#      state byte 2 (the stock real off); policy bit3 -> 1.5 s = the stock off; control: the same 1.5 s on the stock words
#      (no payload) -> state byte 2; payload stores: of the stock globals only 0x4139E7F4 (always 14) besides the analysed
#      counter / state byte / GP3[8] event, no NOR op
from __future__ import annotations

import pathlib
import re
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rh = rtlib.load("roundtrip_ota_hooks")
ro, rp, rt, op, images, pa = rh.ro, rh.rp, rh.rt, rh.op, rh.images, rh.pa
check, Piano, le = ro.check, ro.Piano, ro.le
STOCKFW = ro.STOCKFW
RUN = ro.RUN
T_A, T_BUILTIN = rh.T_A, rh.T_BUILTIN
AP_DONE = rh.AP_DONE

NOR_ALLOWED = [(0x180000, 0x200000), (0x4C0000, 0x500000)]
KV_LO, KV_HI = 0x604C0000, 0x60500000
TIM12, TIM_INTSTAT, TIM_RUNNING, TIM_BASE_US = 0x01C21010, 0x01C21044, 0x410342F8, 0x410342FC
IN_DATA45 = 0x01E26070
BINDING = 0x413CA3DE
APO_COUNTER, APO_SETTING, POWER_REQ, GP3_EVENT, GP3_LEVEL = 0x4139E7E4, 0x4139E7F9, 0x4139E7FC, 0x4139E7FD, 0x4139E7FF
HP_VAR, BT_POWER, BOOT_BT, A2DP = 0x4139E7FA, 0x4139E7F0, 0x413A77B0, 0x413AE5B8
SWITCH_PREV = 0x4139E809
BT_RING, BT_WR, BT_RD, BT_FLAG = 0x413AA2FC, 0x413AE58A, 0x413AE588, 0x413AE5BD
BT_RESET, BT_ASSEMBLER, BT_OFF = 0x40145C00, 0x40145C44, 0x400AD030
CLEANUP, HP_ON_PATH, HP_OFF_PATH = 0x400B0EA8, 0x4013BC90, 0x4013BD94
NOR_ERASE, NOR_BUF = ro.NOR_ERASE, ro.NOR_BUF
SITES = [s for s in rh.SITES if s[0] != 0x401539AC]          # kawai, hp, exec, apo
STUBS = {0x40147E94: "hk_pw_kawai", 0x40143AB4: "hk_pw_hp", 0x40143AB8: "hk_pw_exec", 0x400AA874: "hk_pw_apo"}
OP_POLICY, OP_STANDBY, OP_DIAG = 0x18, 0x19, 0x1A
SB = dict(on=0, standby=1, waking=2, off_pending=3, off=4)
# the host core's replies (result/firmware tests/test_power_cave.c, ngext.c)
R_GET_DEFAULT = bytes.fromhex("58000010010178006001000000")      # frozen payloads: max hold 2 h (78 00)
R_GET_DEFAULT_5H = bytes.fromhex("5800001001012C016001000000")   # ota_main: 5 h (2C 01)
# stock_power.ld: name -> address (checked against the ELF)
BINDINGS = {
    "stk_amp_off": 0x400ABCAC, "stk_amp_on": 0x400ACCE8, "stk_mute": 0x400ABCC0, "stk_unmute": 0x400ACCFC,
    "stk_cleanup": 0x400B0EA8, "stk_hp_apply": 0x400ACC7C, "stk_hp_present": 0x400ACC58,
    "stk_bt_audio_off": 0x400AD030, "stk_bt_audio_on": 0x400AD098, "stk_bt_flags_clear": 0x40146810,
    "stk_apo_counter": APO_COUNTER, "stk_power_req": POWER_REQ, "stk_gp3_event": GP3_EVENT,
    "stk_gp3_level": GP3_LEVEL, "stk_apo_setting": APO_SETTING, "stk_power_press": 0x4139E7F4,
    "stk_bt_power": BT_POWER, "stk_bt_audio_cfg": 0x418CE0F8, "stk_apo_limits": 0x4009EB1C,
    "stk_gpio_out67": 0x01E2608C, "stk_hk_kawai": 0x4014A2C8, "stk_hk_hp": 0x400ACFA0,
    "stk_hk_exec": 0x400ACD28, "stk_hk_apo": 0x400ABFE8,
}


def pack7(raw):
    return ro.pack7(raw)


def policy_raw(apo_standby=True, hold_connected=False, hold_min=120, cap_min=480, bt_off=False):
    return bytes([1, (1 if apo_standby else 0) | (2 if hold_connected else 0) | (4 if bt_off else 0),
                  hold_min & 0xFF, hold_min >> 8, cap_min & 0xFF, cap_min >> 8, 0, 0])


class Rig:
    """One power-on of the loader with the clock, the stubs and the input pins of this test."""

    def __init__(self, image, syms, nor_mem=None, loader_syms=None):
        self.pn = pn = Piano(image, syms, nor_mem=nor_mem, nor_allowed=NOR_ALLOWED)
        self.emu = emu = pn.emu
        self.us = 5_000_000
        self.calls = {CLEANUP: 0, HP_ON_PATH: 0, HP_OFF_PATH: 0}
        for a in self.calls:
            emu.hook(a, (lambda aa: lambda e: self.calls.__setitem__(aa, self.calls[aa] + 1))(a))
        emu.write_u32(TIM_RUNNING, 20000)
        emu.periph[TIM12] = 0
        emu.periph[TIM_INTSTAT] = 0
        emu.periph[IN_DATA45] = 0                    # GP5[14] = 0: no headphones
        pn.nor.in_data23 = 1 << 17                   # power switch released (bit 17 high), GP3[8] = 0
        emu.write_bytes(SWITCH_PREV, b"\x01")
        emu.write_u32(BOOT_BT, 1)                    # boot parameter: BT allowed (FUN_400B39F0)
        emu.write_bytes(0x418CE0F8, b"\x01")         # BluetoothAudio on
        emu.write_u32(BT_POWER, 1)
        emu.write_bytes(APO_SETTING, b"\x01")        # 15 min
        emu.write_u32(APO_COUNTER, 1)
        pn.nor.gpio67 |= 0x8800                      # amp on, unmuted (as after the stock boot)
        self.clock(0)

    def clock(self, ms):
        self.us += int(ms * 1000)
        self.emu.write_u32(TIM_BASE_US, self.us & 0xFFFFFFFF)

    def tick(self, n=1, ms=8):
        for _ in range(n):
            self.clock(ms)
            self.pn.tick()

    def ask(self, port, body):
        """A 0x7D request: the resident hands it to the payload, the reply leaves on the next tick."""
        pn = self.pn
        direct = pn.sysex(port, body)
        pn.sent.clear()
        self.tick()
        out = direct + list(pn.sent)
        pn.sent.clear()
        return out

    def raw_reply(self, port, body, opc):
        """The reply to this op (other frames of the same pass, e.g. a standby notice, are kept in self.other)."""
        mine, self.other = [], []
        for p_, m_ in self.ask(port, body):
            r = rp.body_of([(p_, m_)], port)
            if r and r[0] == (opc | 0x40) and not (opc == OP_STANDBY and m_[7:8] == b"\x7F"):
                mine.append(r[1])
            else:
                self.other.append((p_, m_))
        return mine[0] if len(mine) == 1 else None

    def diag(self):
        r = self.raw_reply(2, rp.req(OP_DIAG), OP_DIAG)
        if r is None:
            return None
        d = rp.unpack7(r)
        out = dict(layout=d[0], hooks=d[1], state=d[2], flags=d[3], bt=d[4], kv=d[5], setting=d[6], wake=d[7],
                   counter=le(d, 8), timer=le(d, 12), expiries=le(d, 16), sb_ms=le(d, 20), held=le(d, 24),
                   gen=le(d, 28), woff=le(d, 32), hp_skips=le(d, 36, 2), exec_clears=le(d, 38, 2),
                   touches=le(d, 40, 2), nor_err=le(d, 42, 2), starts=d[46], relight=d[47], raw=d)
        if d[0] == 2 and len(d) >= 52:   # TV wake build (layout 2)
            out.update(btmod=d[48], rxflag=d[49], unlatches=d[50], resets=d[51])
        return out

    def site(self, addr):
        """Run the stock instruction at a hooked site (and what it reaches) back to site + 4."""
        res = self.emu.call(addr, until=addr + 4, max_insns=20_000_000)
        self.pn.writes.extend(res.writes)
        return res

    def amp_bits(self):
        return self.pn.nor.gpio67 & 0x8800

    def bt_line(self, text):
        wr = self.emu.read_u32(BT_WR) & 0xFFFF
        for ch in text.encode() + b"\r\n":
            self.emu.write_bytes(BT_RING + (wr & 0x7FF), bytes([ch]))
            wr = (wr + 1) & 0x7FF
        self.emu.write_bytes(BT_WR, wr.to_bytes(2, "little"))

    def close(self):
        self.pn.close()


def deliver(rig, port, body):
    emu = rig.emu
    emu.write_bytes(rt.BUF_BASE + port * 0x800, body)
    emu.write_bytes(rt.LEN_BASE + port * 2, len(body).to_bytes(2, "little"))
    emu.write_bytes(rt.STATE_BASE + port, b"\xF0")
    res = emu.call(rt.END_HANDLER, port, max_insns=5_000_000)
    rig.pn.writes.extend(res.writes)
    return res


def upload_apply(rig, data):
    """Boot, upload data to slot A, commit, apply live: (apply rc, status).  Run once per process for one machine
    state and payload; later rigs in the same state get its outcome (rtlib.memo)."""
    def run():
        pn = rig.pn
        pn.boot()
        pn.upload(0, data, seq=1)
        pn.run_job()
        rc = rh.apply(pn, T_A)
        st, _ = rh.run_apply(pn)
        return rc, st
    return rtlib.memo(("upload_apply", data), [rig, rig.pn, rig.pn.nor, rig.emu], run)


# payload name -> (variant, feat1, stub at 0x40147E94, extra stock calls, extra bindings)
# (stk_bt_rx_flag: the ring-reset fix of bt_feed, 07_tv_wake F4)
VARIANTS = {
    # ota_main: every feature - the SysEx front (FUN_4014EBFC), the
    # session core (a fifth hook 0x401539AC, the stock calls of stock_session.ld) and the BT port (its two stock
    # calls), the playback control (REC_CALLS); IDENTIFY feat2 0x7F, five hooks
    "ota_main": ("pmain", 0x77, "hk_sx_kawai", {0x4014EBFC: "stk_param_set", 0x40145DFC: "stk_p2_rx_reset",
                                                0x4014512C: "stk_bt_tx"},
                 {"stk_bt_rx_flag": BT_FLAG, "stk_bt_rx_reset": BT_RESET, "stk_p2_rx_reset": 0x40145DFC,
                  "stk_bt_tx": 0x4014512C, "stk_sx_state": 0x413B5400, "stk_sx_len": 0x413B53F0}),
}
# payloads built with the policy cap 0xFFFF = no cap (08_no_cap_autowake)
NO_CAP = {"ota_main"}
SESSION_CALLS = {"stk_desc": 0x4014C034, "stk_req_match": 0x4015205C, "stk_part_x": 0x4014EAF4,
                 "stk_desc_has": 0x401520B4, "stk_ctx_of": 0x4015B470, "stk_slot16": 0x4015B188, "stk_slot5": 0x4015B00C,
                 "stk_getter": 0x4015E960, "stk_frame_build": 0x4014B0DC, "stk_remote_get": 0x40151BB8,
                 "stk_remote_set": 0x40151B90, "stk_conn_mode": 0x400B1414, "stk_conn_sub": 0x400B1428,
                 "stk_conn_kind": 0x400B143C, "stk_conn_post_a": 0x400B0FBC, "stk_conn_post_b": 0x400B13AC,
                 "stk_ctx_copy": 0x4015DE98, "stk_ctx_clean": 0x4015C33C, "stk_bind": 0x40151B5C, "stk_unbind": 0x4014B55C,
                 "stk_reset_a": 0x4014B5A0, "stk_reset_b": 0x4014B5D0, "stk_reset_c": 0x4014B610, "stk_hk_midi": 0x4012A7D4}

# the playback control (feat2 0x10, stock_rec.ld): the stock routines the SysEx apply
# FUN_4016053C itself calls for Music / SongSelect / IntSongPart / MusicTempo / MusicProgress
REC_CALLS = {"stk_rec_post": 0x4015D70C, "stk_rec_block": 0x4015B4D8, "stk_rec_select": 0x401687E4,
             "stk_rec_part": 0x4016C9B8, "stk_rec_maxtime": 0x401694E4, "stk_rec_result": 0x4014DC0C,
             "stk_rec_tempo": 0x40167294, "stk_rec_seek": 0x40169448,
             # InternalSongDelete by the core: the calls of FUN_4016C7BC /
             # the KSO import after a song is built - the writer FUN_40168408 (+ its NOR job) and the player reload
             "stk_rec_store_write": 0x40168408, "stk_rec_store_changed": 0x401686E8, "stk_rec_load": 0x401674FC,
             "stk_rec_load_done": 0x40167128, "stk_rec_edit_ok": 0x4014F294}

# the MIDI channel port (feat3 0x10, stock_chan.ld): the three ota_loader_v4 hook targets, the
# reads both channel routines start with, and the apply FUN_4016053C the core's delayed-apply walk calls
MIDICH_CALLS = {"stk_mc_pc": 0x401475CC, "stk_mc_cc": 0x40148218, "stk_mc_defer": 0x401638E0, "stk_mc_run": 0x40135980,
                "stk_mc_sub": 0x401593B0, "stk_mc_block": 0x4015B4D8, "stk_param_apply": 0x4016053C}

# the intake gate (feat3 0x02, stock_session.ld; in ota_main since the 2026-11-16 fold
# 01_fold_feature_bits): the stock getters behind FUN_4014A2C8's 7E 04 / 05 info answers the core calls for a bound controller
GATE_CALLS = {"stk_gate_model_idx": 0x400B39B8, "stk_gate_model_entry": 0x400B389C, "stk_gate_version": 0x400B3894,
              "stk_gate_info_b": 0x400B39F0, "stk_gate_info_c": 0x400B39D8}


def no_cap_checks(image, syms, data, which):
    """Check 12: cap 0xFFFF. ota_main: saved, back after a power cycle, a 7-day (virtual)
    standby never reaches the real off; a payload without the no-cap policy (control): rc 1, nothing changed."""
    ok = True
    rig = Rig(image, syms)
    pn, emu = rig.pn, rig.emu
    upload_apply(rig, data)
    emu.write_bytes(BINDING, b"\x03")
    rig.tick(2)
    pol = policy_raw(cap_min=0xFFFF)
    nc = len(pn.nor_calls)
    r = rig.raw_reply(2, rp.req(OP_POLICY, 0x01, *pack7(pol)), OP_POLICY)
    if which not in NO_CAP:
        ok &= check(f"12 control ({which}, frozen): 18 01 with cap 0xFFFF -> rc 1, the reply still the default policy, "
                    "nothing written", r is not None and r[:2] == b"\x01\x01" and rp.unpack7(r[2:]) == policy_raw()
                    and len(pn.nor_calls) == nc, r.hex() if r else "none")
        rig.close()
        return ok
    ok &= check("12 18 01 with cap 0xFFFF (no cap) from the bound port -> rc 0, the reply echoes FF FF",
                r is not None and r[:2] == b"\x01\x00" and rp.unpack7(r[2:]) == pol, r.hex() if r else "none")
    nor = bytes(pn.nor.mem)
    rig.close()
    rig = Rig(image, syms, nor_mem=nor)
    pn, emu = rig.pn, rig.emu
    st, _ = pn.boot()
    emu.write_bytes(BINDING, b"\x03")
    rig.tick(3)
    r = rig.raw_reply(2, rp.req(OP_POLICY, 0x00), OP_POLICY)
    rs = rig.raw_reply(2, rp.req(OP_STANDBY, 0x01), OP_STANDBY)
    ok &= check("12 power-on again on the same NOR: 18 00 answers cap 0xFFFF; 19 01 -> standby",
                st["active"] == ro.ACT_A and r is not None and rp.unpack7(r[2:]) == pol
                and rs is not None and rs[1:3] == bytes([0, SB["standby"]]), f"{r and r.hex()} {rs and rs.hex()}")
    held, req, states = [], [], set()
    for _ in range(7 * 48):                       # 7 days in 30-min engine-loop steps (the µs clock wraps)
        rig.tick(1, ms=30 * 60 * 1000)
        emu.write_u32(APO_COUNTER, 45000)         # whatever the stock counter did: the timer site holds it
        rig.site(0x400AA874)
        held.append(emu.read_u32(APO_COUNTER))
        req.append(emu.read_bytes(POWER_REQ, 1)[0])
        states.add(rig.diag()["state"])
    d = rig.diag()
    week = 7 * 24 * 3600 * 1000
    ok &= check("12 7 days (virtual) of standby with no cap: every 30 min still standby, the 20 ms site holds the stock "
                "counter (never an expiry), state byte never 2; state timer = 7 days",
                states == {SB["standby"]} and max(held) <= 2 and not any(req) and d["expiries"] == 0
                and week <= d["sb_ms"] < week + 60_000, f"{states} {max(held)} {set(req)} {d['sb_ms']}")
    rw_ = rig.raw_reply(2, rp.req(OP_STANDBY, 0x02), OP_STANDBY)
    rig.tick(15)
    dn = rig.diag()
    ok &= check("12 19 02 after the week wakes: ON, amp + unmute back",
                rw_ is not None and rw_[1] == 0 and dn["state"] == SB["on"] and rig.amp_bits() == 0x8800,
                f"{rw_ and rw_.hex()} {dn['state']} {rig.amp_bits():#x}")
    rig.close()
    return ok


REC_STATE, FILEOP, JOB1 = 0x4191EF79, 0x413CA3EA, 0x4139DAB4
CONNECT = bytes.fromhex("F0407F7E08020101F7")
IDENT_Q = bytes.fromhex("F0407F7E08020540F7")


def notices(frames, state):
    """{stock port: count} of the standby notices F0 7D C A 7 9 59 7F 00 <state> F7 among (port, frame)."""
    want = b"\xF0\x7D\x43\x41\x37\x39\x59\x7F\x00" + bytes([state]) + b"\xF7"
    out = {}
    for p_, m_ in frames:
        if bytes(m_) == want:
            out[p_] = out.get(p_, 0) + 1
    return out


def expire(rig):
    """The stock counter at the 15 min limit, then the real 20 ms site: returns (counter, state byte) after it."""
    rig.emu.write_u32(APO_COUNTER, 45000)
    rig.site(0x400AA874)
    return rig.emu.read_u32(APO_COUNTER), rig.emu.read_bytes(POWER_REQ, 1)[0]


def main_checks(image, syms, data):
    """Check 13 (ota_main): features 1 and 2 of 09_p3_power."""
    ok = True
    rig = Rig(image, syms)
    pn, emu = rig.pn, rig.emu
    upload_apply(rig, data)
    emu.write_bytes(BINDING, b"\x03")
    rig.tick(2)
    # -- feature 1: hold until STOP (100 virtual minutes), another link's STOP ignored
    rig.bt_line("CONNECT 0 A2DP 19")
    rig.bt_line("CONNECT 2 A2DP 19")
    rig.bt_line("A2DP STREAMING START 0")
    rig.tick()
    held = []
    for i in range(10):
        rig.tick(1, ms=10 * 60 * 1000)
        if i == 3:
            rig.bt_line("A2DP STREAMING STOP 2")
            rig.tick()
        held.append(expire(rig))
    d = rig.diag()
    ok &= check("13 feature 1: A2DP STREAMING START, then 100 virtual minutes with an expiry tick every 10 min through the "
                "real 20 ms site (a STOP of link 2 in between): every tick held - the counter rests at the limit 45000 (the "
                "hold stops the expiry, not the clock), no state byte 2 - no expiry, still ON and streaming",
                all(h == (45000, 0) for h in held) and d["expiries"] == 0 and d["state"] == SB["on"] and d["bt"] == 2,
                f"{set(held)} exp {d['expiries']} st {d['state']} bt {d['bt']}")
    rig.bt_line("A2DP STREAMING STOP 0")
    rig.tick()
    rig.site(0x400AA874)
    c_stop = emu.read_u32(APO_COUNTER)
    after = expire(rig)
    rig.tick()
    d = rig.diag()
    ok &= check("13 feature 1: STREAMING STOP of the playing link -> the next 20 ms site resets the counter (1, +1 by the "
                "stock body = the full menu time from the STOP); the next expiry goes to the engine loop -> standby",
                c_stop == 2 and d["expiries"] == 1 and d["state"] == SB["standby"] and after == (2, 0),
                f"stop {c_stop} {after} {d['expiries']} {d['state']}")
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x02), OP_STANDBY)
    rig.tick(15)
    # -- feature 1: the open stream (TV / Windows paused) is held only up to the max hold (30 min policy)
    r = rig.raw_reply(2, rp.req(OP_POLICY, 0x01, *pack7(policy_raw(hold_min=30))), OP_POLICY)
    rig.bt_line("A2DP STREAMING START 0")
    rig.tick()
    rig.tick(1, ms=29 * 60 * 1000)
    h29 = expire(rig)
    rig.tick()
    s29 = rig.diag()
    rig.tick(1, ms=2 * 60 * 1000)
    h31 = expire(rig)
    rig.tick()
    s31 = rig.diag()
    ok &= check("13 feature 1: max hold 30 min (op 18 01), stream left open, nobody plays: at 29 min the "
                "site still holds (counter at the limit, flag 0x08); at 31 min the hold is used up (flag 0x10) and the idle "
                "piano's counter expires on the next real site -> standby right away (not 30 min + the menu time)",
                r is not None and r[1] == 0 and h29 == (45000, 0) and s29["state"] == SB["on"] and s29["flags"] & 0x18 == 0x08
                and s31["flags"] & 0x10 and s31["state"] == SB["standby"] and h31 == (2, 0),
                f"{r and r.hex()} {h29} {s29['state']} {s29['flags']:#x} {h31} {s31['state']} {s31['flags']:#x}")
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x02), OP_STANDBY)
    rig.bt_line("A2DP STREAMING STOP 0")
    rig.tick(15)
    # -- activity right before the cap: the normal APO counts on from it
    rig.bt_line("A2DP STREAMING START 0")
    rig.tick()
    rig.tick(1, ms=29 * 60 * 1000)
    h29 = expire(rig)                          # expired under the hold: resting at the limit
    emu.call(0x400ACB84)                       # the REAL stock activity reset FUN_400ACB84 (a key / pedal / set)
    rig.tick(1, ms=2 * 60 * 1000)              # past the cap
    for _ in range(3):
        rig.site(0x400AA874)
    rig.tick()
    s_cap = rig.diag()
    c_cap = emu.read_u32(APO_COUNTER)
    later = expire(rig)                        # the menu time since that activity runs out
    rig.tick()
    s_end = rig.diag()
    ok &= check("13 feature 1: activity (the real FUN_400ACB84) at 29 min of a 30 min hold: past the cap "
                "(flag 0x10) the counter counts on from that activity (no expiry, still ON); when it reaches the menu limit "
                "-> standby (the normal APO)",
                h29 == (45000, 0) and s_cap["flags"] & 0x10 and s_cap["state"] == SB["on"] and 2 <= c_cap <= 5
                and later == (2, 0) and s_end["state"] == SB["standby"],
                f"{h29} cap {s_cap['flags']:#x} st {s_cap['state']} c {c_cap} later {later} end {s_end['state']}")
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x02), OP_STANDBY)
    rig.bt_line("A2DP STREAMING STOP 0")
    rig.tick(15)
    rig.raw_reply(2, rp.req(OP_POLICY, 0x02), OP_POLICY)
    # -- feature 2: the panel holds the stock binding, the session core keeps the app bound
    deliver(rig, 2, IDENT_Q)                   # a frame of the bound app: the core's table takes the binding 3
    rig.tick()
    deliver(rig, 3, CONNECT)                   # the panel connects ...
    rig.tick()
    emu.write_bytes(BINDING, b"\x04")          # ... and holds the stock binding (the stock CONNECT answer)
    deliver(rig, 3, IDENT_Q)
    rig.tick()
    rc0 = rig.raw_reply(0, rp.req(OP_STANDBY, 0x03), OP_STANDBY)
    cl0 = rig.calls[CLEANUP]
    emu.write_bytes(REC_STATE, b"\x04")
    rr = rig.raw_reply(2, rp.req(OP_STANDBY, 0x03), OP_STANDBY)
    emu.write_bytes(REC_STATE, b"\x02")
    emu.write_bytes(FILEOP, b"\x01")
    rf = rig.raw_reply(2, rp.req(OP_STANDBY, 0x03), OP_STANDBY)
    emu.write_bytes(FILEOP, b"\x00")
    emu.write_bytes(JOB1, b"\x01")
    rj = rig.raw_reply(2, rp.req(OP_STANDBY, 0x03), OP_STANDBY)
    emu.write_bytes(JOB1, b"\x00")
    d = rig.diag()
    ok &= check("13 feature 2: 19 03 from port 0 (not bound) -> rc 2; from the app while recording (0x4191EF79 = 4) -> rc 6 "
                "reason 1, a file operation (0x413CA3EA) -> reason 2, a RecorderData job (0x4139DAB4) -> reason 3; no "
                "cleanup, still ON, amp on",
                rc0 == bytes([3, 2, 0, 0]) and rr == bytes([3, 6, 0, 1]) and rf == bytes([3, 6, 0, 2])
                and rj == bytes([3, 6, 0, 3]) and rig.calls[CLEANUP] == cl0 and d["state"] == SB["on"]
                and rig.amp_bits() == 0x8800 and len(d["raw"]) >= 54 and d["raw"][53] == 3,
                f"{rc0 and rc0.hex()} {rr and rr.hex()} {rf and rf.hex()} {rj and rj.hex()} {d['state']}")
    pn.sent.clear()
    ro_ = rig.raw_reply(2, rp.req(OP_STANDBY, 0x03), OP_STANDBY)
    seen = list(rig.other)
    rig.tick()
    seen += list(pn.sent)
    d = rig.diag()
    n_enter = notices(seen, SB["standby"])
    ok &= check("13 feature 2: the standby notice 59 7F 00 01 reaches the core-bound app (port 2) while "
                "the panel holds the stock binding; never the panel (port 3) nor the unbound port 0",
                n_enter == {2: 1}, f"{n_enter}")
    ok &= check("13 feature 2: the stock binding is the panel's (4), the session core has the app bound: 19 03 from the app "
                "-> rc 0, standby the way the APO goes (cleanup once, amp off + muted), no state byte 2",
                emu.read_bytes(BINDING, 1)[0] == 4 and ro_ is not None and ro_[:2] == b"\x03\x00"
                and ro_[2] == SB["standby"] and d["state"] == SB["standby"] and rig.calls[CLEANUP] == cl0 + 1
                and rig.amp_bits() == 0 and emu.read_bytes(POWER_REQ, 1)[0] == 0 and d["raw"][52] == 1,
                f"bind {emu.read_bytes(BINDING, 1)[0]} {ro_ and ro_.hex()} {d['state']} {rig.amp_bits():#x}")
    pn.sent.clear()
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x02), OP_STANDBY)
    seen = list(rig.other)
    for _ in range(15):
        rig.tick()
        seen += list(pn.sent)
        pn.sent.clear()
    n_wake = notices(seen, SB["on"])
    ok &= check("13 feature 2: woken (19 02 from the app) -> the awake notice 59 7F 00 00 to the app "
                "(port 2) only, the panel still holds the stock binding",
                n_wake == {2: 1} and emu.read_bytes(BINDING, 1)[0] == 4, f"{n_wake}")
    r = rig.raw_reply(2, rp.req(OP_POLICY, 0x01, *pack7(policy_raw(apo_standby=False))), OP_POLICY)
    ro_ = rig.raw_reply(2, rp.req(OP_STANDBY, 0x03), OP_STANDBY)
    for _ in range(40):
        rig.tick()
        if emu.read_bytes(POWER_REQ, 1)[0] == 2:
            break
    ok &= check("13 feature 2: policy \"power off\" (bound app, op 18 01) -> 19 03 rc 0, then the stock executor's request "
                "(state byte 2) once the startup snapshot is written",
                r is not None and r[1] == 0 and ro_ is not None and ro_[:2] == b"\x03\x00"
                and emu.read_bytes(POWER_REQ, 1)[0] == 2 and rig.diag()["state"] == SB["off"],
                f"{r and r.hex()} {ro_ and ro_.hex()} {emu.read_bytes(POWER_REQ, 1)[0]}")
    rig.close()
    return ok


def bt_fill_to(rig, to):
    """Ignored module lines (one per engine-loop pass) until the ring write index is `to`."""
    emu = rig.emu
    while (wr := emu.read_u32(BT_WR) & 0x7FF) != to:
        if (to - wr) & 0x7FF >= 34:
            rig.bt_line("AVRCP 0 PLAYBACK STATUS CHANGED")
        else:
            emu.write_bytes(BT_RING + wr, b"\n")
            emu.write_bytes(BT_WR, ((wr + 1) & 0x7FF).to_bytes(2, "little"))
        emu.write_bytes(BT_RD, emu.read_bytes(BT_WR, 2))   # the stock assembler took it
        rig.tick()


def tv_wake_checks(image, syms, data, psyms, plen, rule9=False):
    """Check 11: TV wake, UART1 latch clear (F1), no re-read after a ring reset (F4)."""
    ok = True
    rig = Rig(image, syms)
    pn, emu = rig.pn, rig.emu
    upload_apply(rig, data)
    emu.write_bytes(BINDING, b"\x03")
    rig.tick(2)
    pn.writes.clear()
    offs = []
    emu.hook(BT_OFF, lambda e: (offs.append(1), rt.se.StockEmu.CONTINUE)[1])
    rig.bt_line("CONNECT 0 A2DP 19")
    emu.write_bytes(A2DP, b"\x01")
    rig.tick()
    rs = rig.raw_reply(2, rp.req(OP_STANDBY, 0x01), OP_STANDBY)
    rig.tick(10)
    d0 = rig.diag()
    rig.bt_line("NO CARRIER 0 ERROR 0")
    rig.bt_line("CONNECT 1 A2DP 19")
    rig.bt_line("A2DP CODEC SBC JOINT_STEREO 44100 BITPOOL 2-53")
    rig.tick(10)
    d1 = rig.diag()
    ok &= check("11 standby (19 01) with the TV linked: FUN_400AD030 never entered, 0x4139E7F0 = 1, diag layout 2 module "
                "on + TV wake armed; CONNECT / NO CARRIER / codec lines in the UART1 ring do not wake, amp stays off",
                rs is not None and rs[1:3] == bytes([0, SB["standby"]]) and not offs and emu.read_u32(BT_POWER) == 1
                and d0["layout"] == 2 and d0["btmod"] & 0x07 == 0x03 and d0["bt"] == 1 and d1["state"] == SB["standby"]
                and rig.amp_bits() == 0, f"{rs and rs.hex()} offs {len(offs)} {d0 and d0.get('btmod')} {d1 and d1['state']}")
    rig.bt_line("A2DP STREAMING START 1")
    rig.tick()
    d2 = rig.diag()
    rig.tick(15)
    d3 = rig.diag()
    emu.write_u32(APO_COUNTER, 45000)
    rig.site(0x400AA874)
    ok &= check("11 A2DP STREAMING START in the ring -> the next engine-loop pass wakes (cause 5), 120 ms later ON with "
                "amp + unmute (0x8800), the module never switched (no FUN_400AD030 / FUN_400AD098 needed), the streaming "
                "hold keeps the counter from expiring through the 20 ms site (frozen: pinned at 1 -> 2; ota_main, 09_p3_power "
                "§9: resting at the limit 45000)",
                d2["state"] == SB["waking"] and d2["wake"] == 5 and d3["state"] == SB["on"] and rig.amp_bits() == 0x8800
                and emu.read_u32(BT_POWER) == 1 and not offs and d3["bt"] == 2 and d3["flags"] & 0x08
                and emu.read_u32(APO_COUNTER) == (45000 if rule9 else 2) and emu.read_bytes(POWER_REQ, 1)[0] == 0,
                f"{d2 and (d2['state'], d2['wake'])} {d3 and d3['state']} amp {rig.amp_bits():#x} c {emu.read_u32(APO_COUNTER)}")

    # F1: UART error latch. Control: the stock assembler leaves it latched (ring empty, nothing to take).
    wr = emu.read_bytes(BT_WR, 2)
    emu.write_bytes(BT_RD, wr)
    emu.write_bytes(BT_FLAG, b"\x80")
    res = emu.call(BT_ASSEMBLER, max_insns=2_000_000)
    pn.writes.extend(res.writes)
    stock_keeps = emu.read_bytes(BT_FLAG, 1) == b"\x80"
    resets = []
    emu.hook(BT_RESET, lambda e: (resets.append(1), rt.se.StockEmu.CONTINUE)[1])
    rig.tick(1, ms=300)
    early = emu.read_bytes(BT_FLAG, 1) == b"\x80" and not resets
    rig.tick(1, ms=300)
    d4 = rig.diag()
    cleared = (emu.read_bytes(BT_FLAG, 1) == b"\x00" and emu.read_u32(BT_WR) & 0xFFFF == 0
               and emu.read_bytes(BT_RD, 2) == b"\x00\x00")
    rig.bt_line("A2DP STREAMING STOP 1")
    res = emu.call(BT_ASSEMBLER, max_insns=2_000_000)
    pn.writes.extend(res.writes)
    stock_reads = emu.read_bytes(BT_RD, 2) == emu.read_bytes(BT_WR, 2) and emu.read_bytes(BT_FLAG, 1) == b"\x00"
    rig.tick()
    d5 = rig.diag()
    ok &= check("11 UART1 latch 0x80 (control: the real assembler FUN_40145C44 leaves it latched): not cleared at 300 ms, "
                "at 600 ms the payload calls the real FUN_40145C00 once (flag, write and read index 0), diag 1 clear; the "
                "real assembler takes the next line and the payload sees it (STREAMING STOP)",
                res.stop == "returned" and stock_keeps and early and len(resets) == 1 and cleared and d4["unlatches"] == 1
                and stock_reads and d5["bt"] != 2,
                f"keeps {stock_keeps} early {early} resets {len(resets)} cleared {cleared} reads {stock_reads} bt {d5 and d5['bt']}")

    # F4: a ring reset must not replay the previous lap. START / STOP at 0x600 (lap 1), lap 2 up to 0x100.
    bt_fill_to(rig, 0x600)
    rig.bt_line("A2DP STREAMING START 0")
    rig.tick()
    rig.bt_line("A2DP STREAMING STOP 0")
    rig.tick()
    bt_fill_to(rig, 0x100)
    rs = rig.raw_reply(2, rp.req(OP_STANDBY, 0x01), OP_STANDBY)
    starts = rig.diag()["starts"]
    ring = emu.read_bytes(BT_RING, 0x800)
    trap = b"A2DP STREAMING START" in ring[0x100:]
    res = emu.call(BT_RESET)                       # the stock assembler's reset (overflow / error)
    pn.writes.extend(res.writes)
    rig.bt_line("AVRCP 0 PLAYBACK STATUS CHANGED")
    rig.tick(5)
    d6 = rig.diag()
    rig.bt_line("A2DP STREAMING START 0")
    rig.tick()
    d7 = rig.diag()
    ok &= check("11 ring reset by the real FUN_40145C00 while in standby (an old START still in the ring past the read "
                "position): no stale START, still standby, one reset taken; the next real START wakes",
                rs is not None and rs[1] == 0 and trap and d6["state"] == SB["standby"] and d6["starts"] == starts
                and d6["resets"] >= 1 and d7["state"] == SB["waking"] and d7["wake"] == 5,
                f"trap {trap} {d6 and (d6['state'], d6['starts'], d6.get('resets'))} {d7 and d7['state']}")
    emu.unhook(BT_OFF)
    emu.unhook(BT_RESET)

    okw = [(RUN, RUN + 0x20000), (0x40180000, 0x401A0000), ro.STACK, (0x40B90000, 0x40B90200),
           (APO_COUNTER, APO_COUNTER + 4), (POWER_REQ, POWER_REQ + 1), (GP3_EVENT, GP3_EVENT + 1)]
    if "hk_ss_midi" in psyms:   # session core (ota_main), contract (b): the pump tail (ssc_pump) marks the resident's inflight flag around its sender
        okw.append((syms["ota_in_sysex"], syms["ota_in_sysex"] + 1))
    mine = [w_ for w_ in pn.writes if RUN <= w_.pc < RUN + plen]
    strayw = [w_ for w_ in mine if not any(a <= w_.addr < b_ for a, b_ in okw)]
    stockw = sorted({w_.addr for w_ in mine if 0x41000000 <= w_.addr < 0x41D00000})
    ok &= check(f"11 {len(mine)} stores by payload code (check 11): the same set as check 8 - the UART1 ring, its indices "
                "and flag are written only by stock code (FUN_40145C00)",
                mine and not strayw and set(stockw) <= {APO_COUNTER, POWER_REQ, GP3_EVENT},
                f"stray {strayw[:2]}" if strayw else str([hex(a) for a in stockw]))
    rig.close()
    return ok


# ---- 14. polls are not activity ------------------------------------------------------------------------
PARSER, PUMP_TAIL = 0x40148B24, 0x401539AC    # FUN_40148B24(port, byte): resets the APO counter for every byte < 0xF8
SX_STATE, SX_LEN = 0x413B5400, 0x413B53F0
TAG7D = bytes.fromhex("F07D43413739")
POLL_1B03 = TAG7D + bytes([0x1B, 0x03, 0xF7])
POLL_1D04 = TAG7D + bytes([0x1D, 0x04, 0xF7])
POLL_1A = TAG7D + bytes([0x1A, 0xF7])
POLL_1D0F = TAG7D + bytes([0x1D, 0x0F, 0xF7])
POLL_1900 = TAG7D + bytes([0x19, 0x00, 0xF7])
K_GET = bytes.fromhex("F0407F0208025100" "7FF7")         # fn 02 get AutoPowerOff
K_ACK = bytes.fromhex("F0407F0708025100" "7FF7")         # fn 07 ACK
K_SET = bytes.fromhex("F0407F1008025501" "0040F7")       # fn 10 set Volume (55 01) = 0x40
K_TOUCH = bytes.fromhex("F0407F3208025139" "007FF7")     # the panel touch notice (PowerManagementTimerReset)
WAKE = TAG7D + bytes([0x19, 0x02, 0xF7])
NOTE_ON = bytes([0x90, 60, 0x40])


def parse(rig, port, frame):
    """The bytes of one frame through the REAL stock byte parser (end handler -> 0x40147E94 stub / resident -> payload)."""
    for b in frame:
        res = rig.emu.call(PARSER, port, b, max_insns=20_000_000)
        rig.pn.writes.extend(res.writes)


def pump_tail(rig):
    """The MIDI pump tail site 0x401539AC (task 19 after the four pumps): hk_ss_midi -> pwc_midi_tail, ssc_pump, stock."""
    return rig.site(PUMP_TAIL)


def ticks(rig, psyms, n):
    """n 20 ms timer ticks without an expiry in between: what n passes of the timer site do to the stock counter and the
    payload's tick count (FUN_400ABFE8 adds 1, pwc_apo_timer adds 1), then one REAL pass of the site (expiry test)."""
    emu = rig.emu
    emu.write_u32(APO_COUNTER, emu.read_u32(APO_COUNTER) + n - 1)
    emu.write_u32(psyms["timer_ticks"], emu.read_u32(psyms["timer_ticks"]) + n - 1)
    rig.site(0x400AA874)


def quiet_checks(image, syms, data, psyms):
    """Check 14 (ota_main): which frames reset the stock APO counter, which resets stay; an app polling every few seconds
    no longer keeps the piano on; a real set and the wake op still reset it; the BT stream hold is untouched."""
    ok = True
    rig = Rig(image, syms)
    pn, emu = rig.pn, rig.emu
    upload_apply(rig, data)
    emu.write_bytes(BINDING, b"\x03")
    rig.tick(3)
    # -- measurement: each frame through the real parser at counter 30000, the counter after the parser and after the tail
    table = []
    for name, port, frame in (("1B 03", 2, POLL_1B03), ("1D 04", 2, POLL_1D04), ("1A", 2, POLL_1A), ("1D 0F", 2, POLL_1D0F),
                              ("19 00", 2, POLL_1900), ("fn 02 get", 2, K_GET), ("fn 07 ACK", 3, K_ACK),
                              ("CONNECT", 2, CONNECT), ("fn 10 set", 2, K_SET), ("touch 32 51 39", 3, K_TOUCH),
                              ("19 02 wake", 2, WAKE), ("note on", 0, NOTE_ON)):
        ticks(rig, psyms, 2)
        pump_tail(rig)                       # a pass with nothing: the base follows the counter
        emu.write_u32(APO_COUNTER, 30000)
        pump_tail(rig)
        parse(rig, port, frame)
        after_parse = emu.read_u32(APO_COUNTER)
        pump_tail(rig)
        table.append((name, after_parse, emu.read_u32(APO_COUNTER)))
        rig.tick()
        pn.sent.clear()
    quiet = {"1B 03", "1D 04", "1A", "1D 0F", "19 00", "fn 02 get", "fn 07 ACK", "CONNECT"}
    print("    frame            after parser  after pump tail")
    for name, a, b in table:
        print(f"    {name:16} {a:12}  {b:15}")
    ok &= check("14 measured: every frame byte < 0xF8 through the real parser FUN_40148B24 resets the stock counter (30000 -> 1); "
                "at the pump tail the polls / reads (1B 03, 1D 04, 1A, 1D 0F, 19 00, Kawai fn 02 get, fn 07 ACK from the "
                "panel, CONNECT) get it back (30000), a fn 10 set, the panel touch 32 51 39, 19 02 and a note-on keep it at 1",
                all(a == 1 for _n, a, _b in table)
                and all((b == 30000) if n in quiet else (b == 1) for n, _a, b in table), str(table))
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x02), OP_STANDBY)   # the 19 02 above may have run (ON anyway)
    rig.tick(3)
    # -- the field case: an app polls 1D 04 (every 4 s here; the app's 2 s only doubles the passes), 1B 03 every 12 s,
    #    1A + a Kawai get every 8 s, the panel ACKs
    emu.write_u32(APO_COUNTER, 1)
    pump_tail(rig)                           # an empty pass: the base follows
    w0 = len(pn.writes)
    raw0 = rig.diag()["raw"]
    t, entered = 0, None
    while t < 20 * 60 * 1000 and entered is None:
        parse(rig, 2, POLL_1D04)
        if t % 12000 == 0:
            parse(rig, 2, POLL_1B03)
        if t % 8000 == 0:
            parse(rig, 2, POLL_1A)
            parse(rig, 2, K_GET)
            parse(rig, 3, K_ACK)
        pump_tail(rig)
        ticks(rig, psyms, 200)               # 4 s
        rig.tick(1, ms=4000)
        pn.sent.clear()
        t += 4000
        if t % 60000 == 0 or emu.read_u32(APO_COUNTER) < 100:
            if rig.diag()["state"] == SB["standby"]:
                entered = t
    d = rig.diag()
    raw = d["raw"]
    undone, nq, nr = (le(raw, o, 2) - le(raw0, o, 2) for o in (54, 56, 58))
    ok &= check("14 an app polling 1D 04 every 4 s, 1B 03 every 12 s, 1A + Kawai fn 02 every 8 s, the panel ACKing: APO 15 min "
                "still expires through the real 20 ms site at 15 min (+- one 4 s step) -> standby; diag: resets undone, "
                "quiet frames counted, no activity frame",
                entered is not None and 15 * 60 * 1000 - 4000 <= entered <= 15 * 60 * 1000 + 4000 and d["state"] == SB["standby"]
                and undone > 200 and nq > 300 and nr == 0, f"standby at {entered} ms, undone {undone} quiet {nq} real {nr}")
    mine = [w_ for w_ in pn.writes[w0:] if RUN <= w_.pc < RUN + 0x20000]
    stockw = sorted({w_.addr for w_ in mine if 0x41000000 <= w_.addr < 0x41D00000})
    ok &= check("14 stores by payload code in that run: of the stock globals only the counter 0x4139E7E4 (the tail never "
                "writes the state byte)", set(stockw) <= {APO_COUNTER}, str([hex(a) for a in stockw]))
    # -- a real fn 10 set resets it (after the wake op, itself activity)
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x02), OP_STANDBY)
    rig.tick(15)
    ticks(rig, psyms, 20000)
    pump_tail(rig)
    before = emu.read_u32(APO_COUNTER)
    parse(rig, 2, POLL_1D04)
    pump_tail(rig)
    kept = emu.read_u32(APO_COUNTER)
    parse(rig, 2, K_SET)
    pump_tail(rig)
    after = emu.read_u32(APO_COUNTER)
    ok &= check("14 ON again (19 02), counter 20000: a 1D 04 poll leaves it (>= 20000), a fn 10 set resets it to 1 for good",
                rig.diag()["state"] == SB["on"] and before >= 20000 and kept >= before and after == 1, f"{before} {kept} {after}")
    # -- BT stream hold with polls: held for 30 min, then STOP -> the APO runs out 15 min later despite the polls
    rig.bt_line("CONNECT 0 A2DP 19")
    emu.write_bytes(A2DP, b"\x01")
    rig.bt_line("A2DP STREAMING START 0")
    rig.tick()
    held = []
    for i in range(15):                      # 30 virtual minutes, 2 min steps
        parse(rig, 2, POLL_1D04)
        pump_tail(rig)
        ticks(rig, psyms, 6000)
        rig.tick(1, ms=120_000)
        held.append(emu.read_u32(APO_COUNTER))
    s_hold = rig.diag()
    rig.bt_line("A2DP STREAMING STOP 0")
    rig.tick()
    t, entered = 0, None
    while t < 20 * 60 * 1000 and entered is None:
        parse(rig, 2, POLL_1D04)
        pump_tail(rig)
        ticks(rig, psyms, 1500)              # 30 s
        rig.tick(1, ms=30_000)
        pn.sent.clear()
        t += 30_000
        if rig.diag()["state"] == SB["standby"]:
            entered = t
    ok &= check("14 BT stream hold with a poll every 2 min: 30 min held by the 20 ms site (ON; the counter keeps counting - "
                "every poll's reset undone at the pump tail - and rests at the limit 45000); after STOP the APO runs out "
                "15 min later (+- one 30 s step) although the polls go on",
                s_hold["state"] == SB["on"] and held == sorted(held) and held[-1] == 45000 and held[0] > 6000
                and entered is not None
                and 15 * 60 * 1000 - 30_000 <= entered <= 15 * 60 * 1000 + 30_000,
                f"held {held[:3]}..{held[-1]} st {s_hold['state']} standby at {entered}")
    rig.close()
    return ok


# ---- 15. APO test shortcut 19 04 ---------------------------------------------------------------------
LIMIT_15 = 45000                              # stk_apo_limits[1] (0x4009EB1C), 20 ms ticks


def apo_test(rig, secs, port=2):
    """19 04 <s> as the app sends it: through the REAL byte parser (activity), the pump tail, then the engine-loop pass
    that answers.  Returns the reply data after 59 (sub rc state s_hi s_lo setting flags) or None."""
    frame = TAG7D + bytes([0x19, 0x04, (secs >> 7) & 0x7F, secs & 0x7F, 0xF7])
    rig.pn.sent.clear()
    parse(rig, port, frame)
    pump_tail(rig)
    rig.tick()
    out = [m_[7:-1] for _p, m_ in rig.pn.sent if m_[:7] == TAG7D + b"\x59" and m_[7:8] == b"\x04"]
    rig.pn.sent.clear()
    return out[0] if len(out) == 1 else None


def polled(rig, psyms, limit_ms, step_ms=2000):
    """An app polling 1D 04 every step through the real parser + pump tail, the 20 ms site (step / 20 ticks, the last one
    real), the engine loop.  Returns ms until standby, or None."""
    t = 0
    while t < limit_ms:
        parse(rig, 2, POLL_1D04)
        pump_tail(rig)
        ticks(rig, psyms, step_ms // 20)
        rig.tick(1, ms=step_ms)
        rig.pn.sent.clear()
        t += step_ms
        if rig.diag()["state"] == SB["standby"]:
            return t
    return None


def apo_test_checks(image, syms, data, psyms):
    """Check 15 (ota_main): 19 04 makes the APO expire after s seconds through the normal path."""
    ok = True
    rig = Rig(image, syms)
    pn, emu = rig.pn, rig.emu
    upload_apply(rig, data)
    emu.write_bytes(BINDING, b"\x03")
    rig.tick(3)
    pump_tail(rig)
    w0, n0 = len(pn.writes), len(pn.nor_calls)
    # -- 60 s with app polling -> standby at 60 s
    r = apo_test(rig, 60)
    rig.site(0x400AA874)                      # the next 20 ms tick: the jump
    left = LIMIT_15 - emu.read_u32(APO_COUNTER)
    t = polled(rig, psyms, 5 * 60 * 1000)
    d = rig.diag()
    ok &= check("15 19 04 00 3C (60 s) through the real parser -> 59 04 00 00 00 3C 01 00; the next real 20 ms site puts the "
                "stock counter at 45000 - 3000 (+1 by FUN_400ABFE8); an app polling 1D 04 every 2 s: the real site expires "
                "at 60 s (+- one 2 s step) -> standby (the policy's APO action), state byte 0",
                r == bytes([0x04, 0, SB["on"], 0, 0x3C, 1, 0]) and left == 3000 - 1 and t is not None
                and 58_000 <= t <= 62_000 and d["state"] == SB["standby"] and emu.read_bytes(POWER_REQ, 1)[0] == 0
                and rig.calls[CLEANUP] == 1, f"r {r and r.hex()} left {left} standby at {t} st {d['state']}")
    r_sb = apo_test(rig, 60)
    ok &= check("15 in standby: 19 04 -> rc 3, nothing moves", r_sb is not None and r_sb[1] == 3 and r_sb[2] == SB["standby"],
                str(r_sb and r_sb.hex()))
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x02), OP_STANDBY)
    rig.tick(15)
    # -- activity during the countdown: a fn 10 set at 30 s -> the full menu time
    r = apo_test(rig, 60)
    rig.site(0x400AA874)
    t1 = polled(rig, psyms, 30_000)
    parse(rig, 2, K_SET)
    pump_tail(rig)
    after_set = emu.read_u32(APO_COUNTER)
    t2 = polled(rig, psyms, 5 * 60 * 1000, step_ms=10_000)
    c_after = emu.read_u32(APO_COUNTER)
    ok &= check("15 a fn 10 set 30 s into a 60 s countdown resets the counter to 1 (activity): 5 more minutes of polling, still "
                "ON, the counter counts the full 15 min from the set",
                r is not None and r[1] == 0 and t1 is None and after_set == 1 and t2 is None
                and rig.diag()["state"] == SB["on"] and 14_000 <= c_after <= 15_600, f"set {after_set} t {t1} {t2} c {c_after}")
    # -- a BT stream holds it; after STOP the menu time
    r = apo_test(rig, 60)
    rig.site(0x400AA874)
    polled(rig, psyms, 10_000)
    rig.bt_line("CONNECT 0 A2DP 19")
    emu.write_bytes(A2DP, b"\x01")
    rig.bt_line("A2DP STREAMING START 0")
    rig.tick()
    t_h = polled(rig, psyms, 3 * 60 * 1000)
    held = emu.read_u32(APO_COUNTER)
    r_h = apo_test(rig, 20)                  # asked while held: flags bit 0, the hold still stops the expiry
    t_h2 = polled(rig, psyms, 60_000)
    rig.bt_line("A2DP STREAMING STOP 0")
    rig.tick()
    t_s = polled(rig, psyms, 20 * 60 * 1000, step_ms=30_000)
    ok &= check("15 a BT stream started 10 s into a 60 s countdown holds it (3 min polling: ON, the counter rests at the "
                "limit); 19 04 while held -> rc 0 flags 1 and the hold wins; after STOP the APO runs out at the menu time "
                "15 min (+- 30 s)",
                r is not None and r[1] == 0 and t_h is None and held == LIMIT_15 and r_h is not None and r_h[1] == 0
                and r_h[6] == 1 and t_h2 is None and t_s is not None and 15 * 60 * 1000 - 30_000 <= t_s <= 15 * 60 * 1000 + 30_000,
                f"held {held} r_h {r_h and r_h.hex()} standby at {t_s}")
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x02), OP_STANDBY)
    rig.tick(15)
    # -- APO Off (menu "Never"): rc 7, the counter left alone
    emu.write_bytes(APO_SETTING, b"\x00")
    r_off = apo_test(rig, 60)
    rig.site(0x400AA874)
    c_off = emu.read_u32(APO_COUNTER)
    emu.write_bytes(APO_SETTING, b"\x01")
    r_bad = apo_test(rig, 5)
    ok &= check("15 APO Off (0x4139E7F9 = 0): 19 04 -> rc 7 (setting 0), the counter not moved; 5 s -> rc 1",
                r_off is not None and r_off[1] == 7 and r_off[5] == 0 and c_off < 100 and r_bad is not None and r_bad[1] == 1,
                f"{r_off and r_off.hex()} c {c_off} {r_bad and r_bad.hex()}")
    mine = [w_ for w_ in pn.writes[w0:] if RUN <= w_.pc < RUN + 0x20000]
    stockw = sorted({w_.addr for w_ in mine if 0x41000000 <= w_.addr < 0x41D00000} - {APO_COUNTER})
    menu = [w_ for w_ in pn.writes[w0:] if w_.addr == APO_SETTING and RUN <= w_.pc < RUN + 0x20000]
    ok &= check("15 RAM only: of the stock globals payload code stored only the counter 0x4139E7E4 (never the menu value "
                "0x4139E7F9) and no other power global, no NOR op in the whole check", not menu and len(pn.nor_calls) == n0
                and all(not (0x4139E7E0 <= a_ < 0x4139E800) for a_ in stockw),
                f"{[hex(a_) for a_ in stockw][:12]} nor {len(pn.nor_calls) - n0}")
    rig.close()
    return ok


def apo_test_same_pass_checks(image, syms, data, psyms):
    """Check 15b (ota_main): the engine loop answers 19 04 and the real 20 ms site moves the counter
    while task 19 is still in the pump pass that carried the 19 04 frame; a poll later in that pass resets the counter
    after the move.  The pump tail keeps the move (the pass's only activity frame was the request itself); a real set in
    that position still gives the full menu time back."""
    ok = True
    rig = Rig(image, syms)
    emu = rig.emu
    upload_apply(rig, data)
    emu.write_bytes(BINDING, b"\x03")
    rig.tick(3)
    pump_tail(rig)
    frame = TAG7D + bytes([0x19, 0x04, 0x00, 0x3C, 0xF7])
    for _ in range(25):                       # 50 s connected, polled: the counter counts from the last activity
        parse(rig, 2, POLL_1A)
        pump_tail(rig)
        ticks(rig, psyms, 100)
        rig.tick(1, ms=2000)
        rig.pn.sent.clear()
    before = emu.read_u32(APO_COUNTER)
    parse(rig, 2, frame)                      # the 19 04 frame ends; the pass goes on ...
    rig.tick()                                # ... task 24 answers it ...
    r = [m_[7:-1] for _p, m_ in rig.pn.sent if m_[:7] == TAG7D + b"\x59" and m_[7:8] == b"\x04"]
    rig.pn.sent.clear()
    rig.site(0x400AA874)                      # ... the real 20 ms site moves the counter ...
    moved = emu.read_u32(APO_COUNTER)
    parse(rig, 2, POLL_1A)                    # ... and a poll in the same pass resets it
    reset = emu.read_u32(APO_COUNTER)
    pump_tail(rig)
    kept = emu.read_u32(APO_COUNTER)
    t = polled(rig, psyms, 5 * 60 * 1000)
    ok &= check("15b 19 04 00 3C answered and moved (real site) inside its own pump pass, a 1A poll later in that pass resets "
                "the counter to 1: the pump tail puts the move back (limit - counter <= 3000), an app polling 1D 04 every 2 s "
                "-> standby at 60 s (+- one 2 s step)",
                before > 2000 and len(r) == 1 and r[0][1] == 0 and moved == LIMIT_15 - 3000 + 1 and reset == 1
                and LIMIT_15 - kept <= 3000 and t is not None and 58_000 <= t <= 62_000,
                f"before {before} r {r and r[0].hex()} moved {moved} reset {reset} kept {kept} standby at {t}")
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x02), OP_STANDBY)
    rig.tick(15)
    parse(rig, 2, frame)
    rig.tick()
    rig.pn.sent.clear()
    rig.site(0x400AA874)
    parse(rig, 2, K_SET)                      # a real set after the move, same pass: activity wins
    pump_tail(rig)
    after_set = emu.read_u32(APO_COUNTER)
    t2 = polled(rig, psyms, 2 * 60 * 1000)
    ok &= check("15b the same with a fn 10 set in place of the poll: the set's reset stands (counter 1), no standby in 2 min",
                after_set == 1 and t2 is None and rig.diag()["state"] == SB["on"], f"set {after_set} standby at {t2}")
    rig.close()
    return ok


PC_LINES = ("RING 3 a2:83:83:83:13:23 19 A2DP", "RING 5 a2:83:83:83:13:23 19 A2DP", "RING 4 a2:83:83:83:13:23 17 AVRCP")


def ps_line(rig, kind, word):
    """`AVRCP 4 REGISTER_NOTIFICATION_RSP <kind> PLAYBACK_STATUS_CHANGED <word>` (RM 6.10.2); after a CHANGED the source
    manager registers again (AN986 4.3) and the INTERIM answer follows 0.4 s later, as a real target answers"""
    rig.bt_line(f"AVRCP 4 REGISTER_NOTIFICATION_RSP {kind} PLAYBACK_STATUS_CHANGED {word}")
    rig.tick(50)
    if kind == "CHANGED":
        rig.bt_line(f"AVRCP 4 REGISTER_NOTIFICATION_RSP INTERIM PLAYBACK_STATUS_CHANGED {word}")
        rig.tick(5)


def play_status_checks(image, syms, data, psyms):
    """Check 16 (ota_main): the BT hold ends when the streaming source reports PAUSED / STOPPED."""
    ok = True
    rig = Rig(image, syms)
    emu = rig.emu
    upload_apply(rig, data)
    emu.write_bytes(BINDING, b"\x03")
    rig.tick(3)
    for ln in PC_LINES + ("A2DP STREAMING START 3",):
        rig.bt_line(ln)
    rig.tick(125)                                  # 1 s: the source manager registers the play status
    ps_line(rig, "INTERIM", "PLAYING")
    rig.tick(1, ms=60 * 60 * 1000)
    h_play = expire(rig)
    rig.tick()
    d_play = rig.diag()
    ps_line(rig, "CHANGED", "PAUSED")              # Windows: paused, no STREAMING STOP
    rig.site(0x400AA874)
    c_rel = emu.read_u32(APO_COUNTER)
    d_rel = rig.diag()
    t = polled(rig, psyms, 20 * 60 * 1000, step_ms=10_000)
    ok &= check("16 play status: the PC (A2DP 3, AVRCP 4) streams and reports INTERIM PLAYING -> after 60 "
                "virtual minutes the real 20 ms site still holds (counter at the limit, flag 0x08); CHANGED PAUSED with the "
                "stream left open (still streaming) -> the hold ends like a STOP: the next site resets the counter (1, +1 = "
                "2), flag 0x08 off; with an app polling -> standby 15 min (+- one 10 s step) after the pause",
                h_play == (45000, 0) and d_play["flags"] & 0x08 and d_play["state"] == SB["on"]
                and c_rel == 2 and not d_rel["flags"] & 0x08 and d_rel["bt"] == 2 and d_rel["state"] == SB["on"]
                and t is not None and 890_000 <= t <= 910_000,
                f"held {h_play} {d_play['flags']:#x} rel {c_rel} {d_rel['flags']:#x} bt {d_rel['bt']} standby at {t}")
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x02), OP_STANDBY)
    rig.tick(15)
    # -- PLAYING again with the stream still open: a new window (max hold 30 min)
    r = rig.raw_reply(2, rp.req(OP_POLICY, 0x01, *pack7(policy_raw(hold_min=30))), OP_POLICY)
    ps_line(rig, "CHANGED", "PLAYING")
    rig.tick(1, ms=20 * 60 * 1000)
    ps_line(rig, "CHANGED", "PAUSED")              # paused at 20 min
    rig.site(0x400AA874)
    rig.tick(1, ms=10 * 60 * 1000)
    rig.site(0x400AA874)
    d_p = rig.diag()
    ps_line(rig, "CHANGED", "PLAYING")             # played again at 30 min, no new START
    rig.tick(1, ms=29 * 60 * 1000)
    h59 = expire(rig)
    rig.tick()
    d59 = rig.diag()
    rig.tick(1, ms=2 * 60 * 1000)
    h61 = expire(rig)
    rig.tick()
    d61 = rig.diag()
    ok &= check("16 play status: max hold 30 min; PLAYING, paused at 20 min (10 min: not held, still ON), PLAYING again at "
                "30 min with the stream still open -> a new window: at 59 min the site still holds (flag 0x08, not 0x10), at "
                "61 min it is used up (0x10) and the idle piano goes to standby right away",
                r is not None and r[1] == 0 and not d_p["flags"] & 0x08 and d_p["state"] == SB["on"]
                and h59 == (45000, 0) and d59["flags"] & 0x18 == 0x08 and d59["state"] == SB["on"]
                and d61["flags"] & 0x10 and h61 == (2, 0) and d61["state"] == SB["standby"],
                f"{r and r.hex()} paused {d_p['flags']:#x} 59 {h59} {d59['flags']:#x} 61 {h61} {d61['flags']:#x} "
                f"{d61['state']}")
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x02), OP_STANDBY)
    rig.tick(15)
    # -- no play status: REJ -> the stream alone holds, up to the cap
    rig.bt_line("A2DP STREAMING STOP 3")
    rig.bt_line("A2DP STREAMING START 3")
    rig.tick(5)
    rig.bt_line("AVRCP 4 REGISTER_NOTIFICATION_RSP REJ")
    rig.tick(5)
    rig.tick(1, ms=29 * 60 * 1000)
    hr = expire(rig)
    rig.tick()
    dr = rig.diag()
    rig.tick(1, ms=2 * 60 * 1000)
    hc = expire(rig)
    rig.tick()
    dc = rig.diag()
    ok &= check("16 play status not known (registration REJ): the stream holds as before - at 29 min held (0x08), at 31 min "
                "the cap (0x10) -> standby",
                hr == (45000, 0) and dr["flags"] & 0x18 == 0x08 and dc["flags"] & 0x10 and hc == (2, 0)
                and dc["state"] == SB["standby"],
                f"29 {hr} {dr['flags']:#x} 31 {hc} {dc['flags']:#x} {dc['state']}")
    rig.close()
    return ok


PRESS, BOOT_DONE = 0x4139E7F4, 0x413A92F8
BTN_SITE = 0x400AA874


def button_press(rig, ms, real_site=True):
    """Hold GP3[1] low for ms (20 ms sites, an engine-loop pass after each), release, two more sites.  Returns
    (highest counter left by a site, state byte after the release)."""
    nor, emu = rig.pn.nor, rig.emu

    def one_site():
        if real_site:
            rig.site(BTN_SITE)               # stub -> pwc_apo_timer -> FUN_400ABFE8
        else:
            emu.call(0x400ABFE8)             # the stock body alone: no hook (control)

    top = 0
    nor.in_data23 &= ~(1 << 17)
    for _ in range(ms // 20):
        one_site()
        top = max(top, emu.read_u32(PRESS))
        rig.tick(1, ms=20)
    nor.in_data23 |= 1 << 17
    for _ in range(2):
        if emu.read_bytes(POWER_REQ, 1)[0] == 2:
            break                            # the 2 s watchdog 0x4139E7E8 runs from here: stop before its GPIO cut
        one_site()
        rig.tick(1, ms=20)
    return top, emu.read_bytes(POWER_REQ, 1)[0]


def button_rig(image, syms, data):
    rig = Rig(image, syms)
    upload_apply(rig, data)
    rig.emu.write_bytes(BINDING, b"\x03")
    rig.emu.write_u32(BOOT_DONE, 1)          # FUN_40143AF4 == 1: past the boot (else the release cuts the GPIO at once)
    rig.tick(3)
    return rig


def button_checks(image, syms, data, plen):
    """Check 17 (F-A1): the POWER switch through the real 20 ms site."""
    ok = True
    rig = button_rig(image, syms, data)
    pn, emu = rig.pn, rig.emu
    pn.writes.clear()
    nops = len(pn.nor.ops)
    d0 = rig.diag()
    t02 = button_press(rig, 200)
    rig.tick(2)
    d1 = rig.diag()
    cl0 = rig.calls[CLEANUP]
    t15 = button_press(rig, 1500)
    rig.tick(2)
    d2 = rig.diag()
    amp2 = rig.amp_bits()
    ok &= check("17 F-A1 from ON: 0.2 s -> nothing (still ON, state byte 0); 1.5 s -> the stock counter never above 15 "
                "(the hook holds 14, the body's +1 gives 15), no state byte 1 / 2, the engine loop enters standby "
                "(cleanup once, amp off + muted); diag 48 bit 0x08 armed",
                d0["raw"][48] & 0x08 and t02[0] <= 15 and t02[1] == 0 and d1["state"] == SB["on"]
                and t15 == (15, 0) and d2["state"] == SB["standby"] and rig.calls[CLEANUP] == cl0 + 1 and amp2 == 0,
                f"02 {t02} {d1['state']} 15 {t15} {d2['state']} cl {rig.calls[CLEANUP] - cl0} amp {amp2:#x}")
    tap = button_press(rig, 200)
    rig.tick(15)
    d3 = rig.diag()
    amp3 = rig.amp_bits()
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x01), OP_STANDBY)
    rig.tick(2)
    s4 = rig.diag()["state"]
    t4 = button_press(rig, 1500)
    rig.tick(15)
    d4 = rig.diag()
    ok &= check("17 F-A1 in standby: a 0.2 s tap wakes (cause 3 power button, amp back); 19 01 again, then 1.5 s -> woken "
                "at the press, the release does nothing more (ON, no state byte, no second standby)",
                tap[1] == 0 and d3["state"] == SB["on"] and d3["wake"] == 3 and amp3 == 0x8800
                and s4 == SB["standby"] and t4 == (15, 0) and d4["state"] == SB["on"] and d4["wake"] == 3,
                f"tap {tap} {d3['state']} w{d3['wake']} amp {amp3:#x} | {s4} {t4} {d4['state']}")
    t5 = button_press(rig, 5000)
    ok &= check("17 F-A1 from ON: held 5 s -> the clamp lets go at 4 s, the counter passes 15 (state byte 1), the REAL "
                "release block writes state byte 2 = the stock real off (the escape hatch)",
                t5[0] > 15 and t5[1] == 2, str(t5))
    mine = [w_ for w_ in pn.writes if RUN <= w_.pc < RUN + plen]
    stockw = {w_.addr for w_ in mine if 0x41000000 <= w_.addr < 0x41D00000}
    pv = {w_.value for w_ in mine if w_.addr == PRESS}
    ok &= check("17 stores by payload code in these button runs: of the stock globals only 0x4139E7F4 (value always 14) "
                "besides the analysed counter / state byte / GP3[8] event; no NOR op",
                PRESS in stockw and stockw <= {APO_COUNTER, POWER_REQ, GP3_EVENT, PRESS} and pv == {14}
                and len(pn.nor.ops) == nops, f"{sorted(hex(a) for a in stockw)} values {pv} nor {len(pn.nor.ops) - nops}")
    rig.close()
    rig = button_rig(image, syms, data)
    rig.raw_reply(2, rp.req(OP_STANDBY, 0x01), OP_STANDBY)
    rig.tick(2)
    t6 = button_press(rig, 5000)
    w6 = rig.diag()
    ok &= check("17 F-A1 from standby: held 5 s -> woken at the press, then the stock real off at the release (state byte 2)",
                t6[0] > 15 and t6[1] == 2 and w6["wake"] == 3, f"{t6} w{w6['wake']}")
    rig.close()
    rig = button_rig(image, syms, data)
    r = rig.raw_reply(2, rp.req(OP_POLICY, 0x01, *pack7(bytes([1, 0x09, 0x2C, 0x01, 0xE0, 0x01, 0, 0]))), OP_POLICY)
    rig.tick(2)
    d7 = rig.diag()
    t7 = button_press(rig, 1500)
    ok &= check("17 policy bit3 (the stock button, op 18 01 flags 0x09): diag 48 bit 0x08 clear, 1.5 s from ON -> the "
                "stock real off (state byte 2), no standby",
                r is not None and r[1] == 0 and not d7["raw"][48] & 0x08 and t7[0] > 15 and t7[1] == 2,
                f"{r and r.hex()} {d7['raw'][48]:#x} {t7}")
    rig.close()
    rig = Rig(image, syms)
    rig.pn.boot()
    rig.emu.write_u32(BOOT_DONE, 1)
    rig.tick(3)
    t8 = button_press(rig, 1500, real_site=False)
    ok &= check("17 control: no payload applied, FUN_400ABFE8 alone - 1.5 s from ON -> state byte 2 (the stock off T2 saw)",
                t8[0] > 15 and t8[1] == 2, str(t8))
    rig.close()
    return ok


def main(argv):
    which = rtlib.first_arg(argv, "ota_main")
    variant, feat1, kawai_stub, extra_calls, extra_bind = VARIANTS[which]
    tv = bool(feat1 & 0x40)
    # the max-hold rule (owner decision 2026-11-02): only the live payload ota_main; the frozen ones keep theirs
    rule9 = bool(next(x for x in images.OTA_PAYLOADS.values() if x.name == which).feat2 & images.FEAT2_PWROFF)
    r_default = R_GET_DEFAULT_5H if rule9 else R_GET_DEFAULT
    session = bool(next(x for x in images.OTA_PAYLOADS.values() if x.name == which).feat2 & images.FEAT2_SESSION)
    if session:
        extra_calls = {**extra_calls, **{a: n for n, a in SESSION_CALLS.items()}}
        extra_bind = {**extra_bind, **SESSION_CALLS}
    if next(x for x in images.OTA_PAYLOADS.values() if x.name == which).feat2 & images.FEAT2_REC:
        extra_calls = {**extra_calls, **{a: n for n, a in REC_CALLS.items()}}
        extra_bind = {**extra_bind, **REC_CALLS}
    midich = bool(next(x for x in images.OTA_PAYLOADS.values() if x.name == which).feat3 & images.FEAT3_MIDICH)
    if midich:
        extra_calls = {**extra_calls, **{a: n for n, a in MIDICH_CALLS.items()}}
        extra_bind = {**extra_bind, **MIDICH_CALLS}
    if next(x for x in images.OTA_PAYLOADS.values() if x.name == which).feat3 & images.FEAT3_GATE:
        extra_calls = {**extra_calls, **{a: n for n, a in GATE_CALLS.items()}}
        extra_bind = {**extra_bind, **GATE_CALLS}
    STUBS = {**globals()["STUBS"], 0x40147E94: kawai_stub}
    BINDINGS = {**globals()["BINDINGS"], **extra_bind}
    resident = next((a.split("=", 1)[1] for a in argv[1:] if a.startswith("--loader=")), rtlib.DEFAULT_LOADER)
    img, stock, manifest, image, syms = rtlib.loader(resident)
    p, data, pay, plen, pelf, psyms = rtlib.payload(which)
    assert p.variant == variant
    nm = rt.mm.NM
    ok = True
    print(f"payload {which}  {plen} B  sha256 {pa.sha256(data)}  on {img.name}")

    # ---- 1. static ------------------------------------------------------------------------------
    feat = le(pay, 0x10)
    ok &= check(f"1 {which}.c79 verifies (header, CRC, HMAC); entry C79E, ABI 1, feat {feat1:#04x}3B + OTA_EF_ATTACH, "
                "attach() and quiesce inside the payload",
                not op.verify(data, ro.KEY) and pay[:4] == b"C79E" and le(pay, 4) == 1
                and feat == (p.feat0 | p.feat1 << 8 | 0x10000) and p.feat1 == feat1
                and RUN <= le(pay, 0x18) < RUN + plen and RUN <= le(pay, 0x14) < RUN + plen, f"feat {feat:#x}")
    bad = {n: hex(psyms.get(n, 0)) for n, a in BINDINGS.items() if psyms.get(n) != a}
    ok &= check(f"1 stock_power.ld: all {len(BINDINGS)} bindings at their documented addresses", not bad, str(bad))
    insx = rtlib.instructions_x(pelf)
    ins = [(a, m, o) for a, m, o, _t in insx]
    nmS = subprocess.run([str(nm), "-S", str(pelf)], capture_output=True, text=True, check=True).stdout
    size = {q[3]: (int(q[0], 16), int(q[0], 16) + int(q[1], 16)) for q in (l.split() for l in nmS.splitlines()) if len(q) == 4}

    def body(name):
        lo, hi = size[name]
        return [(a, m, o) for a, m, o in ins if lo <= a < hi]

    stubs = []
    for site, word, target, _ctx, _n in SITES:
        b = body(STUBS[site])
        last = b[-1]
        # a Thumb payload's ARM stub reaches its C body by `blx <imm>` (ARMv5 interworking call, the same as `bl`);
        # a register blx (computed target) is still refused
        stubs.append(last[1] == "b" and int(last[2].split()[0], 16) == target == rh.bl_target(site, word)
                     and not any(m in ("svc", "mcr", "msr") or (m == "blx" and not re.match(r"[0-9a-f]+\b", o))
                                 for _a, m, o in b))
    ok &= check("1 the four stubs end in `b` = the stock target of their site (FUN_4014A2C8 / FUN_400ACFA0 / FUN_400ACD28 / "
                "FUN_400ABFE8); no svc / register blx / privileged op", all(stubs), str(stubs))
    call_rx = re.compile(r"(bl|blx|svc)(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?$")   # not b<cond> (bls / blt)
    leaf = {n: [(hex(a), m) for a, m, _o in body(n) if call_rx.match(m) or (m.startswith("bx") and _o != "lr")]
            for n in ("pwc_apo_timer", "pwc_kawai_peek", "pwc_hp_pass", "pwc_midi_tail", "pwc_frame_7d") if n in size}
    ok &= check(f"1 kernel-timer body pwc_apo_timer and task-19 bodies {', '.join(n for n in leaf if n != 'pwc_apo_timer')} are "
                "leaves (loads / stores only: nothing that can block, no payload frame left behind)",
                not any(leaf.values()) and (not session or "pwc_midi_tail" in leaf), str(leaf))
    if session:   # the pump-tail stub runs pwc_midi_tail first, then the session core's ssc_pump
        tb = [(m, o) for _a, m, o in body("hk_ss_midi") if m in ("bl", "blx")]   # blx <imm>: Thumb C body
        ok &= check("1 hk_ss_midi calls pwc_midi_tail, then ssc_pump, then `b` to the stock FUN_4012A7D4",
                    len(tb) == 2 and int(tb[0][1].split()[0], 16) == psyms["pwc_midi_tail"]
                    and int(tb[1][1].split()[0], 16) == psyms["ssc_pump"], str(tb))
    targets, nor_lits, _ind = rp.static_calls(pelf, RUN, RUN + plen)
    allowed = {**rp.ALLOWED_CALLS, 0x400A8340: "stk_time_us", 0x400AA030: "stk_flash_copy", 0x4013427C: "stk_flash_busy",
               ro.DIS: "dis_dsp", ro.ENA: "ena_dsp", ro.DLY: "dly_tsk", 0x401342AC: "stk_flash_jobs"}
    allowed.update({a: n for n, a in BINDINGS.items() if a < 0x40180000 and a >= 0x40060000 and not n.startswith("stk_apo_l")})
    allowed.update(extra_calls)
    if p.feat3 & images.FEAT3_XGATE:   # the executor gate's two stubs tail-call the v3 executors (roundtrip_xgate.py)
        allowed.update({0x40127594: "stk_hk_media", 0x40123E34: "stk_hk_xport"})
    fread = "hk_fr_file" in psyms   # the fn 0x24 Read job (fread_cave.c, stock_fread.ld; roundtrip_fread.py), v3 site #15
    if fread:
        allowed.update({0x4014F364: "stk_hk_file", 0x400A9238: "stk_fsize", 0x400A9408: "stk_fread",
                        0x40086E94: "stk_wai_sem", 0x4008819C: "stk_sig_sem", 0x4014B6E0: "stk_fa_top",
                        0x4014D7E0: "stk_fa_build", 0x4014B638: "stk_fa_send", 0x4014D664: "stk_fa_cancel",
                        0x4014B7D8: "stk_fa_end", 0x4014F294: "stk_fa_result_ok", 0x4014DC0C: "stk_fa_result",
                        # + the sender with a Cancel test and the upload receiver
                        0x4014B4C0: "stk_fa_flags_clear", 0x4014AD04: "stk_fa_tx", 0x4014B4E0: "stk_fa_wait",
                        0x4014FAF4: "stk_fa_unpack", 0x4014B31C: "stk_fa_xor", 0x4014B8A0: "stk_fa_ack",
                        0x4014B778: "stk_fa_nak", 0x4014B55C: "stk_unbind", 0x4014B5A0: "stk_reset_a",
                        0x4014B5D0: "stk_reset_b", 0x4014B610: "stk_reset_c"})
    badt = {hex(t): v for t, v in targets.items() if t not in allowed}
    ok &= check("1 branches out of the payload only to the ota_core_v2 stock set + the power bindings + copier / busy / "
                "dis / ena / dly + the four hook targets (no stock NOR writer)", not badt, str(badt))
    wrefs = rtlib.load("build_images").writer_refs(pelf)
    ok &= check("1 no stock NOR writer / busy flag setter anywhere in the payload (no branch, no literal): its kvstore "
                "writes go only through the loader's guarded service bound in attach() (pwc_nor_bind, svc version 3)",
                not wrefs and "pwc_nor_bind" in psyms, str(wrefs[:3]))
    # literals actually loaded by code (`ldr rN, [pc, #imm]`): strings / tables in .rodata are not addresses
    loaded = []
    for a_, m_, o_, t_ in insx:
        mm_ = re.match(r"ldr\w*$", m_) and re.match(r"r\d+, \[pc, #(-?\d+)\]", o_)
        if mm_:
            at = rtlib.pc_literal(a_, int(mm_[1]), t_)
            if RUN <= at < RUN + plen:
                loaded.append((a_, le(pay, at - RUN)))
    norl = [(hex(a_), hex(w_)) for a_, w_ in loaded if 0x60000000 <= w_ < 0x62000000]
    # KV_HI itself = the region's exclusive end, a bound compared against (`len <= KV_HI - addr`): Thumb-1 has no
    # rotated immediates, so the Thumb payload loads it from a literal where the ARM build used `cmp rN, #0x60500000`
    badl = [x for x in norl if not KV_LO <= int(x[1], 16) <= KV_HI]
    ok &= check(f"1 every NOR address loaded by payload code ({len(norl)} pc-relative literals of {len(loaded)}) is inside "
                "unused_3 (0x604C0000-0x604FFFFF)", norl and not badl, str(badl or norl))

    # bindings do what the analysis says (fresh emulator, real stock routines)
    rig = Rig(image, syms)
    emu = rig.emu
    g = rig.pn.nor
    beh = []
    g.gpio67 = 0x8800 | 0x123
    emu.call(BINDINGS["stk_amp_off"]); beh.append(g.gpio67 == 0x0800 | 0x123)
    emu.call(BINDINGS["stk_mute"]); beh.append(g.gpio67 == 0x123)
    emu.call(BINDINGS["stk_amp_on"]); beh.append(g.gpio67 == 0x8000 | 0x123)
    emu.call(BINDINGS["stk_unmute"]); beh.append(g.gpio67 == 0x8800 | 0x123)
    emu.periph[IN_DATA45] = 1 << 30
    emu.call(BINDINGS["stk_hp_apply"])
    beh.append(emu.read_bytes(HP_VAR, 1) == b"\x01" and emu.call(BINDINGS["stk_hp_present"]).r0 == 1
               and rig.calls[HP_OFF_PATH] == 1 and rig.calls[HP_ON_PATH] == 0)
    emu.periph[IN_DATA45] = 0
    emu.call(BINDINGS["stk_hp_apply"])
    beh.append(emu.read_bytes(HP_VAR, 1) == b"\x00" and emu.call(BINDINGS["stk_hp_present"]).r0 == 0
               and rig.calls[HP_ON_PATH] == 1 and g.gpio67 & 0x8800 == 0x8800)
    emu.call(BINDINGS["stk_bt_audio_off"]); beh.append(emu.read_u32(BT_POWER) == 0)
    emu.call(BINDINGS["stk_bt_audio_on"]); beh.append(emu.read_u32(BT_POWER) == 1)
    emu.write_bytes(A2DP, b"\x01")
    emu.call(BINDINGS["stk_bt_flags_clear"]); beh.append(emu.read_bytes(A2DP, 1) == b"\x00")
    if tv:   # FUN_40145C00: write index, flag, read index = 0 (and nothing else of the ring)
        emu.write_bytes(BT_RD, (5).to_bytes(2, "little")); emu.write_bytes(BT_WR, (5).to_bytes(2, "little"))
        emu.write_bytes(BT_FLAG, b"\x80"); emu.write_bytes(BT_RING, b"AB")
        res_ = emu.call(BINDINGS["stk_bt_rx_reset"])
        beh.append(emu.read_u32(BT_RD) & 0xFFFFFFFF == 0 and emu.read_bytes(BT_FLAG, 1) == b"\x00"
                   and emu.read_bytes(BT_RING, 2) == b"AB"
                   and sorted({w_.addr for w_ in res_.writes}) == [BT_RD, BT_WR, BT_FLAG])
    ok &= check("1 run on the image: amp_off / mute / amp_on / unmute change only OUT_DATA67 bit 15 / 11; hp_apply samples "
                "GP5[14] into 0x4139E7FA and takes the DSP-path half for that state (never the amp-on one with param 1); "
                "BT audio off / on drive 0x4139E7F0; flags_clear clears the A2DP flag"
                + ("; bt_rx_reset (FUN_40145C00) zeroes write index, flag, read index only" if tv else ""),
                all(beh), str(beh))
    rig.close()

    # ---- 2. applied live ------------------------------------------------------------------------
    rig = Rig(image, syms)
    pn, emu = rig.pn, rig.emu
    rc, st = upload_apply(rig, data)
    hk = rh.hooks(pn)
    w = rh.words(pn)
    want = {s[0]: rh.bl_word(s[0], s[1], psyms[STUBS[s[0]]]) for s in SITES}
    if session:   # the fifth site: the MIDI pump tail -> hk_ss_midi
        want[0x401539AC] = rh.bl_word(0x401539AC, 0xEBFF5B88, psyms["hk_ss_midi"])
    ok &= check(f"2 {which} applied live (slot A) -> attach() hooks the {'five' if session else 'four'} sites (bl -> its stub)"
                f"{'' if session else ', 0x401539AC stays stock'}; "
                "attach rc 0 x4", rc == 0 and st["ap_state"] == AP_DONE and st["active"] == ro.ACT_A
                and [w[i] == want.get(s[0], s[1]) for i, s in enumerate(rh.SITES)] == [True] * 5
                and list(emu.read_bytes(psyms["pw_attach_rc"], 4)) == [0, 0, 0, 0],
                f"{[hex(x) for x in w]} rc {list(emu.read_bytes(psyms['pw_attach_rc'], 4))}")
    id_ = rh.identify(pn)
    want_id = bytes([p.ngver, 2, 19, 0x3B, feat1]) + (bytes([p.feat2]) if p.feat2 else b"") + (bytes([p.feat3]) if p.feat3 else b"")
    # + the three MIDI channel sites on ota_loader_v4
    # + the fn 0x24 Read site 0x40143ACC on ota_loader_v4
    # + the two executor gate sites on ota_loader_v4 when the payload boots with the gate on
    n_hk = (5 if session else 4) + (3 if midich and resident in rtlib.ALLOW17 else 0) + (1 if fread and resident in rtlib.ALLOW17 else 0)
    n_hk += 2 if p.boot_on and p.feat3 & images.FEAT3_XGATE and resident in rtlib.ALLOW17 else 0
    ok &= check(f"2 identify {want_id.hex(' ').upper()} (feat1 0x10 = power{f', feat2 {p.feat2:#04x}' if p.feat2 else ''}"
                f"{f', feat3 {p.feat3:#04x}' if p.feat3 else ''}); op 0x16 reports {n_hk} hooked",
                id_ is not None and id_ == want_id and hk["hooked"] == n_hk, f"{id_} {hk and hk['hooked']}")

    # ---- 3. policy op, kvstore ------------------------------------------------------------------
    emu.write_bytes(BINDING, b"\x00")
    r = rig.raw_reply(2, rp.req(OP_POLICY, 0x00), OP_POLICY)
    ok &= check("3 18 00 -> 58 00 00 <default policy> byte-equal to the host core (ngext.c)",
                r is not None and bytes([0x58]) + r == r_default, r.hex() if r else "none")
    want_pol = policy_raw(apo_standby=True, hold_min=60, cap_min=30)
    nc = len(pn.nor_calls)
    r = rig.raw_reply(2, rp.req(OP_POLICY, 0x01, *pack7(want_pol)), OP_POLICY)
    ok &= check("3 18 01 from an unbound port -> rc 2, nothing written", r is not None and r[:2] == b"\x01\x02"
                and len(pn.nor_calls) == nc)
    emu.write_bytes(BINDING, b"\x03")                # the app on BLE (stock port 2) is bound
    emu.write_bytes(0x40FF0000, b"\xCD" * 0x10000)
    n0 = len(pn.nor_calls)
    r = rig.raw_reply(2, rp.req(OP_POLICY, 0x01, *pack7(want_pol)), OP_POLICY)
    m = emu.read_bytes(0x40FF0000, 0x10000)
    depth = 0x10000 - next(j for j, b_ in enumerate(m) if b_ != 0xCD)
    calls = pn.nor_calls[n0:]
    kv = rig.diag()
    ok &= check("3 bound 18 01 -> rc 0, the reply carries the new policy; kvstore mounted in unused_3 (generation 1)",
                r is not None and r[:2] == b"\x01\x00" and rp.unpack7(r[2:]) == want_pol and kv["kv"] == 2 and kv["gen"] == 1,
                f"{r.hex() if r else None} kv {kv and kv['kv']}")
    ok &= check("3 blank unused_3: no erase; every NOR write (write buffer only) inside unused_3, with dispatch disabled; "
                "the NOR model saw no out-of-range op",
                calls and all(c[0] == "buf" and c[2] == 1 and KV_LO <= c[3] < KV_HI for c in calls),
                f"{[(c[0], hex(c[3]), c[2]) for c in calls[:4]]} ({len(calls)})")
    ok &= check(f"3 deepest stack of that engine-loop pass (mount + format + put): {depth} B <= 3072 (task 24 stack 0x1000)",
                depth <= 3072, f"{depth} B")
    nor_saved = bytes(pn.nor.mem)
    rig.close()
    rig2 = Rig(image, syms, nor_mem=nor_saved)
    st2, _ = rig2.pn.boot()
    rig2.tick(3)
    r = rig2.raw_reply(2, rp.req(OP_POLICY, 0x00), OP_POLICY)
    ok &= check("3 power-on again on the same NOR (slot A boots, attach) -> 18 00 answers the saved policy",
                st2["active"] == ro.ACT_A and r is not None and rp.unpack7(r[2:]) == want_pol and rh.words(rig2.pn)[0] != rh.SITES[0][1],
                r.hex() if r else "none")
    rig2.close()

    # ---- 4. APO expiry through the real 20 ms site -> standby -------------------------------------
    rig = Rig(image, syms)
    pn, emu = rig.pn, rig.emu
    upload_apply(rig, data)
    emu.write_bytes(BINDING, b"\x03")
    rig.tick(2)
    pn.writes.clear()
    bt_offs = []
    emu.hook(BT_OFF, lambda e: (bt_offs.append(1), rt.se.StockEmu.CONTINUE)[1])
    emu.write_u32(APO_COUNTER, 45000)
    rig.site(0x400AA874)
    after = (emu.read_u32(APO_COUNTER), emu.read_bytes(POWER_REQ, 1)[0])
    d0 = dict(expiries=emu.read_u32(psyms["apo_expiries"]))
    pn.sent.clear()
    rig.tick()
    sent = list(pn.sent)
    d1 = rig.diag()
    ok &= check("4 counter 45000 + the 20 ms site (stub -> pwc_apo_timer -> FUN_400ABFE8): no stock state byte 2, the "
                "counter back at 2, one expiry for the engine loop", after == (2, 0) and d0["expiries"] == 1, f"{after} {d0 and d0['expiries']}")
    note = [m_ for p_, m_ in sent if m_[:7] == b"\xF0\x7D\x43\x41\x37\x39\x59"]
    emu.unhook(BT_OFF)
    ok &= check("4 next engine-loop pass: standby (cleanup once, amp off + muted, "
                + ("BT audio module kept ON - FUN_400AD030 never entered (TV wake)" if tv else "BT audio module off")
                + "), notice F0 7D C A 7 9 59 7F 00 01 F7 on the app port 2 only",
                d1["state"] == SB["standby"] and rig.calls[CLEANUP] == 1 and rig.amp_bits() == 0
                and emu.read_u32(BT_POWER) == (1 if tv else 0) and len(bt_offs) == (0 if tv else 1)
                and len(sent) == 1 and sent[0][0] == 2
                and note == [bytes.fromhex("F07D4341373959 7F0001F7".replace(" ", ""))], f"{d1 and d1['state']} {sent}")
    for _ in range(5):
        emu.write_u32(APO_COUNTER, 45000)
        rig.site(0x400AA874)
    ok &= check("4 in standby the site holds the counter (1 -> 2), never an expiry, never state byte 2",
                emu.read_u32(APO_COUNTER) == 2 and emu.read_bytes(POWER_REQ, 1)[0] == 0 and rig.diag()["expiries"] == 1)
    ctrl = Rig(image, syms)
    ctrl.pn.boot()
    ctrl.emu.write_u32(APO_COUNTER, 45000)
    ctrl.site(0x400AA874)
    ok &= check("4 control: the same tick on the stock word (built-in cave, no hook) writes state byte 2 (stock APO expiry)",
                ctrl.emu.read_bytes(POWER_REQ, 1)[0] == 2)

    # ---- 6. standby: re-light blocked, panel touch wakes ------------------------------------------
    emu.write_bytes(HP_VAR, b"\x01")               # stock believes headphones were in ...
    emu.periph[IN_DATA45] = 0                       # ... and now they are out: the poll would re-light the amp
    entered = []
    emu.hook(0x400ACFA0, lambda e: (entered.append(1), rt.se.StockEmu.CONTINUE)[1])
    rig.site(0x40143AB4)
    emu.unhook(0x400ACFA0)
    emu.write_bytes(GP3_EVENT, b"\x01")             # GP3[8] alarm cleared -> event 1 = amp on + unmute
    rig.site(0x40143AB8)
    dd = rig.diag()
    ok &= check("6 in standby: the headphone-poll site returns without entering FUN_400ACFA0; the executor site runs the "
                "executor with the GP3[8] event dropped; amp stays off and muted",
                not entered and rig.amp_bits() == 0 and emu.read_bytes(GP3_EVENT, 1) == b"\x00"
                and dd["hp_skips"] >= 1 and dd["exec_clears"] == 1, f"{entered} {rig.amp_bits():#x} {dd and dd['hp_skips']}")
    ctrl.emu.write_bytes(GP3_EVENT, b"\x01")
    ctrl.pn.nor.gpio67 &= ~0x8800
    ctrl.site(0x40143AB8)
    ok &= check("6 control: on the stock word the executor takes the same event and re-lights amp + unmute",
                ctrl.amp_bits() == 0x8800)
    ctrl.close()
    emu.write_bytes(HP_VAR, b"\x00")
    seen = []
    emu.hook(0x4014A2C8, lambda e: (seen.append([e.reg(i) for i in range(16)]), rt.se.StockEmu.CONTINUE)[1])
    other = deliver(rig, 3, bytes.fromhex("F040003208025100 7F02".replace(" ", "")))
    rig.tick()
    st_other = rig.diag()["state"]
    res = deliver(rig, 3, bytes.fromhex("F04000320802513 97F00".replace(" ", "")))
    emu.unhook(0x4014A2C8)
    rig.tick()
    dw = rig.diag()
    ok &= check("6 panel touch F0 40 00 32 08 02 51 39 7F 00 on stock port 3 through the real end handler: the Kawai site "
                "stub records it and FUN_4014A2C8 still runs (r0 3, lr 0x40147E98), the handler returns; another Kawai "
                "message did not wake; the next pass wakes (cause = panel touch)",
                res.stop == "returned" and len(seen) == 2 and seen[1][0] == 3 and seen[1][14] == 0x40147E98
                and st_other == SB["standby"] and dw["state"] == SB["waking"] and dw["wake"] == 2 and dw["touches"] == 1,
                f"{res.stop} {len(seen)} {st_other} {dw and (dw['state'], dw['wake'])}")
    rig.tick(15)
    dn = rig.diag()
    ok &= check("6 wake: headphones re-read (FUN_400ACC7C), unmute, 100 ms later amp on, BT audio back -> ON; the poll runs "
                "again", dn["state"] == SB["on"] and rig.amp_bits() == 0x8800 and emu.read_u32(BT_POWER) == 1
                and rig.calls[HP_ON_PATH] == 1, f"{dn and dn['state']} {rig.amp_bits():#x}")

    # ---- 5. BT streaming holds the counter --------------------------------------------------------
    rig.bt_line("CONNECT 0 A2DP 19")
    rig.bt_line("A2DP STREAMING START 0")
    rig.tick()
    for _ in range(3):
        emu.write_u32(APO_COUNTER, 45000)
        rig.site(0x400AA874)
    dh = rig.diag()
    c_held = emu.read_u32(APO_COUNTER)
    if rule9:   # the counter is not pinned, the site only keeps it from expiring (at the limit)
        ok &= check("5 BT lines in the stock UART1 ring (CONNECT, A2DP STREAMING START) -> streaming; an expired counter "
                    "rests at the limit 45000 through the site (no expiry, no state byte 2, not pinned: flag 0x08 without "
                    "0x02), still ON",
                    dh["bt"] == 2 and dh["flags"] & 0x0A == 0x08 and c_held == 45000 and dh["expiries"] == 1
                    and dh["state"] == SB["on"] and emu.read_bytes(POWER_REQ, 1)[0] == 0,
                    f"bt {dh and dh['bt']} flags {dh and dh['flags']:#x} c {c_held}")
    else:
        ok &= check("5 BT lines in the stock UART1 ring (CONNECT, A2DP STREAMING START) -> streaming, the site holds the counter "
                    "at 1 (-> 2 after the stock body), no expiry, still ON",
                    dh["bt"] == 2 and dh["flags"] & 0x0A == 0x0A and c_held == 2 and dh["expiries"] == 1
                    and dh["state"] == SB["on"], f"bt {dh and dh['bt']} flags {dh and dh['flags']:#x}")
    rig.bt_line("A2DP STREAMING STOP 0")
    rig.tick()
    rig.site(0x400AA874)                       # (ota_main: this tick takes the STOP reset)
    emu.write_u32(APO_COUNTER, 45000)
    rig.site(0x400AA874)
    ok &= check("5 STREAMING STOP -> the hold ends: the next expiry goes to the engine loop again",
                rig.diag()["expiries"] == 2)
    rig.tick()

    # ---- 7. standby cap -> the stock shutdown request -------------------------------------------
    r = rig.raw_reply(2, rp.req(OP_POLICY, 0x01, *pack7(policy_raw(cap_min=30))), OP_POLICY)
    rs = rig.raw_reply(2, rp.req(OP_STANDBY, 0x00), OP_STANDBY)   # standby since the expiry of check 5
    rig.tick(1, ms=29 * 60 * 1000)
    s29 = rig.diag()["state"]
    rig.tick(1, ms=2 * 60 * 1000)
    s31 = rig.diag()
    req = emu.read_bytes(POWER_REQ, 1)[0]
    emu.write_bytes(POWER_REQ, b"\x00")
    rig.site(0x400AA874)
    ok &= check("7 cap 30 min (op 18 01) while in standby (check 5's expiry); 29 min still standby; 31 min -> OFF and state byte 2 (the stock "
                "executor's request); the 20 ms site re-asserts it after an activity reset",
                r is not None and r[1] == 0 and rs is not None and rs[1:3] == bytes([0, SB["standby"]])
                and s29 == SB["standby"] and s31["state"] == SB["off"] and req == 2
                and emu.read_bytes(POWER_REQ, 1)[0] == 2, f"{rs and rs.hex()} {s29} {s31 and s31['state']} {req}")

    # ---- 8. stores ------------------------------------------------------------------------------
    data_lo = RUN
    okw = [(RUN, RUN + 0x20000), (0x40180000, 0x401A0000), ro.STACK, (0x40B90000, 0x40B90200),
           (APO_COUNTER, APO_COUNTER + 4), (POWER_REQ, POWER_REQ + 1), (GP3_EVENT, GP3_EVENT + 1)]
    if session:   # contract (b): the pump tail (ssc_pump) marks the resident's inflight flag around its sender
        okw.append((syms["ota_in_sysex"], syms["ota_in_sysex"] + 1))
    mine = [w_ for w_ in pn.writes if data_lo <= w_.pc < RUN + plen]
    strayw = [w_ for w_ in mine if not any(a <= w_.addr < b_ for a, b_ in okw)]
    stockw = sorted({w_.addr for w_ in mine if 0x41000000 <= w_.addr < 0x41D00000})
    ok &= check(f"8 {len(mine)} stores by payload code (checks 4-7): only payload .data, core RW, stack and the stock "
                "globals 0x4139E7E4 (counter), 0x4139E7FC (state byte), 0x4139E7FD (GP3[8] event)",
                mine and not strayw and set(stockw) <= {APO_COUNTER, POWER_REQ, GP3_EVENT},
                f"stray {strayw[:2]}" if strayw else str([hex(a) for a in stockw]))
    rig.close()

    # ---- 9. apply to the built-in while in standby ------------------------------------------------
    rig = Rig(image, syms)
    pn, emu = rig.pn, rig.emu
    upload_apply(rig, data)
    emu.write_bytes(BINDING, b"\x03")
    rig.tick()
    rs = rig.raw_reply(2, rp.req(OP_STANDBY, 0x01), OP_STANDBY)
    off_bits = rig.amp_bits()
    rh.apply(pn, T_BUILTIN)
    for _ in range(60):
        rig.tick()
        if pn.status()["ap_state"] in (AP_DONE, rh.AP_ERROR):
            break
    st = pn.status()
    ok &= check("9 apply-now to the built-in while in standby: quiesce wakes first (amp + unmute back), then the switch; every "
                "site word stock again, hook table empty",
                rs is not None and rs[1] == 0 and off_bits == 0 and st["ap_state"] == AP_DONE and st["active"] == ro.ACT_BUILTIN
                and rh.words(pn) == rh.STOCK_WORDS and rig.amp_bits() == 0x8800,
                f"enter {rs and rs.hex()} off {off_bits:#x} ap {st['ap_state']} act {st['active']} amp {rig.amp_bits():#x}")
    rig.close()

    if tv:
        ok &= tv_wake_checks(image, syms, data, psyms, plen, rule9)
    ok &= no_cap_checks(image, syms, data, which)
    if p.feat2 & images.FEAT2_PWROFF:
        ok &= main_checks(image, syms, data)
        ok &= quiet_checks(image, syms, data, psyms)
        ok &= apo_test_checks(image, syms, data, psyms)
        ok &= apo_test_same_pass_checks(image, syms, data, psyms)
        ok &= play_status_checks(image, syms, data, psyms)
        ok &= button_checks(image, syms, data, plen)

    return rtlib.finish(ok, " - emulation only: no interrupts, no RTOS scheduling, no caches; time = the stock timer base we set.")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
