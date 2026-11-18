/* OpenGrand — metronome / drum rhythm timing.
 *
 * Requirements: time signatures 1/4 2/4 3/4 4/4 5/4 3/8 6/8 7/8 9/8 12/8,
 * tempo 10..400 BPM (quarter) — eighth-note signatures show 20..800 —,
 * 100 drum rhythms, volume, beat counter notification to remotes.
 * Wire encodings (app analysis): BPM is 14-bit [n>>7, n&127] (10..500),
 * Beat is [numerator, index into {1,2,4,8,16,32,64}]. */
#ifndef CA79NG_RHYTHM_H
#define CA79NG_RHYTHM_H

#include <stdbool.h>
#include <stdint.h>

#define METRO_PPQ 480
#define RHYTHM_COUNT 100

typedef enum { METRO_CLICK = 0, METRO_RHYTHM = 1 } metro_mode_t;

typedef struct {
    bool running;
    metro_mode_t mode;
    uint16_t bpm;        /* quarter-note BPM, 10..400 */
    uint8_t numerator;   /* beats per bar */
    uint8_t denominator; /* 4 or 8 */
    uint8_t rhythm;      /* 0..99 */
    uint8_t volume;      /* 0..127 */
    /* runtime */
    uint64_t tick_q16;   /* position in PPQ ticks, 16.16 fixed point */
    uint32_t beat_index; /* beat within bar, 0-based */
    uint32_t bar;
    bool pending_downbeat;
    uint64_t rem;        /* sub-tick remainder (exact long-term timing) */
    uint8_t steps_per_beat; /* Rhythm mode: drum grid steps per beat (0 = no step events) */
} metronome_t;

typedef enum { METRO_EV_NONE, METRO_EV_BEAT, METRO_EV_STEP } metro_event_kind_t;

typedef struct {
    metro_event_kind_t kind;
    uint8_t beat;      /* 0-based beat in bar; METRO_EV_STEP: step in bar */
    bool accent;       /* first beat of bar */
    uint32_t bar;
} metro_event_t;

/* ---- drum rhythm patterns (Rhythm mode) ----
 * One bar on a step grid. The 100 patterns are OpenGrand originals generated
 * from the rhythm's category and its position inside the category
 * (src/rhythm/patterns.c); the original pattern data (0x2A0000) is not used.
 * Instruments are drum_inst_t (sound.h). */
#define DRUM_PATTERN_INSTS 6
typedef struct {
    uint8_t steps_per_beat; /* 2 = eighths, 3 = triplets, 4 = sixteenths, 6 = sixteenth triplets */
    uint8_t beats;          /* beats per bar (time signature numerator) */
    uint8_t denominator;    /* 4 or 8 */
    uint32_t hit[DRUM_PATTERN_INSTS]; /* bit s = hit on step s */
    uint32_t accent;        /* accented steps */
} drum_pattern_t;

void rhythm_pattern(unsigned index, drum_pattern_t *out);
/* Velocity of instrument `inst` on step `step` (0 = no hit). */
uint8_t rhythm_hit(const drum_pattern_t *pt, unsigned inst, unsigned step);

void metronome_init(metronome_t *m);
void metronome_set_bpm(metronome_t *m, int bpm);
bool metronome_set_signature(metronome_t *m, int numerator, int denominator);
void metronome_start(metronome_t *m);
void metronome_stop(metronome_t *m);
/* Advance by elapsed_us. Writes at most max_ev beat events, returns count. */
int metronome_advance(metronome_t *m, uint32_t elapsed_us, metro_event_t *ev, int max_ev);

/* Wire codecs */
void bpm_encode(int bpm, uint8_t out[2]);
int bpm_decode(const uint8_t in[2]);
bool beat_encode(int numerator, int denominator, uint8_t out[2]);
bool beat_decode(const uint8_t in[2], int *numerator, int *denominator);

#endif
