/* Phase 3-2 session core, stage 1 (src/kproto/kpsess.c) against a fake stock: bindings, CONNECT /
 * DISCONNECT, the request sender on 32-bit time, ACK / NAK / Wait / Cancel assignment. */
#include <string.h>

#include "ca79ng/kpsess.h"
#include "test.h"

static struct {
    uint8_t binding;
    uint32_t device;
    bool fwd;
    bool link[4];
    int ntx;
    uint8_t txp[64];
    uint8_t tx[64][KPS_FRAME_MAX];
    uint16_t txn[64];
    uint32_t matchmask[6];   /* descriptor bits answering any request */
    int frames_of[KPS_DESC_N];
    int take_rep, post, unbind, cleared;
    uint8_t cleared_port;
    uint8_t flag[4];
} fk;

static uint8_t f_binding(void *c) { (void)c; return fk.binding; }
static uint32_t f_device(void *c) { (void)c; return fk.device; }
static bool f_forward(void *c) { (void)c; return fk.fwd; }
static void f_tx(void *c, uint8_t p, const uint8_t *m, uint16_t n)
{
    (void)c;
    if (fk.ntx < 64) {
        fk.txp[fk.ntx] = p;
        memcpy(fk.tx[fk.ntx], m, n);
        fk.txn[fk.ntx] = n;
    }
    fk.ntx++;
}
static bool f_match(void *c, uint8_t i, uint8_t v1, uint8_t v2)
{
    (void)c;
    (void)v1;
    (void)v2;
    return (fk.matchmask[i / 32] >> (i % 32)) & 1u;
}
static int f_value(void *c, uint8_t port, uint8_t cc, uint8_t i, uint8_t part, uint8_t j, uint8_t out[KPS_FRAME_MAX])
{
    (void)c;
    (void)port;
    if (j >= fk.frames_of[i])
        return -1;
    out[0] = 0xF0; out[1] = 0x40; out[2] = cc; out[3] = 0x10; out[4] = 0x04; out[5] = 0x1E;
    out[6] = i; out[7] = j; out[8] = part; out[9] = 0x11; out[10] = 0xF7;
    return 11;
}
static void f_connect_info(void *c, uint8_t p, uint8_t *mode, uint8_t *sub)
{
    (void)c;
    *mode = 0;
    *sub = fk.flag[p] ? 0x00 : 0x40;
    fk.flag[p] = 0;
}
static void f_take_rep(void *c, uint8_t p) { (void)c; fk.take_rep++; fk.binding = (uint8_t)(p + 1); }
static void f_post(void *c) { (void)c; fk.post++; }
static void f_unbind(void *c) { (void)c; fk.unbind++; fk.binding = 0; }
static void f_clear(void *c, uint8_t p) { (void)c; fk.cleared++; fk.cleared_port = p; }
static bool f_link(void *c, uint8_t p) { (void)c; return fk.link[p]; }

static const kps_io_t IO = {
    .ctx = 0, .binding = f_binding, .device = f_device, .forward = f_forward, .tx = f_tx, .match = f_match,
    .value = f_value, .connect_info = f_connect_info, .take_rep = f_take_rep, .connect_post = f_post,
    .stock_unbind = f_unbind, .remote_clear = f_clear, .link_up = f_link,
};

static kps_t S;
static uint32_t T;

static void reset(uint8_t binding)
{
    memset(&fk, 0, sizeof fk);
    for (int p = 0; p < 4; p++)
        fk.link[p] = true;
    fk.binding = binding;
    T = 100000;
    kps_init(&S, &IO, T);
}

static void frame(uint8_t *f, uint8_t fn, const uint8_t *body, int nb, uint32_t *n)
{
    f[0] = 0xF0; f[1] = 0x40; f[2] = 0x7F; f[3] = fn; f[4] = 0x08; f[5] = 0x02;
    memcpy(f + 6, body, (size_t)nb);
    f[6 + nb] = 0xF7;
    *n = (uint32_t)(7 + nb);
}

static kps_verdict_t rx(uint8_t port, uint8_t fn, const uint8_t *body, int nb)
{
    uint8_t f[48];
    uint32_t n;
    frame(f, fn, body, nb, &n);
    return kps_rx(&S, port, f, n, T);
}

static kps_verdict_t ack_last(uint8_t port, uint8_t fn)
{
    int k = fk.tx[fk.ntx - 1][6] == 0x70 ? 2 : 3;
    return rx(port, fn, fk.tx[fk.ntx - 1] + 6, k);
}

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i += 2) {
        T += 2;
        kps_tick(&S, T);
    }
}

TEST(kps_clock_wraps)
{
    kps_clock_t c = {0};
    uint32_t a = kps_clock(&c, 0xFF00);
    uint32_t b = kps_clock(&c, 0xFFFF);
    uint32_t d = kps_clock(&c, 0x0005);   /* wrapped */
    CHECK_EQ(b - a, 0xFF * 2);
    CHECK_EQ(d - b, 6 * 2);
    CHECK(d > b);
}

TEST(kps_connect_pass_and_own)
{
    static const uint8_t con[] = {0x01, 0x01};
    reset(0);
    CHECK_EQ(rx(2, 0x7E, con, 2), KPS_PASS);   /* unbound: the stock binds and answers */
    fk.binding = 3;
    CHECK(kps_bound(&S, 2));
    CHECK_EQ(rx(2, 0x7E, con, 2), KPS_PASS);   /* same port: the stock's 00 40 */
    /* the panel CONNECTs while the app is bound: the core answers, nobody displaced */
    fk.flag[3] = 1;
    CHECK_EQ(rx(3, 0x7E, con, 2), KPS_DONE);
    CHECK_EQ(fk.ntx, 1);
    CHECK_EQ(fk.txp[0], 3);
    {
        static const uint8_t want[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x02, 0x00, 0x00, 0xF7};
        CHECK_EQ(fk.txn[0], 10);
        CHECK_MEM(fk.tx[0], want, 10);
    }
    CHECK_EQ(fk.take_rep, 1);
    CHECK_EQ(fk.post, 1);
    CHECK_EQ(fk.binding, 4);
    CHECK(kps_bound(&S, 2) && kps_bound(&S, 3));
    CHECK_EQ(S.st.conn_own, 1);
    CHECK_EQ(S.st.conn_pass, 2);
    /* the app CONNECTs again: answered, rep back to it, panel stays */
    CHECK_EQ(rx(2, 0x7E, con, 2), KPS_DONE);
    CHECK_EQ(fk.binding, 3);
    CHECK_EQ(fk.tx[1][8], 0x40);
    CHECK(kps_bound(&S, 3));
}

TEST(kps_disconnect)
{
    static const uint8_t con[] = {0x01, 0x01}, dis[] = {0x01, 0x00};
    reset(3);   /* app bound */
    rx(3, 0x7E, con, 2);   /* panel, core-answered -> binding 4 */
    CHECK_EQ(fk.binding, 4);
    CHECK_EQ(rx(2, 0x7E, dis, 2), KPS_DONE);   /* the app only the core binds */
    CHECK(!kps_bound(&S, 2));
    CHECK_EQ(S.st.disc_own, 1);
    CHECK_EQ(rx(1, 0x7E, dis, 2), KPS_PASS);   /* never bound: the stock ignores it */
    CHECK_EQ(rx(3, 0x7E, dis, 2), KPS_PASS);   /* the stock binding: the stock's own path */
    CHECK(!kps_bound(&S, 3));
    CHECK_EQ(S.st.unbinds, 0);
}

static void setup_dump(void)
{
    reset(3);
    fk.matchmask[0] = (1u << 1) | (1u << 2) | (1u << 5);
    fk.matchmask[0x69 / 32] |= 1u << (0x69 % 32);   /* 0x69 is skipped regardless */
    fk.frames_of[1] = 1;
    fk.frames_of[2] = 2;
    fk.frames_of[5] = 0;   /* answers, but no value */
    fk.frames_of[0x69] = 1;
}

TEST(kps_request_dump_in_order)
{
    static const uint8_t req[] = {0x5F, 0x7F, 0x7F};
    setup_dump();
    CHECK_EQ(rx(2, 0x02, req, 3), KPS_DONE);
    CHECK_EQ(fk.ntx, 1);
    CHECK_EQ(fk.tx[0][6], 1);
    CHECK_EQ(rx(2, 0x02, req, 3), KPS_DONE);   /* busy: dropped */
    CHECK_EQ(S.st.dropped, 1);
    CHECK_EQ(fk.ntx, 1);
    CHECK_EQ(ack_last(2, 0x07), KPS_DONE);
    CHECK_EQ(fk.ntx, 2);
    CHECK(fk.tx[1][6] == 2 && fk.tx[1][7] == 0);
    ack_last(2, 0x07);
    CHECK(fk.tx[2][6] == 2 && fk.tx[2][7] == 1);
    ack_last(2, 0x07);
    {   /* End F0 40 7F 03 04 1E 5F 7F 7F F7 */
        static const uint8_t end[] = {0xF0, 0x40, 0x7F, 0x03, 0x04, 0x1E, 0x5F, 0x7F, 0x7F, 0xF7};
        CHECK_EQ(fk.ntx, 4);
        CHECK_MEM(fk.tx[3], end, 10);
    }
    CHECK_EQ(fk.cleared, 0);
    ack_last(2, 0x07);
    CHECK_EQ(fk.cleared, 1);
    CHECK_EQ(fk.cleared_port, 2);
    CHECK(!S.port[2].active);
    CHECK_EQ(S.st.values, 3);
    CHECK_EQ(S.st.ends, 1);
    CHECK_EQ(S.st.acks, 4);
    run_ms(3000);
    CHECK_EQ(fk.ntx, 4);   /* nothing more */
}

TEST(kps_retry_and_fail)
{
    static const uint8_t req[] = {0x5F, 0x7F, 0x7F};
    setup_dump();
    rx(2, 0x02, req, 3);
    run_ms(498);
    CHECK_EQ(fk.ntx, 1);   /* 500 ms per try */
    run_ms(2);
    CHECK_EQ(fk.ntx, 2);
    CHECK_MEM(fk.tx[1], fk.tx[0], 11);
    run_ms(500 * 14);
    CHECK_EQ(fk.ntx, 16);
    CHECK_EQ(fk.unbind, 0);
    run_ms(500);
    /* 16 sends failed: the stock binding (the app) released, the panel invited, no End */
    CHECK_EQ(fk.unbind, 1);
    CHECK(!kps_bound(&S, 2));
    CHECK_EQ(S.st.failed, 1);
    CHECK_EQ(fk.ntx, 17);
    CHECK_EQ(fk.txp[16], 3);
    CHECK_EQ(fk.tx[16][7], 0x01);
    CHECK_EQ(fk.cleared, 0);
}

TEST(kps_panel_never_released)
{
    static const uint8_t req[] = {0x5F, 0x7F, 0x7F};
    setup_dump();
    fk.binding = 4;
    kps_tick(&S, T);   /* sync: the panel bound by the stock */
    rx(3, 0x02, req, 3);
    run_ms(500 * 17);
    CHECK_EQ(S.st.failed, 1);
    CHECK(kps_bound(&S, 3));
    CHECK_EQ(fk.unbind, 0);
}

TEST(kps_no_wrap_burst)
{
    /* the stock's 16-bit deadline: a wait starting in the last 250 ticks times out at once.  Time here is
     * the 32-bit clock fed by the same counter, so a frame is resent only 500 ms later */
    static const uint8_t req[] = {0x5F, 0x7F, 0x7F};
    kps_clock_t c = {0};
    uint16_t tick = 0xFF06;
    setup_dump();
    T = kps_clock(&c, tick);
    kps_tick(&S, T);
    rx(2, 0x02, req, 3);
    for (int i = 0; i < 249; i++) {   /* 498 ms across the wrap */
        tick++;
        T = kps_clock(&c, tick);
        kps_tick(&S, T);
    }
    CHECK_EQ(fk.ntx, 1);
    tick++;
    T = kps_clock(&c, tick);
    kps_tick(&S, T);
    CHECK_EQ(fk.ntx, 2);
    CHECK_EQ(S.st.timeouts, 1);
}

TEST(kps_nak_wait)
{
    static const uint8_t req[] = {0x5F, 0x7F, 0x7F};
    setup_dump();
    rx(2, 0x02, req, 3);
    CHECK_EQ(ack_last(2, 0x06), KPS_DONE);   /* NAK: resend after 5 ms */
    run_ms(4);
    CHECK_EQ(fk.ntx, 1);
    run_ms(2);
    CHECK_EQ(fk.ntx, 2);
    CHECK_EQ(S.st.naks, 1);
    CHECK_EQ(S.st.timeouts, 0);
    run_ms(400);
    CHECK_EQ(ack_last(2, 0x04), KPS_DONE);   /* Wait: 500 ms more, no try spent */
    run_ms(400);
    CHECK_EQ(fk.ntx, 2);
    run_ms(100);
    CHECK_EQ(fk.ntx, 3);
    CHECK_EQ(S.st.waits, 1);
    CHECK_EQ(S.port[2].tries, 3);
}

TEST(kps_ack_assignment)
{
    static const uint8_t other[] = {0x10, 0x00, 0x00}, req[] = {0x5F, 0x7F, 0x7F}, con[] = {0x01, 0x01};
    reset(3);
    /* the stock binding, no core frame in flight: ACK / NAK to the stock, Wait never */
    CHECK_EQ(rx(2, 0x07, other, 3), KPS_PASS);
    CHECK_EQ(rx(2, 0x06, other, 3), KPS_PASS);
    CHECK_EQ(rx(2, 0x04, other, 3), KPS_DONE);
    CHECK_EQ(S.st.wait_drop, 1);
    {   /* fn 24 7C: file Wait, dropped; 24 7F to the stock */
        static const uint8_t w[] = {0x7C}, a[] = {0x7F};
        CHECK_EQ(rx(2, 0x24, w, 1), KPS_DONE);
        CHECK_EQ(rx(2, 0x24, a, 1), KPS_PASS);
    }
    /* the panel joins (core-bound, now the stock binding); the app is core-only: its stray ACK dropped */
    rx(3, 0x7E, con, 2);
    CHECK_EQ(rx(2, 0x07, other, 3), KPS_DONE);
    CHECK_EQ(S.st.ack_drop, 1);
    /* a core request on the app's port: its ACK is the core's even with another key in the stock */
    setup_dump();
    rx(2, 0x02, req, 3);
    CHECK_EQ(ack_last(2, 0x07), KPS_DONE);
    CHECK_EQ(S.st.acks, 1);
    S.st.acks = 0;
    /* v1 0x70 keys are two bytes */
    {
        uint8_t f[16];
        uint32_t n;
        static const uint8_t k3[] = {0x70, 0x01, 0x00};
        S.port[1].busy = true;
        memcpy(S.port[1].frame, "\xF0\x40\x7F\x10\x04\x1E\x70\x01\x05\x00\xF7", 11);
        S.bound |= 2;
        frame(f, 0x07, k3, 2, &n);
        CHECK_EQ(kps_rx(&S, 1, f, n, T), KPS_DONE);
        CHECK(!S.port[1].busy);
        CHECK_EQ(S.st.acks, 1);
    }
}

TEST(kps_cancel)
{
    static const uint8_t req[] = {0x5F, 0x7F, 0x7F};
    setup_dump();
    rx(2, 0x02, req, 3);
    CHECK_EQ(rx(2, 0x05, NULL, 0), KPS_PASS);   /* from the stock binding: passed too */
    ack_last(2, 0x07);   /* descriptor 1 done -> Cancel seen, no End */
    CHECK(!S.port[2].active);
    CHECK_EQ(fk.ntx, 1);
    CHECK_EQ(S.st.cancels, 1);
}

TEST(kps_packed_and_unbound)
{
    static const uint8_t packed[] = {0x5F, 0x00, 0x7F}, req[] = {0x5F, 0x7F, 0x7F}, con[] = {0x01, 0x01};
    setup_dump();
    CHECK_EQ(rx(2, 0x02, packed, 3), KPS_PASS);   /* the stock binding: FUN_401525D4 */
    CHECK_EQ(rx(1, 0x02, req, 3), KPS_DONE);      /* not bound: dropped like the stock */
    CHECK_EQ(fk.ntx, 0);
    rx(3, 0x7E, con, 2);
    CHECK_EQ(rx(2, 0x02, packed, 3), KPS_DONE);   /* core-only port: no packed dump */
    /* the other functions pass */
    {
        static const uint8_t set[] = {0x55, 0x07, 0x00, 0x01};
        CHECK_EQ(rx(2, 0x10, set, 4), KPS_PASS);
    }
    /* the device byte is the stock's */
    {
        uint8_t f[16];
        uint32_t n;
        frame(f, 0x02, req, 3, &n);
        f[2] = 0x05;
        CHECK_EQ(kps_rx(&S, 2, f, n, T), KPS_PASS);
    }
}

TEST(kps_link_down_and_sync)
{
    static const uint8_t con[] = {0x01, 0x01};
    reset(3);
    rx(3, 0x7E, con, 2);   /* panel rep, app core-only */
    fk.link[2] = false;
    kps_tick(&S, T);
    CHECK(!kps_bound(&S, 2));
    CHECK_EQ(S.st.unbinds, 1);
    /* the stock binds port 0 on its own (factory frame) and later drops it */
    fk.binding = 1;
    kps_tick(&S, T);
    CHECK(kps_bound(&S, 0));
    fk.binding = 0;
    kps_tick(&S, T);
    CHECK(!kps_bound(&S, 0));
    CHECK(kps_bound(&S, 3));
}

TEST(kps_disabled_passes)
{
    static const uint8_t req[] = {0x5F, 0x7F, 0x7F};
    setup_dump();
    rx(2, 0x02, req, 3);
    kps_enable(&S, false);
    CHECK(!S.port[2].busy);
    CHECK_EQ(rx(2, 0x02, req, 3), KPS_PASS);
    CHECK_EQ(rx(2, 0x04, req, 3), KPS_PASS);
    kps_enable(&S, true);
    CHECK_EQ(rx(2, 0x02, req, 3), KPS_DONE);
}

TEST(kps_forwarding)
{
    static const uint8_t req[] = {0x5F, 0x7F, 0x7F};
    setup_dump();
    fk.binding = 4;
    fk.fwd = true;
    kps_tick(&S, T);
    rx(3, 0x02, req, 3);
    CHECK_EQ(fk.ntx, 4);
    CHECK(fk.txp[0] == 0 && fk.txp[1] == 1 && fk.txp[2] == 2 && fk.txp[3] == 3);
}

/* ---- stage 2: fan-out ---------------------------- */

static struct {
    uint8_t key[3];
    uint8_t val;
} kv[6];
static int kv_calls;

static int f_key_value(void *c, uint8_t v1, uint8_t v2, uint8_t part, uint8_t k, uint8_t out[KPS_FRAME_MAX])
{
    (void)c;
    kv_calls++;
    if (k)
        return 0;
    for (unsigned i = 0; i < sizeof kv / sizeof kv[0]; i++)
        if (kv[i].key[0] == v1 && kv[i].key[1] == v2 && kv[i].key[2] == part) {
            out[0] = 0xF0; out[1] = 0x40; out[2] = 0x7F; out[3] = 0x10; out[4] = 0x04; out[5] = 0x1E;
            out[6] = v1; out[7] = v2; out[8] = part; out[9] = kv[i].val; out[10] = 0xF7;
            return 11;
        }
    return 0;
}

static kps_io_t IOF;

/* app (2) and panel (3) both bound, the panel holds the stock binding (it CONNECTed last) */
static void setup_two(void)
{
    static const uint8_t con[] = {0x01, 0x01};
    IOF = IO;
    IOF.key_value = f_key_value;
    memset(&fk, 0, sizeof fk);
    memset(kv, 0, sizeof kv);
    for (int p = 0; p < 4; p++)
        fk.link[p] = true;
    T = 100000;
    kps_init(&S, &IOF, T);
    rx(2, 0x7E, con, 2);
    fk.binding = 3;
    rx(3, 0x7E, con, 2);
    kv[0].key[0] = 0x55; kv[0].key[1] = 0x08; kv[0].key[2] = 0x00; kv[0].val = 0;
    kv[1].key[0] = 0x55; kv[1].key[1] = 0x07; kv[1].key[2] = 0x00; kv[1].val = 1;
    kv[2].key[0] = 0x56; kv[2].key[1] = 0x01; kv[2].key[2] = 0x7F; kv[2].val = 0x40;
    fk.ntx = 0;
    kv_calls = 0;
}

/* a set from port p of key kv[i] to value v, applied by the "stock" between begin and end */
static void set_from(uint8_t p, int i, uint8_t v)
{
    uint8_t m[11];
    uint32_t n;
    uint8_t body[4] = {kv[i].key[0], kv[i].key[1], kv[i].key[2], v};
    frame(m, 0x10, body, 4, &n);
    kps_set_begin(&S, p, m, n);
    kv[i].val = v;
    kps_set_end(&S);
}

static int sent_to(uint8_t p)
{
    int k = 0;
    for (int i = 0; i < fk.ntx && i < 64; i++)
        if (fk.txp[i] == p)
            k++;
    return k;
}

TEST(kps_fan_set_to_others_only)
{
    setup_two();
    CHECK(kps_bound(&S, 2) && kps_bound(&S, 3));
    CHECK_EQ(fk.binding, 4);
    set_from(2, 0, 4);                        /* the app sets reverb type */
    CHECK_EQ(fk.ntx, 1);
    CHECK_EQ(fk.txp[0], 3);                   /* to the panel, never back to the app */
    CHECK_EQ(fk.tx[0][3], 0x10);
    CHECK_EQ(fk.tx[0][4], 0x04);
    CHECK_EQ(fk.tx[0][9], 4);
    CHECK_EQ(S.st.fan_sets, 1u);
    /* the panel's ACK: also passed to the stock (it is the stock binding's port) */
    CHECK_EQ(ack_last(3, 0x07), KPS_PASS);
    CHECK(!S.port[3].busy);
    CHECK_EQ(S.st.fan_acks, 1u);
    /* the panel sets: the app gets it; its ACK is the core's (the app is not the stock binding) */
    set_from(3, 1, 0);
    CHECK_EQ(fk.ntx, 2);
    CHECK_EQ(fk.txp[1], 2);
    CHECK_EQ(ack_last(2, 0x07), KPS_DONE);
    /* no change, no frame (the panel's cache push) */
    set_from(3, 1, 0);
    CHECK_EQ(fk.ntx, 2);
    CHECK_EQ(S.st.fan_same, 1u);
}

/* a set the core makes itself (startup restore, kpstart.h): every bound port gets the change - the stock binding's
 * (the panel) too; nothing when nobody is bound */
TEST(kps_fan_core_set_to_everybody)
{
    setup_two();
    set_from(KPS_FROM_CORE, 0, 6);
    CHECK_EQ(fk.ntx, 2);
    CHECK(sent_to(2) == 1 && sent_to(3) == 1);
    CHECK_EQ(S.st.fan_sets, 1u);
    set_from(KPS_FROM_CORE, 0, 6);            /* same value again: no frame */
    CHECK_EQ(fk.ntx, 2);
    CHECK_EQ(S.st.fan_same, 1u);
    /* nobody bound: begin returns at once, no key read */
    IOF = IO;
    IOF.key_value = f_key_value;
    memset(&fk, 0, sizeof fk);
    for (int p = 0; p < 4; p++)
        fk.link[p] = true;
    kps_init(&S, &IOF, T);
    kv_calls = 0;
    set_from(KPS_FROM_CORE, 0, 7);
    CHECK_EQ(kv_calls, 0);
    CHECK_EQ(fk.ntx, 0);
}

TEST(kps_fan_needs_another_port)
{
    static const uint8_t con[] = {0x01, 0x01};
    IOF = IO;
    IOF.key_value = f_key_value;
    memset(&fk, 0, sizeof fk);
    for (int p = 0; p < 4; p++)
        fk.link[p] = true;
    kps_init(&S, &IOF, T);
    rx(2, 0x7E, con, 2);
    fk.binding = 3;
    fk.ntx = 0;
    kv_calls = 0;
    kv[0].key[0] = 0x55; kv[0].key[1] = 0x08; kv[0].key[2] = 0x00;
    set_from(2, 0, 7);
    CHECK_EQ(kv_calls, 0);                    /* nobody else bound: not even read */
    CHECK_EQ(fk.ntx, 0);
}

TEST(kps_fan_queue_merge_and_drop)
{
    setup_two();
    set_from(2, 0, 1);                        /* in flight to the panel, not ACKed */
    set_from(2, 0, 2);
    set_from(2, 0, 3);                        /* replaces the waiting one */
    set_from(2, 2, 0x41);
    CHECK_EQ(fk.ntx, 1);
    CHECK_EQ(kps_fan_pending(&S, 3), 2);
    CHECK_EQ(S.st.fan_merged, 1u);
    ack_last(3, 0x07);
    CHECK_EQ(fk.ntx, 2);
    CHECK_EQ(fk.tx[1][7], 0x08);              /* order kept: reverb type (newest value 3) first */
    CHECK_EQ(fk.tx[1][9], 3);
    ack_last(3, 0x07);
    CHECK_EQ(fk.tx[2][7], 0x01);
    ack_last(3, 0x07);
    CHECK_EQ(kps_fan_pending(&S, 3), 0);
    /* full queue: the oldest waiting frame is dropped */
    set_from(2, 1, 0);                        /* in flight */
    for (int i = 0; i < (int)KPS_FANQ + 2; i++) {
        uint8_t f[11] = {0xF0, 0x40, 0x7F, 0x10, 0x04, 0x1E, 0x60, (uint8_t)i, 0x00, 0x01, 0xF7};
        kps_fanout(&S, 1u << 2, f, 11);
    }
    CHECK_EQ(kps_fan_pending(&S, 3), KPS_FANQ);
    CHECK_EQ(S.st.fan_drop, 2u);
    CHECK_EQ(S.fan[3].q[0].frame[7], 2);
}

TEST(kps_fan_interleaves_with_request)
{
    static const uint8_t req[] = {0x5F, 0x7F, 0x7F};
    setup_two();
    fk.matchmask[0] = 0x7;                    /* descriptors 0..2, one frame each */
    fk.frames_of[0] = fk.frames_of[1] = fk.frames_of[2] = 1;
    rx(2, 0x02, req, 3);                      /* the app's dump: value 0 in flight */
    CHECK_EQ(fk.ntx, 1);
    set_from(3, 0, 9);                        /* the panel sets meanwhile: waits behind value 0 */
    CHECK_EQ(fk.ntx, 1);
    ack_last(2, 0x07);
    CHECK_EQ(fk.ntx, 2);
    CHECK_EQ(fk.tx[1][6], 0x55);              /* the change first ... */
    ack_last(2, 0x07);
    CHECK_EQ(fk.tx[2][6], 1);                 /* ... then the dump goes on (descriptor 1) */
    ack_last(2, 0x07);
    ack_last(2, 0x07);
    CHECK_EQ(fk.tx[fk.ntx - 1][3], 0x03);     /* End */
    ack_last(2, 0x07);
    CHECK(!S.port[2].active && !S.port[2].busy);
    /* a request while a fan frame is in flight starts after its ACK */
    set_from(3, 0, 10);
    CHECK(S.port[2].busy && S.port[2].fan);
    rx(2, 0x02, req, 3);
    CHECK_EQ(fk.tx[fk.ntx - 1][6], 0x55);
    ack_last(2, 0x07);
    CHECK_EQ(fk.tx[fk.ntx - 1][6], 0);        /* the first value */
}

TEST(kps_fan_tap_stock_notice)
{
    /* the stock's headphone notice to its binding (the panel, port 3), seen on the TX ring in pieces */
    static const uint8_t hp[] = {0xF0, 0x40, 0x7F, 0x10, 0x04, 0x1E, 0x51, 0x29, 0x7F, 0x01, 0xF7};
    static const uint8_t ack[] = {0xF0, 0x40, 0x7F, 0x07, 0x04, 0x1E, 0x55, 0x08, 0x00, 0xF7};
    static const uint8_t packed[] = {0xF0, 0x40, 0x7F, 0x10, 0x04, 0x1E, 0x5F, 0x00, 0x7F, 0x05, 0x55, 0x08, 0x00, 0x01, 0xF7};
    uint8_t lng[60];
    setup_two();
    kps_tap(&S, 3, ack, sizeof ack);
    kps_tap(&S, 3, packed, sizeof packed);
    kps_tap(&S, 3, hp, 5);
    CHECK_EQ(fk.ntx, 0);
    kps_tap(&S, 3, hp + 5, sizeof hp - 5);
    CHECK_EQ(fk.ntx, 1);
    CHECK_EQ(fk.txp[0], 2);                   /* to the app only: the stock told the panel itself */
    CHECK_MEM(fk.tx[0], hp, sizeof hp);
    CHECK_EQ(S.st.tap_frames, 1u);
    CHECK_EQ(S.st.tap_fanned, 1u);
    /* a status byte inside cuts the frame; a long one is counted, not sent */
    memset(lng, 0x01, sizeof lng);
    memcpy(lng, hp, 9);
    lng[sizeof lng - 1] = 0xF7;
    ack_last(2, 0x07);
    kps_tap(&S, 3, lng, sizeof lng);
    CHECK_EQ(S.st.tap_long, 1u);
    {
        static const uint8_t cut[] = {0xF0, 0x40, 0x7F, 0x10, 0x90, 0x3C, 0x40, 0xF7};
        kps_tap(&S, 3, cut, sizeof cut);
    }
    CHECK_EQ(fk.ntx, 1);
    /* fn 0x32 (song progress, never ACKed): relayed at once, even with a fan frame in flight to the app */
    {
        static const uint8_t prog[] = {0xF0, 0x40, 0x7F, 0x32, 0x04, 0x1E, 0x52, 0x03, 0x7F, 0x00, 0x00, 0x01, 0xF7};
        set_from(3, 2, 0x55);   /* in flight to the app */
        CHECK(S.port[2].busy);
        kps_tap(&S, 3, prog, sizeof prog);
        CHECK_EQ(fk.txp[fk.ntx - 1], 2);
        CHECK_MEM(fk.tx[fk.ntx - 1], prog, sizeof prog);
        CHECK_EQ(S.st.tap_relayed, 1u);
        CHECK(S.port[2].busy && S.port[2].fan);   /* the reliable frame is still the one waited for */
    }
    /* BeatCounter 32 56 0B: not relayed by the tap - the metronome module makes those frames */
    {
        static const uint8_t beat[] = {0xF0, 0x40, 0x7F, 0x32, 0x04, 0x1E, 0x56, 0x0B, 0x7F, 0x02, 0x7F, 0xF7};
        int before = fk.ntx;
        kps_tap(&S, 3, beat, sizeof beat);
        CHECK_EQ(fk.ntx, before);
        CHECK_EQ(S.st.tap_relayed, 1u);
        /* kps_relay sends it at once to the bound ports not excluded (here: the app; the panel is excluded) */
        CHECK_EQ(kps_relay(&S, 0x08, beat, sizeof beat), 1);
        CHECK_EQ(fk.ntx, before + 1);
        CHECK_EQ(fk.txp[fk.ntx - 1], 2);
        CHECK_MEM(fk.tx[fk.ntx - 1], beat, sizeof beat);
        CHECK(S.port[2].busy && S.port[2].fan);
    }
    /* off: nothing */
    {
        int before = fk.ntx;
        kps_enable(&S, false);
        kps_tap(&S, 3, hp, sizeof hp);
        CHECK_EQ(fk.ntx, before);
        CHECK_EQ(kps_relay(&S, 0, hp, sizeof hp), 0);
        CHECK_EQ(fk.ntx, before);
    }
}

TEST(kps_fan_failures)
{
    setup_two();
    set_from(3, 0, 5);                        /* to the app, which never answers */
    set_from(3, 1, 0);
    run_ms(16 * 500 + 10);
    CHECK_EQ(S.st.fan_fail, 1u);
    CHECK(!kps_bound(&S, 2));                 /* not the panel: unbound, as the stock's 16 failures */
    CHECK_EQ(kps_fan_pending(&S, 2), 0);
    /* to the panel: that frame is lost, the next goes out */
    setup_two();
    set_from(2, 0, 5);
    set_from(2, 1, 0);
    run_ms(16 * 500 + 10);
    CHECK(kps_bound(&S, 3));
    CHECK_EQ(S.st.fan_fail, 1u);
    CHECK(S.port[3].busy && S.port[3].fan);
    CHECK_EQ(S.port[3].frame[7], 0x07);
}

TEST(kps_resume_after_live_swap)
{
    static const uint8_t con[] = {0x01, 0x01};
    static const uint8_t inv[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x01, 0x01, 0xF7};
    /* field 2026-11-01: apply now while the app (port 2) held the stock binding - the new core knows
     * only 0x04; the panel gets the stock's 7E 01 01 on port 3 alone, the app nothing */
    reset(3);
    CHECK_EQ(S.bound, 0x04);
    CHECK(kps_resume(&S));
    CHECK_EQ(fk.ntx, 1);
    CHECK_EQ(fk.txp[0], 3);
    CHECK_EQ(fk.txn[0], 9);
    CHECK(memcmp(fk.tx[0], inv, sizeof inv) == 0);
    CHECK_EQ(S.st.resume_invites, 1u);
    /* the panel CONNECTs again: the core answers, takes the stock binding, the app stays bound */
    CHECK_EQ(rx(3, 0x7E, con, 2), KPS_DONE);
    CHECK_EQ(S.bound, 0x0C);
    CHECK_EQ(fk.binding, 4);
    CHECK_EQ(fk.take_rep, 1);
    for (int i = 0; i < fk.ntx && i < 64; i++)
        CHECK(fk.txp[i] == 3);                 /* nothing to the app: no 7E 01 00 */
    /* the app's own re-CONNECT afterwards: core answer, nobody displaced */
    CHECK_EQ(rx(2, 0x7E, con, 2), KPS_DONE);
    CHECK_EQ(S.bound, 0x0C);
    CHECK_EQ(fk.binding, 3);
    /* the forwarding switch makes no copies of the invitation (FUN_40144CD0 = port 3 only) */
    reset(3);
    fk.fwd = true;
    CHECK(kps_resume(&S));
    CHECK_EQ(fk.ntx, 1);
    CHECK_EQ(fk.txp[0], 3);
    /* the panel holds the stock binding: already in the table, nothing sent */
    reset(4);
    CHECK(!kps_resume(&S));
    CHECK_EQ(fk.ntx, 0);
    /* boot load: nobody bound yet, the panel CONNECTs by itself later - nothing sent */
    reset(0);
    CHECK(!kps_resume(&S));
    CHECK_EQ(fk.ntx, 0);
    /* core switched off (1B 01 0): the stock alone, nothing sent */
    reset(3);
    kps_enable(&S, false);
    CHECK(!kps_resume(&S));
    CHECK_EQ(fk.ntx, 0);
    CHECK_EQ(S.st.resume_invites, 0u);
}

/* ---- file lease (field 2026-11-09) ---------------------------------------- */
static uint8_t ff_state;
static int ff_binds;
static uint8_t ff_bind_port[8];
static void f_file_bind(void *c, uint8_t p)
{
    (void)c;
    if (ff_binds < 8)
        ff_bind_port[ff_binds] = p;
    ff_binds++;
    fk.binding = (uint8_t)(p + 1);
}
static uint8_t f_file_state(void *c) { (void)c; return ff_state; }

/* app (2) + panel (3) bound, the panel holds the stock binding (the field 2026-11-09) */
static void setup_file(void)
{
    static const uint8_t con[] = {0x01, 0x01};
    kps_io_t io = IO;
    io.file_bind = f_file_bind;
    io.file_state = f_file_state;
    memset(&fk, 0, sizeof fk);
    for (int p = 0; p < 4; p++)
        fk.link[p] = true;
    fk.binding = 3;
    T = 100000;
    kps_init(&S, &io, T);
    rx(3, 0x7E, con, 2);   /* the panel CONNECTs last: core-answered, stock binding 4 */
    ff_state = 0;
    ff_binds = 0;
    fk.ntx = 0;
}

static const uint8_t F_LIST[] = {0x11}, F_ACK[] = {0x7F}, F_WAIT[] = {0x7C}, F_DEL[] = {0x06}, F_END[] = {0x1F};
static const uint8_t F_ACKF[] = {0xF0, 0x40, 0x7F, 0x24, 0x04, 0x1E, 0x7F, 0xF7};
static const uint8_t F_BUSY[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x03, 0x01, 0x00, 0x00, 0xF7};

TEST(kps_file_lease_list_from_app)
{
    uint8_t owner = 0, holder = 0;
    setup_file();
    CHECK_EQ(S.bound, 0x0C);
    CHECK_EQ(fk.binding, 4);
    /* the app's list request: the app becomes the stock binding for the job, the stock handles the frame */
    CHECK_EQ(rx(2, 0x24, F_LIST, 1), KPS_PASS);
    CHECK_EQ(fk.binding, 3);
    CHECK_EQ(ff_binds, 1);
    CHECK_EQ(ff_bind_port[0], 2);
    CHECK(kps_file_lease(&S, &owner, &holder));
    CHECK(owner == 3 && holder == 4);
    CHECK_EQ(S.st.file_leases, 1u);
    CHECK_EQ(fk.ntx, 0);                 /* the core sends nothing: the stock answers */
    ff_state = KPS_FS_HARD;              /* the file task walks (0x413CA3EC) */
    /* the app's ACKs go to the stock (it is the binding now); its file Wait is still dropped */
    CHECK_EQ(rx(2, 0x24, F_ACK, 1), KPS_PASS);
    CHECK_EQ(rx(2, 0x24, F_WAIT, 1), KPS_DONE);
    /* the panel asks meanwhile: the core's ACK + Busy to the panel only, nothing to the stock */
    CHECK_EQ(rx(3, 0x24, F_LIST, 1), KPS_DONE);
    CHECK_EQ(fk.ntx, 2);
    CHECK(fk.txp[0] == 3 && fk.txp[1] == 3);
    CHECK_MEM(fk.tx[0], F_ACKF, sizeof F_ACKF);
    CHECK_MEM(fk.tx[1], F_BUSY, sizeof F_BUSY);
    CHECK_EQ(S.st.file_busy, 1u);
    CHECK_EQ(rx(3, 0x24, F_ACK, 1), KPS_DONE);   /* the panel's stray ACK: not the owner's */
    /* sync() keeps both bound across the switch (no unbind of anybody) */
    run_ms(100);
    CHECK_EQ(S.bound, 0x0C);
    /* long walk: still the app's while the list runs, however quiet */
    run_ms(3000);
    CHECK_EQ(fk.binding, 3);
    /* the last ACK (24 1F), the list flag cleared: KPS_FILE_QUIET_MS later the panel has its binding back */
    CHECK_EQ(rx(2, 0x24, F_ACK, 1), KPS_PASS);
    ff_state = 0;
    run_ms(KPS_FILE_QUIET_MS - 100);
    CHECK_EQ(fk.binding, 3);
    run_ms(200);
    CHECK_EQ(fk.binding, 4);
    CHECK(!kps_file_lease(&S, 0, 0));
    CHECK_EQ(ff_binds, 2);
    CHECK_EQ(ff_bind_port[1], 3);
    CHECK_EQ(S.st.file_back, 1u);
    CHECK_EQ(S.bound, 0x0C);
    CHECK_EQ(fk.ntx, 2);                 /* nothing more to anybody */
    /* the panel's own list again: the stock's, no lease */
    CHECK_EQ(rx(3, 0x24, F_LIST, 1), KPS_PASS);
    CHECK(!kps_file_lease(&S, 0, 0));
    CHECK_EQ(fk.binding, 4);
}

TEST(kps_file_lease_busy_and_stuck)
{
    setup_file();
    /* the panel's own job runs (list flag up): the app gets Busy, the binding stays */
    CHECK_EQ(rx(3, 0x24, F_LIST, 1), KPS_PASS);
    ff_state = KPS_FS_HARD;
    CHECK_EQ(rx(2, 0x24, F_LIST, 1), KPS_DONE);
    CHECK_EQ(fk.binding, 4);
    CHECK_EQ(fk.ntx, 2);
    CHECK(fk.txp[0] == 2 && fk.txp[1] == 2);
    CHECK_MEM(fk.tx[1], F_BUSY, sizeof F_BUSY);
    /* a pending job of the panel (soft state) shortly after its last file frame: Busy too */
    ff_state = KPS_FS_SOFT;
    rx(3, 0x24, F_DEL, 1);
    run_ms(1000);
    CHECK_EQ(rx(2, 0x24, F_DEL, 1), KPS_DONE);
    CHECK_EQ(S.st.file_busy, 2u);
    /* the same soft state long after the last file frame (an abandoned job): the app gets its lease */
    run_ms(KPS_FILE_SOFT_MS);
    CHECK_EQ(rx(2, 0x24, F_DEL, 1), KPS_PASS);
    CHECK_EQ(fk.binding, 3);
    /* a soft state of the leased job keeps it while frames flow, ends KPS_FILE_SOFT_MS after the last one */
    CHECK_EQ(rx(2, 0x24, F_END, 1), KPS_PASS);
    run_ms(KPS_FILE_SOFT_MS - 100);
    CHECK_EQ(fk.binding, 3);
    run_ms(200);
    CHECK_EQ(fk.binding, 4);
    /* stock bytes to the owner keep it too (kps_tap during the lease) */
    ff_state = 0;
    run_ms(KPS_FILE_SOFT_MS);
    CHECK_EQ(rx(2, 0x24, F_LIST, 1), KPS_PASS);
    for (int i = 0; i < 5; i++) {
        static const uint8_t b[] = {0xF0, 0x40, 0x7F, 0x24, 0x04, 0x1E, 0x1F, 0xF7};
        run_ms(400);
        kps_tap(&S, 2, b, sizeof b);
    }
    CHECK_EQ(fk.binding, 3);
    run_ms(KPS_FILE_QUIET_MS + 10);
    CHECK_EQ(fk.binding, 4);
    /* a port nobody bound: the stock's (it drops it), no lease */
    CHECK_EQ(rx(1, 0x24, F_LIST, 1), KPS_PASS);
    CHECK_EQ(fk.binding, 4);
    /* the holder's job just ended (state idle, its Result may not be out yet): Busy until KPS_FILE_QUIET_MS of quiet */
    run_ms(KPS_FILE_SOFT_MS);
    CHECK_EQ(rx(3, 0x24, F_ACK, 1), KPS_PASS);
    run_ms(100);
    CHECK_EQ(rx(2, 0x24, F_LIST, 1), KPS_DONE);
    CHECK_EQ(fk.binding, 4);
    run_ms(KPS_FILE_QUIET_MS);
    CHECK_EQ(rx(2, 0x24, F_LIST, 1), KPS_PASS);
    CHECK_EQ(fk.binding, 3);
}

TEST(kps_file_lease_ends_by_others)
{
    static const uint8_t con[] = {0x01, 0x01}, dis[] = {0x01, 0x00};
    /* the stock drops the owner mid-job (16 failures of FUN_4014B638: binding 0): the holder gets it back */
    setup_file();
    rx(2, 0x24, F_LIST, 1);
    fk.binding = 0;
    run_ms(10);
    CHECK_EQ(fk.binding, 4);
    CHECK(!kps_bound(&S, 2));            /* the app unbound, as the stock's own 16 failures */
    CHECK(kps_bound(&S, 3));
    CHECK_EQ(S.st.file_lost, 1u);
    /* the holder CONNECTs during the lease: the last CONNECT keeps the binding, no give-back */
    setup_file();
    rx(2, 0x24, F_LIST, 1);
    CHECK_EQ(rx(3, 0x7E, con, 2), KPS_DONE);
    CHECK_EQ(fk.binding, 4);
    run_ms(KPS_FILE_QUIET_MS + 10);
    CHECK_EQ(fk.binding, 4);
    CHECK(!kps_file_lease(&S, 0, 0));
    CHECK_EQ(ff_binds, 1);
    /* the owner CONNECTs: it keeps the binding */
    setup_file();
    rx(2, 0x24, F_LIST, 1);
    CHECK_EQ(rx(2, 0x7E, con, 2), KPS_PASS);
    run_ms(KPS_FILE_QUIET_MS + 10);
    CHECK_EQ(fk.binding, 3);
    CHECK(!kps_file_lease(&S, 0, 0));
    /* the owner DISCONNECTs (the stock's path: unbind + invite) -> the holder back */
    setup_file();
    rx(2, 0x24, F_LIST, 1);
    CHECK_EQ(rx(2, 0x7E, dis, 2), KPS_PASS);
    fk.binding = 0;                      /* the stock's FUN_4014B55C */
    run_ms(10);
    CHECK_EQ(fk.binding, 4);
    /* the core switched off mid-lease: the holder back at once */
    setup_file();
    rx(2, 0x24, F_LIST, 1);
    kps_enable(&S, false);
    CHECK_EQ(fk.binding, 4);
    CHECK_EQ(rx(2, 0x24, F_LIST, 1), KPS_PASS);   /* off: everything to the stock, no lease */
    CHECK_EQ(fk.binding, 4);
    /* nobody held the stock binding before: the app keeps it after the job (nothing to give back) */
    setup_file();
    fk.binding = 0;
    run_ms(2);
    CHECK(kps_bound(&S, 2));
    rx(2, 0x24, F_LIST, 1);
    CHECK_EQ(fk.binding, 3);
    run_ms(KPS_FILE_QUIET_MS + 10);
    CHECK_EQ(fk.binding, 3);
    CHECK(kps_bound(&S, 2));
}

TEST(kps_file_no_lease_without_io)
{
    static const uint8_t con[] = {0x01, 0x01};
    /* a platform without file_bind / file_state: fn 0x24 is the stock's as before (7C still dropped) */
    reset(4);
    rx(2, 0x7E, con, 2);
    fk.ntx = 0;
    CHECK_EQ(rx(3, 0x24, F_LIST, 1), KPS_PASS);
    CHECK_EQ(rx(3, 0x24, F_WAIT, 1), KPS_DONE);
    CHECK_EQ(fk.ntx, 0);
}

TEST(kps_tap_file_result_not_relayed)
{
    static const uint8_t res[] = {0xF0, 0x40, 0x7F, 0x32, 0x04, 0x1E, 0x51, 0x7E, 0x7F, 0x00, 0x00, 0xF7};
    static const uint8_t prog[] = {0xF0, 0x40, 0x7F, 0x32, 0x04, 0x1E, 0x52, 0x03, 0x7F, 0x01, 0x02, 0xF7};
    setup_file();
    kps_tap(&S, 3, res, sizeof res);     /* a file job's Result to the binding: the others do not get it */
    CHECK_EQ(fk.ntx, 0);
    kps_tap(&S, 3, prog, sizeof prog);   /* song progress still relayed */
    CHECK_EQ(fk.ntx, 1);
    CHECK_EQ(fk.txp[0], 2);
}

void suite_kpsess(void);
void suite_kpsess(void)
{
    printf("session core (Phase 3-2)\n");
    RUN(kps_clock_wraps);
    RUN(kps_connect_pass_and_own);
    RUN(kps_disconnect);
    RUN(kps_request_dump_in_order);
    RUN(kps_retry_and_fail);
    RUN(kps_panel_never_released);
    RUN(kps_no_wrap_burst);
    RUN(kps_nak_wait);
    RUN(kps_ack_assignment);
    RUN(kps_cancel);
    RUN(kps_packed_and_unbound);
    RUN(kps_link_down_and_sync);
    RUN(kps_disabled_passes);
    RUN(kps_forwarding);
    RUN(kps_fan_set_to_others_only);
    RUN(kps_fan_needs_another_port);
    RUN(kps_fan_queue_merge_and_drop);
    RUN(kps_fan_interleaves_with_request);
    RUN(kps_fan_tap_stock_notice);
    RUN(kps_fan_failures);
    RUN(kps_resume_after_live_swap);
    RUN(kps_fan_core_set_to_everybody);
    RUN(kps_file_lease_list_from_app);
    RUN(kps_file_lease_busy_and_stuck);
    RUN(kps_file_lease_ends_by_others);
    RUN(kps_file_no_lease_without_io);
    RUN(kps_tap_file_result_not_relayed);
}
