# ota_pack.py - the per-device OTA key and signed slot payload files (remote cave update).
#
#   cd result/stockfw
#   uv run python -I tools/ota_pack.py key                 # key id of secrets/ota_key.bin (creates it if absent)
#   uv run python -I tools/ota_pack.py verify build/ota_main.c79   # header, CRC, HMAC against the key
#
# Payload file (.c79) = 64 B header + payload, uploaded as-is over BLE (REMOTE_PROTOCOL 10.3 / 10.12):
#   0x00 'C79P'  0x04 u16 format (1 main, 2 v4)  0x06 u16 header size 64  0x08 u32 payload length
#   0x0C u32 load address (main 0x401C0000, loader 0x40176000)  0x10 u32 entry offset 0  0x14 u32 CRC-32 of the payload
#   0x18 u32 stock tag (first 4 B of the stock sha256)  0x1C u32 ABI  0x20 u32 feat0 | feat1 << 8
#   0x24 u32 key id  0x28 format 1: 8 B zero; format 2: u8 type (1 main, 2 loader) u8 signature alg (1 HMAC)
#   u8 min stage-1 API u8 0 u32 image version  0x30 16 B HMAC-SHA256(key, header[0:0x30] || payload)[:16]
# Layout constants mirror cave/include/ota.h and cave/include/s0.h.  The main ota_main stays format 1 (old apps);
# the stage-1 loader ota_stage1 is format 2, type 2 (pack_loader).
#
# The key: 32 random bytes in result/stockfw/secrets/ota_key.bin (git-ignored, mode 0600),
# created on first use.  It is patched into the ota_loader image (not into the tracked cave
# blob) and signs every payload; the app needs a copy to authenticate begin/commit/reject.
# Losing it means: USB-flash a loader built with a new key.  Nothing here touches a device.
from __future__ import annotations

import hashlib
import hmac
import os
import pathlib
import secrets
import struct
import sys
import zlib

HERE = pathlib.Path(__file__).resolve().parent
STOCKFW = HERE.parent
KEY_PATH = STOCKFW / "secrets" / "ota_key.bin"

HDR_SIZE = 64
HDR_MAGIC = b"C79P"
FORMAT = 1
ABI = 1
RUN_BASE = 0x401C0000
SECTOR = 0x20000
IMAGE_OFF = 0x200
IMAGE_MAX = SECTOR - IMAGE_OFF
PAYLOAD_MAX = IMAGE_MAX - HDR_SIZE
STOCK_SHA256 = "09ad85703e44ad95d82475cfe2dae14c6edfa9f164a0b17b66b45f74c8d933c8"
STOCK_TAG = int.from_bytes(bytes.fromhex(STOCK_SHA256)[:4], "little")
MAC_LEN = 16
TAG_LEN = 8
ENTRY_MAGIC = b"C79E"
KEY_VA = 0x40175FE0          # ota_loader_v4 root key (s0.ld s0_key, include/s0.h S0_KEY_VA)
KEY_FILE_OFFSET = KEY_VA - 0x40060000
# ota_loader_v4 stage-1 images (include/s0.h)
FORMAT_V4 = 2
TYPE_MAIN, TYPE_LOADER = 1, 2
ALG_HMAC = 1
WINDOW = 0x40176000
WINDOW_SIZE = 0xA000
LOADER_MAX = HDR_SIZE + WINDOW_SIZE
S0_ABI = 1
LOADER_MAGIC = b"C79L"      # stage-1 entry table magic


def ensure_key(path: pathlib.Path = KEY_PATH, *, quiet: bool = False) -> bytes:
    if path.is_file():
        key = path.read_bytes()
        if len(key) != 32:
            raise SystemExit(f"{path}: must be exactly 32 bytes")
        return key
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    key = secrets.token_bytes(32)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(key)
    if not quiet:
        print(f"  NEW OTA key generated: {path} (key id {key_id(key):08x}) - keep it, never commit it")
    return key


def key_id(key: bytes) -> int:
    return int.from_bytes(hmac.new(key, b"C79 key id", hashlib.sha256).digest()[:4], "little")


def pack(payload: bytes, key: bytes, feat: int) -> bytes:
    if not HDR_SIZE // 4 <= len(payload) <= PAYLOAD_MAX:
        raise SystemExit(f"payload {len(payload)} B is outside 16..{PAYLOAD_MAX} B")
    if payload[:4] != ENTRY_MAGIC:
        raise SystemExit("payload does not start with the 'C79E' entry table (ota_payload.ld)")
    hdr = bytearray(HDR_SIZE)
    hdr[0:4] = HDR_MAGIC
    struct.pack_into("<HHIIIIIIII", hdr, 4, FORMAT, HDR_SIZE, len(payload), RUN_BASE, 0,
                     zlib.crc32(payload), STOCK_TAG, ABI, feat, key_id(key))
    hdr[0x30:0x40] = mac(key, bytes(hdr[:0x30]), payload)
    return bytes(hdr) + payload


def pack_loader(payload: bytes, key: bytes, version: int) -> bytes:
    """A stage-1 loader image (format 2, type 2, alg 1, load = the stage-1 window, entry 0, stage-0 ABI)."""
    if not 24 <= len(payload) <= WINDOW_SIZE:
        raise SystemExit(f"loader payload {len(payload)} B is outside 24..{WINDOW_SIZE} B")
    if payload[:4] != LOADER_MAGIC:
        raise SystemExit("loader payload does not start with the 'C79L' entry table (s1.ld)")
    hdr = bytearray(HDR_SIZE)
    hdr[0:4] = HDR_MAGIC
    struct.pack_into("<HHIIIIIIII", hdr, 4, FORMAT_V4, HDR_SIZE, len(payload), WINDOW, 0,
                     zlib.crc32(payload), STOCK_TAG, S0_ABI, 0, key_id(key))
    struct.pack_into("<BBBBI", hdr, 0x28, TYPE_LOADER, ALG_HMAC, 0, 0, version)
    hdr[0x30:0x40] = mac(key, bytes(hdr[:0x30]), payload)
    return bytes(hdr) + payload


def mac(key: bytes, hdr48: bytes, payload: bytes) -> bytes:
    return hmac.new(key, hdr48 + payload, hashlib.sha256).digest()[:MAC_LEN]


def parse(image: bytes) -> dict:
    if len(image) < HDR_SIZE:
        raise ValueError("shorter than the header")
    (fmt, hsz, n, load, entry, crc, stock, abi, feat, kid) = struct.unpack_from("<HHIIIIIIII", image, 4)
    typ, alg, min_api, _r, version = struct.unpack_from("<BBBBI", image, 0x28)
    return dict(magic=image[:4], format=fmt, header_size=hsz, length=n, load=load, entry=entry, crc=crc,
                stock=stock, abi=abi, feat=feat, key_id=kid, mac=image[0x30:0x40], payload=image[HDR_SIZE:],
                type=typ if fmt >= 2 else 0, alg=alg if fmt >= 2 else ALG_HMAC, min_api=min_api if fmt >= 2 else 0,
                version=version if fmt >= 2 else 0)


def is_loader(image: bytes) -> bool:
    return len(image) >= HDR_SIZE and parse(image)["type"] == TYPE_LOADER


def verify(image: bytes, key: bytes) -> list[str]:
    """Problems found (empty = the loader would accept it)."""
    h = parse(image)
    bad = []
    ldr = h["type"] == TYPE_LOADER
    if h["magic"] != HDR_MAGIC: bad.append("magic")
    if h["header_size"] != HDR_SIZE or not (h["format"] == FORMAT or (h["format"] >= FORMAT_V4 and h["type"]
                                                                      in (TYPE_MAIN, TYPE_LOADER))):
        bad.append("format")
    if h["format"] >= FORMAT_V4 and h["alg"] != ALG_HMAC: bad.append("alg")
    if h["length"] != len(image) - HDR_SIZE or h["length"] > (WINDOW_SIZE if ldr else PAYLOAD_MAX): bad.append("length")
    if h["load"] != (WINDOW if ldr else RUN_BASE) or h["entry"] != 0: bad.append("load/entry")
    if h["stock"] != STOCK_TAG: bad.append("stock tag")
    if h["abi"] != (S0_ABI if ldr else ABI): bad.append("abi")
    if h["key_id"] != key_id(key): bad.append("key id")
    if zlib.crc32(h["payload"]) != h["crc"]: bad.append("crc")
    if not hmac.compare_digest(mac(key, image[:0x30], h["payload"]), h["mac"]): bad.append("mac")
    return bad


def auth_tag(key: bytes, op: int, challenge: int, args: bytes) -> bytes:
    """Tag of an authenticated op (begin 0x11, commit 0x13, reject 0x14)."""
    msg = b"C79A" + bytes([op]) + challenge.to_bytes(4, "little") + args
    return hmac.new(key, msg, hashlib.sha256).digest()[:TAG_LEN]


def main(argv: list[str]) -> int:
    if len(argv) >= 2 and argv[1] == "key":
        key = ensure_key()
        print(f"{KEY_PATH}: key id {key_id(key):08x}")
        return 0
    if len(argv) >= 3 and argv[1] == "verify":
        key = ensure_key(quiet=True)
        data = pathlib.Path(argv[2]).read_bytes()
        bad = verify(data, key)
        h = parse(data)
        print(f"{argv[2]}: {len(data)} B, payload {h['length']} B, feat {h['feat']:#06x}, "
              f"key id {h['key_id']:08x} -> {'OK' if not bad else 'BAD: ' + ', '.join(bad)}")
        return 0 if not bad else 1
    print(__doc__ or "usage: ota_pack.py key | verify <file.c79>")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
