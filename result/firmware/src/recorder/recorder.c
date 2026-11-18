#include "ca79ng/recorder.h"

#include <string.h>

#include "ca79ng/util.h"

void recorder_init(recorder_t *r, rec_event_t *pool, rec_event_t *stage, rec_out_fn out, void *ctx)
{
    memset(r, 0, sizeof *r);
    r->pool = pool;
    r->stage = stage;
    r->out = out;
    r->out_ctx = ctx;
    r->play_mask = 3;
    r->tempo_q16 = 65536;
    r->limit_ms = UINT32_MAX;
    r->ch_mask = 0xFFFF;
    for (int i = 0; i < REC_SLOTS; i++) {
        r->song[i].bpm = 120;
        r->song[i].numerator = 4;
        r->song[i].denominator = 4;
    }
}

uint32_t recorder_free_events(const recorder_t *r) { return REC_MAX_EVENTS - r->pool_used; }

void recorder_set_tempo(recorder_t *r, uint16_t bpm)
{
    const rec_song_t *s = &r->song[r->cur_song];
    if (bpm == 0 || s->bpm == 0)
        r->tempo_q16 = 65536;
    else
        r->tempo_q16 = (uint32_t)(((uint64_t)ca_clampi(bpm, 10, 500) << 16) / s->bpm);
}

const rec_event_t *recorder_events(const recorder_t *r, uint8_t song, uint32_t *count)
{
    if (song >= REC_SLOTS) {
        *count = 0;
        return r->pool;
    }
    *count = r->song[song].count;
    return &r->pool[r->song[song].first];
}

static void mark_dirty(recorder_t *r, uint8_t song)
{
    if (song < REC_SLOTS)
        r->dirty |= (uint16_t)(1u << song);
}

static rec_song_t *cur(recorder_t *r) { return &r->song[r->cur_song]; }

static void all_notes_off(recorder_t *r)
{
    if (!r->out)
        return;
    for (uint8_t p = 0; p < REC_PARTS; p++) {
        r->out(r->out_ctx, p, 0xB0, 64, 0);
        r->out(r->out_ctx, p, 0xB0, 123, 0);
    }
}

bool recorder_select(recorder_t *r, uint8_t song)
{
    if (song >= REC_SLOTS)
        return false;
    recorder_stop(r);
    r->cur_song = song;
    r->pos_ms = 0;
    r->next_index = 0;
    return true;
}

/* Remove song events from the shared pool and close the gap. */
static void pool_remove(recorder_t *r, uint8_t song, int part)
{
    rec_song_t *s = &r->song[song];
    uint32_t w = s->first;
    for (uint32_t i = s->first; i < s->first + s->count; i++)
        if (part >= 0 && r->pool[i].part != part)
            r->pool[w++] = r->pool[i];
    uint32_t removed = s->first + s->count - w;
    if (removed == 0)
        return;
    uint32_t tail = r->pool_used - (s->first + s->count);
    memmove(&r->pool[w], &r->pool[s->first + s->count], tail * sizeof(rec_event_t));
    r->pool_used -= removed;
    s->count -= removed;
    for (int k = 0; k < REC_SLOTS; k++)
        if (k != song && r->song[k].first > s->first)
            r->song[k].first -= removed;
}

bool recorder_erase(recorder_t *r, uint8_t song, int part)
{
    if (song >= REC_SLOTS || r->state == REC_RECORDING)
        return false;
    if (song == r->cur_song && r->state != REC_IDLE)
        recorder_stop(r);
    uint32_t before = r->song[song].count;
    pool_remove(r, song, part);
    if (r->song[song].count == 0) {
        r->song[song].length_ms = 0;
        r->song[song].ext = false;
    }
    if (before != r->song[song].count)
        mark_dirty(r, song);
    return true;
}

void recorder_standby(recorder_t *r, uint8_t part, uint16_t bpm, uint8_t num, uint8_t den)
{
    recorder_stop(r);
    r->rec_part = part & 1;
    rec_song_t *s = cur(r);
    if (s->count == 0) {
        s->bpm = bpm;
        s->numerator = num;
        s->denominator = den;
    }
    r->stage_count = 0;
    r->pos_ms = 0;
    r->next_index = 0;
    r->state = REC_STANDBY;
}

void recorder_start(recorder_t *r)
{
    if (r->state != REC_STANDBY)
        return;
    r->state = REC_RECORDING;
    r->pos_ms = 0;
    r->next_index = 0;
}

/* Insert staged events of the recorded part into the song (time-ordered merge). */
static void commit(recorder_t *r)
{
    rec_song_t *s = cur(r);
    pool_remove(r, r->cur_song, r->rec_part); /* overdub replaces that part */
    /* move this song to the end of the pool so it can grow */
    uint32_t keep = s->count;
    if (r->pool_used + r->stage_count > REC_MAX_EVENTS)
        r->stage_count = REC_MAX_EVENTS - r->pool_used;
    static rec_event_t tmp[1024];
    uint32_t moved = 0;
    if (s->first + keep != r->pool_used) {
        /* rotate [first, first+keep) to the end in chunks */
        while (moved < keep) {
            uint32_t n = CA_MIN(keep - moved, (uint32_t)CA_ARRAY_LEN(tmp));
            memcpy(tmp, &r->pool[s->first], n * sizeof *tmp);
            memmove(&r->pool[s->first], &r->pool[s->first + n], (r->pool_used - s->first - n) * sizeof *tmp);
            memcpy(&r->pool[r->pool_used - n], tmp, n * sizeof *tmp);
            for (int k = 0; k < REC_SLOTS; k++)
                if (&r->song[k] != s && r->song[k].first > s->first)
                    r->song[k].first -= n;
            moved += n;
        }
        s->first = r->pool_used - keep;
    }
    /* merge existing (other part) with staged events */
    rec_event_t *dst = &r->pool[s->first];
    uint32_t i = keep, j = r->stage_count, k = keep + r->stage_count;
    while (j > 0) {
        if (i > 0 && dst[i - 1].ms > r->stage[j - 1].ms)
            dst[--k] = dst[--i];
        else
            dst[--k] = r->stage[--j];
    }
    s->count = keep + r->stage_count;
    r->pool_used += r->stage_count;
    if (s->count)
        s->length_ms = CA_MAX(s->length_ms, dst[s->count - 1].ms);
    r->stage_count = 0;
    mark_dirty(r, r->cur_song);
}

void recorder_stop(recorder_t *r)
{
    if (r->state == REC_RECORDING)
        commit(r);
    if (r->state == REC_PLAYING || r->state == REC_RECORDING)
        all_notes_off(r);
    r->state = REC_IDLE;
    r->pos_ms = 0;
    r->next_index = 0;
}

void recorder_play(recorder_t *r, uint8_t mask)
{
    if (r->state == REC_RECORDING)
        recorder_stop(r);
    r->play_mask = mask ? mask : 3;
    if (r->state != REC_PAUSED) {
        r->pos_ms = 0;
        r->next_index = 0;
    }
    r->state = REC_PLAYING;
}

void recorder_pause(recorder_t *r)
{
    if (r->state == REC_PLAYING) {
        r->state = REC_PAUSED;
        all_notes_off(r);
    }
}

void recorder_seek(recorder_t *r, uint32_t ms)
{
    rec_song_t *s = cur(r);
    r->pos_ms = ms;
    uint32_t i = 0;
    while (i < s->count && r->pool[s->first + i].ms < ms)
        i++;
    r->next_index = i;
    all_notes_off(r);
}

void recorder_reset(recorder_t *r) { recorder_seek(r, 0); }

void recorder_set_ab(recorder_t *r, bool on, uint32_t a, uint32_t b)
{
    r->ab_on = on && b > a;
    r->a_ms = a;
    r->b_ms = b;
}

void recorder_input(recorder_t *r, uint8_t status, uint8_t d1, uint8_t d2)
{
    if (r->state == REC_STANDBY && (status & 0xF0) == 0x90 && d2)
        recorder_start(r); /* the first key starts recording */
    if (r->state != REC_RECORDING)
        return;
    if (r->pool_used + r->stage_count >= REC_MAX_EVENTS)
        return; /* memory full: further events are dropped */
    r->stage[r->stage_count++] = (rec_event_t){r->pos_ms, status, d1, d2, r->rec_part};
}

void recorder_tick(recorder_t *r, uint32_t elapsed)
{
    if (r->state != REC_PLAYING && r->state != REC_RECORDING)
        return;
    rec_song_t *s = cur(r);
    if (r->state == REC_PLAYING && r->tempo_q16 != 65536) {
        uint64_t q = (uint64_t)elapsed * r->tempo_q16 + r->tempo_frac;
        elapsed = (uint32_t)(q >> 16);
        r->tempo_frac = (uint32_t)(q & 0xFFFF);
    }
    uint32_t end = r->pos_ms + elapsed;
    if (r->state == REC_PLAYING && end > r->limit_ms)
        end = r->limit_ms > r->pos_ms ? r->limit_ms : r->pos_ms;
    /* during recording the other part plays back (overdub) */
    uint8_t mask = r->state == REC_RECORDING ? (uint8_t)(1u << (r->rec_part ^ 1)) : r->play_mask;
    while (r->next_index < s->count) {
        const rec_event_t *e = &r->pool[s->first + r->next_index];
        if (r->ab_on && r->state == REC_PLAYING && e->ms >= r->b_ms)
            break;
        if (e->ms > end)
            break;
        if ((mask >> e->part) & 1u && r->out && (!s->ext || ((r->ch_mask >> (e->status & 0x0F)) & 1u)))
            r->out(r->out_ctx, e->part, e->status, e->d1, e->d2);
        r->next_index++;
    }
    r->pos_ms = end;
    if (r->state == REC_PLAYING) {
        if (r->ab_on && r->pos_ms >= r->b_ms) {
            recorder_seek(r, r->a_ms);
        } else if (r->next_index >= s->count && r->pos_ms > s->length_ms && r->limit_ms == UINT32_MAX) {
            r->state = REC_IDLE; /* song finished */
            r->pos_ms = 0;
            r->next_index = 0;
        }
    }
}

void recorder_progress(const recorder_t *r, uint32_t *bar, uint32_t *beat)
{
    const rec_song_t *s = &r->song[r->cur_song];
    uint32_t beat_ms = 60000u / CA_MAX(s->bpm, 10u);
    if (s->denominator == 8)
        beat_ms /= 2;
    uint32_t beats = r->pos_ms / CA_MAX(beat_ms, 1u);
    *bar = beats / CA_MAX(s->numerator, 1u) + 1;
    *beat = beats % CA_MAX(s->numerator, 1u) + 1;
}

/* ---------------- SMF ---------------- */

static size_t put_vlq(uint8_t *p, uint32_t v)
{
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = v & 0x7F;
    while ((v >>= 7))
        tmp[n++] = (uint8_t)(0x80 | (v & 0x7F));
    for (int i = 0; i < n; i++)
        p[i] = tmp[n - 1 - i];
    return (size_t)n;
}

static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

#define SMF_PPQ 480u

size_t smf_write(const recorder_t *r, uint8_t song, uint8_t *out, size_t cap)
{
    if (song >= REC_SLOTS)
        return 0;
    const rec_song_t *s = &r->song[song];
    uint32_t us_per_q = 60000000u / CA_MAX(s->bpm, 10u);
    size_t o = 0;
#define NEED(n) do { if (o + (n) > cap) return 0; } while (0)
    NEED(14);
    memcpy(out, "MThd", 4);
    put_be32(out + 4, 6);
    out[8] = 0; out[9] = 1;                 /* format 1 */
    out[10] = 0; out[11] = 1 + REC_PARTS;   /* conductor + parts */
    out[12] = SMF_PPQ >> 8; out[13] = SMF_PPQ & 0xFF;
    o = 14;
    /* conductor track: tempo, time signature, key signature (as the original export) */
    {
        uint8_t d = 0;
        while ((1u << d) < s->denominator) d++;
        const uint8_t trk[] = {0x00, 0xFF, 0x51, 0x03, (uint8_t)(us_per_q >> 16), (uint8_t)(us_per_q >> 8), (uint8_t)us_per_q,
                               0x00, 0xFF, 0x58, 0x04, s->numerator, d, 24, 8,
                               0x00, 0xFF, 0x59, 0x02, 0x00, 0x00,
                               0x00, 0xFF, 0x2F, 0x00};
        NEED(8 + sizeof trk);
        memcpy(out + o, "MTrk", 4);
        put_be32(out + o + 4, sizeof trk);
        memcpy(out + o + 8, trk, sizeof trk);
        o += 8 + sizeof trk;
    }
    for (uint8_t part = 0; part < REC_PARTS; part++) {
        NEED(8);
        size_t hdr = o;
        memcpy(out + o, "MTrk", 4);
        o += 8;
        uint32_t last_tick = 0;
        for (uint32_t i = 0; i < s->count; i++) {
            const rec_event_t *e = &r->pool[s->first + i];
            if (e->part != part)
                continue;
            uint32_t tick = (uint32_t)((uint64_t)e->ms * 1000u * SMF_PPQ / us_per_q);
            NEED(8);
            o += put_vlq(out + o, tick - last_tick);
            last_tick = tick;
            out[o++] = (uint8_t)((e->status & 0xF0) | (s->ext ? (e->status & 0x0F) : part)); /* part -> channel */
            out[o++] = e->d1;
            if ((e->status & 0xE0) != 0xC0)
                out[o++] = e->d2;
        }
        NEED(4);
        out[o++] = 0; out[o++] = 0xFF; out[o++] = 0x2F; out[o++] = 0;
        put_be32(out + hdr + 4, (uint32_t)(o - hdr - 8));
    }
#undef NEED
    return o;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static bool get_vlq(const uint8_t *p, size_t end, size_t *i, uint32_t *v)
{
    *v = 0;
    for (int k = 0; k < 4; k++) {
        if (*i >= end)
            return false;
        uint8_t b = p[(*i)++];
        *v = (*v << 7) | (b & 0x7F);
        if (!(b & 0x80))
            return true;
    }
    return false;
}

#define SMF_TEMPO_MAX 128

typedef struct {
    uint32_t tick;
    uint32_t us_per_q;
} smf_tempo_t;

/* Walks one track; with tm == NULL collects FF 51 into the tempo map, else
 * appends channel events (ms via the map) to the song. */
static bool smf_track(recorder_t *r, rec_song_t *s, const uint8_t *d, size_t i, size_t end, uint16_t div,
                      smf_tempo_t *map, int *nmap, bool collect)
{
    uint32_t tick = 0;
    uint8_t run = 0;
    rec_event_t *base = &r->pool[s->first];
    while (i < end) {
        uint32_t dt;
        if (!get_vlq(d, end, &i, &dt))
            return false;
        tick += dt;
        if (i >= end)
            return false;
        uint8_t st = d[i];
        if (st & 0x80) i++; else st = run;
        if (st == 0xFF) {
            if (i + 1 > end) return false;
            uint8_t type = d[i++];
            uint32_t l;
            if (!get_vlq(d, end, &i, &l) || i + l > end) return false;
            if (collect && type == 0x51 && l == 3 && *nmap < SMF_TEMPO_MAX) {
                int k = *nmap;
                while (k > 0 && map[k - 1].tick > tick) { map[k] = map[k - 1]; k--; }
                map[k] = (smf_tempo_t){tick, (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | d[i + 2]};
                (*nmap)++;
            }
            if (!collect && type == 0x58 && l >= 2) { s->numerator = d[i]; s->denominator = (uint8_t)(1u << (d[i + 1] & 7)); }
            if (type == 0x2F) return true;
            i += l;
            continue;
        }
        if (st == 0xF0 || st == 0xF7) {
            uint32_t l;
            if (!get_vlq(d, end, &i, &l) || i + l > end) return false;
            i += l;
            continue;
        }
        if (!(st & 0x80) || st >= 0xF0) return false;
        run = st;
        if (i >= end) return false;
        uint8_t d1 = d[i++], d2 = 0;
        if ((st & 0xE0) != 0xC0) {
            if (i >= end) return false;
            d2 = d[i++];
        }
        if (collect)
            continue;
        /* tick -> microseconds through the tempo map */
        uint64_t us = 0;
        uint32_t t0 = 0, tempo = 500000;
        for (int k = 0; k < *nmap && map[k].tick <= tick; k++) {
            us += (uint64_t)(map[k].tick - t0) * tempo / div;
            t0 = map[k].tick;
            tempo = map[k].us_per_q;
        }
        us += (uint64_t)(tick - t0) * tempo / div;
        if (r->pool_used >= REC_MAX_EVENTS) return false;
        /* insertion keeps the song sorted by time across tracks */
        uint32_t ms = (uint32_t)(us / 1000u);
        uint32_t k = s->count;
        while (k > 0 && base[k - 1].ms > ms) { base[k] = base[k - 1]; k--; }
        base[k] = (rec_event_t){ms, st, d1, d2, (uint8_t)((st & 0x0F) == 1 ? 1 : 0)};
        s->count++;
        r->pool_used++;
    }
    return true;
}

bool smf_read(recorder_t *r, uint8_t song, const uint8_t *d, size_t len)
{
    if (song >= REC_SLOTS || len < 14 || memcmp(d, "MThd", 4) != 0)
        return false;
    uint16_t ntrk = (uint16_t)(d[10] << 8 | d[11]);
    uint16_t div = (uint16_t)(d[12] << 8 | d[13]);
    if (div & 0x8000 || div == 0)
        return false; /* SMPTE timing not supported */
    if (r->state != REC_IDLE && r->cur_song == song)
        recorder_stop(r);
    recorder_erase(r, song, -1);
    rec_song_t *s = &r->song[song];
    /* the new song goes to the end of the pool */
    s->first = r->pool_used;
    s->count = 0;
    s->ext = false;
    static smf_tempo_t map[SMF_TEMPO_MAX];
    int nmap = 0;
    for (int pass = 0; pass < 2; pass++) {
        size_t pos = 8 + rd32(d + 4);
        for (uint16_t t = 0; t < ntrk && pos + 8 <= len; t++) {
            if (memcmp(d + pos, "MTrk", 4) != 0)
                goto bad;
            size_t end = pos + 8 + rd32(d + pos + 4);
            if (end > len)
                goto bad;
            if (!smf_track(r, s, d, pos + 8, end, div, map, &nmap, pass == 0))
                goto bad;
            pos = end;
        }
    }
    s->bpm = (uint16_t)(60000000u / (nmap ? map[0].us_per_q : 500000u));
    s->length_ms = s->count ? r->pool[s->first + s->count - 1].ms : 0;
    mark_dirty(r, song);
    return true;
bad:
    r->pool_used -= s->count;
    s->count = 0;
    s->length_ms = 0;
    mark_dirty(r, song);
    return false;
}
