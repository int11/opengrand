#include <string.h>

#include "ca79ng/sound.h"
#include "ca79ng/util.h"

/* ---------------- keyboard layout ---------------- */

void kb_layout_default(kb_layout_t *l)
{
    memset(l, 0, sizeof *l);
    l->mode = KBMODE_SINGLE;
    l->split_point = 54; /* F#3 */
    l->balance = 64;
    part_state_t base = {.msb = 95, .lsb = 112, .program = 0, .volume = 127, .enabled = true};
    l->main = l->layer = l->lower = l->right4h = l->left4h = base;
    l->right4h.octave_shift = -2;
    l->left4h.octave_shift = 2;
}

static int shift(const part_state_t *p, int note)
{
    int n = note + p->octave_shift * 12 + p->transpose;
    return (n < 0 || n > 127) ? -1 : n;
}

/* balance 0..127 -> (a, b) volumes with 64 = both full */
static void balance_pair(uint8_t bal, uint8_t *a, uint8_t *b)
{
    *a = bal <= 64 ? 127 : (uint8_t)(127 - (bal - 64) * 2);
    *b = bal >= 64 ? 127 : (uint8_t)(127 - (64 - bal) * 2);
}

int kb_layout_route(const kb_layout_t *l, int note, part_route_t out[2])
{
    uint8_t va, vb;
    balance_pair(l->balance, &va, &vb);
    switch (l->mode) {
    case KBMODE_DUAL:
        out[0] = (part_route_t){PART_MAIN1, (int8_t)shift(&l->main, note), va};
        out[1] = (part_route_t){PART_LAYER, (int8_t)shift(&l->layer, note), vb};
        return 2;
    case KBMODE_SPLIT:
        if (note < l->split_point)
            out[0] = (part_route_t){PART_LOWER, (int8_t)shift(&l->lower, note), vb};
        else
            out[0] = (part_route_t){PART_MAIN1, (int8_t)shift(&l->main, note), va};
        return 1;
    case KBMODE_4HANDS:
        if (note < l->split_point)
            out[0] = (part_route_t){PART_4H_LEFT, (int8_t)shift(&l->left4h, note), vb};
        else
            out[0] = (part_route_t){PART_4H_RIGHT, (int8_t)shift(&l->right4h, note), va};
        return 1;
    default:
        out[0] = (part_route_t){PART_MAIN1, (int8_t)shift(&l->main, note), 127};
        return 1;
    }
}

/* ---------------- voice pool ---------------- */

void voice_pool_init(voice_pool_t *p, sound_engine_t *engine)
{
    memset(p, 0, sizeof *p);
    p->engine = engine;
}

static int pick_voice(voice_pool_t *p)
{
    int best = -1;
    uint32_t best_age = UINT32_MAX;
    int best_rank = 4;
    for (int i = 0; i < VOICE_MAX; i++) {
        voice_t *v = &p->v[i];
        if (!v->active)
            return i;
        /* steal order: released & not sustained < sustained < held; oldest first */
        int rank = v->key_down ? 3 : (v->sustained || v->sostenuto ? 2 : 1);
        if (rank < best_rank || (rank == best_rank && v->age < best_age)) {
            best = i;
            best_rank = rank;
            best_age = v->age;
        }
    }
    return best;
}

int voice_note_on(voice_pool_t *p, uint8_t part, uint8_t note, uint8_t velocity)
{
    /* re-strike of a sounding note on the same part: release the old voice
     * (an acoustic string is damped and re-struck) */
    for (int i = 0; i < VOICE_MAX; i++) {
        voice_t *v = &p->v[i];
        if (v->active && v->part == part && v->note == note) {
            if (p->engine && p->engine->release)
                p->engine->release(p->engine->ctx, i);
            v->key_down = false;
            v->sustained = false;
            v->sostenuto = false;
        }
    }
    int i = pick_voice(p);
    voice_t *v = &p->v[i];
    if (v->active) {
        p->stolen++;
        if (p->engine && p->engine->kill)
            p->engine->kill(p->engine->ctx, i);
    }
    *v = (voice_t){.active = true, .key_down = true, .part = part, .note = note,
                   .velocity = velocity, .age = ++p->clock};
    return i;
}

void voice_note_off(voice_pool_t *p, uint8_t part, uint8_t note)
{
    uint8_t pi = part & 15;
    for (int i = 0; i < VOICE_MAX; i++) {
        voice_t *v = &p->v[i];
        if (!v->active || !v->key_down || v->part != part || v->note != note)
            continue;
        v->key_down = false;
        if (v->sostenuto || p->damper[pi] >= 64) {
            v->sustained = true;
            if (!v->sostenuto && p->engine && p->engine->sustain)
                p->engine->sustain(p->engine->ctx, i, p->damper_hold);
            continue;
        }
        if (p->engine && p->engine->release)
            p->engine->release(p->engine->ctx, i);
    }
}

void voice_pedal(voice_pool_t *p, uint8_t part, uint8_t cc, uint8_t value)
{
    uint8_t pi = part & 15;
    switch (cc) {
    case 64: {
        uint8_t was = p->damper[pi];
        p->damper[pi] = value;
        if (was >= 64 && value < 64) { /* damper released: let go of sustained notes */
            for (int i = 0; i < VOICE_MAX; i++) {
                voice_t *v = &p->v[i];
                if (v->active && v->part == part && v->sustained && !v->sostenuto && !v->key_down) {
                    v->sustained = false;
                    if (p->engine && p->engine->release)
                        p->engine->release(p->engine->ctx, i);
                }
            }
        }
        break;
    }
    case 66: {
        bool on = value >= 64;
        if (on && !p->sostenuto[pi]) { /* capture keys held right now */
            for (int i = 0; i < VOICE_MAX; i++) {
                voice_t *v = &p->v[i];
                if (v->active && v->part == part && v->key_down)
                    v->sostenuto = true;
            }
        } else if (!on && p->sostenuto[pi]) {
            for (int i = 0; i < VOICE_MAX; i++) {
                voice_t *v = &p->v[i];
                if (v->active && v->part == part && v->sostenuto) {
                    v->sostenuto = false;
                    if (!v->key_down && p->damper[pi] < 64) {
                        v->sustained = false;
                        if (p->engine && p->engine->release)
                            p->engine->release(p->engine->ctx, i);
                    }
                }
            }
        }
        p->sostenuto[pi] = on;
        break;
    }
    case 67:
        p->soft[pi] = value;
        break;
    default:
        break;
    }
    /* half-pedal values are forwarded so the engine can shorten the release */
    if (p->engine && p->engine->control)
        p->engine->control(p->engine->ctx, part, cc, value);
}

void voice_all_off(voice_pool_t *p, bool immediate)
{
    for (int i = 0; i < VOICE_MAX; i++) {
        voice_t *v = &p->v[i];
        if (!v->active)
            continue;
        if (p->engine) {
            if (immediate && p->engine->kill)
                p->engine->kill(p->engine->ctx, i);
            else if (p->engine->release)
                p->engine->release(p->engine->ctx, i);
        }
        if (immediate)
            v->active = false;
        else
            v->key_down = v->sustained = v->sostenuto = false;
    }
    memset(p->damper, 0, sizeof p->damper);
    memset(p->sostenuto, 0, sizeof p->sostenuto);
}

void voice_release_finished(voice_pool_t *p, int index)
{
    if (index >= 0 && index < VOICE_MAX && !p->v[index].key_down)
        p->v[index].active = false;
}

void voice_set_damper_hold(voice_pool_t *p, bool on)
{
    p->damper_hold = on;
    for (int i = 0; i < VOICE_MAX; i++) {
        voice_t *v = &p->v[i];
        if (v->active && v->sustained && !v->sostenuto && !v->key_down && p->engine && p->engine->sustain)
            p->engine->sustain(p->engine->ctx, i, on);
    }
}

int voice_active_count(const voice_pool_t *p)
{
    int n = 0;
    for (int i = 0; i < VOICE_MAX; i++)
        n += p->v[i].active;
    return n;
}
