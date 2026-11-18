/* OpenGrand — Sound Preview (SoundPreviewStart / Stop / Volume, fn10 50 1D/1E/1F).
 *
 * Each Virtual Technician parameter gets a short phrase written for OpenGrand
 * that makes that parameter audible (the original preview phrases are not
 * reused). The phrase plays on the Main1 part through the current sound and
 * VT settings, directly into the engine (no MIDI out, not recorded), scaled
 * by SoundPreviewVolume. A new Start replaces a running preview. */
#include "ca79ng/util.h"
#include "piano_int.h"

#define ON(t, n, v) {t, 0, n, v}
#define OFF(t, n) {t, 1, n, 0}
#define PED(t, v) {t, 2, v, 0}
#define END(t) {t, 3, 0, 0}

/* Touch curve / minimum touch / key volume: one note, soft to strong */
static const preview_step_t P_DYNAMICS[] = {
    ON(0, 60, 20), OFF(350, 60), ON(400, 60, 45), OFF(750, 60), ON(800, 60, 70), OFF(1150, 60),
    ON(1200, 60, 95), OFF(1550, 60), ON(1600, 60, 120), OFF(2200, 60), END(2600)};
/* Voicing / brilliance / topboard: broken chord then the chord */
static const preview_step_t P_TONE[] = {
    ON(0, 48, 80), ON(250, 55, 80), ON(500, 64, 85), ON(750, 72, 90), OFF(1200, 48), OFF(1200, 55),
    OFF(1200, 64), OFF(1200, 72), ON(1300, 48, 100), ON(1300, 64, 100), ON(1300, 67, 100), ON(1300, 72, 105),
    OFF(2600, 48), OFF(2600, 64), OFF(2600, 67), OFF(2600, 72), END(3200)};
/* Damper resonance / undamped string resonance: pedal held over a low chord */
static const preview_step_t P_DAMPER[] = {
    PED(0, 127), ON(100, 36, 100), ON(100, 43, 90), ON(100, 52, 85), OFF(400, 36), OFF(400, 43), OFF(400, 52),
    ON(900, 67, 80), OFF(1200, 67), PED(2800, 0), END(3400)};
/* Damper noise: pedal strokes with and without a note */
static const preview_step_t P_DAMPER_NOISE[] = {
    PED(0, 127), PED(500, 0), ON(800, 55, 70), PED(900, 127), OFF(1100, 55), PED(1700, 0), PED(2100, 127),
    PED(2600, 0), END(3000)};
/* String resonance: a soft held chord, short notes an octave above */
static const preview_step_t P_STRING[] = {
    ON(0, 48, 30), ON(0, 52, 30), ON(0, 55, 30), ON(300, 60, 100), OFF(450, 60), ON(700, 64, 100), OFF(850, 64),
    ON(1100, 67, 100), OFF(1250, 67), OFF(2200, 48), OFF(2200, 52), OFF(2200, 55), END(2700)};
/* Key-off effect / key action noise / release: repeated staccato notes */
static const preview_step_t P_STACCATO[] = {
    ON(0, 40, 90), OFF(120, 40), ON(300, 40, 90), OFF(420, 40), ON(600, 47, 90), OFF(720, 47),
    ON(900, 52, 90), OFF(1020, 52), ON(1200, 40, 110), OFF(1900, 40), END(2400)};
/* Hammer delay: pianissimo repeated notes */
static const preview_step_t P_SOFT_REPEAT[] = {
    ON(0, 72, 15), OFF(200, 72), ON(300, 72, 20), OFF(500, 72), ON(600, 74, 25), OFF(800, 74),
    ON(900, 76, 30), OFF(1300, 76), END(1700)};
/* Decay time: long single low note */
static const preview_step_t P_DECAY[] = {ON(0, 33, 110), OFF(3500, 33), END(3900)};
/* Stretch tuning / temperament: octaves over the range, then thirds */
static const preview_step_t P_TUNING[] = {
    ON(0, 36, 80), ON(0, 48, 80), OFF(700, 36), OFF(700, 48), ON(800, 84, 80), ON(800, 96, 80), OFF(1500, 84),
    OFF(1500, 96), ON(1600, 60, 85), ON(1600, 64, 85), ON(1600, 67, 85), OFF(2800, 60), OFF(2800, 64),
    OFF(2800, 67), END(3200)};
/* Pedal depth / half pedal: phrase with half and soft pedal */
static const preview_step_t P_PEDAL[] = {
    ON(0, 60, 90), PED(0, 64), OFF(300, 60), ON(400, 64, 90), OFF(700, 64), PED(900, 0), ON(1000, 67, 90),
    OFF(1300, 67), END(1700)};

static const preview_step_t *phrase(uint8_t target)
{
    switch (target) {
    case 0: case 10: case 15: return P_DYNAMICS;     /* Touch Curve, Minimum Touch, Key Volume */
    case 1: case 8: case 20: case 19: return P_TONE; /* Voicing, Topboard, Brilliance, Stereo Width */
    case 2: case 21: return P_DAMPER;                /* Damper Resonance, Undamped Str. Reso. */
    case 3: return P_DAMPER_NOISE;
    case 4: case 22: return P_STRING;                /* String / Cabinet Resonance */
    case 5: case 6: return P_STACCATO;               /* Key Off Effect, Key Action Noise */
    case 7: return P_SOFT_REPEAT;                    /* Hammer Delay */
    case 9: return P_DECAY;
    case 11: case 13: return P_TUNING;               /* Stretch Tuning, Temperament */
    case 17: case 18: return P_PEDAL;                /* Half Pedal Adjust, Soft Pedal Depth */
    default: return P_TONE;
    }
}

void preview_stop(piano_t *p)
{
    if (!p->preview.active)
        return;
    p->preview.active = false;
    for (int i = 0; i < VOICE_MAX; i++)
        if (p->voices.v[i].active && p->voices.v[i].part == PART_MAIN1 && p->voices.v[i].key_down)
            voice_note_off(&p->voices, PART_MAIN1, p->voices.v[i].note);
    pn_control(p, PART_MAIN1, 64, 0);
    pn_control(p, PART_MAIN1, 67, 0);
}

void preview_start(piano_t *p, uint8_t target)
{
    preview_stop(p);
    p->preview.target = target;
    p->preview.steps = phrase(target);
    p->preview.i = 0;
    p->preview.t_ms = 0;
    p->preview.active = true;
    preview_tick(p, 0);
}

void preview_tick(piano_t *p, uint32_t elapsed_ms)
{
    preview_t *pv = &p->preview;
    if (!pv->active)
        return;
    pv->t_ms += elapsed_ms;
    while (pv->active && pv->steps[pv->i].at_ms <= pv->t_ms) {
        const preview_step_t *s = &pv->steps[pv->i++];
        switch (s->kind) {
        case 0: pn_note(p, PART_MAIN1, s->a, (uint8_t)ca_clampi(s->b * pv->volume / 127, 1, 127)); break;
        case 1: pn_note(p, PART_MAIN1, s->a, 0); break;
        case 2:
            /* Soft Pedal Depth preview uses the soft pedal instead of the damper */
            pn_control(p, PART_MAIN1, pv->target == 18 ? 67 : 64, s->a);
            break;
        default:
            preview_stop(p);
            /* tell the controllers the preview ended on its own */
            {
                const uint8_t t = pv->target;
                kp_send_status(&p->kp, 0x10, 0x50, 0x1E, 0x7F, &t, 1);
            }
            return;
        }
    }
}
