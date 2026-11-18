/* OpenGrand - Phase 3-5 metronome stage 1 (core judges, stock plays).  See kpmetro.h. */
#include "ca79ng/kpmetro.h"

#include <string.h>

#define MV1 0x56
#define MPART 0x7F

/* beat table 0x401235B8 (REA read_bytes, 10 x {u16 ticks, u8 beats, 0}): 1/4..5/4, 3/8, 6/8, 7/8, 9/8, 12/8 */
static const uint8_t BEAT_TICKS[KPM_BEATS] = {0x60, 0x60, 0x60, 0x60, 0x60, 0x30, 0x30, 0x30, 0x30, 0x30};
static const uint8_t BEAT_N[KPM_BEATS] = {1, 2, 3, 4, 5, 3, 6, 7, 9, 12};

void kpm_beat_entry(uint8_t index, uint16_t *ticks, uint8_t *beats)
{
    if (index > 9)
        index = 3;   /* FUN_4016E5FC / FUN_4016E610 */
    *ticks = BEAT_TICKS[index];
    *beats = BEAT_N[index];
}

/* FUN_4014EE08(den, num): den < 5 and num < 13, table 0x400FD88A[den * 13 + num] (0xFF = refused).  The table
 * holds exactly the beat-table entries: den code 2 = ticks 0x60, 3 = ticks 0x30, num = beats per bar
 * (result/stockfw/tools/roundtrip_metro.py checks all 65 cells against the image). */
int kpm_beat_index(uint8_t num, uint8_t den)
{
    if (num >= 13 || den >= 5)
        return -1;
    for (uint8_t i = 0; i < KPM_BEATS; i++)
        if (BEAT_N[i] == num && ((den == 2 && BEAT_TICKS[i] == 0x60) || (den == 3 && BEAT_TICKS[i] == 0x30)))
            return i;
    return -1;
}

uint16_t kpm_accum(uint16_t tempo, uint32_t *q, uint32_t *r)
{
    uint16_t bpm = (tempo < 10 || tempo > 400) ? 120 : tempo;   /* FUN_40165F18 */
    uint32_t x = (uint32_t)bpm * 0x60u * 2u;                     /* FUN_40165ED4(bpm, 0x60): x / 60000 */
    *q = x >= 60000u ? 1u : 0u;                                  /* x <= 76800: no divide (cave runtime) */
    *r = x - *q * 60000u;
    return bpm;
}

uint8_t kpm_stock_ports(uint8_t binding, bool forward)
{
    if (binding >= 1 && binding <= 3)
        return (uint8_t)(1u << (binding - 1));
    if (binding == 4)
        return (uint8_t)(0x08u | (forward ? 0x07u : 0u));
    return 0;
}

void kpm_beat_frame(uint8_t b, uint8_t out[KPM_FRAME_LEN])
{
    /* FUN_4014ACD8(buf, 0x7F, 0x32) header + 56 0B 7F b 7F F7 (FUN_4014BF24) */
    static const uint8_t head[9] = {0xF0, 0x40, 0x7F, 0x32, 0x04, 0x1E, 0x56, 0x0B, 0x7F};
    memcpy(out, head, sizeof head);
    out[9] = b;
    out[10] = 0x7F;
    out[11] = 0xF7;
}

void kpm_init(kpm_t *m, const kpm_cfg_t *cfg)
{
    memset(m, 0, sizeof *m);
    m->cfg = *cfg;
}

bool kpm_is_key(uint8_t v1, uint8_t v2, uint8_t part)
{
    if (v1 != MV1 || part != MPART)
        return false;
    return v2 == 0x01 || v2 == 0x02 || v2 == 0x03 || v2 == 0x08 || v2 == 0x09 || v2 == 0x0A;
}

static uint8_t start_fx(const kpm_cfg_t *c)
{
    /* FUN_4016E6BC: mode byte (+0x4A) 0 -> FUN_4016E668, else FUN_4016EF68; both send BeatCounter 1 when on */
    return (uint8_t)((c->mode == 0 ? KPM_FX_START : KPM_FX_RHYTHM) | KPM_FX_TEMPO | (c->on ? KPM_FX_BEAT1 : 0));
}

kpm_result_t kpm_judge(const kpm_cfg_t *cur, uint8_t v2, const uint8_t *d, uint32_t n)
{
    kpm_result_t r;
    uint32_t want = (v2 == 0x01 || v2 == 0x02) ? 2 : 1;
    uint32_t q, rem;
    memset(&r, 0, sizeof r);
    r.cfg = *cur;
    if (!kpm_is_key(MV1, v2, MPART))
        return r;   /* KPM_NOT_MINE */
    if (n != want) {
        r.verdict = KPM_IGNORED;   /* the stock front refuses a length other than the descriptor's (ACK only) */
        return r;
    }
    r.verdict = KPM_APPLIED;
    switch (v2) {
    case 0x08:   /* id 0x70: +0x49 = d0, FUN_4016E6BC */
        r.cfg.on = d[0];
        r.fx = d[0] ? start_fx(&r.cfg) : KPM_FX_STOP;
        break;
    case 0x01:   /* id 0x71: +0x4E = d0 * 128 + d1, FUN_40169354 -> FUN_40165F18 */
        r.cfg.tempo = (uint16_t)(d[0] * 128u + d[1]);
        r.fx = KPM_FX_TEMPO;
        break;
    case 0x02: { /* id 0x72: index if FUN_4014EE08 allows it, then FUN_4016E668 whatever the value */
        int i = kpm_beat_index(d[0], d[1]);
        if (i >= 0)
            r.cfg.beat = (uint8_t)i;
        r.fx = (uint8_t)(KPM_FX_START | KPM_FX_TEMPO | (r.cfg.on ? KPM_FX_BEAT1 : 0));
        break;
    }
    case 0x09:   /* id 0x73: < 100 -> +0x4B, FUN_4016FBAC */
        if (d[0] < 100) {
            r.cfg.rhythm = d[0];
            r.fx = KPM_FX_RELOAD;
        }
        break;
    case 0x03:   /* id 0x74: +0x50, FUN_4012EA00(0x1BC) / FUN_4016A2FC(0x1B8) */
        r.cfg.volume = d[0];
        r.fx = KPM_FX_VOLUME;
        break;
    case 0x0A:   /* id 0x75: +0x4A, d0 != 0 -> FUN_4016EF68, else FUN_4016E668 */
        r.cfg.mode = d[0];
        r.fx = (uint8_t)((d[0] ? KPM_FX_RHYTHM : KPM_FX_START) | KPM_FX_TEMPO | (r.cfg.on ? KPM_FX_BEAT1 : 0));
        break;
    default:
        break;
    }
    if (memcmp(&r.cfg, cur, sizeof r.cfg) != 0)
        r.fx |= KPM_FX_CHANGED;
    if (r.fx & KPM_FX_TEMPO)
        r.bpm = kpm_accum(r.cfg.tempo, &q, &rem);
    return r;
}

uint32_t kpm_set(kpm_t *m, uint8_t v2, const uint8_t *d, uint32_t n, kpm_result_t *r, uint8_t out[KPM_FRAME_LEN])
{
    *r = kpm_judge(&m->cfg, v2, d, n);
    if (r->verdict == KPM_NOT_MINE)
        return 0;
    m->st.sets++;
    if (r->verdict == KPM_IGNORED) {
        m->st.ignored++;
        return 0;
    }
    if ((v2 == 0x02 && kpm_beat_index(d[0], d[1]) < 0) || (v2 == 0x09 && d[0] >= 100))
        m->st.refused++;
    m->cfg = r->cfg;
    if (r->fx & (KPM_FX_START | KPM_FX_RHYTHM))
        m->resync = true;   /* the counters jumped (beat = beats per bar, position = beat length) */
    if (r->fx & KPM_FX_BEAT1) {
        m->st.starts++;
        kpm_beat_frame(1, out);
        return KPM_FRAME_LEN;
    }
    return 0;
}

/* right after FUN_4016E668 / FUN_4016EF68 (FUN_4016E6E8) the counters sit at beat = beats per bar,
 * position = beat length; the first task-20 pass that advances wraps them to beat 1 */
static bool armed(const kpm_run_t *r)
{
    return r->pos == r->len && (uint8_t)r->beat == r->bpb;
}

uint32_t kpm_poll(kpm_t *m, const kpm_run_t *run, uint8_t out[2][KPM_FRAME_LEN])
{
    uint32_t k = 0;
    const kpm_run_t *l = &m->last;
    bool first = !m->have_last;
    if (first || run->rec_state == 6 || run->rec_state != l->rec_state) {
        /* count-in counts with other variables (R3); record start / stop restart the counters (R2) */
        if (!first)
            m->st.resyncs++;
    } else if (l->reload && !run->reload) {
        /* FUN_4016FBC0 ran FUN_4016EF68 in task 20: BeatCounter 1 when on, then maybe the first beat */
        if (run->on) {
            kpm_beat_frame(1, out[k++]);
            m->st.reloads++;
            if (!armed(run) && run->half != 0) {
                kpm_beat_frame((uint8_t)run->beat, out[k++]);
                m->st.beats++;
            }
        }
    } else if (m->resync) {
        /* a judged set restarted the counters (its BeatCounter 1 went with the set); a pass may have wrapped */
        if (run->on && l->on && !armed(run) && run->half != 0) {
            kpm_beat_frame((uint8_t)run->beat, out[k++]);
            m->st.beats++;
        }
    } else if (run->on && l->on && run->half != 0 && (run->beat != l->beat || run->pos < l->pos)) {
        /* FUN_4016EA18 moves the beat on by one or wraps it to 1; anything else is a restart the poll did not
         * see happen (a set that preempted task 20 inside FUN_4016EA18 and had its position overwritten) */
        if (run->beat == 1 || run->beat == l->beat + 1) {
            kpm_beat_frame((uint8_t)run->beat, out[k++]);   /* FUN_4016DE5C: FUN_4014BF24(0x7F, FUN_4016E9A0(), 0x7F) */
            m->st.beats++;
        } else {
            m->st.resyncs++;
        }
    }
    m->resync = false;
    m->last = *run;
    m->have_last = true;
    return k;
}

uint32_t kpm_value(const kpm_cfg_t *c, uint8_t v2, uint8_t out[2])
{
    uint16_t ticks;
    uint8_t beats;
    switch (v2) {
    case 0x08: out[0] = c->on; return 1;       /* getter case 0x39: +0x49 */
    case 0x0A: out[0] = c->mode; return 1;     /* 0x3E: +0x4A */
    case 0x09: out[0] = c->rhythm; return 1;   /* 0x3C: +0x4B */
    case 0x03: out[0] = c->volume; return 1;   /* 0x3D: +0x50 */
    case 0x01:                                 /* 0x3A: +0x4E u16 -> [v >> 7, v & 0x7F] */
        out[0] = (uint8_t)((c->tempo >> 7) & 0x7F);
        out[1] = (uint8_t)(c->tempo & 0x7F);
        return 2;
    case 0x02:                                 /* 0x3B: +0x4C -> table entry [beats, 0x60 -> 2 / 0x30 -> 3] */
        kpm_beat_entry(c->beat, &ticks, &beats);
        out[0] = beats;
        out[1] = ticks == 0x60 ? 2 : 3;
        return 2;
    default:
        return 0;
    }
}

uint32_t kpm_store(const kpm_cfg_t *c, param_store_t *s)
{
    static const struct { param_id_t id; uint8_t v2; } K[] = {
        {PRM_METRONOME, 0x08}, {PRM_METRONOME_TEMPO, 0x01}, {PRM_BEAT_SELECT, 0x02},
        {PRM_RHYTHM_SELECT, 0x09}, {PRM_METRONOME_VOLUME, 0x03}, {PRM_METRONOME_MODE, 0x0A},
    };
    int slot = param_slot_from_part(MPART);
    uint32_t changed = 0;
    for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++) {
        uint8_t v[2];
        uint32_t n = kpm_value(c, K[i].v2, v);
        if (param_put(s, K[i].id, slot, v, (uint8_t)n))
            changed++;
    }
    return changed;
}
