/* Phase 3-6 R3: save decision (src/recorder/recsave.c) over hand-made recorder views.  The stock write paths themselves
 * are run in the emulator (result/stockfw/tools/roundtrip_recsave.py). */
#include "ca79ng/recsave.h"
#include "test.h"

/* song 1 part 1, song 2 both parts, songs 3-10 empty; stopped, nothing busy, table OK */
static rv_view_t view(void)
{
    rv_view_t v = {0};
    v.state = RV_ST_STOP, v.music_mode = 2, v.store = RV_STORE_OK;
    v.songs[0] = (rv_song_t){0x200, 3, RV_SONG_PART1};
    v.songs[1] = (rv_song_t){0x300, 5, RV_SONG_PART1 | RV_SONG_PART2};
    for (int i = 2; i < RV_SONGS; i++)
        v.songs[i] = (rv_song_t){0x74, 0, 0};
    return v;
}

static void on(rs_t *rs, const rv_view_t *v)
{
    rs_init(rs);
    rs_enable(rs, 1);
    CHECK_EQ(rs_poll(rs, v), 0);   /* settled: base taken */
}

TEST(recsave_off_is_inert)
{
    rs_t rs;
    rs_init(&rs);
    rv_view_t v = view();
    CHECK_EQ(rs_delete(&rs, &v, 0x04, 0, 0x7F), RS_V_IGNORE);
    CHECK_EQ(rs_file(&rs, &v, 0x09, 0), RS_V_PASS);
    v.state = RV_ST_RECORDING;
    CHECK_EQ(rs_poll(&rs, &v), 0);
    v.state = RV_ST_STOP, v.busy = RV_BUSY_JOB;
    CHECK_EQ(rs_poll(&rs, &v), 0);
    CHECK_EQ(rs_busy(&rs), 0);
    CHECK_EQ(rs.n_windows, 0);
}

TEST(recsave_commit_window)
{
    rs_t rs;
    rv_view_t v = view();
    v.song = 2;
    on(&rs, &v);
    v.state = RV_ST_RECORDING, v.busy = RV_BUSY_REC;
    CHECK_EQ(rs_poll(&rs, &v), 0);
    CHECK_EQ(rs_busy(&rs), 0);   /* recording is R2's; the window opens at the commit */
    /* the commit: job queued, then state 2 (FUN_40168774) - seen together */
    v.state = RV_ST_STOP, v.busy = RV_BUSY_JOB;
    v.songs[2] = (rv_song_t){0x24C, 4, RV_SONG_PART1};
    CHECK_EQ(rs_poll(&rs, &v), 0);
    CHECK_EQ(rs_busy(&rs), 1);
    CHECK_EQ(rs.kind, RS_K_COMMIT);
    CHECK_EQ(rs.target, 2);
    CHECK_EQ(rs.jobs, 1);
    CHECK_EQ(rs_wants_table(&rs, &v), 0);
    v.busy = RV_BUSY_WORKER;
    CHECK_EQ(rs_poll(&rs, &v), 0);
    v.busy = 0;
    CHECK_EQ(rs_wants_table(&rs, &v), 1);
    CHECK_EQ(rs_poll(&rs, &v), 1);
    CHECK_EQ(rs.last, RS_R_OK);
    CHECK_EQ(rs.last_changed, 1u << 2);
    CHECK_EQ(rs_busy(&rs), 0);
    CHECK_EQ(rs.n_ok, 1);
    /* the next window compares with the new table */
    CHECK_EQ(rs.base_len[2], 0x24C);
}

TEST(recsave_commit_bad_results)
{
    rs_t rs;
    rv_view_t v = view();
    on(&rs, &v);
    /* another song changed too */
    v.state = RV_ST_RECORDING;
    rs_poll(&rs, &v);
    v.state = RV_ST_STOP, v.busy = RV_BUSY_JOB;
    rs_poll(&rs, &v);
    v.busy = 0, v.songs[0].len = 0x204, v.songs[4].len = 0x80;
    CHECK_EQ(rs_poll(&rs, &v), 1);
    CHECK_EQ(rs.last, RS_R_OTHER);
    CHECK_EQ(rs.last_changed, (1u << 0) | (1u << 4));
    /* the table fails the check afterwards */
    v.state = RV_ST_RECORDING;
    rs_poll(&rs, &v);
    v.state = RV_ST_STOP, v.busy = RV_BUSY_JOB;
    rs_poll(&rs, &v);
    v.busy = 0, v.store = RV_STORE_SONG_ALIGN;
    CHECK_EQ(rs_poll(&rs, &v), 1);
    CHECK_EQ(rs.last, RS_R_TABLE);
    /* two jobs in one window: a job that falls and rises again while something keeps the window open (a job that
     * falls and rises between two polls is one job here) */
    v = view();
    on(&rs, &v);
    v.state = RV_ST_RECORDING;
    rs_poll(&rs, &v);
    v.state = RV_ST_STOP, v.busy = RV_BUSY_JOB;
    rs_poll(&rs, &v);
    v.busy = RV_BUSY_FILEOP;   /* job gone, a file operation keeps the window open */
    rs_poll(&rs, &v);
    v.busy = RV_BUSY_JOB | RV_BUSY_FILEOP;   /* a second job rises */
    rs_poll(&rs, &v);
    v.busy = 0;
    CHECK_EQ(rs_poll(&rs, &v), 1);
    CHECK_EQ(rs.last, RS_R_DOUBLE);
    CHECK_EQ(rs.last_jobs, 2);
}

TEST(recsave_delete_rules)
{
    rs_t rs;
    rv_view_t v = view();
    on(&rs, &v);
    CHECK_EQ(rs_delete(&rs, &v, 0x00, 1, 0x7F), RS_V_IGNORE);    /* stock drops fmt 00 */
    CHECK_EQ(rs_delete(&rs, &v, 0x04, 12, 0x7F), RS_V_ARG);      /* stock: song 1 */
    CHECK_EQ(rs_delete(&rs, &v, 0x04, 1, 0x05), RS_V_IGNORE);    /* FUN_4016C800: nothing */
    CHECK_EQ(rs.n_windows, 0);
    static const uint8_t busy_states[] = {RV_ST_PLAY, RV_ST_REC_STANDBY, RV_ST_RECORDING, RV_ST_REC_STANDBY_NOMETRO,
                                          RV_ST_COUNT_IN, 7};
    for (unsigned i = 0; i < sizeof busy_states; i++) {
        rv_view_t b = v;
        b.state = busy_states[i];
        CHECK_EQ(rs_delete(&rs, &b, 0x04, 1, 0x7F), RS_V_BUSY);
    }
    rv_view_t b = v;
    b.busy = RV_BUSY_JOB;
    CHECK_EQ(rs_delete(&rs, &b, 0x04, 1, 0x7F), RS_V_BUSY);
    b.busy = RV_BUSY_FILEOP;
    CHECK_EQ(rs_delete(&rs, &b, 0x04, 1, 0x7F), RS_V_BUSY);
    b = v, b.store = RV_STORE_TRACK_FA;
    CHECK_EQ(rs_delete(&rs, &b, 0x04, 1, 0x7F), RS_V_BUSY);       /* single song in a broken table */
    CHECK_EQ(rs.n_refused, 1 + 6 + 2 + 1);
    CHECK_EQ(rs.n_windows, 0);
    /* accepted: song 2, part 2 */
    CHECK_EQ(rs_delete(&rs, &v, 0x04, 1, 1), RS_V_PASS);
    CHECK_EQ(rs_busy(&rs), 1);
    CHECK_EQ(rs.kind, RS_K_DELETE);
    CHECK_EQ(rs.target, 1);
    CHECK_EQ(rs_delete(&rs, &v, 0x04, 0, 0x7F), RS_V_BUSY);       /* one write at a time */
    CHECK_EQ(rs_file(&rs, &v, 0x09, 3), RS_V_BUSY);
    /* the stock apply ran (task 19): table moved, job queued */
    v.busy = RV_BUSY_JOB, v.songs[1] = (rv_song_t){0x1A0, 5, RV_SONG_PART1};
    CHECK_EQ(rs_poll(&rs, &v), 0);
    v.busy = 0;
    CHECK_EQ(rs_poll(&rs, &v), 1);
    CHECK_EQ(rs.last, RS_R_OK);
    CHECK_EQ(rs.last_kind, RS_K_DELETE);
    CHECK_EQ(rs.n_delete, 1);
    /* format: every song may change; allowed with a broken table */
    v.store = RV_STORE_ORDER;
    CHECK_EQ(rs_poll(&rs, &v), 0);
    CHECK_EQ(rs_delete(&rs, &v, 0x7F, RS_ALL_SONGS, 0x7F), RS_V_PASS);
    CHECK_EQ(rs.target, RS_T_ALL);
    v.busy = RV_BUSY_JOB;
    rs_poll(&rs, &v);
    v.busy = 0, v.store = RV_STORE_OK;
    for (int i = 0; i < RV_SONGS; i++)
        v.songs[i] = (rv_song_t){0x74, 0, 0};
    CHECK_EQ(rs_poll(&rs, &v), 1);
    CHECK_EQ(rs.last, RS_R_OK);
    CHECK_EQ(rs.last_kind, RS_K_FORMAT);
}

TEST(recsave_delete_without_job)
{
    rs_t rs;
    rv_view_t v = view();
    on(&rs, &v);
    CHECK_EQ(rs_delete(&rs, &v, 0x04, 0, 0x7F), RS_V_PASS);
    unsigned n = 0;
    while (rs_busy(&rs) && n < RS_WAIT_POLLS + 5)
        rs_poll(&rs, &v), n++;
    CHECK_EQ(n, RS_WAIT_POLLS);
    CHECK_EQ(rs.last, RS_R_NOJOB);
    CHECK_EQ(rs.n_bad, 1);
}

TEST(recsave_delete_nowrite)
{
    rs_t rs;
    rv_view_t v = view();
    on(&rs, &v);
    CHECK_EQ(rs_delete(&rs, &v, 0x04, 0, 1), RS_V_PASS);   /* song 1 has no part 2: the core writes nothing */
    CHECK_EQ(rs_busy(&rs), 1);
    rs_nowrite(&rs);
    CHECK_EQ(rs_busy(&rs), 0);
    CHECK_EQ(rs.n_windows, 0);
    CHECK_EQ(rs.n_delete, 0);
    CHECK_EQ(rs.last, RS_R_NONE);
    CHECK_EQ(rs_delete(&rs, &v, 0x04, 1, 1), RS_V_PASS);   /* the next delete is taken at once */
    v.busy = RV_BUSY_JOB;
    CHECK_EQ(rs_poll(&rs, &v), 0);
    rs_nowrite(&rs);                                       /* a job is up: the window stays */
    CHECK_EQ(rs_busy(&rs), 1);
    CHECK_EQ(rs.jobs, 1);
}

TEST(recsave_file_rules)
{
    rs_t rs;
    rv_view_t v = view();
    on(&rs, &v);
    rv_view_t b = v;
    b.state = RV_ST_RECORDING;
    CHECK_EQ(rs_file(&rs, &b, 0x09, 3), RS_V_BUSY);   /* import would reload the player under the take */
    CHECK_EQ(rs_file(&rs, &b, 0x08, 0), RS_V_BUSY);   /* USB load shares 0x40A00000 with the commit */
    b.state = RV_ST_PLAY;
    CHECK_EQ(rs_file(&rs, &b, 0x09, 3), RS_V_BUSY);
    CHECK_EQ(rs_file(&rs, &b, 0x08, 0), RS_V_PASS);   /* USB load while the internal song plays: the stock's */
    b = v, b.busy = RV_BUSY_JOB;
    CHECK_EQ(rs_file(&rs, &b, 0x09, 3), RS_V_BUSY);
    CHECK_EQ(rs_file(&rs, &v, 0x0A, 3), RS_V_PASS);   /* save to USB: reads only */
    CHECK_EQ(rs_file(&rs, &v, 0x11, 0), RS_V_PASS);
    /* import into song 4: the next file operation is that song's */
    CHECK_EQ(rs_file(&rs, &v, 0x09, 3), RS_V_PASS);
    CHECK_EQ(rs_busy(&rs), 0);
    v.busy = RV_BUSY_FILEOP;
    rs_poll(&rs, &v);
    CHECK_EQ(rs_busy(&rs), 1);
    CHECK_EQ(rs.kind, RS_K_FILE);
    CHECK_EQ(rs.target, 3);
    v.busy = RV_BUSY_FILEOP | RV_BUSY_JOB, v.songs[3] = (rv_song_t){0x900, 12, RV_SONG_PART1 | RV_SONG_EXTRA};
    rs_poll(&rs, &v);
    v.busy = RV_BUSY_JOB;
    CHECK_EQ(rs_poll(&rs, &v), 0);
    v.busy = 0;
    CHECK_EQ(rs_poll(&rs, &v), 1);
    CHECK_EQ(rs.last, RS_R_OK);
    CHECK_EQ(rs.last_changed, 1u << 3);
    /* an export: a file operation, no job, nothing changed */
    v.busy = RV_BUSY_FILEOP;
    rs_poll(&rs, &v);
    CHECK_EQ(rs.target, RS_T_ANY);
    v.busy = 0;
    CHECK_EQ(rs_poll(&rs, &v), 1);
    CHECK_EQ(rs.last, RS_R_NOWRITE);
}

TEST(recsave_stuck_and_encode)
{
    rs_t rs;
    rv_view_t v = view();
    on(&rs, &v);
    v.state = RV_ST_RECORDING;
    rs_poll(&rs, &v);
    v.state = RV_ST_STOP, v.busy = RV_BUSY_JOB;
    unsigned n = 0;
    do
        rs_poll(&rs, &v), n++;
    while (rs_busy(&rs) && n < RS_MAX_POLLS + 5);
    CHECK_EQ(n, RS_MAX_POLLS + 1);
    CHECK_EQ(rs.last, RS_R_STUCK);
    uint8_t e[RS_ENC_LEN];
    rs_encode(&rs, e);
    CHECK_EQ(e[0], 1);
    CHECK_EQ(e[1], 1);
    CHECK_EQ(e[6], RS_R_STUCK);
    CHECK_EQ(e[7], RS_K_COMMIT);
    CHECK_EQ(e[10] | e[11] << 8, 1);
    CHECK_EQ(e[14] | e[15] << 8, 1);
}

void suite_recsave(void)
{
    printf("recsave (Phase 3-6 R3)\n");
    RUN(recsave_off_is_inert);
    RUN(recsave_commit_window);
    RUN(recsave_commit_bad_results);
    RUN(recsave_delete_rules);
    RUN(recsave_delete_without_job);
    RUN(recsave_delete_nowrite);
    RUN(recsave_file_rules);
    RUN(recsave_stuck_and_encode);
}
