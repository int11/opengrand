/* Phase 4 MIDI channel messages + delayed parameter apply (src/kproto/kpchan.c) and the same-tick store update
 * (kst_absorb in src/kproto/kpstate.c).  The emulator round trip
 * result/stockfw/tools/roundtrip_midich.py runs the same decisions against the real stock routines. */
#include <string.h>

#include "ca79ng/kpchan.h"
#include "ca79ng/kpstate.h"
#include "test.h"

static kch_in_t unbound(uint32_t rx)
{
    kch_in_t in = {.bound = 0, .run = 0, .sub3 = 0, .mode = 0, .omni = 0, .rx_ch = rx};
    return in;
}

TEST(kch_path_and_accept_follow_the_stock)
{
    kch_in_t in = unbound(0);
    /* the stock binding: channel 0 is the settings path, accepted; others the part path */
    in.bound = 1;
    CHECK_EQ(kch_path(&in, 0), 0);
    CHECK_EQ(kch_path(&in, 3), 2);
    CHECK(kch_accept(&in, 0));
    CHECK(!kch_accept(&in, 1));
    in.omni = 1;
    CHECK(!kch_accept(&in, 1));   /* omni does not count for the binding */
    /* unbound: run state forces path 0, then sub3 -> 2, else the mode byte */
    in = unbound(4);
    CHECK_EQ(kch_path(&in, 9), 0);
    in.mode = 5;
    CHECK_EQ(kch_path(&in, 9), 5);
    in.sub3 = 1;
    CHECK_EQ(kch_path(&in, 9), 2);
    in.run = 1;
    CHECK_EQ(kch_path(&in, 9), 0);
    /* unbound accept: the receive channel word, or omni */
    in = unbound(4);
    CHECK(kch_accept(&in, 4));
    CHECK(!kch_accept(&in, 5));
    in.omni = 1;
    CHECK(kch_accept(&in, 5));
    in = unbound(0x104);   /* the word is compared whole */
    CHECK(!kch_accept(&in, 4));
}

TEST(kch_program_change)
{
    kch_in_t in = unbound(0);
    kch_dec_t d = kch_pc(&in, 0);
    CHECK_EQ(d.act, KCH_SETTING);
    CHECK_MEM(d.key, ((const uint8_t[]){0x55, 0x00, 0x00}), 3);
    CHECK_EQ(d.side, 1);
    CHECK_EQ(kch_pc(&in, 1).act, KCH_DROP);   /* another channel, no omni: the stock returns */
    in.omni = 1;
    CHECK_EQ(kch_pc(&in, 1).act, KCH_SETTING);
    in = unbound(0);
    in.mode = 1;                              /* multi-timbral: the part voice, not a setting */
    CHECK_EQ(kch_pc(&in, 0).act, KCH_STOCK);
    CHECK_EQ(kch_pc(&in, 0).path, 1);
    in.bound = 1;                             /* the binding: channel 0 = the sound, others the parts */
    CHECK_EQ(kch_pc(&in, 0).act, KCH_SETTING);
    CHECK_EQ(kch_pc(&in, 2).act, KCH_STOCK);
}

TEST(kch_control_change)
{
    kch_in_t in = unbound(2);
    kch_dec_t d;
    /* not accepted: nothing at all, whatever the CC */
    CHECK_EQ(kch_cc(&in, 3, 0x07, 100).act, KCH_DROP);
    CHECK_EQ(kch_cc(&in, 3, 0x7A, 0).act, KCH_DROP);
    /* Local Control: rx channel, 0 or 0x7F only */
    d = kch_cc(&in, 2, 0x7A, 0x7F);
    CHECK_EQ(d.act, KCH_SETTING);
    CHECK_MEM(d.key, ((const uint8_t[]){0x58, 0x01, 0x7F}), 3);
    CHECK_EQ(d.side, 0);
    CHECK_EQ(kch_cc(&in, 2, 0x7A, 0).act, KCH_SETTING);
    CHECK_EQ(kch_cc(&in, 2, 0x7A, 0x40).act, KCH_DROP);
    in.omni = 1;
    CHECK_EQ(kch_cc(&in, 3, 0x7A, 0).act, KCH_DROP);    /* accepted by omni, but not the rx channel */
    in.omni = 0;
    /* performance / run state: the stock runs, no key */
    static const uint8_t stock[] = {0x00, 0x20, 0x07, 0x0A, 0x0B, 0x40, 0x42, 0x43, 0x78, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F};
    for (unsigned k = 0; k < sizeof stock; k++)
        CHECK_EQ(kch_cc(&in, 2, stock[k], 1).act, KCH_STOCK);
    /* CCs the settings path ignores */
    static const uint8_t none[] = {0x01, 0x06, 0x26, 0x41, 0x5B, 0x5D, 0x62, 0x63, 0x64, 0x65, 0x79};
    for (unsigned k = 0; k < sizeof none; k++)
        CHECK_EQ(kch_cc(&in, 2, none[k], 1).act, KCH_DROP);
    /* 0x50-0x53 only while FUN_40135980() != 0; 0x5C (effect speed) only while it is 0 */
    CHECK_EQ(kch_cc(&in, 2, 0x52, 1).act, KCH_DROP);
    d = kch_cc(&in, 2, 0x5C, 0x7F);
    CHECK_EQ(d.act, KCH_SETTING);
    CHECK_MEM(d.key, ((const uint8_t[]){0x55, 0x14, KCH_PART_ALL}), 3);
    in.run = 1;
    CHECK_EQ(kch_cc(&in, 2, 0x52, 1).act, KCH_STOCK);
    CHECK_EQ(kch_cc(&in, 2, 0x51, 1).act, KCH_STOCK);
    CHECK_EQ(kch_cc(&in, 2, 0x5C, 0x7F).act, KCH_DROP);
    /* the part path: everything is part state but Local Control */
    in = unbound(2);
    in.mode = 2;
    CHECK_EQ(kch_cc(&in, 9, 0x07, 1).act, KCH_STOCK);
    CHECK_EQ(kch_cc(&in, 9, 0x01, 1).act, KCH_STOCK);
    CHECK_EQ(kch_cc(&in, 9, 0x5C, 1).act, KCH_STOCK);
    CHECK_EQ(kch_cc(&in, 9, 0x7A, 0).act, KCH_DROP);    /* Local Control: still the rx channel only */
    CHECK_EQ(kch_cc(&in, 2, 0x7A, 0).act, KCH_SETTING);
}

static struct {
    uint32_t n;
    uint8_t c[8], x[8], v[8];
    volatile int32_t *tm;
} ap;

static void f_apply(void *ctx, uint8_t c, uint8_t x, uint8_t v)
{
    (void)ctx;
    if (ap.n < 8) {
        ap.c[ap.n] = c;
        ap.x[ap.n] = x;
        ap.v[ap.n] = v;
    }
    ap.n++;
    ap.tm[c * KCH_DEF_X + x] = 400;   /* the apply re-arms (FUN_401604E8) - the stock overwrites "any" afterwards */
}

TEST(kch_defer_is_the_stock_walk)
{
    int32_t tm[45];
    uint8_t val[45];
    uint32_t any = 0;
    memset(tm, 0, sizeof tm);
    memset(val, 0xFF, sizeof val);
    memset(&ap, 0, sizeof ap);
    ap.tm = tm;
    /* not armed: nothing read, nothing written */
    tm[3] = 1;
    CHECK_EQ(kch_defer(tm, val, &any, 2, f_apply, 0), 0);
    CHECK_EQ(tm[3], 1);
    /* armed: counts down, expires, applies only a value < 0x80 */
    tm[3] = 400;
    tm[7] = 3;
    val[7] = 0x22;
    tm[44] = 2;              /* expires with value 0xFF: cleared, no apply */
    tm[10] = -5;             /* negative = idle */
    any = 1;
    CHECK_EQ(kch_defer(tm, val, &any, 2, f_apply, 0), 0);
    CHECK_EQ(tm[3], 398);
    CHECK_EQ(tm[7], 1);
    CHECK_EQ(tm[44], 0);
    CHECK_EQ(tm[10], -5);
    CHECK_EQ(any, 1);
    CHECK_EQ(kch_defer(tm, val, &any, 2, f_apply, 0), 1);
    CHECK_EQ(ap.c[0], 1);
    CHECK_EQ(ap.x[0], 2);
    CHECK_EQ(ap.v[0], 0x22);
    CHECK_EQ(val[7], 0xFF);
    CHECK_EQ(tm[7], 400);    /* re-armed by the apply */
    CHECK_EQ(any, 1);        /* slot 3 still running */
    /* the last running slot expires in the same pass as a re-arming apply: "any" ends 0 (the stock's order) */
    memset(tm, 0, sizeof tm);
    tm[0] = 1;
    val[0] = 0x01;
    any = 1;
    ap.n = 0;
    CHECK_EQ(kch_defer(tm, val, &any, 2, f_apply, 0), 1);
    CHECK_EQ(tm[0], 400);
    CHECK_EQ(any, 0);
    /* exactly elapsed = expired (subs / bgt) */
    tm[5] = 2;
    val[5] = 0x7F;
    any = 1;
    ap.n = 0;
    CHECK_EQ(kch_defer(tm, val, &any, 2, f_apply, 0), 1);
    CHECK_EQ(ap.v[0], 0x7F);
}

static struct {
    uint32_t n;
    uint8_t f[KST_FRAME_MAX];
} fan;

static void f_fan(void *c, const uint8_t *f, uint32_t n)
{
    (void)c;
    fan.n++;
    memcpy(fan.f, f, n);
}

static bool f_match(void *c, uint8_t i, uint8_t v1, uint8_t v2)
{
    (void)c; (void)i; (void)v1; (void)v2;
    return false;
}

static int f_value(void *c, uint8_t port, uint8_t cc, uint8_t i, uint8_t part, uint8_t j, uint8_t out[KST_FRAME_MAX])
{
    (void)c; (void)port; (void)cc; (void)i; (void)part; (void)j; (void)out;
    return -1;
}

static uint32_t frame(uint8_t *f, uint8_t v1, uint8_t v2, uint8_t part, const uint8_t *d, uint8_t n)
{
    uint32_t o = 0;
    f[o++] = 0xF0; f[o++] = 0x40; f[o++] = 0x7F; f[o++] = 0x10; f[o++] = 0x04; f[o++] = 0x1E;
    f[o++] = v1; f[o++] = v2; f[o++] = part;
    memcpy(f + o, d, n);
    o += n;
    f[o++] = 0xF7;
    return o;
}

TEST(kst_absorb_updates_the_store_now)
{
    static param_store_t store;
    static kst_t st;
    static const kst_io_t io = {.ctx = 0, .match = f_match, .value = f_value, .fan = f_fan};
    uint8_t f[KST_FRAME_MAX], v[4];
    uint32_t n, gen;
    memset(&store, 0, sizeof store);
    memset(&fan, 0, sizeof fan);
    kst_init(&st, &store, &io);
    /* Sound of part 0 after a Program Change: stored, silent key -> fanned only with S3 on */
    n = frame(f, 0x55, 0x00, 0x00, (const uint8_t[]){0x5F, 0x70, 0x05}, 3);
    gen = st.gen;
    CHECK(kst_absorb(&st, f, n));
    CHECK(param_get(&store, (param_id_t)param_find(0x10, 0x55, 0x00, 0x00), param_slot_from_part(0x00), v));
    CHECK_MEM(v, ((const uint8_t[]){0x5F, 0x70, 0x05}), 3);
    CHECK_EQ(fan.n, 0);
    CHECK_EQ(st.gen, gen);
    CHECK(kst_seen(&st, param_find(0x10, 0x55, 0x00, 0x00), param_slot_from_part(0x00)));
    CHECK(!kst_absorb(&st, f, n));   /* the same value again: no change, no frame */
    kst_s3_enable(&st, true);
    n = frame(f, 0x55, 0x00, 0x00, (const uint8_t[]){0x5F, 0x70, 0x06}, 3);
    CHECK(kst_absorb(&st, f, n));
    CHECK_EQ(fan.n, 1);
    CHECK_MEM(fan.f, f, n);
    /* Local Control: silent too */
    n = frame(f, 0x58, 0x01, 0x7F, (const uint8_t[]){0x00}, 1);
    CHECK(kst_absorb(&st, f, n));
    CHECK_EQ(fan.n, 2);
    kst_s3_enable(&st, false);
    n = frame(f, 0x58, 0x01, 0x7F, (const uint8_t[]){0x01}, 1);
    CHECK(kst_absorb(&st, f, n));
    CHECK_EQ(fan.n, 2);
    /* 55 14 (effect speed via CC 0x5C): not a stock key - an unexplained difference would have been fanned: always */
    n = frame(f, 0x55, 0x14, 0x08, (const uint8_t[]){0x7F}, 1);
    CHECK(kst_absorb(&st, f, n));
    CHECK_EQ(fan.n, 3);
    /* a key the stock notifies itself (headphone): stored, never fanned here */
    n = frame(f, 0x51, 0x29, 0x7F, (const uint8_t[]){0x01}, 1);
    CHECK(kst_absorb(&st, f, n));
    CHECK_EQ(fan.n, 3);
    /* bad / unknown frames: counted, store untouched */
    n = frame(f, 0x55, 0x01, 0x1F, (const uint8_t[]){0x10}, 1);
    CHECK(!kst_absorb(&st, f, n));
    f[n - 1] = 0;
    CHECK(!kst_absorb(&st, f, n));
    CHECK_EQ(st.abs.frames, 9);
    CHECK_EQ(st.abs.changed, 6);
    CHECK_EQ(st.abs.fanned, 3);
    CHECK_EQ(st.abs.bad, 2);
}

void suite_kpchan(void);
void suite_kpchan(void)
{
    printf("MIDI channel messages / delayed apply (Phase 4)\n");
    RUN(kch_path_and_accept_follow_the_stock);
    RUN(kch_program_change);
    RUN(kch_control_change);
    RUN(kch_defer_is_the_stock_walk);
    RUN(kst_absorb_updates_the_store_now);
}
