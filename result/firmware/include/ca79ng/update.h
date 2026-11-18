/* OpenGrand — update container and A/B boot selection.
 *
 * The original CAx9_P06.SYS is a raw, header-less MainProgram partition dump
 * The replacement container makes every
 * update self-describing and verifiable before anything is written:
 *
 *   offset  size  field
 *   0       8     magic "C79NGUP1"
 *   8       2     header version (1)
 *   10      2     section count (1..8)
 *   12      1     model group  (0x04)    \  must match the instrument
 *   13      1     model machine (0x1E)    } identity (CA79 = 04 1E sub 00 02)
 *   14      2     model sub (0x0002, BE) /
 *   16      4     firmware version (BE, major<<16 | minor<<8 | patch)
 *   20      4     build unix time (BE)
 *   24      4     total container length (BE)
 *   28      4     flags (bit0: signed)
 *   32      64    signature slot (Ed25519 over SHA-256 of bytes [0..32) + table)
 *   96      56*n  section table, per entry: type | flags | offset | length |
 *                 load | rsvd (6 x u32 BE) + sha256 of the payload (32 B)
 *   96+56n  4     CRC32 of bytes [0..96+56n) with the signature slot zeroed
 *   ...           payloads, 16-byte aligned
 */
#ifndef CA79NG_UPDATE_H
#define CA79NG_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPD_MAGIC "C79NGUP1"
#define UPD_HDR_FIXED 96u
#define UPD_ENTRY_SIZE 56u
#define UPD_MAX_SECTIONS 8

typedef enum {
    UPD_SEC_BOOT = 1,     /* boot loader (written last, only with flag FORCE) */
    UPD_SEC_MAIN = 2,     /* main ARM program */
    UPD_SEC_DSP = 3,      /* DSP program */
    UPD_SEC_VOICE = 4,    /* tone/voice database */
    UPD_SEC_RHYTHM = 5,   /* drum rhythm patterns */
    UPD_SEC_BLEMIDI = 6,  /* BLE-MIDI module firmware (forwarded over UART DFU) */
    UPD_SEC_PANEL = 7,    /* panel app package (forwarded to the panel) */
} upd_section_type_t;

typedef struct {
    uint32_t type, flags, offset, length, load;
    uint8_t sha256[32];
} upd_section_t;

typedef struct {
    uint16_t hdr_version;
    uint16_t count;
    uint8_t group, machine;
    uint16_t sub;
    uint32_t version;
    uint32_t build_time;
    uint32_t total_len;
    uint32_t flags;
    uint8_t signature[64];
    upd_section_t sec[UPD_MAX_SECTIONS];
} upd_info_t;

typedef enum {
    UPD_OK = 0,
    UPD_E_SHORT, UPD_E_MAGIC, UPD_E_VERSION, UPD_E_COUNT, UPD_E_HDRCRC,
    UPD_E_BOUNDS, UPD_E_MODEL, UPD_E_HASH, UPD_E_SIGNATURE, UPD_E_DOWNGRADE,
} upd_result_t;

/* Optional signature verification hook (Ed25519 on target). Return true if
 * `sig` is valid for `digest`. NULL = accept unsigned (development builds). */
typedef bool (*upd_verify_fn)(const uint8_t digest[32], const uint8_t sig[64]);

typedef struct {
    uint8_t group, machine;
    uint16_t sub;
    uint32_t current_version;
    bool allow_downgrade;
    upd_verify_fn verify;
} upd_policy_t;

upd_result_t upd_parse(const uint8_t *buf, size_t len, upd_info_t *out);
upd_result_t upd_verify(const uint8_t *buf, size_t len, const upd_policy_t *pol, upd_info_t *out);
const char *upd_strerror(upd_result_t r);

/* ---- A/B slot selection (kept in the kvstore under KV_KEY_BOOT_STATE) ---- */
typedef struct {
    uint8_t active;     /* slot that last booted successfully (0/1) */
    uint8_t pending;    /* slot holding a fresh update, 0xFF = none */
    uint8_t tries;      /* boot attempts of the pending slot */
    uint8_t rsvd;
    uint32_t version[2];
} boot_state_t;

#define BOOT_MAX_TRIES 3

void boot_state_init(boot_state_t *b);
/* Called by the boot loader: returns the slot to start and updates tries. */
uint8_t boot_select(boot_state_t *b);
/* Called by the main program once it is up (panel connected, audio running). */
void boot_confirm(boot_state_t *b, uint8_t running_slot);
/* Called after an update has been written and verified into `slot`. */
void boot_mark_pending(boot_state_t *b, uint8_t slot, uint32_t version);

#endif
