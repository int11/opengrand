/* OpenGrand — persistent storage.
 *
 * kvstore: a log-structured key/value store over two flash sectors
 * ("UserMemory" partition in the original layout). Every
 * record carries a CRC32; a power loss in the middle of a write leaves an
 * invalid tail that is ignored on mount. When the active sector fills up the
 * latest value of every key is compacted into the other sector, which is then
 * activated with a higher generation number.
 *
 * Flash access goes through hal_flash_t so the same code runs on the target
 * and against a RAM-backed fake (with fault injection) in tests. */
#ifndef CA79NG_STORAGE_H
#define CA79NG_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *ctx;
    uint32_t sector_size;
    /* erase sets bytes to 0xFF; program can only clear bits (NOR semantics) */
    bool (*erase)(void *ctx, uint32_t sector);
    bool (*program)(void *ctx, uint32_t sector, uint32_t offset, const void *data, uint32_t len);
    bool (*read)(void *ctx, uint32_t sector, uint32_t offset, void *data, uint32_t len);
} hal_flash_t;

#define KV_MAX_VALUE 512u
#define KV_MAX_KEYS 64u

typedef struct {
    const hal_flash_t *flash;
    uint32_t sector[2];   /* the two physical sectors used */
    int active;           /* 0/1 index into sector[] */
    uint32_t generation;
    uint32_t write_off;   /* next free offset in the active sector */
    /* index: key -> offset of its latest record (0 = absent) */
    uint16_t keys[KV_MAX_KEYS];
    uint32_t offs[KV_MAX_KEYS];
    uint32_t nkeys;
} kvstore_t;

bool kv_mount(kvstore_t *kv, const hal_flash_t *flash, uint32_t sector_a, uint32_t sector_b);
bool kv_format(kvstore_t *kv);
bool kv_put(kvstore_t *kv, uint16_t key, const void *value, uint16_t len);
/* returns stored length, or -1 if missing; copies at most cap bytes */
int kv_get(kvstore_t *kv, uint16_t key, void *value, uint16_t cap);
bool kv_delete(kvstore_t *kv, uint16_t key);
/* bytes left in the active sector before the next put compacts (= erases the other sector) */
uint32_t kv_free(const kvstore_t *kv);
/* bytes one record of a len-byte value takes in a sector */
uint32_t kv_record_size(uint16_t len);

/* Well-known keys */
enum {
    KV_KEY_STARTUP_SETTINGS = 0x0001, /* "Startup Settings" snapshot of params */
    KV_KEY_USER_SOUNDS = 0x0002,
    KV_KEY_USER_TOUCHCURVE = 0x0003,
    KV_KEY_PERNOTE_VOICING = 0x0010,
    KV_KEY_PERNOTE_VOLUME = 0x0011,
    KV_KEY_PERNOTE_TUNING = 0x0012,
    KV_KEY_PERNOTE_TEMPERAMENT = 0x0013,
    KV_KEY_BLE_NAME = 0x0020,
    KV_KEY_PEDAL_CAL = 0x0030,
    KV_KEY_POWER_POLICY = 0x0040, /* power_policy_t wire form (ca79ng/power.h), 0x7D op 0x18 */
    KV_KEY_BT_POLICY = 0x0041,    /* the BT source policy's kept part (ca79ng/btsrc.h bts_saved_*), 0x7D op 1D 06 */
    KV_KEY_BOOT_STATE = 0x00F0,
    /* core startup settings (kpstart.h, cave port startup_cave.c): mode, then per snapshot (C = "current",
     * P = "power off") a header, two banks of KSU_CHUNKS chunks and per bank the NG part (base + 9 + bank) */
    KV_KEY_STARTUP_MODE = 0x0200,
    KV_KEY_STARTUP_ALL = 0x0201, /* [1, mode]: the mode was chosen by a build whose modes cover every setting */
    KV_KEY_STARTUP_C = 0x0210,
    KV_KEY_STARTUP_P = 0x0220,
};

#endif
