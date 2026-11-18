#ifndef CA79NG_SIM_SYNTH_H
#define CA79NG_SIM_SYNTH_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/sound.h"

#define SYNTH_VOICES VOICE_MAX

typedef struct {
    bool on, released;
    bool damp_decay; /* held by the damper with Damper Hold Off: decays naturally */
    uint8_t part, fam;
    float freq, amp, bright, env, decay, release, noise;
    float ph[6];
    uint32_t rnd;
} synth_voice_t;

typedef struct {
    float sr, master;
    synth_voice_t v[SYNTH_VOICES];
    float part_volume[64], part_reverb[64];
    float soft, decay_scale, release_scale, hammer_noise;
    bool reverb_on;
    float reverb_time;
    float comb[4][1700], comb_lp[4];
    int comb_len[4], comb_pos[4];
    float ap[2][600];
    int ap_len[2], ap_pos[2];
    float click_phase, click_env, click_freq;
    float soft_depth;    /* SoftPedalDepth 1..10 */
    struct { float env, phase, freq, decay, noise_amt, tone_amt; uint32_t rnd; float hp; } drum[DRUM_COUNT];
} synth_t;

void synth_init(synth_t *s, float sample_rate);
void synth_bind(synth_t *s, sound_engine_t *e);
void synth_click(synth_t *s, bool accent, uint8_t volume);
void synth_drum(synth_t *s, uint8_t inst, uint8_t velocity);
/* soft pedal gain for the current soft amount and depth (1 = no effect) */
float synth_soft_gain(const synth_t *s);
int synth_render(synth_t *s, float *lr, int frames, int *finished, int max_finished);

#endif
