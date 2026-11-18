/* OpenGrand cave port - Phase 3-6 R1 playback control, R2 record control (feat3 0x01), R3 save decision (feat3 0x04).  See recctl_cave.h. */
#include "recctl_cave.h"

#include <stdbool.h>
#include <string.h>

#include "ca79ng/recctl.h"
#include "ca79ng/recdel.h"
#include "ca79ng/recsave.h"
#include "ca79ng/recview.h"
#include "hal_cave.h"
#include "rec_stock.h"

#define GUARD 0x52C1C7A5u

/* R2 and R3: the record transports and the
 * save decision are the core's in every cave build (ota_main; field 2026-11-15, folded -
 * the build flags RCC_REC2 / RCC_REC3 are gone).  The host tests keep them as runtime switches
 * (rcc_host_rec2 / rcc_host_rec3, off until a test turns them on) to run the R1-only and R2-only cases. */
#ifdef RCC_HOST
static uint8_t rec2 = 0;
void rcc_host_rec2(int on) { rec2 = on ? 1 : 0; }
static uint8_t rec3 = 0;
void rcc_host_rec3(int on) { rec3 = on ? 1 : 0; }
#else
#define rec2 1
#define rec3 1
#endif

/* ---- stock memory: plain loads (the host test binds its own reader) --------------------------------------- */
#ifdef RCC_HOST
void rcc_host_read(uint32_t addr, void *dst, uint32_t n);
static void mem_read(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
    (void)ctx;
    rcc_host_read(addr, dst, n);
}
#else
static void mem_read(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
    const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)addr;
    uint8_t *d = dst;
    (void)ctx;
    for (uint32_t i = 0; i < n; i++)
        d[i] = s[i];
}
#endif
static const rv_mem_t MEM = {mem_read, 0};

/* stock memory the song delete builds from / into (the song table, the ROM template, the serialisation buffer) */
#ifdef RCC_HOST
uint8_t *rcc_host_ptr(uint32_t addr);
#define PTR(a) rcc_host_ptr(a)
#else
#define PTR(a) ((uint8_t *)(uintptr_t)(a))
#endif

/* ---- rc_ops_t: the stock apply's own sequence per parameter (rec_stock.h) ------------------- */
static void op_post(void *c, uint8_t kind, uint8_t value)
{
    (void)c;
    stk_rec_post(kind, value);
}

/* 0x40160F68..0x40160FB4: rc = FUN_401687E4(block, n); FUN_401694E4(rc); rc ? FUN_4014DC0C(rc) */
static uint16_t op_select(void *c, uint8_t song)
{
    uint32_t rc;
    (void)c;
    rc = stk_rec_select(stk_rec_block(0), song) & 0xFFFFu;
    stk_rec_maxtime(rc);
    if (rc)
        stk_rec_result(rc);
    return (uint16_t)rc;
}

/* 0x40160F40..0x40160F64: rc = FUN_4016C9B8(v); FUN_401694E4(rc); rc ? FUN_4014DC0C(rc) */
static uint16_t op_part(void *c, uint8_t v)
{
    uint32_t rc;
    (void)c;
    rc = stk_rec_part(v) & 0xFFFFu;
    stk_rec_maxtime(rc);
    if (rc)
        stk_rec_result(rc);
    return (uint16_t)rc;
}

static void op_tempo(void *c, uint16_t bpm)
{
    (void)c;
    stk_rec_tempo(bpm);
}

/* 0x40160A9C..0x40160ACC (MusicMode 2, bar form): FUN_40169448(bar, data[5] = 0, block) */
static void op_seek(void *c, uint16_t bar)
{
    (void)c;
    stk_rec_seek(bar, 0, stk_rec_block(0));
}

static const rc_ops_t OPS = {op_post, op_select, op_part, op_tempo, op_seek, 0};

/* ---- RW (core RW region, never loaded: zeroed by init) -------------------------------------------------- */
typedef struct {
    uint32_t guard_lo;
    rv_view_t cur, prev;   /* the view now / at the last pump pass (both keep the song table)      */
    rc_t rc;
    uint8_t viewed;        /* the first full read (live + table) is done                            */
    uint8_t subs;          /* bit p: stock port p gets 5E 00 / 5E 01 notifications                  */
    uint8_t cmd_port;      /* port + 1 that sent the command now pending through op 0x1E (5E 03)    */
    uint8_t q_full, q_port, q_cmd;   /* op 1E 02 mailbox, run by the next pump pass              */
    uint16_t q_arg;
    uint8_t dirty;         /* 1 a state change to tell, 2 only the bar moved                         */
    uint8_t list_dirty;    /* the song table changed                                                  */
    uint32_t last_note;    /* ms of the last 5E 00 notification                                       */
    /* counters (op 1E 00) */
    uint16_t k_owned, k_refused, k_passed, op_cmds, notes, lists;
    rs_t rs;               /* R3: the save decision (write windows)                                   */
    uint8_t save_dirty;    /* R3: a window closed on this pass - 5E 04 to the subscribers              */
    uint32_t guard_hi;
} rcc_rw_t;

CAVE_RW static rcc_rw_t rw;
static uint8_t inited = 0;   /* payload .data: fresh with every payload copy */

static void init(void)
{
    memset(&rw, 0, sizeof rw);
    rw.guard_lo = GUARD;
    rw.guard_hi = ~GUARD;
    rc_init(&rw.rc, &OPS);
    rc_enable_rec(&rw.rc, rec2);
    rs_init(&rw.rs);
    rs_enable(&rw.rs, rec3);
    inited = 1;
}

void rcc_restart(void)
{
    inited = 0;
}

/* ---- frames ------------------------------------------------------------------------------------------ */
static const uint8_t BUSY[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x03, 0x01, 0x00, 0x00, 0xF7};    /* FUN_4014AFB8 */
static const uint8_t NODATA[] = {0xF0, 0x40, 0x7F, 0x7E, 0x04, 0x1E, 0x03, 0x00, 0x01, 0x6C, 0xF7};  /* FUN_4014DC0C(0xEC) */

static size_t pack7(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 7) {
        size_t g = n - i < 7 ? n - i : 7;
        uint8_t hi = 0;
        for (size_t k = 0; k < g; k++)
            hi |= (uint8_t)(((in[i + k] >> 7) & 1u) << k);
        out[o++] = hi;
        for (size_t k = 0; k < g; k++)
            out[o++] = in[i + k] & 0x7F;
    }
    return o;
}

/* F0 7D 43 41 37 39 5E <sub> <a> [<b> | pack7 raw] F7 */
#define FRAME_MAX (10 + RCC_LIST_RAW + (RCC_LIST_RAW + 6) / 7)
static uint16_t head(uint8_t *m, uint8_t sub, uint8_t a)
{
    m[0] = 0xF0, m[1] = 0x7D, m[2] = 0x43, m[3] = 0x41, m[4] = 0x37, m[5] = 0x39;
    m[6] = RCC_OP | 0x40, m[7] = sub & 0x7F, m[8] = a & 0x7F;
    return 9;
}

static void send_short(uint8_t port, uint8_t sub, uint8_t a, uint8_t b)
{
    uint8_t m[11];
    uint16_t o = head(m, sub, a);
    m[o++] = b & 0x7F;
    m[o++] = 0xF7;
    sxc_tx(port, m, o);
}

/* noinline: the frame buffer stays out of the callers' frames */
__attribute__((noinline)) static void send_record(uint8_t port, uint8_t sub)
{
    uint8_t m[FRAME_MAX], raw[RCC_LIST_RAW];
    uint16_t o = head(m, sub, 0);
    if (sub == 1) {
        rcc_list(raw);
        o = (uint16_t)(o + pack7(raw, RCC_LIST_RAW, m + o));
    } else if (sub == 4) {
        rs_encode(&rw.rs, raw);
        o = (uint16_t)(o + pack7(raw, RS_ENC_LEN, m + o));
    } else {
        rcc_status(raw);
        o = (uint16_t)(o + pack7(raw, RCC_STATUS_RAW, m + o));
    }
    m[o++] = 0xF7;
    sxc_tx(port, m, o);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }

void rcc_status(uint8_t raw[RCC_STATUS_RAW])
{
    uint8_t enc[RV_ENC_LEN];
    memset(raw, 0, RCC_STATUS_RAW);
    if (!inited || !rw.viewed) {
        raw[0] = RV_ENC_VERSION;
        raw[19] = 0xFF;   /* table not read yet */
        return;
    }
    rv_encode(&rw.cur, enc, sizeof enc);
    memcpy(raw, enc, 32);
    raw[31] = 0x01;   /* the core deletes songs itself (InternalSongDelete, recdel.h) - the app's cue to offer Delete */
    raw[32] = rw.rc.pend;
    raw[33] = rw.rc.last_rc;
    raw[34] = rw.subs;
    raw[35] = (uint8_t)(0x01 | (rw.q_full ? 0x02 : 0) | (rw.cur.mq_hold ? 0x04 : 0) |
                        (rw.guard_lo == GUARD && rw.guard_hi == ~GUARD ? 0x08 : 0) |
                        (rw.rc.rec ? 0x10 : 0) | (rw.rc.fc_out ? 0x20 : 0) | (rw.rc.defer ? 0x40 : 0)
                        | (rw.rs.on ? 0x80 : 0)
                        );
    put16(raw + 36, rw.rc.n_ok);
    put16(raw + 38, rw.rc.n_refused);
    put16(raw + 40, rw.rc.n_timeout);
    put16(raw + 42, rw.k_owned);
    put16(raw + 44, rw.k_passed);
    put16(raw + 46, rw.notes);
}

void rcc_list(uint8_t raw[RCC_LIST_RAW])
{
    uint8_t enc[RV_ENC_LEN];
    memset(raw, 0, RCC_LIST_RAW);
    if (!inited || !rw.viewed)
        return;
    rv_encode(&rw.cur, enc, sizeof enc);
    memcpy(raw, enc + 32, RCC_LIST_RAW);
}

/* ---- the decision, shared by the Kawai bytes and op 0x1E ------------------------------------------------- */
/* run one command on the fresh view; `from` = the op sender (port + 1) for 5E 03, 0 for Kawai bytes */
static uint8_t run_cmd(uint8_t cmd, uint16_t arg, uint8_t from)
{
    uint8_t old = rw.rc.pend, old_port = rw.cmd_port, r;
    rv_read_live(&MEM, &rw.cur);
    /* the stock seeks only in MusicMode 2 (0x40160A00: mode 0 falls to the common tail) */
    if (cmd == RC_SEEK && rw.cur.music_mode != RC_MODE_INTERNAL &&
        rw.cur.state != RV_ST_REC_STANDBY && rw.cur.state != RV_ST_RECORDING &&
        rw.cur.state != RV_ST_REC_STANDBY_NOMETRO && rw.cur.state != RV_ST_COUNT_IN)
        return RC_E_MODE;
    /* R3: a song-table write window is open (commit / delete / import until its NOR job is done and checked) - nothing
     * that loads the table or starts a take; stop / pause / tempo do not touch it */
    if (rs_busy(&rw.rs) && cmd != RC_STOP && cmd != RC_PAUSE && cmd != RC_TEMPO)
        return RC_E_BUSY;
    r = rc_cmd(&rw.rc, &rw.cur, cmd, arg);
    if (r == RC_OK) {
        if (old && old_port && old != cmd)
            send_short((uint8_t)(old_port - 1), 0x03, RCC_EV_REPLACED, old);   /* a Stop / Pause took its place */
        rw.cmd_port = from;
    }
    return r;
}

/* Kawai frame -> recctl command; 0 = not one the core takes (the stock's) */
static uint8_t kawai_map(const uint8_t *m, uint32_t n, uint16_t *arg)
{
    uint8_t fn = m[3];
    if (n < 11 || m[6] != 0x52 || m[8] != 0x7F || m[4] != 0x08 || m[5] != 0x02)
        return 0;
    switch (m[7]) {
    case 0x21:   /* InternalSongDelete 0xCE -> 0x40160AD4: [fmt, hi7, lo7, part] - the core's (recdel.h) */
        if (n != 14 || (fn != 0x10 && fn != 0x32))
            return 0;
        *arg = 0;
        return RCC_K_DELETE;
    case 0x08:   /* Music: descriptors 0x93 (fn 0x10) / 0xD0 (fn 0x32) -> 0x401636CC */
        if (n != 11 || (fn != 0x10 && fn != 0x32))
            return 0;
        *arg = 0;
        if (rec2 && m[9] >= RC_MUSIC_ARM && m[9] <= RC_MUSIC_COUNT_IN) {
            /* R2: the panel's own record buttons - its part (0x4191EF73), it asks the user before replacing */
            *arg = RC_REC_KEEP | RC_REC_OVERWRITE;
            return m[9] == RC_MUSIC_ARM ? RC_REC : m[9] == RC_MUSIC_START ? RC_REC_START
                 : m[9] == RC_MUSIC_ARM_NOMETRO ? RC_REC_NOMETRO : RC_COUNT_IN;
        }
        return m[9] == 0 ? RC_PAUSE : m[9] == 1 ? RC_PLAY : m[9] == 2 ? RC_STOP : 0;
    case 0x00:   /* SongSelect 0x8A -> 0x40160F68: [numType, hi7, lo7], numType 0 only */
        if (n != 13 || fn != 0x10 || m[9] != 0)
            return 0;
        *arg = (uint16_t)(m[10] << 7 | m[11]);
        return RC_SELECT;
    case 0x01:   /* IntSongPart 0x8B -> 0x40160F40 */
        if (n != 11 || fn != 0x10)
            return 0;
        *arg = m[9];
        return RC_PART;
    case 0x02:   /* MusicTempo 0x92 -> 0x4016372C: hi7 << 7 | lo7 */
        if (n != 12 || fn != 0x10)
            return 0;
        *arg = (uint16_t)(m[9] << 7 | m[10]);
        return RC_TEMPO;
    case 0x03:   /* MusicProgress 0x94 / 0xD1 -> 0x401609FC: bar form data[0] == 0, bar = d1 << 7 | d2, b = d5 */
        if (n != 16 || (fn != 0x10 && fn != 0x32) || m[9] != 0 || m[14] != 0)
            return 0;
        *arg = (uint16_t)(m[10] << 7 | m[11]);
        return RC_SEEK;
    default: return 0;
    }
}

static bool rec_state(uint8_t s)
{
    return s == RV_ST_REC_STANDBY || s == RV_ST_RECORDING || s == RV_ST_REC_STANDBY_NOMETRO || s == RV_ST_COUNT_IN;
}

static uint32_t rd32(uint32_t a)
{
    uint8_t b[4];
    mem_read(0, a, b, 4);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* the core's delete (judged by the caller): build the song into the stock buffer and write it with the stock writer,
 * then the stock's own after-write sequence (FUN_4016C7BC: FUN_401686E8, FUN_401674FC(song), FUN_40167128) and its
 * answer (rc ? Result : 32 51 7E 7F 00 00).  noinline: none of this in rcc_kawai's frame */
__attribute__((noinline)) static void own_delete(uint8_t port, uint8_t song, uint8_t part)
{
    uint32_t o0 = rd32(RV_A_STORE + 4u * song), o1 = rd32(RV_A_STORE + 4u * (song + 1u)), len = 0, rc = 0;
    uint8_t r = RD_E_SONG;
    if (o1 > o0 && o1 <= RV_STORE_LIMIT)
        r = rd_build(PTR(RV_A_STORE + o0), o1 - o0, part, PTR(RD_A_TEMPLATE + RD_HDR * song), PTR(RD_A_BUF), RD_BUF_MAX,
                     &len);
    if (rec3 && r != RD_OK)   /* nothing will be written: the window rs_delete opened has nothing to wait for */
        rs_nowrite(&rw.rs);
    if (r != RD_OK && r != RD_NOTHING) {   /* a song we cannot read: nothing written */
        rw.k_refused++;
        sxc_tx(port, BUSY, sizeof BUSY);
        return;
    }
    if (r == RD_OK) {
        rc = stk_rec_store_write(song, RD_A_BUF, len) & 0xFFFFu;
        stk_rec_store_changed();
        stk_rec_load(song);
        stk_rec_load_done();
        rv_read_store(&MEM, &rw.cur);   /* 1E 01 right after the answer lists the new table (RAM is written, the job
                                           programs NOR later); the next pump pass tells the subscribers (5E 01) */
    }
    if (rc)
        stk_rec_result(rc);
    else
        stk_rec_edit_ok(0);
}

int rcc_kawai(uint8_t port, const uint8_t *m, uint32_t n)
{
    uint16_t arg = 0;
    uint8_t cmd, r;
    if (!inited || !rw.viewed || port > 3)
        return 0;   /* before the first pump pass: the stock's, as without the core */
    cmd = kawai_map(m, n, &arg);
    if (!cmd)
        return 0;
    rv_read_live(&MEM, &rw.cur);
    if (cmd == RCC_K_DELETE) {   /* the stock's own delete re-serialises the last take (recdel.h): the core deletes */
        uint16_t song = (uint16_t)(m[10] << 7 | m[11]);
        rv_read_store(&MEM, &rw.cur);
        r = rd_judge(&rw.cur, m[9], song, m[12]);
        if (rec3 && r != RD_V_ARG) {   /* R3 opens its write window (or refuses / ignores) first */
            uint8_t s3 = rs_delete(&rw.rs, &rw.cur, m[9], song, m[12]);
            if (s3 == RS_V_BUSY || s3 == RS_V_ARG)
                r = RD_V_BUSY;
        }
        if (r == RD_V_STOCK) {
            rw.k_passed++;
            return 0;
        }
        rw.k_owned++;
        if (r != RD_V_OWN) {
            rw.k_refused++;
            sxc_tx(port, BUSY, sizeof BUSY);
            return 1;
        }
        own_delete(port, (uint8_t)song, m[12]);
        return 1;
    }
    /* another player (USB 3, lesson 4, Concert Magic 5, Piano Music 6) and recording (R2): the stock's */
    if ((rw.cur.music_mode != 0 && rw.cur.music_mode != RC_MODE_INTERNAL) ||
        (!rec2 && rec_state(rw.cur.state) && (cmd == RC_STOP || cmd == RC_PAUSE)) ||
        (cmd == RC_SEEK && rw.cur.music_mode != RC_MODE_INTERNAL && !rec_state(rw.cur.state))) {
        rw.k_passed++;
        return 0;
    }
    r = run_cmd(cmd, arg, 0);
    rw.k_owned++;
    if (r != RC_OK && r != RC_NOP) {
        rw.k_refused++;
        if (r == RC_E_NODATA)
            sxc_tx(port, NODATA, sizeof NODATA);
        else
            sxc_tx(port, BUSY, sizeof BUSY);
    }
    return 1;
}

/* ---- op 0x1E --------------------------------------------------------------------------------------------- */
void rcc_sysex(uint8_t port, const uint8_t *buf, uint32_t len)
{
    uint8_t sub = len >= 9 ? buf[7] : 0;
    bool bound;
    port &= 3;
    if (!inited)
        init();
    bound = (sxc_bound_mask() >> port) & 1u;
    if (len == 9 && sub == 4 && rec3) {   /* R3: the save record 5E 04 */
        if (bound)
            rw.subs |= (uint8_t)(1u << port);
        send_record(port, 4);
        return;
    }
    if (len == 8 || (len == 9 && (sub == 0 || sub == 1))) {
        if (bound)
            rw.subs |= (uint8_t)(1u << port);
        send_record(port, sub);
        return;
    }
    if (len == 12 && sub == 2) {
        uint8_t cmd = buf[8], rc = RC_OK;
        if (!bound)
            rc = RCC_NOT_BOUND;
        else if (!rw.viewed || rw.q_full)
            rc = RC_E_PENDING;
        else if (cmd < RC_PLAY || cmd > (rec2 ? RC_REC_START : RC_SEEK))
            rc = RC_E_ARG;
        if (rc != RC_OK) {
            send_short(port, 0x02, rc, cmd);
            return;
        }
        rw.subs |= (uint8_t)(1u << port);
        rw.q_port = port, rw.q_cmd = cmd, rw.q_arg = (uint16_t)(buf[9] << 7 | buf[10]);
        rw.q_full = 1;   /* the next pump pass (<= 2 ms) runs it and answers 5E 02 */
        return;
    }
    send_short(port, sub, RC_E_ARG, 0);
}

/* R3, task 19 (sysex_cave.c, before the session core): a fn 0x24 request 24 08 / 24 09 (d1 = its first data byte) */
int rcc_file_veto(uint8_t d0, uint8_t d1)
{
    if (!inited || !rw.viewed || !rec3)
        return 0;
    rv_read_live(&MEM, &rw.cur);
    return rs_file(&rw.rs, &rw.cur, d0, d1) == RS_V_BUSY;
}

int rcc_quiesce(void)
{
    return inited && rw.q_full;
}

/* ---- pump tail ------------------------------------------------------------------------------------------- */
void rcc_pump(uint32_t now_ms)
{
    uint32_t d;
    uint16_t ev;
    uint8_t bound;
    if (!inited)
        init();
    if (!rw.viewed) {
        rv_read(&MEM, &rw.cur);
        rw.prev = rw.cur;
        rw.viewed = 1;
        rw.last_note = now_ms;
        return;
    }
    rv_read_live(&MEM, &rw.cur);
    d = rv_diff(&rw.prev, &rw.cur);
    if (d & RV_CH_COMMIT) {   /* a recording ended / a NOR job or a file operation finished: the table again */
        rv_read_store(&MEM, &rw.cur);
        d |= rv_diff(&rw.prev, &rw.cur);
    }
    if (rs_wants_table(&rw.rs, &rw.cur)) {   /* a window may close: compare with the table as it is now */
        rv_read_store(&MEM, &rw.cur);
        d |= rv_diff(&rw.prev, &rw.cur);
    }
    if (rs_poll(&rw.rs, &rw.cur))
        rw.save_dirty = 1;
    ev = rc_poll(&rw.rc, &rw.cur);
    if (ev && rw.cmd_port) {
        send_short((uint8_t)(rw.cmd_port - 1), 0x03, (uint8_t)(ev >> 8), (uint8_t)ev);
        rw.cmd_port = 0;
    } else if (ev) {
        rw.cmd_port = 0;
    }
    if (rw.q_full) {   /* op 1E 02, judged on this pass's view */
        uint8_t r = run_cmd(rw.q_cmd, rw.q_arg, (uint8_t)(rw.q_port + 1));
        rw.op_cmds++;
        rw.q_full = 0;
        send_short(rw.q_port, 0x02, r, rw.q_cmd);
        if (r != RC_OK && r != RC_NOP)
            rw.k_refused++;
        rv_read_live(&MEM, &rw.cur);   /* a select / part / seek / tempo acts at once */
        d |= rv_diff(&rw.prev, &rw.cur);
    }
    if (d & ~(uint32_t)(RV_CH_BAR | RV_CH_COMMIT))
        rw.dirty |= 1;
    else if (d & RV_CH_BAR)
        rw.dirty |= 2;
    if (d & RV_CH_STORE)
        rw.list_dirty = 1;
    bound = sxc_bound_mask();
    rw.subs &= bound;   /* a controller that left is forgotten (it subscribes again with its next 1E request) */
    if (rw.save_dirty && rw.subs)   /* a write window closed: its result to every subscriber */
        for (uint8_t p = 0; p < 4; p++)
            if (rw.subs & (1u << p))
                send_record(p, 4);
    rw.save_dirty = 0;
    if (!rw.subs) {
        rw.dirty = 0, rw.list_dirty = 0;
    } else if (rw.dirty && now_ms - rw.last_note >= ((rw.dirty & 1) ? RCC_NOTE_MS : RCC_BAR_MS)) {
        for (uint8_t p = 0; p < 4; p++)
            if (rw.subs & (1u << p)) {
                send_record(p, 0);
                rw.notes++;
                if (rw.list_dirty) {
                    send_record(p, 1);
                    rw.lists++;
                }
            }
        rw.dirty = 0, rw.list_dirty = 0;
        rw.last_note = now_ms;
    }
    rw.prev = rw.cur;
}
