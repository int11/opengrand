/* OpenGrand - Phase 3-4 settings state S1 (store first fill) / compare pass.  See kpstate.h. */
#include "ca79ng/kpstate.h"

#include <string.h>

#define DUMP_V1 0x5F
#define DUMP_V2 0x7F
#define DUMP_PART 0x7F
#define DUMP_CC 0x7F
#define DUMP_PORT 3   /* the panel port: only passed through to io.value, which ignores it for part 0x7F */

void kst_init(kst_t *s, param_store_t *store, const kst_io_t *io)
{
    memset(s, 0, sizeof *s);
    s->io = *io;
    s->store = store;
}

void kst_start(kst_t *s, kst_mode_t mode)
{
    /* S2 window: a difference this walk finds is explained by events since the previous walk began */
    s->mark_save = s->mark_prev;
    s->mark_prev = s->mark_cur;
    s->mark_cur = s->ev;
    s->gen_start = s->gen;   /* S3: the walk is clean when no store-relevant event happens before it ends */
    s->mode = mode;
    s->i = 0;
    s->j = 0;
    uint32_t passes = s->st.passes;
    memset(&s->st, 0, sizeof s->st);
    s->st.passes = passes;   /* walks finished since init */
}

/* kst_put_frame + what one() needs: the key's id / slot (one param_find - a linear search, the costly part of a
 * pump-tail pass) and the store's bytes before the put (old / olen, olen 0 = none) */
static kst_frame_t put_frame(param_store_t *store, const uint8_t *f, uint32_t n, bool *changed, int *pid, int *pslot,
                             uint8_t old[4], uint8_t *olen)
{
    int id, slot;
    uint32_t dlen;
    *changed = false;
    *olen = 0;
    if (n < 10 || f[0] != 0xF0 || f[1] != 0x40 || f[3] != 0x10 || f[4] != 0x04 || f[5] != 0x1E || f[n - 1] != 0xF7)
        return KST_BAD;
    dlen = n - 10;
    id = param_find(0x10, f[6], f[7], f[8]);
    slot = param_slot_from_part(f[8]);
    if (id < 0 || slot < 0)
        return KST_UNKNOWN;
    if (PARAM_DESC[id].var_len || PARAM_DESC[id].len > 4 || dlen > 4)
        return KST_LONG;
    if (dlen != PARAM_DESC[id].len || !param_validate((param_id_t)id, f + 9, (uint8_t)dlen))
        return KST_BAD;
    *pid = id;
    *pslot = slot;
    if (param_get(store, (param_id_t)id, slot, old))
        *olen = (uint8_t)dlen;
    *changed = param_put(store, (param_id_t)id, slot, f + 9, (uint8_t)dlen);
    return *changed ? KST_PUT : KST_SAME;
}

kst_frame_t kst_put_frame(param_store_t *store, const uint8_t *f, uint32_t n, bool *changed)
{
    int id, slot;
    uint8_t old[4], olen;
    return put_frame(store, f, n, changed, &id, &slot, old, &olen);
}

/* FUN_40152A34: 0x69 always, 0x76 / 0x8E for v1 0x5F, then FUN_4015205C */
static bool skip(const kst_t *s, uint8_t i)
{
    if (i == 0x69 || i == 0x76 || i == 0x8E)
        return true;
    return !s->io.match(s->io.ctx, i, DUMP_V1, DUMP_V2);
}

/* keys whose first data byte is a sub-index: the stock sends several frames with the same v1 v2 part
 * (REA FUN_4015E960 cases 0x2D / 0x2E = ids 0x64 / 0x65 -> 54 05 0F [03|04] 00 v; ids 0x77..0x79 are
 * walked x4 by FUN_401526DC: case 0x41 = id 0x78 -> 51 1A 7F band gain+0x40 00 (bands 0..3), case 0x42 =
 * id 0x79 -> 51 1B 7F band freq-lo freq-hi (bands 1, 2)).  The store has one value per (id, slot), so these
 * live in the side table instead (S3 answers them from there). */
static const uint8_t INDEXED[][3] = {{0x54, 0x05, 0x0F}, {0x51, 0x1A, 0x7F}, {0x51, 0x1B, 0x7F}};

bool kst_is_indexed(uint8_t v1, uint8_t v2, uint8_t part)
{
    for (unsigned k = 0; k < sizeof INDEXED / sizeof INDEXED[0]; k++)
        if (INDEXED[k][0] == v1 && INDEXED[k][1] == v2 && INDEXED[k][2] == part)
            return true;
    return false;
}

static kst_frame_t put_indexed(kst_t *s, const uint8_t *f, uint32_t n, bool *changed, uint8_t old[4], uint8_t *olen)
{
    uint32_t dlen = n - 10;
    kst_idx_t *e = 0;
    *changed = false;
    *olen = 0;
    if (dlen < 1 || dlen > 4)
        return KST_BAD;
    for (unsigned k = 0; k < KST_IDX_MAX; k++) {
        kst_idx_t *x = &s->idx[k];
        if (x->len && memcmp(x->key, f + 6, 3) == 0 && x->data[0] == f[9]) {
            e = x;
            break;
        }
        if (!x->len && !e)
            e = x;
    }
    if (!e)
        return KST_LONG;   /* table full: counted like a value the store cannot hold */
    *changed = e->len != dlen || memcmp(e->key, f + 6, 3) != 0 || memcmp(e->data, f + 9, dlen) != 0;
    if (e->len && memcmp(e->key, f + 6, 3) == 0) {
        memcpy(old, e->data, 4);
        *olen = e->len;
    }
    memcpy(e->key, f + 6, 3);
    memset(e->data, 0, sizeof e->data);
    memcpy(e->data, f + 9, dlen);
    e->len = (uint8_t)dlen;
    e->i = s->i;   /* where the walk read it: S3 answers frame j of descriptor i from this entry */
    e->j = s->j;
    return *changed ? KST_PUT : KST_SAME;
}

bool kst_seen(const kst_t *s, int id, int slot)
{
    return id >= 0 && id < PRM_COUNT && slot >= 0 && slot < PARAM_SLOT_COUNT && (s->seen[id] >> slot & 1u);
}

int kst_indexed_get(const kst_t *s, uint8_t v1, uint8_t v2, uint8_t part, uint8_t sub, uint8_t out[4])
{
    for (unsigned k = 0; k < KST_IDX_MAX; k++) {
        const kst_idx_t *x = &s->idx[k];
        if (x->len && x->key[0] == v1 && x->key[1] == v2 && x->key[2] == part && x->data[0] == sub) {
            memcpy(out, x->data, 4);
            return x->len;
        }
    }
    return 0;
}

/* ---- S2: explained / unexplained ------------------------------------------------------------------------------ */
void kst_event(kst_t *s, kst_why_t why)
{
    if (why <= KST_WHY_NONE || why >= KST_WHY_N)
        return;
    s->last_ev[why] = ++s->ev;
    s->gen++;   /* S3: the store may be behind until a whole walk ran after this */
}

void kst_event_run(kst_t *s, kst_why_t why)
{
    uint32_t g = s->gen;
    kst_event(s, why);
    s->gen = g;
}

void kst_note(kst_t *s, const uint8_t key[3])
{
    kst_note_t *e = 0;
    for (unsigned k = 0; k < KST_NOTES; k++)
        if (s->notes[k].live && memcmp(s->notes[k].key, key, 3) == 0)
            e = &s->notes[k];   /* the same key again: refresh it in place */
    if (!e) {
        e = &s->notes[s->note_next];
        s->note_next = (uint8_t)((s->note_next + 1u) % KST_NOTES);
        if (e->live && e->ev > s->mark_prev)
            kst_event(s, KST_WHY_NOTIFY);   /* a notice still in the window is pushed out: any key may be it */
    }
    if (!kst_key_stock(key[0], key[1], key[2]))
        s->gen++;   /* S3: a notice of a settings key - the store is behind until the next clean walk */
    memcpy(e->key, key, 3);
    e->live = 1;
    e->ev = ++s->ev;
}

/* REA: keys the stock writes itself, outside the set apply and whoever is bound.  part 0xFF =
 * every part of v1 v2. */
static const uint8_t STOCK_KEYS[][3] = {
    {0x56, 0x08, 0x7F},   /* metronome run: FUN_4016E6BC(block + 0x12, 0) from stop-all FUN_400B0EA8 (CONNECT
                           * FUN_40151BA0, standby, factory reset, power off), recorder FUN_40169EDC / FUN_40169F00,
                           * Kawai FUN_4014A1B4 */
    {0x52, 0x02, 0x7F},   /* song tempo *0x4191570C: song / recorder code only (literal pools 0x40166F7C, 0x4016E5F8) */
    {0x52, 0x01, 0x7F},   /* song part: recorder bytes 0x4191EF72 / 73 / 79 and MusicMode 0x4139F4C6 (run state)    */
    {0x52, 0x18, 0x7F},   /* MaxTime: song load FUN_401694E4 / FUN_401736E4 -> notify queue FUN_40151A38            */
    {0x51, 0x29, 0x7F},   /* headphone jack: engine loop 0x4013BD7C / 0x4013BEA0 -> FUN_4014C048(0x58)               */
    {0x51, 0x2E, 0x7F},   /* USB memory: 0x401275E0 -> FUN_4014C048(0x86)                                            */
    {0x58, 0x10, 0x7F},   /* MIDI mode: GM / GS reset FUN_40149610 / FUN_401497BC (universal SysEx, any MIDI port)   */
    {0x58, 0x01, 0x7F},   /* Local Control: MIDI CC 0x7A, FUN_40148218 -> FUN_4016053C(ctx, 0x7F, 0x5C)              */
    {0x55, 0x00, 0xFF},   /* Sound of a part: MIDI Program Change, FUN_401475CC -> FUN_4016053C(ctx, 0, 7)            */
};

bool kst_key_stock(uint8_t v1, uint8_t v2, uint8_t part)
{
    for (unsigned k = 0; k < sizeof STOCK_KEYS / sizeof STOCK_KEYS[0]; k++)
        if (STOCK_KEYS[k][0] == v1 && STOCK_KEYS[k][1] == v2 && (STOCK_KEYS[k][2] == 0xFF || STOCK_KEYS[k][2] == part))
            return true;
    return false;
}

kst_why_t kst_explain(const kst_t *s, const uint8_t key[3])
{
    static const uint8_t order[] = {KST_WHY_RESTORE, KST_WHY_POWER, KST_WHY_REC, KST_WHY_SET, KST_WHY_STOCK,
                                    KST_WHY_NOTIFY};
    for (unsigned k = 0; k < KST_NOTES; k++)
        if (s->notes[k].live && s->notes[k].ev > s->mark_prev && memcmp(s->notes[k].key, key, 3) == 0)
            return KST_WHY_NOTIFY;
    if (kst_key_stock(key[0], key[1], key[2]))
        return KST_WHY_KEY;
    for (unsigned k = 0; k < sizeof order; k++)
        if (s->last_ev[order[k]] > s->mark_prev)
            return (kst_why_t)order[k];
    return KST_WHY_NONE;
}

void kst_abort(kst_t *s)
{
    if (s->mode == KST_IDLE)
        return;
    s->mode = KST_IDLE;
    s->mark_cur = s->mark_prev;
    s->mark_prev = s->mark_save;
}

uint32_t kst_ring_count(const kst_t *s)
{
    return s->why[KST_WHY_NONE] < KST_RING ? s->why[KST_WHY_NONE] : KST_RING;
}

bool kst_ring_get(const kst_t *s, uint32_t k, kst_unexp_t *out)
{
    if (k >= kst_ring_count(s))
        return false;
    *out = s->ring[(s->ring_head + KST_RING - 1u - k) % KST_RING];
    return true;
}

static void differs(kst_t *s, const uint8_t *f, uint32_t n, const uint8_t old[4], uint8_t olen)
{
    kst_why_t why = kst_explain(s, f + 6);
    uint32_t dlen = n - 10 > 4 ? 4 : n - 10;
    s->why[why]++;
    s->last_why = (uint8_t)why;
    if (why == KST_WHY_KEY && s->s3_on && s->io.fan && kst_key_silent(f[6], f[7], f[8])) {
        s->io.fan(s->io.ctx, f, n);   /* S3: nobody heard this change of the stock's (MIDI channel path) */
        s->s3.fanned_key++;
    }
    if (why == KST_WHY_SET && s->s4_on && kst_s3_trusted(s) && kst_s3_owned(s->i)) {
        /* S4: only a kept set (kst_event_run) leaves a SET in the window while trusted - its apply changed this key too */
        s->s4.side++;
        memcpy(s->s4_last_side, f + 6, 3);
        s->gen++;   /* other keys of that set may be behind as well: untrusted until a clean walk */
    }
    if (why != KST_WHY_NONE)
        return;
    if (s->s3_on && kst_s3_trusted(s) && kst_s3_owned(s->i))
        s->s3.stale++;   /* S3 may have answered the old value: the field's measure of the trust rule */
    kst_unexp_t *e = &s->ring[s->ring_head];
    s->ring_head = (uint8_t)((s->ring_head + 1u) % KST_RING);
    memset(e, 0, sizeof *e);
    memcpy(e->key, f + 6, 3);
    e->lens = (uint8_t)((olen & 0x0F) << 4 | (dlen & 0x0F));
    memcpy(e->old, old, olen > 4 ? 4 : olen);
    memcpy(e->now, f + 9, dlen);
    if (s->io.fan) {
        s->io.fan(s->io.ctx, f, n);
        s->fanned++;
    }
}

static void one(kst_t *s, const uint8_t *f, uint32_t n)
{
    bool changed;
    kst_frame_t r;
    uint8_t old[4] = {0}, olen = 0;
    if (n >= 10 && f[0] == 0xF0 && f[3] == 0x10 && f[n - 1] == 0xF7 && kst_is_indexed(f[6], f[7], f[8])) {
        r = put_indexed(s, f, n, &changed, old, &olen);
        if (r == KST_PUT || r == KST_SAME)
            s->st.indexed++;
    } else {
        int id = 0, slot = 0;
        r = put_frame(s->store, f, n, &changed, &id, &slot, old, &olen);
        if (r == KST_PUT || r == KST_SAME)   /* put_frame found both: the key the snapshot may carry */
            s->seen[id] |= 1u << slot;
    }
    s->st.frames++;
    switch (r) {
    case KST_PUT:
    case KST_SAME:
        s->st.stored++;
        if (s->mode == KST_COMPARE && changed) {
            s->st.diffs++;
            s->diffs_total++;
            memcpy(s->last_diff, f + 6, 3);
            differs(s, f, n, old, olen);
        }
        break;
    case KST_UNKNOWN:
        s->st.unknown++;
        memcpy(s->last_unknown, f + 6, 3);
        break;
    case KST_LONG: s->st.longval++; break;
    case KST_BAD: s->st.bad++; break;
    }
}

static bool step(kst_t *s, uint32_t budget, uint32_t calls)
{
    uint8_t f[KST_FRAME_MAX];
    if (s->mode == KST_IDLE)
        return true;
    while (s->i < KST_DESC_N && budget && calls) {
        int r;
        if (s->j == 0 && skip(s, s->i)) {
            s->i++;
            continue;
        }
        if (s->j < KST_J_MAX) {
            r = s->io.value(s->io.ctx, DUMP_PORT, DUMP_CC, s->i, DUMP_PART, s->j, f);
            calls--;
        } else {
            r = -1;
        }
        if (r > 0) {
            one(s, f, (uint32_t)r > KST_FRAME_MAX ? KST_FRAME_MAX : (uint32_t)r);
            s->j++;
            continue;
        }
        if (r == 0) {
            s->j++;
            continue;
        }
        s->i++;   /* descriptor done */
        s->j = 0;
        budget--;
    }
    if (s->i < KST_DESC_N)
        return false;
    if (s->mode == KST_FILL)
        s->synced = true;
    if (s->gen == s->gen_start) {   /* S3: nothing store-relevant happened during this whole walk */
        s->clean = true;
        s->clean_gen = s->gen;
    }
    s->st.passes++;
    s->mode = KST_IDLE;
    return true;
}

bool kst_step(kst_t *s, uint32_t budget)
{
    return step(s, budget, UINT32_MAX);
}

bool kst_step_calls(kst_t *s, uint32_t calls)
{
    return step(s, KST_DESC_N, calls ? calls : 1u);
}

void kst_run(kst_t *s, kst_mode_t mode)
{
    kst_start(s, mode);
    while (!kst_step(s, KST_DESC_N))
        ;
}

/* ---- S3: read owner ----------------------------------------------------------------------------------------------
 * Settings descriptors (getter id = descriptor index i), REA FUN_4015E960: the value is a byte of
 * the settings block (iVar6 = FUN_4015B4D8(ctx), ctx 0..5 one block 0x418F19E4) or of the live global (iVar5 =
 * FUN_4015B4D0() = 0x418CE0DC), plain or through a ROM table (id 0x15: FUN_4015DB88 / FUN_4015DB3C -> 0x400A74FB /
 * 0x400A712F; id 0x37: FUN_4014EDC8 -> 0x400FD8F0; id 0x72: the beat table 0x401235B8), and "no value" depends on x
 * only (ids 0x0E, 0x0F).  Everything else is the stock's: run-state calls (0: FUN_40135980, 7: Sound table
 * FUN_40165108, 0x58 / 0x68 / 0x6A / 0x6F / 0x76 / 0x79 / 0x86 / 0x87 / 0x89-0x8C / 0x8E / 0x92-0x94 / 0xA4-0xA9) and
 * the metronome run byte 0x70 (block +0x49, written by stop-all FUN_400B0EA8); ids with no case answer nothing. */
static const uint8_t S3_OWNED[][2] = {
    {0x01, 0x06}, {0x08, 0x18}, {0x37, 0x40}, {0x42, 0x57}, {0x5A, 0x62}, {0x64, 0x66}, {0x71, 0x75},
    {0x78, 0x78}, {0x8D, 0x8D}, {0x90, 0x91}, {0xAF, 0xAF},
};

bool kst_s3_owned(uint8_t i)
{
    for (unsigned k = 0; k < sizeof S3_OWNED / sizeof S3_OWNED[0]; k++)
        if (i >= S3_OWNED[k][0] && i <= S3_OWNED[k][1])
            return true;
    return false;
}

/* REA: the MIDI channel paths apply through FUN_4016053C and send no notification -
 * FUN_401475CC (Program Change) and FUN_40148218 (CC 0x7A) */
bool kst_key_silent(uint8_t v1, uint8_t v2, uint8_t part)
{
    (void)part;
    return (v1 == 0x55 && v2 == 0x00) || (v1 == 0x58 && v2 == 0x01 && part == 0x7F);
}

void kst_s3_enable(kst_t *s, bool on)
{
    s->s3_on = on;
}

bool kst_s3_trusted(const kst_t *s)
{
    return s->synced && s->clean && s->clean_gen == s->gen;
}

int kst_s3_frame(kst_t *s, uint8_t i, uint8_t j, uint8_t cc, const uint8_t key[3], uint8_t out[KST_FRAME_MAX])
{
    uint8_t d[4];
    unsigned n = 0, o = 0;
    if (!s->s3_on)
        return 0;
    if (!kst_s3_owned(i) || kst_key_stock(key[0], key[1], key[2])) {
        s->s3.stock++;
        return 0;
    }
    if (!kst_s3_trusted(s)) {
        s->s3.untrusted++;
        return 0;
    }
    if (kst_is_indexed(key[0], key[1], key[2])) {
        for (unsigned k = 0; k < KST_IDX_MAX && !n; k++) {
            const kst_idx_t *x = &s->idx[k];
            if (x->len && x->i == i && x->j == j && memcmp(x->key, key, 3) == 0) {
                memcpy(d, x->data, 4);
                n = x->len;
            }
        }
    } else {
        int id = param_find(0x10, key[0], key[1], key[2]), slot = param_slot_from_part(key[2]);
        if (id >= 0 && slot >= 0 && kst_seen(s, id, slot) && !PARAM_DESC[id].var_len && PARAM_DESC[id].len <= 4 &&
            param_get(s->store, (param_id_t)id, slot, d))
            n = PARAM_DESC[id].len;
    }
    if (!n) {
        s->s3.miss++;
        return 0;
    }
    out[o++] = 0xF0;
    out[o++] = 0x40;
    out[o++] = cc;
    out[o++] = 0x10;
    out[o++] = 0x04;
    out[o++] = 0x1E;
    memcpy(out + o, key, 3);
    o += 3;
    memcpy(out + o, d, n);
    o += n;
    out[o++] = 0xF7;
    s->s3.served++;
    return (int)o;
}

/* ---- Phase 4: same-tick store update ---------------------------------------------------------------------------- */
bool kst_absorb(kst_t *s, const uint8_t *f, uint32_t n)
{
    bool changed = false, fan;
    int id = 0, slot = 0;
    uint8_t old[4], olen;
    kst_frame_t r;
    s->abs.frames++;
    if (n > KST_FRAME_MAX || n < 10 || kst_is_indexed(f[6], f[7], f[8])) {
        s->abs.bad++;
        return false;
    }
    r = put_frame(s->store, f, n, &changed, &id, &slot, old, &olen);
    if (r != KST_PUT && r != KST_SAME) {
        s->abs.bad++;
        return false;
    }
    s->seen[id] |= 1u << slot;
    if (!changed)
        return false;
    s->abs.changed++;
    fan = kst_key_silent(f[6], f[7], f[8]) ? s->s3_on : !kst_key_stock(f[6], f[7], f[8]);
    if (fan && s->io.fan) {
        s->io.fan(s->io.ctx, f, n);
        s->abs.fanned++;
    }
    return true;
}

/* ---- S4: write owner ------------------------------------------------------------------------------------------- */
void kst_s4_enable(kst_t *s, bool on)
{
    if (s->s4_on != on)
        s->gen++;   /* the trust built under the other rule is not carried over: a clean walk first (S3 exactly) */
    s->s4_on = on;
}

/* keys whose stock apply changes no other dump value, whatever the value - emulator-measured (roundtrip_state.py 8: every
 * settings byte the apply wrote is read by this key's getter only, for the values 0 / mask / mask/2, defaults and this
 * piano's NOR settings).  v2 0xFF = every v2 of v1, part 0xFF = every part. */
static const uint8_t S4_PURE[][3] = {
    /* S4_PURE_BEGIN - 135 keys, roundtrip_state.py 8 checks this list == the measurement */
    {0x50, 0x01, 0x00}, {0x50, 0x02, 0x00}, {0x50, 0x03, 0x00}, {0x50, 0x03, 0x08}, {0x50, 0x03, 0x09}, {0x50, 0x03, 0x0A},
    {0x50, 0x03, 0x0B}, {0x50, 0x04, 0x00}, {0x50, 0x04, 0x08}, {0x50, 0x04, 0x09}, {0x50, 0x04, 0x0A}, {0x50, 0x04, 0x0B},
    {0x50, 0x05, 0x00}, {0x50, 0x06, 0x00}, {0x50, 0x06, 0x08}, {0x50, 0x06, 0x09}, {0x50, 0x06, 0x0A}, {0x50, 0x06, 0x0B},
    {0x50, 0x07, 0x00}, {0x50, 0x07, 0x08}, {0x50, 0x07, 0x09}, {0x50, 0x07, 0x0A}, {0x50, 0x07, 0x0B}, {0x50, 0x08, 0x00},
    {0x50, 0x09, 0x00}, {0x50, 0x09, 0x08}, {0x50, 0x09, 0x09}, {0x50, 0x09, 0x0A}, {0x50, 0x09, 0x0B}, {0x50, 0x0A, 0x00},
    {0x50, 0x0B, 0x00}, {0x50, 0x0B, 0x08}, {0x50, 0x0B, 0x09}, {0x50, 0x0B, 0x0A}, {0x50, 0x0B, 0x0B}, {0x50, 0x0D, 0x00},
    {0x50, 0x0F, 0x00}, {0x50, 0x0F, 0x08}, {0x50, 0x0F, 0x09}, {0x50, 0x0F, 0x0A}, {0x50, 0x0F, 0x0B}, {0x50, 0x11, 0x00},
    {0x50, 0x11, 0x08}, {0x50, 0x11, 0x09}, {0x50, 0x11, 0x0A}, {0x50, 0x11, 0x0B}, {0x50, 0x12, 0x00}, {0x50, 0x14, 0x7F},
    {0x50, 0x15, 0x00}, {0x50, 0x15, 0x08}, {0x50, 0x15, 0x09}, {0x50, 0x15, 0x0A}, {0x50, 0x15, 0x0B}, {0x50, 0x16, 0x00},
    {0x50, 0x20, 0x0F}, {0x50, 0x21, 0x0F}, {0x50, 0x26, 0x00}, {0x50, 0x27, 0x0F}, {0x50, 0x28, 0x00}, {0x50, 0x28, 0x08},
    {0x50, 0x28, 0x09}, {0x50, 0x28, 0x0A}, {0x50, 0x28, 0x0B}, {0x50, 0x2A, 0x00}, {0x50, 0x2B, 0x7F}, {0x51, 0x03, 0x7F},
    {0x51, 0x08, 0x7F}, {0x51, 0x09, 0x7F}, {0x51, 0x0A, 0x7F}, {0x51, 0x0B, 0x7F}, {0x51, 0x0C, 0x7F}, {0x51, 0x0D, 0x7F},
    {0x51, 0x0F, 0x7F}, {0x51, 0x10, 0x7F}, {0x51, 0x2F, 0x7F}, {0x51, 0x30, 0x7F}, {0x51, 0x31, 0x7F}, {0x51, 0x3D, 0x7F},
    {0x51, 0x42, 0x7F}, {0x52, 0x0E, 0x7F}, {0x53, 0x00, 0x0F}, {0x53, 0x02, 0x00}, {0x53, 0x02, 0x08}, {0x53, 0x02, 0x09},
    {0x53, 0x02, 0x0A}, {0x53, 0x02, 0x0B}, {0x53, 0x03, 0x08}, {0x53, 0x03, 0x09}, {0x53, 0x03, 0x0A}, {0x53, 0x03, 0x0B},
    {0x53, 0x04, 0x08}, {0x53, 0x04, 0x09}, {0x53, 0x04, 0x0B}, {0x53, 0x05, 0x08}, {0x53, 0x06, 0x09}, {0x53, 0x06, 0x0B},
    {0x55, 0x03, 0x00}, {0x55, 0x05, 0x00}, {0x55, 0x06, 0x0F}, {0x55, 0x06, 0x31}, {0x55, 0x06, 0x32}, {0x55, 0x06, 0x33},
    {0x55, 0x06, 0x34}, {0x55, 0x06, 0x35}, {0x55, 0x06, 0x36}, {0x55, 0x06, 0x37}, {0x55, 0x06, 0x38}, {0x55, 0x06, 0x39},
    {0x55, 0x06, 0x3A}, {0x55, 0x06, 0x3B}, {0x55, 0x06, 0x3C}, {0x55, 0x06, 0x3D}, {0x55, 0x06, 0x3E}, {0x55, 0x06, 0x3F},
    {0x55, 0x07, 0x00}, {0x55, 0x08, 0x00}, {0x55, 0x09, 0x00}, {0x55, 0x11, 0x00}, {0x55, 0x12, 0x00}, {0x55, 0x13, 0x00},
    {0x55, 0x45, 0x00}, {0x55, 0x45, 0x08}, {0x55, 0x45, 0x09}, {0x55, 0x45, 0x0A}, {0x55, 0x45, 0x0B}, {0x55, 0x60, 0x00},
    {0x55, 0x61, 0x00}, {0x56, 0x01, 0x7F}, {0x56, 0x02, 0x7F}, {0x56, 0x03, 0x7F}, {0x56, 0x09, 0x7F}, {0x56, 0x0A, 0x7F},
    {0x58, 0x00, 0x7F}, {0x58, 0x09, 0x7F}, {0x58, 0x0B, 0x7F},
    /* S4_PURE_END */
};

bool kst_s4_pure(uint8_t v1, uint8_t v2, uint8_t part)
{
    for (unsigned k = 0; k < sizeof S4_PURE / sizeof S4_PURE[0]; k++)
        if (S4_PURE[k][0] == v1 && (S4_PURE[k][1] == 0xFF || S4_PURE[k][1] == v2) &&
            (S4_PURE[k][2] == 0xFF || S4_PURE[k][2] == part))
            return true;
    return false;
}

uint32_t kst_s4_pure_count(void)
{
    return (uint32_t)(sizeof S4_PURE / sizeof S4_PURE[0]);
}

typedef enum { S4_OWN, S4_REFUSE, S4_PASS } s4_kind_t;

static s4_kind_t s4_one(kst_t *s, kst_s4_set_t *r, uint8_t v1, uint8_t v2, uint8_t part, const uint8_t *d, uint32_t dlen)
{
    int id = param_find(0x10, v1, v2, part), slot = param_slot_from_part(part);
    const param_desc_t *pd;
    bool ok;
    s->s4.values++;
    if (id < 0 || slot < 0) {
        s->s4.passed++;
        r->passed++;
        return S4_PASS;
    }
    pd = &PARAM_DESC[id];
    ok = (pd->var_len || dlen == pd->len) && dlen <= 255 && param_validate((param_id_t)id, d, (uint8_t)dlen);
    if (ok) {
        s->s4.own++;
        r->nown++;
        if (!kst_s4_pure(v1, v2, part))
            r->pure = false;
    } else {
        s->s4.refused++;
        r->refused++;
        s->s4_last_refused[0] = v1;
        s->s4_last_refused[1] = v2;
        s->s4_last_refused[2] = part;
    }
    if (pd->var_len || pd->len > 4 || kst_is_indexed(v1, v2, part) || r->n >= KST_S4_KEYS) {
        if (ok) {
            r->unlisted++;
            r->pure = false;
        }
    } else {
        unsigned k;
        for (k = 0; k < r->n; k++)
            if (memcmp(r->key[k], (const uint8_t[3]){v1, v2, part}, 3) == 0)
                break;
        if (k == r->n) {
            r->key[k][0] = v1;
            r->key[k][1] = v2;
            r->key[k][2] = part;
            r->own[k] = ok;
            r->n++;
        } else if (ok) {
            r->own[k] = 1;   /* the same key twice in a packed set: the stock applies both, the last one stays */
        }
    }
    return ok ? S4_OWN : S4_REFUSE;
}

uint32_t kst_s4_judge(kst_t *s, const uint8_t *m, uint32_t n, uint8_t *out, kst_s4_set_t *r)
{
    memset(r, 0, sizeof *r);
    r->pure = true;
    if (n < 10)
        return 0;
    s->s4.sets++;
    if (m[6] == 0x5F && m[7] == 0x00) {   /* packed 10 5F 00 part <len v1 v2 part data..>.. (FUN_4014EBFC) */
        uint32_t end = n - 1, i = 9, o = 9;
        memcpy(out, m, 9);
        while (i < end) {
            uint8_t l = m[i];
            if (l < 4 || i + l > end)
                break;   /* the stock stops here too: what follows is never applied */
            if (s4_one(s, r, m[i + 1], m[i + 2], m[i + 3], m + i + 4, (uint32_t)l - 4) != S4_REFUSE) {
                memcpy(out + o, m + i, l);
                o += l;
            }
            i += l;
        }
        if (o == 9) {
            s->s4.dropped++;
            return 0;
        }
        out[o++] = 0xF7;
        if (!r->refused) {   /* nothing refused: the set as it came (bytes after a malformed entry included) */
            memcpy(out, m, n);
            return n;
        }
        return o;
    }
    if (s4_one(s, r, m[6], m[7], m[8], m + 9, n - 10) == S4_REFUSE) {
        s->s4.dropped++;
        return 0;
    }
    memcpy(out, m, n);
    return n;
}

void kst_s4_follow(kst_t *s, kst_s4_set_t *r, uint32_t k, const uint8_t *f, uint32_t fn)
{
    bool changed = false;
    kst_frame_t x;
    if (k >= r->n || fn < 10 || memcmp(f + 6, r->key[k], 3) != 0)
        return;
    x = kst_put_frame(s->store, f, fn, &changed);
    if (!r->own[k])
        return;   /* a refused key: the store keeps (or gets back) the stock's value */
    if ((x == KST_PUT || x == KST_SAME) && !changed) {
        s->s4.followed++;
        return;
    }
    s->s4.wmiss++;
    r->miss++;
    memcpy(s->s4_last_miss, r->key[k], 3);
}

void kst_s4_done(kst_t *s, const kst_s4_set_t *r)
{
    if (!r->nown && !r->passed)
        return;   /* nothing reached the stock */
    if (r->pure && !r->passed && !r->miss) {
        kst_event_run(s, KST_WHY_SET);   /* S2 still explains by it; the store stays trusted */
        s->s4.kept++;
    } else {
        kst_event(s, KST_WHY_SET);
    }
}
