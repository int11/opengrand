#include <math.h>
#include <string.h>

#include "ca79ng/kproto.h"
#include "ca79ng/util.h"

int param_slot_from_part(uint8_t part)
{
    for (int i = 0; i < PARAM_SLOT_COUNT; i++)
        if (PARAM_SLOT_PART[i] == part)
            return i;
    return -1;
}

uint8_t param_part_from_slot(int slot)
{
    return (slot >= 0 && slot < PARAM_SLOT_COUNT) ? PARAM_SLOT_PART[slot] : 0x7F;
}

void param_store_defaults(param_store_t *s)
{
    memset(s, 0, sizeof *s);
    for (unsigned i = 0; i < PARAM_DEFAULT_COUNT; i++) {
        const param_default_t *d = &PARAM_DEFAULTS[i];
        memcpy(s->v[d->param][d->slot], d->data, d->len);
        s->valid[d->param] |= 1u << d->slot;
    }
}

bool param_get(const param_store_t *s, param_id_t id, int slot, uint8_t out[4])
{
    if ((unsigned)id >= PRM_COUNT || slot < 0 || slot >= PARAM_SLOT_COUNT || !(s->valid[id] & (1u << slot)))
        return false;
    memcpy(out, s->v[id][slot], 4);
    return true;
}

uint8_t param_get1(const param_store_t *s, param_id_t id, int slot, uint8_t fallback)
{
    uint8_t v[4];
    return param_get(s, id, slot, v) ? v[0] : fallback;
}

bool param_validate(param_id_t id, const uint8_t *data, uint8_t len)
{
    if ((unsigned)id >= PRM_COUNT)
        return false;
    const param_desc_t *d = &PARAM_DESC[id];
    if (d->var_len)
        return true; /* streamed payloads are checked by their handler */
    if (len < d->len)
        return false;
    /* The descriptor's first-byte mask for InternalSongDelete/-Save (0x04)
     * rejects the app's "all songs" form 7F 7F 7F 7F (protocol_spec raw_seen);
     * accept 0x7F there (OpenGrand decision, REMOTE_PROTOCOL.md §9). */
    if ((id == PRM_INTERNAL_SONG_DELETE_REMOTE || id == PRM_INTERNAL_SONG_SAVE_REMOTE) && data[0] == 0x7F) {
        for (int i = 1; i < d->len; i++)
            if (data[i] > 0x7F)
                return false;
        return true;
    }
    /* TouchCurveLearn [memoryNo, learn]: the app sends learn 1 = start and
     * 2 = end (protocol_spec), above the descriptor's second-byte mask (1). */
    if (id == PRM_TOUCH_CURVE_LEARN_REMOTE)
        return data[0] <= 0x7F && data[1] >= 1 && data[1] <= 2;
    for (int i = 0; i < d->len && i < 4; i++)
        if (data[i] > 0x7F || data[i] > d->data_max[i])
            return false;
    return true;
}

bool param_put(param_store_t *s, param_id_t id, int slot, const uint8_t *data, uint8_t len)
{
    if ((unsigned)id >= PRM_COUNT || slot < 0 || slot >= PARAM_SLOT_COUNT)
        return false;
    const param_desc_t *d = &PARAM_DESC[id];
    if (d->var_len || d->len > 4)
        return false;
    uint8_t n = CA_MIN(len, d->len);
    bool changed = !(s->valid[id] & (1u << slot)) || memcmp(s->v[id][slot], data, n) != 0;
    memcpy(s->v[id][slot], data, n);
    s->valid[id] |= 1u << slot;
    return changed;
}

/* Several descriptors share (fn, v1, v2) and differ only by part rule
 * (e.g. TransposeValue for the Global part 0x0F vs. per-part). */
int param_find(uint8_t fn, uint8_t v1, uint8_t v2, uint8_t part)
{
    int any = -1;
    for (int i = 0; i < PRM_COUNT; i++) {
        const param_desc_t *d = &PARAM_DESC[i];
        if (d->fn != fn || d->v1 != v1 || d->v2 != v2)
            continue;
        bool fits = (part == 0x7F) ? d->scope == SCOPE_GLOBAL
                  : (part == 0x0F) ? d->scope == SCOPE_SYSTEM
                  : d->scope == SCOPE_PART || d->scope == SCOPE_RECORDER;
        if (fits)
            return i;
        if (any < 0)
            any = i;
    }
    return any;
}

uint8_t param_range_encode(param_id_t id, int ui)
{
    const param_desc_t *d = &PARAM_DESC[id];
    if (d->codec != CODEC_RANGE || d->ui_max == d->ui_min)
        return (uint8_t)ca_clampi(ui, 0, 127);
    ui = ca_clampi(ui, d->ui_min, d->ui_max);
    /* JS: floor((ui-min)/(max-min)*(rawMax-rawMin)) + rawMin */
    double x = (double)(ui - d->ui_min) / (d->ui_max - d->ui_min) * (d->raw_max - d->raw_min);
    return (uint8_t)((int)floor(x + 1e-9) + d->raw_min);
}

int param_range_decode(param_id_t id, uint8_t raw)
{
    const param_desc_t *d = &PARAM_DESC[id];
    if (d->codec != CODEC_RANGE || d->raw_max == d->raw_min)
        return raw;
    /* JS: round((raw-rawMin)/(rawMax-rawMin)*(max-min)) + min */
    double x = (double)((int)raw - d->raw_min) / (d->raw_max - d->raw_min) * (d->ui_max - d->ui_min);
    return ca_clampi((int)lround(x) + d->ui_min, d->ui_min, d->ui_max);
}
