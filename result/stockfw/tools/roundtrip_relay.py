# roundtrip_relay.py - emulator verification of the panel-UI relay of the slot payload ota_main (feat2 0x04;
# relay rules R1-R6, REMOTE_PROTOCOL.md §10.7): a whole .c79u transfer phone -> piano -> panel and back, with the panel modelled.
#
#   cd result/stockfw && uv run python -I tools/roundtrip_relay.py [ota_main] [-q]
#
# Machine: the real ota_loader_v4 image with the payload uploaded and applied live (roundtrip_power.Rig).
#   - phone -> piano: each frame through the stock end handler FUN_40147DB0 for port 2 (-> manufacturer branch
#     0x40147E1C -> resident ng_sysex -> payload), as the BLE-MIDI port 2 hands it over;
#   - piano -> panel: the four TX ring writers are modelled (frame recorded, the u16 byte count of that port's
#     ring advanced as the writer does; check 1 runs the REAL FUN_40144CD0 once); the panel link drains the
#     port 3 ring at PANEL_BPMS bytes per ms; a frame reaches the panel when its last byte has left;
#   - panel -> piano: the panel's reply bytes go into the REAL port 3 RX ring (0x413AB2FC, count 0x413AE596) and the
#     REAL port 3 pump FUN_40148ED8 runs (-> byte parser FUN_40148B24 -> end handler -> resident -> payload);
#   - the panel = a Python model of the loader's receiver (Begin / Data / Commit / Status / Control of §10.7: resume,
#     go-back rc 7, CRC check at Commit) - the real ES5 loader is checked against the same frames in the app tests;
#   - the phone = the app's PanelUiClient.upload algorithm (window W, cumulative acks, rc 7 rewind, ack timeout ->
#     Status -> resume or Begin again, 8 timeouts in a row -> give up), virtual time in 1 ms steps, BLE frames
#     paced at 2.5 B/ms; the engine loop tail (cave_tick) every 8 ms.
# Checks (numbered as printed):
#   1  static: Identify feat2 0x7F (relay 0x04 + startup 0x08 + playback 0x10 + remote off 0x20 + read owner 0x40); the real FUN_40144CD0 run from the payload's relay puts the frame whole into the
#      port 3 ring (count + n, wr + n, bytes equal) with AINTC HIER bit 1 toggled per byte; op 1B 10 answers layout 1
#   2  window 1: a whole package (header + signature + DEFLATE payload) stored by the panel, bytes equal; every frame
#      the panel got == the phone's frame (F0..F7), every reply the phone got == the panel's; no drop
#   3  window 2: the same, faster than window 1; port 3 ring peak <= 0x7FE - 512 (the stock's reserve never touched)
#   4  aborted: Control 4 at 40 % -> the panel idles (Status rx 0), a new upload starts from 0 and finishes; a sender
#      that vanishes at 60 % -> the next upload resumes at the panel's `received`
#   5  the panel is not there (UI not loaded): 3 Begins relayed, no reply, the sender gives up with "no reply"; no
#      drop, no frame anywhere else
#   6  timeout: the panel stalls 5 s mid-transfer (page reload) -> ack timeouts, Status / Begin resume, finishes
#   7  the stock's own panel traffic is untouched: the panel's CONNECT and fn 0x10 sets during the transfer get their
#      stock / core answers on port 3, and the port 3 byte stream parses into whole frames (ours and the stock's,
#      never interleaved); a stock dump filling the port 3 ring makes the relay drop (counted, R4) and the sender
#      recovers
#   8  R3: a 257 B frame, a request from port 3, a reply from port 2 - dropped and counted (1B 10), nothing sent
from __future__ import annotations

import hashlib
import pathlib
import random
import struct
import sys
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rbt = rtlib.load("roundtrip_bt")
rpw = rbt.rpw
rt, pa = rbt.rt, rbt.pa
le, pack7, unpack7 = rtlib.le, rtlib.pack7, rtlib.unpack7

TAG = b"\xF0\x7D\x43\x41\x37\x39"
TX_CNT = {0: 0x413AE5A8, 1: 0x413AE5B4, 2: 0x413AE586, 3: 0x413AE59C}   # relay_stock.h / stock_relay.ld
TX3_RING, TX3_WR = 0x413ABAFC, 0x413AE59A
RX3_RING, RX3_RD, RX3_CNT, RX3_FLAG, PUMP3 = 0x413AB2FC, 0x413AE592, 0x413AE596, 0x413AE5C4, 0x40148ED8
WAI_SEM, SIG_SEM = 0x40086E94, 0x4008819C
LIMIT, RESERVE = 0x7FE, 512
PANEL_BPMS = 3.125     # panel link 31.25 kbaud [unverified]
BLE_BPMS = 2.5         # the app's GattWriter pacing
CHUNK = 200


# ---- package (.c79u: header 80 + signature 64 + payload; REMOTE_PROTOCOL §10.7) -------------------------
def package(seq: int, size: int, seed: int = 1) -> bytes:
    rnd = random.Random(seed)
    words = ["ca79", "panel", "loader", "relay", "tone", "metronome", "reverb", "screen", "bluetooth"]
    text = ('{"format":1,"version":"0.9.%d","js":"' % seq + " ".join(rnd.choice(words) + str(rnd.randrange(999))
                                                                  for _ in range(size // 5)) + '","css":""}').encode()
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    payload = c.compress(text) + c.flush()
    hdr = bytearray(80)
    hdr[0:4] = b"C79U"
    hdr[4], hdr[5], hdr[6] = 1, 1, 1
    struct.pack_into("<IIIII", hdr, 8, seq, len(payload), len(text), zlib.crc32(payload), 0x87C87E16)
    hdr[28:44] = (b"0.9.%d" % seq).ljust(16, b"\0")
    hdr[48:80] = hashlib.sha512(payload).digest()[:32]
    sig = bytes(rnd.randrange(256) for _ in range(64))
    return bytes(hdr) + sig + payload


def frame(op: int, args: bytes = b"") -> bytes:
    return TAG + bytes([op]) + bytes(b & 0x7F for b in args) + b"\xF7"


def data_frame(off: int, chunk: bytes) -> bytes:
    return frame(0x32, bytes([(off >> 14) & 0x7F, (off >> 7) & 0x7F, off & 0x7F]) + pack7(chunk))


# ---- the panel loader's receiver (Python model of panelui/uiloader.js, §10.7) ---------------------------
class Panel:
    def __init__(self):
        self.present = True
        self.stall_until = -1
        self.head = None
        self.length = 0
        self.got = bytearray()
        self.rx = 0              # 0 idle, 1 receiving, 2 complete, 4 stored
        self.stored = {}         # seq -> bytes (head + payload)
        self.frames_in = []
        self.replies = []

    def _status(self) -> bytes:
        r = bytearray(93)
        r[0], r[1], r[2], r[3] = 1, 1, 1, self.rx
        r[5] = 3
        seq = le(self.head, 8) if self.head else 0
        struct.pack_into("<IIII", r, 6, seq, len(self.got), self.length, max(self.stored, default=0))
        r[38], r[39] = CHUNK & 0xFF, CHUNK >> 8
        return frame(0x70, pack7(bytes(r)))

    def handle(self, f: bytes, now: int):
        """A frame from the piano (port 3).  Returns the reply frame or None."""
        self.frames_in.append(f)
        if not self.present or now < self.stall_until or f[:6] != TAG or f[-1] != 0xF7:
            return None
        op, body = f[6], f[7:-1]

        def ack(opr, rc, n):
            return frame(opr, pack7(bytes([rc]) + struct.pack("<I", n)))
        if op == 0x30:
            return self._status()
        if op == 0x31:
            head = unpack7(body)
            if len(head) != 144 or head[:4] != b"C79U":
                return ack(0x71, 11, 0)
            seq = le(head, 8)
            if seq in self.stored and self.stored[seq][:144] == head:
                return ack(0x71, 14, le(head, 12))
            if self.head != head or self.rx not in (1, 2):
                self.head, self.length, self.got, self.rx = head, le(head, 12), bytearray(), 1
            return ack(0x71, 0, len(self.got))
        if op == 0x32:
            if self.rx not in (1, 2):
                return ack(0x72, 8, 0)
            off = (body[0] << 14) | (body[1] << 7) | body[2]
            data = unpack7(body[3:])
            if off < len(self.got):
                return ack(0x72, 0, len(self.got))
            if off > len(self.got):
                return ack(0x72, 7, len(self.got))
            self.got += data
            if len(self.got) >= self.length:
                self.rx = 2
            return ack(0x72, 0, len(self.got))
        if op == 0x33:
            if self.rx != 2:
                return frame(0x73, pack7(bytes([8 if self.rx == 0 else 9, 0])))
            if zlib.crc32(bytes(self.got)) != le(self.head, 20):
                return frame(0x73, pack7(bytes([10, 0])))
            self.stored[le(self.head, 8)] = bytes(self.head) + bytes(self.got)
            self.rx = 4
            return frame(0x73, pack7(bytes([0, 0])))
        if op == 0x34:
            sub = body[0] if body else 0
            if sub == 4:
                self.head, self.length, self.got, self.rx = None, 0, bytearray(), 0
            return frame(0x74, bytes([sub, 0]))
        return None


# ---- the network in virtual time -------------------------------------------------------------------------
class Net:
    def __init__(self, image, syms, data):
        self.rig = rig = rpw.Rig(image, syms)
        rbt.Hier(rig.emu)
        rpw.upload_apply(rig, data)
        rig.tick(2)
        self.emu = emu = rig.emu
        self.now = 0
        self.panel = Panel()
        self.ring3 = bytearray()          # port 3 bytes not yet on the panel link
        self.link3 = bytearray()          # bytes the panel has received, parsed into frames
        self.drain = 0.0
        self.to_phone = []                # frames on port 2 (the phone)
        self.port3_frames = []            # every frame written to port 3 (relay and stock)
        self.other = []                   # frames on ports 0 / 1
        self.peak3 = 0
        self.stock3 = []                  # stock / core frames the panel received
        for p, addr in rt.TX.items():
            emu.hook(addr, (lambda pp: lambda e: self._tx(pp, e.read_bytes(e.reg(0), e.reg(1))))(p))

    def _cnt(self, p):
        return le(self.emu.read_bytes(TX_CNT[p], 2), 0, 2)

    def _tx(self, p, b):
        b = bytes(b)
        if p == 3:
            self.port3_frames.append(b)
            self.ring3 += b
            self.emu.write_bytes(TX_CNT[3], min(len(self.ring3), 0xFFFF).to_bytes(2, "little"))
        elif p == 2:
            self.to_phone.append(b)
        else:
            self.other.append((p, b))

    def phone_send(self, f: bytes):
        rpw.deliver(self.rig, 2, f[:-1])          # the end handler appends F7

    def panel_send(self, f: bytes):
        """The panel's bytes through the real port 3 RX ring and pump."""
        emu = self.emu
        rd, cnt = le(emu.read_bytes(RX3_RD, 2), 0, 2), le(emu.read_bytes(RX3_CNT, 2), 0, 2)
        for i, b in enumerate(f):
            emu.write_bytes(RX3_RING + ((rd + cnt + i) & 0x7FF), bytes([b]))
        emu.write_bytes(RX3_CNT, (cnt + len(f)).to_bytes(2, "little"))
        res = emu.call(PUMP3, max_insns=50_000_000)
        if not res.ok:
            raise RuntimeError(f"port 3 pump stopped: {res}")

    def step(self, ms: int = 1):
        """Time passes: the panel link drains port 3, whole frames reach the panel, its replies come back."""
        for _ in range(ms):
            self.now += 1
            self.peak3 = max(self.peak3, len(self.ring3))
            self.drain += PANEL_BPMS
            n = min(int(self.drain), len(self.ring3))
            self.drain -= n
            if not self.ring3:
                self.drain = 0.0
            self.link3 += self.ring3[:n]
            del self.ring3[:n]
            self.emu.write_bytes(TX_CNT[3], len(self.ring3).to_bytes(2, "little"))
            while True:
                s = self.link3.find(b"\xF0")
                e = self.link3.find(b"\xF7", s + 1) if s >= 0 else -1
                if e < 0:
                    break
                f = bytes(self.link3[s:e + 1])
                del self.link3[:e + 1]
                if f[:6] == TAG:
                    r = self.panel.handle(f, self.now)
                    if r is not None:
                        self.panel.replies.append(r)
                        self.panel_send(r)
                else:
                    self.stock3.append(f)
            if self.now % 8 == 0:
                self.rig.tick()

    def diag(self):
        before = len(self.to_phone)
        self.phone_send(frame(0x1B, b"\x10"))
        r = [f for f in self.to_phone[before:] if f[:8] == TAG + b"\x5B\x10"]
        if len(r) != 1:
            return None
        raw = unpack7(r[0][9:-1])
        return dict(layout=raw[0], src=raw[1], why=raw[2], reserve=raw[3] * 16, cnt3=le(raw, 4, 2), peak3=le(raw, 6, 2),
                    to_panel=le(raw, 8), bytes=le(raw, 12), from_panel=le(raw, 16), d_len=le(raw, 20), d_dir=le(raw, 24),
                    d_nosrc=le(raw, 28), d_full=le(raw, 32), d_ctx=le(raw, 36))

    def close(self):
        self.rig.close()


# ---- the phone: PanelUiClient.upload (result/app/src/protocol/panelui.ts) in virtual time ------------------
class Phone:
    def __init__(self, net: Net, window: int = 2, ack_ms: int = 3000, max_retries: int = 8):
        self.net, self.W, self.ack_ms, self.max_retries = net, window, ack_ms, max_retries
        self.sent = []
        self.seen = 0
        self.retries_total = 0

    def send(self, f: bytes):
        """The frame crosses BLE (paced) while time runs - replies to earlier frames keep arriving meanwhile - and
        reaches the piano's port 2 when its last byte is through."""
        n = self.net
        n.step(max(1, int(len(f) / BLE_BPMS)))
        self.sent.append(f)
        n.phone_send(f)

    def replies(self, op: int):
        out = []
        while self.seen < len(self.net.to_phone):
            f = self.net.to_phone[self.seen]
            self.seen += 1
            if f[:6] == TAG and f[6] == op:
                out.append(f)
        return out

    def request(self, op: int, args: bytes = b"", timeout: int = 1500):
        self.replies(op | 0x40)
        self.send(frame(op, args))
        t0 = self.net.now
        while self.net.now - t0 < timeout:
            self.net.step(1)
            r = self.replies(op | 0x40)
            if r:
                return r[0][7:-1]
        return None

    def status(self):
        d = self.request(0x30)
        if d is None:
            return None
        r = unpack7(d)
        return dict(rx=r[3], seq=le(r, 6), received=le(r, 10), total=le(r, 14))

    def begin(self, head: bytes):
        d = self.request(0x31, pack7(head), 4000)
        if d is None:
            return None
        r = unpack7(d)
        return r[0], le(r, 1)

    def upload(self, file: bytes, stop_at: float | None = None, abort_at: float | None = None):
        head, payload = file[:144], file[144:]
        total, seq = len(payload), le(file, 8)
        b = None
        for _ in range(3):
            b = self.begin(head)
            if b:
                break
        if not b:
            return "no reply", 0
        if b[0] == 14:
            return "installed", total
        if b[0] != 0:
            return f"rc {b[0]}", 0
        base = nxt = b[1]
        rewound, retries = -1, 0
        self.replies(0x72)
        while base < total:
            while nxt < total and nxt - base < self.W * CHUNK:
                n = min(CHUNK, total - nxt)
                self.send(data_frame(nxt, payload[nxt:nxt + n]))
                nxt += n
            if stop_at is not None and base >= total * stop_at:
                return "stopped", base
            if abort_at is not None and base >= total * abort_at:
                self.request(0x34, b"\x04")
                return "aborted", base
            t0, acks = self.net.now, []
            while not acks and self.net.now - t0 < self.ack_ms:
                self.net.step(1)
                acks = self.replies(0x72)
            if not acks:
                retries += 1
                self.retries_total += 1
                if retries > self.max_retries:
                    return "too many timeouts", base
                st = self.status()
                if st and st["rx"] >= 1 and st["seq"] == seq:
                    base = st["received"]
                else:
                    again = self.begin(head)
                    if not again or again[0] != 0:
                        continue
                    base = again[1]
                nxt, rewound = base, -1
                continue
            for a in acks:
                r = unpack7(a[7:-1])
                rc, rec = r[0], le(r, 1)
                if rc == 0:
                    if rec > base:
                        base, retries = rec, 0
                elif rc == 7:
                    if rec != rewound:
                        rewound, base, nxt = rec, max(base, rec), rec
                else:
                    return f"rc {rc}", base
        c = self.request(0x33, b"", 30000)
        if c is None:
            return "no commit reply", total
        c = unpack7(c)
        return ("done" if c[0] == 0 else f"commit rc {c[0]}"), total


def frames_ok(net: Net, phone: Phone):
    """Every frame the panel got from the relay == a phone frame (same bytes, same order); every relayed reply on
    port 2 == a panel reply (same order)."""
    ui_in = [f for f in net.panel.frames_in if 0x30 <= f[6] <= 0x3F]
    ui_out = [f for f in phone.sent if 0x30 <= f[6] <= 0x3F]
    back = [f for f in net.to_phone if f[:6] == TAG and 0x70 <= f[6] <= 0x7F]
    return ui_in == ui_out[:len(ui_in)] and back == net.panel.replies[:len(back)], len(ui_in), len(ui_out), len(back)


def main(argv):
    quiet = "-q" in argv
    img, _stock, _manifest, image, syms = rtlib.loader()
    which = rtlib.first_arg(argv, "ota_main")
    pl = rtlib.payload(which)
    data = pl.data
    ok = True
    print(f"payload {which}  {len(data) - 64} B  sha256 {pa.sha256(data)}  on {img.name}")
    pkg = package(2001, 40_000)
    plen = len(pkg) - 144
    print(f"package: {len(pkg)} B (payload {plen} B DEFLATE, {-(-plen // CHUNK)} Data frames)")

    def check(name, cond, detail=""):
        return rtlib.check(name, cond, detail, quiet)

    # ---- 1. static: identify, the real port 3 writer, 1B 10 ------------------------------------------------
    net = Net(image, syms, data)
    emu = net.emu
    before = len(net.to_phone)
    net.phone_send(frame(0x01))
    ident = [f for f in net.to_phone[before:] if f[:7] == TAG + b"\x41"]
    emu.unhook(rt.TX[3])                                # the real FUN_40144CD0 this time
    emu.hook(WAI_SEM, lambda e: 0)                      # the port semaphore: free
    emu.hook(SIG_SEM, lambda e: 0)
    hier = []
    emu.on_periph_write(0xFFFEF500, 0xFFFEF504, lambda e, a, s, v: hier.append(v & 3))
    for a in (TX_CNT[3], TX3_WR):
        emu.write_bytes(a, b"\x00\x00")
    f = frame(0x30)
    net.phone_send(f)
    cnt, wr = le(emu.read_bytes(TX_CNT[3], 2), 0, 2), le(emu.read_bytes(TX3_WR, 2), 0, 2)
    ring = bytes(emu.read_bytes(TX3_RING, len(f)))
    emu.unhook(WAI_SEM)
    emu.unhook(SIG_SEM)
    emu.write_bytes(TX_CNT[3], b"\x00\x00")
    emu.hook(rt.TX[3], lambda e: net._tx(3, e.read_bytes(e.reg(0), e.reg(1))))
    dg = net.diag()
    ok &= check("1 identify feat2 0x7F (relay 0x04, startup 0x08, playback 0x10, remote off 0x20, read owner 0x40); the real FUN_40144CD0 called by the relay puts the 8 B Status request "
                "whole into the port 3 ring (count 8, wr 8, bytes equal), HIER bit 1 off/on per byte; 1B 10 layout 1, "
                "reserve 512, 1 frame to the panel",
                len(ident) == 1 and ident[0][7:-1][:6] == bytes([pl[0].ngver, 2, 19, 0x3B, 0x77, 0x7F]) and cnt == len(f) and wr == len(f)
                and ring == f and hier[:4] == [1, 3, 1, 3] and len(hier) == 2 * len(f) and dg is not None
                and dg["layout"] == 1 and dg["reserve"] == RESERVE and dg["to_panel"] == 1 and dg["src"] == 3,
                f"identify {ident and ident[0].hex()} count {cnt} wr {wr} hier {hier[:6]} diag {dg}")
    net.close()

    # ---- 2 / 3. window 1 and 2 ------------------------------------------------------------------------------
    times = {}
    for w in (1, 2):
        net = Net(image, syms, data)
        ph = Phone(net, window=w)
        t0 = net.now
        res, _ = ph.upload(pkg)
        times[w] = net.now - t0
        stored = net.panel.stored.get(2001)
        same, n_in, n_out, n_back = frames_ok(net, ph)
        dg = net.diag()
        ok &= check(f"{w + 1} window {w}: {res} in {times[w] / 1000:.1f} s (virtual), stored bytes == package; "
                    f"{n_in} frames to the panel == the phone's, {n_back} replies back == the panel's; 1B 10 counts them, "
                    f"no drop; port 3 ring peak {net.peak3} B <= {LIMIT - RESERVE}; nothing on ports 0 / 1",
                    res == "done" and stored == pkg and same and n_in == n_out and n_back == len(net.panel.replies)
                    and dg is not None and dg["to_panel"] == n_in and dg["from_panel"] == n_back
                    and dg["d_len"] + dg["d_dir"] + dg["d_nosrc"] + dg["d_full"] + dg["d_ctx"] == 0
                    and net.peak3 <= LIMIT - RESERVE and dg["peak3"] <= LIMIT - RESERVE and not net.other
                    and max(len(x) for x in ph.sent) <= 240,
                    f"{res} in {n_in}/{n_out} back {n_back}/{len(net.panel.replies)} diag {dg} peak {net.peak3}")
        net.close()
    ok &= check("3b window 2 is faster than window 1", times[2] < times[1], f"{times}")

    # ---- 4. aborted -------------------------------------------------------------------------------------------
    net = Net(image, syms, data)
    ph = Phone(net, window=2)
    r1, at1 = ph.upload(pkg, abort_at=0.4)
    st1 = ph.status()
    r2, _ = ph.upload(pkg)
    first_begin = [f for f in net.panel.replies if f[6] == 0x71]
    resumed_from = le(unpack7(first_begin[-1][7:-1]), 1) if first_begin else None
    ok_a = r1 == "aborted" and st1 is not None and st1["rx"] == 0 and r2 == "done" and resumed_from == 0 \
        and net.panel.stored.get(2001) == pkg
    net.close()
    net = Net(image, syms, data)
    ph = Phone(net, window=2)
    r3, at3 = ph.upload(pkg, stop_at=0.6)
    net.step(500)
    ph2 = Phone(net, window=2)
    ph2.seen = len(net.to_phone)
    r4, _ = ph2.upload(pkg)
    begins = [le(unpack7(f[7:-1]), 1) for f in net.panel.replies if f[6] == 0x71]
    ok_b = r3 == "stopped" and r4 == "done" and len(begins) == 2 and begins[1] >= at3 > 0 and net.panel.stored.get(2001) == pkg
    dg = net.diag()
    ok &= check("4 aborted: Control 4 at 40 % -> panel rx idle, the next upload begins at 0 and finishes; a sender that "
                "vanishes at 60 % -> the next one resumes at the panel's received and finishes; no drop",
                ok_a and ok_b and dg is not None and dg["d_full"] + dg["d_dir"] + dg["d_len"] == 0,
                f"abort {r1}@{at1} st {st1} then {r2} from {resumed_from}; vanish {r3}@{at3} then {r4} begins {begins}")
    net.close()

    # ---- 5. the panel is not there --------------------------------------------------------------------------
    net = Net(image, syms, data)
    net.panel.present = False
    ph = Phone(net, window=2)
    res, _ = ph.upload(pkg)
    st = ph.status()
    dg = net.diag()
    ok &= check("5 no panel UI: 3 Begins (+ 1 Status) relayed to port 3, no reply; the sender says 'no reply'; the relay "
                "drops nothing, sends nothing to ports 0-2",
                res == "no reply" and st is None and dg is not None and dg["to_panel"] == 4 and dg["from_panel"] == 0
                and dg["d_len"] + dg["d_dir"] + dg["d_nosrc"] + dg["d_full"] == 0 and not net.other
                and not [f for f in net.to_phone if f[:6] == TAG and 0x70 <= f[6] <= 0x7F],
                f"{res} status {st} diag {dg}")
    net.close()

    # ---- 6. timeout: the panel stalls 5 s mid-transfer ------------------------------------------------------
    net = Net(image, syms, data)
    ph = Phone(net, window=2)
    orig = net.panel.handle
    state = {"armed": True}

    def handle(f, now, _orig=orig):
        if state["armed"] and f[6] == 0x32 and len(net.panel.got) > plen // 2:
            state["armed"] = False
            net.panel.stall_until = now + 5000
        return _orig(f, now)
    net.panel.handle = handle
    res, _ = ph.upload(pkg)
    ok &= check("6 timeout: the panel stalls 5 s at 50 % -> the sender's ack timeouts, Status / Begin resume, done; "
                "stored bytes == package",
                res == "done" and ph.retries_total >= 1 and net.panel.stored.get(2001) == pkg and not state["armed"],
                f"{res} retries {ph.retries_total}")
    net.close()

    # ---- 7. the stock's panel traffic during a transfer, and a full ring ---------------------------------------
    net = Net(image, syms, data)
    ph = Phone(net, window=2)
    kawai = []
    orig = net.panel.handle

    def handle7(f, now, _orig=orig):
        r = _orig(f, now)
        n_data = sum(1 for x in net.panel.frames_in if x[6] == 0x32)
        if n_data in (5, 40, 80) and f[6] == 0x32:
            # the panel's own traffic: CONNECT, then a set (as its JS does), through the same port 3 RX chain
            msg = bytes.fromhex("F0407F7E080201 01F7".replace(" ", "")) if n_data == 5 else \
                bytes.fromhex("F0407F100802550800%02XF7" % (n_data & 0x3F))
            kawai.append(msg)
            net.panel_send(msg)
        if n_data == 60 and f[6] == 0x32:
            # a stock dump in flight: the port 3 ring almost full for 300 ms
            net.ring3 += bytes([0xF0, 0x40, 0x7F, 0x10]) + bytes(1500) + b"\xF7"
        return r
    net.panel.handle = handle7
    res, _ = ph.upload(pkg)
    stream_ok = all(f[0] == 0xF0 and f[-1] == 0xF7 and 0xF0 not in f[1:-1] and 0xF7 not in f[1:-1]
                    for f in net.port3_frames)
    stock_to_panel = [f for f in net.port3_frames if f[1] == 0x40]
    dg = net.diag()
    ok &= check("7 stock traffic: the panel's CONNECT and sets mid-transfer get their answers on port 3 (whole frames, "
                "never cut by ours), the port 3 stream is whole frames only; a ring filled by a stock dump -> the relay "
                "drops (R4, counted) instead of waiting, the sender recovers and the package is stored",
                res == "done" and stream_ok and len(stock_to_panel) >= 3 and net.panel.stored.get(2001) == pkg
                and dg is not None and dg["d_full"] >= 1 and dg["peak3"] + 240 + RESERVE > LIMIT,
                f"{res} stock->panel {len(stock_to_panel)} {[x[:8].hex() for x in stock_to_panel[:4]]} diag {dg}")
    net.close()

    # ---- 8. R3 / length -----------------------------------------------------------------------------------------
    net = Net(image, syms, data)
    net.phone_send(frame(0x32, bytes(249)))        # 257 B
    net.panel_send(frame(0x31, b"\x01"))           # a request from the panel
    net.phone_send(frame(0x72, b"\x00"))           # a reply from the phone's port
    net.panel_send(frame(0x70, b"\x00"))           # a reply nobody asked for
    net.step(20)
    dg = net.diag()
    ok &= check("8 R3: 257 B frame, request from port 3, reply from port 2, orphan reply -> dropped and counted "
                "(len 1, dir 2, no source 1), nothing relayed",
                dg is not None and dg["d_len"] == 1 and dg["d_dir"] == 2 and dg["d_nosrc"] == 1 and dg["to_panel"] == 0
                and dg["from_panel"] == 0 and not net.port3_frames and not net.other,
                f"diag {dg} port3 {len(net.port3_frames)}")
    net.close()

    return rtlib.finish(ok, " - emulation only: TX writers, panel link and panel loader modelled (FUN_40144CD0 real "
                            "in check 1, port 3 RX ring + pump real); no interrupts, no RTOS scheduling.")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
