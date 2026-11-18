/* OpenGrand - Phase 4 (A) intake gate.  See kpgate.h; the stock tests re-stated here are FUN_4014A2C8
 * (0x4014A2C8..0x4014AC94, REA). */
#include "ca79ng/kpgate.h"

#include <string.h>

/* the stock's file replies (FUN_4014B8A0 request ACK, FUN_4014BEF0(0) Busy, FUN_4014DC0C(0x21) "no USB memory") */
static const uint8_t file_ack[] = {0xF0, 0x40, 0x7F, 0x24, 0x04, 0x1E, 0x7F, 0xF7};
static const uint8_t file_busy[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x03, 0x01, 0x00, 0x00, 0xF7};
static const uint8_t file_nomedia[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x03, 0x00, 0x00, 0x21, 0xF7};

void kpg_init(kpg_t *g, const kpg_io_t *io)
{
    memset(g, 0, sizeof *g);
    g->io = *io;
    g->enabled = true;
}

void kpg_enable(kpg_t *g, bool on)
{
    g->enabled = on;
}

uint16_t kpg_sys_frame(uint8_t sub, uint8_t sel, const uint8_t *data, uint8_t out[KPG_FRAME_MAX])
{
    uint16_t o = 0;
    out[o++] = 0xF0;
    out[o++] = 0x40;
    out[o++] = 0x7F;
    out[o++] = 0x7E;
    out[o++] = 0x04;
    out[o++] = 0x1E;
    out[o++] = sub;
    out[o++] = sel;
    for (uint8_t k = 0; k < KPG_INFO_MAX && data[k] != 0xFF; k++)
        out[o++] = data[k];
    out[o++] = 0xF7;
    return o;
}

/* FUN_4014AD04 routing for the binding: the panel with port 3 forwarding on gets 0, 1, 2 first */
static void out(kpg_t *g, uint8_t p, const uint8_t *m, uint16_t n)
{
    if (p == KPG_HOME && g->io.forward && g->io.forward(g->io.ctx))
        for (uint8_t q = 0; q < 3; q++)
            g->io.tx(g->io.ctx, q, m, n);
    g->io.tx(g->io.ctx, p, m, n);
}

static kpg_verdict_t pass(kpg_t *g)
{
    g->st.passed++;
    return KPG_PASS;
}

static kpg_verdict_t drop(kpg_t *g)
{
    g->st.dropped++;
    return KPG_DROP;
}

static void inc16(uint16_t *c)
{
    if (*c != 0xFFFFu)
        (*c)++;
}

/* fn 0x24 of the stock binding (0x4014A894..0x4014AA68).  Called only for rep: the gate FUN_40151BCC is the stock's. */
static kpg_verdict_t on_file(kpg_t *g, uint8_t p, uint8_t d0)
{
    if (d0 == 0x11) {   /* 0x4014A96C: ACK, mode 5 -> nothing more, FUN_4014F018: no media -> 0x21, list running -> Busy */
        if (g->io.mode(g->io.ctx) == 5) {
            out(g, p, file_ack, sizeof file_ack);
            inc16(&g->st.file_ack);
            return KPG_DONE;
        }
        if (!g->io.media(g->io.ctx)) {
            out(g, p, file_ack, sizeof file_ack);
            out(g, p, file_nomedia, sizeof file_nomedia);
            inc16(&g->st.file_err);
            return KPG_DONE;
        }
        if (g->io.file(g->io.ctx, KPG_FILE_LIST)) {
            out(g, p, file_ack, sizeof file_ack);
            out(g, p, file_busy, sizeof file_busy);
            inc16(&g->st.file_busy);
            return KPG_DONE;
        }
        return pass(g);
    }
    if ((d0 >= 0x04 && d0 <= 0x0B) || d0 == 0x12) {   /* 0x4014A9B0 / A98C / A950: ACK, then a pending job -> Busy */
        if (g->io.file(g->io.ctx, KPG_FILE_JOB)) {
            out(g, p, file_ack, sizeof file_ack);
            out(g, p, file_busy, sizeof file_busy);
            inc16(&g->st.file_busy);
            return KPG_DONE;
        }
        return pass(g);
    }
    switch (d0) {
    case 0x01: case 0x02: case 0x03:   /* TopOfData / data / FileName of a job: the stock checks and stores them */
    case 0x1F: case 0x7B:              /* list end ACK only / end of data                                         */
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:   /* ACK flags of the stock's reliable sender (7C: kpsess drops) */
        return pass(g);
    default:
        return drop(g);                /* 0x4014A8F8 / A934: nothing */
    }
}

/* fn 0x7E (0x4014A428..0x4014A834) */
static kpg_verdict_t on_sys(kpg_t *g, uint8_t p, const uint8_t *m, uint32_t n, bool rep)
{
    uint8_t sub = m[6], sel;
    if (sub == 0x01) {   /* CONNECT / DISCONNECT: m[7] 0 / 1 and F7 at m[8] (0x4014A6D8 / A800), the session core's */
        if (n < 9 || m[8] != 0xF7 || m[7] > 0x01)
            return drop(g);
        return pass(g);
    }
    if (sub == 0x02) {   /* 0x4014A6A4: binding, m[7] 03, FUN_4015BA64() != 0 -> FUN_4013D614(m[8]) */
        if (rep && m[7] == 0x03 && g->io.test_ok(g->io.ctx))
            return pass(g);
        inc16(&g->st.test_drop);
        return drop(g);
    }
    if (sub != 0x04 && sub != 0x05)
        return drop(g);  /* 7E 00 (0x4014A834 -> 0x4014AB78: nothing), 7E 03, others */
    if (n < 9 || m[7] < 0x40 || (m[7] & 0x3F) > 2)
        return drop(g);  /* 0x4014A470 / A4E8: < 0x40 nothing, & 0x3F > 2 nothing */
    if (rep)
        return pass(g);
    sel = (uint8_t)(m[7] & 0x3F);
    /* deliberate difference: a bound controller that is not the stock binding gets the stock's answer from the core.
     * 7E 04 02 (FUN_400B0D68: a checksum over 4 MB of NOR, 128 x 32 KB reads in task 19) stays the binding's. */
    if (g->io.info && g->io.bound(g->io.ctx, p) && !(sub == 0x04 && sel == 0x02)) {
        uint8_t d[KPG_INFO_MAX + 1u], f[KPG_FRAME_MAX];
        memset(d, 0xFF, sizeof d);
        if (g->io.info(g->io.ctx, sub, sel, d) > 0) {
            out(g, p, f, kpg_sys_frame(sub, sel, d, f));
            inc16(&g->st.info_own);
            return KPG_DONE;
        }
    }
    inc16(&g->st.other_drop);
    return drop(g);
}

kpg_verdict_t kpg_rx(kpg_t *g, uint8_t port, const uint8_t *m, uint32_t n)
{
    uint8_t fn;
    uint32_t dev;
    bool rep;
    if (!g->enabled || port >= KPG_PORTS || n < 7 || m[0] != 0xF0 || m[1] != 0x40)
        return KPG_PASS;
    fn = m[3];
    if (fn == 0x70 || fn == 0x60)
        return pass(g);   /* before the device test (0x4014A2DC / A2FC): flash tools left as they are, identity */
    dev = g->io.device(g->io.ctx);
    if (dev != m[2] && m[2] != 0x7F)
        return drop(g);   /* 0x4014A304..A310 */
    if (m[4] == 0x71)     /* 0x4014AC50: 71 02 from port 0 -> the stock binds port 0 and runs FUN_401516BC */
        return m[5] == 0x02 && port == 0 ? pass(g) : drop(g);
    if (m[4] != 0x08 || m[5] != 0x02)
        return drop(g);   /* 0x4014A35C..A36C */
    if (m[6] == 0x5A || (m[6] == 0x50 && (m[7] == 0x0C || m[7] == 0x0E || m[7] == 0x10 || m[7] == 0x17)))
        return pass(g);   /* VT edit (0x4014A370..A3B4): the stock binding's, not judged here */
    rep = g->io.binding(g->io.ctx) == (uint8_t)(port + 1);
    switch (fn) {
    case 0x02: case 0x04: case 0x05: case 0x06: case 0x07:   /* the session core judged them (kpsess) */
    case 0x10: case 0x31: case 0x32:                         /* the front / the commands                */
        return pass(g);
    case 0x03:            /* 0x4014AB28: binding, n 10 -> FUN_4015D5D4 */
        if (rep && n == 10)
            return pass(g);
        inc16(&g->st.other_drop);
        return drop(g);
    case 0x09:            /* 0x4014AAA4: binding -> FUN_4015D5A8 */
        if (rep)
            return pass(g);
        inc16(&g->st.other_drop);
        return drop(g);
    case 0x24:            /* 0x4014A894: binding only (a non-binding controller's job: the session core's file lease) */
        return rep ? on_file(g, port, m[6]) : drop(g);
    case 0x7E:
        return on_sys(g, port, m, n, rep);
    default:
        return drop(g);   /* 08, 0A-0F, 11-23, 25-30, 33-6F, 71-7D, 7F, 00, 01: no branch does anything */
    }
}
