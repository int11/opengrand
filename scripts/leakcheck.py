#!/usr/bin/env python3
"""leakcheck.py - refuse to publish things that must never be in this public repository.

Standard library only. Run from anywhere inside the repository:

    python3 scripts/leakcheck.py            # every tracked file (git ls-files), working-tree content
    python3 scripts/leakcheck.py --cached   # every file in the index, index content (what the pre-commit hook uses)
    python3 scripts/leakcheck.py --all      # tracked + untracked-but-not-ignored files (what `git add -A` would add)
    python3 scripts/leakcheck.py PATH...    # only these files

What fails (one line per finding, `path:line: [category] text`, exit status 1):

  path        forbidden paths: docs/ notes layout (records, data, INDEX/PROTOCOL/PROFILE), target/, any .claude/ or secrets/ directory, CLAUDE.md,
              *.SYS, *.c79, *.c79u, CA79NOR*
  mac         a MAC address written as six colon/dash separated octets, or 12 bare hex digits on a
              line that talks about Bluetooth / addresses.  Locally administered addresses (second
              hex digit of the first octet 2, 6, A or E - e.g. 02:...) and all-same-octet values are
              fine, since they cannot belong to a real device.
  homepath    absolute home-directory paths (Linux and Windows)
  personal    names listed in the local, untracked file .git/info/leakcheck-personal (one per line;
              skipped when the file is absent - the names themselves never appear in this repository)
  ip          private (10/8, 172.16/12, 192.168/16) and CGNAT/Tailscale (100.64/10) IPv4 addresses
  pem         PEM blocks (private keys, certificates)
  hex64       hex strings of 64 or more digits (keys, digests) that are not in the allowlist
              (lock files are skipped: their digests are package checksums)
  crypto      names of the vendor's bundle-encryption routines
  stock       a run of 16 or more bytes shared with Kawai's system file, when target/CAx9_P06.SYS
              (or --sys PATH) exists locally.  Runs made of only one or two distinct byte values
              (zero/0xFF padding and the like) are ignored.  A run made only of printable ASCII
              (NUL bytes count as string separators) is text: each NUL-separated string in it is
              reported only when it is 48 bytes or longer and matches no `stock-text:` pattern -
              short strings like Bluetooth module commands, alphabets and space padding are
              interface facts, not vendor content.  Any other (binary) run of 16 or more bytes
              fails unless a `stock-bin:` entry names exactly that run (path, offset, length and
              the sha256 of its bytes).
  dangling-notes  a reference to the maintainer's private research notes, which are not part of
              this repository: the docs/ subfolders records and data, the docs/ INDEX file, the flash
              checklist, and record folder names (six figures, dash, topic, slash)

Allowlist: scripts/leakcheck.allow, one entry per line, `#` starts a comment.
  <token>        a literal string (case-insensitive) that is fine wherever it appears, e.g. a public
                 sha256 constant or a documentation MAC
  file:<glob>    files (fnmatch on the repo-relative path) exempt from the content rules
                 (forbidden-path and stock-overlap rules still apply)
  stock-text:<regex>  a text string shared with the stock file that is fine (full match, after
                 stripping surrounding white space)
  stock-bin:<path> <offset> <length> <sha256>  one binary run shared with the stock file that is fine:
                 the finding must be in file <path> (repo-relative) at byte <offset> (hex 0x.. or
                 decimal) with exactly <length> bytes, and those bytes must hash to <sha256>.  A
                 changed byte, a longer or shorter run, another offset or another file still fails.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import PurePosixPath

ALLOW_FILE = "scripts/leakcheck.allow"
DEFAULT_SYS = "target/CAx9_P06.SYS"

# Literal needles are assembled from pieces so that this file does not match itself.
_HOME_NIX = "/" + "home" + "/"
_HOME_WIN = r"[A-Za-z]:[\\/]+" + "Us" + "ers"
# Personal needles (account / institution names) are not written here: this file is public. They are read from
# a local, untracked list - one per line - in the git directory (shared by worktrees). Missing file = rule skipped.
PERSONAL_FILE = "info/leakcheck-personal"
_CRYPTO = [r"kwm_?" + "crypt", "kwm_" + "decrypt"]
_DASH5 = "-" * 5

RE_HOME = re.compile(re.escape(_HOME_NIX) + "|" + _HOME_WIN, re.I)
RE_PERSONAL: re.Pattern[str] | None = None
RE_CRYPTO = re.compile("|".join(_CRYPTO), re.I)
RE_NOTES = re.compile(r"\bdocs/(?:" + "rec" + "ords|da" + "ta|IN" + r"DEX)\b|\bFlash_" + r"Checklist|\b\d{6}-[a-z]+/")
RE_PEM = re.compile(_DASH5 + r"BEGIN [A-Z0-9 ]+" + _DASH5)
RE_MAC_SEP = re.compile(
    r"(?<![0-9A-Fa-f])(?<![0-9A-Fa-f][:-])"
    r"([0-9A-Fa-f]{2})([:-])(?:[0-9A-Fa-f]{2}\2){4}[0-9A-Fa-f]{2}"
    r"(?![:-]?[0-9A-Fa-f])")
RE_MAC_BARE = re.compile(r"(?<![0-9A-Za-z_-])[0-9A-Fa-f]{12}(?![0-9A-Za-z_])")
RE_BT_WORDS = re.compile(r"bluetooth|\bbt|\bble|bdaddr|bd_addr|\bmac\b|mac[ _-]?addr|\baddr|a2dp|pair", re.I)
RE_IPV4 = re.compile(r"(?<![0-9.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?![0-9]|\.[0-9])")
RE_HEX64 = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{64,}(?![0-9A-Fa-f])")

LOCKFILES = {"uv.lock", "package-lock.json", "yarn.lock", "pnpm-lock.yaml", "Cargo.lock", "poetry.lock"}


def git(*args: str, data: bytes | None = None) -> bytes:
    return subprocess.run(["git", *args], input=data, stdout=subprocess.PIPE, check=True).stdout


def repo_root() -> str:
    return git("rev-parse", "--show-toplevel").decode().strip()


def load_personal() -> re.Pattern[str] | None:
    try:
        common = git("rev-parse", "--path-format=absolute", "--git-common-dir").decode().strip()
        with open(os.path.join(common, PERSONAL_FILE), encoding="utf-8") as f:
            words = [w.strip() for w in f if w.strip() and not w.lstrip().startswith("#")]
    except (OSError, subprocess.CalledProcessError):
        return None
    return re.compile("|".join(re.escape(w) for w in words), re.I) if words else None


# ----------------------------------------------------------------------------- allowlist

class Allow:
    def __init__(self, path: str):
        self.tokens: set[str] = set()
        self.globs: list[str] = []
        self.stock_text: list[re.Pattern] = []
        self.stock_bin: set[tuple[str, int, int, str]] = set()
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                for raw in f:
                    if raw.startswith("stock-text:"):          # a regex may contain '#'
                        self.stock_text.append(re.compile(raw[11:].rstrip("\r\n")))
                        continue
                    line = raw.split("#", 1)[0].strip()
                    if not line:
                        continue
                    if line.startswith("file:"):
                        self.globs.append(line[5:].strip())
                    elif line.startswith("stock-bin:"):
                        f = line[10:].split()
                        if len(f) != 4:
                            raise SystemExit(f"{path}: stock-bin needs <path> <offset> <length> <sha256>: {line}")
                        self.stock_bin.add((f[0], int(f[1], 0), int(f[2], 0), f[3].lower()))
                    else:
                        self.tokens.add(line.lower())

    def token(self, s: str) -> bool:
        return s.lower() in self.tokens

    def file(self, rel: str) -> bool:
        return rel == ALLOW_FILE or any(fnmatch.fnmatchcase(rel, g) for g in self.globs)

    def bin_ok(self, rel: str, off: int, run: bytes) -> bool:
        """A binary run shared with the stock file that a stock-bin entry names byte for byte."""
        return (rel, off, len(run), hashlib.sha256(run).hexdigest()) in self.stock_bin

    def text_ok(self, seg: str) -> bool:
        seg = seg.strip()
        return any(p.fullmatch(seg) for p in self.stock_text)


# ----------------------------------------------------------------------------- rules

def forbidden_path(rel: str) -> str | None:
    p = PurePosixPath(rel)
    parts = p.parts
    name = p.name
    low = name.lower()
    if parts and parts[0] == "target" and len(parts) > 1:
        return "target/ must not be published"
    # docs/ holds the public guides; the private research-notes layout must never appear in it
    if parts and parts[0] == "docs" and len(parts) > 1 and (
            parts[1] in ("rec" + "ords", "da" + "ta") or parts[1] in ("IN" + "DEX.md", "PROTOCOL.md", "PROFILE.md")):
        return f"docs/{parts[1]} is private research-notes layout and must not be published"
    for d in parts[:-1]:
        if d in (".claude", "secrets"):
            return f"{d}/ must not be published"
    if name == "CLAUDE.md":
        return "CLAUDE.md must not be published"
    if low.endswith((".sys", ".c79", ".c79u")):
        return "firmware image / slot payload must not be published"
    if low.startswith("ca79nor"):
        return "NOR dump must not be published"
    return None


def mac_is_fake(mac: str) -> bool:
    octets = re.split(r"[:-]", mac) if len(mac) > 12 else [mac[i:i + 2] for i in range(0, 12, 2)]
    if len(set(o.lower() for o in octets)) == 1:
        return True                          # 00:00:..., ff:ff:..., 11:11:...
    first = int(octets[0], 16)
    return bool(first & 0x02)                # locally administered (x2 / x6 / xA / xE)


def ip_is_private(o: tuple[int, ...]) -> bool:
    a, b = o[0], o[1]
    return a == 10 or (a == 172 and 16 <= b <= 31) or (a == 192 and b == 168) or (a == 100 and 64 <= b <= 127)


def scan_text(rel: str, text: str, allow: Allow, out: list):
    base = PurePosixPath(rel).name
    lockfile = base in LOCKFILES
    for n, line in enumerate(text.splitlines(), 1):
        def add(cat: str, hit: str):
            if not allow.token(hit):
                out.append((rel, f"{n}", cat, hit if len(hit) <= 80 else hit[:77] + "..."))

        for m in RE_HOME.finditer(line):
            add("homepath", line[max(0, m.start() - 10):m.end() + 30].strip())
        if not lockfile:                     # base64 integrity hashes contain any 4-letter string
            for m in (RE_PERSONAL.finditer(line) if RE_PERSONAL else ()):
                add("personal", m.group(0))
        for m in RE_CRYPTO.finditer(line):
            add("crypto", m.group(0))
        for m in RE_NOTES.finditer(line):
            add("dangling-notes", line[max(0, m.start() - 20):m.end() + 30].strip())
        for m in RE_PEM.finditer(line):
            add("pem", m.group(0))
        for m in RE_MAC_SEP.finditer(line):
            if not mac_is_fake(m.group(0)):
                add("mac", m.group(0))
        if RE_BT_WORDS.search(line):
            for m in RE_MAC_BARE.finditer(line):
                s = m.group(0)
                if s.isdigit() and not RE_BT_WORDS.search(line[max(0, m.start() - 24):m.end() + 24]):
                    continue
                if not mac_is_fake(s):
                    add("mac", s)
        for m in RE_IPV4.finditer(line):
            o = tuple(int(x) for x in m.groups())
            if all(x <= 255 for x in o) and ip_is_private(o):
                add("ip", m.group(0))
        if not lockfile:
            for m in RE_HEX64.finditer(line):
                s = m.group(0)
                if not s.isdigit():
                    add("hex64", s)


# ----------------------------------------------------------------------------- stock overlap

class StockIndex:
    """Every 16-byte run shared with the stock file contains two or three whole 8-byte blocks of
    the stock file that start at a multiple of 4.  So the stock file is indexed by those blocks
    (about 950 k entries for 3.8 MB); a scanned file is probed at every offset, and a hit is
    extended in both directions to the full common run.  Blocks with fewer than 3 distinct byte
    values, and blocks that occur more than CAP times in the stock file (tables, padding), are not
    indexed - a run made only of those carries little vendor content (in a test with 136 random
    16-byte stock slices, 133 were found; the 3 missed sat in repetitive tables)."""

    BLOCK = 8
    STEP = 4
    MIN_RUN = 16
    CAP = 64

    def __init__(self, data: bytes):
        self.data = data
        idx: dict[bytes, list[int]] = {}
        B = self.BLOCK
        for p in range(0, len(data) - B + 1, self.STEP):
            blk = data[p:p + B]
            if len(set(blk)) < 3:
                continue
            lst = idx.get(blk)
            if lst is None:
                idx[blk] = [p]
            elif len(lst) <= self.CAP:
                lst.append(p)
        self.idx = {k: v for k, v in idx.items() if len(v) <= self.CAP}

    def scan(self, buf: bytes):
        """Yield (offset_in_buf, length, offset_in_stock) for each maximal common run >= MIN_RUN."""
        B, S, idx = self.BLOCK, self.data, self.idx
        n = len(buf)
        j = 0
        get = idx.get
        while j <= n - B:
            plist = get(buf[j:j + B])
            if plist is None:
                j += 1
                continue
            best = None
            for p in plist:
                a, b = j, p                  # extend backwards
                while a > 0 and b > 0 and buf[a - 1] == S[b - 1]:
                    a -= 1
                    b -= 1
                e, f = j + B, p + B          # extend forwards
                while e < n and f < len(S) and buf[e] == S[f]:
                    e += 1
                    f += 1
                if e - a >= self.MIN_RUN and len(set(buf[a:e])) >= 3 and (best is None or e - a > best[1]):
                    best = (a, e - a, b)
            if best:
                yield best
                j = best[0] + best[1]
            else:
                j += 1


def line_of(buf: bytes, off: int) -> int:
    return buf.count(b"\n", 0, off) + 1


STOCK_TEXT_MIN = 48
_PRINTABLE = frozenset(range(0x20, 0x7F)) | {0x09, 0x0A, 0x0D}


def stock_findings(run: bytes, allow: Allow) -> list[tuple[int, int, str]]:
    """The parts of a run shared with the stock file that are findings: (offset in run, length, kind)."""
    if all(c in _PRINTABLE or c == 0 for c in run):
        out, pos = [], 0
        for seg in run.split(b"\0"):
            if len(seg) >= STOCK_TEXT_MIN and not allow.text_ok(seg.decode("ascii")):
                out.append((pos, len(seg), "text"))
            pos += len(seg) + 1
        return out
    return [(0, len(run), "binary")]


# ----------------------------------------------------------------------------- driver

def list_files(mode: str, paths: list[str]) -> list[str]:
    if paths:
        root = repo_root()
        return [os.path.relpath(os.path.abspath(p), root).replace(os.sep, "/") for p in paths]
    if mode == "all":
        raw = git("ls-files", "-z", "--cached", "--others", "--exclude-standard")
    else:
        raw = git("ls-files", "-z")
    return sorted({p for p in raw.decode("utf-8", "surrogateescape").split("\0") if p})


def read_index(files: list[str]) -> dict[str, bytes]:
    """Index (staged) content of each file, via one git cat-file --batch."""
    raw = git("ls-files", "-z", "-s", "--", *files) if files else b""
    oids: dict[str, str] = {}
    for rec in raw.decode("utf-8", "surrogateescape").split("\0"):
        if rec:
            meta, path = rec.split("\t", 1)
            mode, oid, _stage = meta.split()
            if mode != "160000":             # skip submodules
                oids[path] = oid
    out: dict[str, bytes] = {}
    if not oids:
        return out
    order = list(oids)
    blob = git("cat-file", "--batch", data="".join(oids[p] + "\n" for p in order).encode())
    pos = 0
    for p in order:
        nl = blob.index(b"\n", pos)
        header = blob[pos:nl].split()
        size = int(header[2])
        out[p] = blob[nl + 1:nl + 1 + size]
        pos = nl + 1 + size + 1
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--all", action="store_true", help="tracked + untracked (not ignored) files")
    g.add_argument("--cached", action="store_true", help="index content instead of the working tree")
    ap.add_argument("--sys", metavar="PATH", help=f"stock system file to compare against (default {DEFAULT_SYS})")
    ap.add_argument("--no-stock", action="store_true", help="skip the stock-overlap scan")
    ap.add_argument("-q", "--quiet", action="store_true", help="print only the summary")
    ap.add_argument("paths", nargs="*")
    a = ap.parse_args()

    global RE_PERSONAL
    root = repo_root()
    os.chdir(root)
    RE_PERSONAL = load_personal()
    allow = Allow(ALLOW_FILE)
    files = list_files("all" if a.all else "tracked", a.paths)
    staged = read_index(files) if a.cached else None

    stock = None
    sys_path = a.sys or DEFAULT_SYS
    if not a.no_stock and os.path.isfile(sys_path):
        with open(sys_path, "rb") as f:
            stock = StockIndex(f.read())
    elif a.sys and not a.no_stock:
        print(f"leakcheck: {a.sys} not found", file=sys.stderr)
        return 2

    findings: list[tuple[str, str, str, str]] = []
    for rel in files:
        why = forbidden_path(rel)
        if why:
            findings.append((rel, "-", "path", why))
        if staged is not None:
            buf = staged.get(rel)
        else:
            try:
                with open(rel, "rb") as f:
                    buf = f.read()
            except (FileNotFoundError, IsADirectoryError):
                buf = None                   # deleted in the working tree, or a submodule
        if buf is None:
            continue
        binary = b"\0" in buf[:8192]
        if not binary and not allow.file(rel):
            scan_text(rel, buf.decode("utf-8", "replace"), allow, findings)
        if stock is not None:
            for off, length, soff in stock.scan(buf):
                for d, n, kind in stock_findings(buf[off:off + length], allow):
                    if kind == "binary" and allow.bin_ok(rel, off + d, buf[off + d:off + d + n]):
                        continue
                    where = f"0x{off + d:x}" if binary else f"{line_of(buf, off + d)}"
                    findings.append((rel, where, "stock",
                                     f"{n} bytes ({kind}) identical to {sys_path} @0x{soff + d:x}"))

    if not a.quiet:
        for rel, where, cat, text in findings:
            print(f"{rel}:{where}: [{cat}] {text}")
    counts = Counter(f[2] for f in findings)
    summary = ", ".join(f"{k} {v}" for k, v in sorted(counts.items())) or "none"
    stock_note = "" if stock is not None else f" (stock-overlap scan skipped: no {sys_path})"
    print(f"leakcheck: {len(files)} files, {len(findings)} findings: {summary}{stock_note}", file=sys.stderr)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
