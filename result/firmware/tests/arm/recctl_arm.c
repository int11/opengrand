/* Phase 3-6 R1 / R2 / R3 emulator glue: recctl (src/recorder/recctl.c) + recview bound to the STOCK routines
 * the stock SysEx apply FUN_4016053C uses, for result/stockfw/tools/roundtrip_recctl.py (R1),
 * roundtrip_recrec.py (R2: rca_rec(1) offers the record transports first) and roundtrip_recsave.py (R3: rca_save(1)
 * adds the save decision src/recorder/recsave.c - the cave port's RCC_REC3 calls, recctl_cave.c).
 *
 * Test only - not a cave port and not in any image (the cave binding is ports/cave/recctl_cave.c).  The round trip builds this file with the pinned ARM
 * toolchain, links it into a free SDRAM window of the stock-image emulator and calls rca_cmd() /
 * rca_poll() where the stock apply / the 2 ms poll would run (task 19).  The bodies below repeat
 * the stock apply's own sequence for each parameter (addresses: include/ca79ng/recctl.h). */
#include "ca79ng/recctl.h"
#include "ca79ng/recdel.h"
#include "ca79ng/recsave.h"

typedef uint32_t (*fn0_t)(void);
typedef uint32_t (*fn1_t)(uint32_t);
typedef uint32_t (*fn2_t)(uint32_t, uint32_t);
typedef uint32_t (*fn3_t)(uint32_t, uint32_t, uint32_t);

#define STOCK_Q_POST      ((fn2_t)0x4015D70Cu) /* (kind, value) -> Music queue */
#define STOCK_CUR_BLOCK   ((fn1_t)0x4015B4D8u) /* (0) -> current settings block */
#define STOCK_SONG_SELECT ((fn2_t)0x401687E4u) /* (block, song) */
#define STOCK_INT_PART    ((fn1_t)0x4016C9B8u) /* (v) */
#define STOCK_MAXTIME     ((fn1_t)0x401694E4u) /* (rc): MaxTime notification 52 18 */
#define STOCK_RESULT      ((fn1_t)0x4014DC0Cu) /* (rc): Result 7E 03 to the bound controller */
#define STOCK_TEMPO       ((fn1_t)0x40167294u) /* (bpm) */
#define STOCK_SEEK        ((fn3_t)0x40169448u) /* (bar, data[5], block) */
/* R3: the stock InternalSongDelete apply 0x40160AD4..0x40160B54 (the cave lets that apply run itself) */
#define STOCK_FLASH_STATE ((fn0_t)0x40134244u) /* () == 1: the format is skipped */
#define STOCK_FORMAT      ((fn0_t)0x4016C908u) /* () empty table + NOR job */
#define STOCK_EDIT_OK     ((fn1_t)0x4014F294u) /* (0): 32 51 7E 7F 00 00 */
/* the core's delete (recctl_cave.c own_delete): the stock writer and its after-write */
#define STOCK_STORE_WRITE ((fn3_t)0x40168408u) /* (song, src, len): song write + NOR job */
#define STOCK_STORE_CHG   ((fn0_t)0x401686E8u) /* () */
#define STOCK_LOAD        ((fn1_t)0x401674FCu) /* (song): player loader */
#define STOCK_LOAD_DONE   ((fn0_t)0x40167128u) /* () */

static void cave_read(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
    (void)ctx;
    const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)addr;
    uint8_t *d = dst;
    for (uint32_t i = 0; i < n; i++)
        d[i] = s[i];
}

static const rv_mem_t MEM = {cave_read, 0};

static void op_post(void *c, uint8_t kind, uint8_t value)
{
    (void)c;
    STOCK_Q_POST(kind, value);
}

/* 0x40160F68..0x40160FB4: rc = FUN_401687E4(block, n); FUN_401694E4(rc); rc ? FUN_4014DC0C(rc) */
static uint16_t op_select(void *c, uint8_t song)
{
    (void)c;
    uint32_t rc = STOCK_SONG_SELECT(STOCK_CUR_BLOCK(0), song) & 0xFFFFu;
    STOCK_MAXTIME(rc);
    if (rc)
        STOCK_RESULT(rc);
    return (uint16_t)rc;
}

/* 0x40160F40..0x40160F64: rc = FUN_4016C9B8(v); FUN_401694E4(rc); rc ? FUN_4014DC0C(rc) */
static uint16_t op_part(void *c, uint8_t v)
{
    (void)c;
    uint32_t rc = STOCK_INT_PART(v) & 0xFFFFu;
    STOCK_MAXTIME(rc);
    if (rc)
        STOCK_RESULT(rc);
    return (uint16_t)rc;
}

static void op_tempo(void *c, uint16_t bpm)
{
    (void)c;
    STOCK_TEMPO(bpm);
}

/* 0x40160A9C..0x40160ACC (MusicMode 2, bar form): FUN_40169448(bar, data[5] = 0, block) */
static void op_seek(void *c, uint16_t bar)
{
    (void)c;
    STOCK_SEEK(bar, 0, STOCK_CUR_BLOCK(0));
}

static const rc_ops_t OPS = {op_post, op_select, op_part, op_tempo, op_seek, 0};

static rc_t RC;
static rv_view_t VIEW;
static uint8_t INITED;
static rs_t SAVE;          /* R3 (rca_save) */
static uint8_t SAVE_CLOSED;

/* out[0] = result, out[1] = last stock rc of select / part */
uint32_t rca_cmd(uint32_t cmd, uint32_t arg, uint8_t *out)
{
    if (!INITED)
        rc_init(&RC, &OPS), INITED = 1;
    rv_read(&MEM, &VIEW); /* a fresh live view and table (the cave keeps the table by rv_diff) */
    /* R3: the cave's run_cmd gate (recctl_cave.c) */
    if (rs_busy(&SAVE) && cmd != RC_STOP && cmd != RC_PAUSE && cmd != RC_TEMPO) {
        out[0] = RC_E_BUSY, out[1] = RC.last_rc;
        return RC_E_BUSY;
    }
    uint8_t r = rc_cmd(&RC, &VIEW, (uint8_t)cmd, (uint16_t)arg);
    out[0] = r, out[1] = RC.last_rc;
    return r;
}

/* R2: offer the record transports (the cave's feat2 0x40 build) */
uint32_t rca_rec(uint32_t on)
{
    if (!INITED)
        rc_init(&RC, &OPS), INITED = 1;
    rc_enable_rec(&RC, (uint8_t)on);
    return RC.rec;
}

/* R2 diagnostics: fc_out | defer << 1 | n_finish << 8 */
uint32_t rca_state(void)
{
    return (uint32_t)RC.fc_out | (uint32_t)RC.defer << 1 | (uint32_t)RC.n_finish << 8;
}

/* R3 repair: as recctl_cave.c own_fix */
static void own_fix(uint8_t song)
{
    uint32_t off[RV_SONGS + 2], len = 0;
    uint8_t r = RD_E_SONG, ok = 0;
    for (unsigned i = 0; i < RV_SONGS + 2; i++)
        off[i] = *(volatile uint32_t *)(uintptr_t)(RV_A_STORE + 4u * i);
    if (rs_fix_table(&SAVE, off))
        r = rd_repair((const uint8_t *)(uintptr_t)(RV_A_STORE + off[song]), off[song + 1] - off[song],
                      (const uint8_t *)(uintptr_t)(RD_A_TEMPLATE + RD_HDR * song), (uint8_t *)(uintptr_t)RD_A_BUF,
                      off[song + 1] - off[song], &len);
    if (r == RD_OK && (STOCK_STORE_WRITE(song, RD_A_BUF, len) & 0xFFFFu) == 0) {
        STOCK_STORE_CHG();
        STOCK_LOAD(song);
        STOCK_LOAD_DONE();
        ok = 1;
    }
    rv_read_store(&MEM, &VIEW);
    rs_fixed(&SAVE, &VIEW, ok);
    if (SAVE.phase == RS_P_IDLE)
        SAVE_CLOSED = 1;
}

uint32_t rca_poll(void)
{
    if (!INITED)
        rc_init(&RC, &OPS), INITED = 1;
    uint8_t was = VIEW.state;
    rv_read_live(&MEM, &VIEW);
    if ((was == RV_ST_RECORDING && VIEW.state != RV_ST_RECORDING) || rs_wants_table(&SAVE, &VIEW))
        rv_read_store(&MEM, &VIEW);   /* R3: as rcc_pump (RV_CH_COMMIT / a window that looks at its table) */
    SAVE_CLOSED |= rs_poll(&SAVE, &VIEW);
    uint8_t fix = rs_fix_wanted(&SAVE, &VIEW);
    if (fix < RV_SONGS)
        own_fix(fix);
    return rc_poll(&RC, &VIEW);
}

/* R3: the save decision on / off (the cave's feat3 0x04 build); the first full view is its settled table */
uint32_t rca_save(uint32_t on)
{
    if (!INITED)
        rc_init(&RC, &OPS), INITED = 1;
    rs_init(&SAVE);
    rs_enable(&SAVE, (uint8_t)on);
    rv_read(&MEM, &VIEW);
    rs_poll(&SAVE, &VIEW);
    return SAVE.on;
}

/* R3: closed-since-last | phase << 1 | kind << 4 | last << 8 | jobs << 12 | last_jobs << 16 | target << 20 |
 * (last_changed & 0xF) << 28; rca_save_changed() = last_changed */
uint32_t rca_save_state(void)
{
    uint32_t r = (uint32_t)SAVE_CLOSED | (uint32_t)SAVE.phase << 1 | (uint32_t)SAVE.kind << 4 |
                 (uint32_t)SAVE.last << 8 | (uint32_t)(SAVE.jobs & 0xF) << 12 | (uint32_t)(SAVE.last_jobs & 0xF) << 16 |
                 (uint32_t)SAVE.target << 20;
    SAVE_CLOSED = 0;
    return r;
}

uint32_t rca_save_changed(void) { return SAVE.last_changed; }

/* R3: a Kawai InternalSongDelete [fmt, hi, lo, part] from a bound controller, as the cave's rcc_kawai (recctl_cave.c):
 * rd_judge, then rs_delete opens its window (or refuses); the format 7F 7F and the stock-handled forms run the stock
 * apply's own sequence 0x40160AD4..0x40160B54 (part outside 0 / 1 / 7F: FUN_4016C800 writes nothing, the answer is OK),
 * an owned delete builds the song (rd_build) into 0x40A00000 and writes it with the stock writer FUN_40168408 like
 * own_delete; nothing to write -> rs_nowrite.  Returns the RS_V_* verdict (RS_V_PASS when the core wrote or answered). */
uint32_t rca_delete(uint32_t fmt, uint32_t song, uint32_t part)
{
    uint32_t rc = 0, len = 0;
    if (!INITED)
        rc_init(&RC, &OPS), INITED = 1;
    rv_read(&MEM, &VIEW);
    uint8_t r = rd_judge(&VIEW, (uint8_t)fmt, (uint16_t)song, (uint8_t)part);
    uint8_t v = r == RD_V_ARG ? RS_V_ARG : rs_delete(&SAVE, &VIEW, (uint8_t)fmt, (uint16_t)song, (uint8_t)part);
    if (v == RS_V_BUSY || v == RS_V_ARG)
        r = RD_V_BUSY;
    if (r == RD_V_STOCK) {
        if (fmt != 0x04 && fmt != 0x7F)   /* dropped by the stock apply */
            return RS_V_IGNORE;
        if (song == RS_ALL_SONGS) {   /* 0x40160B30 */
            if (STOCK_FLASH_STATE() != 1)
                STOCK_FORMAT();
        }   /* part outside 0 / 1 / 7F: FUN_4016C800 returns 0 without a write */
        STOCK_EDIT_OK(0);
        return v;
    }
    if (r != RD_V_OWN)   /* the cave sends the stock Busy bytes itself (not a stock call): nothing here */
        return v == RS_V_ARG ? RS_V_ARG : RS_V_BUSY;
    uint32_t o0 = *(volatile uint32_t *)(uintptr_t)(RV_A_STORE + 4u * song);
    uint32_t o1 = *(volatile uint32_t *)(uintptr_t)(RV_A_STORE + 4u * (song + 1u));
    uint8_t b = RD_E_SONG;
    if (o1 > o0 && o1 <= RV_STORE_LIMIT)
        b = rd_build((const uint8_t *)(uintptr_t)(RV_A_STORE + o0), o1 - o0, (uint8_t)part,
                     (const uint8_t *)(uintptr_t)(RD_A_TEMPLATE + RD_HDR * song), (uint8_t *)(uintptr_t)RD_A_BUF,
                     RD_BUF_MAX, &len);
    if (b != RD_OK)
        rs_nowrite(&SAVE);
    if (b != RD_OK && b != RD_NOTHING)
        return RS_V_BUSY;
    if (b == RD_OK) {
        rc = STOCK_STORE_WRITE(song, RD_A_BUF, len) & 0xFFFFu;
        STOCK_STORE_CHG();
        STOCK_LOAD(song);
        STOCK_LOAD_DONE();
    }
    if (rc)
        STOCK_RESULT(rc);
    else
        STOCK_EDIT_OK(0);
    return RS_V_PASS;
}

/* R3: a fn 0x24 request 24 <d0> <d1> (cave: rcc_file_veto) */
uint32_t rca_file(uint32_t d0, uint32_t d1)
{
    rv_read_live(&MEM, &VIEW);
    return rs_file(&SAVE, &VIEW, (uint8_t)d0, (uint8_t)d1);
}
