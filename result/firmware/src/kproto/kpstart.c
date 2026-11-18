/* OpenGrand - startup settings owned by the core.  See kpstart.h. */
#include "ca79ng/kpstart.h"

#include <string.h>

#include "ca79ng/util.h"

/* ---- keys ------------------------------------------------------------------------------------ */
bool ksu_excluded(uint8_t v1, uint8_t v2)
{
    switch (v1) {
    case 0x50:   /* pedal AD value (sensor), sound preview start / stop / volume, rendering status */
        return v2 == 0x1C || v2 == 0x1D || v2 == 0x1E || v2 == 0x1F || v2 == 0x2B;
    case 0x51:   /* headphones / USB memory: notifications; BluetoothMIDI: never switched off by a restore.
                  * AutoPowerOff 51 00 / LockMode 51 22 are startup settings too (user decision "A") */
        return v2 == 0x29 || v2 == 0x2E || v2 == 0x3D;
    case 0x52:   /* recorder / song run state - but the audio settings the stock boot applies (FUN_4015D9CC: audio
                  * play volume +2, rec gain +0x14) and the rec format are settings */
        return !(v2 == 0x0C || v2 == 0x0E || v2 == 0x1E);
    case 0x56:   /* metronome running (the stock never starts with it on), BeatCounter (notification) */
        return v2 == 0x08 || v2 == 0x0B;
    case 0x57:   /* unnamed keys of the file / recorder family: not known to be settings */
    case 0x5A:   /* VT edit */
        return true;
    default:
        return false;
    }
}

/* ---- snapshot -------------------------------------------------------------------------------- */
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

static bool rec_put(uint8_t *out, uint32_t cap, uint32_t *o, const uint8_t key[3], const uint8_t *d, uint8_t len)
{
    if (*o + 4u + len > cap)
        return false;
    memcpy(out + *o, key, 3);
    out[*o + 3] = len;
    memcpy(out + *o + 4, d, len);
    *o += 4u + len;
    return true;
}

int ksu_snap_build(const param_store_t *store, const kst_t *st, uint8_t *out, uint32_t cap)
{
    uint32_t o = KSU_SNAP_HEAD, count = 0;
    if (cap < KSU_SNAP_HEAD)
        return -1;
    for (int id = 0; id < PRM_COUNT; id++) {
        const param_desc_t *d = &PARAM_DESC[id];
        if (d->fn != 0x10 || d->var_len || d->len > 4 || ksu_excluded(d->v1, d->v2))
            continue;
        for (int slot = 0; slot < PARAM_SLOT_COUNT; slot++) {
            uint8_t v[4], key[3];
            if (!kst_seen(st, id, slot) || !param_get(store, (param_id_t)id, slot, v))
                continue;
            key[0] = d->v1;
            key[1] = d->v2;
            key[2] = param_part_from_slot(slot);
            if (!rec_put(out, cap, &o, key, v, d->len))
                return -1;
            count++;
        }
    }
    for (unsigned k = 0; k < KST_IDX_MAX; k++) {
        const kst_idx_t *x = &st->idx[k];
        if (!x->len || ksu_excluded(x->key[0], x->key[1]))
            continue;
        if (!rec_put(out, cap, &o, x->key, x->data, x->len))
            return -1;
        count++;
    }
    out[0] = 'K';
    out[1] = 'S';
    out[2] = 1;
    out[3] = 0;
    put16(out + 4, count);
    put16(out + 6, 0);
    return (int)o;
}

int ksu_rec(const uint8_t *snap, uint32_t n, uint32_t pos, const uint8_t **rec)
{
    if (pos == n)
        return 0;
    if (pos + 4u > n || snap[pos + 3] < 1 || snap[pos + 3] > 4 || pos + 4u + snap[pos + 3] > n)
        return -1;
    *rec = snap + pos;
    return 4 + snap[pos + 3];
}

bool ksu_snap_check(const uint8_t *snap, uint32_t n, uint16_t *count)
{
    uint32_t pos = KSU_SNAP_HEAD, c = 0;
    const uint8_t *r;
    int l;
    if (n < KSU_SNAP_HEAD || snap[0] != 'K' || snap[1] != 'S' || snap[2] != 1)
        return false;
    while ((l = ksu_rec(snap, n, pos, &r)) > 0) {
        pos += (uint32_t)l;
        c++;
    }
    if (l < 0 || c != get16(snap + 4))
        return false;
    if (count)
        *count = (uint16_t)c;
    return true;
}

int ksu_rec_differs(const uint8_t *rec, const param_store_t *store, const kst_t *st)
{
    uint8_t len = rec[3], v[4];
    int id, slot;
    if (kst_is_indexed(rec[0], rec[1], rec[2])) {
        int l = kst_indexed_get(st, rec[0], rec[1], rec[2], rec[4], v);
        if (l <= 0)
            return -1;
        return l != len || memcmp(v, rec + 4, len) != 0;
    }
    id = param_find(0x10, rec[0], rec[1], rec[2]);
    slot = param_slot_from_part(rec[2]);
    if (id < 0 || slot < 0 || !param_get(store, (param_id_t)id, slot, v))
        return -1;
    return memcmp(v, rec + 4, len) != 0;
}

static uint32_t frame(const uint8_t *rec, uint8_t b4, uint8_t b5, uint8_t out[KSU_FRAME_MAX])
{
    uint8_t len = rec[3];
    out[0] = 0xF0;
    out[1] = 0x40;
    out[2] = 0x7F;
    out[3] = 0x10;
    out[4] = b4;
    out[5] = b5;
    memcpy(out + 6, rec, 3);
    memcpy(out + 9, rec + 4, len);
    out[9 + len] = 0xF7;
    return 10u + len;
}

uint32_t ksu_set_frame(const uint8_t *rec, uint8_t out[KSU_FRAME_MAX]) { return frame(rec, 0x08, 0x02, out); }
uint32_t ksu_value_frame(const uint8_t *rec, uint8_t out[KSU_FRAME_MAX]) { return frame(rec, 0x04, 0x1E, out); }

/* ---- debouncer ------------------------------------------------------------------------------- */
void ksu_deb_init(ksu_deb_t *d, uint32_t quiet_ms, uint32_t gap_ms)
{
    memset(d, 0, sizeof *d);
    d->quiet_ms = quiet_ms;
    d->gap_ms = gap_ms;
}

void ksu_deb_change(ksu_deb_t *d, uint32_t now)
{
    d->dirty = true;
    d->last_change = now;
}

bool ksu_deb_due(const ksu_deb_t *d, uint32_t now)
{
    return d->dirty && now - d->last_change >= d->quiet_ms && (!d->saved || now - d->last_save >= d->gap_ms);
}

void ksu_deb_saved(ksu_deb_t *d, uint32_t now)
{
    d->dirty = false;
    d->saved = true;
    d->last_save = now;
}

/* ---- kvstore --------------------------------------------------------------------------------- */
static uint16_t base(ksu_slot_t slot) { return slot == KSU_C ? KV_KEY_STARTUP_C : KV_KEY_STARTUP_P; }
static uint16_t chunk_key(ksu_slot_t slot, uint32_t bank, uint32_t k)
{
    return (uint16_t)(base(slot) + 1u + bank * KSU_CHUNKS + k);
}

/* header: 'K' 'H' 1 bank | nchunks 0 len u16 | gen u32 | crc32 of the snapshot u32 */
static bool read_hdr(kvstore_t *kv, ksu_slot_t slot, uint8_t h[KSU_KV_HDR])
{
    return kv_get(kv, base(slot), h, KSU_KV_HDR) == (int)KSU_KV_HDR && h[0] == 'K' && h[1] == 'H' && h[2] == 1 &&
           h[3] <= 1 && h[4] >= 1 && h[4] <= KSU_CHUNKS && get16(h + 6) <= KSU_SNAP_MAX;
}

ksu_mode_t ksu_kv_mode(kvstore_t *kv)
{
    uint8_t m[2];
    if (kv_get(kv, KV_KEY_STARTUP_MODE, m, sizeof m) != 2 || m[0] != 1 || m[1] >= KSU_MODES)
        return KSU_STOCK;
    return (ksu_mode_t)m[1];
}

bool ksu_kv_set_mode(kvstore_t *kv, ksu_mode_t mode)
{
    const uint8_t m[2] = {1, (uint8_t)mode};   /* the record builds before read: unchanged */
    if (mode >= KSU_MODES)
        return false;
    return kv_put(kv, KV_KEY_STARTUP_MODE, m, sizeof m) && kv_put(kv, KV_KEY_STARTUP_ALL, m, sizeof m);
}

bool ksu_kv_mode_all(kvstore_t *kv)
{
    uint8_t m[2], a[2];
    return kv_get(kv, KV_KEY_STARTUP_MODE, m, sizeof m) == 2 && m[0] == 1 && m[1] < KSU_MODES &&
           kv_get(kv, KV_KEY_STARTUP_ALL, a, sizeof a) == 2 && a[0] == 1 && a[1] == m[1];
}

static uint16_t ng_key(ksu_slot_t slot, uint32_t bank) { return (uint16_t)(base(slot) + 1u + 2u * KSU_CHUNKS + bank); }

uint32_t ksu_kv_need(uint32_t n, uint32_t ngn)
{
    uint32_t t = kv_record_size(KSU_KV_HDR) + (ngn ? kv_record_size((uint16_t)(12u + ngn)) : 0);
    while (n) {
        uint32_t c = n > KSU_CHUNK ? KSU_CHUNK : n;
        t += kv_record_size((uint16_t)c);
        n -= c;
    }
    return t;
}

int ksu_kv_save(kvstore_t *kv, ksu_slot_t slot, const uint8_t *snap, uint32_t n, const uint8_t *ng, uint32_t ngn,
                bool allow_compact)
{
    uint8_t h[KSU_KV_HDR];
    uint32_t bank = 0, gen = 0, nch = (n + KSU_CHUNK - 1) / KSU_CHUNK;
    if (n == 0 || n > KSU_SNAP_MAX || ngn > KSU_NG_MAX)
        return KSU_KV_FAIL;
    if (!allow_compact && kv_free(kv) < ksu_kv_need(n, ngn))
        return KSU_KV_NOROOM;
    if (read_hdr(kv, slot, h)) {
        bank = 1u - h[3];
        gen = get32(h + 8);
    }
    for (uint32_t k = 0; k < nch; k++) {
        uint32_t c = n - k * KSU_CHUNK > KSU_CHUNK ? KSU_CHUNK : n - k * KSU_CHUNK;
        if (!kv_put(kv, chunk_key(slot, bank, k), snap + k * KSU_CHUNK, (uint16_t)c))
            return KSU_KV_FAIL;
    }
    if (ngn) {   /* the NG part of this bank and gen, before the header: a cut leaves the old pair whole */
        uint8_t r[KSU_NG_REC];
        r[0] = 'N';
        r[1] = 'G';
        r[2] = 1;
        r[3] = (uint8_t)bank;
        put32(r + 4, gen + 1u);
        memcpy(r + 8, ng, ngn);
        put32(r + 8 + ngn, ca_crc32(0, r, 8u + ngn));
        if (!kv_put(kv, ng_key(slot, bank), r, (uint16_t)(12u + ngn)))
            return KSU_KV_FAIL;
    }
    h[0] = 'K';
    h[1] = 'H';
    h[2] = 1;
    h[3] = (uint8_t)bank;
    h[4] = (uint8_t)nch;
    h[5] = 0;
    put16(h + 6, n);
    put32(h + 8, gen + 1u);
    put32(h + 12, ca_crc32(0, snap, n));
    return kv_put(kv, base(slot), h, sizeof h) ? KSU_KV_OK : KSU_KV_FAIL;
}

int ksu_kv_load(kvstore_t *kv, ksu_slot_t slot, uint8_t *out, uint32_t cap, uint32_t *gen)
{
    uint8_t h[KSU_KV_HDR];
    uint32_t n, o = 0;
    if (!read_hdr(kv, slot, h))
        return -1;
    n = get16(h + 6);
    if (n > cap || (n + KSU_CHUNK - 1) / KSU_CHUNK != h[4])
        return -1;
    for (uint32_t k = 0; k < h[4]; k++) {
        uint32_t c = n - o > KSU_CHUNK ? KSU_CHUNK : n - o;
        if (kv_get(kv, chunk_key(slot, h[3], k), out + o, (uint16_t)c) != (int)c)
            return -1;
        o += c;
    }
    if (ca_crc32(0, out, n) != get32(h + 12) || !ksu_snap_check(out, n, NULL))
        return -1;
    if (gen)
        *gen = get32(h + 8);
    return (int)n;
}

int ksu_kv_load_ng(kvstore_t *kv, ksu_slot_t slot, uint8_t *out, uint32_t cap)
{
    uint8_t h[KSU_KV_HDR], r[KSU_NG_REC];
    int l;
    uint32_t n;
    if (!read_hdr(kv, slot, h))
        return -1;
    l = kv_get(kv, ng_key(slot, h[3]), r, sizeof r);
    if (l < 13 || l > (int)sizeof r)
        return -1;
    n = (uint32_t)l - 12u;
    if (r[0] != 'N' || r[1] != 'G' || r[2] != 1 || r[3] != h[3] || get32(r + 4) != get32(h + 8) || n > cap ||
        ca_crc32(0, r, 8u + n) != get32(r + 8 + n) || !ksu_ng_parts(r + 8, n, NULL, NULL, NULL, NULL))
        return -1;   /* none, torn, or the NG part of an older snapshot in this bank (a build before wrote this one) */
    memcpy(out, r + 8, n);
    return (int)n;
}

bool ksu_kv_clear(kvstore_t *kv)
{
    bool ok = kv_delete(kv, base(KSU_C));
    ok = kv_delete(kv, base(KSU_P)) && ok;
    return ksu_kv_set_mode(kv, KSU_STOCK) && ok;
}

/* ---- the NG part ------------------------------------------------------------------ */
uint32_t ksu_ng_build(const uint8_t *bt, uint8_t btn, const uint8_t *pw, uint8_t pwn, uint8_t out[KSU_NG_MAX])
{
    uint32_t o = 0;
    if (3u + btn + pwn > KSU_NG_MAX)
        return 0;
    out[o++] = 1;
    out[o++] = btn;
    if (btn)
        memcpy(out + o, bt, btn);
    o += btn;
    out[o++] = pwn;
    if (pwn)
        memcpy(out + o, pw, pwn);
    return o + pwn;
}

bool ksu_ng_parts(const uint8_t *ng, uint32_t n, const uint8_t **bt, uint8_t *btn, const uint8_t **pw, uint8_t *pwn)
{
    uint32_t b, p;
    if (n < 3 || ng[0] != 1)
        return false;
    b = ng[1];
    if (2u + b + 1u > n)
        return false;
    p = ng[2u + b];
    if (3u + b + p != n)
        return false;
    if (bt)
        *bt = ng + 2;
    if (btn)
        *btn = (uint8_t)b;
    if (pw)
        *pw = ng + 3u + b;
    if (pwn)
        *pwn = (uint8_t)p;
    return true;
}

ksu_ng_t ksu_ng_decide(ksu_mode_t mode, bool all, bool at_boot, bool have_ng)
{
    if (!at_boot)
        return KSU_NG_KEEP;   /* a live apply keeps what the piano has now */
    if (mode == KSU_STOCK)
        return all ? KSU_NG_DEFAULTS : KSU_NG_KEEP;
    return have_ng ? KSU_NG_SAVED : KSU_NG_KEEP;
}

uint8_t ksu_apo_factory(uint32_t model_code, uint32_t dest)
{
    if (dest == 0xA5A5AA01u)
        return 1;
    if (dest == 0xAAA55504u || dest == 0x555AA503u || dest == 0x55AA5A02u)
        return 0;
    return model_code == 0xAA5AA502u ? 1 : 0;
}

int ksu_snap_factory(uint8_t apo, uint8_t *out, uint32_t cap)
{
    static const uint8_t k_apo[3] = {0x51, 0x00, 0x7F}, k_lock[3] = {0x51, 0x22, 0x7F}, unlocked[2] = {0, 0};
    uint32_t o = KSU_SNAP_HEAD;
    if (cap < KSU_SNAP_HEAD || !rec_put(out, cap, &o, k_apo, &apo, 1) || !rec_put(out, cap, &o, k_lock, unlocked, 2))
        return -1;
    out[0] = 'K';
    out[1] = 'S';
    out[2] = 1;
    out[3] = 0;
    put16(out + 4, 2);
    put16(out + 6, 0);
    return (int)o;
}
