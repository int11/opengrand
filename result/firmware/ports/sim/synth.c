/* Reference synthesizer for the host simulator.
 *
 * This is NOT the original SK-EX / Harmonic Imaging sound (sample data is not
 * reused). It is a small additive model good enough to hear that keyboard,
 * pedals, layering, tuning and Virtual Technician wiring behave correctly:
 *   - per voice: 6 partials with piano-like inharmonicity, two-stage decay,
 *     velocity-dependent brightness, release time;
 *   - timbre family chosen from the bank/program (piano, e-piano, organ,
 *     strings/pad, other);
 *   - Schroeder reverb (4 combs + 2 all-pass) driven by ReverbDepth/Time;
 *   - metronome click, drum kit for the Rhythm mode (kick, snare, rim,
 *     closed/open hi-hat, ride from noise and sine);
 *   - Damper Hold: organ/pad voices held by the damper decay naturally when
 *     it is Off; soft pedal depth scales the soft-pedal attenuation. */
#include "synth.h"

#include <math.h>
#include <string.h>

#include "ca79ng/params_gen.h"
#include "ca79ng/util.h"

#define TWO_PI 6.28318530718f

typedef enum { FAM_PIANO, FAM_EPIANO, FAM_ORGAN, FAM_PAD, FAM_OTHER } family_t;

static family_t family_of(uint8_t msb, uint8_t lsb, uint8_t pc)
{
    (void)lsb;
    (void)msb;
    /* GM-style program groups (PC 0-based), matching the manual's bank layout */
    if (pc <= 3) return FAM_PIANO;
    if (pc <= 7) return FAM_EPIANO;
    if (pc >= 16 && pc <= 23) return FAM_ORGAN;
    if ((pc >= 40 && pc <= 55) || (pc >= 88 && pc <= 95)) return FAM_PAD;
    return FAM_OTHER;
}

void synth_init(synth_t *s, float sample_rate)
{
    memset(s, 0, sizeof *s);
    s->sr = sample_rate;
    s->master = 0.25f;
    for (int p = 0; p < 64; p++) {
        s->part_volume[p] = 1.0f;
        s->part_reverb[p] = 0.3f;
    }
    s->reverb_time = 0.5f;
    static const int comb_len[4] = {1557, 1617, 1491, 1422};
    static const int ap_len[2] = {225, 556};
    for (int i = 0; i < 4; i++) s->comb_len[i] = comb_len[i];
    for (int i = 0; i < 2; i++) s->ap_len[i] = ap_len[i];
    s->decay_scale = 1.0f;
    s->release_scale = 1.0f;
    s->hammer_noise = 0.4f;
    s->soft_depth = 5.0f;
}

float synth_soft_gain(const synth_t *s)
{
    /* depth 1 -> -6 % at full pedal, depth 10 -> -60 % */
    return 1.0f - s->soft * 0.06f * s->soft_depth;
}

static void v_sustain(void *ctx, int vi, bool hold)
{
    synth_t *s = ctx;
    synth_voice_t *v = &s->v[vi];
    if (v->fam == FAM_ORGAN || v->fam == FAM_PAD) {
        v->damp_decay = !hold;
        /* a sustained string-like decay of ~4 s */
        if (!hold)
            v->decay = expf(-6.9f / (4.0f * s->sr));
    }
}

void synth_drum(synth_t *s, uint8_t inst, uint8_t velocity)
{
    if (inst >= DRUM_COUNT)
        return;
    static const struct { float freq, t60, noise, tone; } K[DRUM_COUNT] = {
        [DRUM_KICK] = {110.0f, 0.35f, 0.05f, 1.0f},
        [DRUM_SNARE] = {190.0f, 0.22f, 0.8f, 0.4f},
        [DRUM_RIM] = {900.0f, 0.05f, 0.5f, 0.6f},
        [DRUM_HAT] = {0.0f, 0.06f, 0.6f, 0.0f},
        [DRUM_OHAT] = {0.0f, 0.35f, 0.5f, 0.0f},
        [DRUM_RIDE] = {620.0f, 0.9f, 0.25f, 0.35f},
    };
    s->drum[inst].env = velocity / 127.0f;
    s->drum[inst].phase = 0.0f;
    s->drum[inst].freq = K[inst].freq;
    s->drum[inst].decay = expf(-6.9f / (K[inst].t60 * s->sr));
    s->drum[inst].noise_amt = K[inst].noise;
    s->drum[inst].tone_amt = K[inst].tone;
    if (!s->drum[inst].rnd)
        s->drum[inst].rnd = 0x9E3779B9u * (inst + 1u);
}

static void v_drum(void *ctx, uint8_t inst, uint8_t vel) { synth_drum(ctx, inst, vel); }

static float render_drums(synth_t *s)
{
    float out = 0.0f;
    for (int i = 0; i < DRUM_COUNT; i++) {
        float env = s->drum[i].env;
        if (env < 0.0005f)
            continue;
        s->drum[i].rnd = s->drum[i].rnd * 1664525u + 1013904223u;
        float nz = (int32_t)s->drum[i].rnd / 2147483648.0f;
        if (i == DRUM_HAT || i == DRUM_OHAT || i == DRUM_RIDE) { /* crude high-pass for metal */
            float hp = nz - s->drum[i].hp;
            s->drum[i].hp = nz;
            nz = hp;
        }
        float f = s->drum[i].freq;
        if (i == DRUM_KICK)
            f = 45.0f + (f - 45.0f) * env; /* pitch sweep down */
        float tone = 0.0f;
        if (f > 0.0f) {
            s->drum[i].phase += TWO_PI * f / s->sr;
            if (s->drum[i].phase > TWO_PI) s->drum[i].phase -= TWO_PI;
            tone = sinf(s->drum[i].phase);
            if (i == DRUM_RIDE) tone = 0.6f * tone + 0.4f * sinf(s->drum[i].phase * 2.76f);
        }
        out += env * (s->drum[i].tone_amt * tone + s->drum[i].noise_amt * nz) * 0.6f;
        s->drum[i].env = env * s->drum[i].decay;
    }
    return out;
}

static void v_start(void *ctx, int vi, const engine_note_t *n)
{
    synth_t *s = ctx;
    synth_voice_t *v = &s->v[vi];
    memset(v, 0, sizeof *v);
    v->on = true;
    v->freq = n->freq_hz;
    v->part = n->part;
    v->fam = (uint8_t)family_of(n->msb, n->lsb, n->program);
    float vel = n->velocity / 127.0f;
    v->amp = (0.15f + 0.85f * vel * vel) * s->part_volume[n->part & 63];
    v->bright = 0.35f + 0.65f * vel + 0.05f * n->voicing; /* per-note voicing: brighter / mellower */
    if (v->bright < 0.1f) v->bright = 0.1f;
    if (v->bright > 0.98f) v->bright = 0.98f;
    v->env = (v->fam == FAM_PAD) ? 0.0f : 1.0f;
    v->noise = (v->fam == FAM_PIANO) ? s->hammer_noise * vel : 0.0f;
    /* decay: lower notes ring longer, like real strings */
    float note = 12.0f * log2f(v->freq / 440.0f) + 69.0f;
    float t60 = (v->fam == FAM_PIANO) ? (14.0f - note * 0.11f) : (v->fam == FAM_EPIANO ? 4.0f : 1e6f);
    t60 = CA_MAX(t60, 0.6f) * s->decay_scale;
    v->decay = expf(-6.9f / (t60 * s->sr));
    v->release = expf(-6.9f / (0.25f * s->release_scale * s->sr));
    v->rnd = 0x1234567u ^ (uint32_t)vi * 2654435761u;
}

static void v_release(void *ctx, int vi)
{
    synth_t *s = ctx;
    s->v[vi].released = true;
}

static void v_kill(void *ctx, int vi)
{
    synth_t *s = ctx;
    s->v[vi].on = false;
}

static void v_control(void *ctx, uint8_t part, uint8_t cc, uint8_t value)
{
    synth_t *s = ctx;
    if (cc == 7)
        s->part_volume[part & 63] = value / 127.0f;
    else if (cc == 67)
        s->soft = value / 127.0f;
}

static void v_param(void *ctx, uint16_t id, uint8_t part, int32_t value)
{
    synth_t *s = ctx;
    (void)part;
    switch (id) {
    case PRM_REVERB: s->reverb_on = value != 0; break;
    case PRM_REVERB_DEPTH: s->part_reverb[0] = value / 127.0f; break;
    case PRM_REVERB_TIME: s->reverb_time = 0.2f + 0.75f * value / 127.0f; break;
    case PRM_DECAY_TIME: s->decay_scale = 0.4f + value / 127.0f * 1.4f; break;
    case PRM_DCARELEASE_TIME: s->release_scale = 0.3f + value / 127.0f * 2.0f; break;
    case PRM_KEY_ATTACK_NOISE: s->hammer_noise = value / 127.0f; break;
    case PRM_SOFT_PEDAL_DEPTH: s->soft_depth = (float)(value < 1 ? 1 : (value > 10 ? 10 : value)); break;
    default: break;
    }
}

void synth_bind(synth_t *s, sound_engine_t *e)
{
    e->ctx = s;
    e->start = v_start;
    e->release = v_release;
    e->kill = v_kill;
    e->control = v_control;
    e->set_param = v_param;
    e->sustain = v_sustain;
    e->drum = v_drum;
}

void synth_click(synth_t *s, bool accent, uint8_t volume)
{
    s->click_phase = 0.0f;
    s->click_env = (volume / 127.0f) * (accent ? 1.0f : 0.6f);
    s->click_freq = accent ? 1760.0f : 1320.0f;
}

static float reverb(synth_t *s, float in)
{
    float fb = 0.70f + 0.27f * s->reverb_time;
    float out = 0.0f;
    for (int i = 0; i < 4; i++) {
        float *buf = s->comb[i];
        int idx = s->comb_pos[i];
        float y = buf[idx];
        s->comb_lp[i] = y * 0.8f + s->comb_lp[i] * 0.2f;
        buf[idx] = in + s->comb_lp[i] * fb;
        s->comb_pos[i] = (idx + 1) % s->comb_len[i];
        out += y;
    }
    out *= 0.25f;
    for (int i = 0; i < 2; i++) {
        float *buf = s->ap[i];
        int idx = s->ap_pos[i];
        float d = buf[idx];
        float y = -out + d;
        buf[idx] = out + d * 0.5f;
        s->ap_pos[i] = (idx + 1) % s->ap_len[i];
        out = y;
    }
    return out;
}

/* returns the indices of voices whose release finished (for the voice pool) */
int synth_render(synth_t *s, float *lr, int frames, int *finished, int max_finished)
{
    int nf = 0;
    for (int f = 0; f < frames; f++) {
        float dry = 0.0f;
        for (int i = 0; i < SYNTH_VOICES; i++) {
            synth_voice_t *v = &s->v[i];
            if (!v->on)
                continue;
            float x = 0.0f;
            /* partials: inharmonic stretch B ~ 1e-4, brightness rolls off highs */
            for (int k = 1; k <= 6; k++) {
                float fk = v->freq * k * sqrtf(1.0f + 0.0001f * k * k);
                if (fk > s->sr * 0.45f)
                    break;
                float a = powf(v->bright, (float)(k - 1)) / k;
                if (v->fam == FAM_ORGAN) a = (k == 1 || k == 2 || k == 4) ? 0.5f : 0.1f;
                v->ph[k - 1] += TWO_PI * fk / s->sr;
                if (v->ph[k - 1] > TWO_PI) v->ph[k - 1] -= TWO_PI;
                x += a * sinf(v->ph[k - 1]);
            }
            if (v->noise > 0.0005f) { /* hammer thump */
                v->rnd = v->rnd * 1664525u + 1013904223u;
                x += v->noise * ((int32_t)v->rnd / 2147483648.0f) * 0.3f;
                v->noise *= 0.995f;
            }
            if (v->damp_decay && !v->released)
                v->env *= v->decay;
            else if (v->fam == FAM_PAD && !v->released)
                v->env += (1.0f - v->env) * (8.0f / s->sr);
            else
                v->env *= v->released ? v->release : v->decay;
            float soft = synth_soft_gain(s);
            dry += x * v->env * v->amp * soft;
            if (v->env < 0.0005f && (v->released || v->damp_decay || v->fam == FAM_PIANO || v->fam == FAM_EPIANO)) {
                v->on = false;
                if (finished && nf < max_finished)
                    finished[nf++] = i;
            }
        }
        dry += render_drums(s);
        if (s->click_env > 0.001f) {
            s->click_phase += TWO_PI * s->click_freq / s->sr;
            dry += sinf(s->click_phase) * s->click_env * 0.5f;
            s->click_env *= 0.9985f;
        }
        float wet = s->reverb_on ? reverb(s, dry) * s->part_reverb[0] : 0.0f;
        float l = (dry + wet) * s->master, r = (dry + wet * 0.9f) * s->master;
        lr[2 * f] = l > 1.0f ? 1.0f : (l < -1.0f ? -1.0f : l);
        lr[2 * f + 1] = r > 1.0f ? 1.0f : (r < -1.0f ? -1.0f : r);
    }
    return nf;
}
