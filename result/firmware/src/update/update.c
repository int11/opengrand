#include "ca79ng/update.h"

#include <string.h>

#include "ca79ng/util.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

const char *upd_strerror(upd_result_t r)
{
    static const char *s[] = {"ok", "file too short", "bad magic", "unsupported header version",
                              "bad section count", "header crc mismatch", "section out of bounds",
                              "model mismatch", "section hash mismatch", "signature invalid",
                              "downgrade refused"};
    return (unsigned)r < CA_ARRAY_LEN(s) ? s[r] : "?";
}

/* CRC covers header + table with the signature slot treated as zeros, so the
 * signature can be added after the CRC is computed. */
static uint32_t header_crc(const uint8_t *buf, size_t table_end)
{
    static const uint8_t zeros[64];
    uint32_t c = ca_crc32(0, buf, 32);
    c = ca_crc32(c, zeros, 64);
    return ca_crc32(c, buf + UPD_HDR_FIXED, table_end - UPD_HDR_FIXED);
}

upd_result_t upd_parse(const uint8_t *buf, size_t len, upd_info_t *o)
{
    memset(o, 0, sizeof *o);
    if (len < UPD_HDR_FIXED + 4)
        return UPD_E_SHORT;
    if (memcmp(buf, UPD_MAGIC, 8) != 0)
        return UPD_E_MAGIC;
    o->hdr_version = be16(buf + 8);
    if (o->hdr_version != 1)
        return UPD_E_VERSION;
    o->count = be16(buf + 10);
    if (o->count == 0 || o->count > UPD_MAX_SECTIONS)
        return UPD_E_COUNT;
    size_t table_end = UPD_HDR_FIXED + (size_t)o->count * UPD_ENTRY_SIZE;
    if (len < table_end + 4)
        return UPD_E_SHORT;
    if (header_crc(buf, table_end) != be32(buf + table_end))
        return UPD_E_HDRCRC;
    o->group = buf[12];
    o->machine = buf[13];
    o->sub = be16(buf + 14);
    o->version = be32(buf + 16);
    o->build_time = be32(buf + 20);
    o->total_len = be32(buf + 24);
    o->flags = be32(buf + 28);
    memcpy(o->signature, buf + 32, 64);
    if (o->total_len != len)
        return UPD_E_BOUNDS;
    for (unsigned i = 0; i < o->count; i++) {
        const uint8_t *e = buf + UPD_HDR_FIXED + i * UPD_ENTRY_SIZE;
        upd_section_t *s = &o->sec[i];
        s->type = be32(e);
        s->flags = be32(e + 4);
        s->offset = be32(e + 8);
        s->length = be32(e + 12);
        s->load = be32(e + 16);
        memcpy(s->sha256, e + 24, 32);
        if (s->offset < table_end + 4 || s->offset > len || s->length > len - s->offset)
            return UPD_E_BOUNDS;
    }
    return UPD_OK;
}

upd_result_t upd_verify(const uint8_t *buf, size_t len, const upd_policy_t *pol, upd_info_t *o)
{
    upd_result_t r = upd_parse(buf, len, o);
    if (r != UPD_OK)
        return r;
    if (o->group != pol->group || o->machine != pol->machine || o->sub != pol->sub)
        return UPD_E_MODEL;
    if (!pol->allow_downgrade && o->version < pol->current_version)
        return UPD_E_DOWNGRADE;
    for (unsigned i = 0; i < o->count; i++) {
        uint8_t d[32];
        ca_sha256_t c;
        ca_sha256_init(&c);
        ca_sha256_update(&c, buf + o->sec[i].offset, o->sec[i].length);
        ca_sha256_final(&c, d);
        if (memcmp(d, o->sec[i].sha256, 32) != 0)
            return UPD_E_HASH;
    }
    if (pol->verify) {
        if (!(o->flags & 1u))
            return UPD_E_SIGNATURE;
        uint8_t d[32];
        ca_sha256_t c;
        ca_sha256_init(&c);
        ca_sha256_update(&c, buf, 32);
        ca_sha256_update(&c, buf + UPD_HDR_FIXED, (size_t)o->count * UPD_ENTRY_SIZE);
        ca_sha256_final(&c, d);
        if (!pol->verify(d, o->signature))
            return UPD_E_SIGNATURE;
    }
    return UPD_OK;
}

void boot_state_init(boot_state_t *b)
{
    memset(b, 0, sizeof *b);
    b->pending = 0xFF;
}

uint8_t boot_select(boot_state_t *b)
{
    if (b->pending <= 1) {
        if (b->tries < BOOT_MAX_TRIES) {
            b->tries++;
            return b->pending;
        }
        /* pending image never confirmed: roll back */
        b->pending = 0xFF;
        b->tries = 0;
    }
    return b->active;
}

void boot_confirm(boot_state_t *b, uint8_t slot)
{
    if (slot == b->pending) {
        b->active = slot;
        b->pending = 0xFF;
        b->tries = 0;
    }
}

void boot_mark_pending(boot_state_t *b, uint8_t slot, uint32_t version)
{
    b->pending = slot & 1u;
    b->tries = 0;
    b->version[slot & 1u] = version;
}
