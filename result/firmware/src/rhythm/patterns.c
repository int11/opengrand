/* OpenGrand drum rhythm patterns.
 *
 * Every pattern here is an original, deliberately simple groove written for
 * OpenGrand. The original firmware's pattern block (0x2A0000, 100 entries) is
 * not decoded or reused. A pattern is built from the rhythm's category (from
 * RHYTHM_TABLE, i.e. the manual's rhythm list) and varied by the rhythm's
 * position inside its category, so the 100 entries are distinct and stay in
 * their style family. */
#include <string.h>

#include "ca79ng/params_gen.h"
#include "ca79ng/rhythm.h"
#include "ca79ng/sound.h"

#define B(s) (1u << (s))

/* every n-th step from `from` up to `steps` */
static uint32_t every(unsigned from, unsigned n, unsigned steps)
{
    uint32_t m = 0;
    for (unsigned s = from; s < steps; s += n)
        m |= B(s);
    return m;
}

/* repeat a one-beat mask over all beats */
static uint32_t per_beat(uint32_t beat_mask, unsigned spb, unsigned beats)
{
    uint32_t m = 0;
    for (unsigned b = 0; b < beats; b++)
        m |= beat_mask << (b * spb);
    return m;
}

static bool cat(const char *c, const char *want) { return c && strcmp(c, want) == 0; }

static void eight_beat(drum_pattern_t *p, unsigned v, bool ballad, bool rock)
{
    unsigned n = p->beats * 2;
    p->steps_per_beat = 2;
    p->hit[DRUM_KICK] = B(0) | (p->beats >= 3 ? B(4) : 0);
    p->hit[ballad ? DRUM_RIM : DRUM_SNARE] = every(2, 4, n);
    p->hit[(v & 4) ? DRUM_RIDE : DRUM_HAT] = every(0, 1, n);
    p->accent = every(0, 2, n);
    if (v % 3 == 1) p->hit[DRUM_KICK] |= B(5 % n);
    if (v % 3 == 2) p->hit[DRUM_KICK] |= B(3 % n);
    if (v & 8) { p->hit[DRUM_OHAT] |= B(n - 1); p->hit[DRUM_HAT] &= ~B(n - 1); }
    if (ballad && (v & 1)) { p->hit[DRUM_SNARE] = p->hit[DRUM_RIM]; p->hit[DRUM_RIM] = 0; }
    if (rock) {
        p->hit[DRUM_KICK] |= B(5 % n) | B(1);
        p->accent |= p->hit[DRUM_SNARE];
        if (v & 1) { p->hit[DRUM_RIDE] |= every(0, 2, n); p->hit[DRUM_HAT] &= ~every(0, 2, n); }
    }
}

static void sixteen_beat(drum_pattern_t *p, unsigned v, bool ballad, bool dance)
{
    unsigned n = p->beats * 4;
    p->steps_per_beat = 4;
    if (dance) {
        p->hit[DRUM_KICK] = every(0, 4, n);
        p->hit[DRUM_SNARE] = every(4, 8, n);
        p->hit[DRUM_OHAT] = every(2, 4, n);
        p->hit[DRUM_HAT] = every(0, 1, n) & ~p->hit[DRUM_OHAT];
        if (v & 1) p->hit[DRUM_SNARE] |= B(n - 1);
        if (v & 2) p->hit[DRUM_RIM] = every(3, 8, n);
    } else {
        static const uint32_t kicks[4] = {B(0) | B(6) | B(8), B(0) | B(7) | B(10), B(0) | B(3) | B(8) | B(11), B(0) | B(10) | B(14)};
        p->hit[DRUM_KICK] = kicks[v % 4] & every(0, 1, n);
        p->hit[ballad ? DRUM_RIM : DRUM_SNARE] = every(4, 8, n);
        p->hit[DRUM_HAT] = ballad ? (every(0, 2, n) | ((v & 1) ? B(n - 1) : 0)) : every(0, 1, n);
        if (v & 4) { p->hit[DRUM_RIDE] = p->hit[DRUM_HAT]; p->hit[DRUM_HAT] = 0; }
        if ((v & 8) && !ballad) p->hit[DRUM_SNARE] |= B(n - 2);
    }
    p->accent = every(0, 2, n);
}

/* swing on a triplet grid: spb 3 (8th swing) or 6 (16th swing) */
static void swing(drum_pattern_t *p, unsigned v, unsigned spb, bool triplet)
{
    unsigned n = p->beats * spb;
    p->steps_per_beat = (uint8_t)spb;
    uint32_t beat = triplet ? (spb == 3 ? (B(0) | B(1) | B(2)) : 0x3F) : (spb == 3 ? (B(0) | B(2)) : (B(0) | B(2) | B(3) | B(5)));
    p->hit[(v & 2) ? DRUM_RIDE : DRUM_HAT] = per_beat(beat, spb, p->beats) & every(0, 1, n);
    p->hit[DRUM_KICK] = B(0) | (p->beats >= 3 ? B(2 * spb) : 0);
    p->hit[DRUM_SNARE] = every(spb, 2 * spb, n);
    if (v & 1) p->hit[DRUM_KICK] |= B((2 * spb + spb - 1) % n);
    if (v & 4) p->hit[DRUM_OHAT] = B(n - 1);
    p->accent = every(0, spb, n);
}

static void jazz(drum_pattern_t *p, unsigned v)
{
    unsigned spb = 3, n = p->beats * spb;
    p->steps_per_beat = 3;
    /* ride: on every beat plus the swung "and" before beats 2 and 4 */
    uint32_t ride = every(0, spb, n);
    for (unsigned b = 1; b < p->beats; b += 2)
        ride |= B(b * spb - 1);
    p->hit[DRUM_RIDE] = ride;
    p->hit[DRUM_HAT] = every(spb, 2 * spb, n); /* foot hi-hat on 2 and 4 */
    p->hit[DRUM_KICK] = (v & 1) ? every(0, spb, n) : B(0);
    if (v & 2) p->hit[DRUM_RIM] = B((p->beats - 1) * spb + 2);
    if (v & 4) { p->hit[DRUM_HAT] = 0; p->hit[DRUM_RIM] |= every(spb, 2 * spb, n); }
    p->accent = every(spb, 2 * spb, n);
}

static void latin(drum_pattern_t *p, unsigned v)
{
    if (p->beats == 3) { /* waltz: bass on 1, brushes on 2 and 3 */
        unsigned n = 6;
        p->steps_per_beat = 2;
        p->hit[DRUM_KICK] = B(0);
        p->hit[DRUM_RIM] = B(2) | B(4);
        p->hit[DRUM_HAT] = every(0, 2, n) | ((v & 1) ? B(5) : 0);
        p->accent = B(0);
        return;
    }
    if (p->denominator == 8) { /* compound march */
        unsigned n = p->beats * 2;
        p->steps_per_beat = 2;
        p->hit[DRUM_KICK] = B(0) | B(6 % n);
        p->hit[DRUM_SNARE] = every(0, 2, n) | B(5) | B(11 % n);
        p->accent = B(0) | B(6 % n);
        return;
    }
    unsigned n = p->beats * 4;
    p->steps_per_beat = 4;
    switch (v % 8) {
    case 0: /* bossa-like: kick dotted pairs, rim on a 3-3-4 / 2 clave feel */
        p->hit[DRUM_KICK] = every(0, 8, n) | every(3, 8, n) | every(4, 8, n) | every(7, 8, n);
        p->hit[DRUM_RIM] = (B(0) | B(3) | B(6) | B(10) | B(13)) & every(0, 1, n);
        p->hit[(v & 8) ? DRUM_RIDE : DRUM_HAT] = every(0, 2, n);
        break;
    case 1: /* slow latin ballroom: kick 1 and 3, rim on the "and" of 2 */
        p->hit[DRUM_KICK] = every(0, 8, n);
        p->hit[DRUM_RIM] = every(6, 8, n);
        p->hit[DRUM_HAT] = every(0, 2, n);
        break;
    case 2: /* cha-cha feel: cowbell-ish hat on beats, two quick hits on 4 */
        p->hit[DRUM_KICK] = every(0, 8, n);
        p->hit[DRUM_SNARE] = B(n - 4) | B(n - 2);
        p->hit[DRUM_HAT] = every(0, 4, n);
        p->hit[DRUM_RIDE] = every(2, 4, n);
        break;
    case 3: /* mambo-like: syncopated kick, rim on 2 and 4 offsets */
        p->hit[DRUM_KICK] = every(3, 8, n) | every(4, 16, n);
        p->hit[DRUM_RIM] = every(4, 8, n) | every(14, 16, n);
        p->hit[DRUM_RIDE] = every(0, 2, n);
        break;
    case 4: /* samba-like: driving 16ths, kick on every beat with pickup */
        p->hit[DRUM_KICK] = every(0, 4, n) | every(3, 4, n);
        p->hit[DRUM_HAT] = every(0, 1, n);
        p->hit[DRUM_RIM] = every(2, 8, n) | every(7, 16, n);
        break;
    case 5: /* tango-like: four marked beats, last one leaning */
        p->hit[DRUM_KICK] = every(0, 4, n);
        p->hit[DRUM_SNARE] = B(n - 4) | B(n - 3);
        p->hit[DRUM_HAT] = every(2, 4, n);
        break;
    case 6: /* habanera / dotted feel */
        p->hit[DRUM_KICK] = every(0, 8, n);
        p->hit[DRUM_RIM] = every(3, 8, n) | every(4, 8, n) | every(6, 8, n);
        p->hit[DRUM_HAT] = every(0, 2, n);
        break;
    default: /* two-beat (ragtime / march) */
        p->hit[DRUM_KICK] = every(0, 8, n);
        p->hit[DRUM_SNARE] = every(4, 8, n);
        p->hit[DRUM_HAT] = every(0, 2, n);
        if (v & 8) p->hit[DRUM_SNARE] |= B(n - 1);
        break;
    }
    p->accent = every(0, 4, n);
}

void rhythm_pattern(unsigned index, drum_pattern_t *p)
{
    memset(p, 0, sizeof *p);
    index %= RHYTHM_COUNT_GEN;
    const rhythm_desc_t *r = &RHYTHM_TABLE[index];
    p->beats = r->numerator ? r->numerator : 4;
    p->denominator = r->denominator ? r->denominator : 4;
    /* position within the category */
    unsigned v = 0;
    for (unsigned i = 0; i < index; i++)
        if (strcmp(RHYTHM_TABLE[i].category, r->category) == 0)
            v++;
    const char *c = r->category;
    if (cat(c, "8 Beat")) eight_beat(p, v, false, false);
    else if (cat(c, "8 Beat Ballad")) eight_beat(p, v, true, false);
    else if (cat(c, "8 Beat Rock")) eight_beat(p, v, false, true);
    else if (cat(c, "16 Beat")) sixteen_beat(p, v, false, false);
    else if (cat(c, "16 Beat Ballad")) sixteen_beat(p, v, true, false);
    else if (cat(c, "16 Beat Dance")) sixteen_beat(p, v, false, true);
    else if (cat(c, "16 Beat Swing")) swing(p, v, 6, false);
    else if (cat(c, "8 Beat Swing")) swing(p, v, 3, false);
    else if (cat(c, "Triplet")) swing(p, v, 3, true);
    else if (cat(c, "Jazz")) jazz(p, v);
    else latin(p, v);
    /* keep everything inside the bar */
    unsigned n = (unsigned)p->steps_per_beat * p->beats;
    uint32_t mask = n >= 32 ? 0xFFFFFFFFu : (B(n) - 1);
    for (int k = 0; k < DRUM_PATTERN_INSTS; k++)
        p->hit[k] &= mask;
    p->accent &= mask;
}

uint8_t rhythm_hit(const drum_pattern_t *pt, unsigned inst, unsigned step)
{
    if (inst >= DRUM_PATTERN_INSTS || step >= 32 || !(pt->hit[inst] & B(step)))
        return 0;
    static const uint8_t base[DRUM_PATTERN_INSTS] = {110, 100, 90, 70, 80, 85};
    int v = base[inst] + ((pt->accent & B(step)) ? 17 : 0);
    return (uint8_t)(v > 127 ? 127 : v);
}
