# roundtrip_fread.py - fn 0x24 file read with 7-bit DataPacket counters (ota_main, ports/cave/fread_cave.c).
#
#   cd result/stockfw && uv run python -I tools/roundtrip_fread.py [ota_main] [-q]
#
# Machine as roundtrip_xgate.py (roundtrip_power's Rig: the real ota_loader_v4 image on StockEmu, the payload uploaded
# over the 0x7D OTA ops and applied live, the resident's real poke writing the hook words).  The engine-loop site
# #15 0x40143ACC is run from its `bl` (Rig.site) with a Read job due (job code 4 at 0x413CA3EA, executor-due byte
# 0x413CA3EB = 1, the name at 0x413C9FC4) - on H (ota_main applied: hk_fr_file -> frc_pass) and on S (the same
# loader, no payload: the stock executor FUN_4014F364 -> FUN_4014D974).  Stock code everywhere except the core's
# packet loop; models: the FAT size / read FUN_400A9238 / FUN_400A9408 give the test file, the ACK wait FUN_4014B4E0
# answers ACK (0) at once or NAK (1) where a check says so, wai_sem / sig_sem / dly_tsk return at once (counted).
# Checks (numbered as printed):
#   1  static: hk_fr_file = push, bl frc_pass, cmp, pop, bxeq lr, `b` FUN_4014F364 (= the bl target of the site word);
#      after the apply the site word is `bl hk_fr_file`
#   2  H, a 4000-byte file (191 packets): every TX frame F0 .. F7 has no byte >= 0x80 inside; TopOfData 01 with the
#      length; DataPackets counters 0..7F, 00..3E (wrap 7F -> 00), each checksum = XOR(bytes 1..n-3) & 0x7F (app
#      readPacket), the unpacked bytes = the file; then EndOfData 7B and Result 32 51 7E 7F 00 00; semaphores
#      taken / given in pairs; job and due bytes 0
#   3  S, the same file on the unpatched stock: the 129th DataPacket carries counter 0x80 raw (a status byte inside
#      the SysEx -> check 2's rule fails), the stock stops after 129 packets (2709 B) and still sends 7B + Result ok
#   4  H = S frame for frame (S's counter byte & 0x7F) for sizes the stock sends whole, <= 129 packets: 0, 1, 6, 7, 20, 21,
#      14, 15, 20, 21, 22, 42, 2688 (an empty 129th packet, counter 0x00 after 0x7F), 2702; packets = the first c + 1 with
#      size <= 21 c + 14 (the stock builder marks the last packet only when one of its three groups starts at >= size)
#   5  H, NAK on packet 130 once -> the stock sender resends the same frame, the transfer completes
#   6  H, Cancel (0x413CA3E7 = 1) seen after packet 5 -> Result 7E 03 00 01 70 (0xF0), no 7B, semaphores given back
#   7  H, a job that is not a Read (delete, job 6) and a pass with nothing due go to the stock executor (Result ok /
#      nothing sent), the core's read count unchanged
# Cancel and the write side:
#   8  Cancel (24 7D) sent INSTEAD of the ACK of packet 5 (the ACK wait times out, the flag 0x413CA3E7 is set - app head()):
#      H: packet 5 sent once, Result 7E 03 00 01 70 at once, binding kept, job 0, semaphores in pairs;
#      S (stock executor): packet 5 sent 16 times (the stock wait FUN_4014B4E0 never looks at the flag), then the binding
#      is dropped (0x413CA3DE = 0) - the field's Busy-until-re-CONNECT
#   9  H upload 24 05 01 00 + name, executor (job 5), TopOfData, 6000 B in app-format DataPackets (counter & 0x7F, wraps
#      twice), 24 7B, the writer FUN_4014FA30: every packet ACKed, upload state 2 after the last, 7B ACK only, the writer
#      calls FUN_400A92C0(name, 0x40B00000, 6000) with the file byte-exact and returns 0
#  10  S the same: packets 128 and 256 (counter 0) land on offset 0 again (the buffer start = bytes 5376.. of the file);
#      the short last packet 285 is read with counter 29's full 7-byte groups -> NAK + Result 0xF0, upload reset (state 0);
#      7B -> ACK + Result 7E 03 00 01 6D (0xED), the writer writes nothing (0xED)
#  11  H = S (TX bytes, writer call, buffer) where the stock works: 20, 21, 42, 2681 B (an empty packet after 20 / 21: no group
#      of the last packet reaches the length otherwise); files of 1 / 6 B fail on both (the receiver reads a 7-byte group
#      from offset 0: NAK + 0xF0, a stock limit); a bad checksum on packet 2 of 100 B: NAK + 0xF0, upload reset, the next
#      packet ACK + 0xED, nothing written
#  12  H 5376 B (256 x 21: the empty packet 256 has counter 0) with packet 200 delivered twice (resend): written byte-exact
# After the field failure of 213ae247 (field test 2026-11-16 - the front answered DataPackets itself, the piano
# went silent at the first one; not reproduced here), the front only rewrites the frame for the stock receiver:
#  13  static: frc_rx / fw_commit / fw_rx call nothing (no bl / blx out of them but to each other); H uploads of 100, 2681, 6000 B
#      and a bad checksum on packet 200: every ACK / NAK comes from the stock dispatcher FUN_4014A2C8 (the caller of
#      FUN_4014B8A0 / FUN_4014B778 is 0x4014AA00 / 0x4014AA14), one per name / data packet; the bad packet -> NAK + Result 0xF0, upload
#      reset (state 0, length 0), nothing moved after it, 7B -> 0xED
#  15  retry budget (long-read retries): H, packet 130 NAKed 20 / 40 / 63 times then ACKed -> 191 + n sends, the file
#      byte-exact, 7B + Result ok; 64 NAKs -> 64 sends of it, then failed (binding dropped, no 7B); packet 5 never answered
#      -> 16 sends as the stock (check 8 S), binding dropped; 20 NAKs then silence -> 20 + 16 sends (NAKs do not use up
#      the silence budget)
#  14  field replay on the field's port layout: panel CONNECT (port 3, the stock binding) and app CONNECT (port 2), the
#      file lease, the field's frames (24 05 01 00, name, TopOfData 00 00 00 64, DataPacket 02 00 00 00 01 02 .., data i & 0xFF)
#      for 100 B and 6000 B, binary semaphores (0x4136D820 / 0x413CA384: count 1, max 1 - a wait on 0 is a hang); every request /
#      packet answered, no wait on a taken semaphore, the writer writes the file, afterwards the app's CONNECT is answered
from __future__ import annotations

import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rtlib  # noqa: E402  (tools/rtlib.py: the shared round-trip harness)

rpw = rtlib.load("roundtrip_power")
rh, rp, ro, images = rpw.rh, rpw.rp, rpw.ro, rpw.images
check = rtlib.check

SITE, WORD, EXECUTOR = 0x40143ACC, 0xEB002E24, 0x4014F364
JOB, DUE, NAME, CANCEL, BINDING = 0x413CA3EA, 0x413CA3EB, 0x413C9FC4, 0x413CA3E7, 0x413CA3DE
FAT_SIZE, FAT_READ, FAT_DELETE = 0x400A9238, 0x400A9408, 0x400A94BC
ACK_WAIT, WAI_SEM, SIG_SEM = 0x4014B4E0, 0x40086E94, 0x4008819C
UP_STATE, UP_LEN, UP_FLAG, BUF = 0x413CA3E5, 0x413CA3B4, 0x413CA3ED, 0x40B00000
FAT_WRITE, WRITER, WAKE = 0x400A92C0, 0x4014FA30, 0x4012854C
ACK = bytes.fromhex("F0407F24041E7FF7")
NAK = bytes.fromhex("F0407F24041E7EF7")
RESULT_ED = bytes.fromhex("F0407F7E041E0300016DF7")
APP = 2
HEAD = bytes.fromhex("F0407F24041E")
RESULT_OK = bytes.fromhex("F0407F32041E517E7F0000F7")
RESULT_F0 = bytes.fromhex("F0407F7E041E03000170F7")
NAMESTR = "\\SONG_01.MID"


def test_file(n):
    return bytes((i * 37 + (i >> 3)) & 0xFF for i in range(n))   # every bit-7 pattern, not periodic in 21


def xor_chk(f):
    x = 0
    for b in f[1:len(f) - 2]:
        x ^= b
    return x & 0x7F


def unpack78(body):
    """app fileaccess.ts unpack78: groups of [hi bits (byte k = bit 6 - k, FUN_4014D700), up to 7 data bytes]"""
    out, i = [], 0
    while i < len(body):
        hi = body[i]
        g = body[i + 1:i + 8]
        out += [b | (0x80 if hi & (0x40 >> k) else 0) for k, b in enumerate(g)]
        i += 1 + len(g)
    return bytes(out)


def clean(f):
    """the BT module's view: one SysEx = F0 .. F7 with nothing >= 0x80 between"""
    return f[0] == 0xF0 and f[-1] == 0xF7 and all(b < 0x80 for b in f[1:-1])


class Run:
    """One Rig (ota_loader_v4), payload applied (h) or not, a file behind the FAT model."""

    def __init__(self, image, syms, data, h):
        self.rig = rig = rpw.Rig(image, syms)
        e = self.e = rig.emu
        e.hook(ro.DLY, lambda em: None)
        if h:
            rc, _st = rpw.upload_apply(rig, data)
            assert rc == 0, rc
        else:
            rig.pn.boot()
        e.write_bytes(BINDING, bytes([APP + 1]))
        rig.tick(2)
        self.file = b""
        self.sem = [0, 0]
        self.nak_at = set()          # DataPacket counters (full) to NAK once
        self.nak_n = {}              # DataPacket number -> NAKs before it is ACKed (a damaged frame resent damaged)
        self.silent_at = None        # DataPacket number whose ACK wait always times out (the controller gone)
        self.cancel_after = None
        self.cancel_instead = None   # packet number answered by Cancel instead of the ACK (the wait times out)
        self.sent_data = 0
        self.writes = []
        self.deletes = 0

        def size(em):
            em.write_u32(em.reg(1), len(self.file))
            return 0

        def read(em):
            em.write_bytes(em.reg(1), self.file)
            return 0

        def ack_wait(em):
            last = self.rig.pn.sent[-1][1] if self.rig.pn.sent else b""
            if last.startswith(HEAD) and last[6] == 0x02:
                k = self.sent_data
                self.sent_data += 1
                if k in self.nak_at:
                    self.nak_at.discard(k)
                    self.sent_data -= 1          # the resend is the same packet
                    return 1
                if self.nak_n.get(k, 0) > 0:
                    self.nak_n[k] -= 1
                    self.sent_data -= 1
                    return 1
                if k == self.silent_at:
                    self.sent_data -= 1
                    return 2
                if self.cancel_after is not None and k == self.cancel_after:
                    em.write_bytes(CANCEL, b"\x01")
                if self.cancel_instead is not None and k == self.cancel_instead:
                    self.sent_data -= 1          # no ACK: a resend would be the same packet
                    em.write_bytes(CANCEL, b"\x01")
                    return 2
            return 0

        def delete(em):
            self.deletes += 1
            return 0
        e.hook(FAT_SIZE, size)
        e.hook(FAT_READ, read)
        e.hook(FAT_DELETE, delete)
        e.hook(FAT_WRITE, lambda em: (self.writes.append((em.read_cstr(em.reg(0)), em.reg(1), em.reg(2),
                                                           em.read_bytes(em.reg(1), em.reg(2)))), 0)[1])
        e.hook(WAKE, lambda em: 0)
        e.hook(ACK_WAIT, ack_wait)
        e.hook(WAI_SEM, lambda em: (self.sem.__setitem__(0, self.sem[0] + 1), 0)[1])
        e.hook(SIG_SEM, lambda em: (self.sem.__setitem__(1, self.sem[1] + 1), 0)[1])

    def job(self, code, file=b"", due=1):
        self.file = file
        self.sent_data = 0
        self.sem = [0, 0]
        e = self.e
        e.write_bytes(NAME, NAMESTR.encode() + b"\0")
        e.write_bytes(JOB, bytes([code]))
        e.write_bytes(DUE, bytes([due]))
        e.write_bytes(BINDING, bytes([APP + 1]))
        self.rig.pn.sent.clear()
        res = self.rig.site(SITE)
        if res.stop != "until":
            raise AssertionError(f"site {SITE:#x}: {res}")
        frames = [bytes(m) for p, m in self.rig.pn.sent if p == APP]
        self.rig.pn.sent.clear()
        return frames, (e.read_bytes(JOB, 1)[0], e.read_bytes(DUE, 1)[0])


    def rx(self, msg):
        """one app frame (F0 .. without F7) through the stock end handler -> 0x40147E94 hook -> front / stock"""
        rpw.deliver(self.rig, APP, msg)
        out = [bytes(m) for p, m in self.rig.pn.sent if p == APP]
        self.rig.pn.sent.clear()
        return out

    def clock_ms(self, ms):
        """time for the session core: the 2 ms counter 0x4139E796 and one engine pass"""
        self.t_ms = getattr(self, "t_ms", 1000) + ms
        self.e.write_bytes(0x4139E796, ((self.t_ms // 2) & 0xFFFF).to_bytes(2, "little"))
        self.rig.tick(1)

    def field_connect(self):
        """the field's ports: the panel (port 3) and the app (port 2) CONNECTed, the panel the stock binding"""
        self.e.write_bytes(BINDING, b"\x00")
        out = []
        for port in (3, 2, 3):
            rpw.deliver(self.rig, port, CONNECT)
            out += [(pp, bytes(m)) for pp, m in self.rig.pn.sent]
            self.rig.pn.sent.clear()
            self.clock_ms(1000)
        return out

    def field_upload(self, data):
        """the app's write() as on the field (fileaccess.ts): request, name, settle, TopOfData, packets, 7B; replies per frame"""
        e, n = self.e, len(data)
        self.writes = []
        self.clock_ms(1000)
        rep = [("req", self.rx(fa(0x05, 0x01, 0x00)))]
        self.clock_ms(30)
        for q in name_packets(NAMESTR):
            rep.append(("name", self.rx(q)))
            self.clock_ms(30)
        self.clock_ms(200)
        self.rig.site(SITE)                       # the executor (job 5) during the settle
        self.rig.pn.sent.clear()
        rep.append(("top", self.rx(fa(0x01, (n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F))))
        self.clock_ms(30)
        for q in app_packets(data):
            rep.append(("data", self.rx(q)))
            self.clock_ms(30)
        st = e.read_bytes(UP_STATE, 1)[0]
        rep.append(("end", self.rx(fa(0x7B))))
        r = self.rig.emu.call(WRITER, max_insns=20_000_000)
        self.rig.pn.sent.clear()
        return rep, st, r.r0, list(self.writes)

    def upload(self, data, packets=None, dup=None, bad=None):
        """24 05 01 00 + name, executor, TopOfData, DataPackets, 7B, writer -> dict"""
        e = self.e
        self.writes = []
        e.write_bytes(UP_STATE, b"\x00")
        e.write_bytes(JOB, b"\x00")
        e.write_bytes(BINDING, bytes([APP + 1]))
        e.write_bytes(BUF, bytes(64))
        self.rig.pn.sent.clear()
        tx = self.rx(fa(0x05, 0x01, 0x00))
        for q in name_packets(NAMESTR):
            tx += self.rx(q)
        self.rig.site(SITE)                       # the executor: job 5 -> 0x413CA3ED
        tx += [bytes(m) for p, m in self.rig.pn.sent if p == APP]
        self.rig.pn.sent.clear()
        tx += self.rx(fa(0x01, (len(data) >> 21) & 0x7F, (len(data) >> 14) & 0x7F, (len(data) >> 7) & 0x7F, len(data) & 0x7F))
        pk = packets if packets is not None else app_packets(data)
        acks, n = [], 0
        for i, q in enumerate(pk):
            if bad is not None and i == bad:
                q = q[:-1] + bytes([q[-1] ^ 1])
            acks += self.rx(q)
            if dup is not None and i == dup:
                acks += self.rx(q)
            n += 1
        st = e.read_bytes(UP_STATE, 1)[0]
        end = self.rx(fa(0x7B))
        r = self.rig.emu.call(WRITER, max_insns=20_000_000)
        self.rig.pn.writes.extend(r.writes)
        wtx = [bytes(m) for p, m in self.rig.pn.sent if p == APP]
        self.rig.pn.sent.clear()
        return dict(tx=tx, acks=acks, state=st, end=end, writer=r.r0, writes=list(self.writes), wtx=wtx,
                    buf=e.read_bytes(BUF, 64), n=n)


DISPATCH = (0x4014A2C8, 0x4014AC98)   # FUN_4014A2C8 (the stock fn 0x24 answers are called from inside it)
STK_ACK, STK_NAK = 0x4014B8A0, 0x4014B778
CONNECT = bytes.fromhex("F0407F7E08020101")


def answer_log(R):
    """hook the stock ACK / NAK senders (run as they are) and log their callers"""
    log = []
    for a, k in ((STK_ACK, "ack"), (STK_NAK, "nak")):
        R.e.hook(a, (lambda kk: lambda em: (log.append((kk, em.reg(14))), rpw.rt.se.StockEmu.CONTINUE)[1])(k))
    return log


def binary_sems(R):
    """the file / sender semaphores as NORTi binary semaphores (cre_sem {0, 1, 1} at 0x4012859C / 0x4014F928): a wait on a
    taken one would block task 19 / 24 for ever - logged and stopped"""
    e = R.e
    ids = {0x11: "file", 0x12: "tx"}
    e.write_u32(0x4136D820, 0x11)
    e.write_u32(0x413CA384, 0x12)
    R.cnt, R.blocks = {0x11: 1, 0x12: 1}, []

    def wai(em):
        i = em.reg(0)
        if i in R.cnt:
            if not R.cnt[i]:
                R.blocks.append((ids[i], hex(em.reg(14))))
                em.stop("blocked")
                return 0
            R.cnt[i] = 0
        return 0

    def sig(em):
        if em.reg(0) in R.cnt:
            R.cnt[em.reg(0)] = 1
        return 0
    e.hook(WAI_SEM, wai)
    e.hook(SIG_SEM, sig)


def fa(*body):
    return bytes([0xF0, 0x40, 0x7F, 0x24, 0x08, 0x02, *body])


def pack87(raw):
    """app fileaccess.ts pack87"""
    out = []
    for i in range(0, len(raw), 7):
        g = raw[i:i + 7]
        out.append(sum((b & 0x80) >> (k + 1) for k, b in enumerate(g)))
        out += [b & 0x7F for b in g]
    return out


def app_packet(v1, counter, raw):
    """app fileaccess.ts packet(): counter & 0x7F, checksum XOR(bytes 1..) & 0x7F (frame without F7)"""
    m = [0xF0, 0x40, 0x7F, 0x24, 0x08, 0x02, v1, counter & 0x7F, *pack87(raw)]
    x = 0
    for b in m[1:]:
        x ^= b
    return bytes(m + [x & 0x7F])


def app_packets(data):
    """app fileaccess.ts dataPackets(): 21 B each, one empty packet more when no group of the last one starts at or past the
    length (length % 21 = 0 or > 14: the stock receiver completes only on such a group)"""
    out = [app_packet(0x02, c, data[i:i + 21]) for c, i in enumerate(range(0, len(data), 21))]
    if len(data) % 21 == 0 or len(data) % 21 > 14:
        out.append(app_packet(0x02, len(out), b""))
    return out


def name_packets(name):
    b = name.encode() + b"\0"
    return [app_packet(0x03, c, b[i:i + 21]) for c, i in enumerate(range(0, len(b), 21))]


def analyse(frames, file):
    """-> dict: data packets (dedup of resends), counters, checks"""
    data = [f for f in frames if f.startswith(HEAD) and len(f) > 7 and f[6] == 0x02]
    uniq = []
    for f in data:
        if not uniq or uniq[-1] != f:
            uniq.append(f)
    ctrs = [f[7] for f in uniq]
    body = b"".join(unpack78(f[8:-2]) for f in uniq)
    return dict(n=len(uniq), sent=len(data), ctrs=ctrs, clean=all(clean(f) for f in frames),
                chk=all(xor_chk(f) == f[-2] for f in uniq), body_ok=body == file, got=len(body),
                top=[f for f in frames if f.startswith(HEAD) and f[6] == 0x01],
                end=[f for f in frames if f.startswith(HEAD) and f[6] == 0x7B],
                tail=frames[-1:] and frames[-1], dirty=[f.hex() for f in frames if not clean(f)][:1])


def top_of(n):
    return HEAD + bytes([0x01, (n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F, 0xF7])


def main(argv):
    name = rtlib.first_arg(argv, "ota_main")
    p, data, _pay, plen, pelf, psyms = rtlib.payload(name)
    _i3, _s3, _m3, image3, syms3 = rtlib.loader()
    ok = True
    print(f"payload {name}  {plen} B  sha256 {ro.pa.sha256(data)}  on ota_loader_v4")

    # ---- 1 static ----
    ins = rtlib.instructions(pelf)
    lo = psyms["hk_fr_file"]
    b = [(m, o) for a, m, o in ins if lo <= a < lo + 24]
    stub = (len(b) == 6 and b[0][0] == "push" and b[1][0] in ("bl", "blx") and int(b[1][1].split()[0], 16) == psyms["frc_pass"]
            and b[4][0] == "bxeq" and b[5][0] == "b" and int(b[5][1].split()[0], 16) == EXECUTOR == rh.bl_target(SITE, WORD))
    H = Run(image3, syms3, data, True)
    word = H.e.read_u32(SITE)
    ok &= check(f"1 hk_fr_file push / bl frc_pass / cmp / pop / bxeq lr / b FUN_4014F364; site word after the apply {word:#010x}",
                stub and word == rh.bl_word(SITE, WORD, psyms["hk_fr_file"]), f"{b}")

    # ---- 2 H 4000 B ----
    f4 = test_file(4000)
    fr, jd = H.job(4, f4)
    a = analyse(fr, f4)
    want = list(range(128)) + list(range(191 - 128))
    ok &= check(f"2 H 4000 B: {len(fr)} frames all clean {a['clean']} {a['dirty']}; TopOfData {a['top'][:1] == [top_of(4000)]}; "
                f"{a['n']} DataPackets counters {a['ctrs'][126:131]}… last {a['ctrs'][-1]:#x}, checksums {a['chk']}, "
                f"unpacked {a['got']} B = file {a['body_ok']}; 7B {len(a['end'])}, last {a['tail'].hex()}; sem {H.sem}; job/due {jd}",
                a["clean"] and a["top"] == [top_of(4000)] and a["n"] == 191 and a["ctrs"] == want and a["chk"]
                and a["body_ok"] and len(a["end"]) == 1 and a["tail"] == RESULT_OK and H.sem[0] == H.sem[1] and jd == (0, 0))

    # ---- 3 S unpatched ----
    S = Run(image3, syms3, data, False)
    fr, jd = S.job(4, f4)
    s = analyse(fr, f4)
    ok &= check(f"3 S (stock, no payload) 4000 B: clean {s['clean']} (first bad {s['dirty'][0][:40] if s['dirty'] else None}…); "
                f"{s['n']} DataPackets, counters {s['ctrs'][126:]}, {s['got']} B of 4000; 7B {len(s['end'])}, last {s['tail'].hex()} "
                f"-> the stock fails check 2's rule",
                not s["clean"] and s["n"] == 129 and s["ctrs"][-1] == 0x80 and s["got"] == 129 * 21
                and len(s["end"]) == 1 and s["tail"] == RESULT_OK)

    # ---- 4 edges: H = S frame for frame where the stock works (<= 129 packets), and the wrap rule above ----
    res4 = []
    for n in (0, 1, 6, 7, 14, 15, 20, 21, 22, 42, 2688, 2702):
        f = test_file(n)
        fr, _jd = H.job(4, f)
        fs, _js = S.job(4, f)
        fs = [g[:7] + bytes([g[7] & 0x7F]) + g[8:] if g.startswith(HEAD) and len(g) > 8 and g[6] == 0x02 else g for g in fs]
        a = analyse(fr, f)
        npk = max(0, -(-(n - 14) // 21)) + 1   # the builder marks the last packet when a group starts at >= size
        res4.append((n, a["n"], a["ctrs"][-2:], fr == fs and a["clean"] and a["chk"] and a["n"] == npk
                     and a["ctrs"] == [i & 0x7F for i in range(npk)] and a["tail"] == RESULT_OK and len(a["end"]) == 1))
    ok &= check(f"4 H = S frame for frame (size, packets, last counters, ok): {res4}", all(r[3] for r in res4))

    # ---- 8 Cancel instead of the ACK ----
    res8 = {}
    for side, R in (("H", H), ("S", S)):
        R.cancel_instead = 5
        fr, jd = R.job(4, f4)
        R.cancel_instead = None
        a = analyse(fr, f4)
        res8[side] = (a["sent"], a["n"], fr[-1].hex() if fr else None, R.e.read_bytes(BINDING, 1)[0], jd, R.sem[0] == R.sem[1])
        R.e.write_bytes(CANCEL, b"\x00")
        R.e.write_bytes(BINDING, bytes([APP + 1]))
    h8, s8 = res8["H"], res8["S"]
    ok &= check(f"8 Cancel instead of the ACK of packet 5 (sends, packets, last frame, binding, job/due, sem pairs): H {h8}; S {s8}",
                h8[0] == 6 and h8[1] == 6 and h8[2] == RESULT_F0.hex() and h8[3] == APP + 1 and h8[4] == (0, 0) and h8[5]
                and s8[0] == 5 + 16 and s8[3] == 0)

    # ---- 9 / 10 write 6000 B ----
    f6 = test_file(6000)
    w = H.upload(f6)
    wr = w["writes"]
    ok &= check(f"9 H upload 6000 B ({w['n']} app packets, counters wrap twice): acks all ACK {all(x == ACK for x in w['acks'])} "
                f"({len(w['acks'])}); state {w['state']}; 7B -> {[x.hex() for x in w['end']]}; writer r0 {w['writer']:#x}, "
                f"writes {[(x[0], hex(x[1]), x[2]) for x in wr]}, bytes = file {bool(wr) and wr[0][3] == f6}",
                len(w["acks"]) == w["n"] and all(x == ACK for x in w["acks"]) and w["state"] == 2 and w["end"] == [ACK]
                and w["writer"] == 0 and len(wr) == 1 and wr[0][1] == BUF and wr[0][2] == 6000 and wr[0][3] == f6)
    s9 = S.upload(f6)
    bad10 = next((k for k, x in enumerate(s9["acks"]) if x != ACK), None)
    ok &= check(f"10 S the same: first non-ACK at reply {bad10} {[x.hex() for x in s9['acks'][bad10:bad10 + 2]] if bad10 else ''}; "
                f"state {s9['state']}; 7B -> {[x.hex() for x in s9['end']]}; writer r0 {s9['writer']:#x}, writes {len(s9['writes'])}; "
                f"buffer[0:21] = file[5376:5397] {s9['buf'][:21] == f6[5376:5397]} -> the stock fails",
                bad10 == 285 and s9["acks"][285:287] == [NAK, RESULT_F0] and s9["state"] == 0 and s9["end"] == [ACK, RESULT_ED]
                and s9["writer"] == 0xED and not s9["writes"] and s9["buf"][:21] == f6[5376:5397])

    # ---- 11 H = S where the stock works ----
    res11 = []
    for n, bad in ((1, None), (6, None), (20, None), (21, None), (42, None), (2681, None), (100, 2)):
        f = test_file(n)
        h, s_ = H.upload(f, bad=bad), S.upload(f, bad=bad)
        same = all(h[k] == s_[k] for k in ("tx", "acks", "state", "end", "writer", "writes", "wtx", "buf"))
        if bad is not None:
            good = h["acks"][bad:bad + 4] == [NAK, RESULT_F0, ACK, RESULT_ED] and not h["writes"]
        elif n < 7:
            good = h["acks"] == [NAK, RESULT_F0] and h["writer"] == 0xED and not h["writes"]
        else:
            good = h["writer"] == 0 and len(h["writes"]) == 1 and h["writes"][0][3] == f
        res11.append((n, bad, same, good))
    ok &= check(f"11 H = S (size, bad packet, same bytes, outcome as expected): {res11}", all(r[2] and r[3] for r in res11))
    S.rig.close()

    # ---- 12 multiple of 21 past the wrap + a resend ----
    f12 = test_file(5376)
    w = H.upload(f12, dup=200)
    wr = w["writes"]
    ok &= check(f"12 H 5376 B (256 x 21 + empty packet 256 = counter 0), packet 200 twice: {len(w['acks'])} ACKs, state {w['state']}, "
                f"writer {w['writer']:#x}, bytes = file {bool(wr) and wr[0][3] == f12 and wr[0][2] == 5376}",
                len(w["acks"]) == 258 and all(x == ACK for x in w["acks"]) and w["state"] == 2 and w["writer"] == 0
                and len(wr) == 1 and wr[0][2] == 5376 and wr[0][3] == f12)

    # ---- 13 the stock answers every upload frame ----
    own = {psyms[k] for k in ("frc_rx", "fw_rx", "fw_commit", "fw_copy", "fw_bytes", "fw_index") if k in psyms}
    out13 = []
    for f in ("frc_rx", "fw_rx", "fw_commit"):
        if f not in psyms:
            continue
        a0 = psyms[f]
        a1 = min(v for v in psyms.values() if v > a0)
        for a, m, o in ins:
            if a0 <= a < a1 and m in ("bl", "blx") and int(o.split()[0], 16) not in own | {psyms.get("frc_rx")}:
                out13.append((f, hex(a), o))
    log = answer_log(H)
    res13 = []
    for n, bad in ((100, None), (2681, None), (6000, None), (6000, 200)):
        log.clear()
        w = H.upload(test_file(n), bad=bad)
        npk = len(app_packets(test_file(n))) + len(name_packets(NAMESTR))   # 0x4014A9F8 answers FileName packets too
        stock = all(DISPATCH[0] <= lr < DISPATCH[1] for _k, lr in log)
        pk = [lr for k, lr in log if lr in (0x4014AA00, 0x4014AA14)]
        if bad is None:
            good = len(pk) == npk and all(x == ACK for x in w["acks"]) and w["writer"] == 0 and w["writes"][0][3] == test_file(n)
        else:
            good = (w["acks"][bad:bad + 2] == [NAK, RESULT_F0] and all(x == ACK for x in w["acks"][:bad]) and w["state"] == 0
                    and not w["writes"] and w["end"] == [ACK, RESULT_ED] and pk.count(0x4014AA14) == 1)
        res13.append((n, bad, len(pk), stock, good))
    H.e.unhook(STK_ACK)
    H.e.unhook(STK_NAK)
    ok &= check(f"13 the front calls nothing {out13 or 'ok'}; uploads (size, bad packet, packet answers by the stock dispatcher, "
                f"every answer from FUN_4014A2C8, outcome): {res13}", not out13 and all(r[3] and r[4] for r in res13))

    # ---- 14 field replay ----
    res14 = []
    for n in (100, 6000):
        F = Run(image3, syms3, data, True)
        binary_sems(F)
        log = answer_log(F)
        con = F.field_connect()
        fdata = bytes(i & 0xFF for i in range(n))
        rep, st, wr0, wr = F.field_upload(fdata)
        first = app_packets(fdata)[0]
        unanswered = [k for k, r in rep if k != "end" and r[:1] != [ACK]]
        F.clock_ms(1000)
        after = F.rx(CONNECT)
        res14.append((n, first.hex()[:24], len(rep), unanswered[:3], F.blocks, F.cnt, wr0, bool(wr) and wr[0][3] == fdata,
                      [x.hex() for x in after], all(DISPATCH[0] <= lr < DISPATCH[1] for _k, lr in log)))
        F.rig.close()
    ok &= check(f"14 field replay (size, first packet, frames, unanswered, semaphore waits on a taken one, counts after, writer, "
                f"bytes, CONNECT after): {res14}",
                all(r[1] == "f0407f240802020000000102" and not r[3] and not r[4] and r[5] == {0x11: 1, 0x12: 1} and r[6] == 0
                    and r[7] and r[8] and r[9] for r in res14))

    # ---- 5 NAK ----
    H.nak_at = {130}
    fr, jd = H.job(4, f4)
    a = analyse(fr, f4)
    ok &= check(f"5 H NAK on packet 130 once: {a['sent']} sends for {a['n']} packets, all clean {a['clean']}, file {a['body_ok']}, "
                f"last {a['tail'].hex()}",
                a["sent"] == 192 and a["n"] == 191 and a["clean"] and a["body_ok"] and a["tail"] == RESULT_OK)

    # ---- 15 retry budget: NAKs (the controller answering) vs silence ----
    res15 = []
    for nak, silent in ((20, None), (40, None), (63, None), (64, None), (None, 5), (20, 5)):
        k = 130 if silent is None else 5
        H.nak_n = {k: nak} if nak else {}
        H.silent_at = silent
        fr, jd = H.job(4, f4)
        H.nak_n, H.silent_at = {}, None
        a = analyse(fr, f4)
        res15.append((nak, silent, a["sent"], a["n"], a["body_ok"], len(a["end"]), a["tail"] == RESULT_OK,
                      H.e.read_bytes(BINDING, 1)[0], jd, H.sem[0] == H.sem[1]))
        H.e.write_bytes(BINDING, bytes([APP + 1]))
    def want15(nak, silent, sent, n, body, end, okr, bind, jd, sems):
        # on a give-up the unbind path FUN_4014B610 also signals a semaphore, as on the stock (check 8 S): pairs only on success
        common = jd == (0, 0)
        if silent is None and nak < 64:   # NAKed nak times, the next send ACKed: the file byte-exact
            return common and sems and sent == 191 + nak and n == 191 and body and end == 1 and okr and bind == APP + 1
        if silent is None:                # 64 NAKs: TX_TRIES sends, then given up (binding dropped as the stock does)
            return common and sent == 130 + 64 and end == 0 and not okr and bind == 0
        # silence: 16 silent sends as the stock (check 8 S: 5 + 16), NAKed sends do not count against them
        return common and sent == 5 + (nak or 0) + 16 and end == 0 and not okr and bind == 0
    ok &= check(f"15 H retry budget (NAKs, silent at, sends, packets, file, 7B, Result ok, binding, job/due, sem pairs): {res15}",
                all(want15(*r) for r in res15))

    # ---- 6 Cancel ----
    H.cancel_after = 5
    fr, jd = H.job(4, f4)
    H.cancel_after = None
    a = analyse(fr, f4)
    ok &= check(f"6 H Cancel after packet 5: {a['n']} packets, 7B {len(a['end'])}, last {a['tail'].hex()}, sem {H.sem}, job/due {jd}",
                a["n"] == 6 and not a["end"] and a["tail"] == RESULT_F0 and H.sem[0] == H.sem[1] and jd == (0, 0))

    # ---- 7 other jobs ----
    fr, jd = H.job(6)
    fr0, jd0 = H.job(4, f4, due=0)
    ok &= check(f"7 H delete (job 6) -> stock executor: frames {[f.hex() for f in fr]}, deletes {H.deletes}, job/due {jd}; "
                f"nothing due -> {len(fr0)} frames, job/due {jd0}",
                fr == [RESULT_OK] and H.deletes == 1 and jd == (0, 0) and fr0 == [] and jd0 == (4, 0))
    H.rig.close()
    return rtlib.finish(ok)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
