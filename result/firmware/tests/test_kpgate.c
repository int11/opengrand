/* Phase 4 (A) intake gate (src/kproto/kpgate.c) against a fake stock: the core's accept / refuse / answer of the
 * frames FUN_4014A2C8 handles past the session core. */
#include <string.h>

#include "ca79ng/kpgate.h"
#include "test.h"

static struct {
    uint8_t binding;
    uint32_t device;
    bool fwd;
    uint8_t bound;      /* bit p */
    uint8_t job, list;
    bool media, test;
    uint8_t mode;
    bool no_info;
    int ntx;
    uint8_t txp[16];
    uint8_t tx[16][KPG_FRAME_MAX];
    uint16_t txn[16];
} fk;

static uint8_t f_binding(void *c) { (void)c; return fk.binding; }
static uint32_t f_device(void *c) { (void)c; return fk.device; }
static bool f_forward(void *c) { (void)c; return fk.fwd; }
static bool f_bound(void *c, uint8_t p) { (void)c; return (fk.bound >> p) & 1u; }
static void f_tx(void *c, uint8_t p, const uint8_t *m, uint16_t n)
{
    (void)c;
    if (fk.ntx < 16) {
        fk.txp[fk.ntx] = p;
        memcpy(fk.tx[fk.ntx], m, n);
        fk.txn[fk.ntx] = n;
    }
    fk.ntx++;
}
static uint8_t f_file(void *c, uint8_t w) { (void)c; return w == KPG_FILE_JOB ? fk.job : fk.list; }
static bool f_media(void *c) { (void)c; return fk.media; }
static uint8_t f_mode(void *c) { (void)c; return fk.mode; }
static bool f_test(void *c) { (void)c; return fk.test; }
/* the stock's data: 04 00 six bytes of the model table, 04 01 "V2.19", 05 00 [v >> 7, v], 05 01 [0, v], 05 02 [v] */
static int f_info(void *c, uint8_t sub, uint8_t sel, uint8_t out[KPG_INFO_MAX + 1u])
{
    static const char name[] = "V2.19";
    (void)c;
    if (fk.no_info)
        return 0;
    if (sub == 4 && sel == 0) {
        memcpy(out, "KP813Q", 6);
        out[6] = 0xFF;
        return 6;
    }
    if (sub == 4 && sel == 1) {
        memcpy(out, name, 5);
        out[5] = 0xFF;
        return 5;
    }
    if (sub == 5 && sel == 0) {
        out[0] = 0x01; out[1] = 0x82; out[2] = 0xFF;   /* v 0x82: the stock leaves the low byte unmasked */
        return 2;
    }
    if (sub == 5 && sel == 1) {
        out[0] = 0x00; out[1] = 0x03; out[2] = 0xFF;
        return 2;
    }
    if (sub == 5 && sel == 2) {
        out[0] = 0x05; out[1] = 0xFF;
        return 1;
    }
    return 0;
}

static const kpg_io_t IO = {
    .ctx = 0, .binding = f_binding, .device = f_device, .forward = f_forward, .bound = f_bound, .tx = f_tx,
    .file = f_file, .media = f_media, .mode = f_mode, .test_ok = f_test, .info = f_info,
};

static kpg_t G;

static void reset(uint8_t binding, uint8_t bound)
{
    memset(&fk, 0, sizeof fk);
    fk.binding = binding;
    fk.bound = bound;
    fk.media = true;
    kpg_init(&G, &IO);
}

static kpg_verdict_t rxd(uint8_t port, uint8_t dev, uint8_t fn, uint8_t m4, const uint8_t *body, int nb)
{
    uint8_t f[48];
    f[0] = 0xF0; f[1] = 0x40; f[2] = dev; f[3] = fn; f[4] = m4; f[5] = 0x02;
    memcpy(f + 6, body, (size_t)nb);
    f[6 + nb] = 0xF7;
    return kpg_rx(&G, port, f, (uint32_t)(7 + nb));
}

static kpg_verdict_t rx(uint8_t port, uint8_t fn, const uint8_t *body, int nb)
{
    return rxd(port, 0x7F, fn, 0x08, body, nb);
}

static bool sent(int k, uint8_t port, const uint8_t *f, uint16_t n)
{
    return k < fk.ntx && fk.txp[k] == port && fk.txn[k] == n && memcmp(fk.tx[k], f, n) == 0;
}

static const uint8_t ACK[] = {0xF0, 0x40, 0x7F, 0x24, 0x04, 0x1E, 0x7F, 0xF7};
static const uint8_t BUSY[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x03, 0x01, 0x00, 0x00, 0xF7};
static const uint8_t NOMEDIA[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x03, 0x00, 0x00, 0x21, 0xF7};

TEST(kpg_stock_paths_pass)
{
    static const uint8_t req[] = {0x55, 0x7F, 0x7F}, set[] = {0x55, 0x08, 0x00, 0x10}, vt[] = {0x5A, 0x00};
    static const uint8_t con[] = {0x01, 0x01}, dis[] = {0x01, 0x00}, cmd[] = {0x56, 0x08, 0x7F, 0x01};
    reset(4, 0x0C);
    CHECK_EQ(rx(3, 0x02, req, 3), KPG_PASS);   /* the session core's */
    CHECK_EQ(rx(2, 0x07, req, 3), KPG_PASS);
    CHECK_EQ(rx(2, 0x04, req, 3), KPG_PASS);
    CHECK_EQ(rx(2, 0x05, req, 3), KPG_PASS);
    CHECK_EQ(rx(2, 0x06, req, 3), KPG_PASS);
    CHECK_EQ(rx(2, 0x10, set, 4), KPG_PASS);   /* the front */
    CHECK_EQ(rx(2, 0x32, cmd, 4), KPG_PASS);   /* the commands */
    CHECK_EQ(rx(2, 0x31, cmd, 4), KPG_PASS);
    CHECK_EQ(rx(2, 0x7E, con, 2), KPG_PASS);   /* CONNECT / DISCONNECT */
    CHECK_EQ(rx(3, 0x7E, dis, 2), KPG_PASS);
    CHECK_EQ(rx(1, 0x50, vt, 2), KPG_PASS);    /* VT edit (fn byte any, d0 0x5A) */
    CHECK_EQ(rxd(0, 0x33, 0x70, 0x00, req, 3), KPG_PASS);   /* flash tools before the device test, untouched */
    CHECK_EQ(rxd(1, 0x33, 0x60, 0x00, req, 0), KPG_PASS);   /* identity: every port */
    CHECK_EQ(rxd(0, 0x7F, 0x20, 0x71, req, 3), KPG_PASS);   /* 71 02 from port 0: the stock's own path */
    CHECK_EQ(fk.ntx, 0);
    CHECK_EQ(G.st.passed, 14);
    CHECK_EQ(G.st.dropped, 0);
}

TEST(kpg_drops_what_the_stock_ignores)
{
    static const uint8_t b[] = {0x00, 0x00, 0x00, 0x00};
    reset(4, 0x0C);
    fk.device = 0x10;
    CHECK_EQ(rxd(3, 0x11, 0x10, 0x08, b, 4), KPG_DROP);   /* another device number */
    CHECK_EQ(rxd(3, 0x10, 0x09, 0x08, b, 0), KPG_PASS);   /* ours */
    CHECK_EQ(rxd(3, 0x7F, 0x10, 0x09, b, 4), KPG_DROP);   /* m[4] not 08 */
    CHECK_EQ(rxd(1, 0x7F, 0x20, 0x71, b, 3), KPG_DROP);   /* 71 02 not from port 0 */
    for (uint8_t fn = 0; fn < 0x80; fn++) {
        bool stock = fn == 0x02 || fn == 0x03 || (fn >= 0x04 && fn <= 0x07) || fn == 0x09 || fn == 0x10 || fn == 0x24 ||
                     fn == 0x31 || fn == 0x32 || fn == 0x60 || fn == 0x70 || fn == 0x7E;
        if (!stock)
            CHECK_EQ(rx(3, fn, b, 4), KPG_DROP);
    }
    CHECK_EQ(fk.ntx, 0);
}

TEST(kpg_binding_only_functions)
{
    static const uint8_t end[] = {0x55, 0x7F, 0x7F}, nine[] = {0x00};
    reset(4, 0x0C);   /* panel = stock binding, app (2) bound in the core */
    CHECK_EQ(rx(3, 0x03, end, 3), KPG_PASS);    /* n 10 from the binding */
    CHECK_EQ(rx(3, 0x03, end, 2), KPG_DROP);    /* n 9: the stock tests r7 == 10 */
    CHECK_EQ(rx(2, 0x03, end, 3), KPG_DROP);    /* not the binding */
    CHECK_EQ(rx(3, 0x09, nine, 0), KPG_PASS);
    CHECK_EQ(rx(2, 0x09, nine, 0), KPG_DROP);
    CHECK_EQ(G.st.other_drop, 3);
    CHECK_EQ(fk.ntx, 0);
}

TEST(kpg_test_mode)
{
    static const uint8_t t3[] = {0x02, 0x03, 0x01}, t4[] = {0x02, 0x04, 0x01};
    reset(4, 0x0C);
    fk.test = true;
    CHECK_EQ(rx(3, 0x7E, t3, 3), KPG_PASS);
    CHECK_EQ(rx(3, 0x7E, t4, 3), KPG_DROP);     /* m[7] != 3 */
    CHECK_EQ(rx(2, 0x7E, t3, 3), KPG_DROP);     /* not the binding: no deliberate difference here */
    fk.test = false;
    CHECK_EQ(rx(3, 0x7E, t3, 3), KPG_DROP);     /* model class 0 (*0x419156A7) */
    CHECK_EQ(G.st.test_drop, 3);
    CHECK_EQ(fk.ntx, 0);
}

TEST(kpg_sys_other_subs)
{
    static const uint8_t s0[] = {0x00, 0x00}, s0b[] = {0x00, 0x40}, s3[] = {0x03, 0x00}, s9[] = {0x09, 0x00};
    static const uint8_t c2[] = {0x01, 0x02}, lo[] = {0x04, 0x3F}, hi[] = {0x05, 0x43};
    reset(4, 0x0C);
    CHECK_EQ(rx(3, 0x7E, s0, 2), KPG_DROP);
    CHECK_EQ(rx(3, 0x7E, s0b, 2), KPG_DROP);
    CHECK_EQ(rx(3, 0x7E, s3, 2), KPG_DROP);
    CHECK_EQ(rx(3, 0x7E, s9, 2), KPG_DROP);
    CHECK_EQ(rx(3, 0x7E, c2, 2), KPG_DROP);     /* 7E 01 02: neither CONNECT nor DISCONNECT */
    CHECK_EQ(rx(3, 0x7E, lo, 2), KPG_DROP);     /* < 0x40 */
    CHECK_EQ(rx(2, 0x7E, hi, 2), KPG_DROP);     /* & 0x3F = 3 */
    CHECK_EQ(fk.ntx, 0);
}

TEST(kpg_info_binding_and_others)
{
    static const uint8_t q40[] = {0x04, 0x40}, q41[] = {0x04, 0x41}, q42[] = {0x04, 0x42};
    static const uint8_t q50[] = {0x05, 0x40}, q51[] = {0x05, 0x41}, q52[] = {0x05, 0x42};
    static const uint8_t a40[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x04, 0x00, 'K', 'P', '8', '1', '3', 'Q', 0xF7};
    static const uint8_t a41[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x04, 0x01, 'V', '2', '.', '1', '9', 0xF7};
    static const uint8_t a50[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x05, 0x00, 0x01, 0x82, 0xF7};
    static const uint8_t a51[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x05, 0x01, 0x00, 0x03, 0xF7};
    static const uint8_t a52[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x05, 0x02, 0x05, 0xF7};
    reset(4, 0x0D);   /* panel = stock binding; app 2 and port 0 bound in the core; port 1 nobody */
    CHECK_EQ(rx(3, 0x7E, q40, 2), KPG_PASS);    /* the binding: the stock answers it */
    CHECK_EQ(rx(3, 0x7E, q42, 2), KPG_PASS);
    CHECK_EQ(fk.ntx, 0);
    CHECK_EQ(rx(2, 0x7E, q40, 2), KPG_DONE);    /* deliberate difference: the core answers the app */
    CHECK_EQ(rx(2, 0x7E, q41, 2), KPG_DONE);
    CHECK_EQ(rx(2, 0x7E, q50, 2), KPG_DONE);
    CHECK_EQ(rx(2, 0x7E, q51, 2), KPG_DONE);
    CHECK_EQ(rx(0, 0x7E, q52, 2), KPG_DONE);
    CHECK_EQ(fk.ntx, 5);
    CHECK(sent(0, 2, a40, sizeof a40));
    CHECK(sent(1, 2, a41, sizeof a41));
    CHECK(sent(2, 2, a50, sizeof a50));
    CHECK(sent(3, 2, a51, sizeof a51));
    CHECK(sent(4, 0, a52, sizeof a52));
    CHECK_EQ(G.st.info_own, 5);
    CHECK_EQ(rx(2, 0x7E, q42, 2), KPG_DROP);    /* the flash checksum stays the binding's */
    CHECK_EQ(rx(1, 0x7E, q40, 2), KPG_DROP);    /* not bound: as the stock */
    fk.no_info = true;
    CHECK_EQ(rx(2, 0x7E, q40, 2), KPG_DROP);    /* no data: nothing */
    CHECK_EQ(fk.ntx, 5);
}

TEST(kpg_info_frame_rule)
{
    uint8_t d[KPG_INFO_MAX + 1u], f[KPG_FRAME_MAX];
    memset(d, 0x41, sizeof d);
    CHECK_EQ(kpg_sys_frame(4, 1, d, f), 9 + KPG_INFO_MAX);   /* no 0xFF: 0x1C bytes at most */
    CHECK_EQ(f[8 + KPG_INFO_MAX], 0xF7);
    d[2] = 0xFF;
    CHECK_EQ(kpg_sys_frame(4, 1, d, f), 11);                 /* the first 0xFF ends the data */
    d[0] = 0xFF;
    CHECK_EQ(kpg_sys_frame(5, 2, d, f), 9);
}

TEST(kpg_info_to_panel_forwarding)
{
    static const uint8_t q50[] = {0x05, 0x40};
    reset(3, 0x0C);   /* the app is the stock binding, the panel bound in the core, forwarding on */
    fk.fwd = true;
    CHECK_EQ(rx(3, 0x7E, q50, 2), KPG_DONE);
    CHECK_EQ(fk.ntx, 4);                        /* FUN_4014AD04 for the panel: 0, 1, 2 then 3 */
    CHECK_EQ(fk.txp[0], 0);
    CHECK_EQ(fk.txp[3], 3);
}

TEST(kpg_file_list_intake)
{
    static const uint8_t list[] = {0x11};
    reset(4, 0x0C);
    CHECK_EQ(rx(3, 0x24, list, 1), KPG_PASS);   /* media, idle: the stock queues it */
    CHECK_EQ(fk.ntx, 0);
    fk.list = 1;
    CHECK_EQ(rx(3, 0x24, list, 1), KPG_DONE);   /* a list running: ACK + Busy, the stock's bytes */
    CHECK(sent(0, 3, ACK, sizeof ACK));
    CHECK(sent(1, 3, BUSY, sizeof BUSY));
    fk.list = 0;
    fk.media = false;
    fk.ntx = 0;
    CHECK_EQ(rx(3, 0x24, list, 1), KPG_DONE);   /* no USB memory: ACK + 7E 03 00 00 21 */
    CHECK(sent(0, 3, ACK, sizeof ACK));
    CHECK(sent(1, 3, NOMEDIA, sizeof NOMEDIA));
    fk.mode = 5;
    fk.ntx = 0;
    CHECK_EQ(rx(3, 0x24, list, 1), KPG_DONE);   /* mode 5: the ACK only (0x4014A978) - before the media test */
    CHECK_EQ(fk.ntx, 1);
    CHECK(sent(0, 3, ACK, sizeof ACK));
    CHECK_EQ(G.st.file_busy, 1);
    CHECK_EQ(G.st.file_err, 1);
    CHECK_EQ(G.st.file_ack, 1);
    CHECK_EQ(rx(2, 0x24, list, 1), KPG_DROP);   /* not the binding (the lease is the session core's) */
}

TEST(kpg_file_job_intake)
{
    static const uint8_t del[] = {0x06}, ren[] = {0x09, 0x00, 0x00}, kso[] = {0x12}, up[] = {0x05, 0x01, 0x00};
    reset(3, 0x0C);
    CHECK_EQ(rx(2, 0x24, del, 1), KPG_PASS);
    CHECK_EQ(rx(2, 0x24, ren, 3), KPG_PASS);
    fk.list = 1;                                /* a list does not hold a job back (the stock tests the job byte) */
    CHECK_EQ(rx(2, 0x24, kso, 1), KPG_PASS);
    fk.job = 0x06;
    for (uint8_t d0 = 0x04; d0 <= 0x0B; d0++) {
        uint8_t b[] = {d0, 0x01, 0x00};
        fk.ntx = 0;
        CHECK_EQ(rx(2, 0x24, b, 3), KPG_DONE);
        CHECK(sent(0, 2, ACK, sizeof ACK));
        CHECK(sent(1, 2, BUSY, sizeof BUSY));
    }
    CHECK_EQ(rx(2, 0x24, kso, 1), KPG_DONE);
    CHECK_EQ(rx(2, 0x24, up, 3), KPG_DONE);
    CHECK_EQ(G.st.file_busy, 10);
}

TEST(kpg_file_data_and_unknown)
{
    reset(3, 0x0C);
    fk.job = 5;
    for (int d0 = 0; d0 < 0x80; d0++) {
        uint8_t b[] = {(uint8_t)d0, 0x00};
        bool data = d0 == 0x01 || d0 == 0x02 || d0 == 0x03 || d0 == 0x1F || d0 == 0x7B || d0 >= 0x7C;
        bool req = (d0 >= 0x04 && d0 <= 0x0B) || d0 == 0x11 || d0 == 0x12;
        if (data)
            CHECK_EQ(rx(2, 0x24, b, 2), KPG_PASS);
        else if (!req && d0 != 0x5A)   /* d0 0x5A = VT edit, judged before the function code */
            CHECK_EQ(rx(2, 0x24, b, 2), KPG_DROP);
    }
}

TEST(kpg_disabled_passes)
{
    static const uint8_t b[] = {0x00};
    reset(4, 0x0C);
    kpg_enable(&G, false);
    CHECK_EQ(rx(3, 0x08, b, 1), KPG_PASS);
    CHECK_EQ(rx(2, 0x09, b, 0), KPG_PASS);
    kpg_enable(&G, true);
    CHECK_EQ(rx(3, 0x08, b, 1), KPG_DROP);
    CHECK_EQ(G.st.passed, 0);
}

void suite_kpgate(void);
void suite_kpgate(void)
{
    printf("intake gate (Phase 4 A)\n");
    RUN(kpg_stock_paths_pass);
    RUN(kpg_drops_what_the_stock_ignores);
    RUN(kpg_binding_only_functions);
    RUN(kpg_test_mode);
    RUN(kpg_sys_other_subs);
    RUN(kpg_info_binding_and_others);
    RUN(kpg_info_frame_rule);
    RUN(kpg_info_to_panel_forwarding);
    RUN(kpg_file_list_intake);
    RUN(kpg_file_job_intake);
    RUN(kpg_file_data_and_unknown);
    RUN(kpg_disabled_passes);
}
