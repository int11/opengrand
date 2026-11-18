/* OpenGrand cave payload.
 *
 * Two jobs:
 *   ng_sysex()  - answers the OpenGrand SysEx family on manufacturer ID 0x7D, which the
 *                 stock firmware ignores completely.
 *                 A reply therefore *is* the "custom firmware present" signal.
 *   ng_tick()   - engine loop tail: one step of the read-only NOR dump, the canary fill,
 *                 and (FEAT1_CORE) the dormant core's tick.
 *
 * Wire format
 *   request  F0 7D 43 41 37 39 <op> [args] F7        ("CA79" tag)
 *   reply    F0 7D 43 41 37 39 <op|0x40> [data] F7
 * Every byte is 7-bit.  Requests are validated on tag AND length before any field is
 * read: the stock receiver hands us messages that were cut short by a status byte, so
 * a trailing F7 does not mean the message is complete.
 *
 * The dump never writes to flash.  It reads through the stock window copier and streams
 * 64 KB chunks to a file on the USB stick.
 *
 * One source, many images: what goes into a build is chosen by two feature bytes passed
 * on the command line (-DCA79NG_FEAT0=0x.. -DCA79NG_FEAT1=0x..), and IDENTIFY reports
 * exactly those bytes.  The image table (name -> feature bytes) lives in
 * result/stockfw/tools/images.py; `make image` at the repo root builds every image.
 *
 *   FEAT_NORDUMP  read-only NOR dump to USB (phase2b)
 *   FEAT_PORT / FEAT_ECHO / FEAT_APO / FEAT1_BTLOG
 *                 read-only diagnostic ops (phase2c_probe, baseline)
 *   FEAT_CANARY   R1 canary - the only op that writes RAM outside the cave's own .data
 *                 (the unused hole above the cave), and only when asked to.
 *   FEAT1_OTA     built for the ota_loader (remote cave update, src/ota.c): this file is
 *                 either its built-in cave (ng_sysex/ng_tick renamed builtin_*) or a slot
 *                 payload (src/ota_payload.c); IDENTIFY reports the bit, the ops are ota.c's.
 *   FEAT1_CORE    the OpenGrand core (result/firmware) linked in, dormant: initialised and
 *                 ticked from the engine loop, answers only the self-test op 0x0A.  Its RW
 *                 state lives in R1 (cave_core.ld); see result/firmware/ports/cave.
 *   FEAT1_POWER   slot payload ota_power only (with CORE + OTA): the power port
 *                 (result/firmware/ports/cave/power_cave.c) - ops 0x18 policy, 0x19 standby,
 *                 0x1A diagnostics, its runtime hooks (src/power_hooks.c); IDENTIFY feat1 0x10.
 *   FEAT1_SYSEX   slot payload ota_sysex only (with POWER): Phase 3-1 - the core's SysEx front owns
 *                 the Kawai fn 0x10 set of the bound controller at the 0x40147E94 hook
 *                 (result/firmware/ports/cave/sysex_cave.c), op 0x1B front diagnostics; feat1 0x20.
 *   FEAT1_TVWAKE  slot payload ota_power2 (with POWER): the power port built with PWC_TVWAKE -
 *                 standby keeps the BT audio module on, an A2DP STREAMING START wakes; feat1 0x40
 *                 Nothing in this file changes.
 *   FEAT2_BT      slot payload ota_bt (with POWER; third feature byte -DCA79NG_FEAT2, IDENTIFY then
 *                 answers 6 bytes - feat1 has no free bit): the BT port (result/firmware/ports/cave/
 *                 bt_cave.c) - BLE-MIDI port 2 latch clear, active-sensing guard, BT audio module
 *                 read-only query, op 0x1D; feat2 0x01.
 *   FEAT2_SESSION slot payload ota_session (with SYSEX): Phase 3-2 stage 1 - the core's session table,
 *                 CONNECT answers, core-sent request dumps, ACK assignment, a fifth hook 0x401539AC
 *                 (sysex_cave.c / src/power_hooks.c built with CA79NG_SESSION); feat2 0x02.
 *   FEAT2_RELAY   panel-UI relay (with POWER): 0x7D ops 0x30-0x3F pass through to the panel port 3 and the
 *                 replies 0x70-0x7F back (result/firmware/ports/cave/relay_cave.c, op 0x1B 10 its counters);
 *                 feat2 0x04.
 *   FEAT2_STARTUP startup settings owned by the core (with SESSION): boot restore of a
 *                 snapshot by core-originated sets, snapshots in the kvstore, op 0x1C (result/firmware/ports/cave/
 *                 startup_cave.c); feat2 0x08.
 *   FEAT2_REC     Phase 3-6 R1 playback control by the core (with SESSION): the bound controllers' play / stop /
 *                 pause / song / part / tempo / seek commands judged by recctl and run through the stock routines,
 *                 op 0x1E status / song list / commands / notifications (result/firmware/ports/cave/recctl_cave.c); feat2 0x10.
 *   FEAT2_PWROFF  remote off (with POWER, BT, REC): op 19 03 from any controller the session core has bound - where
 *                 an APO expiry goes (standby / real off), refused while recording, a file operation or a flash job;
 *                 the power ops' bound test follows the core table (power_cave.c PWC_REMOTE_OFF); feat2 0x20.
 *   FEAT2_S3      Phase 3-4 S3 (with SESSION): the settings store answers the bound controllers' requests while it is
 *                 trusted (kpstate S3, sysex_cave.c), on at boot since 2026-11-16, op 1B 05 04 <0|1>; 1B 05 05
 *                 record; feat2 0x40.
 *   FEAT3_REC2    Phase 3-6 R2 record control by the core (with REC): arm / count-in / start / finish (= the stock
 *                 commit through ring FC) judged by recctl - one finish per take, deferred while a NOR job / file
 *                 operation runs, no start without arming (recctl_cave.c RCC_REC2, op 0x1E cmd 8-11); FEAT3 0x01 (fourth feature byte) - field-tested in
 *                 ota_main_rec2, in ota_main since the fold.
 *   FEAT3_GATE    Phase 4 A intake gate (with SESSION): the core accepts / refuses / answers the rest of FUN_4014A2C8's
 *                 functions (kpgate.c, sysex_cave.c CA79NG_GATE, op 1B 06);
 *                 FEAT3 0x02 - field-tested in ota_main_gate, in ota_main since the fold
 *   FEAT3_REC3    Phase 3-6 R3 save decision (with REC2): song-table write windows around every commit / delete /
 *                 import, checked after the NOR job; Kawai InternalSongDelete and 24 08 / 24 09 refused while a take,
 *                 a job or a window runs (recsave.c, recctl_cave.c RCC_REC3, sysex_cave.c CA79NG_REC3, op 1E 04); FEAT3 0x04 - field-tested in ota_main_rec3, in ota_main
 *                 since the fold.
 *   FEAT3_XGATE   Phase 4 executor gate (with POWER + SESSION): the core decides when the stock media executor
 *                 FUN_40127594 and the audio transport executor FUN_40123E34 run (ota_loader_v3 sites 0x40143AC4 /
 *                 0x40143AD8, hooked only while on), op 0x1F status / switch, on at boot since 2026-11-16 (result/firmware/
 *                 ports/cave/xgate_cave.c); FEAT3 0x08.
 *   FEAT3_S4      Phase 3-4 S4 (with S3): every set of a bound controller judged by the core first (known key, length,
 *                 descriptor masks), applied by the stock, read back into the store; the store stays trusted after a set
 *                 of pure keys (kpstate S4, sysex_cave.c), on at boot since 2026-11-16, op 1B 05 06 <0|1>; 1B 05 07
 *                 record; FEAT3 0x20.
 *                 ota_main = every feat2 bit + every feat3 bit: feat1 0x77, feat2 0x7F, feat3 0x3F.
 *   (no bit)      every power-family payload built from source (ota_main) also answers the read-only
 *                 GPIO diagnostic op 0x17 (result/firmware/ports/cave/gpio_cave.c): GPIO bank snapshot, change log, change notices to a bound controller.  No feature
 *                 bit (as the APO test 19 04): an older payload stays silent.
 * The probe ops only READ stock globals.
 */
#include "stock.h"

#define TAG0 0x43 /* 'C' */
#define TAG1 0x41 /* 'A' */
#define TAG2 0x37 /* '7' */
#define TAG3 0x39 /* '9' */

#define OP_IDENTIFY 0x01
#define OP_DUMP     0x02
#define OP_STATUS   0x03
#define OP_CANCEL   0x04
#define OP_PORT     0x05   /* receiving port, binding, port 3 forwarding flag, length   */
#define OP_ECHO     0x06   /* sink / echo / generate with CRC-32 (throughput, long SysEx) */
#define OP_APO      0x07   /* APO + BT state snapshot (read-only)                       */
#define OP_BTLOG    0x08   /* BT audio module UART RX ring snapshot (read-only)         */
#define OP_CANARY   0x09   /* R1 canary: arm / count changed words                      */
#define OP_CORE     0x0A   /* core self-test, answered from the engine loop (FEAT1_CORE) */

/* NG_PROTO = OpenGrand protocol version (Identify byte 1 ngver, REMOTE_PROTOCOL.md section 10.0): below */
#define BASE_FW_MAJ 2  /* stock image this cave was built against: V2.19 */
#define BASE_FW_MIN 19

/* feature bits reported by IDENTIFY (feat0) */
#define FEAT_IDENTIFY 0x01
#define FEAT_STATUS   0x02
#define FEAT_NORDUMP  0x04
#define FEAT_PORT     0x08
#define FEAT_ECHO     0x10
#define FEAT_APO      0x20
#define FEAT_CANARY   0x40
/* feature bits reported by IDENTIFY (feat1) */
#define FEAT1_BTLOG   0x01
#define FEAT1_CORE    0x02
#define FEAT1_OTA     0x04   /* built for an ota_loader: built-in cave or slot payload (src/ota.c) */
#define FEAT1_POWER   0x10   /* power policy / standby port (slot payload ota_power)            */
#define FEAT1_SYSEX   0x20   /* Phase 3-1 SysEx front (slot payload ota_sysex, with POWER)      */
#define FEAT1_TVWAKE  0x40   /* TV wake in the power port (slot payload ota_power2, with POWER) */

#ifndef CA79NG_FEAT0
#error "pick an image: build with -DCA79NG_FEAT0=0x.. -DCA79NG_FEAT1=0x.. (see tools/images.py)"
#endif
#ifndef CA79NG_FEAT1
#define CA79NG_FEAT1 0
#endif
#ifndef CA79NG_FEAT2
#define CA79NG_FEAT2 0
#endif
#ifndef CA79NG_FEAT3
#define CA79NG_FEAT3 0
#endif
#define FEAT2_BT      0x01   /* BT port (slot payload ota_bt, with POWER)                           */
#define FEAT2_SESSION 0x02   /* Phase 3-2 session core (slot payload ota_session, with SYSEX)        */
#define FEAT2_RELAY   0x04   /* panel-UI relay 0x30-0x3F <-> port 3 (ota_main, with POWER)            */
#define FEAT2_STARTUP 0x08   /* startup settings owned by the core (with SESSION)                    */
#define FEAT2_REC     0x10   /* Phase 3-6 R1 playback control, op 0x1E (with SESSION)                */
#define FEAT2_PWROFF  0x20   /* remote off op 19 03 + the core's binding table for the power ops (with BT, REC) */
#define FEAT2_S3      0x40   /* settings store as read owner, op 1B 05 04 / 05 (with SESSION)       */
#define FEAT3_REC2    0x01   /* Phase 3-6 R2 record control (with REC) - fourth byte, in ota_main      */
#define FEAT3_GATE    0x02   /* Phase 4 A intake gate (with SESSION) - in ota_main                     */
#define FEAT3_REC3    0x04   /* Phase 3-6 R3 save decision (with REC2) - in ota_main                   */
#define FEAT3_S4      0x20   /* settings store as write owner, op 1B 05 06 / 07 (with S3) - in ota_main */
#define FEAT3_XGATE   0x08   /* Phase 4 executor gate, op 0x1F (with SESSION), on at boot (1F 01 0/1)   */
#define FEAT3_MIDICH  0x10   /* Phase 4 MIDI channel messages / delayed apply by the core, op 1B 07 (with SESSION) */

#define FEATURES0 (CA79NG_FEAT0)
#define FEATURES1 (CA79NG_FEAT1)
#define FEATURES2 (CA79NG_FEAT2)
#define FEATURES3 (CA79NG_FEAT3)

#if (FEATURES0 & (FEAT_IDENTIFY | FEAT_STATUS)) != (FEAT_IDENTIFY | FEAT_STATUS)
#error "identify and status are the framework itself: every image carries them"
#endif
#if (FEATURES0 & ~0x7F) || (FEATURES1 & ~(FEAT1_BTLOG | FEAT1_CORE | FEAT1_OTA | FEAT1_POWER | FEAT1_SYSEX | FEAT1_TVWAKE))
#error "unknown feature bit (feature bytes are 7-bit and every bit must mean something)"
#endif
#if FEATURES2 & ~(FEAT2_BT | FEAT2_SESSION | FEAT2_RELAY | FEAT2_STARTUP | FEAT2_REC | FEAT2_PWROFF | FEAT2_S3)
#error "unknown feature bit in the third feature byte"
#endif
#if FEATURES3 & ~(FEAT3_REC2 | FEAT3_GATE | FEAT3_REC3 | FEAT3_XGATE | FEAT3_S4 | FEAT3_MIDICH)
#error "unknown feature bit in the fourth feature byte"
#endif
#if FEATURES3 && !FEATURES2
#error "IDENTIFY sends the fourth feature byte after the third: a feat3 build needs feat2"
#endif

#define HAVE_DUMP    ((FEATURES0 & FEAT_NORDUMP) != 0)
#define HAVE_PORT    ((FEATURES0 & FEAT_PORT) != 0)
#define HAVE_ECHO    ((FEATURES0 & FEAT_ECHO) != 0)
#define HAVE_APO     ((FEATURES0 & FEAT_APO) != 0)
#define HAVE_CANARY  ((FEATURES0 & FEAT_CANARY) != 0)
#define HAVE_BTLOG   ((FEATURES1 & FEAT1_BTLOG) != 0)
#define HAVE_CORE    ((FEATURES1 & FEAT1_CORE) != 0)
#define HAVE_POWER   ((FEATURES1 & FEAT1_POWER) != 0)
/* the shared reply buffer (rbuf / rb_* / pack7) of the diagnostic ops */
#define HAVE_RBUF    (HAVE_PORT || HAVE_ECHO || HAVE_APO || HAVE_BTLOG || HAVE_CANARY)

#if (FEATURES1 & FEAT1_OTA) && (HAVE_DUMP || HAVE_CANARY)
#error "OTA builds keep staging / run / read-back buffers in R1 (include/ota.h): no NOR dump, no canary"
#endif
#if HAVE_DUMP && HAVE_CANARY
#error "the NOR dump keeps its 64 KB buffers at 0x401E0000, inside the canary range"
#endif
#if HAVE_CORE && HAVE_CANARY
#error "the core keeps its RW state in R1 (cave_core.ld), inside the canary range"
#endif
#if HAVE_POWER && !(HAVE_CORE && (FEATURES1 & FEAT1_OTA))
#error "the power port is a slot payload of ota_loader_v2 with the core (runtime hooks, core RW)"
#endif
/* The power family = ota_main, every feature at once (feat1 0x77, feat2 0x7F, feat3 0x3F).  The per-feature switches
 * (SysEx front, BT port, session core, relay, startup, playback / record control, remote off, S3 / S4, intake gate, R3,
 * executor gate, MIDI channel, TV wake) were folded after their field tests: the code is always in, the feature bytes are only what IDENTIFY reports (frozen - new features raise
 * NG_PROTO instead).  ota_power* / ota_main_15 / ota_main_20 are frozen blobs rebuilt from their own commits.  Bits outside the power
 * family stay real switches (NOR dump, probes, canary, BT log, core, OTA: the diagnostic / loader images). */
#if HAVE_POWER
/* NG_PROTO_VERSION and the frozen feature bytes: the protocol SSOT (result/protocol/spec/ca79.json ng_versions,
 * generated by make gen - result/protocol/NGVER.md) */
#include "ca79ng/ngver_gen.h"
#define NG_PROTO NG_PROTO_VERSION
#else
#define NG_PROTO 1   /* the diagnostic / loader images: version 1, the feature bits are the truth */
#endif
#if (FEATURES1 & (FEAT1_SYSEX | FEAT1_TVWAKE)) && !HAVE_POWER
#error "the SysEx front and TV wake are part of the power-family slot payload (ota_main)"
#endif
#if (FEATURES2 || FEATURES3) && !HAVE_POWER
#error "the third / fourth feature bytes are ota_main's"
#endif
#if HAVE_POWER && (FEATURES0 != NG_FEAT0_FROZEN || FEATURES1 != NG_FEAT1_FROZEN || FEATURES2 != NG_FEAT2_FROZEN || FEATURES3 != NG_FEAT3_FROZEN)
#error "a power-family payload built from source is ota_main with the frozen feature bytes (ngver_gen.h): no per-feature builds"
#endif

#if HAVE_CORE
#include "core_cave.h"   /* result/firmware/ports/cave */
#if CORE_OP_SELFTEST != OP_CORE
#error "core_cave.h and this file disagree on the self-test op"
#endif
#endif
#if HAVE_POWER
#include "power_cave.h"  /* result/firmware/ports/cave */
#include "gpio_cave.h"   /* result/firmware/ports/cave - read-only GPIO diagnostic, op 0x17 */
#include "sysex_cave.h"  /* result/firmware/ports/cave */
#include "bt_cave.h"     /* result/firmware/ports/cave */
#include "relay_cave.h"  /* result/firmware/ports/cave */
#include "startup_cave.h" /* result/firmware/ports/cave */
#include "recctl_cave.h"  /* result/firmware/ports/cave */
#include "xgate_cave.h"   /* result/firmware/ports/cave */
#endif

/* dump states, reported verbatim by OP_STATUS */
enum { ST_IDLE = 0, ST_START = 1, ST_RUN = 2, ST_CLOSE = 3, ST_DONE = 4, ST_ERROR = 5 };

/* error codes, reported verbatim by OP_STATUS (7-bit) */
enum {
    E_OK = 0, E_NOMEDIA = 1, E_FS = 2, E_HANDLE_BUSY = 3, E_FLASH_BUSY = 4,
    E_SIGNATURE = 5, E_MISMATCH = 6, E_WRITE = 7, E_SHORT_WRITE = 8,
    E_CLOSE = 9, E_SIZE = 10, E_CANCELLED = 11, E_BUSY = 12, E_UNSUPPORTED = 13
};

#define FLASH_BASE  0x60000000u
#define DUMP_BYTES  0x600000u   /* 6 MB: the whole partition table */
#define CHUNK       0x10000u    /* 64 KB */
#define CHUNKS      (DUMP_BYTES / CHUNK)
#define WINDOW      0x8000u     /* the NOR window the stock copier moves                */
#define VERIFY_PIECE 0x1000u    /* re-read granularity for the compare pass             */

/* Scratch RAM inside the same unused hole as the cave, above our code and below the
 * voice DB at 0x40200000.  Not copied from flash: we only ever write before reading.
 * No stock literal points here and no C64x constant does
 * either (dsp_scan.py).  Dynamic use remains [unverified]. */
#define BUF    ((u8 *)0x401E0000u)   /* 64 KB chunk buffer   */
#define VBUF   ((u8 *)0x401F0000u)   /* 4 KB verify buffer   */

#if HAVE_DUMP
static const char dump_path[] = "\\CA79NOR.BIN";
#endif

/* -fno-zero-initialized-in-bss keeps these in .data, which is loaded from flash. */
static u8  dump_state = ST_IDLE;
static u8  dump_err = E_OK;
static u16 dump_chunk = 0;
#if HAVE_DUMP
static u32 dump_crc = 0;
#endif

/* ---------------------------------------------------------------------------- */

static void tx(u8 port, const u8 *buf, u32 len)
{
    switch (port & 3) {
    case 0: stk_tx_port0(buf, len); break;
    case 1: stk_tx_port1(buf, len); break;
    case 2: stk_tx_port2(buf, len); break;
    default: stk_tx_port3(buf, len); break;
    }
}

/* Build `F0 7D C A 7 9 <op|0x40> <data...> F7` and send it out of the port the request
 * came in on - no dependency on the bound controller or on port 3 forwarding. */
static void reply(u8 port, u8 op, const u8 *data, u32 n)
{
    u8 msg[16];
    u32 i = 0;
    msg[i++] = 0xF0; msg[i++] = 0x7D;
    msg[i++] = TAG0; msg[i++] = TAG1; msg[i++] = TAG2; msg[i++] = TAG3;
    msg[i++] = (u8)(op | 0x40);
    while (n-- && i < sizeof msg - 1)
        msg[i++] = *data++ & 0x7F;
    msg[i++] = 0xF7;
    tx(port, msg, i);
}

#if HAVE_DUMP || HAVE_ECHO
static u32 crc32_step(u32 crc, const u8 *p, u32 n)
{
    /* nibble-wise CRC-32 (reflected, poly 0xEDB88320) - 16-entry table, no 1 KB blob */
    static const u32 tbl[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ tbl[crc & 0x0F];
        crc = (crc >> 4) ^ tbl[crc & 0x0F];
    }
    return crc;
}
#endif

#if HAVE_DUMP
/* Read `len` bytes of NOR into RAM with the chip to ourselves.  The stock copier picks
 * the 32 KB window, so we hand it one window at a time and re-check the busy flag for
 * each: a stock writer must never be interrupted mid command cycle. */
static int flash_read(u32 src, u8 *dst, u32 len)
{
    u32 done = 0;
    while (done < len) {
        u32 n = len - done;
        int tries = 0;
        int ok = 0;
        if (n > WINDOW)
            n = WINDOW;
        while (!ok) {
            stk_dis_dsp();
            if (!stk_flash_busy()) {
                stk_flash_copy(src + done, dst + done, n);
                ok = 1;
            }
            stk_ena_dsp();
            if (!ok) {
                if (++tries > 200)
                    return E_FLASH_BUSY;
                stk_dly_tsk(1);
            }
        }
        done += n;
    }
    return E_OK;
}

static int mem_differs(const u8 *a, const u8 *b, u32 n)
{
    while (n--)
        if (*a++ != *b++)
            return 1;
    return 0;
}

static void dump_fail(u8 code)
{
    stk_file_close_write(1);
    stk_file_delete(dump_path);
    dump_err = code;
    dump_state = ST_ERROR;
}

/* Pre-flight: a stick is mounted, the single global write handle is free, and the
 * first two words of the MainProgram partition look like the stock reset vector -
 * which is also how we check that the window hypothesis holds. */
static void dump_start(void)
{
    u32 head[2];

    if (stk_media_present() != 0) { dump_err = E_NOMEDIA; dump_state = ST_ERROR; return; }
    if (stk_fs_ready(0) != 0)     { dump_err = E_FS;      dump_state = ST_ERROR; return; }
    if (stk_write_handle_ptr != 0) { dump_err = E_HANDLE_BUSY; dump_state = ST_ERROR; return; }

    if (flash_read(FLASH_BASE + 0x60000u, (u8 *)head, sizeof head) != E_OK) {
        dump_err = E_FLASH_BUSY; dump_state = ST_ERROR; return;
    }
    if (head[0] != 0xE51FF004u || head[1] != 0x4009D6D4u) {
        dump_err = E_SIGNATURE; dump_state = ST_ERROR; return;
    }

    /* Deliberately NOT raising the stock busy indicator (FUN_4013403C): stk_flash_busy()
     * reads the same flag, so setting it would make every read below think a stock writer
     * owns the chip. It bought little anyway - the delayed-write task clears the flag when
     * it finishes, and a power cut during a read-only dump cannot damage the flash. */
    dump_chunk = 0;
    dump_crc = 0xFFFFFFFFu;
    dump_state = ST_RUN;
    dump_err = E_OK;
}

static void dump_step(void)
{
    u32 off = (u32)dump_chunk * CHUNK;
    u32 written = 0;
    u32 piece;
    int rc;

    rc = flash_read(FLASH_BASE + off, BUF, CHUNK);
    if (rc != E_OK) { dump_fail((u8)rc); return; }

    /* second read, compared piece by piece - catches a writer that moved the window
     * underneath us as well as a wrong window hypothesis */
    for (piece = 0; piece < CHUNK; piece += VERIFY_PIECE) {
        rc = flash_read(FLASH_BASE + off + piece, VBUF, VERIFY_PIECE);
        if (rc != E_OK) { dump_fail((u8)rc); return; }
        if (mem_differs(BUF + piece, VBUF, VERIFY_PIECE)) { dump_fail(E_MISMATCH); return; }
    }

    dump_crc = crc32_step(dump_crc, BUF, CHUNK);

    rc = stk_file_write(dump_chunk == 0 ? dump_path : "", CHUNK, BUF, &written);
    if (rc != 0) { dump_fail(E_WRITE); return; }
    if (written != CHUNK) { dump_fail(E_SHORT_WRITE); return; }

    if (++dump_chunk >= CHUNKS)
        dump_state = ST_CLOSE;
}

static void dump_close(void)
{
    u32 size = 0;

    if (stk_file_close_write(1) != 0) { dump_fail(E_CLOSE); return; }
    if (stk_file_size(dump_path, &size) != 0 || size != DUMP_BYTES) {
        dump_err = E_SIZE; dump_state = ST_ERROR; return;
    }
    dump_crc ^= 0xFFFFFFFFu;
    dump_state = ST_DONE;
}
#endif /* HAVE_DUMP */

#if HAVE_RBUF
/* ============================================================================
 * Diagnostic ops (FEAT_PORT/ECHO/APO/CANARY, FEAT1_BTLOG).  Everything below except
 * the canary fill only READS stock state
 * (INDEX rule: no stock global is written).  Replies that carry 8-bit data use the
 * 7-in-8 packing of pack7(); echo payloads are already 7-bit and go back verbatim.
 * ========================================================================== */

#define ECHO_MAX   0x7C0u  /* echo/generate cap: whole reply < the 2 KB TX ring */
#define RBUF_SIZE  (7u + 2u + 8u + ECHO_MAX + 1u)
#define BTRING     0x800u  /* UART1 RX ring of the BT audio module: 2 KB, 11-bit index      */
#define BTPAGE     128u
#define CANARY_HI  0x40200000u  /* R1 ends where loader copy #2 (voice DB) starts       */
#define CANARY_XOR 0xC79A5A5Cu
#define CANARY(a)  ((a) ^ CANARY_XOR)

enum { CAN_OFF = 0, CAN_ARM = 1, CAN_ARMED = 2 };

extern u8 cave_end[];     /* linker: first byte after the cave */

#if HAVE_POWER
/* ota_main family: in the core's NOLOAD RW (0x40180000 - 0x401A0000, cave_ota / ota_payload .core_rw) instead of
 * 2 KB of zero bytes in the slot - every reply writes the bytes it sends first */
static u8  rbuf[RBUF_SIZE] __attribute__((section(".core_rw")));
#else
static u8  rbuf[RBUF_SIZE] = {0};
#endif
static u32 rlen = 0;
#if HAVE_BTLOG
static u8  bt_snap[BTRING] = {0};
static u16 bt_snap_wr = 0;
static u16 bt_snap_rd = 0;
static u8  bt_snap_line = 0;
static u8  bt_snap_seq = 0;
#endif
#if HAVE_CANARY
static volatile u8  can_state = CAN_OFF;
static volatile u32 can_ticks = 0;

static u32 canary_lo(void)
{
    return ((u32)cave_end + 0xFFu) & ~0xFFu;
}
#endif

/* 8-bit -> 7-bit: every group of up to 7 bytes becomes one prefix byte holding the top
 * bits (bit i = bit 7 of byte i) followed by the low 7 bits of each byte. */
static u32 pack7(u8 *out, const u8 *in, u32 n)
{
    u32 o = 0;
    while (n) {
        u32 g = n < 7 ? n : 7;
        u32 i;
        u8 hi = 0;
        for (i = 0; i < g; i++)
            if (in[i] & 0x80)
                hi |= (u8)(1u << i);
        out[o++] = hi;
        for (i = 0; i < g; i++)
            out[o++] = in[i] & 0x7F;
        in += g;
        n -= g;
    }
    return o;
}

#if HAVE_PORT || HAVE_ECHO || HAVE_CANARY
static void put_u16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put_u32(u8 *p, u32 v) { put_u16(p, v); put_u16(p + 2, v >> 16); }
#endif

static void rb_begin(u8 op)
{
    rbuf[0] = 0xF0; rbuf[1] = 0x7D;
    rbuf[2] = TAG0; rbuf[3] = TAG1; rbuf[4] = TAG2; rbuf[5] = TAG3;
    rbuf[6] = (u8)(op | 0x40);
    rlen = 7;
}

#if HAVE_ECHO || HAVE_BTLOG || HAVE_CANARY
static void rb_byte(u8 b) { if (rlen < RBUF_SIZE - 1) rbuf[rlen++] = b & 0x7F; }
#endif

static void rb_pack(const u8 *raw, u32 n)
{
    /* packed size = n + ceil(n / 7); callers keep n small enough to fit */
    rlen += pack7(rbuf + rlen, raw, n);
}

static void rb_send(u8 port)
{
    rbuf[rlen++] = 0xF7;
    tx(port, rbuf, rlen);
}

#if HAVE_PORT
/* op 0x05: which port did this arrive on, and how does the stock router see it. */
static void probe_port(u8 port, u32 len)
{
    u8 raw[8];
    raw[0] = port;
    raw[1] = stk_binding;               /* controller port + 1, 0 = none   */
    put_u32(raw + 2, stk_port3_fwd);    /* port 3 -> 0..2 forwarding flag (01_power)        */
    put_u16(raw + 6, len);              /* total length as the stock receiver delivered it  */
    rb_begin(OP_PORT);
    rb_pack(raw, sizeof raw);
    rb_send(port);
}

#endif

#if HAVE_ECHO
/* op 0x06: mode 0 sink (count + CRC), mode 1 echo (count + CRC + payload),
 * mode 2 generate (`06 02 n_hi7 n_lo7 seed`: n bytes of (seed + i) & 0x7F + CRC). */
static void probe_echo(u8 port, const u8 *buf, u32 len)
{
    u8 raw[6];
    u8 mode;
    u32 n, i, crc;

    if (len < 9)
        return;
    mode = buf[7];
    rb_begin(OP_ECHO);
    rb_byte(mode);
    if (mode == 0 || mode == 1) {
        n = len - 9;                    /* bytes between the mode byte and F7 */
        crc = crc32_step(0xFFFFFFFFu, buf + 8, n) ^ 0xFFFFFFFFu;
        put_u16(raw, n);
        put_u32(raw + 2, crc);
        rb_pack(raw, sizeof raw);
        if (mode == 1) {
            if (n > ECHO_MAX)
                n = ECHO_MAX;
            for (i = 0; i < n; i++)
                rbuf[rlen++] = buf[8 + i] & 0x7F;
        }
    } else if (mode == 2) {
        u8 seed;
        u8 *out;
        if (len < 12)
            return;
        n = ((u32)(buf[8] & 0x7F) << 7) | (buf[9] & 0x7F);
        seed = buf[10];
        if (n > ECHO_MAX)
            n = ECHO_MAX;
        out = rbuf + rlen + 7;          /* leave room for the packed count + CRC */
        for (i = 0; i < n; i++)
            out[i] = (u8)((seed + i) & 0x7F);
        crc = crc32_step(0xFFFFFFFFu, out, n) ^ 0xFFFFFFFFu;
        put_u16(raw, n);
        put_u32(raw + 2, crc);
        rb_pack(raw, sizeof raw);       /* exactly 7 bytes */
        rlen += n;
    } else {
        return;                         /* unknown mode: silent */
    }
    rb_send(port);
}

#endif

#if HAVE_APO
/* op 0x07: APO counter block + BT link state, copied as-is (02_apo_a2dp). */
static void probe_apo(u8 port)
{
    u8 raw[32];
    u32 i;
    for (i = 0; i < 28; i++)            /* 0x4139E7E4..0x4139E7FF */
        raw[i] = stk_apo_block[i];
    raw[28] = stk_a2dp_flag;            /* 0x413AE5B8 */
    raw[29] = stk_bt_ready;             /* 0x413AE5C1 */
    raw[30] = (u8)stk_bt_dialect();     /* FUN_400ACFF8: 1 = AT, 0 = iWRAP (reads 2 registers) */
    raw[31] = stk_special_mode;         /* 0x419156A7: 1 = APO paused (test / wave ROM) */
    rb_begin(OP_APO);
    rb_pack(raw, sizeof raw);
    rb_send(port);
}

#endif

#if HAVE_BTLOG
/* op 0x08: `08 00` freezes the 2 KB RX ring (oldest byte first) into our own buffer,
 * `08 01 <page>` returns 128 bytes of that snapshot.  Reads only; the ISR may be adding
 * bytes while we copy, so the newest line can be torn. */
static void probe_btlog(u8 port, const u8 *buf, u32 len)
{
    u8 raw[BTPAGE + 1];
    u32 i;

    if (len < 9)
        return;
    if (buf[7] == 0) {
        u32 wr = stk_bt_rx_wr & (BTRING - 1);
        bt_snap_rd = stk_bt_rx_rd & (BTRING - 1);
        bt_snap_line = stk_bt_line_len;
        for (i = 0; i < BTRING; i++)
            bt_snap[i] = stk_bt_rx_ring[(wr + i) & (BTRING - 1)];
        bt_snap_wr = (u16)wr;
        bt_snap_seq = (u8)((bt_snap_seq + 1) & 0x7F);
        raw[0] = (u8)bt_snap_wr; raw[1] = (u8)(bt_snap_wr >> 8);
        raw[2] = (u8)bt_snap_rd; raw[3] = (u8)(bt_snap_rd >> 8);
        raw[4] = bt_snap_line;
        raw[5] = bt_snap_seq;
        rb_begin(OP_BTLOG);
        rb_byte(0);
        rb_pack(raw, 6);
        rb_send(port);
    } else if (buf[7] == 1 && len >= 10) {
        u32 page = buf[8] & 0x7F;
        if (page >= BTRING / BTPAGE)
            return;
        raw[0] = bt_snap_seq;
        for (i = 0; i < BTPAGE; i++)
            raw[1 + i] = bt_snap[page * BTPAGE + i];
        rb_begin(OP_BTLOG);
        rb_byte(1);
        rb_byte((u8)page);
        rb_pack(raw, BTPAGE + 1);
        rb_send(port);
    }
}

#endif

#if HAVE_CANARY
/* op 0x09: `09 01` asks the engine loop to fill R1 above the cave with an address
 * dependent pattern; `09 00` counts the words that no longer hold it. */
static void probe_canary(u8 port, const u8 *buf, u32 len)
{
    u8 raw[25];
    u32 lo = canary_lo(), a, changed = 0, first = 0, last = 0;
    u8 st;

    if (len < 9)
        return;
    if (buf[7] == 1) {
        if (can_state != CAN_ARM)
            can_state = CAN_ARM;        /* the engine loop fills it on its next pass */
        rb_begin(OP_CANARY);
        rb_byte(1);
        rb_byte(can_state);
        rb_send(port);
        return;
    }
    if (buf[7] != 0)
        return;
    st = can_state;
    if (st == CAN_ARMED) {
        for (a = lo; a < CANARY_HI; a += 4) {
            if (*(volatile u32 *)a != CANARY(a)) {
                if (!changed)
                    first = a;
                last = a;
                changed++;
            }
        }
    }
    raw[0] = st;
    put_u32(raw + 1, changed);
    put_u32(raw + 5, first);
    put_u32(raw + 9, last);
    put_u32(raw + 13, lo);
    put_u32(raw + 17, CANARY_HI);
    put_u32(raw + 21, can_ticks);
    rb_begin(OP_CANARY);
    rb_byte(0);
    rb_pack(raw, sizeof raw);
    rb_send(port);
}

static void probe_tick(void)
{
    if (can_state == CAN_ARM) {
        u32 a;
        for (a = canary_lo(); a < CANARY_HI; a += 4)
            *(volatile u32 *)a = CANARY(a);
        can_ticks = 0;
        can_state = CAN_ARMED;
    } else if (can_state == CAN_ARMED) {
        can_ticks++;
    }
}
#endif /* HAVE_CANARY */
#endif /* HAVE_RBUF */

/* Engine loop tail, roughly every 8 ms.  One chunk per visit; everything else returns
 * immediately so the stock loop keeps its timing when no dump is running. */
void ng_tick(void)
{
#if HAVE_CORE
    core_tick();        /* dormant core: time accounting + a pending self-test request */
#endif
#if HAVE_POWER
    pwc_tick();         /* power policy, standby, APO rule, queued 0x18 / 0x19 / 0x1A */
    sxc_tick();         /* Phase 3-1 front: its mirror store is set up on the first pass */
    btc_tick();         /* port 2 latch clear, AS guard, BT module query, queued 0x1D */
    xgc_tick();         /* executor gate: hook / unhook its two v3 sites when switched (op 1F 01) */
    gdc_tick();         /* GPIO diagnostic (read only): sample the watched pins, answer a queued 0x17, change notice */
#endif
#if HAVE_CANARY
    probe_tick();
#endif
#if HAVE_DUMP
    switch (dump_state) {
    case ST_START: dump_start(); break;
    case ST_RUN:   dump_step();  break;
    case ST_CLOSE: dump_close(); break;
    default: break;
    }
#endif
}

#if FEATURES1 & FEAT1_OTA
/* OTA cave ABI (include/ota.h struct ota_entry, ota_loader_v2 "apply now").
 * ng_quiesce: 0 = idle - nothing pending, no stock resource held.  In the OTA builds the only
 * deferred work is a core self-test request (answered on the next tick); every other op is
 * answered inside ng_sysex, and the NOR dump / canary (which own a file handle / R1) are
 * compile-time excluded above.
 * ng_restart: the built-in cave only (the resident calls it before switching BACK to the
 * built-in): its .data was loaded once at power-on and a slot core may have reused the core RW
 * since, so the core starts again from scratch on the next tick.  A slot payload needs no
 * restart: the resident copies it fresh, .data included. */
int ng_quiesce(void)
{
#if HAVE_POWER
    if (pwc_quiesce())
        return 1;       /* a queued op, or not ON yet (a standby is woken first) */
    if (btc_quiesce())
        return 1;       /* a queued op 0x1D, or a module query still on the wire */
    if (suc_quiesce())
        return 1;       /* a queued op 0x1C, or a startup snapshot not written yet */
    if (rcc_quiesce())
        return 1;       /* an op 0x1E command waiting for the pump tail */
#if HAVE_CORE
    if (!core_idle())
        return 1;
#endif
    btc_handover();     /* idle: the next payload gets the connected BT devices and the selection */
    return 0;
#elif HAVE_CORE
    return core_idle() ? 0 : 1;
#else
    return 0;
#endif
}

void ng_restart(void)
{
#if HAVE_CORE
    core_restart();
#endif
}
#endif

/* ---------------------------------------------------------------------------- */

static void send_status(u8 port)
{
    u8 d[4];
    d[0] = dump_state;
    d[1] = (u8)((dump_chunk >> 7) & 0x7F);
    d[2] = (u8)(dump_chunk & 0x7F);
    d[3] = dump_err;
    reply(port, OP_STATUS, d, 4);
}

void ng_sysex(u32 port, const u8 *buf, u32 len)
{
    u8 op;

    if (len < 8 || buf[0] != 0xF0 || buf[1] != 0x7D || buf[len - 1] != 0xF7)
        return;
    if (buf[2] != TAG0 || buf[3] != TAG1 || buf[4] != TAG2 || buf[5] != TAG3)
        return;

    op = buf[6];
#if HAVE_POWER
    pwc_frame_7d(buf, len);   /* polls / reads are not APO activity (power_cave.c PWC_QUIET) */
    if (rlc_owns(op)) {   /* panel-UI ops 0x30-0x3F / replies 0x70-0x7F: passed through, never read */
        rlc_sysex((u8)port, buf, len);
        return;
    }
#endif
    switch (op) {
    case OP_IDENTIFY: {
#if FEATURES3
        u8 d[7];
#elif FEATURES2
        u8 d[6];
#else
        u8 d[5];
#endif
        d[0] = NG_PROTO;
        d[1] = BASE_FW_MAJ;
        d[2] = BASE_FW_MIN;
        d[3] = FEATURES0;
        d[4] = FEATURES1;
#if FEATURES3
        d[5] = FEATURES2;
        d[6] = FEATURES3;   /* the fourth feature byte goes last: older apps read the first six and stop */
        reply((u8)port, OP_IDENTIFY, d, 7);
#elif FEATURES2
        d[5] = FEATURES2;   /* only builds with a third feature byte answer 6 bytes */
        reply((u8)port, OP_IDENTIFY, d, 6);
#else
        reply((u8)port, OP_IDENTIFY, d, 5);
#endif
        break;
    }
    case OP_STATUS:
        send_status((u8)port);
        break;
    case OP_DUMP: {
#if HAVE_DUMP
        u8 r = E_OK;
        if (dump_state == ST_RUN || dump_state == ST_START || dump_state == ST_CLOSE)
            r = E_BUSY;
        else {
            dump_chunk = 0;
            dump_err = E_OK;
            dump_state = ST_START;   /* the engine loop picks it up on its next pass */
        }
        reply((u8)port, OP_DUMP, &r, 1);
#else
        u8 r = E_UNSUPPORTED;
        reply((u8)port, OP_DUMP, &r, 1);
#endif
        break;
    }
    case OP_CANCEL: {
        u8 s;
#if HAVE_DUMP
        if (dump_state == ST_RUN || dump_state == ST_START) {
            stk_file_close_write(1);
            stk_file_delete(dump_path);
            dump_err = E_CANCELLED;
            dump_state = ST_ERROR;
        }
#endif
        s = dump_state;
        reply((u8)port, OP_CANCEL, &s, 1);
        break;
    }
#if HAVE_PORT
    case OP_PORT:   probe_port((u8)port, len);      break;
#endif
#if HAVE_ECHO
    case OP_ECHO:   probe_echo((u8)port, buf, len); break;
#endif
#if HAVE_APO
    case OP_APO:    probe_apo((u8)port);            break;
#endif
#if HAVE_BTLOG
    case OP_BTLOG:  probe_btlog((u8)port, buf, len); break;
#endif
#if HAVE_CANARY
    case OP_CANARY: probe_canary((u8)port, buf, len); break;
#endif
#if HAVE_CORE
    case OP_CORE:   core_request((u8)port);           break;   /* runs on the next tick */
#endif
#if HAVE_POWER
    case PW_OP_POLICY:
    case PW_OP_STANDBY:
    case PW_OP_DIAG: pwc_sysex((u8)port, buf, len);   break;   /* runs on the next tick */
    case GDC_OP:    gdc_sysex((u8)port, buf, len);    break;   /* read only; runs on the next tick */
    case SX_OP_FRONT:
#if HAVE_POWER
        if (len == 9 && buf[7] == RL_DIAG_SUB) {   /* 1B 10: the relay's counters */
            rlc_diag((u8)port);
            break;
        }
#endif
        sxc_sysex((u8)port, buf, len);   /* answered at once */
        break;
    case BT_OP:     btc_sysex((u8)port, buf, len);    break;   /* runs on the next tick */
    case SU_OP:     suc_sysex((u8)port, buf, len);    break;   /* status at once, the rest from the pump tail */
    case RCC_OP:    rcc_sysex((u8)port, buf, len);    break;   /* status / list at once, commands from the pump tail */
    case XGC_OP:    xgc_sysex((u8)port, buf, len);    break;   /* answered at once, the switch applied on the next tick */
#endif
    default:
        break;   /* unknown op: stay silent, exactly like the stock firmware */
    }
}
