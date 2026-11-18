#include "ca79ng/concert.h"

#include <string.h>

#include "ca79ng/util.h"

bool cm_is_trigger(const concert_t *c, const rec_event_t *e)
{
    if ((e->status & 0xF0) != 0x90 || e->d2 == 0)
        return false;
    return c->ext ? (e->status & 0x0F) == c->trigger : e->part == c->trigger;
}

/* first trigger note-on at or after index i */
static uint32_t find_trigger(const concert_t *c, const rec_event_t *ev, uint32_t count, uint32_t i)
{
    while (i < count && !cm_is_trigger(c, &ev[i]))
        i++;
    return i;
}

static void update_next(concert_t *c, const rec_event_t *ev, uint32_t count)
{
    uint32_t i = find_trigger(c, ev, count, c->next);
    c->next_ms = i < count ? ev[i].ms : UINT32_MAX;
}

void cm_start(concert_t *c, const rec_event_t *ev, uint32_t count, bool ext)
{
    uint8_t mode = c->mode, trig = c->trigger;
    bool smooth = c->smooth;
    memset(c, 0, sizeof *c);
    c->mode = mode;
    c->trigger = trig;
    c->smooth = smooth;
    c->ext = ext;
    c->active = true;
    c->ratio_q16 = 65536;
    c->last = CM_OK;
    update_next(c, ev, count);
}

void cm_stop(concert_t *c) { c->active = false; }

void cm_key_off(concert_t *c, uint8_t key, cm_emit_fn emit, void *ctx)
{
    cm_held_t *h = &c->held[key & (CM_KEYS - 1)];
    for (uint8_t i = 0; i < h->n; i++)
        emit(ctx, &h->ev[i], 0);
    h->n = 0;
}

/* stop a note that another key still holds (re-strike) */
static void release_note(concert_t *c, const rec_event_t *e, cm_emit_fn emit, void *ctx)
{
    for (int k = 0; k < CM_KEYS; k++) {
        cm_held_t *h = &c->held[k];
        for (uint8_t i = 0; i < h->n; i++) {
            if (h->ev[i].d1 == e->d1 && h->ev[i].status == e->status) {
                emit(ctx, &h->ev[i], 0);
                h->ev[i] = h->ev[--h->n];
                return;
            }
        }
    }
}

cm_result_t cm_key_on(concert_t *c, const rec_event_t *ev, uint32_t count, uint8_t key, uint8_t vel,
                      uint32_t now, cm_emit_fn emit, void *ctx)
{
    if (!c->active)
        return c->last = CM_NG;
    uint32_t first = find_trigger(c, ev, count, c->next);
    if (first >= count)
        return c->last = CM_NG; /* end of song */
    uint32_t onset = ev[first].ms;

    /* collect the group */
    rec_event_t grp[CM_GROUP_MAX];
    uint8_t n = 0, loudest = 1;
    uint32_t i = first, last = first;
    for (; i < count && ev[i].ms <= onset + CM_CHORD_MS; i++) {
        if (!cm_is_trigger(c, &ev[i]))
            continue;
        if (n < CM_GROUP_MAX) {
            grp[n++] = ev[i];
            if (ev[i].d2 > loudest)
                loudest = ev[i].d2;
        }
        last = i;
    }
    c->next = last + 1;

    /* tap tempo */
    cm_result_t res = CM_OK;
    if (c->tapped && onset > c->group_ms) {
        uint32_t expected = onset - c->group_ms;            /* song ms */
        uint32_t interval = CA_MAX(now - c->last_tap_ms, 1u); /* real ms */
        uint64_t raw = ((uint64_t)expected << 16) / interval; /* >1: player faster than the song */
        if (raw > 2 * 65536u)
            res = CM_TOOFAST;
        else if (raw > 81920u) /* > 1.25 */
            res = CM_FAST;
        else if (raw < 52429u) /* < 0.8 */
            res = CM_SLOW;
        if (raw < 32768u) raw = 32768u;  /* clamp 0.5 .. 1.5 */
        if (raw > 98304u) raw = 98304u;
        /* blend: Melody follows the player directly, Ensemble halves the
         * step, "smooth" quarters it */
        uint32_t w = c->smooth ? 16384u : (c->mode == CM_MODE_ENSEMBLE ? 32768u : 65536u);
        c->ratio_q16 = (uint32_t)(((uint64_t)c->ratio_q16 * (65536u - w) + raw * w) >> 16);
    }
    if (res == CM_OK && n > 1)
        res = CM_CHORD;
    c->tapped = true;
    c->last_tap_ms = now;
    c->group_ms = onset;
    c->groups++;

    /* sound the group: the key velocity goes to the loudest note, the others
     * keep their balance relative to it */
    cm_held_t *h = &c->held[key & (CM_KEYS - 1)];
    cm_key_off(c, key, emit, ctx);
    for (uint8_t k = 0; k < n; k++) {
        release_note(c, &grp[k], emit, ctx);
        uint8_t v = (uint8_t)ca_clampi(vel * grp[k].d2 / loudest, 1, 127);
        emit(ctx, &grp[k], v);
        if (h->n < CM_GROUP_MAX)
            h->ev[h->n++] = grp[k];
    }
    update_next(c, ev, count);
    return c->last = res;
}
