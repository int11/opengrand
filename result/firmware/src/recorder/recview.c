/* OpenGrand — read-only view of the stock recorder / song player (Phase 3-6 R0).
 * Addresses and their evidence: include/ca79ng/recview.h.
 * Nothing here writes stock memory. */
#include "ca79ng/recview.h"

static uint8_t rd8(const rv_mem_t *m, uint32_t a)
{
    uint8_t v = 0;
    m->read(m->ctx, a, &v, 1);
    return v;
}

static uint16_t rd16(const rv_mem_t *m, uint32_t a)
{
    uint8_t b[2] = {0, 0};
    m->read(m->ctx, a, b, 2);
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t rd32(const rv_mem_t *m, uint32_t a)
{
    uint8_t b[4] = {0, 0, 0, 0};
    m->read(m->ctx, a, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

uint8_t rv_transport(uint8_t st)
{
    switch (st) {
    case RV_ST_STOPPED0:
    case RV_ST_STOP: return RV_T_IDLE;
    case RV_ST_PLAY: return RV_T_PLAYING;
    case RV_ST_REC_STANDBY:
    case RV_ST_REC_STANDBY_NOMETRO: return RV_T_REC_ARMED;
    case RV_ST_COUNT_IN: return RV_T_COUNT_IN;
    case RV_ST_RECORDING: return RV_T_RECORDING;
    default: return RV_T_UNKNOWN;
    }
}

void rv_read_live(const rv_mem_t *m, rv_view_t *v)
{
    v->state = rd8(m, RV_A_STATE);
    v->transport = rv_transport(v->state);
    v->song = rd8(m, RV_A_SONG);
    v->rec_part = rd8(m, RV_A_REC_PART);
    v->play_sel = rd8(m, RV_A_PLAY_SEL);
    v->music_mode = rd8(m, RV_A_MUSIC_MODE);
    v->tempo = rd16(m, RV_A_TEMPO);
    v->tempo_orig = rd16(m, RV_A_TEMPO_ORIG);
    v->pos = rd32(m, RV_A_POS);
    v->max_bars = rd16(m, RV_A_MAX_BARS);
    v->max_slot = rd8(m, RV_A_MAX_SLOT);
    v->cur_bar = v->max_slot < RV_TRACKS
                     ? rd16(m, RV_A_TRACKS + (uint32_t)v->max_slot * RV_TRACK_SIZE + RV_TRACK_BAR)
                     : 0;
    v->ring = rd16(m, RV_A_RING_COUNT);
    v->bar = rd16(m, RV_A_BAR);
    v->mq_count = rd8(m, RV_A_MQ_COUNT);
    v->mq_hold = rd8(m, RV_A_MQ_HOLD);
    uint8_t b = 0;
    if (rd8(m, RV_A_JOB0) || rd8(m, RV_A_JOB1))
        b |= RV_BUSY_JOB;
    if (rd8(m, RV_A_WORKER))
        b |= RV_BUSY_WORKER;
    if (rd8(m, RV_A_FILEOP))
        b |= RV_BUSY_FILEOP;
    if (v->transport == RV_T_RECORDING || v->transport == RV_T_REC_ARMED || v->transport == RV_T_COUNT_IN)
        b |= RV_BUSY_REC;
    v->busy = b;
}

static const char SOUNDMODE[] = "- SoundMode             "; /* ROM 0x400A802C, 24 B compared */

static bool in_area(uint32_t off) { return off >= RV_STORE_DATA && off < RV_STORE_LIMIT; }

void rv_read_store(const rv_mem_t *m, rv_view_t *v)
{
    uint32_t off[RV_SONGS + 2];
    for (unsigned i = 0; i < RV_SONGS + 2; i++)
        off[i] = rd32(m, RV_A_STORE + 4 * i);
    for (unsigned i = 0; i < RV_SONGS; i++)
        v->songs[i] = (rv_song_t){0, 0, 0};
    v->store_end = off[RV_SONGS];
    v->store_bad = 0;
    v->store = RV_STORE_OK;

    /* the boot check FUN_4016E174, in its order: blank, then per song range / alignment, then
     * per track range / FA */
    if (off[0] == 0xFFFFFFFFu) {
        v->store = RV_STORE_BLANK;
        return;
    }
    for (unsigned i = 0; i < RV_SONGS; i++) {
        uint32_t s = off[i];
        if (!in_area(s)) {
            v->store = RV_STORE_SONG_RANGE, v->store_bad = (uint8_t)i;
            return;
        }
        if (s & 3) {
            v->store = RV_STORE_SONG_ALIGN, v->store_bad = (uint8_t)i;
            return;
        }
        for (unsigned t = 0; t < RV_TRACKS; t++) {
            uint32_t to = rd32(m, RV_A_STORE + s + 0x2C + 4 * t);
            if (!to)
                continue;
            if (!in_area(s + to)) {
                v->store = RV_STORE_TRACK_RANGE, v->store_bad = (uint8_t)(i * 16 + t);
                return;
            }
            if (rd8(m, RV_A_STORE + s + to) != 0xFA) {
                v->store = RV_STORE_TRACK_FA, v->store_bad = (uint8_t)(i * 16 + t);
                return;
            }
        }
    }
    /* our structure rules (FUN_40168408 keeps them; the boot check does not look) */
    for (unsigned i = 0; i < RV_SONGS; i++) {
        if (off[i + 1] < off[i] || off[i + 1] - off[i] < 0x74 || off[i + 1] > RV_STORE_LIMIT) {
            v->store = RV_STORE_ORDER, v->store_bad = (uint8_t)i;
            return;
        }
    }
    if (off[RV_SONGS + 1] != RV_STORE_LIMIT) {
        v->store = RV_STORE_ORDER, v->store_bad = RV_SONGS;
        return;
    }
    if (rd16(m, RV_A_STORE + off[RV_SONGS]) != 0xA55A || rd16(m, RV_A_STORE + RV_STORE_LIMIT) != 0xA55A) {
        v->store = RV_STORE_MARKER, v->store_bad = RV_SONGS;
        return;
    }
    for (unsigned i = 0; i < RV_SONGS; i++) {
        uint32_t s = RV_A_STORE + off[i];
        rv_song_t *g = &v->songs[i];
        g->len = off[i + 1] - off[i];
        g->bars = (uint16_t)rd32(m, s + 0x70);
        uint8_t f = 0;
        for (unsigned t = 0; t < RV_TRACKS; t++)
            if (rd32(m, s + 0x2C + 4 * t))
                f |= t == 0 ? RV_SONG_PART1 : t == 1 ? RV_SONG_PART2 : RV_SONG_EXTRA;
        if (rd32(m, s + 0x24))
            f |= RV_SONG_SETTINGS;
        uint8_t name[24];
        m->read(m->ctx, s + 8, name, sizeof name);
        bool sm = true;
        for (unsigned k = 0; k < sizeof name; k++)
            sm = sm && name[k] == (uint8_t)SOUNDMODE[k];
        if (sm)
            f |= RV_SONG_SOUNDMODE;
        g->flags = f;
    }
}

void rv_read(const rv_mem_t *m, rv_view_t *v)
{
    rv_read_live(m, v);
    rv_read_store(m, v);
}

static bool songs_equal(const rv_view_t *a, const rv_view_t *b)
{
    if (a->store != b->store || a->store_bad != b->store_bad || a->store_end != b->store_end)
        return false;
    for (unsigned i = 0; i < RV_SONGS; i++)
        if (a->songs[i].len != b->songs[i].len || a->songs[i].bars != b->songs[i].bars ||
            a->songs[i].flags != b->songs[i].flags)
            return false;
    return true;
}

uint32_t rv_diff(const rv_view_t *o, const rv_view_t *c)
{
    uint32_t d = 0;
    if (o->state != c->state)
        d |= RV_CH_TRANSPORT;
    if (o->song != c->song)
        d |= RV_CH_SONG;
    if (o->rec_part != c->rec_part || o->play_sel != c->play_sel)
        d |= RV_CH_PART;
    if (o->tempo != c->tempo || o->tempo_orig != c->tempo_orig)
        d |= RV_CH_TEMPO;
    if (o->cur_bar != c->cur_bar || o->bar != c->bar)
        d |= RV_CH_BAR;
    if (o->max_bars != c->max_bars || o->max_slot != c->max_slot)
        d |= RV_CH_MAXTIME;
    if (o->busy != c->busy)
        d |= RV_CH_BUSY;
    if (o->music_mode != c->music_mode)
        d |= RV_CH_MODE;
    if (!songs_equal(o, c))
        d |= RV_CH_STORE;
    /* the table can have changed: a recording ended, a NOR job or a file operation finished */
    if ((o->transport == RV_T_RECORDING && c->transport != RV_T_RECORDING) ||
        ((o->busy & RV_BUSY_JOB) && !(c->busy & RV_BUSY_JOB)) ||
        ((o->busy & RV_BUSY_FILEOP) && !(c->busy & RV_BUSY_FILEOP)))
        d |= RV_CH_COMMIT;
    return d;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v), put16(p + 2, (uint16_t)(v >> 16)); }

size_t rv_encode(const rv_view_t *v, uint8_t *o, size_t cap)
{
    if (cap < RV_ENC_LEN)
        return 0;
    for (unsigned i = 0; i < RV_ENC_LEN; i++)
        o[i] = 0;
    o[0] = RV_ENC_VERSION;
    o[1] = v->state, o[2] = v->transport, o[3] = v->song, o[4] = v->rec_part;
    o[5] = v->play_sel, o[6] = v->music_mode, o[7] = v->busy;
    put16(o + 8, v->tempo), put16(o + 10, v->tempo_orig), put32(o + 12, v->pos);
    put16(o + 16, v->max_bars), o[18] = v->max_slot, o[19] = v->store;
    put16(o + 20, v->cur_bar), put16(o + 22, v->ring), put32(o + 24, v->store_end), o[28] = v->store_bad;
    put16(o + 29, v->bar);
    for (unsigned i = 0; i < RV_SONGS; i++) {
        uint8_t *p = o + 32 + 7 * i;
        put32(p, v->songs[i].len), put16(p + 4, v->songs[i].bars), p[6] = v->songs[i].flags;
    }
    return RV_ENC_LEN;
}
