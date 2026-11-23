/* OpenGrand — save decision for the stock song store (Phase 3-6 R3).  Stock paths and evidence: include/ca79ng/recsave.h.
 * Decides and checks only: no stock RAM written, no stock routine
 * called - the port lets the stock's own path run (or refuses it with the stock's Busy bytes). */
#include "ca79ng/recsave.h"

void rs_init(rs_t *rs)
{
    /* field by field: the ARM build has no memset */
    rs->on = 0;
    rs->phase = RS_P_IDLE, rs->kind = RS_K_NONE, rs->target = 0, rs->jobs = 0;
    rs->prev_state = 0, rs->prev_job = 0, rs->have_base = 0, rs->fix = RS_F_NONE;
    rs->left = 0, rs->file_left = 0, rs->file_song = 0xFF;
    for (unsigned i = 0; i < RV_SONGS; i++)
        rs->base_len[i] = 0, rs->base_flags[i] = 0;
    rs->last = RS_R_NONE, rs->last_kind = RS_K_NONE, rs->last_target = 0, rs->last_jobs = 0, rs->last_changed = 0;
    rs->n_windows = 0, rs->n_ok = 0, rs->n_bad = 0, rs->n_refused = 0, rs->n_delete = 0, rs->n_file = 0;
}

void rs_enable(rs_t *rs, uint8_t on) { rs->on = on ? 1 : 0; }

uint8_t rs_busy(const rs_t *rs) { return rs->on && rs->phase != RS_P_IDLE; }

static uint8_t jobs_now(const rv_view_t *v) { return (v->busy & (RV_BUSY_JOB | RV_BUSY_WORKER)) != 0; }
static uint8_t file_now(const rv_view_t *v) { return (v->busy & RV_BUSY_FILEOP) != 0; }
static uint8_t at_rest(const rv_view_t *v) { return v->state == RV_ST_STOPPED0 || v->state == RV_ST_STOP; }

static void inc(uint16_t *c)
{
    if (*c != 0xFFFFu)
        (*c)++;
}

static void open_window(rs_t *rs, const rv_view_t *v, uint8_t kind, uint8_t target)
{
    uint8_t job = jobs_now(v);
    rs->kind = kind, rs->target = target, rs->fix = RS_F_NONE;
    rs->jobs = job;   /* a commit queues its job before the state leaves 4: already up when we see it */
    rs->phase = job ? RS_P_JOB : RS_P_WAIT;
    rs->left = (job || kind == RS_K_FILE) ? RS_MAX_POLLS : RS_WAIT_POLLS;
    inc(&rs->n_windows);
}

static void take_base(rs_t *rs, const rv_view_t *v)
{
    for (unsigned i = 0; i < RV_SONGS; i++)
        rs->base_len[i] = v->songs[i].len, rs->base_flags[i] = v->songs[i].flags;
    rs->have_base = 1;
}

static uint16_t changed_songs(const rs_t *rs, const rv_view_t *v)
{
    uint16_t c = 0;
    if (!rs->have_base)
        return 0;
    for (unsigned i = 0; i < RV_SONGS; i++)
        if (v->songs[i].len != rs->base_len[i] || v->songs[i].flags != rs->base_flags[i])
            c |= (uint16_t)(1u << i);
    return c;
}

static uint8_t bits(uint16_t c)
{
    uint8_t n = 0;
    for (; c; c &= (uint16_t)(c - 1))
        n++;
    return n;
}

static uint8_t verdict(const rs_t *rs, const rv_view_t *v, uint16_t changed)
{
    if (v->store != RV_STORE_OK)
        return RS_R_TABLE;
    if (rs->fix == RS_F_DONE)
        return RS_R_FIXED;   /* the commit's job + the mend's (one when the mend reached the still pending job) */
    if (!rs->jobs)
        return (rs->kind == RS_K_FILE && !changed) ? RS_R_NOWRITE : RS_R_NOJOB;
    if (rs->jobs > 1)
        return RS_R_DOUBLE;
    if (rs->target == RS_T_ALL)
        return RS_R_OK;
    if (rs->target == RS_T_ANY)
        return bits(changed) <= 1 ? RS_R_OK : RS_R_OTHER;
    return (changed & (uint16_t)~(1u << rs->target)) ? RS_R_OTHER : RS_R_OK;
}

static void close_window(rs_t *rs, const rv_view_t *v, uint8_t result)
{
    uint16_t changed = changed_songs(rs, v);
    if (result == RS_R_NONE)
        result = verdict(rs, v, changed);
    rs->last = result, rs->last_kind = rs->kind, rs->last_target = rs->target, rs->last_jobs = rs->jobs;
    rs->last_changed = changed;
    if (result == RS_R_OK || result == RS_R_NOWRITE || result == RS_R_FIXED)
        inc(&rs->n_ok);
    else
        inc(&rs->n_bad);
    if (v->store == RV_STORE_OK && result != RS_R_STUCK)
        take_base(rs, v);
    rs->phase = RS_P_IDLE, rs->kind = RS_K_NONE, rs->jobs = 0, rs->left = 0;
}

/* the commit wrote its own song broken and nothing else is in the way of mending it (head comment of recsave.h) */
static uint8_t fixable(const rs_t *rs, const rv_view_t *v)
{
    return rs->kind == RS_K_COMMIT && rs->fix == RS_F_NONE && rs->target < RV_SONGS && rs->have_base &&
           (v->store == RV_STORE_TRACK_FA || v->store == RV_STORE_TRACK_RANGE) && (v->store_bad >> 4) == rs->target &&
           at_rest(v);
}

uint8_t rs_wants_table(const rs_t *rs, const rv_view_t *v)
{
    if (!rs->on || rs->phase == RS_P_IDLE || file_now(v) || v->state == RV_ST_RECORDING)
        return 0;
    /* a commit window looks at its table while the job is still pending too: the mend is best done before the worker
     * starts.  Until the first look (rs_fixed / the window closing) only. */
    return !jobs_now(v) || (rs->kind == RS_K_COMMIT && rs->fix == RS_F_NONE);
}

uint8_t rs_fix_wanted(const rs_t *rs, const rv_view_t *v)
{
    if (!rs->on || (rs->phase != RS_P_JOB && rs->phase != RS_P_FIX) || !fixable(rs, v) ||
        (v->busy & (RV_BUSY_WORKER | RV_BUSY_FILEOP)))
        return 0xFF;
    return rs->target;
}

uint8_t rs_fix_table(const rs_t *rs, const uint32_t off[RV_SONGS + 2])
{
    if (!rs->have_base || rs->target >= RV_SONGS || off[RV_SONGS + 1] != RV_STORE_LIMIT || off[RV_SONGS] > RV_STORE_LIMIT)
        return 0;
    for (unsigned i = 0; i < RV_SONGS; i++) {
        if (off[i] < RV_STORE_DATA || off[i] >= RV_STORE_LIMIT || (off[i] & 3u) || off[i + 1] < off[i] ||
            off[i + 1] - off[i] < 0x74u || ((off[i + 1] - off[i]) & 3u))
            return 0;
        if (i != rs->target && off[i + 1] - off[i] != rs->base_len[i])
            return 0;   /* the commit moved more than its own song: not ours to mend */
    }
    return 1;
}

void rs_fixed(rs_t *rs, const rv_view_t *v, uint8_t ok)
{
    if (!rs->on || rs->phase == RS_P_IDLE)
        return;
    rs->fix = ok ? RS_F_DONE : RS_F_LOOKED;
    if (ok) {
        /* the stock writer queued the NOR job synchronously: wait for it (a new rising edge counts as a job) */
        rs->phase = RS_P_JOB, rs->left = RS_MAX_POLLS;
    } else if (rs->phase == RS_P_FIX) {
        close_window(rs, v, RS_R_TABLE);
    }
}

uint8_t rs_poll(rs_t *rs, const rv_view_t *v)
{
    uint8_t job, file, closed = 0;
    if (!rs->on)
        return 0;
    job = jobs_now(v), file = file_now(v);
    if (rs->file_left && --rs->file_left == 0)
        rs->file_song = 0xFF;
    if (rs->phase == RS_P_IDLE) {
        if (rs->prev_state == RV_ST_RECORDING && v->state != RV_ST_RECORDING) {
            open_window(rs, v, RS_K_COMMIT, v->song);   /* any commit: FC, record limit, select, CONNECT */
        } else if (file) {
            open_window(rs, v, RS_K_FILE, rs->file_song < RV_SONGS ? rs->file_song : RS_T_ANY);
            rs->file_song = 0xFF, rs->file_left = 0;
        } else if (v->store == RV_STORE_OK && !job && v->state != RV_ST_RECORDING) {
            take_base(rs, v);   /* settled: this is what the next write is compared with */
        }
    } else {
        /* a commit window read its table on this pass (rs_wants_table): stop looking once nothing is to be mended */
        if (rs->kind == RS_K_COMMIT && rs->fix == RS_F_NONE && !file && v->state != RV_ST_RECORDING &&
            (v->store == RV_STORE_OK || !fixable(rs, v)))
            rs->fix = RS_F_LOOKED;
        if (job && !rs->prev_job && rs->phase == RS_P_JOB)
            rs->jobs++;   /* a second job in the same window */
        if (rs->phase == RS_P_WAIT) {
            if (job) {
                rs->jobs++, rs->phase = RS_P_JOB, rs->left = RS_MAX_POLLS;
            } else if (rs->kind == RS_K_FILE ? !file : --rs->left == 0) {
                close_window(rs, v, rs->kind == RS_K_FILE ? RS_R_NONE : RS_R_NOJOB), closed = 1;
            } else if (rs->kind == RS_K_FILE && --rs->left == 0) {
                close_window(rs, v, RS_R_STUCK), closed = 1;
            }
        } else if (rs->phase == RS_P_FIX) {
            if (job || --rs->left == 0)   /* someone else's job came first, or the port never mended: report */
                rs->fix = RS_F_LOOKED, close_window(rs, v, RS_R_TABLE), closed = 1;
        } else if (!job && !file && v->state != RV_ST_RECORDING) {
            if (fixable(rs, v))
                rs->phase = RS_P_FIX, rs->left = RS_FIX_POLLS;   /* the port mends on this pass (rs_fix_wanted) */
            else
                close_window(rs, v, RS_R_NONE), closed = 1;
        } else if (--rs->left == 0) {
            close_window(rs, v, RS_R_STUCK), closed = 1;
        }
    }
    rs->prev_state = v->state, rs->prev_job = job;
    return closed;
}

static uint8_t refuse(rs_t *rs, uint8_t why)
{
    inc(&rs->n_refused);
    return why;
}

uint8_t rs_delete(rs_t *rs, const rv_view_t *v, uint8_t fmt, uint16_t song, uint8_t part)
{
    uint8_t all = song == RS_ALL_SONGS;
    if (!rs->on || (fmt != 0x04 && fmt != 0x7F))
        return RS_V_IGNORE;   /* 0x40160AD4: any other fmt is dropped by the stock */
    if (!all && song >= RV_SONGS)
        return refuse(rs, RS_V_ARG);   /* the stock would delete song 1 (0x40160B0C cpyge r12, r6) */
    if (!all && part != 0 && part != 1 && part != 0x7F)
        return RS_V_IGNORE;   /* FUN_4016C800 writes nothing, the stock still answers 32 51 7E */
    if (!at_rest(v) || jobs_now(v) || file_now(v) || rs_busy(rs))
        return refuse(rs, RS_V_BUSY);   /* FUN_401674FC would reload the player under a take / a playing song */
    if (!all && v->store != RV_STORE_OK)
        return refuse(rs, RS_V_BUSY);   /* FUN_40168408 would move songs by a broken table; format is the way out */
    open_window(rs, v, all ? RS_K_FORMAT : RS_K_DELETE, all ? RS_T_ALL : (uint8_t)song);
    inc(&rs->n_delete);
    return RS_V_PASS;
}

static void dec(uint16_t *c)
{
    if (*c != 0 && *c != 0xFFFFu)
        (*c)--;
}

void rs_nowrite(rs_t *rs)
{
    if (!rs->on || rs->phase != RS_P_WAIT || (rs->kind != RS_K_DELETE && rs->kind != RS_K_FORMAT) || rs->jobs)
        return;
    rs->phase = RS_P_IDLE, rs->kind = RS_K_NONE, rs->jobs = 0, rs->left = 0;
    dec(&rs->n_windows), dec(&rs->n_delete);
}

uint8_t rs_file(rs_t *rs, const rv_view_t *v, uint8_t d0, uint8_t d1)
{
    if (!rs->on)
        return RS_V_PASS;
    if (d0 == 0x09) {   /* import into song d1: FUN_4014F1A8 -> FUN_40168408 + FUN_401674FC(song) */
        if (!at_rest(v) || jobs_now(v) || rs_busy(rs))
            return refuse(rs, RS_V_BUSY);
        rs->file_song = d1 < RV_SONGS ? d1 : 0xFF;
        rs->file_left = RS_FILE_PEND_POLLS;
        inc(&rs->n_file);
        return RS_V_PASS;
    }
    if (d0 == 0x08) {   /* USB song load: converts into 0x40A00000, the buffer a commit serialises into */
        if ((v->state >= RV_ST_REC_STANDBY && v->state <= RV_ST_COUNT_IN) || rs_busy(rs))
            return refuse(rs, RS_V_BUSY);
        inc(&rs->n_file);
    }
    return RS_V_PASS;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }

void rs_encode(const rs_t *rs, uint8_t o[RS_ENC_LEN])
{
    o[0] = 1;   /* layout */
    o[1] = rs->on, o[2] = rs->phase, o[3] = rs->kind, o[4] = rs->target, o[5] = rs->jobs;
    o[6] = rs->last, o[7] = rs->last_kind, o[8] = rs->last_target, o[9] = rs->last_jobs;
    put16(o + 10, rs->n_windows);
    put16(o + 12, rs->n_ok);
    put16(o + 14, rs->n_bad);
    put16(o + 16, rs->n_refused);
    put16(o + 18, rs->n_delete);
    put16(o + 20, rs->n_file);
    put16(o + 22, rs->last_changed);
}
