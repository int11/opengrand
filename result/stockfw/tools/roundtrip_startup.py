# roundtrip_startup.py - startup settings owned by the core: the slot payload ota_main runs on the stock image with
# the stock boot settings, the real task-19 Kawai handler, the request task, the pump tail 0x401539AC every 2 ms and -
# new here - the engine loop (task 24: power port + kvstore over the NOR model) every 8 ms, so a snapshot written in
# one power-on is restored in the next one (the NOR model's bytes carried over, the slot payload booted by the loader).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_startup.py [ota_main] [--shard i/n]
#   (--shard: the i-th of n groups of the units power-on 1 / 2 / 3 - rtlib.Unit; 2 and 3 take power-on 1's NOR,
#   a shard without power-on 1 runs it first without printing its checks)
#
# Stock stand-ins: the UserMemory save job FUN_400A9990 and the recorder reset FUN_4016C908 are counted and return at
# once (the job itself is the stock deferred writer - its NOR ops are not modelled here).
# Checks (E numbers of the design table):
#   E1  stock 32 51 34 7F 07 00 7F 7F (the app's new "save startup settings" bytes): one UserMemory job, saved image ==
#       live; 07 7F: no job without the staging mark, one with it; 00 00 00 00 (the app's old bytes): nothing
#   E8  mode power-off (1C 01 02): a first P at once; 10 sets 1 s apart -> no write while they come, exactly one
#       append 3 s after the last (generation +1); a change then standby (19 01) -> P written before the 3 s
#   E10 a stock flash job active (0x4139DAAC): 1C 02 writes nothing to the NOR until the job ends, then once (rc 0)
#   E3  power-on again on that NOR, nobody bound, the staging flag up first (E11: restore waits, no set), then down:
#       restore by core-originated sets - no frame to anybody, remote-change flags 0x413CA3B0[0..3] unchanged (ctx 0),
#       BluetoothAudio byte 0x418CE0F8 = 1, 0 differences left; the app's full dump == the dump before the power-off
#       on every startup key; the OpenGrand panel (CONNECT + full request) causes no set (E7)
#   E4  power-on again, the app CONNECTs before the restore: it gets one fan-out frame per restored key, nothing else
#   E2 / E12  51 36 7F 07 7F 7F 7F: live unchanged, one job; 51 36 7F 04 7F 7F 7F: live == the defaults image, recorder
#       reset once; both clear C / P / mode on our side -> the next power-on restores no sound setting (stock); since
#       since the 2026-11-13 decision the cleared mode is "stock, chosen by this build": AutoPowerOff / LockMode go to their factory values
#       there (E17)
#   E14 Phase 3-4 S2: after each restore the periodic compare passes run and find no unexplained
#       difference (the restore's own sets and their side effects are not "missed changes"; S2 waits while it runs)
#   --- (user decision 2026-11-13 "A": the three modes govern every piano setting) ---
#   E15 mode power-off: a change of our own settings (1D 06 reconnect choice / preferred device / follow, 18 01 power
#       policy) starts the same 3 s debounce: no P before, one P after (status: P carries the NG part); then the live
#       values are changed again and the piano goes off before the debounce (NOR: live keys 0x0040 / 0x0041 newer than P)
#   E16 power-on 2 (power-off, nobody bound): the BT policy and the power policy come back as in P - not the newer live
#       values - status NG kind 2 (saved), both ports applied; AutoPowerOff / LockMode are in the snapshot (no set:
#       the stock kept them)
#   E17 after the factory reset (mode stock written by this build): our settings at the factory defaults (reconnect
#       "preferred, else last", no preferred device, follow off, the default power policy), AutoPowerOff = the stock's
#       factory index for the block A words (destination 0xA5A5AA01 poked: 1 = 15 min, RAM 0x4139E7F9), Lock 0
#   E18 compatibility: the NOR of E15 with the records a build before never writes taken away (KV_KEY_STARTUP_ALL, the NG
#       records of C / P - kvstore tombstones): P restores the stock part as before, our settings keep the live values
#       (NG kind 0); then mode stock as a build before wrote it (no ALL record): nothing restored, live values kept
#   E19 a live apply ("apply now") of the payload onto E15's NOR with the app bound: nothing restored, the live values stay
#   E20 (field 2026-11-13 night U16) E15's NOR + 1C 01 00 of this build, block A APO 3: the OpenGrand panel
#       (CONNECT + identity + full request) before and after the boot restore both end at the live APO 1 and get 51 00 = 1
# Not here: E5 / E6 (Kawai panel cache replay guard, Reset-mode display) - not implemented (low priority, the user
# no longer uses the Kawai screens); E9 (a write cut mid-snapshot) is the host test ksu_kv_records (power cut
# at every 37 bytes of a save: old or new snapshot, never mixed).
from __future__ import annotations

import pathlib
import struct
import sys
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rss = rtlib.load("roundtrip_session")
rfo = rtlib.load("roundtrip_fanout")
rs, images = rss.rs, rss.images


def check(name, ok, detail=""):
    return bool(rss.check(name, bool(ok), detail))
STOCKFW = rss.STOCKFW
APP, PANEL = rss.APP, rss.PANEL
ctl, connect, has = rss.ctl, rss.connect, rss.has
values = rfo.values
le = rs.le

OP_SU = 0x1C
UM_JOB, REC_RESET = 0x400A9990, 0x4016C908
LIVE, SAVED, LIVE_LEN = 0x418CE0DC, 0x40400004, 0x23A5E
STAGE_MARK = 0x418F1B3A + 4          # FUN_4015D6B4: u16 "staging copy pending"
STAGE_FLAG, JOB_FLAG, REMOTE = 0x419156A9, 0x4139DAAC, 0x413CA3B0
BT_CFG = 0x418CE0F8
KV_LO, KV_HI = 0x604C0000, 0x60500000
PH = {0: "wait", 1: "restore", 2: "resync", 3: "ready", 4: "capture"}
# startup keys the app changes in E8 (valid values, all different from the stock defaults)
SETS = [[0x51, 0x2F, 0x7F, 0x01],          # BluetoothAudio on (the user's case)
        [0x55, 0x07, 0x00, 0x01], [0x55, 0x08, 0x00, 0x03], [0x55, 0x03, 0x00, 0x30], [0x51, 0x08, 0x7F, 0x00],
        [0x50, 0x00, 0x00, 0x02], [0x50, 0x01, 0x00, 0x01], [0x51, 0x0A, 0x7F, 0x30], [0x56, 0x01, 0x7F, 0x00, 0x64],
        [0x56, 0x03, 0x7F, 0x30]]


class NetS(rfo.NetF):
    """roundtrip_fanout.NetF + the engine loop (rig.tick: power port, startup port's task-24 side) every 8 ms."""

    def __init__(self, side, **kw):
        self.next_eng = 8000
        super().__init__(side, **kw)

    def run(self, ms):
        end = self.t + int(ms * 1000)
        while self.t < end and not self.stuck:
            nxt = min(end, self.next_eng)
            if nxt > self.t:
                super().run((nxt - self.t) / 1000)
            if self.t >= self.next_eng:
                self.next_eng = self.t - self.t % 8000 + 8000
                self.s.rig.tick()


def make_side(m, nor=None, live=True, pre=None):
    """H: the stock boot settings put in after the loader boot (as roundtrip_fanout 12), on a given NOR; live = upload
    + apply now (fresh NOR), else the loader boots the slot payload already on the NOR; pre(side) runs before the
    payload runs: on a NOR boot before the loader boots it (RAM the stock boot would have set), on a live apply before the
    upload (the app's binding)."""
    orig = rs.rpw.Rig

    def factory(image, syms):
        rig = orig(image, syms, nor_mem=nor)
        boot = rig.pn.boot

        def boot2(*a, **k):
            if pre and not live:
                pre(rig.emu)              # before the loader boots the payload on the NOR (it ticks it)
            r = boot(*a, **k)
            rfo.boot_settings(rig.emu)
            if pre and live:
                pre(rig.emu)              # before the upload + apply now
            return r
        rig.pn.boot = boot2
        return rig
    rs.rpw.Rig = factory
    try:
        side = rs.Side(m.image, m.syms, m.data if live else None)
    finally:
        rs.rpw.Rig = orig
    if not live:
        side.rig.tick()                   # attach: the hooks (pump tail included) are in place after the first pass
    return side


def stubs(side):
    c = {"job": 0, "rec": 0}
    e = side.emu

    def job(em):
        c["job"] += 1
        return 0

    def rec(em):
        c["rec"] += 1
        return 0
    e.hook(UM_JOB, job)
    e.hook(REC_RESET, rec)
    return c


def status(side, port=APP):
    out = side.pn.sysex(port, rs.rp.req(OP_SU))
    b = rs.rp.body_of(out, port)
    if not b or b[0] != OP_SU | 0x40 or b[1][:2] != b"\x00\x00":
        return None
    d = rs.rp.unpack7(b[1][2:])
    w = lambda i: d[i] | d[i + 1] << 8
    return dict(layout=d[0], mode=d[1], flags=d[2], ph=PH.get(d[3], d[3]), c_size=w(4), p_size=w(6), c_gen=le(d, 8),
                p_gen=le(d, 12), recs=w(16), applied=w(18), left=w(20), unknown=w(22), saves=w(24), fails=w(26),
                deferred=w(28), stage_waits=d[30], bits=d[31], same=w(32), flash_waits=w(34),
                ngf=d[36] if len(d) > 36 else None, ng_kind=d[37] if len(d) > 37 else None,
                ng_applied=d[38] if len(d) > 38 else None, ngn=d[39] if len(d) > 39 else None)


def su_op(n, port, *args, ms=1500):
    """1C sub [arg] from port; the reply (sub, rc) when it comes (task 24 / pump tail), None if none."""
    n0 = len(n.tx)
    direct = n.s.pn.sysex(port, rs.rp.req(OP_SU, *args))
    head = bytes.fromhex("F07D43413739") + bytes([OP_SU | 0x40])
    for p, f in direct:
        if p == port and f.startswith(head):
            return f[7], f[8]
    for _ in range(int(ms / 8)):
        n.run(8)
        for _t, p, f in n.tx[n0:]:
            if p == port and f.startswith(head):
                return f[7], f[8]
    return None


def wait_ready(n, ms=4000):
    st = None
    for _ in range(int(ms / 50)):
        n.run(50)
        st = status(n.s)
        if st and st["ph"] == "ready":
            return st
    return st


def cmd(n, port, body, ms=300):
    n.send(port, ctl(0x32, body))
    n.run(ms)


def dump(n, port=APP):
    tx = rss.request(n, port, rss.FULL)
    return {bytes(f[6:9]) + (bytes(f[9:10]) if bytes(f[6:9]) in KIDX else b""): f[9:-1] for f in values(tx, port)}


KIDX = {bytes.fromhex(k) for k in ("54050f", "511a7f", "511b7f")}   # sub-indexed keys: data[0] is the index


def excluded(key):
    v1, v2 = key[0], key[1]
    if v1 == 0x50:
        return v2 in (0x1C, 0x1D, 0x1E, 0x1F, 0x2B)
    if v1 == 0x51:   # AutoPowerOff 51 00 / LockMode 51 22: startup settings since the 2026-11-13 decision
        return v2 in (0x29, 0x2E, 0x3D)
    if v1 == 0x52:
        return v2 not in (0x0C, 0x0E, 0x1E)
    if v1 == 0x56:
        return v2 in (0x08, 0x0B)
    return v1 in (0x57, 0x5A)


def ram(side, a, n):
    return bytes(side.emu.read_bytes(a, n))


# ---- our own settings (BT policy 1D 06, power policy 18) and the kvstore as a build before leaves it ----
OP_BT, OP_PW = 0x1D, 0x18
APO_RAM, LOCK_RAM = 0x4139E7F9, 0x4139E7FB          # stock live AutoPowerOff / LockMode
BLKA_MODEL, BLKA_DEST = 0x413A7798, 0x413A77A4      # settings block A words 0 / 3 in RAM
PC = bytes.fromhex("a28383831323")
F_KEEP_PREF, F_FOLLOW = 0x04, 0x10
RC_OFF, RC_PREF, RC_PREF_LAST = 0, 1, 2


def op_reply(n, port, opc, *args, ms=300):
    """A 0x7D op of the BT / power port (answered by the next engine-loop pass): the reply body after the op byte."""
    n0 = len(n.tx)
    head = bytes.fromhex("F07D43413739") + bytes([opc | 0x40])
    direct = n.s.pn.sysex(port, rs.rp.req(opc, *args))
    for p, f in direct:
        if p == port and f.startswith(head):
            return bytes(f[7:-1])
    for _ in range(int(ms / 8)):
        n.run(8)
        for _t, p, f in n.tx[n0:]:
            if p == port and f.startswith(head):
                return bytes(f[7:-1])
    return None


def bt_pol(n, port=APP):
    r = op_reply(n, port, OP_BT, 0x06)
    if not r or len(r) < 17:
        return None
    pref = rtlib.unpack7(r[7:14])[:6]
    return dict(rc=r[1], mode=r[2], flags=r[3], has_pref=r[6], pref=pref if r[6] else b"", recon=r[15], kv=r[16])


def bt_set(n, recon, flags, pref=None, port=APP):
    a = list(rtlib.pack7(pref or bytes(6)))
    r = op_reply(n, port, OP_BT, 0x06, 2, flags, 15, 5, 1 if pref else 0, *a, 0, recon)
    return r and r[1]


def pw_pol(n, port=APP):
    r = op_reply(n, port, OP_PW, 0x00)
    return rtlib.unpack7(r[2:])[:8] if r and len(r) > 2 else None


def pw_set(n, raw, port=APP):
    r = op_reply(n, port, OP_PW, 0x01, *rtlib.pack7(raw), ms=600)
    return r and r[1]


def pw_raw(apo_standby=True, hold_min=300, cap_min=480):
    """power_policy_t wire form (power.h): version 1, flags (bit0 APO -> standby), max hold, standby cap, 0 0."""
    return bytes([1, 1 if apo_standby else 0, hold_min & 0xFF, hold_min >> 8, cap_min & 0xFF, cap_min >> 8, 0, 0])


def kv_append(nor, recs):
    """The kvstore (src/params/kvstore.c) in unused_3 with records appended to its active sector, as kv_put / kv_delete
    write them: recs = [(key, value bytes, or None for a tombstone)].  Used to give the NOR the shape a build before
    leaves (it never writes the NG-owned keys) or one of its mode records."""
    mem = bytearray(nor)
    base, sec = KV_LO - 0x60000000, 0x20000

    def gen(s_):
        mg, g, crc = struct.unpack("<III", mem[base + s_ * sec: base + s_ * sec + 12])
        return g if mg == 0x3153564B and zlib.crc32(struct.pack("<II", mg, g)) == crc else None
    g0, g1 = gen(0), gen(1)
    act = 0 if g0 is not None and (g1 is None or g0 >= g1) else 1
    off = base + act * sec + 12
    while True:
        _k, ln, _f, _r = struct.unpack("<HHHH", mem[off:off + 8])
        if ln == 0xFFFF:
            break
        off += 8 + ((ln + 3) & ~3) + 4
    for key, value in recs:
        v = b"" if value is None else bytes(value)
        h = struct.pack("<HHHH", key, len(v), 0 if value is None else 1, 0xFFFF)
        pad = v + b"\xff" * (((len(v) + 3) & ~3) - len(v))
        mem[off:off + 8 + len(pad) + 4] = h + pad + struct.pack("<I", zlib.crc32(v, zlib.crc32(h)))
        off += 8 + len(pad) + 4
    return bytes(mem)


def power_on_1(m, name):
    """Power-on 1, fresh NOR, the payload applied live (0, E1, E8, E13, E10): (ok, rows, NOR bytes, the app's dump before the power-off)."""
    ok = True
    rows = []

    # ================= power-on 1: fresh NOR, the payload applied live =================
    side = make_side(m)
    cnt = stubs(side)
    n = NetS(side, pump=True)
    connect(n, APP, full=False)
    st0 = wait_ready(n)
    ok &= check(f"0 {name}: op 1C answers layout {st0 and st0['layout']}, mode {st0 and st0['mode']}, phase "
                f"{st0 and st0['ph']}, flags {st0 and hex(st0['flags'])} (blank kvstore: nothing to restore)",
                st0 is not None and st0["layout"] == 1 and st0["mode"] == 0 and st0["ph"] == "ready"
                and st0["flags"] & 0x01 and not st0["flags"] & 0x06)

    # ---- E1: the stock save bytes ----
    rfo.set_and_wait(n, APP, [0x55, 0x08, 0x00, 0x04])
    j0 = cnt["job"]
    cmd(n, APP, [0x51, 0x34, 0x7F, 0x07, 0x00, 0x7F, 0x7F])
    j_save = cnt["job"] - j0
    sv, lv = ram(side, SAVED, LIVE_LEN), ram(side, LIVE, LIVE_LEN)
    # FUN_400A9954 copies live -> image + 4, then writes image + 0x13 = 1 (the port remote flag's saved form) and
    # image + 0x2392F = 5: saved offsets 0x0F / 0x2392B may differ from live
    same_off = [i for i in range(LIVE_LEN) if sv[i] != lv[i]]
    same = all(i in (0x0F, 0x2392B) for i in same_off)
    j0 = cnt["job"]
    cmd(n, APP, [0x51, 0x34, 0x7F, 0x07, 0x7F, 0x7F, 0x7F])
    j_stage0 = cnt["job"] - j0
    side.poke(STAGE_MARK, b"\x01\x00")
    j0 = cnt["job"]
    cmd(n, APP, [0x51, 0x34, 0x7F, 0x07, 0x7F, 0x7F, 0x7F])
    j_stage1 = cnt["job"] - j0
    j0 = cnt["job"]
    cmd(n, APP, [0x51, 0x34, 0x7F, 0x00, 0x00, 0x00, 0x00])
    j_old = cnt["job"] - j0
    ok &= check(f"E1 34 07 00 7F 7F -> jobs {j_save}, saved image == live {same} (differs at {[hex(i) for i in same_off]}); 34 07 7F without / with the staging "
                f"mark -> {j_stage0} / {j_stage1}; 34 00 00 00 00 (old app bytes) -> {j_old}",
                j_save == 1 and same and j_stage0 == 0 and j_stage1 == 1 and j_old == 0)
    rows.append(("E1", "스톡 저장 바이트", f"`07 00 7F 7F` 잡 {j_save} · 저장본 = 라이브, `07 7F` 표시 없음 {j_stage0} · 있음 "
                 f"{j_stage1}, `00 00 00 00` {j_old}"))

    # ---- E8: mode power-off, debounce ----
    r = su_op(n, APP, 0x01, 0x02)
    n.run(400)
    s1 = status(side)
    nor0 = len(side.pn.nor_calls)
    gens, saves_during = [], []
    for k, b in enumerate(SETS):
        rfo.set_and_wait(n, APP, b, ms=0)
        n.run(1000 - 40 if k < len(SETS) - 1 else 0)
        saves_during.append(status(side)["saves"])
    t_last = n.t
    n.run(2800)
    s_early = status(side)
    n.run(800)
    s_due = status(side)
    calls = side.pn.nor_calls[nor0:]
    ok &= check(f"E8 1C 01 02 -> {r}; first P at once: P gen {s1 and s1['p_gen']} size {s1 and s1['p_size']}; 10 sets 1 s "
                f"apart: saves {saves_during[0]}..{saves_during[-1]}; 2.8 s after the last {s_early['saves']}, 3.6 s "
                f"{s_due['saves']} (P gen {s_due['p_gen']}); NOR ops {len(calls)} all write-buffer in unused_3 with "
                f"dispatch off",
                r == (0x01, 0) and s1 and s1["mode"] == 2 and s1["flags"] & 0x04 and s1["p_gen"] == 1
                and len(set(saves_during)) == 1 and s_early["saves"] == saves_during[-1]
                and s_due["saves"] == saves_during[-1] + 1 and s_due["p_gen"] == 2
                and calls and all(c[0] == "buf" and c[2] == 1 and KV_LO <= c[3] < KV_HI for c in calls))
    rows.append(("E8", "`poweroff` 디바운스", f"set 10번(1 s 간격) 동안 쓰기 0, 마지막 뒤 3 s에 1번(세대 {s1 and s1['p_gen']} → "
                 f"{s_due['p_gen']}), NOR 쓰기 {len(calls)}번 모두 unused_3 · dis_dsp"))
    _ = t_last

    # ---- E13 (standby flush): a change, then standby at once -> P before the 3 s ----
    rfo.set_and_wait(n, APP, [0x55, 0x09, 0x00, 0x05], ms=0)
    g0 = status(side)["p_gen"]
    t0 = n.t
    sb = rs.rpw.OP_STANDBY
    n.s.pn.sysex(APP, rs.rp.req(sb, 0x01))
    t_flush = None
    for _ in range(100):
        n.run(10)
        if status(side)["p_gen"] > g0:
            t_flush = (n.t - t0) / 1000
            break
    n.s.pn.sysex(APP, rs.rp.req(sb, 0x02))
    n.run(300)
    ok &= check(f"E13 a change then standby (19 01) at once: P generation {g0} -> written after {t_flush} ms (< 3000)",
                t_flush is not None and t_flush < 1000)
    rows.append(("E13", "대기 진입 때 저장", f"변경 직후 `19 01` → {t_flush} ms 뒤 P 기록(3 s 디바운스 안 기다림)"))

    d1 = dump(n)

    # ---- E10: a stock flash job holds the chip ----
    side.poke(JOB_FLAG, b"\x01")
    nor1 = len(side.pn.nor_calls)
    n0 = len(n.tx)
    side.pn.sysex(APP, rs.rp.req(OP_SU, 0x02))
    n.run(600)
    head = bytes.fromhex("F07D43413739") + bytes([OP_SU | 0x40, 0x02])
    early = [f for _t, p_, f in n.tx[n0:] if f.startswith(head)]
    busy_calls = len(side.pn.nor_calls) - nor1
    st_b = status(side)
    side.poke(JOB_FLAG, b"\x00")
    n.run(200)
    late = [f for _t, p_, f in n.tx[n0:] if f.startswith(head)]
    st_a = status(side)
    ok &= check(f"E10 job flag up: 600 ms -> NOR ops {busy_calls}, replies {len(early)}, waits counted "
                f"{st_b['flash_waits']}; job done -> reply {late and late[0][7:9].hex()}, C gen {st_a['c_gen']}, mode "
                f"{st_a['mode']}",
                busy_calls == 0 and not early and st_b["flash_waits"] > 0 and len(late) == 1 and late[0][8] == 0
                and st_a["c_gen"] == 1 and st_a["mode"] == 1 and st_a["flags"] & 0x02)
    rows.append(("E10", "스톡 플래시 잡 중 저장 요청", f"잡 동안 NOR 쓰기 {busy_calls} · 응답 0, 잡 끝난 뒤 1번(rc 0)"))
    r = su_op(n, APP, 0x01, 0x02)          # back to power-off for the next power-on
    n.run(4000)
    st_end = status(side)
    ok &= check(f"   back to mode power-off -> {r}; P gen {st_end['p_gen']} (an unchanged snapshot is not written again: "
                f"skipped {st_end['same']})", r == (0x01, 0) and st_end["mode"] == 2)

    # ---- E15: our own settings start the same debounce and go into P with the NG part ----
    g0 = st_end["p_gen"]
    want_pw = pw_raw(apo_standby=False, hold_min=30)
    rc1 = bt_set(n, RC_PREF, F_KEEP_PREF | F_FOLLOW, PC)
    rc2 = pw_set(n, want_pw)
    t_chg = n.t
    n.run(2000)
    s_early = status(side)
    t_due = None
    for _ in range(120):                   # 3 s quiet, and 10 s after the last save (the mode switch above)
        n.run(100)
        s_due = status(side)
        if s_due["p_gen"] > g0:
            t_due = (n.t - t_chg) / 1e6
            break
    bt_p, pw_p = bt_pol(n), pw_pol(n)
    # the live values change again; the piano goes off before the 3 s (live keys 0x0040 / 0x0041 newer than P)
    rc3 = bt_set(n, RC_OFF, F_KEEP_PREF, PC)
    rc4 = pw_set(n, pw_raw(apo_standby=True, hold_min=60))
    n.run(600)
    s_off = status(side)
    ok &= check(f"E15 1D 06 (preferred only, the PC, follow) -> rc {rc1}, 18 01 (APO -> real off, hold 30 min) -> rc {rc2}: "
                f"P gen {g0} -> 2 s {s_early['p_gen']} -> {t_due} s {s_due['p_gen']} (P carries the NG part: "
                f"{bool(s_due['ngf'] & 0x04)}); then changed again (rc {rc3} / {rc4}) and off 0.6 s "
                f"later: P gen {s_off['p_gen']} (not yet)",
                rc1 == 0 and rc2 == 0 and s_early["p_gen"] == g0 and s_due["p_gen"] == g0 + 1
                and s_due["ngf"] & 0x04 and rc3 == 0 and rc4 == 0 and s_off["p_gen"] == g0 + 1
                and bt_p and bt_p["recon"] == RC_PREF and bt_p["flags"] & F_FOLLOW and pw_p == want_pw)
    rows.append(("E15", "우리 설정 바꿈 → P(`poweroff`)", f"`1D 06` · `18 01` 뒤 2 s 쓰기 0, {t_due} s(3 s 조용 · 앞 저장 뒤 10 s)에 P 세대 {g0} → {s_due['p_gen']}"
                 f"(NG 부분 함께); 다시 바꾸고 0.6 s 뒤 끔 → P 그대로(살아 있는 값만 NOR 키에)"))
    nor = bytes(side.pn.nor.mem)
    side.close()
    return ok, rows, nor, d1


def power_on_2(m, name, po1):
    """Power-on 2 on power-on 1's NOR, nobody bound (E11, E3, E7, E14): (ok, rows, S2 passes)."""
    ok = True
    rows = []
    nor, d1 = po1[2], po1[3]

    # ================= power-on 2: the same NOR, nobody bound (E3 + E11 + E7) =================
    side = make_side(m, nor, live=False)
    side.poke(STAGE_FLAG, b"\x01")
    remote0 = ram(side, REMOTE, 4)
    bt0 = ram(side, BT_CFG, 1)[0]
    n = NetS(side, pump=True)
    n.pump_log = []
    n.run(1500)
    s_stage = status(side)
    side.poke(STAGE_FLAG, b"\x00")
    st3 = wait_ready(n)
    tx_before = [f for _t, _p, f in n.tx if f[1:2] == b"\x40"]   # Kawai frames (not our 1C status replies)
    remote1 = ram(side, REMOTE, 4)
    bt1 = ram(side, BT_CFG, 1)[0]
    fill = rfo.pump_stats(n.pump_log)
    ok &= check(f"E11 staging flag up at boot: 1.5 s -> phase {s_stage['ph']}, sets {s_stage['applied']}, waits "
                f"{s_stage['stage_waits']}", s_stage["ph"] == "restore" and s_stage["applied"] == 0
                and s_stage["stage_waits"] > 0)
    ok &= check(f"E3 restore (nobody bound): records {st3['recs']}, sets {st3['applied']}, left {st3['left']}, unknown "
                f"{st3['unknown']}; frames to anybody {len(tx_before)}; remote flags {remote0.hex()} -> {remote1.hex()}; "
                f"BluetoothAudio byte {bt0} -> {bt1}; pump tail (passes, max insns, max stack B) {fill}",
                st3["ph"] == "ready" and st3["applied"] >= 10 and st3["left"] == 0 and not tx_before
                and remote1 == remote0 and bt0 == 0 and bt1 == 1 and st3["bits"] & 0x20)
    connect(n, APP, full=False)
    d2 = dump(n)
    diff = sorted(k.hex() for k in set(d1) | set(d2) if d1.get(k) != d2.get(k))
    bad = [k for k in diff if not excluded(bytes.fromhex(k))]
    ok &= check(f"E3 the app's dump after the power-on == before the power-off on every startup key: {len(d2)} values, "
                f"differences {diff} (all excluded keys: {not bad}); BluetoothAudio {d2.get(bytes.fromhex('512f7f'))}",
                len(d2) >= 230 and not bad and d2.get(bytes.fromhex("512f7f")) == b"\x01")
    bt2, pw2, st16 = bt_pol(n), pw_pol(n), status(side)
    ok &= check(f"E16 power-off mode, power-on: BT policy recon {bt2 and bt2['recon']} preferred "
                f"{bt2 and bt2['pref'].hex()} follow {bt2 and bool(bt2['flags'] & F_FOLLOW)}, power policy "
                f"{pw2 and pw2.hex()} = P's (not the newer live values); NG kind {st16['ng_kind']}, ports applied "
                f"{st16['ng_applied']:#x}; AutoPowerOff / LockMode in the dump {d2.get(bytes.fromhex('51007f'))} / "
                f"{d2.get(bytes.fromhex('51227f'))}",
                bt2 and bt2["recon"] == RC_PREF and bt2["pref"] == PC and bt2["flags"] & F_FOLLOW
                and pw2 == pw_raw(apo_standby=False, hold_min=30) and st16["ng_kind"] == 2 and st16["ng_applied"] == 0x03
                and bytes.fromhex("51007f") in d2 and bytes.fromhex("51227f") in d2)
    rows.append(("E16", "`poweroff` 켤 때 우리 설정", f"BT 재연결 · 선호 기기 · 따라가기 · 전원 정책 = P의 값(더 새 살아 있는 값 아님), "
                 f"NG 종류 {st16['ng_kind']} · 포트 적용 {st16['ng_applied']:#x}"))
    rows.append(("E3", "켤 때 복원(바운드 없음)", f"기록 {st3['recs']} · 코어발 set {st3['applied']} · 남은 차이 {st3['left']}, "
                 f"프레임 0, 원격 플래그 불변, BT 오디오 {bt0} → {bt1}, 끄기 전 덤프 = 켠 뒤 덤프(제외 키 {len(diff)}개만 다름); "
                 f"펌프 꼬리 최대 {fill[1]} 명령어 · {fill[2]} B"))
    rows.append(("E11", "스테이징 깃발", f"깃발 동안 set {s_stage['applied']} · 기다림 {s_stage['stage_waits']}, 내린 뒤 복원"))
    n0 = len(n.tx)
    connect(n, PANEL, full=True)
    n.run(200)
    to_app = values(n.tx[n0:], APP)
    st7 = status(side)
    ok &= check(f"E7 the OpenGrand panel CONNECT + full request after the restore: value frames to the app {len(to_app)}, "
                f"sets {st7['applied']} (unchanged)", not to_app and st7["applied"] == st3["applied"])
    rows.append(("E7", "OpenGrand 패널 UI(덤프 요청)", "set 0 · 앱에 프레임 0"))
    s2a = rfo.wait_passes(n, 2)
    ok &= check(f"E14 Phase 3-4 S2 after the restore (nobody bound at boot): periodic passes {s2a and s2a['passes']}, "
                f"explained {s2a and {k: s2a[k] for k in rfo.WHY[1:] if s2a[k]}}, unexplained {s2a and s2a['none']}",
                s2a is not None and s2a["passes"] >= 2 and s2a["none"] == 0)
    side.close()
    return ok, rows, s2a


def power_on_3(m, name, po1):
    """Power-on 3 on power-on 1's NOR, the app bound during the restore (E4, E14, E2, E12), then the power-on after the factory reset (E12): (ok, rows)."""
    ok = True
    rows = []
    nor = po1[2]

    # ================= power-on 3: the app connects before the restore (E4), then E2 / E12 =================
    side = make_side(m, nor, live=False)
    cnt = stubs(side)
    n = NetS(side, pump=True)
    n.run(3)
    connect(n, APP, full=False)
    st4 = wait_ready(n)
    fan = values(n.tx, APP)
    keys = {bytes(f[6:9]) for f in fan}
    ok &= check(f"E4 the app bound during the restore: sets {st4['applied']}, value frames to the app {len(fan)} "
                f"({len(keys)} keys), restored at boot {bool(st4['bits'] & 0x20)}",
                st4["ph"] == "ready" and st4["applied"] >= 10 and len(keys) == len(fan) >= 8
                and len(fan) <= st4["applied"])
    rows.append(("E4", "복원 중 앱 바운드", f"코어발 set {st4['applied']} → 앱 팬아웃 {len(fan)}프레임(키당 1, 값이 안 바뀐 set은 "
                 f"프레임 0 — 파트 00의 TouchCurve set이 08–0B도 바꿔 그 키들의 set은 무변화)"))
    s2b = rfo.wait_passes(n, 2)
    ok &= check(f"E14 Phase 3-4 S2 after the restore with the app bound: periodic passes {s2b and s2b['passes']}, "
                f"explained {s2b and {k: s2b[k] for k in rfo.WHY[1:] if s2b[k]}}, unexplained {s2b and s2b['none']}, "
                f"fanned {s2b and s2b['fanned']}",
                s2b is not None and s2b["passes"] >= 2 and s2b["none"] == 0 and s2b["fanned"] == 0)
    rows.append(("E14", s2b))                     # completed in main with power-on 2's passes (e14_row)

    defaults_img = ram(side, SAVED, LIVE_LEN)      # nobody saved in this power-on: the stock boot left the defaults
    live0 = ram(side, LIVE, LIVE_LEN)
    j0 = cnt["job"]
    cmd(n, APP, [0x51, 0x36, 0x7F, 0x07, 0x7F, 0x7F, 0x7F])
    n.run(300)
    j7 = cnt["job"] - j0
    live7 = ram(side, LIVE, LIVE_LEN)
    saved7 = ram(side, SAVED, LIVE_LEN)
    st12a = status(side)
    j0, r0 = cnt["job"], cnt["rec"]
    cmd(n, APP, [0x51, 0x36, 0x7F, 0x04, 0x7F, 0x7F, 0x7F], ms=1000)
    j4, r4 = cnt["job"] - j0, cnt["rec"] - r0
    live4 = ram(side, LIVE, LIVE_LEN)
    ok &= check(f"E2 36 07: jobs {j7}, live unchanged {live7 == live0} (restored != defaults {live0 != defaults_img}), "
                f"saved image = defaults {saved7 == defaults_img}; 36 04: jobs {j4}, recorder resets {r4}, live == "
                f"defaults {live4 == defaults_img}",
                j7 == 1 and live7 == live0 and live0 != defaults_img and saved7 == defaults_img and j4 == 1 and r4 == 1
                and live4 == defaults_img)
    ok &= check(f"E12 after 36 07: mode {st12a['mode']}, C/P flags {hex(st12a['flags'] & 0x06)}",
                st12a["mode"] == 0 and not st12a["flags"] & 0x06)
    rows.append(("E2", "공장값 `36 07` · `36 04`", f"07: 라이브 그대로 · 저장본 = 기본값 · 잡 {j7}, 04: 라이브 = 기본값 · "
                 f"레코더 초기화 {r4}"))
    nor = bytes(side.pn.nor.mem)
    side.close()

    # since 2026-11-13 (changed deliberately): the factory reset writes mode stock as this build does (KV_KEY_STARTUP_ALL) -
    # the next power-on restores no sound setting (as before) but sets AutoPowerOff / LockMode to the stock's factory
    # values and our settings to their defaults (E17).  The stock boot copies block A to RAM; poked here: this piano's
    # words and a non-factory APO / Lock as the stock would have loaded them.
    def blka(e):
        e.write_bytes(BLKA_MODEL, (0x55AA5A01).to_bytes(4, "little"))
        e.write_bytes(BLKA_DEST, (0xA5A5AA01).to_bytes(4, "little"))
        e.write_bytes(APO_RAM, b"\x03")
        e.write_bytes(LOCK_RAM, b"\x01")
    side = make_side(m, nor, live=False, pre=blka)
    n = NetS(side, pump=True)
    st12 = wait_ready(n)
    bt = ram(side, BT_CFG, 1)[0]
    apo, lock = ram(side, APO_RAM, 1)[0], ram(side, LOCK_RAM, 1)[0]
    connect(n, APP, full=False)
    bt17, pw17 = bt_pol(n), pw_pol(n)
    ok &= check(f"E12 power-on after the factory reset: mode {st12['mode']}, sound sets none (records {st12['recs']} = the "
                f"two block A settings only), BluetoothAudio {bt} (stock default)",
                st12["mode"] == 0 and st12["recs"] == 2 and bt == 0)
    ok &= check(f"E17 mode stock (this build): NG kind {st12['ng_kind']}, sets {st12['applied']}; AutoPowerOff 3 -> {apo} "
                f"(factory for destination 0xA5A5AA01), Lock 1 -> {lock}; BT recon {bt17 and bt17['recon']} preferred "
                f"{bt17 and bt17['has_pref']} follow {bt17 and bool(bt17['flags'] & F_FOLLOW)}; power policy "
                f"{pw17 and pw17.hex()} (default)",
                st12["ng_kind"] == 1 and st12["applied"] == 2 and apo == 1 and lock == 0 and bt17
                and bt17["recon"] == RC_PREF_LAST and bt17["has_pref"] == 0 and not bt17["flags"] & F_FOLLOW
                and pw17 == bytes.fromhex("01012c01e0010000"))
    rows.append(("E12", "공장 초기화 뒤", f"코어 스냅숏 지움 → 다음 켤 때 소리 설정 set 0(스톡), 블록 A 두 설정만 기록 {st12['recs']}"))
    rows.append(("E17", "`stock`(이 판이 쓴 모드) 켤 때", f"APO 3 → {apo}(목적지 0xA5A5AA01의 공장값) · 잠금 1 → {lock}, BT 재연결 "
                 f"「선호 · 마지막」 · 선호 없음 · 따라가기 끔 · 전원 정책 기본값, NG 종류 {st12['ng_kind']}"))
    side.close()
    return ok, rows


def power_on_4(m, name, po1):
    """Compatibility (E18) and a live apply (E19) on power-on 1's NOR: (ok, rows)."""
    ok = True
    rows = []
    nor = kv_append(po1[2], [(k_, None) for k_ in (0x0201, 0x0219, 0x021A, 0x0229, 0x022A)])
    side = make_side(m, nor, live=False)
    n = NetS(side, pump=True)
    st = wait_ready(n)
    bt1 = ram(side, BT_CFG, 1)[0]
    connect(n, APP, full=False)
    b, p = bt_pol(n), pw_pol(n)
    ok &= check(f"E18 a P and a mode of a build before (no NG part, no ALL record): stock part restored (sets "
                f"{st['applied']}, BluetoothAudio {bt1}); NG kind {st['ng_kind']}, ports applied {st['ng_applied']}; BT "
                f"recon {b and b['recon']} follow {b and bool(b['flags'] & F_FOLLOW)}, power {p and p.hex()} = the live "
                f"values on NOR",
                st["applied"] >= 10 and bt1 == 1 and st["ng_kind"] == 0 and st["ng_applied"] == 0 and b
                and b["recon"] == RC_OFF and b["pref"] == PC and not b["flags"] & F_FOLLOW
                and p == pw_raw(apo_standby=True, hold_min=60) and not st["ngf"] & 0x07)
    side.close()
    nor2 = kv_append(nor, [(0x0200, bytes([1, 0]))])           # mode stock, as a build before writes it
    side = make_side(m, nor2, live=False)
    n = NetS(side, pump=True)
    st2 = wait_ready(n)
    connect(n, APP, full=False)
    b2 = bt_pol(n)
    ok &= check(f"E18 mode stock of a build before: NG kind {st2['ng_kind']}, records {st2['recs']}, sets {st2['applied']}; "
                f"BT recon {b2 and b2['recon']} preferred {b2 and b2['pref'].hex()} (the live values)",
                st2["mode"] == 0 and st2["ng_kind"] == 0 and st2["applied"] == 0 and b2 and b2["recon"] == RC_OFF
                and b2["pref"] == PC)
    side.close()
    rows.append(("E18", "옛 판의 스냅숏 · 모드", f"NG 부분 · ALL 기록 없음 → 스톡 부분 복원(set {st['applied']}), 우리 설정 = NOR의 "
                 f"살아 있는 값(NG 종류 0); 옛 판이 쓴 `stock` → 복원 0 · 살아 있는 값"))

    # E19: the same payload applied live onto E15's NOR, the app bound.  The loader first
    # cold-boots the payload already on that NOR (power-off mode: P restored - recon "preferred only", hold 30 min, as E16);
    # the apply must keep exactly those values and restore nothing again
    side = make_side(m, po1[2], live=True, pre=lambda e: e.write_bytes(0x413CA3DE, bytes([APP + 1])))   # the app bound
    n = NetS(side, pump=True)
    connect(n, APP, full=False)
    st3 = wait_ready(n)
    b3, p3 = bt_pol(n), pw_pol(n)
    ok &= check(f"E19 a live apply: restored at boot {bool(st3['bits'] & 0x20)}, sets {st3['applied']}, NG kind "
                f"{st3['ng_kind']}, BT recon {b3 and b3['recon']}, power {p3 and p3.hex()} (what the piano had before "
                f"the apply)",
                not st3["bits"] & 0x20 and st3["applied"] == 0 and st3["ng_kind"] == 0 and b3 and b3["recon"] == RC_PREF
                and b3["flags"] & F_FOLLOW and p3 == pw_raw(apo_standby=False, hold_min=30))
    rows.append(("E19", "지금 적용", f"복원 0 · NG 종류 {st3['ng_kind']} — 살아 있는 값 그대로"))
    side.close()

    # E20 (field 2026-11-13 night, U16): the field's NOR shape - C / P with NG parts written by a
    # power-off mode, then 1C 01 00 of this build (mode stock + KV_KEY_STARTUP_ALL) - and the stock booting block A with
    # APO 3 (120 min) / Lock 1; the OpenGrand panel UI (CONNECT + identity + full request, session.ts) connects before the
    # boot restore and long after it.  Whatever the order, the live APO byte is 1 and the last 51 00 the panel gets is 1
    nor20 = kv_append(po1[2], [(0x0200, bytes([1, 0])), (0x0201, bytes([1, 0]))])
    seen = []
    for early in (True, False):
        side = make_side(m, nor20, live=False, pre=blka_field)
        n = NetS(side, pump=True)
        t0 = len(n.tx)
        if early:
            n.run(3)
            connect(n, PANEL, full=True)
            st20 = wait_ready(n)
        else:
            st20 = wait_ready(n)
            n.run(1000)
            connect(n, PANEL, full=True)
        n.run(2000)
        apo = ram(side, APO_RAM, 1)[0]
        last = [bytes(f)[9] for _t, p, f in n.tx[t0:] if p == PANEL and bytes(f).startswith(APO_VALUE)]
        seen.append((early, st20 and st20["ng_kind"], st20 and st20["applied"], apo, last))
        side.close()
    ok &= check(f"E20 field NOR (C / P with NG parts, then 1C 01 00), block A APO 3: the OpenGrand panel before / after the "
                f"restore - (early, NG kind, sets, live APO, 51 00 values to the panel) {seen}",
                all(k == 1 and a == 2 and apo == 1 and last and last[-1] == 1 for _e, k, a, apo, last in seen))
    rows.append(("E20", "11-13 밤 U16 모양(C · P 있음 → `1C 01 00`) + OpenGrand 패널", "패널이 복원 전 · 뒤 어느 때 붙어도 살아 있는 "
                 "APO 1 · 패널이 받은 마지막 `51 00` = 1(15분) — 실기의 120분은 재현 안 됨"))
    return ok, rows


APO_VALUE = bytes.fromhex("F0407F10041E51007F")


def blka_field(e):
    """The stock boot's RAM for E20: this piano's block A words and APO 3 / Lock 1 as loaded from it."""
    e.write_bytes(BLKA_MODEL, (0x55AA5A01).to_bytes(4, "little"))
    e.write_bytes(BLKA_DEST, (0xA5A5AA01).to_bytes(4, "little"))
    e.write_bytes(APO_RAM, b"\x03")
    e.write_bytes(LOCK_RAM, b"\x01")


def e14_row(s2a, s2b):
    return ("E14", "S2 주기 대조(복원 뒤)", f"바운드 없음 · 앱 바운드 둘 다 설명 안 된 차이 {s2a and s2a['none']} · "
            f"{s2b and s2b['none']}(대조 {s2a and s2a['passes']} · {s2b and s2b['passes']}바퀴), 복원 동안은 대조 멈춤")


def main(argv):
    name = rtlib.first_arg(argv, "ota_main")
    p = images.OTA_PAYLOADS_BY_NAME[name]
    if not p.feat2 & images.FEAT2_STARTUP:
        raise SystemExit(f"{name}: no startup settings (feat2 0x08)")
    _img, _stock, _manifest, image, syms = rtlib.loader()
    data = (STOCKFW / "build" / f"{name}.c79").read_bytes()
    m = rss.Machines(image, syms, data)
    ok = True
    rows = []

    # power-on 2 and 3 start from power-on 1's NOR: a shard without power-on 1 runs it first, silently (rtlib.Unit.needs)
    units = [rtlib.Unit("power-on 1", lambda: power_on_1(m, name), 20),
             rtlib.Unit("power-on 2", lambda po1: power_on_2(m, name, po1), 11, needs=("power-on 1",)),
             rtlib.Unit("power-on 3", lambda po1: power_on_3(m, name, po1), 12, needs=("power-on 1",)),
             rtlib.Unit("power-on 4", lambda po1: power_on_4(m, name, po1), 12, needs=("power-on 1",))]
    res = {u.name: r for u, r in rtlib.run_units(units, argv)}
    for r in res.values():
        ok &= r is not None and bool(r[0])
        rows += r[1] if r is not None else []
    s2a = (res.get("power-on 2") or (None, None, None))[2]
    rows = [e14_row(s2a, r_[1]) if r_[0] == "E14" and len(r_) == 2 else r_ for r_ in rows]

    print("\n| # | 시나리오 | 결과 |\n|---|---|---|")
    for r_ in rows:
        print(f"| {r_[0]} | {r_[1]} | {r_[2]} |")
    return rtlib.finish(ok, rtlib.shard_note(units, argv))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
