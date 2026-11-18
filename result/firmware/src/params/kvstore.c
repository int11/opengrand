#include <string.h>

#include "ca79ng/storage.h"
#include "ca79ng/util.h"

/* Sector layout:
 *   header  : magic "KVS1" | generation u32 | crc32(magic..generation) u32
 *   records : key u16 | len u16 | flags u16 | rsvd u16 | value[len] (padded to 4) | crc32 u32
 *             len = 0xFFFF marks free space; flags 0 = tombstone, 1 = value. */
#define HDR_SIZE 12u
#define REC_HDR 8u
#define MAGIC 0x3153564Bu /* "KVS1" little endian */

static uint32_t pad4(uint32_t n) { return (n + 3u) & ~3u; }

static bool rd(kvstore_t *kv, int s, uint32_t off, void *buf, uint32_t len)
{
    return kv->flash->read(kv->flash->ctx, kv->sector[s], off, buf, len);
}

static bool read_header(kvstore_t *kv, int s, uint32_t *gen)
{
    uint32_t h[3];
    if (!rd(kv, s, 0, h, sizeof h) || h[0] != MAGIC)
        return false;
    if (ca_crc32(0, h, 8) != h[2])
        return false;
    *gen = h[1];
    return true;
}

static bool write_header(kvstore_t *kv, int s, uint32_t gen)
{
    uint32_t h[3] = {MAGIC, gen, 0};
    h[2] = ca_crc32(0, h, 8);
    return kv->flash->program(kv->flash->ctx, kv->sector[s], 0, h, sizeof h);
}

static int index_of(const kvstore_t *kv, uint16_t key)
{
    for (uint32_t i = 0; i < kv->nkeys; i++)
        if (kv->keys[i] == key)
            return (int)i;
    return -1;
}

static void index_set(kvstore_t *kv, uint16_t key, uint32_t off)
{
    int i = index_of(kv, key);
    if (off == 0) { /* remove */
        if (i >= 0) {
            kv->keys[i] = kv->keys[kv->nkeys - 1];
            kv->offs[i] = kv->offs[kv->nkeys - 1];
            kv->nkeys--;
        }
        return;
    }
    if (i < 0) {
        if (kv->nkeys >= KV_MAX_KEYS)
            return;
        i = (int)kv->nkeys++;
        kv->keys[i] = key;
    }
    kv->offs[i] = off;
}

/* Scan records in the active sector, rebuilding the index. Stops at the first
 * free or corrupt record — anything after a torn write is discarded. */
static void scan(kvstore_t *kv)
{
    uint8_t buf[KV_MAX_VALUE + 4];
    uint32_t off = HDR_SIZE, end = kv->flash->sector_size;
    kv->nkeys = 0;
    while (off + REC_HDR + 4 <= end) {
        uint16_t h[4];
        if (!rd(kv, kv->active, off, h, sizeof h) || h[1] == 0xFFFF || h[1] > KV_MAX_VALUE)
            break;
        uint32_t vlen = pad4(h[1]);
        if (off + REC_HDR + vlen + 4 > end)
            break;
        uint32_t crc_stored;
        if (!rd(kv, kv->active, off + REC_HDR, buf, vlen) ||
            !rd(kv, kv->active, off + REC_HDR + vlen, &crc_stored, 4))
            break;
        uint32_t crc = ca_crc32(ca_crc32(0, h, sizeof h), buf, h[1]);
        if (crc != crc_stored)
            break;
        index_set(kv, h[0], h[2] ? off : 0);
        off += REC_HDR + vlen + 4;
    }
    kv->write_off = off;
}

bool kv_format(kvstore_t *kv)
{
    if (!kv->flash->erase(kv->flash->ctx, kv->sector[0]) || !kv->flash->erase(kv->flash->ctx, kv->sector[1]))
        return false;
    kv->active = 0;
    kv->generation = 1;
    if (!write_header(kv, 0, 1))
        return false;
    kv->nkeys = 0;
    kv->write_off = HDR_SIZE;
    return true;
}

bool kv_mount(kvstore_t *kv, const hal_flash_t *flash, uint32_t a, uint32_t b)
{
    memset(kv, 0, sizeof *kv);
    kv->flash = flash;
    kv->sector[0] = a;
    kv->sector[1] = b;
    uint32_t ga = 0, gb = 0;
    bool va = read_header(kv, 0, &ga), vb = read_header(kv, 1, &gb);
    if (!va && !vb)
        return kv_format(kv);
    kv->active = (va && (!vb || ga >= gb)) ? 0 : 1;
    kv->generation = kv->active == 0 ? ga : gb;
    scan(kv);
    return true;
}

static bool append(kvstore_t *kv, int s, uint32_t *off, uint16_t key, uint16_t flags, const void *val, uint16_t len)
{
    uint8_t rec[REC_HDR + KV_MAX_VALUE + 4];
    uint16_t h[4] = {key, len, flags, 0xFFFF};
    uint32_t vlen = pad4(len);
    memcpy(rec, h, REC_HDR);
    memset(rec + REC_HDR, 0xFF, vlen);
    if (len)
        memcpy(rec + REC_HDR, val, len);
    uint32_t crc = ca_crc32(ca_crc32(0, h, sizeof h), val, len);
    memcpy(rec + REC_HDR + vlen, &crc, 4);
    uint32_t total = REC_HDR + vlen + 4;
    if (*off + total > kv->flash->sector_size)
        return false;
    if (!kv->flash->program(kv->flash->ctx, kv->sector[s], *off, rec, total))
        return false;
    *off += total;
    return true;
}

static bool compact(kvstore_t *kv)
{
    int dst = 1 - kv->active;
    uint8_t buf[KV_MAX_VALUE];
    if (!kv->flash->erase(kv->flash->ctx, kv->sector[dst]))
        return false;
    uint32_t off = HDR_SIZE;
    uint16_t nk[KV_MAX_KEYS];
    uint32_t no[KV_MAX_KEYS];
    for (uint32_t i = 0; i < kv->nkeys; i++) {
        uint16_t h[4];
        rd(kv, kv->active, kv->offs[i], h, sizeof h);
        rd(kv, kv->active, kv->offs[i] + REC_HDR, buf, h[1]);
        nk[i] = h[0];
        no[i] = off;
        if (!append(kv, dst, &off, h[0], 1, buf, h[1]))
            return false;
    }
    /* header last: until it is written the old sector stays authoritative */
    if (!write_header(kv, dst, kv->generation + 1))
        return false;
    kv->active = dst;
    kv->generation++;
    kv->write_off = off;
    memcpy(kv->keys, nk, sizeof(uint16_t) * kv->nkeys);
    memcpy(kv->offs, no, sizeof(uint32_t) * kv->nkeys);
    return true;
}

static bool put_record(kvstore_t *kv, uint16_t key, uint16_t flags, const void *val, uint16_t len)
{
    if (len > KV_MAX_VALUE)
        return false;
    if (flags && index_of(kv, key) < 0 && kv->nkeys >= KV_MAX_KEYS)
        return false;
    uint32_t off = kv->write_off;
    if (!append(kv, kv->active, &off, key, flags, val, len)) {
        if (!compact(kv))
            return false;
        off = kv->write_off;
        if (!append(kv, kv->active, &off, key, flags, val, len))
            return false;
    }
    index_set(kv, key, flags ? kv->write_off : 0);
    kv->write_off = off;
    return true;
}

bool kv_put(kvstore_t *kv, uint16_t key, const void *value, uint16_t len)
{
    return put_record(kv, key, 1, value, len);
}

bool kv_delete(kvstore_t *kv, uint16_t key)
{
    if (index_of(kv, key) < 0)
        return true;
    return put_record(kv, key, 0, NULL, 0);
}

uint32_t kv_free(const kvstore_t *kv)
{
    return kv->flash->sector_size > kv->write_off ? kv->flash->sector_size - kv->write_off : 0;
}

uint32_t kv_record_size(uint16_t len)
{
    return REC_HDR + pad4(len) + 4u;
}

int kv_get(kvstore_t *kv, uint16_t key, void *value, uint16_t cap)
{
    int i = index_of(kv, key);
    if (i < 0)
        return -1;
    uint16_t h[4];
    if (!rd(kv, kv->active, kv->offs[i], h, sizeof h))
        return -1;
    uint16_t n = CA_MIN(h[1], cap);
    if (n && !rd(kv, kv->active, kv->offs[i] + REC_HDR, value, n))
        return -1;
    return h[1];
}
