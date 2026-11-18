/* OpenGrand cave port - Phase 4 executor gate.  See xgate_cave.h. */
#include "xgate_cave.h"

#include <stdbool.h>

#include "ca79ng/xgate.h"
#include "recctl_cave.h"   /* sxc_tx */
#include "sysex_cave.h"    /* sxc_ctrl_bound */

/* payload .data (explicit initialisers: nothing zeroes .bss), fresh with every payload copy.  Boot-on (owner decision
 * 2026-11-16): want starts 1, so the first tick - after attach() gave
 * the service - hooks the two v3 sites, as op 1F 01 01 would; 1F 01 00 still unhooks them (exactly stock) */
static xg_t gate = {0};
static volatile uint8_t want = 1;    /* written by task 19 (op), read by task 24 (tick) - one byte, one writer */
static uint8_t inited = 0;

static uint8_t rd(uint32_t a)
{
    return *(const volatile uint8_t *)(uintptr_t)a;
}

static void init(void)
{
    xg_init(&gate);
    inited = 1;
}

int xgc_media_pass(void)
{
    if (!inited)
        return 1;
    return xg_media(&gate, rd(XG_A_MEDIA), rd(XG_A_SEL_TYPE), rd(XG_A_REC_STATE)) == XG_RUN;
}

int xgc_xport_pass(void)
{
    if (!inited)
        return 1;
    return xg_xport(&gate, rd(XG_A_XCMD), rd(XG_A_FILEOP)) == XG_RUN;
}

void xgc_tick(void)
{
    if (!inited)
        init();
    if (want && !gate.on) {
        int m = xgh_set(0), x = xgh_set(1);
        gate.rc_media = (uint8_t)m, gate.rc_xport = (uint8_t)x;
        gate.hooked = (uint8_t)((m == 0 ? XG_HK_MEDIA : 0) | (x == 0 ? XG_HK_XPORT : 0));
        xg_enable(&gate, true);   /* refused sites stay stock: the gate is on, nothing reaches it */
    } else if (!want && gate.on) {
        if (gate.hooked & XG_HK_MEDIA)
            gate.rc_media = (uint8_t)xgh_clear(0);
        if (gate.hooked & XG_HK_XPORT)
            gate.rc_xport = (uint8_t)xgh_clear(1);
        gate.hooked = 0;
        xg_enable(&gate, false);
    }
}

static uint16_t pack7(const uint8_t *in, uint16_t n, uint8_t *out)
{
    uint16_t o = 0;
    for (uint16_t i = 0; i < n; i += 7) {
        uint16_t g = (uint16_t)(n - i < 7 ? n - i : 7);
        uint8_t hi = 0;
        for (uint16_t k = 0; k < g; k++)
            hi |= (uint8_t)(((in[i + k] >> 7) & 1u) << k);
        out[o++] = hi;
        for (uint16_t k = 0; k < g; k++)
            out[o++] = in[i + k] & 0x7F;
    }
    return o;
}

void xgc_sysex(uint8_t port, const uint8_t *buf, uint32_t len)
{
    uint8_t raw[XG_RAW], m[10 + XG_RAW + (XG_RAW + 6) / 7];
    uint8_t sub = len >= 9 ? buf[7] : 0, rc = XGC_RC_OK;
    uint16_t o = 0;
    port &= 3;
    if (len == 10 && sub == 1) {
        if (buf[8] > 1)
            rc = XGC_RC_ARG;
        else if (buf[8] == 1 && sxc_ctrl_bound(port) != 1)
            rc = XGC_RC_NOT_BOUND;   /* switching it on: a bound controller only; off: anyone */
        else
            want = buf[8];
    } else if (!(len == 8 || (len == 9 && sub == 0))) {
        rc = XGC_RC_ARG;
    }
    if (inited)
        xg_encode(&gate, (uint8_t)(XG_F_BUILT | (want ? XG_F_WANT : 0) | (xgh_have_svc() ? XG_F_SVC : 0)),
                  rd(XG_A_AUDIO), raw);
    else
        for (uint16_t i = 0; i < XG_RAW; i++)
            raw[i] = 0;
    m[o++] = 0xF0, m[o++] = 0x7D, m[o++] = 0x43, m[o++] = 0x41, m[o++] = 0x37, m[o++] = 0x39;
    m[o++] = XGC_OP | 0x40;
    m[o++] = sub == 1 ? 1 : 0;
    m[o++] = rc;
    o = (uint16_t)(o + pack7(raw, XG_RAW, m + o));
    m[o++] = 0xF7;
    sxc_tx(port, m, o);
}
