/* OpenGrand — tuning, voice allocation, keyboard modes and the engine backend.
 *
 * Requirements: 256-voice polyphony; Single/Dual/Split/4Hands; master tuning
 * 427..453 Hz; temperaments Equal, Pure Major, Pure Minor, Pythagorean,
 * Meantone, Werckmeister, Kirnberger (+User per-note) with a key; stretch
 * tuning Off/Normal/Wide (+User); damper / sostenuto / soft pedal behaviour. */
#ifndef CA79NG_SOUND_H
#define CA79NG_SOUND_H

#include <stdbool.h>
#include <stdint.h>

/* ---------------- tuning ---------------- */
typedef enum {
    TEMP_EQUAL = 0, TEMP_PURE_MAJOR, TEMP_PURE_MINOR, TEMP_PYTHAGOREAN,
    TEMP_MEANTONE, TEMP_WERCKMEISTER, TEMP_KIRNBERGER, TEMP_USER, TEMP_COUNT
} temperament_t;

typedef enum { STRETCH_OFF = 0, STRETCH_NORMAL = 3, STRETCH_WIDE = 8, STRETCH_USER = 0x40 } stretch_t;

typedef struct {
    float master_hz;           /* A4 reference, 427..453 */
    temperament_t temperament;
    uint8_t temperament_key;   /* 0 = C .. 11 = B */
    stretch_t stretch;
    int8_t user_temperament[12]; /* cents, for TEMP_USER */
    int8_t per_note_cents[128];  /* user per-note tuning (User stretch) */
    int8_t transpose;            /* key transpose -12..+12 */
} tuning_t;

void tuning_default(tuning_t *t);
/* Offset in cents applied to an equal-tempered note (temperament + stretch). */
float tuning_cents(const tuning_t *t, int note);
float tuning_freq(const tuning_t *t, int note);
/* Raw <-> Hz codec used by the Tuning parameter: raw = 2*(Hz-440)+64. */
uint8_t tuning_hz_to_raw(float hz);
float tuning_raw_to_hz(uint8_t raw);

/* ---------------- parts & keyboard modes ---------------- */
typedef enum { KBMODE_SINGLE = 0, KBMODE_DUAL = 1, KBMODE_SPLIT = 2, KBMODE_4HANDS = 3 } kb_mode_t;

/* Part numbers follow the SysEx part byte. */
typedef enum {
    PART_MAIN1 = 0, PART_MAIN2 = 1, PART_MAIN3 = 2, PART_MAIN4 = 3,
    PART_LAYER = 8, PART_LOWER = 9, PART_4H_RIGHT = 10, PART_4H_LEFT = 11,
    PART_GLOBAL = 15,
} part_id_t;

typedef struct {
    uint8_t msb, lsb, program; /* sound selection (bank MSB/LSB + PC 0-based) */
    int8_t octave_shift;       /* -3..+3 */
    int8_t transpose;          /* part transpose */
    uint8_t volume;            /* 0..127 (balance result) */
    bool enabled;
} part_state_t;

typedef struct {
    kb_mode_t mode;
    uint8_t split_point;  /* MIDI note; keys below go to lower / left */
    uint8_t balance;      /* 0..127, 64 = equal */
    bool lower_pedal;     /* Split: damper also affects lower part */
    part_state_t main, layer, lower, right4h, left4h;
} kb_layout_t;

typedef struct {
    uint8_t part;    /* part_id_t */
    int8_t note;     /* resulting sounding note, -1 = none */
    uint8_t volume;  /* part volume */
} part_route_t;

void kb_layout_default(kb_layout_t *l);
/* Resolves a physical key (MIDI note) into up to two sounding parts.
 * Returns the number of routes written. */
int kb_layout_route(const kb_layout_t *l, int note, part_route_t out[2]);

/* ---------------- voices ---------------- */
#define VOICE_MAX 256

typedef struct {
    bool active;
    bool key_down;
    bool sustained;   /* held by damper */
    bool sostenuto;   /* captured by sostenuto */
    uint8_t part, note, velocity;
    uint32_t age;     /* allocation order for stealing */
} voice_t;

typedef struct sound_engine sound_engine_t;

typedef struct {
    voice_t v[VOICE_MAX];
    uint32_t clock;
    uint8_t damper[16];     /* per part (index = part & 15), 0..127 */
    bool sostenuto[16];
    uint8_t soft[16];
    bool damper_hold;       /* VT Damper Hold: sustain non-decaying sounds */
    sound_engine_t *engine;
    uint32_t stolen;
} voice_pool_t;

void voice_pool_init(voice_pool_t *p, sound_engine_t *engine);
int voice_note_on(voice_pool_t *p, uint8_t part, uint8_t note, uint8_t velocity);
void voice_note_off(voice_pool_t *p, uint8_t part, uint8_t note);
void voice_pedal(voice_pool_t *p, uint8_t part, uint8_t cc, uint8_t value);
void voice_all_off(voice_pool_t *p, bool immediate);
void voice_release_finished(voice_pool_t *p, int index); /* engine -> pool */
/* VT Damper Hold changed: re-signals every damper-held voice to the engine. */
void voice_set_damper_hold(voice_pool_t *p, bool on);
int voice_active_count(const voice_pool_t *p);

/* ---------------- engine backend ---------------- */
typedef struct {
    float freq_hz;
    uint8_t velocity;
    uint8_t part;
    uint8_t msb, lsb, program;
    int8_t voicing;  /* per-note voicing offset -5..+5 (User voicing), 0 = none */
} engine_note_t;

/* Drum instruments rendered by the engine for the Rhythm metronome mode. */
typedef enum {
    DRUM_KICK = 0, DRUM_SNARE, DRUM_RIM, DRUM_HAT, DRUM_OHAT, DRUM_RIDE, DRUM_COUNT
} drum_inst_t;

/* Implemented by the platform: the real target drives the DSP, the
 * simulator uses a small reference synthesizer (ports/sim/synth.c). */
struct sound_engine {
    void *ctx;
    void (*start)(void *ctx, int voice, const engine_note_t *n);
    void (*release)(void *ctx, int voice);
    void (*kill)(void *ctx, int voice);
    void (*control)(void *ctx, uint8_t part, uint8_t cc, uint8_t value); /* pedals, volume, fx sends */
    void (*set_param)(void *ctx, uint16_t param_id, uint8_t part, int32_t value);
    /* Optional. A released voice is now held by the damper (or its hold mode
     * changed). hold = true (Damper Hold On): keep non-decaying sounds (organ,
     * strings, pads) at full level; hold = false: let them decay naturally
     * like a sustained piano string. Decaying sounds ignore it. */
    void (*sustain)(void *ctx, int voice, bool hold);
    /* Optional. One drum hit of the Rhythm metronome mode. */
    void (*drum)(void *ctx, uint8_t instrument, uint8_t velocity);
};

#endif
