# stock_kso.py - the KSO files written by the app's MIDI -> KSO converter (result/app/src/protocol/kso.ts), put
# through the STOCK firmware under Unicorn: import (24 09 <song> 03), boot check, loader, save as KSO and as SMF.
#
#   for every case of result/app/tests/fixtures/kso_cases.ts (KSO made by `node scripts/kso_cases.ts`):
#   A. import into an internal song: result 0, one RecorderData NOR job, the other songs unchanged
#   B. the boot check passes (a store that fails it is formatted at the next power-on)
#   C. the loader takes parts 1/2 and counts the bars the converter wrote in the header
#   D. playback: the stock player, stepped tick by tick, starts every note the file holds at its tick with its
#      velocity and length, sets the tempo of every tempo entry and applies the pedals
#   E. save as KSO: the same bytes back, except the 32-byte name (the stock writes the file name there)
#   F. save as SMF: recorded in result/app/tests/fixtures/kso_stock.json - the app test compares its own
#      ksoToSmf() with it byte for byte
#
# Not modelled: RTOS, the NOR job itself, the tone generator (the player's note / pedal / tempo calls are recorded).
#
#   cd result/stockfw && uv run python -I tools/stock_kso.py [-q] [--write]     (--write: rewrite the fixture)
from __future__ import annotations

import importlib.util
import json
import pathlib
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
_HERE = pathlib.Path(__file__).resolve().parent
_APP = _HERE.parents[1] / "app"
FIXTURE = _APP / "tests/fixtures/kso_stock.json"


def _load(name):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, _HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


ss = _load("stock_songs")
p3 = ss.p3

REC = ss.REC
NAME_BUF = 0x413C9FC4        # FileName buffer of 24 xx requests
FILE_SIZE = 0x400A9238       # (name, &size) -> 0 ok
FILE_READ = 0x400A9408       # (name, buf) -> 0 ok
FAT_WRITE = 0x400A92C0       # (name, buf, len, check_exists)
EXPORT = 0x4014F0E4          # (song, type 3 KSO / 4 MID)
IMPORT = 0x4014F1A8          # (song, type) - only 3
BOOT_CHECK = 0x4016E174      # RecorderData check (0 = ok)
LOADER = 0x401674FC          # (song): fills the 16 play states
PLAY_STATES = 0x4191E054     # loader track states, 16 x 0x54 (+0 = track pointer, +0x3C = bars)
LONGEST = 0x4191EF76         # loader: index of the track with most bars (0xFF = none)
PLAYER_TRACK = 0x4016D62C    # (track, x): play this track's events that are due
PLAYER_TEMPO = 0x4016D150    # tempo list: apply the entries that are due
PLAYER_STATES_LIT = 0x4016CB54   # literal: the player's 16 track states
TICK_LIT = 0x4016E16C        # literal: current tick
NOTE_ON = 0x4016D3C0         # (track, key, velocity, release velocity, gate)
SET_TEMPO = 0x40165F34       # (bpm)
PEDAL = 0x401661EC           # (track, controller, value)
EMPTY = [(0, 0)] * 10


def cases():
    out = subprocess.run(["node", "scripts/kso_cases.ts"], cwd=_APP, check=True, capture_output=True, text=True).stdout
    return [(c["name"], bytes.fromhex(c["kso"])) for c in json.loads(out)]


class Piano:
    def __init__(self):
        self.r = p3.StockRun()
        e = self.e = self.r.emu
        e.write_bytes(REC, ss.store(EMPTY))
        e.write_bytes(REC + ss.REC_END, b"\x5A\xA5")
        self.jobs, self.files, self.usb = [], [], {}
        e.hook(ss.NOR_JOB, lambda em: (self.jobs.append((em.reg(0), em.reg(1), em.reg(2))), 0)[1])
        e.hook(FAT_WRITE, self._write)
        e.hook(FILE_SIZE, self._size)
        e.hook(FILE_READ, self._read)

    def _name(self, em, r):
        return em.read_cstr(em.reg(r)).decode("latin1")

    def _write(self, em):
        self.files.append((self._name(em, 0), bytes(em.read_bytes(em.reg(1), em.reg(2)))))
        return 0

    def _size(self, em):
        n = self._name(em, 0)
        if n not in self.usb:
            return 1
        em.write_bytes(em.reg(1), struct.pack("<I", len(self.usb[n])))
        return 0

    def _read(self, em):
        em.write_bytes(em.reg(1), self.usb[self._name(em, 0)])
        return 0

    def call(self, fn, *args, n=50_000_000):
        r = self.e.call(fn, *args, max_insns=n)
        assert r.ok, r
        return r.r0

    def import_(self, song, name, data):
        self.usb[name] = data
        self.e.write_bytes(NAME_BUF, name.encode() + b"\0")
        self.jobs = []
        return self.call(IMPORT, song, 3)

    def export(self, song, name, kind):
        self.e.write_bytes(NAME_BUF, name.encode() + b"\0")
        self.files = []
        rc = self.call(EXPORT, song, kind, n=400_000_000)
        return rc, (self.files[0][1] if self.files else None)

    def songs(self):
        o = struct.unpack_from("<12I", self.e.read_bytes(REC, 0x30))
        return [bytes(self.e.read_bytes(REC + o[i], o[i + 1] - o[i])) for i in range(10)]

    def u32(self, a):
        return struct.unpack("<I", self.e.read_bytes(a, 4))[0]

    def play(self, song, ticks):
        """Step the stock player over the loaded song. -> (notes, tempos, pedals) with the tick of each call."""
        e = self.e
        assert self.call(LOADER, song) == 0
        states, tick_var = self.u32(PLAYER_STATES_LIT), self.u32(TICK_LIT)
        e.write_bytes(states, e.read_bytes(PLAY_STATES, 16 * 0x54))   # the play start copies the loader states
        now = [0]
        notes, tempos, pedals = [], [], []
        stack = lambda em, k: struct.unpack("<I", em.read_bytes(em.reg(13) + 4 * k, 4))[0]
        hooks = [
            e.hook(NOTE_ON, lambda em: (notes.append((now[0], em.reg(0), em.reg(1), em.reg(2), em.reg(3), stack(em, 0) & 0xFFFF)), 0)[1]),
            e.hook(SET_TEMPO, lambda em: (tempos.append((now[0], em.reg(0))), 0)[1]),
            e.hook(PEDAL, lambda em: (pedals.append((now[0], em.reg(0), em.reg(1), em.reg(2))), 0)[1]),
        ]
        for t in range(1, ticks + 2):
            now[0] = t
            e.write_bytes(tick_var, struct.pack("<I", t))
            self.call(PLAYER_TEMPO)
            for trk in range(2):
                if self.u32(states + 0x54 * trk + 0x1C):
                    self.call(PLAYER_TRACK, trk, 0)
        return notes, tempos, pedals


def decode(kso):
    """Our own reading of a KSO: -> (bars, tempo list, [(tick, track, key, vel, rel, gate)], [(tick, track, cc, value)])."""
    w = lambda o: struct.unpack_from(">I", kso, 0x10 + o)[0]
    tempos, p = [], w(0x24)
    while kso[p] != 0xFC:
        wide = kso[p + 3] == 0xFF
        tempos.append((int.from_bytes(kso[p:p + 3], "big"), kso[p + 4] + 0xFF if wide else kso[p + 3]))
        p += 5 if wide else 4
    notes, pedals = [], []
    for trk in range(2):
        p = w(0x2C + 4 * trk)
        if not p:
            continue
        bar = base = 0
        length = None
        while kso[p] != 0xFC:
            b = kso[p]
            if b == 0xFA:
                if length is not None:
                    bar += length
                base = bar
                length = [192, 96, 48, 24][kso[p + 1] >> 5] * ((kso[p + 1] & 0x1F) + 1)
                p += 2
            elif b == 0xF8:
                base += 96
                p += 1
            elif b == 0xB0:
                pedals.append((base + kso[p + 1], trk, kso[p + 2], kso[p + 3]))
                p += 4
            else:
                wide = kso[p + 2] == 0xFE
                q = p + (6 if wide else 4)
                gate = kso[p + 4] * 128 + kso[p + 5] if wide else kso[p + 2]
                notes.append((base + kso[p + 1], trk, b, kso[q], kso[q + 1], gate))
                p = q + 2
    return w(0x70), tempos, notes, pedals, bar + length


def main():
    quiet = "-q" in sys.argv
    fails, fixture = [], []

    def check(cond, what):
        if not cond or not quiet:
            print(f"  [{'ok' if cond else 'FAIL'}] {what}")
        if not cond:
            fails.append(what)

    for i, (name, kso) in enumerate(cases()):
        song = (i * 3) % 10
        if not quiet:
            print(f"{name} ({len(kso)} B) -> song {song + 1}")
        bars, tempos, notes, pedals, end = decode(kso)
        pn = Piano()
        before = pn.songs()
        rc = pn.import_(song, "\\IN.KSO", kso)
        after = pn.songs()
        want = bytearray(kso[0x10:])
        for off in [0x20, 0x24] + [0x2C + 4 * k for k in range(17)]:
            v = struct.unpack_from(">I", want, off)[0]
            struct.pack_into("<I", want, off, v - 0x10 if v else 0)
        for off in (0x28, 0x70):
            struct.pack_into("<I", want, off, struct.unpack_from(">I", want, off)[0])
        check(rc == 0 and after[song] == bytes(want), "A. imported: the song in the store is the file body with little-endian song-relative offsets")
        check(all(after[k] == before[k] for k in range(10) if k != song) and len(pn.jobs) == 1 and pn.jobs[0][0] == 1,
              "A. other songs unchanged, one RecorderData NOR job")
        check(pn.call(BOOT_CHECK) == 0, "B. boot check passes")
        used = [k for k in range(16) if pn.u32(PLAY_STATES + 0x54 * k)]
        have = [k for k in range(16) if struct.unpack_from(">I", kso, 0x3C + 4 * k)[0]]
        counted = [struct.unpack("<H", pn.e.read_bytes(PLAY_STATES + 0x54 * k + 0x3C, 2))[0] for k in used]
        check(used == have and all(c == bars for c in counted), f"C. loader: tracks {used}, bars counted {counted} = header {bars}")
        got_n, got_t, got_p = pn.play(song, end)
        # the player starts an event at the first tick AFTER its time (strict compare), gate as written
        check(sorted(got_n) == sorted((t + 1, trk, key, vel, rel, gate) for t, trk, key, vel, rel, gate in notes),
              f"D. player: {len(notes)} notes at their ticks with velocity / release velocity / gate")
        check(got_t == [(t + 1, bpm) for t, bpm in tempos], f"D. player: tempo list {tempos} -> set at {got_t}")
        check(sorted(got_p) == sorted((t + 1, trk, cc, v) for t, trk, cc, v in pedals), f"D. player: {len(pedals)} pedal events")
        rc, again = pn.export(song, "\\OUT.KSO", 3)
        check(rc == 0 and again is not None and again[:0x10] == kso[:0x10] and again[0x30:] == kso[0x30:], "E. save as KSO: identical except the name")
        rc, mid = pn.export(song, "\\OUT.MID", 4)
        check(rc == 0 and mid is not None and mid[:4] == b"MThd", f"F. save as SMF: {len(mid) if mid else 0} B")
        fixture.append({"name": name, "kso": kso.hex(" "), "stockSmf": mid.hex(" ") if mid else ""})

    text = json.dumps(fixture, indent=1) + "\n"
    if "--write" in sys.argv:
        FIXTURE.write_text(text)
        print(f"wrote {FIXTURE.relative_to(_APP.parents[1])}")
    else:
        check(FIXTURE.is_file() and FIXTURE.read_text() == text, "fixture tests/fixtures/kso_stock.json is current (rewrite with --write)")
    print("stock_kso: all checks passed" if not fails else f"stock_kso: {len(fails)} FAILED: " + "; ".join(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
