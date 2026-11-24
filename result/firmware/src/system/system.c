#include "ca79ng/system.h"

#include <string.h>

/* record: param u8 | slot u8 | len u8 | data[len] */
static bool put_record(kvstore_t *kv, uint8_t *buf, uint16_t *used, int *key, int id, int slot, uint8_t len,
                       const uint8_t *v)
{
    if (*used + 3u + len > KV_MAX_VALUE) {
        if (*key >= SETTINGS_KEYS || !kv_put(kv, (uint16_t)(KV_KEY_STARTUP_SETTINGS + 0x100 + *key), buf, *used))
            return false;
        (*key)++;
        *used = 0;
    }
    buf[(*used)++] = (uint8_t)id;
    buf[(*used)++] = (uint8_t)slot;
    buf[(*used)++] = len;
    memcpy(buf + *used, v, len);
    *used = (uint16_t)(*used + len);
    return true;
}

int settings_save(const param_store_t *s, kvstore_t *kv)
{
    static param_store_t def;
    param_store_defaults(&def);
    uint8_t buf[KV_MAX_VALUE];
    uint16_t used = 0;
    int key = 0, records = 0;
    for (int id = 0; id < PRM_COUNT; id++) {
        const param_desc_t *d = &PARAM_DESC[id];
        if (d->fn != 0x10 || d->var_len || d->len == 0 || d->len > 4)
            continue;
        for (int slot = 0; slot < PARAM_SLOT_COUNT; slot++) {
            if (!(s->valid[id] & (1u << slot)))
                continue;
            int k = param_banded((param_id_t)id, slot);
            if (k >= 0 && s->band_valid[k]) { /* User EQ: a record per band (load puts each back) */
                for (int b = 0; b < PARAM_BANDS; b++) {
                    if (!(s->band_valid[k] & (1u << b)) ||
                        ((def.band_valid[k] & (1u << b)) && memcmp(def.band[k][b], s->band[k][b], d->len) == 0))
                        continue;
                    if (!put_record(kv, buf, &used, &key, id, slot, d->len, s->band[k][b]))
                        return -1;
                    records++;
                }
                continue;
            }
            bool same = (def.valid[id] & (1u << slot)) && memcmp(def.v[id][slot], s->v[id][slot], d->len) == 0;
            if (same)
                continue;
            if (!put_record(kv, buf, &used, &key, id, slot, d->len, s->v[id][slot]))
                return -1;
            records++;
        }
    }
    if (used) {
        if (key >= SETTINGS_KEYS || !kv_put(kv, (uint16_t)(KV_KEY_STARTUP_SETTINGS + 0x100 + key), buf, used))
            return -1;
        key++;
    }
    for (int k = key; k < SETTINGS_KEYS; k++)
        kv_delete(kv, (uint16_t)(KV_KEY_STARTUP_SETTINGS + 0x100 + k));
    return records;
}

int settings_load(param_store_t *s, kvstore_t *kv)
{
    uint8_t buf[KV_MAX_VALUE];
    int records = 0;
    for (int k = 0; k < SETTINGS_KEYS; k++) {
        int n = kv_get(kv, (uint16_t)(KV_KEY_STARTUP_SETTINGS + 0x100 + k), buf, sizeof buf);
        if (n <= 0)
            break;
        for (int i = 0; i + 3 <= n;) {
            uint8_t id = buf[i], slot = buf[i + 1], len = buf[i + 2];
            if (i + 3 + len > n || id >= PRM_COUNT || slot >= PARAM_SLOT_COUNT)
                return records; /* corrupt tail: keep what we have */
            if (param_validate((param_id_t)id, buf + i + 3, len)) {
                param_put(s, (param_id_t)id, slot, buf + i + 3, len);
                records++;
            }
            i += 3 + len;
        }
    }
    return records;
}

bool settings_clear(kvstore_t *kv)
{
    bool ok = true;
    for (int k = 0; k < SETTINGS_KEYS; k++)
        ok &= kv_delete(kv, (uint16_t)(KV_KEY_STARTUP_SETTINGS + 0x100 + k));
    return ok;
}

uint32_t power_timer_limit_ms(uint8_t setting)
{
    switch (setting) {
    case 1: return 15u * 60u * 1000u;
    case 2: return 60u * 60u * 1000u;
    case 3: return 120u * 60u * 1000u;
    default: return 0;
    }
}

void power_timer_init(power_timer_t *t, uint8_t setting)
{
    t->setting = setting;
    t->idle_ms = 0;
}

void power_timer_activity(power_timer_t *t) { t->idle_ms = 0; }

bool power_timer_tick(power_timer_t *t, uint32_t elapsed)
{
    uint32_t lim = power_timer_limit_ms(t->setting);
    if (!lim)
        return false;
    t->idle_ms += elapsed;
    return t->idle_ms >= lim;
}
