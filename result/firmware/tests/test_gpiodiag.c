/* GPIO diagnostic, read only */
#include <string.h>

#include "ca79ng/gpiodiag.h"
#include "test.h"

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

TEST(gpiodiag_signal_bits)
{
    /* the record's pin -> register bit: GP3[8] = IN_DATA23 bit 24, GP5[14] = IN_DATA45 bit 30, GP6[15] / GP6[11] =
     * OUT_DATA67 0x8000 / 0x800, GP7[12] = OUT_DATA67 bit 28, GP4[15] = OUT_DATA45 bit 15, GP5[15] bit 31 */
    uint32_t w[GD_WORDS] = {0};
    w[GD_W_IN23] = 1u << 24;
    CHECK_EQ(gd_level(GD_S_GP3_8, w), 1);
    CHECK_EQ(gd_level(GD_S_GP3_1, w), 0);
    w[GD_W_IN23] = 1u << 17;
    CHECK_EQ(gd_level(GD_S_GP3_8, w), 0);
    CHECK_EQ(gd_level(GD_S_GP3_1, w), 1);
    w[GD_W_IN45] = 1u << 30;
    CHECK_EQ(gd_level(GD_S_GP5_14, w), 1);
    w[GD_W_OUT67] = 0x8000;
    CHECK_EQ(gd_level(GD_S_GP6_15, w), 1);
    CHECK_EQ(gd_level(GD_S_GP6_11, w), 0);
    w[GD_W_OUT67] = 0x0800 | 0x2000 | (1u << 28);
    CHECK_EQ(gd_level(GD_S_GP6_15, w), 0);
    CHECK_EQ(gd_level(GD_S_GP6_11, w), 1);
    CHECK_EQ(gd_level(GD_S_GP6_13, w), 1);
    CHECK_EQ(gd_level(GD_S_GP7_12, w), 1);
    w[GD_W_OUT45] = 0x8000 | 0x80000000u | 0x2000;
    CHECK_EQ(gd_level(GD_S_GP4_15, w), 1);
    CHECK_EQ(gd_level(GD_S_GP5_15, w), 1);
    CHECK_EQ(gd_level(GD_S_GP4_13, w), 1);
    CHECK_EQ(gd_level(GD_SIGNALS, w), 0);
}

TEST(gpiodiag_baseline_then_edges)
{
    gd_t g;
    uint32_t w[GD_WORDS] = {1u << 17, 0, 0, 0x8800};
    uint8_t raw[GD_LOG_MAX];
    gd_init(&g);
    /* the first pass is the baseline: no event */
    CHECK_EQ(gd_sample(&g, 0, w), 0);
    CHECK(g.started);
    CHECK_EQ(g.n, 0);
    CHECK(gd_newest(&g, 0) == NULL);
    CHECK_EQ(gd_sample(&g, 8, w), 0);
    /* GP3[8] goes 1 (mute request): one event */
    w[GD_W_IN23] |= 1u << 24;
    CHECK_EQ(gd_sample(&g, 16, w), 1);
    CHECK_EQ(gd_newest(&g, 0)->sig, GD_S_GP3_8);
    CHECK_EQ(gd_newest(&g, 0)->level, 1);
    CHECK_EQ(gd_newest(&g, 0)->t_ms, 16);
    CHECK_EQ(gd_newest(&g, 0)->seq, 1);
    /* the stock answers with amp off + mute in the same pass: two events, in signal order */
    w[GD_W_OUT67] = 0;
    CHECK_EQ(gd_sample(&g, 24, w), 2);
    CHECK_EQ(gd_newest(&g, 1)->sig, GD_S_GP6_15);
    CHECK_EQ(gd_newest(&g, 0)->sig, GD_S_GP6_11);
    CHECK_EQ(gd_newest(&g, 0)->level, 0);
    CHECK_EQ(g.seq, 3);
    /* bits outside the watched set never make an event (GP2[10] NOR RY/BY#, the NOR window GP6[10:0]) */
    w[GD_W_IN23] ^= 0x400;
    w[GD_W_OUT67] ^= 0x7FF;
    CHECK_EQ(gd_sample(&g, 32, w), 0);
    /* the log: oldest first */
    CHECK_EQ(gd_encode_log(&g, raw), 4 + 3 * GD_EVENT);
    CHECK_EQ(raw[0], GD_LAYOUT);
    CHECK_EQ(raw[1], 3);
    CHECK_EQ(raw[2] | raw[3] << 8, 3);
    CHECK_EQ(raw[4] | raw[5] << 8, 1);
    CHECK_EQ(le32(raw + 6), 16);
    CHECK_EQ(raw[10], GD_S_GP3_8);
    CHECK_EQ(raw[11], 1);
    CHECK_EQ(raw[4 + 2 * GD_EVENT + 6], GD_S_GP6_11);
    CHECK_EQ(g.samples, 5);
}

TEST(gpiodiag_ring_wraps)
{
    gd_t g;
    uint32_t w[GD_WORDS] = {0};
    uint8_t raw[GD_LOG_MAX];
    gd_init(&g);
    gd_sample(&g, 0, w);
    for (uint32_t i = 1; i <= 40; i++) {   /* headphones in / out 40 times */
        w[GD_W_IN45] ^= 1u << 30;
        CHECK_EQ(gd_sample(&g, i * 10, w), 1);
    }
    CHECK_EQ(g.n, GD_RING);
    CHECK_EQ(g.seq, 40);
    CHECK_EQ(gd_newest(&g, 0)->seq, 40);
    CHECK_EQ(gd_newest(&g, GD_RING - 1)->seq, 40 - GD_RING + 1);
    CHECK(gd_newest(&g, GD_RING) == NULL);
    CHECK_EQ(gd_encode_log(&g, raw), GD_LOG_MAX);
    CHECK_EQ(raw[4] | raw[5] << 8, 40 - GD_RING + 1);   /* oldest kept */
    CHECK_EQ(raw[4 + (GD_RING - 1) * GD_EVENT] | raw[5 + (GD_RING - 1) * GD_EVENT] << 8, 40);
    /* level alternates: event 40 = out (0) */
    CHECK_EQ(gd_newest(&g, 0)->level, 0);
    CHECK_EQ(gd_newest(&g, 1)->level, 1);
    /* seq never reads 0 after a wrap */
    g.seq = 0xFFFF;
    w[GD_W_IN45] ^= 1u << 30;
    gd_sample(&g, 999, w);
    CHECK_EQ(g.seq, 1);
}

TEST(gpiodiag_snapshot_record)
{
    gd_t g;
    gd_regs_t r;
    uint8_t raw[GD_SNAP];
    uint32_t w[GD_WORDS] = {0};
    memset(&r, 0, sizeof r);
    gd_init(&g);
    /* before the first pass: not started */
    gd_encode_snapshot(&g, &r, 0, 0x7F, 0, raw);
    CHECK_EQ(raw[0], GD_LAYOUT);
    CHECK_EQ(raw[1], 0);
    CHECK_EQ(raw[2], 0x7F);
    gd_sample(&g, 0, w);
    /* the BootProgram DSP-stage table values as the registers */
    r.dir[0] = 0xFB2BBC00u, r.dir[1] = 0x51A94F60u, r.dir[2] = 0x88F80000u;
    r.out[0] = 0x04440000u, r.out[1] = 0x80000000u, r.out[2] = 0x0306A800u;
    r.in[0] = 0x00020400u, r.in[1] = 0x40000000u, r.in[2] = 0x12345678u;
    r.hp = 1, r.gp3_level = 0, r.gp3_event = 2, r.model = 0x21, r.chip_a = 0xDD;
    for (unsigned i = 0; i < GD_PINMUX_N; i++)
        r.pinmux[i] = 0x11111111u * (i % 15) + i;
    gd_encode_snapshot(&g, &r, GD_F_NOTIFY, 2, 123456, raw);
    CHECK_EQ(raw[1], GD_F_NOTIFY | GD_F_STARTED);
    CHECK_EQ(raw[2], 2);
    CHECK_EQ(raw[3] | raw[4] << 8, 0);
    CHECK_EQ(le32(raw + 5), 0xFB2BBC00u);
    CHECK_EQ(le32(raw + 9), 0x04440000u);
    CHECK_EQ(le32(raw + 13), 0x00020400u);
    CHECK_EQ(le32(raw + 17), 0x51A94F60u);
    CHECK_EQ(le32(raw + 21), 0x80000000u);
    CHECK_EQ(le32(raw + 25), 0x40000000u);
    CHECK_EQ(le32(raw + 29), 0x88F80000u);
    CHECK_EQ(le32(raw + 33), 0x0306A800u);
    CHECK_EQ(le32(raw + 37), 0x12345678u);
    CHECK_EQ(raw[41], 1);
    CHECK_EQ(raw[42], 0);
    CHECK_EQ(raw[43], 2);
    CHECK_EQ(raw[44], 0x21);
    CHECK_EQ(raw[45], 0xDD);
    for (unsigned i = 0; i < GD_PINMUX_N; i++)
        CHECK_EQ(le32(raw + 46 + 4 * i), 0x11111111u * (i % 15) + i);
    CHECK_EQ(le32(raw + 126), 123456);
    /* unknown flag bits are not passed through */
    gd_encode_snapshot(&g, &r, 0xFE, 2, 0, raw);
    CHECK_EQ(raw[1], GD_F_STARTED);
}

void suite_gpiodiag(void)
{
    printf("gpiodiag (read-only GPIO diagnostic, op 0x17)\n");
    RUN(gpiodiag_signal_bits);
    RUN(gpiodiag_baseline_then_edges);
    RUN(gpiodiag_ring_wraps);
    RUN(gpiodiag_snapshot_record);
}
