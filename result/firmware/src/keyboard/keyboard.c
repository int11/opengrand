#include "ca79ng/keyboard.h"

#include <math.h>
#include <string.h>

#include "ca79ng/util.h"

/* ---------------- touch ---------------- */

void touch_config_default(touch_config_t *c)
{
    memset(c, 0, sizeof *c);
    c->curve = TC_NORMAL;
    c->constant_velocity = 100;
    c->minimum_touch = 1;
    c->key_volume = KV_NORMAL;
    c->dynamics = 127;
    /* user curve starts as identity */
    static const uint8_t in[] = {1, 16, 32, 48, 64, 80, 104, 127};
    memcpy(c->user_in, in, sizeof in);
    memcpy(c->user_out, in, sizeof in);
    c->user_points = TC_USER_POINTS;
}

/* Exponent per preset: <1 = lighter (louder for the same force). Light4 is
 * the lightest, Heavy4 the heaviest; Normal is linear. */
static float curve_gamma(touch_curve_t tc)
{
    switch (tc) {
    case TC_LIGHT4: return 0.45f;
    case TC_LIGHT3: return 0.58f;
    case TC_LIGHT2: return 0.72f;
    case TC_LIGHT1: return 0.86f;
    case TC_HEAVY1: return 1.16f;
    case TC_HEAVY2: return 1.34f;
    case TC_HEAVY3: return 1.55f;
    case TC_HEAVY4: return 1.80f;
    default: return 1.0f;
    }
}

static uint8_t user_curve(const touch_config_t *c, uint8_t v)
{
    if (c->user_table_valid)
        return c->user_table[v & 127] ? c->user_table[v & 127] : 1;
    if (c->user_points < 2)
        return v;
    if (v <= c->user_in[0])
        return c->user_out[0];
    for (int i = 1; i < c->user_points; i++) {
        if (v <= c->user_in[i]) {
            int x0 = c->user_in[i - 1], x1 = c->user_in[i];
            int y0 = c->user_out[i - 1], y1 = c->user_out[i];
            if (x1 == x0)
                return (uint8_t)y1;
            return (uint8_t)(y0 + (y1 - y0) * (v - x0) / (x1 - x0));
        }
    }
    return c->user_out[c->user_points - 1];
}

/* Preset key-volume shaping: attenuation (dB) as a function of key position. */
static float key_volume_db(const touch_config_t *c, int key)
{
    float x = (float)key / (KB_KEYS - 1); /* 0 = A0, 1 = C8 */
    switch (c->key_volume) {
    case KV_HIGH_DAMP: return x > 0.5f ? -6.0f * (x - 0.5f) * 2.0f : 0.0f;
    case KV_LOW_DAMP: return x < 0.5f ? -6.0f * (0.5f - x) * 2.0f : 0.0f;
    case KV_HIGH_LOW_DAMP: return -6.0f * fabsf(x - 0.5f) * 2.0f;
    case KV_CENTER_DAMP: return -4.0f * (1.0f - fabsf(x - 0.5f) * 2.0f);
    case KV_USER: return (float)c->per_note_volume[key] * 0.5f;
    default: return 0.0f;
    }
}

uint8_t touch_apply(const touch_config_t *c, int key, uint8_t raw)
{
    if (raw == 0)
        return 0;
    /* Minimum Touch 1..20: strokes softer than the threshold are silent. */
    int threshold = (c->minimum_touch > 1) ? (c->minimum_touch - 1) * 2 : 0;
    if (raw <= threshold)
        return 0;

    float v;
    if (c->curve == TC_OFF) {
        v = c->constant_velocity;
    } else if (c->curve == TC_USER) {
        v = user_curve(c, raw);
    } else {
        v = 127.0f * powf(raw / 127.0f, curve_gamma(c->curve));
    }
    v *= powf(10.0f, key_volume_db(c, ca_clampi(key, 0, KB_KEYS - 1)) / 20.0f);
    v *= c->dynamics / 127.0f;
    return (uint8_t)ca_clampi((int)lrintf(v), 1, 127);
}

/* ---------------- scanner ---------------- */

void key_scanner_init(key_scanner_t *s)
{
    memset(s, 0, sizeof *s);
    s->t_fast_us = 1500;   /* ~fortissimo */
    s->t_slow_us = 120000; /* ~ppp */
}

uint8_t key_scanner_velocity(const key_scanner_t *s, uint32_t travel_us)
{
    if (travel_us <= s->t_fast_us)
        return 127;
    if (travel_us >= s->t_slow_us)
        return 1;
    /* velocity ~ 1/time: log mapping gives an even feel across the range */
    float lf = logf((float)s->t_fast_us), ls = logf((float)s->t_slow_us);
    float x = (ls - logf((float)travel_us)) / (ls - lf);
    return (uint8_t)ca_clampi((int)lrintf(1.0f + x * 126.0f), 1, 127);
}

key_event_t key_scanner_edge(key_scanner_t *s, uint8_t key, uint8_t sensor, bool closed, uint32_t t)
{
    key_event_t ev = {KEY_EV_NONE, key, 0};
    if (key >= KB_KEYS)
        return ev;
    key_state_t *st = &s->state[key];

    switch (sensor) {
    case 1:
        if (closed) {
            if (*st == KS_IDLE) {
                *st = KS_S1;
                s->t_s1[key] = t;
            }
        } else {
            /* key fully returned: release */
            if (*st == KS_DOWN || *st == KS_HELD) {
                ev.kind = KEY_EV_OFF;
                ev.velocity = 64;
            }
            *st = KS_IDLE;
        }
        break;
    case 2:
        if (closed) {
            if (*st == KS_S1 || *st == KS_HELD) {
                /* KS_HELD -> S2 again without S1 release = repetition; the
                 * travel time is measured from the S2 release point. */
                ev.kind = KEY_EV_ON;
                ev.velocity = key_scanner_velocity(s, t - s->t_s1[key]);
                *st = KS_DOWN;
            }
        } else if (*st == KS_DOWN) {
            *st = KS_HELD;
            s->t_s1[key] = t; /* repetition timing reference */
        }
        break;
    case 3: /* let-off / key bottom: informational, used by action diagnostics */
    default:
        break;
    }
    return ev;
}

/* ---------------- pedals ---------------- */

void pedal_config_default(pedal_config_t *p)
{
    memset(p, 0, sizeof *p);
    for (int i = 0; i < PEDAL_COUNT; i++) {
        p->adc_min[i] = 200;
        p->adc_max[i] = 3800;
    }
    p->half_pedal_adjust = 5;
    p->soft_depth = 5;
}

uint8_t pedal_value(const pedal_config_t *p, pedal_id_t id, uint16_t adc)
{
    int lo = p->adc_min[id], hi = p->adc_max[id];
    if (hi <= lo)
        return 0;
    float x = (float)(ca_clampi(adc, lo, hi) - lo) / (float)(hi - lo); /* 0..1 travel */

    if (id == PEDAL_SOSTENUTO)
        return x > 0.5f ? 127 : 0;

    if (id == PEDAL_DAMPER) {
        int hp = ca_clampi(p->half_pedal_adjust, 1, 10);
        if (hp <= 7) {
            /* 1..7: dead zone before dampers lift grows with the setting */
            float dead = 0.05f + (hp - 1) * 0.05f;
            x = x <= dead ? 0.0f : (x - dead) / (1.0f - dead);
        } else if (hp == 8) {
            /* MIDI-controller use: full value reached early */
            x = x * 1.6f;
        } /* 9..10: linear full travel */
        return (uint8_t)ca_clampi((int)lrintf(x * 127.0f), 0, 127);
    }

    /* soft pedal: plain travel; depth is applied by the engine (vt) */
    return (uint8_t)ca_clampi((int)lrintf(x * 127.0f), 0, 127);
}
