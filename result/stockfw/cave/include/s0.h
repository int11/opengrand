/* OpenGrand OTA loader v4 - the FROZEN interface of stage-0 (src/s0.c, USB-flashed with ota_loader_v4.SYS).
 *
 * Stage-0 is the only OpenGrand code that a Bluetooth update can never replace.  Everything in this file is read or
 * called by stage-0, so it is frozen for as long as a piano runs that stage-0: change nothing, only append fields
 * covered by the `size` members (readers use only the fields their `size` covers).  Contract text:
 * result/protocol/REMOTE_PROTOCOL.md section 10.12.  Python mirror: tools/ota_pack.py, tools/images.py; app:
 * result/app/src/protocol/ota.ts.
 *
 *   stage-0 code + .data   0x40173A14 - 0x40175FDF  (loader copy #1 of the stock BootProgram)
 *   root key, 32 B         0x40175FE0 - 0x40175FFF  (patched in from secrets/ota_key.bin)
 *   stage-1 window         0x40176000 - 0x4017FFFF  (40 KB, loaded by stage-0 from a loader slot)
 *   0x40180000 -            main areas (core RW, staging, run, read-back): stage-1's business, not frozen
 */
#ifndef CA79NG_S0_H
#define CA79NG_S0_H

/* ---- NOR: the four slot sectors (CPU addresses; file offset = addr - 0x60060000) ---------------------- */
#define S0_SLOT_MAIN_A  0x60180000u      /* main slot A ('C79S')                 */
#define S0_SLOT_LDR_A   0x601A0000u      /* loader slot A ('C79L')               */
#define S0_SLOT_MAIN_B  0x601C0000u      /* main slot B                          */
#define S0_SLOT_LDR_B   0x601E0000u      /* loader slot B                        */
#define S0_SECTOR       0x20000u
#define S0_PAGE         0x200u           /* stock write-buffer page              */

/* ---- the guarded NOR write service: ONE frozen range table (s0.c s0_nor_ranges) ------------------------------
 * An erase / page program / halfword program is allowed only if the whole access lies inside ONE table entry that is
 * ON and open to the caller, aligned, and (erase / page program) not in the loader sector stage-0 runs from.  Nothing
 * outside the table is ever writable, for anybody: BootProgram, MainProgram outside the slot hole, UserMemory,
 * TouchAdjust, config A, RecorderData are simply not in it.  Entries (CPU addresses, [lo, hi)):
 *
 *   entry   range                    on                      writable by
 *   slots   0x60180000 - 0x601FFFFF  always                  stage-0 / stage-1 only   (the 4 slot sectors; USB-erased)
 *   kv      0x604C0000 - 0x604FFFFF  always                  + the main               (unused_3 = the main's kvstore)
 *   gate 0  0x60480000 - 0x604BFFFF  after nor_enable(0, m)  stage-0/1; the main if m (unused_2 + TouchCalib)
 *   gate 1  0x60600000 - 0x607FFFFF  after nor_enable(1, m)  stage-0/1; the main if m (beyond the 6 MB dump)
 *   gate 2  0x60800000 - 0x60FFFFFF  after nor_enable(2, m)  stage-0/1; the main if m (beyond 8 MB)
 *
 * Caller scope (by the return address of the service call): code in [S0_TRUST_LO, S0_TRUST_HI) = stage-0 and the
 * stage-1 window; every other caller (the main, 0x40180000+) only entries flagged S0RF_MAIN (the kvstore) and gates
 * that stage-1 enabled with main_writable.
 *
 * GATES are OFF at every power-on (s0_boot is .data, copied from NOR by the stock boot) and again after every RESELECT
 * (the newly selected stage-1 decides afresh).  Only stage-0 / stage-1 code can switch one ON (s0_svc.nor_enable), for
 * the rest of this boot; nothing can switch it OFF (the masks only grow - no revoke).  main_writable is decided by the
 * enabling stage-1 at enable time, not fixed in the table: stage-1 is the signed, updatable policy owner and knows
 * what the running main is for; stage-0 only keeps two monotonic bit masks (one more OR), so the frozen code needs no
 * per-gate policy and a later decision ("the main stores modules in gate 0") needs no USB.  A grant also lasts until
 * the next boot.  nor_enable refuses (S0_NOR_ALIAS) a gate whose first S0_ALIAS_LEN bytes read back identical to the
 * first S0_ALIAS_LEN bytes of BootProgram (0x60000000, the AIS header, never erased): that is what a missing high
 * address line looks like.
 *
 * RULES FOR STAGE-1 (stage-0 enforces only the alias refusal above):
 *   - gates 1 and 2: the NOR capacity is UNKNOWN (no CFI / ID read; the dump and the partition table end at 6 MB).
 *     Enable a gate only after a read-only probe on THIS piano proved that the range is real NOR, not a mirror: a
 *     read-back comparison of every 32 KB window of the range against the same window offset at 0x000000 / 0x200000 /
 *     0x400000 (gate 2: against 0x000000 - 0x7FFFFF), plus the erased / first-non-FF check.
 *   - GATE 2 COULD MIRROR BOOTPROGRAM: on an 8 MB part (no address line A23) 0x60800000 is 0x60000000, and an erase
 *     there is a hard brick (no JTAG).  Gate 2 must only ever be enabled by a stage-1 that has proven, by read-back
 *     comparison as above, that 0x60800000 - 0x60FFFFFF is not a mirror.  The stage-0 alias refusal is a last
 *     line, not that proof.
 *   - gates 0, 1, 2 survive official USB updates: whatever is stored there must be signed / versioned.  The factory
 *     SysEx tools fn 0x70 (erases 0x600000 - 0x67FFFF) and fn 0x71 (unused_2) can overwrite them, a Kawai test-mode
 *     calibration rewrites TouchCalib: treat gates 0 / 1 as a cache, not the truth, and block fn 0x70 / 0x71.
 *   - the voice-DB tail sector 0x602E0000 is deliberately NOT in the table: loader copy #3 brings it to RAM
 *     0x402E0000 at every boot and the voice-DB code reaches the DB through data-driven offset tables, so nothing
 *     proves that no stock code reads its content (REA / literal census, hardware baseline study).
 * Invariant (static assert below + roundtrip_v4_boot): no entry overlaps BootProgram [S0_NOR_BOOT_LO, S0_NOR_BOOT_HI)
 * - the table is const, so this holds after any enable; the guard also refuses every address below S0_NOR_BOOT_HI by
 * itself. */
#define S0_NOR_SLOTS_LO 0x60180000u
#define S0_NOR_SLOTS_HI 0x60200000u
#define S0_NOR_KV_LO    0x604C0000u
#define S0_NOR_KV_HI    0x60500000u
#define S0_NOR_G0_LO    0x60480000u      /* gate 0: unused_2 + TouchCalib  */
#define S0_NOR_G0_HI    0x604C0000u
#define S0_NOR_G1_LO    0x60600000u      /* gate 1: 6 MB - 8 MB            */
#define S0_NOR_G1_HI    0x60800000u
#define S0_NOR_G2_LO    0x60800000u      /* gate 2: 8 MB - 16 MB (mirror risk, see above) */
#define S0_NOR_G2_HI    0x61000000u
#define S0_NOR_BOOT_LO  0x60000000u      /* BootProgram: never in the table */
#define S0_NOR_BOOT_HI  0x60060000u
#define S0_NOR_RANGES   5u               /* entries of s0_nor_ranges        */
#define S0_GATES        3u               /* gate ids 0 .. 2                 */
#define S0_ALIAS_LEN    64u              /* bytes compared by the alias refusal */
#define S0RF_MAIN       0x01u            /* entry flag: the main may write it (always-on entries) */
struct s0_nor_range {
    unsigned int lo, hi;                 /* CPU addresses [lo, hi), sector-aligned */
    unsigned char gate;                  /* 0 = always on; else the gate's bit (1 << gate id) */
    unsigned char flags;                 /* S0RF_* */
    unsigned char rsv[2];
};
#define S0_TRUST_LO     0x40173A14u      /* = S0_BASE                      */
#define S0_TRUST_HI     0x40180000u      /* = S0_WINDOW + S0_WINDOW_SIZE   */

/* slot sector layout, both kinds.  Offsets 0x00-0x2F are the frozen "stage-0 view"; new fields only at 0x30+. */
#define S0_ST_MAGIC     0x00u            /* u32 'C79L' (loader) / 'C79S' (main), programmed LAST = commit point */
#define S0_ST_SEQ       0x04u            /* u32 sequence number                        */
#define S0_ST_CONFIRM   0x08u            /* u16 0x0000 = confirmed                     */
#define S0_ST_REJECT    0x0Au            /* u16 0x0000 = never boot again              */
#define S0_ST_LIVE      0x0Cu            /* u16 (mains) live-run mark                  */
#define S0_ST_VERSION   0x0Eu            /* u16 state version (1 = written by v4 stage-1; FFFF = older writer)   */
#define S0_ST_ATTEMPT   0x10u            /* u16[16] one 0xFFFF -> 0x0000 per unconfirmed boot attempt           */
#define S0_ATTEMPTS     16u
#define S0_ST_VIEW      0x30u            /* bytes stage-0 reads                        */
#define S0_IMAGE_OFF    0x200u           /* signed image (64 B header + payload)       */
#define S0_LDR_MAGIC    0x4C393743u      /* bytes "C79L" */
#define S0_MAIN_MAGIC   0x53393743u      /* bytes "C79S" */
#define S0_MAX_TRIES    3u               /* unconfirmed loader boots before stage-0 gives a loader slot up       */

/* ---- signed image header (64 B, fixed size forever) ------------------------------------------------- */
#define S0_H_MAGIC      0x00u            /* u32 'C79P'                                                 */
#define S0_H_FORMAT     0x04u            /* u16 format: 1 = v3-era main (0x28..0x2F zero), 2 = v4      */
#define S0_H_HDRSIZE    0x06u            /* u16 64                                                     */
#define S0_H_LEN        0x08u            /* u32 payload length                                         */
#define S0_H_LOAD       0x0Cu            /* u32 load address (loaders: S0_WINDOW)                     */
#define S0_H_ENTRY      0x10u            /* u32 entry table offset from load (4-aligned)              */
#define S0_H_CRC        0x14u            /* u32 CRC-32 (zlib) of the payload                          */
#define S0_H_STOCK      0x18u            /* u32 first 4 bytes of the stock sha256 (LE) = 0x7085AD09    */
#define S0_H_ABI        0x1Cu            /* u32 loaders: S0_ABI; mains: main ABI                       */
#define S0_H_FEAT       0x20u            /* u32 information                                            */
#define S0_H_KEYID      0x24u            /* u32 key id of the signing key                              */
#define S0_H_TYPE       0x28u            /* u8  0 = main (format 1), 1 = main, 2 = loader              */
#define S0_H_ALG        0x29u            /* u8  signature algorithm (S0_ALG_*); format 1: 0 = HMAC     */
#define S0_H_MINAPI     0x2Au            /* u8  mains: lowest stage-1 API they need (0 = any)          */
#define S0_H_RSV        0x2Bu            /* u8  0                                                      */
#define S0_H_VERSION    0x2Cu            /* u32 image version (build counter of that image)            */
#define S0_H_MAC        0x30u            /* 16 B: alg 1 = HMAC-SHA256(key, hdr[0:0x30] || payload)[:16] */
#define S0_HDR_SIZE     64u
#define S0_HDR_MAGIC    0x50393743u      /* bytes "C79P" */
#define S0_STOCK_TAG    0x7085AD09u
#define S0_TYPE_MAIN0   0u
#define S0_TYPE_MAIN    1u
#define S0_TYPE_LOADER  2u
/* Signature algorithms.  Stage-0 v1 implements S0_ALG_HMAC (root key = 32 B secret).  S0_ALG_ED25519 is reserved:
 * the 32 B key slot would hold the public key, the 64 B signature follows the payload (image = header + payload +
 * 64 B) and hdr 0x30..0x3F is zero.  An image with an algorithm stage-0 does not implement is never booted.        */
#define S0_ALG_HMAC     1u
#define S0_ALG_ED25519  2u               /* reserved, not implemented */
/* key id = first 4 bytes (LE) of HMAC-SHA256(key, "C79 key id") */

/* ---- RAM ------------------------------------------------------------------------------------------- */
#define S0_BASE         0x40173A14u
#define S0_KEY_VA       0x40175FE0u
#define S0_WINDOW       0x40176000u
#define S0_WINDOW_SIZE  0xA000u          /* 40 KB: maximum loader payload length */
#define S0_ABI          1u

/* ---- stage-1 entry table (at S0_WINDOW + entry offset) ------------------------------------------------- */
#define S1_ENTRY_MAGIC  0x4C393743u      /* bytes "C79L" */
struct s0_boot;
struct s1_entry {
    unsigned int magic;                  /* S1_ENTRY_MAGIC */
    unsigned short abi;                  /* S0_ABI */
    unsigned short size;                 /* bytes of this table (>= 24) */
    /* once, from the engine loop, after the image was verified and the caches synced.  0 = running; anything else
     * = refused (stage-0 tries the next candidate; nothing of this stage-1 is called again). */
    int (*init)(const struct s0_boot *boot);
    /* every 0x7D SysEx (MIDI task 19): r7 port, r9 buf, r0 len; frame = the saved r0-r9, ip, lr of the hook site
     * (12 words, restored after the call - a stage-1 may change them) */
    void (*sysex)(unsigned int port, const unsigned char *buf, unsigned int len, unsigned int *frame);
    void (*tick)(void);                  /* every engine-loop pass (task 24) after init */
    /* asked (engine loop) while boot->request == S0_REQ_RESELECT: 0 = quiesced, every runtime hook restored, no
     * stage-1 / main code will be entered again - stage-0 drops this stage-1 and selects a loader again;
     * non-zero = not yet (tick() keeps running, asked again next pass) */
    int (*release)(void);
};

/* ---- the boot block (stage-0 .data; init() gets its address and may keep it) ----------------------- */
#define S0_BOOT_MAGIC   0x42393743u      /* bytes "C79B" */
#define S0_BOOT_VERSION 1u
#define S0_VERSION      2u               /* stage-0 code version (2 = the gated NOR range table, nor_enable) */
/* reason */
enum { S0R_SELECTING = 0, S0R_SLOT = 1, S0R_NONE = 2, S0R_ESCAPE = 3, S0R_FAILED = 4, S0R_BUSY = 5, S0R_SAFE = 6,
       S0R_RESELECT = 7 };
/* flags */
#define S0F_SAFE        0x01u            /* escape E2 (A0+A#0): the older loader runs; stage-1 must not load a main */
#define S0F_NOMARK      0x02u            /* flash busy at boot: only confirmed loaders were considered             */
#define S0F_RESELECTED  0x04u            /* this stage-1 came in by a RESELECT (hot swap)                          */
/* request (written by stage-1) */
#define S0_REQ_NONE     0u
#define S0_REQ_RESELECT 1u
/* per loader slot flags */
#define S0SF_VALID      0x01u
#define S0SF_CONFIRMED  0x02u
#define S0SF_REJECTED   0x04u
#define S0SF_BOOTED     0x08u            /* the running one */
#define S0SF_BAD        0x10u            /* failed this boot (mark, header, CRC, signature, entry, init) */
#define S0SF_EXHAUSTED  0x20u
#define S0SF_SCANNED    0x40u
/* escape diagnostics (diag_esc) */
#define S0E_MBOX        0x01u            /* E1 code 0x40000007 in the BootProgram mailbox */
#define S0E_COPY        0x02u            /* E1 code in MainProgram's copy *0x40000000      */
#define S0E_KEYS        0x04u            /* exactly A0 A#0 B0 down in the stock key-scan state */
#define S0E_MBOX2       0x08u            /* E2 code 0x40000003 in the mailbox               */
#define S0E_COPY2       0x10u            /* E2 code in the copy                             */
#define S0E_KEYS2       0x20u            /* exactly A0 A#0 down                             */
#define S0E_SCAN_ON     0x40u            /* the stock key scan was running when sampled     */

struct s0_slot {
    unsigned int seq;
    unsigned char flags;                 /* S0SF_* */
    unsigned char tries;                 /* attempt halfwords used (after this boot's mark) */
    unsigned char rsv[2];
};

struct s0_svc;
struct s0_boot {
    unsigned int magic;                  /* S0_BOOT_MAGIC */
    unsigned short version;              /* S0_BOOT_VERSION */
    unsigned short size;                 /* sizeof(struct s0_boot) */
    unsigned char s0_version;            /* S0_VERSION */
    unsigned char slot;                  /* running loader slot 0 = A, 1 = B, 0xFF = none */
    unsigned char reason;                /* S0R_* */
    unsigned char flags;                 /* S0F_* */
    volatile unsigned char request;      /* stage-1 writes S0_REQ_*; stage-0 clears it */
    unsigned char alg;                   /* S0_ALG_* of the root key */
    unsigned char max_tries;             /* S0_MAX_TRIES */
    unsigned char reselects;             /* RESELECTs done since power-on */
    const unsigned char *key;            /* root key, 32 B (S0_KEY_VA) */
    unsigned int key_id;                 /* its key id */
    unsigned int window;                 /* S0_WINDOW */
    unsigned int window_size;            /* S0_WINDOW_SIZE */
    const struct s0_svc *svc;            /* the service table */
    struct s0_slot lslot[2];             /* loader slots A / B as stage-0 saw them this boot */
    unsigned int diag_mbox0, diag_mbox1, diag_copy;   /* latched at the first engine-loop pass */
    unsigned char diag_esc, diag_keys_down, diag_keys_low, diag_esc_wait;
    volatile unsigned short guard_trips; /* NOR service refusals since power-on */
    volatile unsigned short busy_defers; /* boot steps deferred: a stock writer held the chip */
    /* ---- size >= 76 (stage-0 v2) ---- */
    volatile unsigned char nor_gates;    /* NOR gates ON (bit = 1 << gate id); 0 at power-on and after RESELECT  */
    volatile unsigned char nor_gates_main;   /* of those, the gates the main may write too (granted at enable) */
    unsigned char rsv2[2];
};
#define S0_BOOT_SIZE1   72u              /* a stage-0 v1 boot block (no nor_gates): readers accept size >= this */

/* ---- the service table (frozen code in stage-0, for stage-1 and the main) ----------------------------- */
#define S0_SVC_MAGIC    0x4E393743u      /* bytes "C79N" */
#define S0_SVC_VERSION  2u               /* 2 = nor_enable appended (size 32) */
#define S0_SVC_SIZE1    28u              /* a version 1 table (no nor_enable): readers accept size >= this */
/* NOR service results */
#define S0_NOR_OK       0
#define S0_NOR_BUSY     1                /* a stock writer owns the chip: nothing done, try again later */
#define S0_NOR_FAIL     2                /* the stock routine reported a failure */
#define S0_NOR_GUARD    3                /* refused: outside the caller's ranges, misaligned, or the running loader sector;
                                          * nor_enable: unknown gate id or not called from stage-0 / stage-1 code */
#define S0_NOR_ALIAS    4                /* nor_enable: the gate reads back like BootProgram (a mirror) - it stays OFF */
/* sig_check results */
#define S0_SIG_OK       0
#define S0_SIG_HEADER   1                /* algorithm unknown / key id != root key */
#define S0_SIG_MAC      2                /* signature mismatch */
struct s0_svc {
    unsigned int magic;                  /* S0_SVC_MAGIC */
    unsigned short version;              /* S0_SVC_VERSION */
    unsigned short size;                 /* sizeof(struct s0_svc) */
    /* One stock NOR writer call as the owner of the chip (dispatch off, stock idle check, stock busy flag raised
     * around it), after the guard.  Any task; never blocks except for the stock routine's own wait.  The guard
     * scopes by caller (return address) over the range table above: stage-0 / stage-1 code = slot sectors + kvstore
     * + the gates that are ON, anyone else (the main) = kvstore + the gates enabled main_writable.  A stage-1 calls
     * these directly (a call or a tail call from stage-1 code), never through a
     * helper outside the window. */
    int (*nor_erase)(unsigned int addr);                           /* 128 KB sector, sector-aligned         */
    int (*nor_prog)(unsigned int addr, const void *src, unsigned int n);   /* 2..512 B, even, inside one 512 B page */
    int (*nor_prog_hw)(unsigned int addr, unsigned int hw);       /* one halfword                          */
    /* D-cache clean + drain + I-cache invalidate of [addr, addr + len) (privileged, through the stock SWI #0) */
    void (*cache_sync)(unsigned int addr, unsigned int len);
    /* THE signature check: hdr = the 64 B header, over hdr[0:0x30] || payload[0:len] with the root key and the
     * algorithm named in hdr (format 1 = HMAC).  The only code that knows the algorithm. */
    int (*sig_check)(const unsigned char *hdr, const unsigned char *payload, unsigned int len);
    /* ---- version >= 2, size >= 32 ---- */
    /* Switch NOR gate `gate` (0 .. S0_GATES-1) ON for the rest of this boot; main_writable != 0 also opens it to the
     * main.  Only from stage-0 / stage-1 code (return address, as above; ARM entry, call it directly).  Never
     * switches anything off.  Before a gate's first enable it compares S0_ALIAS_LEN bytes at the gate's start with
     * BootProgram's first bytes (identical -> S0_NOR_ALIAS, the gate stays OFF).  Result: S0_NOR_OK (also when it
     * was ON already), S0_NOR_BUSY (a stock writer owns the chip, nothing changed - try again), S0_NOR_GUARD
     * (unknown id or caller, counted in guard_trips), S0_NOR_ALIAS.  The state: s0_boot.nor_gates / nor_gates_main. */
    int (*nor_enable)(unsigned int gate, unsigned int main_writable);
};

#endif
