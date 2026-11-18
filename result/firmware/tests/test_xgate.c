/* executor gate (Phase 4 A) */
#include "ca79ng/xgate.h"
#include "test.h"

TEST(xgate_off_runs_everything)
{
    xg_t g;
    uint8_t raw[XG_RAW];
    xg_init(&g);
    CHECK(!g.on);
    /* the case every rule would hold - off: the stock runs it, nothing counted */
    CHECK_EQ(xg_media(&g, 0x80, XG_TYPE_KSO, 4), XG_RUN);
    CHECK_EQ(xg_xport(&g, 0x80, 4), XG_RUN);
    CHECK_EQ(g.media_run + g.media_defer + g.xport_run + g.xport_defer, 0);
    xg_encode(&g, XG_F_BUILT, 2, raw);
    CHECK_EQ(raw[0], XG_LAYOUT);
    CHECK_EQ(raw[1], XG_F_BUILT);
    CHECK_EQ(raw[2], 0xFF);   /* hook rc: never tried */
    CHECK_EQ(raw[3], 0xFF);
    CHECK_EQ(raw[9], 2);
}

TEST(xgate_media_rules)
{
    xg_t g;
    xg_init(&g);
    xg_enable(&g, true);
    /* nothing pending: run (the stock returns at once), not counted */
    CHECK_EQ(xg_media(&g, 0x00, XG_TYPE_KSO, 4), XG_RUN);
    CHECK_EQ(xg_media(&g, 0x01, XG_TYPE_KSO, 4), XG_RUN);
    CHECK_EQ(g.media_run, 0);
    /* mount events always run */
    for (uint8_t st = 0; st < 8; st++)
        CHECK_EQ(xg_media(&g, 0x81, XG_TYPE_KSO, st), XG_RUN);
    CHECK_EQ(g.media_run, 8);
    /* removal, other types or no recording: run */
    for (uint8_t ty = 0; ty < 8; ty++)
        for (uint8_t st = 0; st < 8; st++) {
            int want = ty == XG_TYPE_KSO && st >= 3 && st <= 6 ? XG_DEFER : XG_RUN;
            CHECK_EQ(xg_media(&g, 0x80, ty, st), want);
        }
    CHECK_EQ(g.media_defer, 4);
    CHECK_EQ(g.media_release, 1);   /* the (3, 7) pass after the (3, 6) hold */
    CHECK_EQ(g.media_run, 8 + 60);
}

TEST(xgate_media_hold_and_release)
{
    xg_t g;
    xg_init(&g);
    xg_enable(&g, true);
    for (int i = 0; i < 1000; i++)   /* a long take: held every pass */
        CHECK_EQ(xg_media(&g, 0x80, XG_TYPE_KSO, 4), XG_DEFER);
    CHECK_EQ(g.m_held, 1);
    CHECK_EQ(g.why_media, XG_WHY_REC);
    CHECK_EQ(g.last_media, 0x80);
    /* the take ends (commit -> 2): the stock runs the removal */
    CHECK_EQ(xg_media(&g, 0x80, XG_TYPE_KSO, 2), XG_RUN);
    CHECK_EQ(g.media_release, 1);
    CHECK_EQ(g.media_defer, 1000);
    CHECK_EQ(g.m_held, 0);
    /* held, then the stick comes back: the mount event replaces it and runs */
    CHECK_EQ(xg_media(&g, 0x80, XG_TYPE_KSO, 6), XG_DEFER);
    CHECK_EQ(xg_media(&g, 0x81, XG_TYPE_KSO, 6), XG_RUN);
    CHECK_EQ(g.media_release, 2);
    /* held, switched off: forgotten, the next pass is the stock's */
    CHECK_EQ(xg_media(&g, 0x80, XG_TYPE_KSO, 3), XG_DEFER);
    xg_enable(&g, false);
    CHECK_EQ(g.m_held, 0);
    CHECK_EQ(xg_media(&g, 0x80, XG_TYPE_KSO, 3), XG_RUN);
}

TEST(xgate_xport_rules)
{
    xg_t g;
    xg_init(&g);
    xg_enable(&g, true);
    CHECK_EQ(xg_xport(&g, 0x00, 4), XG_RUN);   /* nothing pending */
    CHECK_EQ(xg_xport(&g, 0x06, 4), XG_RUN);
    CHECK_EQ(g.xport_run, 0);
    /* every op without a file operation runs */
    for (uint8_t op = 0; op < 8; op++)
        CHECK_EQ(xg_xport(&g, (uint8_t)(0x80 | op), 0), XG_RUN);
    /* during a file operation only the stop is held; play / record ... get the stock's Busy */
    for (uint8_t op = 1; op < 8; op++)
        CHECK_EQ(xg_xport(&g, (uint8_t)(0x80 | op), 4), XG_RUN);
    CHECK_EQ(g.xport_run, 15);
    CHECK_EQ(xg_xport(&g, 0x80, 4), XG_DEFER);
    CHECK_EQ(g.why_xport, XG_WHY_FILEOP);
    CHECK_EQ(xg_xport(&g, 0x80, 9), XG_DEFER);   /* any non-zero kind */
    CHECK_EQ(g.x_wait, 2);
    /* the file operation ends: the stop runs */
    CHECK_EQ(xg_xport(&g, 0x80, 0), XG_RUN);
    CHECK_EQ(g.xport_release, 1);
    CHECK_EQ(g.xport_defer, 2);
    CHECK_EQ(g.x_held, 0);
    CHECK_EQ(g.x_wait, 0);
    /* a held stop replaced by another command (the stock writer): that one decides */
    CHECK_EQ(xg_xport(&g, 0x80, 4), XG_DEFER);
    CHECK_EQ(xg_xport(&g, 0x81, 4), XG_RUN);
    CHECK_EQ(g.xport_release, 2);
    /* cleared under it (FUN_40123848 writes 0 when it refuses): nothing held */
    CHECK_EQ(xg_xport(&g, 0x80, 4), XG_DEFER);
    CHECK_EQ(xg_xport(&g, 0x00, 4), XG_RUN);
    CHECK_EQ(g.x_held, 0);
}

TEST(xgate_xport_cap)
{
    xg_t g;
    uint8_t raw[XG_RAW];
    xg_init(&g);
    xg_enable(&g, true);
    for (unsigned i = 0; i < XG_XPORT_CAP; i++)
        CHECK_EQ(xg_xport(&g, 0x80, 5), XG_DEFER);
    CHECK_EQ(g.x_wait, XG_XPORT_CAP);
    CHECK_EQ(xg_xport(&g, 0x80, 5), XG_RUN);   /* the cap: the stock (Busy) */
    CHECK_EQ(g.xport_capped, 1);
    CHECK_EQ(g.why_xport, XG_WHY_CAP);
    CHECK_EQ(g.xport_release, 0);
    /* the stock cleared it; a new stop is held from zero again */
    CHECK_EQ(xg_xport(&g, 0x00, 5), XG_RUN);
    CHECK_EQ(xg_xport(&g, 0x80, 5), XG_DEFER);
    CHECK_EQ(g.x_wait, 1);
    g.hooked = XG_HK_MEDIA | XG_HK_XPORT;
    g.rc_media = 0, g.rc_xport = 2;
    xg_encode(&g, XG_F_BUILT, 3, raw);
    CHECK_EQ(raw[1], XG_F_BUILT | XG_F_ON | 0x0C);
    CHECK_EQ(raw[2], 0);
    CHECK_EQ(raw[3], 2);
    CHECK_EQ(raw[5], 0x80);
    CHECK_EQ(raw[7], XG_WHY_FILEOP);
    CHECK_EQ(raw[8], 2);
    CHECK_EQ(raw[10] | raw[11] << 8, 1);
    CHECK_EQ(raw[24] | raw[25] << 8, XG_XPORT_CAP + 1);   /* xport_defer */
    CHECK_EQ(raw[32], 1);                                 /* xport_capped */
    xg_enable(&g, false);
    CHECK_EQ(g.x_held, 0);
    CHECK_EQ(xg_xport(&g, 0x80, 5), XG_RUN);
}

void suite_xgate(void)
{
    printf("xgate (Phase 4 executor gate)\n");
    RUN(xgate_off_runs_everything);
    RUN(xgate_media_rules);
    RUN(xgate_media_hold_and_release);
    RUN(xgate_xport_rules);
    RUN(xgate_xport_cap);
}
