/* OpenGrand cave port - GPIO diagnostic, read only.  See gpio_cave.h. */
#include "gpio_cave.h"

#include "ca79ng/gpiodiag.h"
#include "hal_cave.h"      /* hal_time_us, hal_midi_tx, cave_port_from_stock, CAVE_RW */
#include "sysex_cave.h"    /* sxc_ctrl_bound */
#include "bt_cave.h"       /* btc_ctrl_bound */
#include "power_stock.h"   /* stk_binding */

/* the change log in the core's NOLOAD RW (set up by gd_init on the first pass); the flags in payload .data (explicit
 * initialisers: nothing zeroes .bss) - fresh with every payload copy, notices off at every boot */
CAVE_RW static gd_t log_;
static uint8_t inited = 0;
static uint8_t notify = 0;            /* written by task 24 only */
static uint8_t notify_port = 0x7F;
static uint16_t notified = 0;         /* seq of the last event sent in a notice */
static uint32_t last_notice = 0;
static uint8_t sent_once = 0;
static uint32_t ms = 0, last_us = 0, acc_us = 0;
/* the one queued request: task 19 fills it and raises `req`, task 24 answers and lowers it */
static volatile uint8_t req = 0;
static volatile uint8_t req_port = 0, req_sub = 0, req_arg = 0;

/* the rule of the power ops and the BT port's changes: any controller the session core has bound, else the stock binding */
static int bound(uint8_t port) { return btc_ctrl_bound(sxc_ctrl_bound(port), stk_binding, port) != 0; }

static uint32_t rd32(uint32_t a) { return *(const volatile uint32_t *)(uintptr_t)a; }
static uint8_t rd8(uint32_t a) { return *(const volatile uint8_t *)(uintptr_t)a; }

static void words(uint32_t w[GD_WORDS])
{
    w[GD_W_IN23] = rd32(GD_A_IN23);
    w[GD_W_IN45] = rd32(GD_A_IN45);
    w[GD_W_OUT45] = rd32(GD_A_OUT45);
    w[GD_W_OUT67] = rd32(GD_A_OUT67);
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

/* F0 7D 'C' 'A' '7' '9' 57 <sub> [head bytes] <pack7 raw> F7 */
static void send(uint8_t port, uint8_t sub, const uint8_t *head, uint8_t nh, const uint8_t *raw, uint16_t n)
{
    uint8_t m[9 + 3 + GD_SNAP + (GD_SNAP + 6) / 7];
    uint16_t o = 0;
    m[o++] = 0xF0, m[o++] = 0x7D, m[o++] = 0x43, m[o++] = 0x41, m[o++] = 0x37, m[o++] = 0x39;
    m[o++] = GDC_OP | 0x40;
    m[o++] = sub;
    for (uint8_t i = 0; i < nh; i++)
        m[o++] = head[i] & 0x7F;
    if (n)
        o = (uint16_t)(o + pack7(raw, n, m + o));
    m[o++] = 0xF7;
    hal_midi_tx(cave_port_from_stock(port), m, o);
}

static void snapshot(uint8_t port)
{
    gd_regs_t r;
    uint8_t raw[GD_SNAP];
    static const uint32_t DIR[3] = {GD_A_DIR23, GD_A_DIR45, GD_A_DIR67};
    static const uint32_t OUT[3] = {GD_A_OUT23, GD_A_OUT45, GD_A_OUT67};
    static const uint32_t IN[3] = {GD_A_IN23, GD_A_IN45, GD_A_IN67};
    for (unsigned p = 0; p < 3; p++) {
        r.dir[p] = rd32(DIR[p]);
        r.out[p] = rd32(OUT[p]);
        r.in[p] = rd32(IN[p]);
    }
    r.hp = rd8(GD_A_HP);
    r.gp3_level = rd8(GD_A_GP3_LEVEL);
    r.gp3_event = rd8(GD_A_GP3_EVENT);
    r.model = rd8(GD_A_MODEL);
    r.chip_a = rd8(GD_A_CHIPA);
    for (unsigned i = 0; i < GD_PINMUX_N; i++)
        r.pinmux[i] = rd32(GD_A_PINMUX0 + 4u * i);
    gd_encode_snapshot(&log_, &r, notify ? GD_F_NOTIFY : 0, notify ? notify_port : 0x7F, ms, raw);
    send(port, 0x00, 0, 0, raw, GD_SNAP);
}

static void answer(void)
{
    uint8_t port = req_port;
    if (req_sub == 0x00) {
        snapshot(port);
    } else if (req_sub == 0x01) {
        uint8_t raw[GD_LOG_MAX];
        send(port, 0x01, 0, 0, raw, (uint16_t)gd_encode_log(&log_, raw));
    } else {
        uint8_t h[3];
        h[0] = GDC_RC_OK;
        if (req_arg > 1) {
            h[0] = GDC_RC_ARG;
        } else if (req_arg == 1) {
            if (!bound(port)) {
                h[0] = GDC_RC_NOT_BOUND;
            } else {
                notify = 1, notify_port = port;
                notified = log_.seq;   /* only what changes from now on */
                sent_once = 0;
            }
        } else {
            notify = 0, notify_port = 0x7F;
        }
        h[1] = (uint8_t)((notify ? GD_F_NOTIFY : 0) | (log_.started ? GD_F_STARTED : 0));
        h[2] = notify ? notify_port : 0x7F;
        send(port, 0x02, h, 3, 0, 0);
    }
}

static void notice(void)
{
    uint16_t pend = (uint16_t)(log_.seq - notified);
    uint8_t raw[3 + GDC_NOTICE_MAX * GD_EVENT];
    unsigned n;
    if (!pend || (sent_once && ms - last_notice < GDC_NOTICE_MS))
        return;
    if (!bound(notify_port)) {   /* the controller left: notices end with it */
        notify = 0, notify_port = 0x7F;
        return;
    }
    n = pend < GDC_NOTICE_MAX ? pend : GDC_NOTICE_MAX;
    if (n > log_.n)
        n = log_.n;
    raw[0] = (uint8_t)n;
    raw[1] = (uint8_t)log_.seq;
    raw[2] = (uint8_t)(log_.seq >> 8);
    for (unsigned i = 0; i < n; i++)
        gd_encode_event(gd_newest(&log_, n - 1u - i), raw + 3 + GD_EVENT * i);
    send(notify_port, 0x7F, 0, 0, raw, (uint16_t)(3 + GD_EVENT * n));
    notified = log_.seq;
    last_notice = ms;
    sent_once = 1;
}

void gdc_tick(void)
{
    uint32_t now = hal_time_us(), w[GD_WORDS];
    if (!inited) {
        gd_init(&log_);
        last_us = now;
        inited = 1;
    } else {
        acc_us += now - last_us;
        last_us = now;
        ms += acc_us / 1000u;
        acc_us %= 1000u;
    }
    words(w);
    gd_sample(&log_, ms, w);
    if (req) {
        answer();
        req = 0;
    }
    if (notify)
        notice();
}

void gdc_sysex(uint8_t port, const uint8_t *buf, uint32_t len)
{
    uint8_t sub = len >= 9 ? buf[7] : 0;
    if (req)
        return;   /* busy: dropped (silent) */
    if (!(len == 8 || (len == 9 && sub <= 0x01) || (len == 10 && sub == 0x02)))
        return;   /* unknown form: silent, like an unknown op */
    req_port = port & 3u;
    req_sub = sub;
    req_arg = len == 10 ? buf[8] : 0;
    req = 1;
}
