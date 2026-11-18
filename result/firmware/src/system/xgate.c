/* OpenGrand — executor gate (Phase 4 A): when the stock media / audio-transport executors run.  See xgate.h. */
#include "ca79ng/xgate.h"

#include <string.h>

void xg_init(xg_t *g)
{
    memset(g, 0, sizeof *g);
    g->rc_media = g->rc_xport = 0xFF;
}

void xg_enable(xg_t *g, bool on)
{
    g->on = on;
    if (!on)
        g->m_held = 0, g->x_held = 0, g->x_wait = 0;
}

static bool recording(uint8_t st)
{
    return st >= 3 && st <= 6;   /* record standby (3, 5), recording (4), count-in (6) */
}

int xg_media(xg_t *g, uint8_t media, uint8_t sel_type, uint8_t rec_state)
{
    if (!g->on)
        return XG_RUN;
    if (!(media & XG_PENDING)) {
        g->m_held = 0;   /* nothing pending (or somebody else ran it): nothing held */
        return XG_RUN;
    }
    g->last_media = media;
    if ((media & 0x7F) == 0 && sel_type == XG_TYPE_KSO && recording(rec_state)) {
        g->m_held = 1;
        g->why_media = XG_WHY_REC;
        g->media_defer++;
        return XG_DEFER;
    }
    if (g->m_held)
        g->media_release++;
    g->m_held = 0;
    g->why_media = XG_WHY_NONE;
    g->media_run++;
    return XG_RUN;
}

int xg_xport(xg_t *g, uint8_t cmd, uint8_t fileop)
{
    if (!g->on)
        return XG_RUN;
    if (!(cmd & XG_PENDING)) {
        g->x_held = 0, g->x_wait = 0;
        return XG_RUN;
    }
    g->last_cmd = cmd;
    if ((cmd & 0x7F) == XG_OP_STOP && fileop != 0) {
        if (!g->x_held || g->x_cmd != cmd)
            g->x_held = 1, g->x_cmd = cmd, g->x_wait = 0;
        if (g->x_wait < XG_XPORT_CAP) {
            g->x_wait++;
            g->why_xport = XG_WHY_FILEOP;
            g->xport_defer++;
            return XG_DEFER;
        }
        g->xport_capped++;   /* held long enough: the stock answers Busy as it always did */
        g->why_xport = XG_WHY_CAP;
        g->x_held = 0, g->x_wait = 0;
        g->xport_run++;
        return XG_RUN;
    }
    if (g->x_held)
        g->xport_release++;
    g->x_held = 0, g->x_wait = 0;
    g->why_xport = XG_WHY_NONE;
    g->xport_run++;
    return XG_RUN;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }

void xg_encode(const xg_t *g, uint8_t flags, uint8_t audio_state, uint8_t raw[XG_RAW])
{
    memset(raw, 0, XG_RAW);
    raw[0] = XG_LAYOUT;
    raw[1] = (uint8_t)((flags & (XG_F_BUILT | XG_F_WANT | XG_F_SVC)) | (g->on ? XG_F_ON : 0) | (g->hooked & 3u) << 2);
    raw[2] = g->rc_media;
    raw[3] = g->rc_xport;
    raw[4] = g->last_media;
    raw[5] = g->last_cmd;
    raw[6] = g->why_media;
    raw[7] = g->why_xport;
    raw[8] = (uint8_t)((g->m_held ? 1 : 0) | (g->x_held ? 2 : 0));
    raw[9] = audio_state;
    put16(raw + 10, g->x_wait);
    put32(raw + 12, g->media_run);
    put32(raw + 16, g->media_defer);
    put32(raw + 20, g->xport_run);
    put32(raw + 24, g->xport_defer);
    put16(raw + 28, g->media_release);
    put16(raw + 30, g->xport_release);
    put16(raw + 32, g->xport_capped);
}
