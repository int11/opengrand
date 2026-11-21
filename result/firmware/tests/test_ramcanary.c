/* RAM canary (op 0x20 core): bounded fill, round-robin verify, change records */
#include <string.h>

#include "ca79ng/ramcanary.h"
#include "test.h"

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* three host buffers posing as piano addresses */
static uint32_t m0[1000], m1[300], m2[64];
static const rcn_region_t TAB[3] = {
    {0x40601000u, 0x40601000u + sizeof m0, (uintptr_t)m0},
    {0x41944000u, 0x41944000u + sizeof m1, (uintptr_t)m1},
    {0x403C1000u, 0x403C1000u + sizeof m2, (uintptr_t)m2},
};

TEST(ramcanary_arm_rules)
{
    rcn_t c;
    memset(m0, 0xEE, sizeof m0);
    rcn_init(&c, TAB, 3);
    CHECK_EQ(rcn_step(&c, 100), 0);         /* not armed: nothing touched */
    CHECK_EQ(m0[0], 0xEEEEEEEEu);
    CHECK_EQ(rcn_arm(&c, 0), 1);
    CHECK_EQ(rcn_arm(&c, 0x08), 1);         /* region 3 does not exist */
    CHECK_EQ(c.armed, 0);
    CHECK_EQ(rcn_arm(&c, 0x05), 0);
    CHECK_EQ(c.phase, RCN_PH_FILL);
    CHECK_EQ(c.r[0].state, RCN_FILL);
    CHECK_EQ(c.r[1].state, RCN_OFF);
    CHECK_EQ(rcn_words(&c, 0x07), 1000 + 300 + 64);
}

TEST(ramcanary_fill_is_bounded)
{
    rcn_t c;
    memset(m0, 0, sizeof m0), memset(m1, 0x11, sizeof m1), memset(m2, 0, sizeof m2);
    rcn_init(&c, TAB, 3);
    rcn_arm(&c, 0x05);                      /* regions 0 and 2 */
    CHECK_EQ(rcn_step(&c, 256), 256);
    CHECK_EQ(m0[255], rcn_pattern(0x40601000u + 4 * 255));
    CHECK_EQ(m0[256], 0);                  /* the budget stops it */
    unsigned passes = 1;
    while (c.filled != c.armed && passes < 100)
        rcn_step(&c, 256), passes++;
    CHECK_EQ(passes, 5);                   /* 1064 words / 256 per pass */
    CHECK_EQ(c.filled, 0x05);
    CHECK_EQ(c.phase, RCN_PH_VERIFY);
    for (unsigned i = 0; i < 1000; i++)
        if (m0[i] != rcn_pattern(TAB[0].lo + 4 * i)) { CHECK(0); break; }
    for (unsigned i = 0; i < 64; i++)
        if (m2[i] != rcn_pattern(TAB[2].lo + 4 * i)) { CHECK(0); break; }
    CHECK_EQ(m1[0], 0x11111111u);          /* region 1 not armed: never written */
    CHECK_EQ(c.filled_words, 1064);
}

TEST(ramcanary_verify_counts_changes)
{
    rcn_t c;
    uint8_t raw[RCN_REGION], st[RCN_STATUS];
    rcn_init(&c, TAB, 3);
    rcn_arm(&c, 0x03);
    while (c.filled != c.armed)
        rcn_step(&c, 512);
    /* two full sweeps of both regions with no change */
    for (unsigned k = 0; k < 2 * 1300 / 100 + 2; k++)
        rcn_step(&c, 100);
    CHECK(c.r[0].sweeps >= 2);
    CHECK(c.r[1].sweeps >= 2);
    CHECK_EQ(c.dirty, 0);
    CHECK_EQ(c.r[0].changed_last, 0);
    /* the "stock" writes three words of region 1 and one of region 0 */
    m1[10] = 0x12345678u, m1[11] = 0, m1[200] = rcn_pattern(TAB[1].lo + 4 * 10);   /* a moved pattern word counts too */
    m0[999] = 0xDEADBEEFu;
    uint32_t s0 = c.r[0].sweeps, s1 = c.r[1].sweeps;
    while (c.r[0].sweeps < s0 + 2 || c.r[1].sweeps < s1 + 2)
        rcn_step(&c, 100);
    CHECK_EQ(c.dirty, 0x03);
    CHECK_EQ(c.r[1].changed_last, 3);
    CHECK_EQ(c.r[1].changed_max, 3);
    CHECK_EQ(c.r[1].first_addr, TAB[1].lo + 40);
    CHECK_EQ(c.r[1].first_val, 0x12345678u);
    CHECK_EQ(c.r[1].min_addr, TAB[1].lo + 40);
    CHECK_EQ(c.r[1].max_addr, TAB[1].lo + 800);
    CHECK(c.r[1].first_sweep > s1 && c.r[1].first_sweep <= s1 + 1);
    CHECK_EQ(c.r[0].changed_last, 1);
    CHECK_EQ(c.r[0].first_addr, TAB[0].lo + 4 * 999);
    /* nothing is written back: the changed words keep the stock's values */
    CHECK_EQ(m1[10], 0x12345678u);
    CHECK_EQ(m0[999], 0xDEADBEEFu);
    /* the stock puts one back: changed_last follows, max and first stay */
    m1[11] = rcn_pattern(TAB[1].lo + 44);
    s1 = c.r[1].sweeps;
    while (c.r[1].sweeps < s1 + 2)
        rcn_step(&c, 100);
    CHECK_EQ(c.r[1].changed_last, 2);
    CHECK_EQ(c.r[1].changed_max, 3);
    rcn_encode_region(&c, 1, raw);
    CHECK_EQ(raw[0], RCN_VERIFY);
    CHECK_EQ(raw[1], 1);
    CHECK_EQ(le32(raw + 4), TAB[1].lo);
    CHECK_EQ(le32(raw + 8), TAB[1].hi);
    CHECK_EQ(le32(raw + 16), c.r[1].sweeps);
    CHECK_EQ(le32(raw + 20), 2);
    CHECK_EQ(le32(raw + 24), 3);
    CHECK_EQ(le32(raw + 28), TAB[1].lo + 40);
    CHECK_EQ(le32(raw + 32), TAB[1].lo + 800);
    CHECK_EQ(le32(raw + 36), TAB[1].lo + 40);
    CHECK_EQ(le32(raw + 40), 0x12345678u);
    rcn_encode_status(&c, 100, 7, 9, st);
    CHECK_EQ(st[0], 1);
    CHECK_EQ(st[1], 3);
    CHECK_EQ(st[2], RCN_PH_VERIFY);
    CHECK_EQ(st[4] | st[5] << 8, 0x03);
    CHECK_EQ(st[6] | st[7] << 8, 0x03);
    CHECK_EQ(st[8] | st[9] << 8, 0x03);
    CHECK_EQ(st[10] | st[11] << 8, 100);
    CHECK_EQ(le32(st + 12), c.passes);
    CHECK_EQ(le32(st + 16), 1300);
    CHECK_EQ(le32(st + 24), 7);
    CHECK_EQ(le32(st + 28), 9);
    /* disarm: no more reads or writes, the records stay; a new arm clears them */
    rcn_disarm(&c);
    m1[50] = 1;
    uint32_t v = c.verified_words;
    CHECK_EQ(rcn_step(&c, 100), 0);
    CHECK_EQ(c.verified_words, v);
    rcn_encode_region(&c, 1, raw);
    CHECK_EQ(raw[0], RCN_OFF);
    CHECK_EQ(raw[1], 0);
    CHECK_EQ(le32(raw + 24), 3);
    rcn_arm(&c, 0x02);
    CHECK_EQ(c.r[1].changed_max, 0);
    CHECK_EQ(c.dirty, 0);
    rcn_encode_region(&c, 5, raw);          /* outside the table: zeros */
    CHECK_EQ(le32(raw + 4), 0);
}

void suite_ramcanary(void)
{
    printf("ramcanary (RAM canary, op 0x20)\n");
    RUN(ramcanary_arm_rules);
    RUN(ramcanary_fill_is_bounded);
    RUN(ramcanary_verify_counts_changes);
}
