/* OpenGrand — GPIO diagnostic (read only).  See gpiodiag.h. */
#include "ca79ng/gpiodiag.h"

#include <string.h>

static const struct {
    uint8_t word, bit;
} SIG[GD_SIGNALS] = {
    [GD_S_GP3_8] = {GD_W_IN23, 24},  [GD_S_GP3_1] = {GD_W_IN23, 17},  [GD_S_GP5_14] = {GD_W_IN45, 30},
    [GD_S_GP6_15] = {GD_W_OUT67, 15}, [GD_S_GP6_11] = {GD_W_OUT67, 11}, [GD_S_GP6_13] = {GD_W_OUT67, 13},
    [GD_S_GP7_12] = {GD_W_OUT67, 28}, [GD_S_GP4_15] = {GD_W_OUT45, 15}, [GD_S_GP5_15] = {GD_W_OUT45, 31},
    [GD_S_GP4_13] = {GD_W_OUT45, 13},
};

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

void gd_init(gd_t *g)
{
    memset(g, 0, sizeof *g);
}

uint8_t gd_level(unsigned sig, const uint32_t w[GD_WORDS])
{
    return sig < GD_SIGNALS ? (uint8_t)((w[SIG[sig].word] >> SIG[sig].bit) & 1u) : 0;
}

static uint16_t levels_of(const uint32_t w[GD_WORDS])
{
    uint16_t l = 0;
    for (unsigned i = 0; i < GD_SIGNALS; i++)
        l |= (uint16_t)(gd_level(i, w) << i);
    return l;
}

unsigned gd_sample(gd_t *g, uint32_t t_ms, const uint32_t w[GD_WORDS])
{
    uint16_t now = levels_of(w), diff;
    unsigned k = 0;
    g->samples++;
    if (!g->started) {
        g->started = true;
        g->levels = now;
        return 0;
    }
    diff = (uint16_t)(now ^ g->levels);
    g->levels = now;
    for (unsigned i = 0; diff && i < GD_SIGNALS; i++) {
        gd_event_t *e;
        if (!(diff & (1u << i)))
            continue;
        if (++g->seq == 0)
            g->seq = 1;
        e = &g->ring[g->head];
        e->seq = g->seq;
        e->t_ms = t_ms;
        e->sig = (uint8_t)i;
        e->level = (uint8_t)((now >> i) & 1u);
        g->head = (uint8_t)((g->head + 1u) % GD_RING);
        if (g->n < GD_RING)
            g->n++;
        k++;
    }
    return k;
}

const gd_event_t *gd_newest(const gd_t *g, unsigned k)
{
    if (k >= g->n)
        return NULL;
    return &g->ring[(g->head + GD_RING - 1u - k) % GD_RING];
}

void gd_encode_event(const gd_event_t *e, uint8_t out[GD_EVENT])
{
    put16(out, e->seq);
    put32(out + 2, e->t_ms);
    out[6] = e->sig;
    out[7] = e->level;
}

void gd_encode_snapshot(const gd_t *g, const gd_regs_t *r, uint8_t flags, uint8_t port, uint32_t t_ms,
                        uint8_t raw[GD_SNAP])
{
    memset(raw, 0, GD_SNAP);
    raw[0] = GD_LAYOUT;
    raw[1] = (uint8_t)((flags & GD_F_NOTIFY) | (g->started ? GD_F_STARTED : 0));
    raw[2] = port;
    put16(raw + 3, g->seq);
    for (unsigned p = 0; p < 3; p++) {
        put32(raw + 5 + 12 * p, r->dir[p]);
        put32(raw + 9 + 12 * p, r->out[p]);
        put32(raw + 13 + 12 * p, r->in[p]);
    }
    raw[41] = r->hp;
    raw[42] = r->gp3_level;
    raw[43] = r->gp3_event;
    raw[44] = r->model;
    raw[45] = r->chip_a;
    for (unsigned i = 0; i < GD_PINMUX_N; i++)
        put32(raw + 46 + 4 * i, r->pinmux[i]);
    put32(raw + 126, t_ms);
}

size_t gd_encode_log(const gd_t *g, uint8_t raw[GD_LOG_MAX])
{
    unsigned n = g->n;
    raw[0] = GD_LAYOUT;
    raw[1] = (uint8_t)n;
    put16(raw + 2, g->seq);
    for (unsigned i = 0; i < n; i++)   /* oldest first */
        gd_encode_event(gd_newest(g, n - 1u - i), raw + 4 + GD_EVENT * i);
    return 4u + GD_EVENT * n;
}
