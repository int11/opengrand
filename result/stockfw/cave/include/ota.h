/* OpenGrand remote cave update (OTA) - layout and wire constants shared by the resident stage
 * (src/ota.c, image ota_loader) and slot payloads (src/ota_payload.c).
 *
 * Contract: result/protocol/REMOTE_PROTOCOL.md section 10.3.
 * The Python side (tools/ota_pack.py, tools/roundtrip_ota.py) and the app
 * (result/app/src/protocol/ota.ts) mirror these numbers; change them together.
 */
#ifndef CA79NG_OTA_H
#define CA79NG_OTA_H

struct s0_svc;                           /* include/s0.h (ota_loader_v4 stage-0) */

/* ---- NOR (CPU addresses; NOR offset = addr - 0x60000000) ---------------------------------
 * The only range the cave may ever erase or program: the 0xFF hole of the MainProgram
 * partition (file 0x120000-0x1A0000), no stock literal, not in the partition table,
 * 4 uniform 128 KB sectors.          */
#define OTA_SLOT_LO     0x60180000u
#define OTA_SLOT_HI     0x60200000u
#define OTA_SECTOR      0x20000u
#define OTA_PAGE        0x200u           /* write-buffer page (FUN_40133D50, <= 512 B)  */
#define OTA_SLOT_A      0x60180000u      /* slot = one sector; 0x601A0000 / 0x601E0000 reserved */
#define OTA_SLOT_B      0x601C0000u

/* slot sector layout */
#define OTA_ST_MAGIC    0x00u            /* u32 'C79S', programmed LAST = commit point   */
#define OTA_ST_SEQ      0x04u            /* u32 sequence number                         */
#define OTA_ST_CONFIRM  0x08u            /* u16 0x0000 once a boot of it ran 60 s       */
#define OTA_ST_REJECT   0x0Au            /* u16 0x0000 = never boot this slot again     */
#define OTA_ST_LIVE     0x0Cu            /* u16 0x0000 once a LIVE-applied run of it lasted 60 s
                                          * (ota_loader_v2; NOT a boot confirmation: boot attempts
                                          * still count until OTA_ST_CONFIRM is written by a boot) */
#define OTA_ST_ATTEMPT  0x10u            /* u16[16] one 0xFFFF->0x0000 per boot attempt  */
#define OTA_ATTEMPTS    16u
#define OTA_STATE_SIZE  0x200u
#define OTA_IMAGE_OFF   0x200u           /* signed image (header + payload) from here    */
#define OTA_IMAGE_MAX   (OTA_SECTOR - OTA_IMAGE_OFF)

/* ---- image header (64 B, built and signed by tools/ota_pack.py) ------------------------ */
#define OTA_HDR_SIZE    64u
#define OTA_H_MAGIC     0x00u            /* u32 'C79P'                                   */
#define OTA_H_FORMAT    0x04u            /* u16 1                                        */
#define OTA_H_HDRSIZE   0x06u            /* u16 64                                       */
#define OTA_H_LEN       0x08u            /* u32 payload length                           */
#define OTA_H_LOAD      0x0Cu            /* u32 load (run) address = OTA_RUN_BASE         */
#define OTA_H_ENTRY     0x10u            /* u32 entry table offset from load (0)          */
#define OTA_H_CRC       0x14u            /* u32 CRC-32 (zlib) of the payload              */
#define OTA_H_STOCK     0x18u            /* u32 first 4 bytes of the stock sha256 (LE)    */
#define OTA_H_ABI       0x1Cu            /* u32 OTA_ABI                                   */
#define OTA_H_FEAT      0x20u            /* u32 feat0 | feat1 << 8 (information)          */
#define OTA_H_KEYID     0x24u            /* u32 key id (see below)                        */
#define OTA_H_MAC       0x30u            /* 16 B HMAC-SHA256(key, hdr[0:0x30] || payload)[:16] */
#define OTA_MAC_LEN     16u
#define OTA_PAYLOAD_MAX (OTA_IMAGE_MAX - OTA_HDR_SIZE)

#define OTA_HDR_MAGIC   0x50393743u      /* bytes "C79P" */
#define OTA_STATE_MAGIC 0x53393743u      /* bytes "C79S" */
#define OTA_ENTRY_MAGIC 0x45393743u      /* bytes "C79E" */
#define OTA_FORMAT      1u
#define OTA_ABI         1u
#define OTA_STOCK_TAG   0x7085AD09u      /* stock sha256 09 ad 85 70 ...                  */
/* key id = first 4 bytes (LE) of HMAC-SHA256(key, "C79 key id") */

/* ---- SDRAM (R1, the 0xFF hole 0x40173A14-0x40200000; canary C6: no stock writer) -------- */
#define OTA_CORE_RW     0x40180000u      /* core RW of whichever cave is active (<128 KB) */
#define OTA_STAGE       0x401A0000u      /* upload / boot-verify staging, 128 KB          */
#define OTA_RUN_BASE    0x401C0000u      /* active slot payload runs here, 128 KB          */
#define OTA_VBUF        0x401E0000u      /* 4 KB read-back buffer                         */
#define OTA_VBUF_SIZE   0x1000u

/* ---- payload entry table (first bytes of the payload, at OTA_RUN_BASE) ------------------- */
struct ota_entry {
    unsigned int magic;                  /* OTA_ENTRY_MAGIC */
    unsigned int abi;                    /* OTA_ABI */
    void (*sysex)(unsigned int port, const unsigned char *buf, unsigned int len);
    void (*tick)(void);
    unsigned int feat;                   /* feat0 | feat1 << 8 */
    /* Quiesce (ota_loader_v2 "apply now"; this word was `reserved` = 0 in the first payloads, and
     * 0 still means "nothing to quiesce").  Called from the engine loop after the resident stopped
     * handing SysEx to this cave: return 0 once no work is pending and no stock resource (file
     * handle, busy flag, dispatch lock) is held - the resident then never calls the cave again and
     * overwrites it.  Non-zero = not yet: the resident keeps calling tick() and asks again on the
     * next pass, and gives up (old cave keeps running) after OTA_QUIESCE_TICKS.  The first loader
     * never reads the word, so payloads with a quiesce entry still boot there (ABI unchanged).   */
    int (*quiesce)(void);
};

/* ---- runtime hooks (ota_loader_v2) ----------------------
 * A slot payload may rewrite a few allow-listed `bl` words of the stock code in SDRAM so that
 * they call into the payload.  Only the resident writes those words (privileged single-word store
 * + D-clean + drain + I-invalidate, IRQs off) and it restores every one of them before the
 * payload is replaced (apply now) - a new payload re-registers its hooks in attach().
 *
 * Opt-in, ABI unchanged (still 1): a payload that wants the service sets OTA_EF_ATTACH in
 * entry->feat (bits 16..31 were always 0: feat0 | feat1 << 8) and puts attach() right after the
 * 24-byte entry table (struct ota_entry_x).  ota_loader (v1) never reads feat or +0x18, so such a
 * payload still boots there - attach() is simply never called and it must run without hooks.
 * attach() is called once from the engine loop (task 24) before the payload's first tick() after
 * it became active; hook_set / hook_clear work only from attach() and tick() (engine-loop task).
 *
 * Contract for hook code (the resident can not check it - see the record, "in flight"):
 *   - registers: as at the site's `bl` (r0-r3 arguments, lr = site + 4); the hook ends by
 *     calling / tail-calling the original target (allow-list) unless the record says otherwise;
 *   - OTA_HK_KTIMER sites (20 ms kernel-timer handler): never block (no twai_sem / dly_tsk /
 *     TX writer), a few microseconds at most; hook_set requires OTA_HKF_NOBLOCK there;
 *   - OTA_HK_TASK19 sites (MIDI task): the hook either tail-calls the stock target (`b`, lr
 *     untouched - no payload frame survives a blocking call), or sets *svc->inflight = 1 before
 *     its first call that may block and back to 0 when it no longer holds a payload frame
 *     (the resident does not replace the payload while it is 1, exactly as for 0x7D SysEx);
 *   - OTA_HK_TASK24 sites: same task as the resident (sequential), may call and return;
 *   - OTA_HK_TASK20 sites (ota_loader_v3, song / metronome task, priority 20 - above the engine
 *     loop like task 19): as OTA_HK_TASK19 but with *svc->inflight20 (svc->version >= 2).     */
#define OTA_EF_ATTACH   0x00010000u      /* entry->feat: struct ota_entry_x, attach() at +0x18 */
#define OTA_SVC_MAGIC   0x48393743u      /* bytes "C79H" */
#define OTA_SVC_VERSION 3u               /* ota_loader_v2 hands out 1; v3: 2 = inflight20 below exists;
                                          * ota_loader_v4 stage-1: 3 = nor below exists */
#define OTA_HKF_NOBLOCK 0x01u            /* hook_set flags: the target obeys the kernel-timer rules */

/* hook_set / hook_clear results */
#define OTA_HK_OK       0
#define OTA_HK_E_CTX    1                /* not from attach()/tick() in the engine-loop task, or no slot payload active */
#define OTA_HK_E_SITE   2                /* address not in the allow-list */
#define OTA_HK_E_ORIG   3                /* expected original word != allow-list word, or RAM word != original */
#define OTA_HK_E_TARGET 4                /* target not a word-aligned address inside the running payload */
#define OTA_HK_E_FLAGS  5                /* unknown flag, or a kernel-timer site without OTA_HKF_NOBLOCK */
#define OTA_HK_E_STATE  6                /* set: already hooked; clear: not hooked */
#define OTA_HK_E_BUSY   7                /* an apply is in progress (SysEx gate closed) */
#define OTA_HK_E_VERIFY 8                /* the word read back differs after the write (restored) */

/* allow-list context classes (status op 0x16) */
#define OTA_HK_TASK19   1
#define OTA_HK_TASK24   2
#define OTA_HK_KTIMER   3
#define OTA_HK_TASK20   4                /* ota_loader_v3: song / metronome / recorder task (priority 20) */

struct ota_svc {
    unsigned int magic;                  /* OTA_SVC_MAGIC */
    unsigned int version;                /* OTA_SVC_VERSION */
    int (*hook_set)(unsigned int site, unsigned int orig, unsigned int target, unsigned int flags);
    int (*hook_clear)(unsigned int site);
    volatile unsigned char *inflight;    /* task-19 hooks: see the contract above */
    /* version >= 2 only (ota_loader_v3) - never read it when version is 1: */
    volatile unsigned char *inflight20;  /* task-20 hooks: the same contract with this flag (one writer per flag) */
    /* version >= 3 only (ota_loader_v4) - never read it below 3: the frozen stage-0 service (include/s0.h); the
     * ONLY way a main may write NOR, and stage-0 scopes it by caller: called from main code it erases / programs the
     * kvstore (unused_3 0x604C0000-0x604FFFFF) and any NOR gate stage-1 enabled main_writable this boot (none in
     * stage-1 API 1) ONLY - every slot sector (loaders and mains) and everything else -> 3.  nor_enable (svc version
     * >= 2) refuses main callers (3).  Below 3 a main does not write NOR at all. */
    const struct s0_svc *nor;
};

struct ota_entry_x {
    struct ota_entry e;                  /* e.feat has OTA_EF_ATTACH */
    void (*attach)(const struct ota_svc *svc);
};

/* ---- 0x7D ops (REMOTE_PROTOCOL section 10.3) -------------------------------------------- */
#define OP_OTA_INFO     0x0E             /* ota_loader_v4 stage-1: slot info (no auth) */
#define OP_OTA_CONFIRM  0x0F             /* ota_loader_v4 stage-1: confirm the running loader (auth) */
#define OP_OTA_STATUS   0x10
#define OP_OTA_BEGIN    0x11
#define OP_OTA_DATA     0x12
#define OP_OTA_COMMIT   0x13
#define OP_OTA_REJECT   0x14
#define OP_OTA_APPLY    0x15             /* ota_loader_v2: switch the running cave now (no power cycle) */
#define OP_OTA_HOOKS    0x16             /* ota_loader_v2: runtime hook table (read only, no auth) */
#define OTA_TGT_A       0u
#define OTA_TGT_B       1u
#define OTA_TGT_BUILTIN 2u
#define OTA_TGT_NEWEST  3u               /* newest sealed, not rejected / exhausted / bad slot, else built-in */
#define OTA_TGT_RESELECT 4u              /* ota_loader_v4: drop the main, then stage-0 selects a loader again */
#define OTA_TGT_LDR_A   0x10u            /* ota_loader_v4 Begin / Commit / info / confirm: loader slot A / B */
#define OTA_TGT_LDR_B   0x11u
#define OTA_REJ_LDR_A   0x10u            /* reject mask bits of the loader slots */
#define OTA_REJ_LDR_B   0x20u
#define OTA_STATUS_LEN3 112u             /* status raw bytes, layout 3 (ota_loader_v4 stage-1) */
#define OTA_INFO_LEN    56u              /* op 0x0E raw bytes */
#define OTA_QUIESCE_TICKS 250u           /* ~2 s of a cave not going idle -> apply refused        */
#define OTA_STATUS_LEN  72u              /* status raw bytes (layout 2); layout 1 = the first 48 */
#define OTA_HOOKS_LEN   57u              /* op 0x16 raw bytes, layout 1 (ota_loader_v2): 12 + 9 per allow-listed site (5) */
#define OTA_HOOKS_PAGE  7u               /* op 0x16 layout 2 (ota_loader_v3): sites per reply page          */
#define OTA_HOOKS_LEN2  (14u + 9u * OTA_HOOKS_PAGE)   /* 77 raw bytes: 14 + 9 per site of the page    */
#define OTA_CHUNK       200u             /* raw bytes per data op: 7 + 3 + pack7(200)=229 + F7 = 240 B frame */
#define OTA_TAG_LEN     8u

/* ---- fast upload (ota_loader_v4 stage-1 from image version 2; REMOTE_PROTOCOL 10.3 "Data v2") -----------------
 * Begin's reply grows from 5 to 9 raw bytes: rc · challenge u32 · caps u8 · window u8 · max u16 (LE).  An older
 * loader answers 5 bytes: the app then sends the 200 B stop-and-wait frames above, which every loader keeps taking.
 * A v2 data frame sets OTA_D2_FLAG in the first offset byte (an older loader reads that as an offset >= 1 MB -> rc 7):
 *   12 <0x40 | off[19:14]> <off[13:7]> <off[6:0]> <pack7: crc32 u32 · flags u8 · data 1..OTA_D2_MAX B>
 * crc32 = zlib CRC-32 over flags || data.  off counts FILE bytes (after decompression).  The loader stores a frame
 * only when off == received and the CRC matches (raw: data as is; OTA_D2_LZ: LZ tokens decoded into the staging
 * area, back references only into bytes already received) and answers every frame
 *   52 <pack7: rc u8 · received u32 · off u32>   rc 0 stored or already held, 7 gap (off > received),
 *                                                10 CRC, 1 malformed, 5 past the end, 8 not receiving
 * so the sender can keep up to `window` frames in flight and go back to `received` on 7 / 10 / a timeout.          */
#define OTA_D2_FLAG     0x40u            /* first offset byte of a v2 data frame                          */
#define OTA_D2_LZ       0x01u            /* frame flags: data = LZ tokens (see s1.c lz_decode)           */
#define OTA_D2_MAX      1536u            /* bytes after the flags byte: 11 + pack7(5 + 1536) = 1773 <= 0x800 */
#define OTA_D2_WINDOW   4u               /* frames in flight the loader advertises                        */
#define OTA_BC_DATA2    0x01u            /* Begin reply caps: v2 data frames                              */
#define OTA_BC_LZ       0x02u            /* Begin reply caps: OTA_D2_LZ frames                            */

#endif
