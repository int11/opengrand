#include "ca79ng/rhythm.h"

#include <string.h>

#include "ca79ng/util.h"

static const uint8_t DENOMS[] = {1, 2, 4, 8, 16, 32, 64};

void metronome_init(metronome_t *m)
{
    memset(m, 0, sizeof *m);
    m->mode = METRO_CLICK;
    m->bpm = 120;
    m->numerator = 4;
    m->denominator = 4;
    m->volume = 80;
}

void metronome_set_bpm(metronome_t *m, int bpm) { m->bpm = (uint16_t)ca_clampi(bpm, 10, 400); }

bool metronome_set_signature(metronome_t *m, int num, int den)
{
    static const struct { uint8_t n, d; } ok[] = {
        {1, 4}, {2, 4}, {3, 4}, {4, 4}, {5, 4}, {3, 8}, {6, 8}, {7, 8}, {9, 8}, {12, 8}};
    for (size_t i = 0; i < CA_ARRAY_LEN(ok); i++) {
        if (ok[i].n == num && ok[i].d == den) {
            m->numerator = (uint8_t)num;
            m->denominator = (uint8_t)den;
            m->beat_index %= m->numerator;
            return true;
        }
    }
    return false;
}

void metronome_start(metronome_t *m)
{
    m->running = true;
    m->tick_q16 = 0;
    m->beat_index = 0;
    m->bar = 0;
    m->pending_downbeat = true;
    m->rem = 0;
}

void metronome_stop(metronome_t *m) { m->running = false; }

int metronome_advance(metronome_t *m, uint32_t elapsed_us, metro_event_t *ev, int max_ev)
{
    if (!m->running)
        return 0;
    int n = 0;
    /* beat length in PPQ ticks: a quarter = 480, an eighth = 240 */
    uint32_t beat_ticks = (m->denominator == 8) ? METRO_PPQ / 2 : METRO_PPQ;
    /* ticks per microsecond = bpm * PPQ / 60e6, in 16.16 */
    uint64_t prev = m->tick_q16;
    uint64_t step_q16 = m->steps_per_beat ? ((uint64_t)beat_ticks << 16) / m->steps_per_beat : 0;
    if (m->pending_downbeat && n < max_ev) {
        m->pending_downbeat = false;
        ev[n++] = (metro_event_t){METRO_EV_BEAT, 0, true, 0}; /* downbeat at start */
        if (step_q16 && n < max_ev)
            ev[n++] = (metro_event_t){METRO_EV_STEP, 0, true, 0};
    }
    m->rem += ((uint64_t)elapsed_us * m->bpm * METRO_PPQ) << 16;
    m->tick_q16 += m->rem / 60000000ull;
    m->rem %= 60000000ull;
    uint64_t beat_q16 = (uint64_t)beat_ticks << 16;
    uint64_t crossed = m->tick_q16 / beat_q16 - prev / beat_q16;
    while (crossed-- > 0) {
        m->beat_index++;
        if (m->beat_index >= m->numerator) {
            m->beat_index = 0;
            m->bar++;
        }
        if (n < max_ev)
            ev[n++] = (metro_event_t){METRO_EV_BEAT, (uint8_t)m->beat_index, m->beat_index == 0, m->bar};
    }
    if (step_q16) {
        uint64_t s0 = prev / step_q16, s1 = m->tick_q16 / step_q16;
        uint32_t bar_steps = (uint32_t)m->steps_per_beat * m->numerator;
        for (uint64_t k = s0 + 1; k <= s1 && n < max_ev; k++) {
            uint32_t st = (uint32_t)(k % bar_steps);
            ev[n++] = (metro_event_t){METRO_EV_STEP, (uint8_t)st, st == 0, (uint32_t)(k / bar_steps)};
        }
    }
    return n;
}

void bpm_encode(int bpm, uint8_t out[2])
{
    bpm = ca_clampi(bpm, 10, 500);
    out[0] = (uint8_t)(bpm >> 7);
    out[1] = (uint8_t)(bpm & 0x7F);
}

int bpm_decode(const uint8_t in[2]) { return ((in[0] & 0x7F) << 7) | (in[1] & 0x7F); }

bool beat_encode(int num, int den, uint8_t out[2])
{
    for (size_t i = 0; i < CA_ARRAY_LEN(DENOMS); i++) {
        if (DENOMS[i] == den) {
            out[0] = (uint8_t)num;
            out[1] = (uint8_t)i;
            return true;
        }
    }
    return false;
}

bool beat_decode(const uint8_t in[2], int *num, int *den)
{
    if (in[1] >= CA_ARRAY_LEN(DENOMS))
        return false;
    *num = in[0];
    *den = DENOMS[in[1]];
    return true;
}
