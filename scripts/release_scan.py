#!/usr/bin/env python3
"""release_scan.py - refuse to publish a release artifact that carries anything of Kawai's or anything secret.

Standard library only; used by scripts/release_build.mjs before it signs update.json:

    python3 -I scripts/release_scan.py --sys <CAx9_P06.SYS> --apk <phone.apk> --zip <OpenGrand-win-x64.zip>

What fails (one line per finding, exit status 1; a missing --sys file is exit status 2, never a silent skip):

  name     an entry whose name looks like vendor material or a secret anywhere in the archive: *.SYS, *.c79, *.c79u,
           *.kwm, *.pem, *.key, ota_key*, CA79NOR*, dist-kwm/, appUI, panelui/, uiloader, secrets/
  layout   APK: an asset outside assets/www/, or an assets/www/ file that is not one of the web build's own files
           (index.html, app-<hash>.js[.map], styles-<hash>.css, icon-*.png, manifest.webmanifest, sw.js).
           Desktop zip: OpenGrand.exe not at the top; resources/ holding anything but app.asar and app/<web build files>;
           app.asar holding anything but package.json and src/*.{cjs,html,js}
  content  the leakcheck.py text rules (home paths, PEM blocks, unlisted 64-hex strings, private IPs, MACs, the vendor's
           bundle-crypto names, private-notes references, personal names) on every text file of the web build and of
           app.asar, with scripts/leakcheck.allow
  stock    a 16-byte run shared with the stock system file (leakcheck.py StockIndex) in the web build files, app.asar's
           files and the APK's classes*.dex / AndroidManifest.xml / resources.arsc
The Electron / Chromium runtime files of the desktop zip are only name-checked: they are the upstream binaries.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import leakcheck as lc  # noqa: E402

RE_BAD_NAME = re.compile(
    r"\.(sys|c79|c79u|kwm|pem|key)$|(^|/)ota_key|(^|/)ca79nor|(^|/)dist-kwm(/|$)|appui|(^|/)panelui(/|$)|uiloader|(^|/)secrets(/|$)",
    re.I)
RE_WEB = re.compile(r"^(index\.html|app-[A-Z0-9]+\.js(\.map)?|styles-[A-Z0-9]+\.css|icon-[a-z0-9-]+\.png|manifest\.webmanifest|sw\.js)$")
RE_ASAR_FILE = re.compile(r"^(package\.json|src/[A-Za-z0-9_.-]+\.(cjs|html|js))$")
TEXT_EXT = (".js", ".map", ".html", ".css", ".json", ".webmanifest", ".cjs", ".txt")


class Scan:
    def __init__(self, sys_path: str):
        with open(sys_path, "rb") as f:
            self.stock = lc.StockIndex(f.read())
        self.sys_path = sys_path
        root = lc.repo_root()
        self.allow = lc.Allow(os.path.join(root, lc.ALLOW_FILE))
        lc.RE_PERSONAL = lc.load_personal()
        self.findings: list[tuple[str, str, str, str]] = []
        self.files = 0

    def add(self, where: str, cat: str, text: str):
        self.findings.append((where, "-", cat, text))

    def name(self, label: str, name: str):
        if RE_BAD_NAME.search(name):
            self.add(f"{label}!{name}", "name", "looks like vendor material or a secret")

    def content(self, label: str, name: str, buf: bytes, text: bool):
        self.files += 1
        rel = f"{label}!{name}"
        if text:
            lc.scan_text(rel, buf.decode("utf-8", "replace"), self.allow, self.findings)
        for off, length, soff in self.stock.scan(buf):
            for d, n, kind in lc.stock_findings(buf[off:off + length], self.allow):
                if kind == "binary" and trivial(buf[off + d:off + d + n]):
                    continue
                self.findings.append((rel, f"0x{off + d:x}", "stock", f"{n} bytes ({kind}) identical to the stock file @0x{soff + d:x}"))


def trivial(run: bytes) -> bool:
    """A shared binary run with no vendor content in it: mostly 00 / FF padding, or a table whose every 2nd or 4th byte
    is one constant (UTF-16 text, counting u16 / u32 tables). Such runs turn up in any large binary (seen in classes.dex,
    resources.arsc and a PNG); leakcheck.py proper would need a byte-exact allowlist entry, which changes every build."""
    if sum(1 for c in run if c in (0x00, 0xFF)) * 2 >= len(run):
        return True
    for k in (2, 4):
        for j in range(k):
            if len(set(run[j::k])) == 1:
                return True
    return False


def scan_apk(s: Scan, path: str):
    label = os.path.basename(path)
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        for n in names:
            s.name(label, n)
            if n.startswith("assets/"):
                sub = n[len("assets/"):]
                if not sub.startswith("www/") or "/" in sub[4:] or not RE_WEB.match(sub[4:]):
                    s.add(f"{label}!{n}", "layout", "not a file of the web build (assets/www/)")
                    continue
                s.content(label, n, z.read(n), n.endswith(TEXT_EXT))
            elif re.match(r"^classes\d*\.dex$|^AndroidManifest\.xml$|^resources\.arsc$", n):
                s.content(label, n, z.read(n), False)
        if "assets/www/index.html" not in names:
            s.add(label, "layout", "assets/www/index.html missing (web build not packaged)")


def asar_files(buf: bytes) -> dict[str, bytes]:
    """Files of an Electron asar archive: {path: bytes}. Layout: pickle(u32 4, u32 header size) + pickle(u32, u32 json
    length, json) + file data at offset 8 + header size."""
    header_size = struct.unpack_from("<I", buf, 4)[0]
    json_len = struct.unpack_from("<I", buf, 12)[0]
    header = json.loads(buf[16:16 + json_len])
    base = 8 + header_size
    out: dict[str, bytes] = {}

    def walk(node: dict, prefix: str):
        for k, v in node.get("files", {}).items():
            p = f"{prefix}{k}"
            if "files" in v:
                walk(v, p + "/")
            elif v.get("unpacked"):
                out[p] = b""
            else:
                off = base + int(v["offset"])
                out[p] = buf[off:off + int(v["size"])]
    walk(header, "")
    return out


def scan_zip(s: Scan, path: str):
    label = os.path.basename(path)
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        if "OpenGrand.exe" not in names:
            s.add(label, "layout", "OpenGrand.exe is not at the top of the zip (the updater needs it there)")
        for n in names:
            s.name(label, n)
            if not n.startswith("resources/") or n.endswith("/"):
                continue
            sub = n[len("resources/"):]
            if sub == "app.asar":
                files = asar_files(z.read(n))
                if "package.json" not in files or not any(f.startswith("src/") for f in files):
                    s.add(f"{label}!{n}", "layout", "app.asar does not hold the desktop shell")
                for f, data in files.items():
                    s.name(f"{label}!{n}", f)
                    if not RE_ASAR_FILE.match(f):
                        s.add(f"{label}!{n}!{f}", "layout", "not a file of the desktop shell (src/, package.json)")
                        continue
                    s.content(f"{label}!{n}", f, data, True)
            elif sub.startswith("app/") and RE_WEB.match(sub[4:]) and "/" not in sub[4:]:
                s.content(label, n, z.read(n), n.endswith(TEXT_EXT))
            else:
                s.add(f"{label}!{n}", "layout", "unexpected file in resources/ (only app.asar and the web build)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sys", required=True, help="Kawai's stock system file (CAx9_P06.SYS) for the overlap scan")
    ap.add_argument("--apk", action="append", default=[])
    ap.add_argument("--zip", action="append", default=[])
    a = ap.parse_args()
    if not os.path.isfile(a.sys):
        print(f"release_scan: {a.sys} not found - the stock-overlap scan is required for a release", file=sys.stderr)
        return 2
    s = Scan(a.sys)
    for p in a.apk:
        scan_apk(s, p)
    for p in a.zip:
        scan_zip(s, p)
    for where, at, cat, text in s.findings:
        print(f"{where}:{at}: [{cat}] {text}")
    print(f"release_scan: {s.files} files scanned, {len(s.findings)} findings", file=sys.stderr)
    return 1 if s.findings else 0


if __name__ == "__main__":
    sys.exit(main())
