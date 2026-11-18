/* OpenGrand — playback control (Phase 3-6 R1) and record control (R2, rc_enable_rec) of the stock song player.
 * Stock paths and their evidence: include/ca79ng/recctl.h.
 * This file only decides; the port's rc_ops_t
 * reaches the stock firmware.  No stock RAM written here: a recording is made, serialised and saved by the stock
 * (ring FA / FC -> FUN_40168774 -> FUN_40168408), the core only decides when the transports may go. */
#include "ca79ng/recctl.h"

void rc_init(rc_t *rc, const rc_ops_t *ops)
{
    /* field by field: the ARM build has no memset */
    rc->ops = ops;
    rc->pend = 0, rc->want = 0, rc->left = 0, rc->last_rc = 0;
    rc->n_ok = 0, rc->n_refused = 0, rc->n_timeout = 0;
    rc->rec = 0, rc->fc_out = 0, rc->defer = 0, rc->n_finish = 0;
}

void rc_enable_rec(rc_t *rc, uint8_t on) { rc->rec = on ? 1 : 0; }

static bool is_rec_cmd(uint8_t cmd) { return cmd >= RC_REC && cmd <= RC_REC_START; }

uint8_t rc_play_sel_of(uint8_t part) { return part == 0x7F ? 0 : (uint8_t)(part + 1); }

/* the song-table guard: a file operation (KSO import / USB load share 0x40A00000 and move the
 * table), a NOR job or the worker (a commit just happened), or a table that fails the boot
 * check rules - nothing that loads a song runs then */
static bool table_busy(const rv_view_t *v)
{
    return (v->busy & (RV_BUSY_JOB | RV_BUSY_WORKER | RV_BUSY_FILEOP)) || v->store != RV_STORE_OK;
}

/* the recorder states R1 acts in: 0 paused / just stopped, 1 playing, 2 stopped (song loaded) */
static uint8_t state_gate(const rv_view_t *v)
{
    switch (v->state) {
    case RV_ST_STOPPED0:
    case RV_ST_PLAY:
    case RV_ST_STOP: return RC_OK;
    case RV_ST_REC_STANDBY:
    case RV_ST_RECORDING:
    case RV_ST_REC_STANDBY_NOMETRO:
    case RV_ST_COUNT_IN: return RC_E_REC;
    default: return RC_E_STATE;
    }
}

/* parts of the selected song the play selector 0x4191EF72 plays (0 both, 1 part 1, 2 part 2) */
static bool has_data(const rv_view_t *v)
{
    if (v->song >= RV_SONGS)
        return false;
    uint8_t f = v->songs[v->song].flags;
    uint8_t want = v->play_sel == 1 ? RV_SONG_PART1 : v->play_sel == 2 ? RV_SONG_PART2
                                                                       : (RV_SONG_PART1 | RV_SONG_PART2);
    return (f & want) != 0;
}

static uint8_t pending(rc_t *rc, uint8_t cmd, uint16_t want)
{
    rc->pend = cmd, rc->want = want, rc->left = RC_TIMEOUT_POLLS;
    rc->n_ok++;
    return RC_OK;
}

static uint8_t refuse(rc_t *rc, uint8_t e)
{
    rc->n_refused++;
    return e;
}

/* Music values (1 or 2, in order) through the stock queue, entering MusicMode 2 first when no mode is active */
static uint8_t post_music2(rc_t *rc, const rv_view_t *v, uint8_t first, int second)
{
    unsigned n = (v->music_mode == RC_MODE_INTERNAL ? 1u : 2u) + (second >= 0 ? 1u : 0u);
    if (v->mq_hold || v->mq_count + n > RC_Q_MAX)
        return refuse(rc, RC_E_BUSY);
    if (v->music_mode != RC_MODE_INTERNAL)
        rc->ops->post(rc->ops->ctx, RC_Q_MODE, RC_MODE_INTERNAL);
    rc->ops->post(rc->ops->ctx, RC_Q_MUSIC, first);
    if (second >= 0)
        rc->ops->post(rc->ops->ctx, RC_Q_MUSIC, (uint8_t)second);
    return RC_OK;
}

static uint8_t post_music(rc_t *rc, const rv_view_t *v, uint8_t value) { return post_music2(rc, v, value, -1); }

/* ---- R2: record transports ------------------------------------------------------------------------------ */
/* what keeps a finish (ring FC -> commit -> FUN_40168408 -> NOR job) from being posted now: a NOR job queued or the
 * worker programming (P2 - FUN_40168408 has no test of its own), a file operation on 0x40A00000 (P3),
 * the ring so full that FC could be dropped, the deferred-MusicMode flag (FUN_4015DBD4 drops the Music command) */
static bool finish_blocked(const rv_view_t *v)
{
    return (v->busy & (RV_BUSY_JOB | RV_BUSY_WORKER | RV_BUSY_FILEOP)) || v->ring >= RC_RING_HIGH || v->mq_hold ||
           v->mq_count + 1u > RC_Q_MAX;
}

/* state 4: Music 2 (0x4016A154) and Music 0 (0x4016A1B4) both branch to 0x4016A1CC = FUN_40169FE8 (ring FC); the value
 * follows the command (stop 2, pause 0) so the queue bytes are the stock's own */
static void post_finish(rc_t *rc)
{
    rc->ops->post(rc->ops->ctx, RC_Q_MUSIC, rc->pend == RC_PAUSE ? RC_MUSIC_PAUSE : RC_MUSIC_STOP);
    rc->fc_out = 1, rc->defer = 0, rc->n_finish++;
    rc->left = RC_TIMEOUT_POLLS;
}

/* stop / pause in a recording state (3-6) */
static uint8_t rec_stop(rc_t *rc, const rv_view_t *v, uint8_t cmd)
{
    switch (v->state) {
    case RV_ST_RECORDING:   /* finish = the stock commit, once per take */
        if (rc->fc_out || rc->defer)
            return RC_NOP;
        pending(rc, cmd, RV_ST_STOP);
        if (finish_blocked(v)) {
            rc->defer = 1, rc->left = RC_DEFER_POLLS;   /* rc_poll posts it when nothing blocks it */
            return RC_OK;
        }
        post_finish(rc);
        return RC_OK;
    case RV_ST_REC_STANDBY:   /* stock: Music 2 in 3 = state 2, nothing else (0x4016A170) */
        if (cmd == RC_PAUSE)
            return RC_NOP;
        if (post_music(rc, v, RC_MUSIC_STOP) != RC_OK)
            return RC_E_BUSY;
        return pending(rc, cmd, RV_ST_STOP);
    case RV_ST_REC_STANDBY_NOMETRO:   /* stock Music 2 does nothing in 5: arm 3 (FUN_401693CC) then stop */
        if (cmd == RC_PAUSE)
            return RC_NOP;
        if (post_music2(rc, v, RC_MUSIC_ARM, RC_MUSIC_STOP) != RC_OK)
            return RC_E_BUSY;
        return pending(rc, cmd, RV_ST_STOP);
    default:   /* 6 count-in: no stock command leaves it except a start (FA) or a play */
        if (cmd == RC_PAUSE)
            return RC_NOP;
        return refuse(rc, RC_E_WRONG);
    }
}

static uint8_t arm_state(uint8_t cmd)
{
    return cmd == RC_REC ? RV_ST_REC_STANDBY : cmd == RC_REC_NOMETRO ? RV_ST_REC_STANDBY_NOMETRO : RV_ST_COUNT_IN;
}

/* RC_REC / RC_REC_NOMETRO / RC_COUNT_IN */
static uint8_t rec_arm(rc_t *rc, const rv_view_t *v, uint8_t cmd, uint16_t arg)
{
    uint8_t want = arm_state(cmd), value, part = (uint8_t)(arg & 0x7F), g;
    bool armed = v->state == RV_ST_REC_STANDBY || v->state == RV_ST_REC_STANDBY_NOMETRO;
    bool stop_first = v->state == RV_ST_PLAY && cmd != RC_REC;   /* stock 5 / 6 in state 1 only stop */
    unsigned n;
    if ((part != RC_REC_KEEP && part > 1) || (arg & ~(uint16_t)(0x7F | RC_REC_OVERWRITE)))
        return refuse(rc, RC_E_ARG);
    if (v->state == want)
        return RC_NOP;
    if (v->state == RV_ST_RECORDING)   /* stock 6 would restart the take (FUN_40169EDC has no state test) */
        return refuse(rc, RC_E_REC);
    if (v->state == RV_ST_COUNT_IN)    /* stock 3 / 5 refuse 6 (0xF0) */
        return refuse(rc, RC_E_WRONG);
    if (part == RC_REC_KEEP)
        part = v->rec_part;
    if (armed && part != v->rec_part)  /* the take was prepared for the armed part (FUN_40169364) */
        return refuse(rc, RC_E_REC);
    if (table_busy(v))                 /* arming loads the song, the take ends in a commit */
        return refuse(rc, RC_E_BUSY);
    if (part > 1 || v->song >= RV_SONGS)
        return refuse(rc, RC_E_ARG);
    if (!(arg & RC_REC_OVERWRITE) && (v->songs[v->song].flags & (part ? RV_SONG_PART2 : RV_SONG_PART1)))
        return refuse(rc, RC_E_EXISTS);
    value = cmd == RC_REC ? RC_MUSIC_ARM : cmd == RC_REC_NOMETRO ? RC_MUSIC_ARM_NOMETRO : RC_MUSIC_COUNT_IN;
    n = (v->music_mode == RC_MODE_INTERNAL ? 1u : 2u) + (stop_first ? 1u : 0u);
    if (v->mq_hold || v->mq_count + n > RC_Q_MAX)
        return refuse(rc, RC_E_BUSY);
    if (part != v->rec_part)   /* IntSongPart (FUN_4016C9B8 -> FUN_4016A1E4 sets 0x4191EF73), task 19 as the apply */
        rc->last_rc = (uint8_t)rc->ops->part(rc->ops->ctx, part);
    if (stop_first)
        g = post_music2(rc, v, RC_MUSIC_STOP, value);
    else
        g = post_music(rc, v, value);   /* stock 3 from 1 stops first itself (0x4016A124) */
    if (g != RC_OK)
        return g;
    return pending(rc, cmd, want);
}

/* RC_REC_START: ring FA -> FUN_401692D0 -> 4, only when armed */
static uint8_t rec_start(rc_t *rc, const rv_view_t *v)
{
    uint8_t g;
    if (v->state == RV_ST_RECORDING)
        return RC_NOP;
    if (v->state != RV_ST_REC_STANDBY && v->state != RV_ST_REC_STANDBY_NOMETRO && v->state != RV_ST_COUNT_IN)
        return refuse(rc, RC_E_WRONG);   /* stock 4 from 0 / 2 records without arming */
    if (table_busy(v) || v->ring >= RC_RING_HIGH)
        return refuse(rc, RC_E_BUSY);
    if ((g = post_music(rc, v, RC_MUSIC_START)) != RC_OK)
        return g;
    return pending(rc, RC_REC_START, RV_ST_RECORDING);
}

uint8_t rc_cmd(rc_t *rc, const rv_view_t *v, uint8_t cmd, uint16_t arg)
{
    /* stop and pause may replace a playback command still on its way; everything else waits for it.  A record
     * transport is never replaced: a stop queued behind a start would run first in state 3 and the FA after it
     * would still start the take (FUN_4016CCDC does not look at the state) */
    if (rc->pend && ((cmd != RC_STOP && cmd != RC_PAUSE) || is_rec_cmd(rc->pend) || rc->defer))
        return refuse(rc, RC_E_PENDING);
    if (is_rec_cmd(cmd) && !rc->rec)
        return refuse(rc, RC_E_ARG);
    uint8_t g = state_gate(v);
    if (g == RC_E_REC && rc->rec) {
        if (v->music_mode != RC_MODE_INTERNAL)
            return refuse(rc, RC_E_MODE);
        switch (cmd) {
        case RC_STOP:
        case RC_PAUSE: return rec_stop(rc, v, cmd);
        case RC_REC:
        case RC_REC_NOMETRO:
        case RC_COUNT_IN: return rec_arm(rc, v, cmd, arg);
        case RC_REC_START: return rec_start(rc, v);
        default: return refuse(rc, RC_E_REC);   /* play / select / part / tempo / seek wait for the take */
        }
    }
    if (g != RC_OK)
        return refuse(rc, g);
    if (v->music_mode != RC_MODE_INTERNAL && v->music_mode != 0)
        return refuse(rc, RC_E_MODE);

    switch (cmd) {
    case RC_PLAY:
        if (v->state == RV_ST_PLAY)
            return RC_NOP;
        if (table_busy(v))
            return refuse(rc, RC_E_BUSY);
        if (!has_data(v))
            return refuse(rc, RC_E_NODATA);
        if ((g = post_music(rc, v, RC_MUSIC_PLAY)) != RC_OK)
            return g;
        return pending(rc, cmd, RV_ST_PLAY);

    case RC_STOP:
        if (v->state == RV_ST_STOP && rc->pend != RC_PLAY)
            return RC_NOP;
        /* stop from 0 / 1 reloads the song (FUN_401694B0 -> FUN_401674FC reads the table) */
        if (v->busy & RV_BUSY_FILEOP)
            return refuse(rc, RC_E_BUSY);
        if ((g = post_music(rc, v, RC_MUSIC_STOP)) != RC_OK)
            return g;
        return pending(rc, cmd, RV_ST_STOP);

    case RC_PAUSE:
        if (v->state != RV_ST_PLAY && rc->pend != RC_PLAY)
            return RC_NOP;
        if ((g = post_music(rc, v, RC_MUSIC_PAUSE)) != RC_OK)
            return g;
        return pending(rc, cmd, RV_ST_STOPPED0);

    case RC_SELECT:
        if (arg >= RV_SONGS) /* the stock takes >= 10 as song 1 */
            return refuse(rc, RC_E_ARG);
        if (table_busy(v))
            return refuse(rc, RC_E_BUSY);
        rc->last_rc = (uint8_t)rc->ops->select(rc->ops->ctx, (uint8_t)arg);
        return pending(rc, cmd, arg);

    case RC_PART:
        if (arg != 0 && arg != 1 && arg != 0x7F)
            return refuse(rc, RC_E_ARG);
        if (table_busy(v))
            return refuse(rc, RC_E_BUSY);
        rc->last_rc = (uint8_t)rc->ops->part(rc->ops->ctx, (uint8_t)arg);
        return pending(rc, cmd, rc_play_sel_of((uint8_t)arg));

    case RC_TEMPO:
        if (arg < RC_TEMPO_MIN || arg > RC_TEMPO_MAX)
            return refuse(rc, RC_E_ARG);
        if (!v->tempo_orig) /* the ratio divides by it (FUN_40167294 -> FUN_40088C14) */
            return refuse(rc, RC_E_NODATA);
        if (v->tempo == arg)
            return RC_NOP;
        rc->ops->tempo(rc->ops->ctx, arg);
        return pending(rc, cmd, arg);

    case RC_SEEK:
        if (!v->max_bars)
            return refuse(rc, RC_E_NODATA);
        if (arg < 1 || arg > v->max_bars)
            return refuse(rc, RC_E_ARG);
        if (table_busy(v))
            return refuse(rc, RC_E_BUSY);
        rc->ops->seek(rc->ops->ctx, arg);
        return pending(rc, cmd, arg);

    case RC_REC:
    case RC_REC_NOMETRO:
    case RC_COUNT_IN: return rec_arm(rc, v, cmd, arg);
    case RC_REC_START: return rec_start(rc, v);

    default: return refuse(rc, RC_E_ARG);
    }
}

static bool reached(const rc_t *rc, const rv_view_t *v)
{
    switch (rc->pend) {
    case RC_PLAY:
    case RC_STOP:
    case RC_PAUSE:
    case RC_REC:
    case RC_REC_NOMETRO:
    case RC_COUNT_IN:
    case RC_REC_START: return v->state == rc->want;
    case RC_SELECT: return v->song == rc->want;
    case RC_PART: return v->play_sel == rc->want;
    case RC_TEMPO: return v->tempo == rc->want;
    /* playing on from the sought bar is fine: the next bar may already be under way */
    case RC_SEEK: return v->bar == rc->want || (v->state == RV_ST_PLAY && v->bar == rc->want + 1);
    default: return false;
    }
}

uint16_t rc_poll(rc_t *rc, const rv_view_t *v)
{
    if (rc->fc_out && v->state != RV_ST_RECORDING)
        rc->fc_out = 0;   /* the take was committed (or ended otherwise): the next take may finish */
    if (!rc->pend)
        return 0;
    uint8_t cmd = rc->pend;
    if (reached(rc, v)) {
        rc->pend = 0, rc->defer = 0;
        return (uint16_t)(RC_EV_DONE << 8 | cmd);
    }
    if (rc->defer && v->state == RV_ST_RECORDING && !finish_blocked(v)) {
        post_finish(rc);
        return 0;
    }
    if (--rc->left == 0) {
        /* still recording although the FC was posted and the queue has drained: the FC was dropped (ring full) -
         * a new stop may try again.  While the queue still holds entries it may yet come: keep fc_out */
        if (rc->fc_out && v->state == RV_ST_RECORDING && v->mq_count == 0)
            rc->fc_out = 0;
        rc->pend = 0, rc->defer = 0;
        rc->n_timeout++;
        return (uint16_t)(RC_EV_TIMEOUT << 8 | cmd);
    }
    return 0;
}
