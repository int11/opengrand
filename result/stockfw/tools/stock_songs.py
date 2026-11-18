# stock_songs.py - what the STOCK firmware tells a remote controller about the internal recorder songs
# (RecorderData, 10 songs), run on the unmodified image under Unicorn with a SYNTHETIC song store
# (never the owner's NOR dump: no personal content).
#
#
#   A. SongSelect 10 52 00 7F [0, hi, lo] (apply 0x40160F68 -> FUN_401687E4 -> loader FUN_401674FC):
#      empty song -> Result 7E 03 00 01 6C (0xEC "no data"); a song with data -> no error. Either way the
#      MaxTime notification (descriptor 0xA4, queued by FUN_40151A38, sent by the notify task FUN_40151E08 ->
#      FUN_4014C048) carries the bar count of the longest track: 10 52 18 7F 00 <bars hi7> <bars lo7> 00 00 00.
#   B. IntSongPart 10 52 01 7F <0|1|7F> reloads the selected song with that part: if the part has no track
#      the stock answers 0xEC and MaxTime 0 - a per-part "recorded?" probe. (The bar count it sends for a
#      recorded part depends on earlier loads, so only the 0xEC is used.) SongSelect's MaxTime does not
#      depend on IntSongPart.
#   C. There is no get for MaxTime / song info (get 52 18 -> only the End frame 03 52 18 7F), the 52 group
#      get (52 00) returns only the selected number, and no SysEx reads the 32-byte song name.
#   D. InternalSongDelete 32 52 21 7F [7F|04, hi, lo, part] (apply 0x40160AD4): data[0] must be 7F or 4,
#      song 0-based (>= 10 -> song 1!), part 0 / 1 / 7F = part 1 / part 2 / both (FUN_4016C800), hi:lo = 7F:7F =
#      all songs (FUN_4016C908). Success -> 32 51 7E 7F 00 00 (FUN_4014F294(0)), error -> Result 7E 03 ...
#      The 0 form is silently ignored.
#
# Not modelled: RTOS (queue post / semaphores / delay stubbed), the NOR write job (FUN_40134048 is
# recorded, not run), the panel.
#
#   cd result/stockfw && uv run python -I tools/stock_songs.py [--json out.json]
from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import struct
import sys

sys.dont_write_bytecode = True
_HERE = pathlib.Path(__file__).resolve().parent


def _load(name):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, _HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


p3 = _load("p3_map")
hexs = p3.hexs

REC = 0x40500000              # RecorderData RAM mirror (NOR 0x60500000)
REC_END = 0xFFFFE             # area end offset, marker 5A A5 there
NOTIFY_SEND = 0x4014C048      # notify task body: (descriptor id, b2, b3, data)
NOR_JOB = 0x40134048          # (slot, sectors, src, 0): queue a NOR rewrite for the worker
FLASH_BUSY = 0x40134244       # 1 while a NOR job runs (delete-all checks it)
DELETE_SONG = 0x4016C800      # (song, part 0/1/7F): re-serialises the song from the RECORD track table (the last take) - it removes nothing
FORMAT_ALL = 0x4016C908       # template store + NOR job (InternalSongDelete hi:lo = 7F 7F)
DELETE_OK = "F0 40 7F 32 04 1E 51 7E 7F 00 00 F7"  # FUN_4014F294(0)
DEV = 0x7F


def frame(*body):
    return bytes([0xF0, 0x40, DEV, *body])


# ------------------------------------------------------------------------------------------- song store
def track(bars, notes=4):
    """A minimal Kawai step track: per bar FA 23 (4/4) + `notes` quarter notes, then FC (section 4.2)."""
    t = bytearray()
    for _ in range(bars):
        t += bytes([0xFA, 0x23])
        for n in range(notes):
            t += bytes([60 + n, 0, 0x40, 0x30, 0xF8])
    return bytes(t + b"\xFC")


def song(i, parts):
    """Song i (0-based) with parts = (bars of part 1, bars of part 2); 0 = no track. Empty -> 0x74 B."""
    h = bytearray(0x74)
    h[0:32] = f"SONG {i + 1:02d}".ljust(32).encode()
    struct.pack_into("<I", h, 0x28, 0x10)
    if not any(parts):
        return bytes(h)
    body = bytearray(b"\x07\xA1\x20\x00\xFC")  # start settings block (tempo 500000 us, ..., FC)
    struct.pack_into("<I", h, 0x24, 0x74)
    for k, bars in enumerate(parts):
        if bars:
            struct.pack_into("<I", h, 0x2C + 4 * k, 0x74 + len(body))
            body += track(bars)
    struct.pack_into("<I", h, 0x6C, 0x74 + len(body))
    struct.pack_into("<I", h, 0x70, max(parts))
    b = bytes(h) + bytes(body)
    return b + b"\0" * ((-len(b)) % 4)


def store(layout):
    """RecorderData image (section 4.1) for 10 songs; layout[i] = (bars part 1, bars part 2)."""
    songs = [song(i, layout[i] if i < len(layout) else (0, 0)) for i in range(10)]
    head, offs, cur = bytearray(0x30), [], 0x30
    for s in songs:
        offs.append(cur)
        cur += len(s)
    for i, o in enumerate(offs):
        struct.pack_into("<I", head, 4 * i, o)
    struct.pack_into("<I", head, 0x28, cur)
    struct.pack_into("<I", head, 0x2C, REC_END)
    return bytes(head) + b"".join(songs) + b"\x5A\xA5"


LAYOUT = [(3, 0), (2, 5), (0, 4), (200, 0)]  # songs 1-4 recorded, 5-10 empty


class Piano:
    def __init__(self, layout=LAYOUT):
        self.r = p3.StockRun()
        e = self.e = self.r.emu
        e.write_bytes(REC, store(layout))
        e.write_bytes(REC + REC_END, b"\x5A\xA5")
        self.jobs = []
        e.hook(NOR_JOB, lambda em: (self.jobs.append((em.reg(0), em.reg(1), em.reg(2))), 0)[1])
        e.hook(FLASH_BUSY, lambda em: 0)

    def send(self, msg):
        """Deliver one request, then run the notify task for every queued notification. -> (stop, [frames])."""
        res = self.r.deliver(msg)
        out = [bytes(m) for _p, m in self.r.sent]
        for _q, a in list(self.r.posts):
            m = self.e.read_bytes(a, 16)
            if m[1] == 0xFF:
                continue
            self.r.sent = []
            nres = self.e.call(NOTIFY_SEND, m[0] * 256 + m[1], m[2], m[3], a + 4, max_insns=5_000_000)
            assert nres.ok, nres
            out += [bytes(x) for _p, x in self.r.sent]
        return res.stop, out

    def get(self, v1, v2):
        self.r.request(v1, v2, 0x7F)
        return [bytes(m) for _p, m in self.r.sent]

    def offsets(self):
        return list(struct.unpack_from("<12I", self.e.read_bytes(REC, 0x30)))


def song_select(n):
    return frame(0x10, 0x08, 0x02, 0x52, 0x00, 0x7F, 0, (n >> 7) & 0x7F, n & 0x7F)


def int_song_part(v):
    return frame(0x10, 0x08, 0x02, 0x52, 0x01, 0x7F, v)


def song_delete(d0, n, part):
    return frame(0x32, 0x08, 0x02, 0x52, 0x21, 0x7F, d0, (n >> 7) & 0x7F, n & 0x7F, part)


NODATA = "F0 40 7F 7E 04 1E 03 00 01 6C F7"


def maxtime(bars):
    return f"F0 40 7F 10 04 1E 52 18 7F 00 {(bars >> 7) & 0x7F:02X} {bars & 0x7F:02X} 00 00 00 F7"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", help="write the captured request/reply bytes (app test fixture)")
    args = ap.parse_args()
    fails, capture = [], []

    def check(cond, what):
        print(f"  [{'ok' if cond else 'FAIL'}] {what}")
        if not cond:
            fails.append(what)

    def log(label, req, frames):
        capture.append({"label": label, "tx": hexs(req + b"\xF7"), "rx": [hexs(f) for f in frames]})
        print(f"     {label}: {hexs(req)} -> {[hexs(f) for f in frames]}")

    pn = Piano()
    print(f"A. SongSelect per song (synthetic store: {LAYOUT} + 6 empty)")
    for n in range(10):
        stop, fr = pn.send(song_select(n))
        log(f"select {n + 1}", song_select(n), fr)
        bars = max(LAYOUT[n]) if n < len(LAYOUT) else 0
        rx = [hexs(f) for f in fr]
        check(stop == "returned" and (NODATA in rx) == (bars == 0), f"song {n + 1}: {'0xEC no data' if not bars else 'no error'}")
        check(maxtime(bars) in rx, f"song {n + 1}: MaxTime = {bars} bars")
        check(not pn.jobs, f"song {n + 1}: no NOR write")

        print(f"B. IntSongPart probe on song {n + 1}") if n < 5 else None
        if n < 5:
            for pv in (1, 0, 0x7F):
                stop, fr = pn.send(int_song_part(pv))
                log(f"song {n + 1} part {pv:#x}", int_song_part(pv), fr)
                rx = [hexs(f) for f in fr]
                has = (LAYOUT[n][pv] if pv < 2 else max(LAYOUT[n])) if n < len(LAYOUT) else 0
                # the bar count after a part change is not reliable (depends on earlier loads); only 0xEC is
                check((NODATA in rx) == (not has) and (maxtime(0) in rx) == (not has) and not pn.jobs,
                      f"song {n + 1} IntSongPart {pv:#x}: {'recorded (MaxTime > 0)' if has else '0xEC, MaxTime 0'}, no NOR write")

    print("C. gets: MaxTime / group 52 / name")
    pn.send(song_select(1))
    g = pn.get(0x52, 0x18)
    log("get 52 18", frame(0x02, 0x08, 0x02, 0x52, 0x18, 0x7F), g)
    check([hexs(x) for x in g] == ["F0 40 7F 03 04 1E 52 18 7F F7"], "get MaxTime: End frame only (no value)")
    g = pn.get(0x52, 0x00)
    log("get 52 00", frame(0x02, 0x08, 0x02, 0x52, 0x00, 0x7F), g)
    check("F0 40 7F 10 04 1E 52 00 7F 00 00 01 F7" in [hexs(x) for x in g], "get 52 group: only the selected song number")

    print("D. InternalSongDelete 32 52 21 7F [d0, hi, lo, part] (the edit itself, FUN_4016C800 / FUN_4016C908, is stubbed:")
    print("   it rebuilds the song from the recorder's edit buffers, which only the stock boot path fills)")
    for d0, n, part, ret, label in ((0x00, 1, 0x7F, 0, "d0 = 0"), (0x7F, 1, 0x7F, 0, "song 2 both"), (0x04, 1, 0, 0, "d0 = 4, song 2 part 1"),
                                    (0x7F, 2, 1, 0, "song 3 part 2"), (0x7F, 12, 0x7F, 0, "song 13"), (0x7F, 1, 0x7F, 0xEC, "edit fails 0xEC"),
                                    (0x7F, 0x3FFF, 0x7F, 0, "all songs (7F 7F)")):
        pd = Piano()
        calls = []
        pd.e.hook(DELETE_SONG, lambda em, ret=ret: (calls.append(("song", em.reg(0), em.reg(1))), ret)[1])
        pd.e.hook(FORMAT_ALL, lambda em: (calls.append(("all",)), 0)[1])
        stop, fr = pd.send(song_delete(d0, n, part))
        log(f"delete {label}", song_delete(d0, n, part), fr)
        print(f"       stop {stop}, calls {calls}")
        rx = [hexs(f) for f in fr]
        if d0 == 0x00:
            check(stop == "returned" and fr == [] and calls == [], "d0 = 0 (the form the app tables suggest): ignored, no reply")
        elif n == 0x3FFF:
            check(calls == [("all",)] and rx == [DELETE_OK], "hi:lo 7F 7F: FUN_4016C908 (format all), then 32 51 7E 7F 00 00")
        elif ret:
            check(calls and rx == ["F0 40 7F 7E 04 1E 03 00 01 6C F7"], "edit error 0xEC -> Result 7E 03 00 01 6C")
        else:
            want = (n if n < 10 else 0, part)
            check(calls == [("song", *want)] and rx == [DELETE_OK],
                  f"{label}: FUN_4016C800(song {want[0]}, part {want[1]:#x}), then 32 51 7E 7F 00 00")

    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(capture, indent=1) + "\n")
        print(f"wrote {args.json}")
    print("all checks passed" if not fails else f"{len(fails)} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
