# images.py - the image table: every .SYS this project builds, and for cave images the
# feature bits the cave is compiled with.  Single source for build_images.py (make image),
# make_phase2_manifest.py and the round-trip checks.  The key-independent sha256 each image must
# hash to lives in ../ARTIFACTS.md (build_images.py --check compares the two).
#
# Feature bits = what IDENTIFY (op 0x01) reports as feat0 / feat1, and what
# cave/src/ca79ng.c compiles in (-DCA79NG_FEAT0 / -DCA79NG_FEAT1):
#   feat0  0x01 identify  0x02 status  0x04 NOR dump  0x08 port  0x10 echo/sink
#          0x20 APO/BT snapshot  0x40 R1 canary
#   feat1  0x01 BT RX log  0x02 OpenGrand core linked in, dormant (self-test op 0x0A)
#          0x04 OTA: built for an ota_loader (remote cave update, ops 0x10-0x14 in the resident;
#               0x15 apply now, 0x16 runtime hook table)
#          0x08 runtime-hook demo payload (op 0x0B hook counters)
#          0x10 power policy / partial standby port (ops 0x18 policy, 0x19 standby, 0x1A diagnostics,
#               runtime hooks)
#          0x20 Phase 3-1 SysEx front (with 0x10: the core owns the Kawai fn 0x10 set at the 0x40147E94
#               hook, op 0x1B)
#          0x40 TV wake (with 0x10: standby keeps the BT audio module on and an A2DP STREAMING START
#               wakes, policy flag bit 2 switches it back off; UART1 0x80 latch auto-clear; op 0x1A
#               layout 2; policy cap 0xFFFF = "no cap")
#   feat2  (third byte, slot payloads only - -DCA79NG_FEAT2, IDENTIFY answers it as byte 6 when non-zero;
#          feat1 has no free bit left)
#          0x01 BT port (with feat1 0x10: BLE-MIDI port 2 RX latch auto-clear, port 2 active-sensing guard,
#               BT audio module read-only query, op 0x1D)
#          0x02 Phase 3-2 session core (with feat1 0x20: bindings / CONNECT / requests / ACK assignment, the
#               0x401539AC hook, op 0x1B 03)
#          0x04 panel-UI relay (0x7D ops 0x30-0x3F to the panel port 3, replies 0x70-0x7F back, op 0x1B 10)
#          0x08 startup settings owned by the core (op 0x1C)
#          0x10 Phase 3-6 R1 playback control by the core (with 0x02: recctl at the front end 0x40147E94 and the
#               pump tail 0x401539AC, op 0x1E)
#          0x20 remote off (op 19 03)
#          0x40 Phase 3-4 S3: the settings store answers the requests while trusted (with 0x02; record op 1B 05 05,
#               on at boot in ota_main since 2026-11-16, always on - no off op - from fold step 3)
#   feat3  (fourth byte, slot payloads only - -DCA79NG_FEAT3, IDENTIFY byte 7 when non-zero, needs feat2)
#          0x01 Phase 3-6 R2 record control (with feat2 0x10; field 2026-11-15)
#          0x02 Phase 4 A intake gate (field 2026-11-15)
#          0x04 Phase 3-6 R3 save decision (with 0x01; field 2026-11-15)
#          0x08 Phase 4 executor gate (record op 1F 00, always on in ota_main)
#          0x10 Phase 4 MIDI channel messages / delayed apply (record op 1B 07, always on in ota_main with v3+ sites)
#          0x20 Phase 3-4 S4 write owner (record op 1B 05 07, always on in ota_main)
#   ota_main = every bit at once (feat1 0x77, feat2 0x7F, feat3 0x3F): the one slot payload kept on the piano.
#          The power family is built only as ota_main: one fixed source / define set (cave/Makefile POWER_SRC), and
#          the feature bytes are FROZEN at 0x3B / 0x77 / 0x7F / 0x3F (generated NG_FEATn_FROZEN, checked in ca79ng.c);
#          new features raise the OpenGrand protocol version ngver (Identify byte 1, result/protocol/NGVER.md) instead
#          of adding bits.
#
# Published images: opengrand_install (the one USB image a user installs: ota_loader_v4 stage-0 + loader slot A seeded
# with ota_stage1 + main slot A seeded with ota_main), ota_stage1 (stage-1 loader image, Bluetooth updates of the
# loader), ota_main (slot payload, Bluetooth updates) and ota_loader_v4 (stage-0 alone, every slot empty: the build step
# opengrand_install is made from, the round trips' base, not a user install path) - nothing else.  TEST_PAYLOADS are
# slot payloads built from source for the emulator round trips only (the OTA upload / apply / hook-service checks need
# small payloads that tell apart); they have no ARTIFACTS row, are never published and their build outputs are
# git-ignored.
from __future__ import annotations

from dataclasses import dataclass
import json
import pathlib

# OpenGrand protocol version that ota_main built from this tree reports (Identify byte 1) - the protocol SSOT
# (result/protocol/spec/ca79.json ng_versions, make gen; the cave reads the generated ca79ng/ngver_gen.h)
NG_PROTO_VERSION = json.loads((pathlib.Path(__file__).resolve().parents[2] / "protocol" / "spec" / "ca79.json")
                              .read_text(encoding="utf-8"))["ng_versions"]["current"]

FEAT_IDENTIFY, FEAT_STATUS, FEAT_NORDUMP = 0x01, 0x02, 0x04
FEAT_PORT, FEAT_ECHO, FEAT_APO, FEAT_CANARY = 0x08, 0x10, 0x20, 0x40
FEAT1_BTLOG, FEAT1_CORE, FEAT1_OTA, FEAT1_HOOKDEMO, FEAT1_POWER, FEAT1_SYSEX = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20
FEAT1_TVWAKE = 0x40
FEAT2_BT, FEAT2_SESSION, FEAT2_RELAY, FEAT2_STARTUP, FEAT2_REC, FEAT2_PWROFF = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20
FEAT2_S3 = 0x40   # Phase 3-4 S3 read owner, in ota_main
# fourth feature byte (Identify byte 7, only when non-zero): feat2 is full with S3
# Phase 3-6 R2 record control - in ota_main since the fold (2026-11-16)
FEAT3_REC2 = 0x01
# Phase 3-6 R3 save decision (with REC2) - in ota_main since the fold
FEAT3_REC3 = 0x04
# Phase 4 A intake gate - in ota_main since the fold
FEAT3_GATE = 0x02
# Phase 4 executor gate: op 0x1F, the ota_loader_v4 engine-loop sites
# 0x40143AC4 / 0x40143AD8 hooked only while switched on (on at boot since 2026-11-16) - in ota_main
FEAT3_XGATE = 0x08
FEAT3_S4 = 0x20   # Phase 3-4 S4 write owner, in ota_main
# Phase 4 MIDI channel messages + delayed parameter apply judged by the core at the ota_loader_v4 sites 6 / 7 / 8
# - in ota_main, on at boot since 2026-11-16 (op 1B 07 0 / 1)
FEAT3_MIDICH = 0x10
CORE = FEAT_IDENTIFY | FEAT_STATUS


@dataclass(frozen=True)
class CaveImage:
    variant: str      # cave/build/cave_<variant>.{bin,elf}
    name: str         # patches/<name>.json -> build/<name>.SYS
    artifact: str     # row name in ARTIFACTS.md
    feat0: int
    feat1: int
    description: str
    stage0: bool = False   # ota_loader_v4 stage-0 (cave make STAGE=0, s0.ld; hooks s0_h1 / s0_tick, key 0x40175FE0)

    def has(self, bit: int, byte: int = 0) -> bool:
        return bool((self.feat1 if byte else self.feat0) & bit)


CAVE_IMAGES = {i.variant: i for i in (
    CaveImage("s0", "ota_loader_v4", "ota_loader_v4", 0, FEAT1_OTA,
              "ota_loader_v4 stage-0 (frozen, USB only): the two stock hooks, the escape keys (E1 A0+A#0+B0 = pure "
              "stock, E2 A0+A#0 = SAFE: the older loader, no main), loader slot choice (A 0x601A0000 / B 0x601E0000, "
              "highest seq, try mark BEFORE the load, 3 unconfirmed boots), NOR -> stage-1 window 0x40176000 (40 KB) "
              "with CRC-32 + HMAC-SHA256 (root key at 0x40175FE0), entry table check, RESELECT; the frozen NOR write "
              "service over one frozen range table (slot sectors + kvstore always on, three NOR gates - unused_2 + "
              "TouchCalib, 6-8 MB, 8-16 MB - off at every boot, switched on only by stage-1 with nor_enable; never "
              "BootProgram, never erase / page-program the running loader sector), cache sync "
              "and the one signature check for stage-1 and the main. Internal: the loader alone, all slots empty "
              "(boots stock) - the build step opengrand_install is made from.", stage0=True),
)}


@dataclass(frozen=True)
class OtaPayload:
    """A slot payload for ota_loader: the cave linked at OTA_RUN_BASE, signed into build/<name>.c79."""
    variant: str      # cave/build/payload_<variant>.{bin,elf}
    name: str         # build/<name>.c79
    artifact: str     # row name in ARTIFACTS.md
    feat0: int
    feat1: int
    description: str
    feat2: int = 0    # third feature byte (make FEAT2=..., IDENTIFY byte 6); 0 = not passed at all
    feat3: int = 0    # fourth feature byte (make FEAT3=..., IDENTIFY byte 7, needs feat2); 0 = not passed at all
    ngver: int = 1    # IDENTIFY byte 1 (OpenGrand protocol version): ota_main = NG_PROTO_VERSION, the test payloads 1
    # S3 / S4 / the MIDI channel judge / the executor gate on from boot with no off op (ota_main, fold step 3).  Round
    # trips read it for their expectations.
    boot_on: bool = False


OTA_PAYLOADS = {p.variant: p for p in (
    OtaPayload("pmain", "ota_main", "ota_main", CORE | FEAT_PORT | FEAT_ECHO | FEAT_APO,
               FEAT1_BTLOG | FEAT1_CORE | FEAT1_OTA | FEAT1_POWER | FEAT1_SYSEX | FEAT1_TVWAKE,
               "every feature in one slot payload: power ("
               "standby, TV wake, no cap) + the Phase 3-1 SysEx front + the Phase 3-2 stage 1 session core (fifth hook "
               "0x401539AC) + the BT port (port 2 latch clear, active-sensing guard, op 0x1D) + the panel-UI relay (ops "
               "0x30-0x3F <-> port 3, op 0x1B 10) + startup settings owned by the core (op 0x1C) + "
               "playback control by the core (Phase 3-6 R1, op 0x1E) + "
               "remote off from any bound controller (op 19 03, feat2 0x20). "
               "The piano runs one slot payload at a time: this is the one kept on it. "
               "+ the settings store as read owner (Phase 3-4 S3, feat2 0x40, record op 1B 05 05) "
               "+ the Phase 4 executor gate (feat3 0x08, op 0x1F; "
               "its two engine-loop sites exist only on ota_loader_v4). "
               "+ the settings store as write owner (Phase 3-4 S4, feat3 0x20, record op 1B 05 07) "
               "+ the MIDI channel messages / delayed apply judged by the core at the "
               "ota_loader_v4 sites 6 / 7 / 8 (Phase 4, feat3 0x10, op 1B 07). "
               "+ the Phase 3-6 R2 record control (feat3 0x01), the Phase 4 A intake gate (feat3 0x02, op 1B 06) and the Phase 3-6 R3 "
               "save decision (feat3 0x04, op 1E 04), folded in after their field tests. "
               "IDENTIFY feat1 0x77, feat2 0x7F, feat3 0x3F. "
               "Fold step 2: one fixed source / define set, no per-feature "
               "switches; IDENTIFY ngver (result/protocol/NGVER.md) with the feature bytes frozen at these values. S3, S4, the MIDI channel judge and the executor gate always on (boot-on 2026-11-16; fold step 3 / ngver 3 removed their off ops - the set forms answer rc 1)",
               feat2=FEAT2_BT | FEAT2_SESSION | FEAT2_RELAY | FEAT2_STARTUP | FEAT2_REC | FEAT2_PWROFF | FEAT2_S3,
               feat3=FEAT3_XGATE | FEAT3_S4 | FEAT3_MIDICH | FEAT3_REC2 | FEAT3_GATE | FEAT3_REC3, ngver=NG_PROTO_VERSION, boot_on=True),
)}

# test payloads for the round trips (see the header): built and signed by `make image` like a slot payload, no
# ARTIFACTS row, not published
TEST_PAYLOADS = {p.variant: p for p in (
    OtaPayload("pcore2", "ota_core_v2", "ota_core_v2", CORE | FEAT_PORT | FEAT_ECHO | FEAT_APO,
               FEAT1_BTLOG | FEAT1_CORE | FEAT1_OTA,
               "test payload: the built-in cave's features (identify / status, diagnostic ops, the core with its "
               "self-test op 0x0A) run from a slot, with the quiesce entry for apply-now"),
    OtaPayload("pbase2", "ota_base_v2", "ota_base_v2", CORE | FEAT_PORT | FEAT_ECHO | FEAT_APO,
               FEAT1_BTLOG | FEAT1_OTA,
               "test payload: the same without the core - identify feat1 0x05 tells it apart from ota_core_v2 (0x07) "
               "and the built-in"),
    OtaPayload("phook3", "ota_hookdemo3", "ota_hookdemo3", FEAT_IDENTIFY, FEAT1_OTA | FEAT1_HOOKDEMO,
               "test payload: runtime-hook demo for ota_loader_v4 with one pass-through counting stub per site "
               "of the 17-site allow-list, op 0x0B (17 counters); attach() hooks 0x40147E94 only"),
)}

OTA_PAYLOADS_BY_NAME = {p.name: p for p in (*OTA_PAYLOADS.values(), *TEST_PAYLOADS.values())}


@dataclass(frozen=True)
class LoaderImage:
    """An ota_loader_v4 stage-1 image (loader slots A/B): cave/src/s1.c linked into the stage-1 window
    (cave make STAGE=1 V=<variant> -> cave/build/loader_<variant>.{bin,elf}), signed into build/<name>.c79 (header
    format 2, type 2 = loader, tools/ota_pack.py pack_loader)."""
    variant: str
    name: str         # build/<name>.c79
    artifact: str     # row name in ARTIFACTS.md
    version: int      # header 0x2C image version (bump on every change that is shipped)
    description: str


LOADER_IMAGES = {i.variant: i for i in (
    LoaderImage("s1", "ota_stage1", "ota_stage1", 2,
                "ota_loader_v4 stage-1 (loader slots, Bluetooth-updatable, 40 KB window): main boot selection (main "
                "slots A/B, 3 tries, confirm after ~60 s), 'no main' responder, OTA ops 0x0E-0x16 for main and loader "
                "slots (verify in RAM before erase, loaders with stage-0's own rules and signature check), explicit "
                "loader confirm op 0x0F, interlocks (never erase or reject the last confirmed loader), apply now incl. "
                "RESELECT, status layout 3, runtime hook service (17-site allow-list, struct ota_svc version 3 = + the "
                "stage-0 NOR service for the main), fast upload (version 2: Begin reply advertises v2 data frames - CRC-32 per "
                "frame, up to 1536 B, LZ option, 4 in flight, cumulative ACK / go back). NOR writes only through stage-0's service."),
)}
LOADER_IMAGES_BY_NAME = {i.name: i for i in LOADER_IMAGES.values()}

# slot sectors: target byte (REMOTE_PROTOCOL 10.12: 0x00 / 0x01 main A / B, 0x10 / 0x11 loader A / B) -> file offset
# in the .SYS (NOR 0x60180000 / 0x601C0000 / 0x601A0000 / 0x601E0000, cave/include/s0.h)
SLOT_FILE_OFFSET = {0x00: 0x120000, 0x01: 0x160000, 0x10: 0x140000, 0x11: 0x180000}
SLOT_STATE_SIZE = 0x200          # the state area; the signed .c79 follows at +0x200
SLOT_STATE_MAGIC = b"C79S"       # main slots (u32 at +0), the sequence number is the u32 at +4
LOADER_STATE_MAGIC = b"C79L"     # loader slots
SLOT_NAME = {0x00: "main A", 0x01: "main B", 0x10: "loader A", 0x11: "loader B"}


@dataclass(frozen=True)
class Seed:
    """One pre-seeded slot of an install image: the state area exactly as stage-1's own commit leaves it (magic, seq,
    state version 1 at +0xE, every other halfword erased) - plus the confirm halfword 0000 when `confirmed` - and the
    signed .c79 at +0x200."""
    target: int       # SLOT_FILE_OFFSET key
    image: str        # OtaPayload or LoaderImage name (build/<image>.c79)
    seq: int
    confirmed: bool

    @property
    def loader(self) -> bool:
        return self.target >= 0x10

    @property
    def tag(self) -> str:
        return ("ldr_" if self.loader else "main_") + "ab"[self.target & 1]


@dataclass(frozen=True)
class InstallImage:
    """A USB image = a loader image's patches + pre-seeded slots (Seed).  The USB updater programs the whole file, so
    each seed lands in its slot sector; stage-0 / stage-1 boot them like uploaded slots (CRC + signature every boot).
    The seed blobs carry the key (signed .c79), so they live only under the git-ignored build/
    (build/<name>.seed_<tag>.bin)."""
    name: str         # patches/<name>.json -> build/<name>.SYS
    artifact: str     # row name in ARTIFACTS.md
    loader: str       # the CaveImage (variant) whose patches it carries
    seeds: tuple      # Seed, ...
    description: str


INSTALL_IMAGES = {i.name: i for i in (
    InstallImage("opengrand_install", "opengrand_install", "s0",
                 (Seed(0x10, "ota_stage1", seq=1, confirmed=True), Seed(0x00, "ota_main", seq=1, confirmed=False)),
                 description="opengrand_install: the one-file first install - ota_loader_v4 stage-0 + loader slot A "
                             "pre-seeded with ota_stage1 (state 'C79L', seq 1, CONFIRMED: there is no older loader to "
                             "fall back to) + main slot A pre-seeded with ota_main (state 'C79S', seq 1, unconfirmed: "
                             "3 tries, confirmed after ~60 s on). Later updates of both go over Bluetooth (slot B, "
                             "seq 2)."),
)}


def by_name(name: str) -> CaveImage:
    for img in CAVE_IMAGES.values():
        if name in (img.variant, img.name, img.artifact):
            return img
    raise KeyError(name)
