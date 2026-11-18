# p3_map.py - Phase 3-1 design input: for every Kawai SysEx parameter the stock firmware knows
# (descriptor table 0x400FAA2C, fn 0x10 / 0x31 / 0x32), run the STOCK set and get paths under Unicorn and
# our core (result/firmware simulator) with the same bytes, and diff the replies.
#
# Output: result/stockfw/build/p3_map.csv (+ p3_map_system.csv)
#
# Stock side (real code, unmodified image):
#   set: end handler 0x40147DB0 -> Kawai handler 0x4014A2C8 -> fn 0x10 ACK FUN_4014BE98 + FUN_4014EBFC
#        (lookup FUN_4014E8AC, length check FUN_4014EBA0, part FUN_4014EB50, staging gate FUN_4015C410,
#        apply FUN_4016053C). Controller bound on port 2 (BLE) = *0x413CA3DE = 3.
#   get: request task body FUN_40152A34(cc, v1, v2, part) (the message FUN_4014FD94 posts; consumer
#        FUN_40152AD8), stop-and-wait ACK wait FUN_4014B4E0 stubbed to "ACK".
#   Recorded per case: TX frames per port, apply-switch arguments, RAM writes outside the stack / RX buffers
#   (minus a baseline run with an unknown parameter), called function entries (block trace x BL-target set,
#   minus baseline), RTOS queue posts, peripheral writes, stop reason.
#   Not modelled: RTOS (queue post / semaphores / delay stubbed), interrupts, other tasks (what a queue
#   post or a flag later causes is NOT seen here), NOR (no flash write can happen in-call).
#
# Our side: build/ca79ng_sim fed "midi usb <hex>" lines (CONNECT on usb and panel, then set, then get);
#   the device byte of our replies (0x00) is normalised to the stock's 0x7F before comparing.
#
#   cd result/stockfw && uv run python -I tools/p3_map.py [--csv out.csv] [--only 0x68]
from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import pathlib
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
_HERE = pathlib.Path(__file__).resolve().parent
REPO = _HERE.parents[2]


def _load(name):
    spec = importlib.util.spec_from_file_location(name, _HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


se = _load("stockemu")
StockEmu = se.StockEmu

IMAGE = REPO / "target" / "CAx9_P06.SYS"
# The stock SysEx parameter descriptor table at 0x400FAA2C: DESC_COUNT entries of 10 bytes,
# index = internal id: fn, group (v1), param (v2), part_spec, value masks x4, value length, var-length flag.
# Read from the stock image itself (not a copy in the repository).
DESC_TABLE, DESC_SIZE, DESC_COUNT = 0x400FAA2C, 10, 216
SPEC = REPO / "result/protocol/spec/ca79.json"
SIM = REPO / "result/firmware/build/ca79ng_sim"

END_HANDLER = 0x40147DB0
TX = {0: 0x4014492C, 1: 0x401445AC, 2: 0x40145284, 3: 0x40144CD0}
STATE_BASE, LEN_BASE, BUF_BASE = 0x413B5400, 0x413B53F0, 0x413B29D0
BINDING = 0x413CA3DE
PORT = 2                       # BLE on this device
APPLY = 0x4016053C
APPLY_F0 = 0x401608D0          # apply default: return 0xF0 (unsupported id)
APPLY_PARAMS = 0x4014EBFC
STAGE_GATE = 0x4015C410        # returns 1 (= do not apply) while *0x419156A9 != 0 (fn 0x09 .. fn 0x03)
STAGE_FLAG = 0x419156A9
NOTIFY = 0x4013A4B4            # remote-change notification (kind, ...)
REQ_BODY = 0x40152A34          # request task: (cc, v1, v2, part)
REQ_ALL = 0x401525D4           # request task: v1 0x5F, v2 0x00 form
Q_POST, SEM_TAKE, SEM_GIVE, DELAY = 0x40084984, 0x40086E94, 0x4008819C, 0x40089658
ACK_WAIT = 0x4014B4E0        # fn 0x24 stop-and-wait (0 = ACK)
ACK_WAIT_10 = 0x4014BCAC     # fn 0x10/0x03 stop-and-wait in FUN_4014BD1C (0 = ACK, 1 = NAK, 2 = 250-tick timeout)
STACK_LO = 0x40F00000
RX_LO, RX_HI = BUF_BASE, BUF_BASE + 4 * 0x800
DEV = 0x7F
UI_TABLE_LO, UI_TABLE_HI = 0x4139DF84, 0x4139DF84 + 0x22 * 8  # FUN_4013A4B4 table (kind x 8 B: u32 pending, u8, u8)


# ------------------------------------------------------------------------------------------------
def bl_targets():
    d = se.stock_bytes()
    out = set()
    for i in range(0, len(d) - 3, 4):
        w = struct.unpack_from("<I", d, i)[0]
        if (w & 0x0F000000) == 0x0B000000 and (w >> 28) != 0xF:
            off = w & 0xFFFFFF
            if off & 0x800000:
                off -= 0x1000000
            out.add((0x40060000 + i + 8 + off * 4) & 0xFFFFFFFF)
    return out


FN_ENTRIES = None


def hexs(b):
    return " ".join(f"{x:02X}" for x in b)


class StockRun:
    """One fresh emulator per case (the loader is fast), with the RTOS stubbed."""

    def __init__(self):
        self.emu = StockEmu()
        assert self.emu.is_stock, "not the stock image"
        self.sent, self.posts, self.applies, self.notifies, self.f0 = [], [], [], [], 0
        e = self.emu
        for p, a in TX.items():
            e.hook(a, (lambda pp: lambda em: self.sent.append((pp, bytes(em.read_bytes(em.reg(0), em.reg(1))))))(p))
        e.hook(Q_POST, lambda em: (self.posts.append((em.reg(0), em.reg(1))), 0)[1])
        e.hook(SEM_TAKE, lambda em: 0)
        e.hook(SEM_GIVE, lambda em: 0)
        e.hook(DELAY, lambda em: None)
        e.hook(ACK_WAIT, lambda em: 0)
        e.hook(ACK_WAIT_10, lambda em: 0)

        def on_apply(em):
            self.applies.append((em.reg(0), em.reg(1), em.reg(2), bytes(em.read_bytes(em.reg(3), 4)), em.writes_so_far()))
            return StockEmu.CONTINUE

        def on_f0(em):
            self.f0 += 1
            return StockEmu.CONTINUE

        def on_notify(em):
            self.notifies.append((em.reg(0), em.reg(1)))
            return StockEmu.CONTINUE

        e.hook(APPLY, on_apply)
        e.hook(APPLY_F0, on_f0)
        e.hook(NOTIFY, on_notify)
        e.write_bytes(BINDING, bytes([PORT + 1]))

    def reset_logs(self):
        self.sent, self.posts, self.applies, self.notifies, self.f0 = [], [], [], [], 0

    def deliver(self, msg):
        e = self.emu
        self.reset_logs()
        e.write_bytes(BUF_BASE + PORT * 0x800, msg)
        e.write_bytes(LEN_BASE + PORT * 2, len(msg).to_bytes(2, "little"))
        e.write_bytes(STATE_BASE + PORT, b"\xF0")
        return e.call(END_HANDLER, PORT, trace=True, max_insns=5_000_000)

    def request(self, v1, v2, part):
        self.reset_logs()
        return self.emu.call(REQ_BODY, DEV, v1, v2, part, trace=True, max_insns=50_000_000)


def interesting_writes(res, skip=()):
    out = {}
    for w in res.writes:
        if STACK_LO <= w.addr < 0x41000000 or RX_LO <= w.addr < RX_HI or w.addr in skip:
            continue
        if 0x413B5400 <= w.addr < 0x413B5410 or 0x413B53F0 <= w.addr < 0x413B5400:
            continue
        out.setdefault(w.addr, w)
    return out


def callees(res):
    return {a for a in res.trace if a in FN_ENTRIES}


# ------------------------------------------------------------------------------------------------
def load_cases():
    spec = json.loads(SPEC.read_text())
    byid = {p["fw_id"]: p for p in spec["params"]}
    cases = []
    img = se.stock_bytes()
    for iid in range(DESC_COUNT):
        o = DESC_TABLE - 0x40060000 + DESC_SIZE * iid
        fn, v1, v2, ps = img[o:o + 4]
        masks = list(img[o + 4:o + 8])
        vlen, var = img[o + 8], img[o + 9] == 1
        p = byid.get(iid)
        name = p["name"] if p else f"(none in ca79.json) {fn:02X}/{v1:02X}/{v2:02X}"
        if p and p.get("parts"):
            part = list(p["parts"].values())[0]
        else:
            part = {0x7F: 0x7F, 0x80: 0x00, 0x0F: 0x0F, 0x81: 0x0F, 0x82: 0x20, 0x1F: 0x1F}.get(ps, 0x7F)
        probe = None
        if p:
            k = p.get("kind")
            vals = p.get("values") or []
            cod = p.get("codec") or {}
            if k == "boolean":
                probe = [1]
            elif k == "enum" and len(vals) > 1:
                probe = [vals[1]["raw"]] + ([vals[1]["sub"]] if vals[1].get("sub") is not None else [])
            elif k == "min" and "raw_min" in cod:
                probe = [min(cod["raw_min"] + 1, cod.get("raw_max", 127))]
            if probe is None:
                for dflt in p.get("default") or []:
                    if dflt.get("raw_data"):
                        probe = list(dflt["raw_data"])
                        break
        if probe is None:
            probe = [0] * vlen
        probe = (probe + [0] * vlen)[:vlen] if not var else (probe or [0] * vlen)
        probe = [min(b, 0x7F) for b in probe]
        cases.append(dict(iid=iid, fn=fn, v1=v1, v2=v2, ps=ps, vlen=vlen, var=var, masks=masks, name=name,
                          part=part, probe=probe, ours_has=p is not None))
    return cases


# ------------------------------------------------------------------------------------------------
def run_sim(lines):
    inp = "".join(l + "\nstatus\n" for l in lines) + "quit\n"
    out = subprocess.run([str(SIM)], input=inp, capture_output=True, text=True, timeout=30).stdout
    chunks, cur, started = [], [], False
    for ln in out.splitlines():
        if ln.startswith("ready"):
            started = True
            continue
        if not started:
            continue
        if ln.startswith("status"):
            chunks.append(cur)
            cur = []
        elif ln.startswith("tx "):
            _, port, *hx = ln.split()
            cur.append((port, bytes(int(x, 16) for x in hx)))
    return chunks


def norm(b):
    """our device byte 0x00 -> stock 0x7F, so only real differences remain"""
    b = bytearray(b)
    if len(b) > 2 and b[0] == 0xF0 and b[1] == 0x40 and b[2] == 0x00:
        b[2] = DEV
    return bytes(b)


def frames(lst):
    return [hexs(m) for _p, m in lst]


# ------------------------------------------------------------------------------------------------
def baseline():
    s = StockRun()
    res = s.deliver(bytes([0xF0, 0x40, DEV, 0x10, 0x08, 0x02, 0x7F, 0x7E, 0x7F, 0x00]))
    return set(interesting_writes(res)), callees(res)


def stock_case(c, base_w, base_c):
    s = StockRun()
    msg = bytes([0xF0, 0x40, DEV, c["fn"], 0x08, 0x02, c["v1"], c["v2"], c["part"], *c["probe"]])
    res = s.deliver(msg)
    ws = interesting_writes(res, base_w)
    calls = sorted(callees(res) - base_c - {APPLY, APPLY_PARAMS})
    applies = list(s.applies)
    after = applies[0][4] if applies else None
    state = []
    if applies:
        b0 = c["probe"][0] if c["probe"] else None
        for w in res.writes[after:]:
            if 0x413CA3B0 <= w.addr < 0x413CA3B4 or UI_TABLE_LO <= w.addr < UI_TABLE_HI:
                continue  # per-port "changed by remote" flags / UI change table, not the value itself
            if w.addr in ws and b0 is not None and (w.value & 0xFF) in (b0, (b0 - 0x40) & 0xFF) and b0 not in (0,):
                state.append(f"{w.addr:#010x}/{w.size}")
    out = dict(
        stop=res.stop, set_tx=[(p, m) for p, m in s.sent], applies=applies, f0=s.f0, posts=list(s.posts),
        notifies=list(s.notifies), writes=sorted(ws), calls=calls,
        periph=sorted({(a.addr, a.value) for a in res.periph if a.op == "W"}), state=sorted(set(state)),
        stage_flag=s.emu.read_bytes(STAGE_FLAG, 1)[0],
    )
    get_tx, get_stop = [], ""
    if c["fn"] == 0x10:
        r = s.request(c["v1"], c["v2"], c["part"])
        get_tx, get_stop = [(p, m) for p, m in s.sent], r.stop
    out.update(get_tx=get_tx, get_stop=get_stop)
    return out


def ours_case(c):
    con = ["midi usb F0 40 7F 7E 08 02 01 01 F7", "midi panel F0 40 7F 7E 08 02 01 01 F7"]
    m = [0xF0, 0x40, DEV, c["fn"], 0x08, 0x02, c["v1"], c["v2"], c["part"], *c["probe"], 0xF7]
    g = [0xF0, 0x40, DEV, 0x02, 0x08, 0x02, c["v1"], c["v2"], c["part"], 0xF7]
    lines = con + ["midi usb " + hexs(m), "midi usb " + hexs(g)]
    ch = run_sim(lines)
    set_tx = ch[2] if len(ch) > 2 else []
    get_tx = ch[3] if len(ch) > 3 else []
    return dict(set_tx=set_tx, get_tx=get_tx)


def classify(c, st, ou):
    stock_set = [hexs(m) for p, m in st["set_tx"] if p == PORT]
    our_set = [hexs(norm(m)) for p, m in ou["set_tx"] if p == "usb"]
    notes = []
    if not c["ours_has"]:
        verdict = "missing"
        notes.append("no descriptor in ca79.json/params_gen")
    elif stock_set == our_set:
        verdict = "same"
    else:
        verdict = "differs"
        if any(x.split()[3] == "06" for x in our_set):
            notes.append("ours NAKs (06) where stock ACKs")
    other_stock = [f"p{p}:{hexs(m)}" for p, m in st["set_tx"] if p != PORT]
    other_ours = [f"{p}:{hexs(norm(m))}" for p, m in ou["set_tx"] if p != "usb"]
    if other_stock:
        notes.append("stock TX on other ports")
    if other_ours:
        notes.append("ours notifies other controllers")
    gverdict = ""
    if c["fn"] == 0x10:
        key = bytes([c["v1"], c["v2"], c["part"]])
        sg = [m for p, m in st["get_tx"]]
        og = [norm(m) for p, m in ou["get_tx"] if p == "usb"]
        sv = [m for m in sg if m[3] == 0x10 and m[6:9] == key]
        ov = [m for m in og if m[3] == 0x10 and m[6:9] == key]
        if not c["ours_has"]:
            gverdict = "missing"
        elif not sv and not ov:
            gverdict = "both silent"
        elif not sv:
            gverdict = "differs(stock has no value frame)"
        elif not ov:
            gverdict = "differs(ours has no value frame)"
        elif [hexs(m) for m in sv] == [hexs(m) for m in ov]:
            gverdict = "same"
        else:
            gverdict = "differs(value)"
        if len([m for m in sg if m[3] == 0x10]) > 1:
            notes.append(f"stock get returns {len([m for m in sg if m[3] == 0x10])} values (v2 0x00 = whole group)")
    return verdict, gverdict, notes, stock_set, our_set, other_stock, other_ours


# ------------------------------------------------------------------------------------------------
# system / session / bulk commands (not in the descriptor table)
SYS_CASES = [
    # (label, body after F0 40 7F (no F7), stock bound before?, extra note)
    ("CONNECT 7E 01 01 (unbound)", [0x7E, 0x08, 0x02, 0x01, 0x01], False),
    ("CONNECT 7E 01 01 (port already bound)", [0x7E, 0x08, 0x02, 0x01, 0x01], True),
    ("DISCONNECT 7E 01 00", [0x7E, 0x08, 0x02, 0x01, 0x00], True),
    ("PowerOff 7E 00 00", [0x7E, 0x08, 0x02, 0x00, 0x00], True),
    ("probe 7E 00 40", [0x7E, 0x08, 0x02, 0x00, 0x40], True),
    ("Mode 7E 02 00", [0x7E, 0x08, 0x02, 0x02, 0x00], True),
    ("FactoryMode 7E 02 03 01", [0x7E, 0x08, 0x02, 0x02, 0x03, 0x01], True),
    ("RomName 7E 04 40", [0x7E, 0x08, 0x02, 0x04, 0x40], True),
    ("Version 7E 04 41", [0x7E, 0x08, 0x02, 0x04, 0x41], True),
    ("Checksum 7E 04 42", [0x7E, 0x08, 0x02, 0x04, 0x42], True),
    ("Model 7E 05 40", [0x7E, 0x08, 0x02, 0x05, 0x40], True),
    ("Destination 7E 05 41", [0x7E, 0x08, 0x02, 0x05, 0x41], True),
    ("Language 7E 05 42", [0x7E, 0x08, 0x02, 0x05, 0x42], True),
    ("Model 7E 05 40 (unbound)", [0x7E, 0x08, 0x02, 0x05, 0x40], False),
    ("Identity fn 60", [0x60, 0x08, 0x02], True),
    ("EndOfData fn 03 5F 7F 7F", [0x03, 0x08, 0x02, 0x5F, 0x7F, 0x7F], True),
    ("Wait fn 04", [0x04, 0x08, 0x02, 0x00, 0x00, 0x00], True),
    ("Cancel fn 05", [0x05, 0x08, 0x02, 0x00, 0x00, 0x00], True),
    ("NAK fn 06", [0x06, 0x08, 0x02, 0x00, 0x00, 0x00], True),
    ("ACK fn 07", [0x07, 0x08, 0x02, 0x00, 0x00, 0x00], True),
    ("fn 09 (stage begin)", [0x09, 0x08, 0x02, 0x5F, 0x7F, 0x7F], True),
    ("fn 10 multi '5F 00' packed (len 4: 51 00 7F 01)", [0x10, 0x08, 0x02, 0x5F, 0x00, 0x7F, 0x05, 0x51, 0x00, 0x7F, 0x01], True),
    ("fn 10 unknown param 7F 7E", [0x10, 0x08, 0x02, 0x7F, 0x7E, 0x7F, 0x00], True),
    ("fn 10 set while unbound (51 00 7F 01)", [0x10, 0x08, 0x02, 0x51, 0x00, 0x7F, 0x01], False),
    ("fn 10 wrong length (51 00 7F 01 01)", [0x10, 0x08, 0x02, 0x51, 0x00, 0x7F, 0x01, 0x01], True),
    ("fn 10 out of range (51 00 7F 09)", [0x10, 0x08, 0x02, 0x51, 0x00, 0x7F, 0x09], True),
    ("Request all 02 5F 7F 7F", [0x02, 0x08, 0x02, 0x5F, 0x7F, 0x7F], True),
    ("Request group 02 51 7F 7F", [0x02, 0x08, 0x02, 0x51, 0x7F, 0x7F], True),
    ("Request 02 5F 00 7F", [0x02, 0x08, 0x02, 0x5F, 0x00, 0x7F], True),
    ("Request unbound 02 51 03 7F", [0x02, 0x08, 0x02, 0x51, 0x03, 0x7F], False),
    ("File list 24 11", [0x24, 0x08, 0x02, 0x11], True),
    ("File ACK 24 7F", [0x24, 0x08, 0x02, 0x7F], True),
    ("fn 70 sub 01 (fw session start)", [0x70, 0x08, 0x02, 0x01, 0x01] + [0] * 6, True),
    ("fn 31 unknown 7F 7E", [0x31, 0x08, 0x02, 0x7F, 0x7E, 0x7F, 0x00], True),
    ("undefined fn 0x40", [0x40, 0x08, 0x02, 0x00], True),
    ("header 04 1E (piano->ctrl) fn 10", [0x10, 0x04, 0x1E, 0x51, 0x00, 0x7F, 0x01], True),
]


def stock_system(body, bound):
    s = StockRun()
    if not bound:
        s.emu.write_bytes(BINDING, b"\x00")
    msg = bytes([0xF0, 0x40, DEV, *body])
    res = s.deliver(msg)
    sent = list(s.sent)
    posts = list(s.posts)
    req = []
    for _q, m in posts:  # request task message (cc<<24 | d0<<16 | d1<<8 | d2), FUN_40152AD8
        if body[0] == 0x02:
            cc, d0, d1, d2 = (m >> 24) & 0xFF, (m >> 16) & 0xFF, (m >> 8) & 0xFF, m & 0xFF
            s.reset_logs()
            r = s.emu.call(REQ_ALL, cc, 0x5F, max_insns=200_000_000) if (d0 == 0x5F and d1 == 0) else \
                s.emu.call(REQ_BODY, cc, d0, d1, d2, max_insns=200_000_000)
            req = list(s.sent)
    ws = interesting_writes(res)
    bind = s.emu.read_bytes(BINDING, 1)[0]
    return dict(stop=res.stop, tx=sent, req=req, posts=posts, applies=list(s.applies), bind=bind,
                stage=s.emu.read_bytes(STAGE_FLAG, 1)[0], nwrites=len(ws))


def ours_system(body, bound):
    con = ["midi usb F0 40 7F 7E 08 02 01 01 F7"] if bound else ["midi din F0 F7"]
    lines = con + ["midi usb " + hexs([0xF0, 0x40, DEV, *body, 0xF7])]
    ch = run_sim(lines)
    return ch[1] if len(ch) > 1 else []


def system_section(path):
    rows = []
    dump_cmp = None
    for label, body, bound in SYS_CASES:
        st = stock_system(body, bound)
        ou = ours_system(body, bound)
        s_tx = [f"p{p}:{hexs(m)}" for p, m in st["tx"]]
        s_req = [hexs(m) for _p, m in st["req"]]
        o_tx = [f"{p}:{hexs(norm(m))}" for p, m in ou]
        s_all = [hexs(m) for _p, m in st["tx"] + st["req"]]
        o_all = [hexs(norm(m)) for _p, m in ou]
        if label.startswith("Request all"):
            sk = [(m[6], m[7], m[8]) for _p, m in st["req"] if m[3] == 0x10]
            ok = [(m[6], m[7], m[8]) for _p, m in ou if m[3] == 0x10]
            dump_cmp = dict(stock_n=len(sk), ours_n=len(ok), only_stock=sorted(set(sk) - set(ok)),
                            only_ours=sorted(set(ok) - set(sk)), same_order=[k for k in sk if k in ok] == [k for k in ok if k in sk])
        verdict = "same" if s_all == o_all else ("both silent" if not s_all and not o_all else "differs")
        if verdict == "differs" and [x[6:] for x in s_all] == [x[6:] for x in o_all]:
            verdict = "differs(header only)"
        rows.append({
            "command": label, "bytes": hexs([0xF0, 0x40, DEV, *body, 0xF7]), "stock_bound_before": bound,
            "stock_reply": " | ".join(s_tx)[:500], "stock_request_task_reply": (" | ".join(s_req[:6]) + (f" ... ({len(s_req)} frames) " + s_req[-1] if len(s_req) > 6 else ""))[:600],
            "stock_queue_posts": " ".join(f"{q:#x}:{m:#x}" for q, m in st["posts"]),
            "stock_apply": " ".join(f"id={a[2]:#x}" for a in st["applies"]),
            "stock_binding_after(port+1)": st["bind"], "stock_stage_flag_after": st["stage"], "stock_stop": st["stop"],
            "ours_reply": (" | ".join(o_tx[:6]) + (f" ... ({len(o_tx)} frames) " + o_tx[-1] if len(o_tx) > 6 else ""))[:600],
            "verdict": verdict,
        })
    p = pathlib.Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    from collections import Counter
    print("system:", dict(Counter(r["verdict"] for r in rows)))
    if dump_cmp:
        print(f"full dump 02 5F 7F 7F: stock {dump_cmp['stock_n']} values, ours {dump_cmp['ours_n']}, "
              f"same order on common keys: {dump_cmp['same_order']}")
        print("  only stock:", " ".join(f"{a:02X}/{b:02X}/{c:02X}" for a, b, c in dump_cmp["only_stock"]))
        print("  only ours :", " ".join(f"{a:02X}/{b:02X}/{c:02X}" for a, b, c in dump_cmp["only_ours"]))
    print(f"wrote {p} ({len(rows)} rows)")


def main():
    global FN_ENTRIES
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default=str(REPO / "result/stockfw/build/p3_map.csv"))
    ap.add_argument("--only", default=None)
    ap.add_argument("--json", default=None, help="also dump raw results as JSON")
    ap.add_argument("--system-csv", default=str(REPO / "result/stockfw/build/p3_map_system.csv"))
    ap.add_argument("--system-only", action="store_true")
    a = ap.parse_args()
    FN_ENTRIES = bl_targets()
    system_section(a.system_csv)
    if a.system_only:
        return 0
    base_w, base_c = baseline()
    cases = load_cases()
    if a.only:
        cases = [c for c in cases if c["iid"] == int(a.only, 0)]
    rows, raw = [], []
    for c in cases:
        st = stock_case(c, base_w, base_c)
        ou = ours_case(c)
        v, gv, notes, ss, os_, oth_s, oth_o = classify(c, st, ou)
        ap0 = st["applies"][0] if st["applies"] else None
        rows.append({
            "internal_id": f"{c['iid']:#04x}", "name": c["name"], "fn": f"{c['fn']:#04x}", "v1": f"{c['v1']:#04x}",
            "v2": f"{c['v2']:#04x}", "part": f"{c['part']:#04x}", "probe": hexs(c["probe"]),
            "stock_apply": (f"FUN_4016053C(ctx={ap0[0]},part={ap0[1]:#x},id={ap0[2]:#x})" if ap0 else "not called"),
            "apply_unsupported_F0": "yes" if st["f0"] else "",
            "stop": st["stop"],
            "notify_4013A4B4": " ".join(f"{k:#x}" for k, _ in st["notifies"]),
            "queue_posts": " ".join(f"{q:#x}:{m:#x}" for q, m in st["posts"]),
            "periph_writes": " ".join(f"{ad:#x}={vv:#x}" for ad, vv in st["periph"])[:200],
            "state_addr(probe match)": " ".join(st["state"]),
            "ram_writes": " ".join(f"{w:#x}" for w in st["writes"])[:400],
            "n_ram_writes": len(st["writes"]),
            "calls_beyond_baseline": " ".join(f"{x:#x}" for x in st["calls"])[:600],
            "stock_set_reply": " | ".join(ss), "ours_set_reply": " | ".join(os_),
            "stock_other_ports": " | ".join(oth_s), "ours_other_ports": " | ".join(oth_o),
            "set_verdict": v,
            "stock_get_reply": " | ".join(hexs(m) for _p, m in st["get_tx"])[:300],
            "ours_get_reply": " | ".join(hexs(norm(m)) for p, m in ou["get_tx"] if p == "usb")[:300],
            "get_verdict": gv, "notes": "; ".join(notes),
        })
        raw.append({"case": {k: v for k, v in c.items()}, "stock": {k: (str(v) if not isinstance(v, (int, str)) else v) for k, v in st.items()}})
    out = pathlib.Path(a.csv)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    if a.json:
        pathlib.Path(a.json).write_text(json.dumps(raw, indent=1, default=str))
    from collections import Counter
    print("set:", dict(Counter(r["set_verdict"] for r in rows)))
    print("get (fn 0x10):", dict(Counter(r["get_verdict"] for r in rows if r["fn"] == "0x10")))
    print(f"wrote {out} ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
