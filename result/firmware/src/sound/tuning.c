#include <math.h>
#include <string.h>

#include "ca79ng/sound.h"
#include "ca79ng/util.h"

/* Deviation from equal temperament in cents for each pitch class, written
 * relative to the temperament key = C. Standard published values. */
static const float TEMP_TABLE[TEMP_USER][12] = {
    [TEMP_EQUAL] = {0},
    /* 5-limit just intonation, major (C D E F G A B with chromatic fills) */
    [TEMP_PURE_MAJOR] = {0, -29.3f, 3.9f, 15.6f, -13.7f, -2.0f, -31.3f, 2.0f, -27.4f, -15.6f, 17.6f, -11.7f},
    /* just intonation, minor (C natural minor oriented) */
    [TEMP_PURE_MINOR] = {0, 11.7f, 3.9f, 15.6f, -13.7f, -2.0f, -31.3f, 2.0f, 13.7f, -15.6f, 17.6f, -11.7f},
    [TEMP_PYTHAGOREAN] = {0, -9.8f, 3.9f, -5.9f, 7.8f, -2.0f, -11.7f, 2.0f, -7.8f, 5.9f, -3.9f, 9.8f},
    /* quarter-comma meantone */
    [TEMP_MEANTONE] = {0, -24.0f, -6.8f, 10.3f, -13.7f, 3.4f, -20.5f, -3.4f, -27.4f, -10.3f, 6.8f, -17.1f},
    /* Werckmeister III */
    [TEMP_WERCKMEISTER] = {0, -9.8f, -7.8f, -5.9f, -9.8f, -2.0f, -11.7f, -3.9f, -7.8f, -11.7f, -3.9f, -7.8f},
    /* Kirnberger III */
    [TEMP_KIRNBERGER] = {0, -9.8f, -6.8f, -5.9f, -13.7f, -2.0f, -9.8f, -3.4f, -7.8f, -10.3f, -3.9f, -11.7f},
};

void tuning_default(tuning_t *t)
{
    memset(t, 0, sizeof *t);
    t->master_hz = 440.0f;
    t->temperament = TEMP_EQUAL;
    t->stretch = STRETCH_NORMAL;
}

/* Railsback-style stretch: flatter in the bass, sharper in the treble,
 * roughly cubic around A4. "Normal" peaks near ±30 cents at the extremes. */
static float stretch_cents(stretch_t s, int note, const tuning_t *t)
{
    if (s == STRETCH_USER)
        return t->per_note_cents[note & 127];
    float scale = (s == STRETCH_NORMAL) ? 1.0f : (s == STRETCH_WIDE ? 1.6f : 0.0f);
    if (scale == 0.0f)
        return 0.0f;
    float x = (note - 69) / 39.0f; /* -1.23 (A0) .. +1.0 (C8) */
    return scale * (x >= 0 ? 30.0f * x * x * x : 25.0f * x * x * x);
}

float tuning_cents(const tuning_t *t, int note)
{
    int pc = ((note - t->temperament_key) % 12 + 12) % 12;
    float c;
    if (t->temperament == TEMP_USER)
        c = t->user_temperament[pc];
    else if (t->temperament < TEMP_USER)
        c = TEMP_TABLE[t->temperament][pc];
    else
        c = 0.0f;
    /* keep the temperament key's A unshifted relative to master tuning */
    int pc_a = ((9 - t->temperament_key) % 12 + 12) % 12;
    float ref = (t->temperament == TEMP_USER) ? t->user_temperament[pc_a]
              : (t->temperament < TEMP_USER ? TEMP_TABLE[t->temperament][pc_a] : 0.0f);
    return (c - ref) + stretch_cents(t->stretch, note, t);
}

float tuning_freq(const tuning_t *t, int note)
{
    float semis = (float)(note + t->transpose - 69);
    return t->master_hz * powf(2.0f, (semis * 100.0f + tuning_cents(t, note + t->transpose)) / 1200.0f);
}

uint8_t tuning_hz_to_raw(float hz)
{
    return (uint8_t)ca_clampi((int)lrintf(2.0f * (hz - 440.0f) + 64.0f), 0, 127);
}

float tuning_raw_to_hz(uint8_t raw) { return 440.0f + ((int)raw - 64) / 2.0f; }
