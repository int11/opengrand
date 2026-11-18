/* Phase 3-6 R1: playback control decisions (src/recorder/recctl.c) over a hand-made recorder view
 * and recording stand-ins for the stock routines.  The stock behaviour itself is checked in the
 * emulator (result/stockfw/tools/roundtrip_recctl.py). */
#include "ca79ng/recctl.h"
#include "test.h"

/* ---- the stock side: every call recorded ---- */
static struct {
    unsigned n;
    struct { char what; uint16_t a, b; } call[16];
    uint16_t select_rc, part_rc;
} S;

static void log_call(char w, uint16_t a, uint16_t b)
{
    if (S.n < 16)
        S.call[S.n].what = w, S.call[S.n].a = a, S.call[S.n].b = b;
    S.n++;
}
static void f_post(void *c, uint8_t k, uint8_t v) { (void)c, log_call('q', k, v); }
static uint16_t f_select(void *c, uint8_t s) { (void)c, log_call('s', s, 0); return S.select_rc; }
static uint16_t f_part(void *c, uint8_t p) { (void)c, log_call('p', p, 0); return S.part_rc; }
static void f_tempo(void *c, uint16_t b) { (void)c, log_call('t', b, 0); }
static void f_seek(void *c, uint16_t b) { (void)c, log_call('k', b, 0); }
static const rc_ops_t OPS = {f_post, f_select, f_part, f_tempo, f_seek, 0};

/* a view: song 1 selected (part 1 recorded, 3 bars), song 2 both parts, song 3 empty; mode 2, stopped */
static rv_view_t view(void)
{
    rv_view_t v = {0};
    v.state = RV_ST_STOP, v.transport = RV_T_IDLE, v.music_mode = 2, v.tempo = 120, v.tempo_orig = 120;
    v.max_bars = 3, v.max_slot = 0, v.bar = 1;
    v.songs[0] = (rv_song_t){0x200, 3, RV_SONG_PART1};
    v.songs[1] = (rv_song_t){0x300, 5, RV_SONG_PART1 | RV_SONG_PART2};
    v.songs[2] = (rv_song_t){0x74, 0, 0};
    return v;
}

static void reset(rc_t *rc)
{
    S.n = 0, S.select_rc = 0, S.part_rc = 0;
    rc_init(rc, &OPS);
}

TEST(recctl_transport_via_queue)
{
    rc_t rc;
    reset(&rc);
    rv_view_t v = view();
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_OK);
    CHECK_EQ(S.n, 1);
    CHECK(S.call[0].what == 'q' && S.call[0].a == RC_Q_MUSIC && S.call[0].b == RC_MUSIC_PLAY);
    /* not there yet: nothing; then the view shows play -> one DONE */
    CHECK_EQ(rc_poll(&rc, &v), 0);
    v.state = RV_ST_PLAY;
    CHECK_EQ(rc_poll(&rc, &v), RC_EV_DONE << 8 | RC_PLAY);
    CHECK_EQ(rc_poll(&rc, &v), 0);
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_NOP);
    CHECK_EQ(rc_cmd(&rc, &v, RC_PAUSE, 0), RC_OK);
    CHECK(S.call[1].what == 'q' && S.call[1].b == RC_MUSIC_PAUSE);
    v.state = RV_ST_STOPPED0;
    CHECK_EQ(rc_poll(&rc, &v), RC_EV_DONE << 8 | RC_PAUSE);
    CHECK_EQ(rc_cmd(&rc, &v, RC_PAUSE, 0), RC_NOP);
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_OK);
    CHECK(S.call[2].what == 'q' && S.call[2].b == RC_MUSIC_STOP);
    v.state = RV_ST_STOP;
    CHECK_EQ(rc_poll(&rc, &v), RC_EV_DONE << 8 | RC_STOP);
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_NOP);
    CHECK_EQ(S.n, 3);
}

TEST(recctl_enters_internal_mode)
{
    rc_t rc;
    reset(&rc);
    rv_view_t v = view();
    v.music_mode = 0; /* no song mode yet: MusicMode 2 goes first, in the same queue */
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_OK);
    CHECK_EQ(S.n, 2);
    CHECK(S.call[0].what == 'q' && S.call[0].a == RC_Q_MODE && S.call[0].b == RC_MODE_INTERNAL);
    CHECK(S.call[1].what == 'q' && S.call[1].a == RC_Q_MUSIC && S.call[1].b == RC_MUSIC_PLAY);
    /* another player's mode: refused, nothing posted */
    for (uint8_t m = 3; m <= 6; m++) {
        reset(&rc);
        v.music_mode = m;
        CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_E_MODE);
        CHECK_EQ(rc_cmd(&rc, &v, RC_SELECT, 1), RC_E_MODE);
        CHECK_EQ(S.n, 0);
    }
}

TEST(recctl_refuses_busy_and_recording)
{
    rc_t rc;
    rv_view_t v;
    static const uint8_t busy[] = {RV_BUSY_JOB, RV_BUSY_WORKER, RV_BUSY_FILEOP};
    for (unsigned i = 0; i < sizeof busy; i++) {
        reset(&rc);
        v = view(), v.busy = busy[i];
        CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_E_BUSY);
        CHECK_EQ(rc_cmd(&rc, &v, RC_SELECT, 1), RC_E_BUSY);
        CHECK_EQ(rc_cmd(&rc, &v, RC_PART, 0), RC_E_BUSY);
        CHECK_EQ(rc_cmd(&rc, &v, RC_SEEK, 2), RC_E_BUSY);
        /* tempo reads no song: allowed */
        CHECK_EQ(rc_cmd(&rc, &v, RC_TEMPO, 90), RC_OK);
        CHECK_EQ(S.n, 1);
        CHECK_EQ(rc.n_refused, 4);
    }
    /* stop reloads the song: refused only while a file operation can be moving the table */
    reset(&rc);
    v = view(), v.state = RV_ST_PLAY, v.busy = RV_BUSY_JOB;
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_OK);
    reset(&rc);
    v.busy = RV_BUSY_FILEOP;
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_E_BUSY);
    CHECK_EQ(rc_cmd(&rc, &v, RC_PAUSE, 0), RC_OK);
    /* a torn / failed table */
    reset(&rc);
    v = view(), v.store = RV_STORE_SONG_ALIGN;
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_E_BUSY);
    CHECK_EQ(rc_cmd(&rc, &v, RC_SELECT, 0), RC_E_BUSY);
    CHECK_EQ(S.n, 0);
    /* every recorder state 3-6 is R2's: nothing at all, not even stop (stop from 4 commits) */
    for (uint8_t st = 3; st <= 6; st++) {
        reset(&rc);
        v = view(), v.state = st, v.transport = rv_transport(st), v.busy = RV_BUSY_REC;
        for (uint8_t c = RC_PLAY; c <= RC_SEEK; c++)
            CHECK_EQ(rc_cmd(&rc, &v, c, c == RC_TEMPO ? 100 : 1), RC_E_REC);
        CHECK_EQ(S.n, 0);
    }
    reset(&rc);
    v = view(), v.state = 7;
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_E_STATE);
    CHECK_EQ(S.n, 0);
}

TEST(recctl_queue_guard)
{
    rc_t rc;
    reset(&rc);
    rv_view_t v = view();
    v.mq_hold = 0x82; /* a deferred MusicMode: FUN_4015DBD4 would drop the Music command */
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_E_BUSY);
    v.mq_hold = 0, v.mq_count = RC_Q_MAX; /* one more post would empty the stock queue */
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_E_BUSY);
    v.mq_count = RC_Q_MAX - 1;
    v.music_mode = 0; /* needs two entries */
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_E_BUSY);
    v.music_mode = 2;
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_OK);
    CHECK_EQ(S.n, 1);
}

TEST(recctl_no_data)
{
    rc_t rc;
    reset(&rc);
    rv_view_t v = view();
    v.song = 2; /* empty */
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_E_NODATA);
    v.song = 0, v.play_sel = 2; /* part 2 of a part-1-only song */
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_E_NODATA);
    v.play_sel = 1;
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_OK);
    reset(&rc);
    v = view(), v.tempo_orig = 0;
    CHECK_EQ(rc_cmd(&rc, &v, RC_TEMPO, 100), RC_E_NODATA);
    v = view(), v.max_bars = 0;
    CHECK_EQ(rc_cmd(&rc, &v, RC_SEEK, 1), RC_E_NODATA);
    CHECK_EQ(S.n, 0);
}

TEST(recctl_args)
{
    rc_t rc;
    reset(&rc);
    rv_view_t v = view();
    CHECK_EQ(rc_cmd(&rc, &v, RC_SELECT, 10), RC_E_ARG); /* stock would take song 1 */
    CHECK_EQ(rc_cmd(&rc, &v, RC_PART, 2), RC_E_ARG);
    CHECK_EQ(rc_cmd(&rc, &v, RC_TEMPO, RC_TEMPO_MIN - 1), RC_E_ARG);
    CHECK_EQ(rc_cmd(&rc, &v, RC_TEMPO, RC_TEMPO_MAX + 1), RC_E_ARG);
    CHECK_EQ(rc_cmd(&rc, &v, RC_SEEK, 0), RC_E_ARG);
    CHECK_EQ(rc_cmd(&rc, &v, RC_SEEK, 4), RC_E_ARG); /* 3 bars */
    CHECK_EQ(rc_cmd(&rc, &v, 0, 0), RC_E_ARG);
    CHECK_EQ(rc_cmd(&rc, &v, 99, 0), RC_E_ARG);
    CHECK_EQ(rc_cmd(&rc, &v, RC_TEMPO, 120), RC_NOP); /* already */
    CHECK_EQ(S.n, 0);
}

TEST(recctl_direct_calls)
{
    rc_t rc;
    reset(&rc);
    rv_view_t v = view();
    S.select_rc = 0xEC;
    CHECK_EQ(rc_cmd(&rc, &v, RC_SELECT, 2), RC_OK); /* empty song: selected all the same, rc kept */
    CHECK(S.call[0].what == 's' && S.call[0].a == 2);
    CHECK_EQ(rc.last_rc, 0xEC);
    CHECK_EQ(rc_cmd(&rc, &v, RC_PART, 0), RC_E_PENDING); /* one at a time */
    v.song = 2;
    CHECK_EQ(rc_poll(&rc, &v), RC_EV_DONE << 8 | RC_SELECT);
    CHECK_EQ(rc_cmd(&rc, &v, RC_PART, 1), RC_OK);
    CHECK(S.call[1].what == 'p' && S.call[1].a == 1);
    v.play_sel = rc_play_sel_of(1);
    CHECK_EQ(v.play_sel, 2);
    CHECK_EQ(rc_play_sel_of(0x7F), 0);
    CHECK_EQ(rc_poll(&rc, &v), RC_EV_DONE << 8 | RC_PART);
    CHECK_EQ(rc_cmd(&rc, &v, RC_TEMPO, 144), RC_OK);
    CHECK(S.call[2].what == 't' && S.call[2].a == 144);
    v.tempo = 144;
    CHECK_EQ(rc_poll(&rc, &v), RC_EV_DONE << 8 | RC_TEMPO);
    v = view();
    CHECK_EQ(rc_cmd(&rc, &v, RC_SEEK, 3), RC_OK);
    CHECK(S.call[3].what == 'k' && S.call[3].a == 3);
    v.state = RV_ST_PLAY, v.bar = 4; /* playing on: the next bar already started */
    CHECK_EQ(rc_poll(&rc, &v), RC_EV_DONE << 8 | RC_SEEK);
    CHECK_EQ(S.n, 4);
}

TEST(recctl_stop_overrides_and_timeout)
{
    rc_t rc;
    reset(&rc);
    rv_view_t v = view();
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_OK);
    CHECK_EQ(rc_cmd(&rc, &v, RC_SELECT, 1), RC_E_PENDING);
    /* stop while the play is still queued (state still 2): posted, not "already stopped" */
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_OK);
    CHECK_EQ(S.n, 2);
    CHECK(S.call[1].b == RC_MUSIC_STOP);
    /* the queue was emptied by someone else: no state change -> one TIMEOUT after the window */
    uint16_t ev = 0;
    unsigned n = 0;
    v.state = RV_ST_PLAY; /* the play ran, the stop was lost */
    while (!(ev = rc_poll(&rc, &v)) && n < RC_TIMEOUT_POLLS + 5)
        n++;
    CHECK_EQ(ev, RC_EV_TIMEOUT << 8 | RC_STOP);
    CHECK_EQ(n, RC_TIMEOUT_POLLS - 1);
    CHECK_EQ(rc.n_timeout, 1);
    CHECK_EQ(rc_poll(&rc, &v), 0);
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_OK); /* free again */
}

/* ---- R2: record transports (rc_enable_rec) ---- */
static unsigned q_music(unsigned from, uint8_t *vals, unsigned max)
{
    unsigned k = 0;
    for (unsigned i = from; i < S.n && i < 16; i++)
        if (S.call[i].what == 'q' && S.call[i].a == RC_Q_MUSIC && k < max)
            vals[k++] = (uint8_t)S.call[i].b;
    return k;
}

static void reset_rec(rc_t *rc)
{
    reset(rc);
    rc_enable_rec(rc, 1);
}

/* song 3 (empty) selected: the place to record */
static rv_view_t view_empty(void)
{
    rv_view_t v = view();
    v.song = 2, v.max_bars = 0;
    return v;
}

TEST(recctl_r2_off_is_r1)
{
    rc_t rc;
    reset(&rc);   /* rec not enabled: the record commands do not exist, states 3-6 refuse everything */
    rv_view_t v = view_empty();
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, RC_REC_KEEP), RC_E_ARG);
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC_START, 0), RC_E_ARG);
    v.state = RV_ST_RECORDING;
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_E_REC);
    CHECK_EQ(rc_cmd(&rc, &v, RC_PAUSE, 0), RC_E_REC);
    CHECK_EQ(S.n, 0);
}

TEST(recctl_r2_arm_start_finish)
{
    rc_t rc;
    reset_rec(&rc);
    rv_view_t v = view_empty();
    uint8_t q[4];
    /* arm part 1 of the empty song: one Music 3 */
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, RC_REC_KEEP), RC_OK);
    CHECK_EQ(q_music(0, q, 4), 1);
    CHECK_EQ(q[0], RC_MUSIC_ARM);
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_E_PENDING);   /* a record transport is never replaced */
    v.state = RV_ST_REC_STANDBY;
    CHECK_EQ(rc_poll(&rc, &v), RC_EV_DONE << 8 | RC_REC);
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, RC_REC_KEEP), RC_NOP);
    /* armed: playback commands wait for the take */
    CHECK_EQ(rc_cmd(&rc, &v, RC_PLAY, 0), RC_E_REC);
    CHECK_EQ(rc_cmd(&rc, &v, RC_SELECT, 1), RC_E_REC);
    CHECK_EQ(rc_cmd(&rc, &v, RC_PAUSE, 0), RC_NOP);
    /* start: Music 4 */
    unsigned n0 = S.n;
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC_START, 0), RC_OK);
    CHECK_EQ(q_music(n0, q, 4), 1);
    CHECK_EQ(q[0], RC_MUSIC_START);
    v.state = RV_ST_RECORDING;
    CHECK_EQ(rc_poll(&rc, &v), RC_EV_DONE << 8 | RC_REC_START);
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC_START, 0), RC_NOP);
    /* while recording: arm / count-in would restart the take - refused */
    CHECK_EQ(rc_cmd(&rc, &v, RC_COUNT_IN, RC_REC_KEEP), RC_E_REC);
    CHECK_EQ(rc_cmd(&rc, &v, RC_SEEK, 1), RC_E_REC);
    /* finish: one Music 2 (-> ring FC), a second stop / pause is NOP (no second commit) */
    n0 = S.n;
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_OK);
    CHECK_EQ(q_music(n0, q, 4), 1);
    CHECK_EQ(q[0], RC_MUSIC_STOP);
    CHECK_EQ(rc.fc_out, 1);
    CHECK_EQ(rc.n_finish, 1);
    n0 = S.n;
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_NOP);
    CHECK_EQ(rc_poll(&rc, &v), 0);
    CHECK_EQ(S.n, n0);
    v.state = RV_ST_STOP;
    CHECK_EQ(rc_poll(&rc, &v), RC_EV_DONE << 8 | RC_STOP);
    CHECK_EQ(rc.fc_out, 0);
}

TEST(recctl_r2_second_stop_while_fc_out)
{
    rc_t rc;
    reset_rec(&rc);
    rv_view_t v = view_empty();
    v.state = RV_ST_RECORDING, v.rec_part = 0;
    CHECK_EQ(rc_cmd(&rc, &v, RC_PAUSE, 0), RC_OK);   /* stock Music 0 in 4 also finishes (ring FC) */
    CHECK(S.call[0].what == 'q' && S.call[0].b == RC_MUSIC_PAUSE);
    /* the pending pause is cleared by a time out with the queue still holding entries: fc_out stays */
    v.mq_count = 3;
    for (unsigned i = 0; i < RC_TIMEOUT_POLLS; i++)
        rc_poll(&rc, &v);
    CHECK_EQ(rc.pend, 0);
    CHECK_EQ(rc.fc_out, 1);
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_NOP);   /* the FC may still come: no second one */
    CHECK_EQ(S.n, 1);
    /* time out again with the queue drained and still state 4: the FC was dropped -> a new stop may post */
    rc.fc_out = 1, rc.pend = RC_STOP, rc.left = 1, v.mq_count = 0;
    CHECK_EQ(rc_poll(&rc, &v), RC_EV_TIMEOUT << 8 | RC_STOP);
    CHECK_EQ(rc.fc_out, 0);
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_OK);
    CHECK_EQ(S.n, 2);
}

TEST(recctl_r2_finish_deferred)
{
    rc_t rc;
    reset_rec(&rc);
    rv_view_t v = view_empty();
    v.state = RV_ST_RECORDING;
    v.busy = RV_BUSY_JOB;   /* a NOR job: FUN_40168408 has no test of its own - the finish waits */
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_OK);
    CHECK_EQ(S.n, 0);
    CHECK_EQ(rc.defer, 1);
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_E_PENDING);
    for (int i = 0; i < 100; i++)
        CHECK_EQ(rc_poll(&rc, &v), 0);
    CHECK_EQ(S.n, 0);
    v.busy = RV_BUSY_WORKER;
    rc_poll(&rc, &v);
    v.busy = RV_BUSY_FILEOP;
    rc_poll(&rc, &v);
    v.busy = 0, v.ring = RC_RING_HIGH;
    rc_poll(&rc, &v);
    CHECK_EQ(S.n, 0);
    v.ring = 10;
    CHECK_EQ(rc_poll(&rc, &v), 0);   /* now posted */
    CHECK_EQ(S.n, 1);
    CHECK(S.call[0].b == RC_MUSIC_STOP);
    CHECK_EQ(rc.defer, 0);
    CHECK_EQ(rc.fc_out, 1);
    v.state = RV_ST_STOP;
    CHECK_EQ(rc_poll(&rc, &v), RC_EV_DONE << 8 | RC_STOP);
    /* a deferred finish that never gets free times out after RC_DEFER_POLLS without posting */
    reset_rec(&rc);
    v.state = RV_ST_RECORDING, v.busy = RV_BUSY_JOB;
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_OK);
    unsigned n = 0;
    uint16_t ev = 0;
    while (!(ev = rc_poll(&rc, &v)) && n < RC_DEFER_POLLS + 5)
        n++;
    CHECK_EQ(ev, RC_EV_TIMEOUT << 8 | RC_STOP);
    CHECK_EQ(n, RC_DEFER_POLLS - 1);
    CHECK_EQ(S.n, 0);
    CHECK_EQ(rc.defer, 0);
}

TEST(recctl_r2_leave_standby)
{
    rc_t rc;
    uint8_t q[4];
    reset_rec(&rc);
    rv_view_t v = view_empty();
    /* 3: one Music 2 (stock stores 2) */
    v.state = RV_ST_REC_STANDBY;
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_OK);
    CHECK_EQ(q_music(0, q, 4), 1);
    CHECK_EQ(q[0], RC_MUSIC_STOP);
    CHECK_EQ(rc.n_finish, 0);
    v.state = RV_ST_STOP;
    rc_poll(&rc, &v);
    /* 5: stock Music 2 does nothing there - Music 3 then Music 2 */
    v.state = RV_ST_REC_STANDBY_NOMETRO;
    unsigned n0 = S.n;
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_OK);
    CHECK_EQ(q_music(n0, q, 4), 2);
    CHECK(q[0] == RC_MUSIC_ARM && q[1] == RC_MUSIC_STOP);
    v.state = RV_ST_STOP;
    rc_poll(&rc, &v);
    /* 6: no stock command leaves a count-in: refused; start is allowed (FA) */
    v.state = RV_ST_COUNT_IN;
    CHECK_EQ(rc_cmd(&rc, &v, RC_STOP, 0), RC_E_WRONG);
    CHECK_EQ(rc_cmd(&rc, &v, RC_PAUSE, 0), RC_NOP);
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, RC_REC_KEEP), RC_E_WRONG);
    n0 = S.n;
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC_START, 0), RC_OK);
    CHECK_EQ(q_music(n0, q, 4), 1);
    CHECK_EQ(q[0], RC_MUSIC_START);
}

TEST(recctl_r2_arm_rules)
{
    rc_t rc;
    uint8_t q[4];
    reset_rec(&rc);
    rv_view_t v = view();   /* song 1: part 1 recorded */
    /* part 1 holds a recording: only with RC_REC_OVERWRITE */
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, 0), RC_E_EXISTS);
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, RC_REC_KEEP), RC_E_EXISTS);   /* rec_part 0 = part 1 */
    CHECK_EQ(S.n, 0);
    /* part 2 is free: IntSongPart 1 first (task 19), then Music 3 */
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, 1), RC_OK);
    CHECK(S.call[0].what == 'p' && S.call[0].a == 1);
    CHECK(S.call[1].what == 'q' && S.call[1].b == RC_MUSIC_ARM);
    v.state = RV_ST_REC_STANDBY, v.rec_part = 1;
    rc_poll(&rc, &v);
    /* armed for part 2: switching to no-metronome keeps the part; another part is refused */
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC_NOMETRO, 0), RC_E_REC);
    unsigned n0 = S.n;
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC_NOMETRO, RC_REC_KEEP), RC_OK);
    CHECK_EQ(q_music(n0, q, 4), 1);
    CHECK_EQ(q[0], RC_MUSIC_ARM_NOMETRO);
    v.state = RV_ST_REC_STANDBY_NOMETRO;
    rc_poll(&rc, &v);
    /* overwrite part 1 from stop */
    reset_rec(&rc);
    v = view();
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, 0 | RC_REC_OVERWRITE), RC_OK);
    CHECK_EQ(S.n, 1);   /* part already 0: no IntSongPart */
    /* from play: 3 is one Music 3 (stock stops first), 5 / 6 get a Music 2 first (stock 5 / 6 in 1 only stop) */
    reset_rec(&rc);
    v = view_empty();
    v.state = RV_ST_PLAY;
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, RC_REC_KEEP), RC_OK);
    CHECK_EQ(q_music(0, q, 4), 1);
    reset_rec(&rc);
    CHECK_EQ(rc_cmd(&rc, &v, RC_COUNT_IN, RC_REC_KEEP), RC_OK);
    CHECK_EQ(q_music(0, q, 4), 2);
    CHECK(q[0] == RC_MUSIC_STOP && q[1] == RC_MUSIC_COUNT_IN);
    /* mode 0: MusicMode 2 first */
    reset_rec(&rc);
    v = view_empty();
    v.music_mode = 0;
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC_NOMETRO, RC_REC_KEEP), RC_OK);
    CHECK(S.call[0].what == 'q' && S.call[0].a == RC_Q_MODE && S.call[0].b == 2);
    CHECK(S.call[1].a == RC_Q_MUSIC && S.call[1].b == RC_MUSIC_ARM_NOMETRO);
    /* busy / args / start before arming */
    reset_rec(&rc);
    v = view_empty();
    v.busy = RV_BUSY_FILEOP;
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, RC_REC_KEEP), RC_E_BUSY);
    v.busy = 0, v.store = RV_STORE_MARKER;
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, RC_REC_KEEP), RC_E_BUSY);
    v.store = RV_STORE_OK;
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, 2), RC_E_ARG);
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, 0x200), RC_E_ARG);
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC_START, 0), RC_E_WRONG);   /* stock 4 from 2 records without arming */
    v.mq_hold = 0x82;
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC, RC_REC_KEEP), RC_E_BUSY);
    v.mq_hold = 0, v.state = RV_ST_REC_STANDBY, v.ring = RC_RING_HIGH;
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC_START, 0), RC_E_BUSY);   /* FA could be dropped */
    v.ring = 0, v.music_mode = 3;
    CHECK_EQ(rc_cmd(&rc, &v, RC_REC_START, 0), RC_E_MODE);
    CHECK_EQ(S.n, 0);
}

void suite_recctl(void)
{
    printf("recctl (Phase 3-6 R1 + R2)\n");
    RUN(recctl_transport_via_queue);
    RUN(recctl_enters_internal_mode);
    RUN(recctl_refuses_busy_and_recording);
    RUN(recctl_queue_guard);
    RUN(recctl_no_data);
    RUN(recctl_args);
    RUN(recctl_direct_calls);
    RUN(recctl_stop_overrides_and_timeout);
    RUN(recctl_r2_off_is_r1);
    RUN(recctl_r2_arm_start_finish);
    RUN(recctl_r2_second_stop_while_fc_out);
    RUN(recctl_r2_finish_deferred);
    RUN(recctl_r2_leave_standby);
    RUN(recctl_r2_arm_rules);
}
