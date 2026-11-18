# roundtrip_usbplay.py - USB playback: the field of 2026-11-13 (a MID and an MP3 sounding together, no remote command
# stopping the MID) and the stop that works, run on the real stock code (record
# 「USB 재생 겹침 · 정지」).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_usbplay.py [ota_main]
#
# Machines as roundtrip_fanout.py (virtual 2 ms clock, the real request task / task 19 end handler, the controller
# models), with the stock boot settings and task 20 (FUN_4016DE5C(1): the Music queue FUN_4015DE0C and the SMF
# sequencer) every 2 ms.  S = the stock, the app alone (the stock binding); H = the payload applied live, app + panel
# bound and the panel holding the stock binding (the field of 11-09 / 11-13).  The app sends what the app sends
# (31 52 1D, 24 08 + FileName, 32 52 08); the engine loop's file executor FUN_4014F364 runs once after each name.
# Model (not the stock): USB memory mounted (media byte 0x41359E45 = 1, the FAT mount word 0x41057EBC != 0 that
# MusicMode 3 asks via FUN_40127A84); the FAT file size / read (FUN_400A9238 / FUN_400A9408) give a 60-bar SMF (one
# note), the MP3 prepare FUN_401258B0 returns 0; the audio transport executor FUN_40123E34 (engine loop, DSP) is
# replaced by its outcome - a pending op 1 makes the audio state 0x41058AD1 = 1, op 0 / 2 makes it 2 (stopped).
# Everything else - the Music queue, FUN_4015DBD4 / FUN_4015DCC4 / FUN_401737A4, the SMF player and its progress
# notices, job 8 of the executor (type, Busy) - is the stock image's code.
# Checks (numbered as printed, each on S and on H):
#   1  the field: SONG_01.MID selected + Play -> SMF state 1, 32 52 08 7F 01, bars advance; 1.mp3 selected (type 5,
#      loaded) + Play -> the audio player plays too (state 1) and the SMF player plays on
#   2  the field's stops, each followed by 1 s: Reset, Pause, MusicMode 0, MusicMode 3 + Reset, MusicMode 2 + Reset ->
#      the SMF state stays 1 and the bars keep advancing after every one of them (only the audio player stops)
#   3  the proven stop: MusicMode 3, 24 08 SONG_01.MID -> Busy 7E 03 01 00 00 with the type set to 4 and nothing
#      read (the playing song untouched), Music Reset -> SMF state 2, bar 1 twice + 32 52 08 7F 02, then no progress
#   4  the app's stop (ui/usbplay.ts usbStopAll) from the field state on a fresh machine: MusicMode 3 + Reset, then
#      MusicMode 3 + 24 08 1.mp3 + Reset, MusicMode 3 + 24 08 SONG_01.MID + Reset -> both players stopped, no
#      progress for 1.5 s
#   5  H = S: the same states after every step of 1-4 (MusicMode, selected type, SMF state, audio) - the session core
#      neither drops nor rewrites any of these commands (MusicMode 2 + Reset on H: the core's recorder control takes
#      the Reset - the recorder, not a USB player, as on S)
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rf = rtlib.load("roundtrip_fanout")
APP, PANEL = rf.APP, rf.PANEL
ctl = rf.ctl
images = rf.images

SMF_STATE, SMF_FLAG = 0x41942842, 0x41942845      # FUN_40173864: 0 pause, 1 play, 2 stop; bit 7 = clock running
AUDIO_STATE, XCMD = 0x41058AD1, 0x41058ACE         # FUN_40123B40; FUN_40123848(op) -> op | 0x80
SEL, MODE = 0x413CA3F5, 0x419156A6                 # FUN_4014EDFC (24 08 type); FUN_4015C244
MOUNT = 0x41057EBC                                 # FUN_40127A84 (MusicMode 3 needs it != 0) [model: mounted]
FAT_SIZE, FAT_READ, MP3_PREP = 0x400A9238, 0x400A9408, 0x401258B0
BUSY = bytes.fromhex("F0407F7E041E03010000F7")
PROG_HEAD = bytes.fromhex("F0407F32041E52037F00")  # MusicProgress, bar kind
MUSIC_HEAD = bytes.fromhex("F0407F32041E52087F")
MID, MP3 = "\\SONG_01.MID", "\\1.mp3"


def smf_file(bars=60):
    def vlq(v):
        out = [v & 0x7F]
        v >>= 7
        while v:
            out.insert(0, 0x80 | (v & 0x7F))
            v >>= 7
        return bytes(out)
    trk = (b"\x00\xff\x51\x03\x07\xa1\x20" + b"\x00\x90\x3c\x40" + vlq(480 * 4 * bars) + b"\x80\x3c\x00"
           + b"\x00\xff\x2f\x00")
    return b"MThd" + bytes.fromhex("000000060000000101e0") + b"MTrk" + len(trk).to_bytes(4, "big") + trk


class Rig:
    def __init__(self, m, h):
        side = rf.side_settings(m, m.data if h else None)
        if h:
            rf.songs_in(side)
        self.h = h
        self.n = n = rf.NetF(side, pump=h)
        if h:
            rf.both_bound(n)
        else:
            rf.connect(n, APP, full=False)
        n.run(100)
        e = n.e
        n.s.poke(rf.MEDIA, b"\x01")
        e.write_u32(MOUNT, 1)
        smf = smf_file()
        self.reads = []
        self.preps = []

        def size(em):
            em.write_u32(em.reg(1), len(smf))
            return 0

        def read(em):
            self.reads.append(em.read_cstr(em.reg(0)))
            em.write_bytes(em.reg(1), smf)
            return 0

        def prep(em):
            self.preps.append(em.reg(1))
            return 0
        e.hook(FAT_SIZE, size)
        e.hook(FAT_READ, read)
        e.hook(MP3_PREP, prep)
        n.t20 = True
        n.t20_max = 100_000_000         # an SMF stop + rewind runs long in one task-20 pass
        n.file_ack = {APP, PANEL}
        self.log = []                   # (step, state) for check 5

    def st(self):
        n = self.n
        return dict(mode=n.b(MODE), sel=n.b(SEL), smf=n.b(SMF_STATE), clock=n.b(SMF_FLAG) >> 7, audio=n.b(AUDIO_STATE))

    def audio_exec(self):
        """[model] the audio transport executor's outcome for a pending op (the DSP is not emulated)"""
        x = self.n.b(XCMD)
        if x & 0x80:
            op = x & 0x7F
            self.n.s.poke(AUDIO_STATE, bytes([1 if op == 1 else 2 if op in (0, 2) else self.n.b(AUDIO_STATE)]))
            self.n.s.poke(XCMD, b"\x00")

    def rx(self, n0):
        return [f for _t, p, f in self.n.tx[n0:] if p == APP]

    def step(self, label, fn=None, body=None, ms=60):
        n = self.n
        n0 = len(n.tx)
        if body is not None:
            n.send(APP, ctl(fn, body))
        n.run(ms)
        self.audio_exec()
        self.log.append((label, self.st()))
        return self.rx(n0)

    def mode(self, v, label=None):
        return self.step(label or f"MusicMode {v}", 0x31, [0x52, 0x1D, 0x7F, v])

    def music(self, v, label=None, ms=60):
        return self.step(label or f"Music {v}", 0x32, [0x52, 0x08, 0x7F, v], ms=ms)

    def select(self, name):
        n = self.n
        n0 = len(n.tx)
        n.send(APP, ctl(0x24, [0x08]))
        n.run(60)
        for pk in rf.name_packets(name):
            n.send(APP, pk)
            n.run(40)
        n.task(rf.EXECUTOR)              # the engine loop's FUN_4014F364 (job 8)
        n.run(80)
        self.log.append((f"24 08 {name}", self.st()))
        return self.rx(n0)

    def wait(self, ms):
        return self.step(f"wait {ms}", ms=ms)


def bars(frames):
    return [(f[10] << 7 | f[11], f[12]) for f in frames if f.startswith(PROG_HEAD)]


def advancing(frames):
    b = bars(frames)
    return len(b) >= 2 and all(y > x for x, y in zip(b, b[1:]))


def field(r):
    """1-3 on one machine; returns (ok, rows)"""
    ok = True
    side = "H" if r.h else "S"
    rows = []
    r.mode(3)
    r.select(MID)
    play_mid = r.music(1)
    s1 = r.st()
    r.mode(3)
    sel_mp3 = r.select(MP3)
    r.music(1)
    s2 = r.st()
    run1 = r.wait(1500)
    ok &= rtlib.check(f"{side} 1 field: MID play -> {s1}, report {[f.hex() for f in play_mid if f.startswith(MUSIC_HEAD)]}; "
                      f"1.mp3 selected (prepares {r.preps}, frames {len(sel_mp3)}) + Play -> {r.st()}; bars {bars(run1)[:6]}",
                      s1["smf"] == 1 and s1["clock"] == 1 and MUSIC_HEAD + b"\x01\xF7" in play_mid and r.preps == [1]
                      and s2["sel"] == 5 and r.st()["audio"] == 1 and r.st()["smf"] == 1 and advancing(run1))
    after = []
    for label, steps in (("Reset", [lambda: r.music(2)]), ("Pause", [lambda: r.music(0)]),
                         ("MusicMode 0", [lambda: r.mode(0)]), ("MusicMode 3 + Reset", [lambda: r.mode(3), lambda: r.music(2)]),
                         ("MusicMode 2 + Reset", [lambda: r.mode(2), lambda: r.music(2)])):
        for s in steps:
            s()
        w = r.wait(1000)
        after.append((label, r.st()["smf"], advancing(w), bars(w)[-1:] if w else None))
    ok &= rtlib.check(f"{side} 2 field stops, the SMF player after each (state, bars advancing): {after}; audio {r.st()['audio']}",
                      all(a[1] == 1 and a[2] for a in after) and r.st()["audio"] == 2)
    rows.append((f"{side} 1–2", "MID ▶ → 1.mp3 ▶ → Reset · Pause · MusicMode 0 · 3+Reset · 2+Reset",
                 f"SMF 상태 1 그대로 · 마디 진행 계속(각 1 s), 오디오만 2 — 실기와 같음"))
    r.mode(3)
    reads0 = len(r.reads)
    sel = r.select(MID)
    s3 = r.st()
    stop = r.music(2, ms=200)
    quiet = r.wait(1500)
    ok &= rtlib.check(f"{side} 3 proven stop: 24 08 SONG_01.MID while it plays -> {[f.hex() for f in sel if f.startswith(BUSY[:7])]} "
                      f"type {s3['sel']} SMF {s3['smf']} reads +{len(r.reads) - reads0}; Reset -> {r.st()}, frames "
                      f"{[f.hex() for f in stop]}; progress afterwards {len(bars(quiet))}",
                      BUSY in sel and s3["sel"] == 4 and s3["smf"] == 1 and len(r.reads) == reads0
                      and r.st()["smf"] == 2 and r.st()["clock"] == 0 and bars(stop) == [(1, 0), (1, 0)]
                      and MUSIC_HEAD + b"\x02\xF7" in stop and not bars(quiet))
    rows.append((f"{side} 3", "MusicMode 3 → `24 08` SONG_01.MID → Reset",
                 "`24 08`에 Busy(형식 4는 저장, 읽기 0) → Reset: SMF 상태 2 · 1마디 ×2 · `52 08 7F 02`, 뒤로 진행 0"))
    return ok, rows


def app_stop(r):
    side = "H" if r.h else "S"
    r.mode(3)
    r.select(MID)
    r.music(1)
    r.mode(3)
    r.select(MP3)
    r.music(1)
    r.wait(800)
    s0 = r.st()
    # ui/usbplay.ts usbStopAll: the selected type's player, then the audio player, then the SMF player
    r.mode(3)
    r.music(2)
    r.mode(3)
    a = r.select(MP3)
    r.music(2)
    r.mode(3)
    m = r.select(MID)
    r.music(2, ms=200)
    quiet = r.wait(1500)
    ok = rtlib.check(f"{side} 4 the app's stop from {s0}: 24 08 1.mp3 -> busy {BUSY in a}, 24 08 SONG_01.MID -> busy "
                     f"{BUSY in m}; after {r.st()}, progress for 1.5 s {len(bars(quiet))}",
                     s0["smf"] == 1 and s0["audio"] == 1 and BUSY in m and r.st()["smf"] == 2 and r.st()["audio"] == 2
                     and not bars(quiet))
    return ok, [(f"{side} 4", "앱 정지 순서(현재 형식 Reset → MP3 고르고 Reset → MID 고르고 Reset)",
                 "두 재생기 모두 정지(SMF 2 · 오디오 2), 1.5 s 진행 0")]


def main(argv):
    name = rtlib.first_arg(argv, "ota_main")
    _img, _stock, _manifest, image, syms = rtlib.loader()
    data = (rf.STOCKFW / "build" / f"{name}.c79").read_bytes()
    m = rf.rss.Machines(image, syms, data)
    ok = True
    rows = []
    logs = {}
    for h in (False, True):
        r = Rig(m, h)
        o, rr = field(r)
        ok &= o
        rows += rr
        r.n.s.close()
        r2 = Rig(m, h)
        o, rr = app_stop(r2)
        ok &= o
        rows += rr
        r2.n.s.close()
        logs[h] = r.log + r2.log
    # 5 H = S step by step (the H-only MusicMode 2 + Reset: the core's recorder control, no USB player either way)
    diff = [(a[0], a[1], b[1]) for a, b in zip(logs[False], logs[True]) if a[1] != b[1]]
    ok &= rtlib.check(f"5 H = S after every step ({len(logs[False])} / {len(logs[True])} steps): differences {diff}",
                      len(logs[False]) == len(logs[True]) and not diff)
    rows.append(("5", "H(ota_main, 패널이 스톡 바인딩) = S", f"단계 {len(logs[True])}개 상태 같음 — 코어가 이 명령들을 버리거나 바꾸지 않음"))
    print("\n| # | 시나리오 | 결과 |\n|---|---|---|")
    for row in rows:
        print(f"| {row[0]} | {row[1]} | {row[2]} |")
    return rtlib.finish(ok)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
