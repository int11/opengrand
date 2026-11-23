/* OpenGrand — save decision for the STOCK song store (Phase 3-6 stage R3).
 *
 * Every write of the 10-song store (RecorderData RAM mirror 0x40500000 -> NOR job slot 1) goes through the stock writer
 * FUN_40168408 and is followed by one NOR job (FUN_40134048(1, ...)).  Its callers (REA):
 *   commit       FUN_40168774 (ring FC in task 20, record limit, SongSelect / CONNECT / power-off in state 4)
 *                -> FUN_40168710 -> FUN_40168664 -> FUN_40168408, THEN state 2 - so when the recorder state is seen to
 *                leave 4 the job is already queued
 *   delete       Kawai InternalSongDelete 32 52 21 7F [fmt hi lo part] (apply 0x40160AD4, task 19): fmt 04 / 7F only;
 *                hi:lo 7F 7F -> FUN_40134244() == 1 ? nothing : FUN_4016C908 (format: rebuild the empty table + job);
 *                else song (>= 10 -> song 1) -> FUN_4016C800(song, part): 7F both parts, 0 / 1 one part, else nothing
 *                -> FUN_4016C7BC = FUN_4016933C + FUN_40168664 (serialise into 0x40A00000) + FUN_401686E8 +
 *                FUN_401674FC(song) (reloads the PLAYER with that song) + FUN_40167128.  No recorder state test.
 *                Reply: rc ? Result FUN_4014DC0C(rc) : FUN_4014F294(0) = 32 51 7E 7F 00 00.
 *   KSO import   fn 0x24 09 <song> 03 + file name -> file executor kind 9 FUN_4014F1A8 (engine loop): file size check,
 *                FUN_40127274 (0x40A00000) -> FUN_40168408(song, ...) -> FUN_401686E8 + FUN_401674FC(song).  No
 *                recorder state test either.  24 08 (USB song load) converts into 0x40A00000 as well.
 *
 * The core cannot step between FUN_40168408's RAM move and its job (no allow-listed hook there), so R3 decides
 * BEFORE a write may start and CHECKS after the job is done:
 *   - a write window opens when a write is handed over (delete), seen (the recorder leaves state 4 = a commit, the
 *     file executor starts = an import / load / export), and closes when no NOR job / worker / file operation is left;
 *     then the song table is read again and compared with the last settled one: it must pass the boot-check rules and
 *     only the target song may differ (else RS_R_TABLE / RS_R_OTHER), one job per window (else RS_R_DOUBLE)
 *   - while a window is open, nothing else that writes or loads the table is let through (rs_busy)
 *   - delete and import are refused (Busy) in the states where the stock would reload the player under a take or a
 *     playing song (1, 3-6), while a job / file operation / window runs, and (single song) when the table fails the
 *     check; a song number >= 10 is refused instead of deleting song 1.  Everything else is the stock's own path.
 * Nothing here writes stock RAM or calls a stock routine.
 *
 * Repair of a broken commit (2026-11-24 field incident: a take of part 2 with no events on an empty song; the stock
 * serialiser FUN_4016804C leaves track slot 0 at the song end, recdel.h rd_repair): a COMMIT window whose table fails
 * the boot-check rules only at a track of its own target song, while the other nine songs keep their settled lengths,
 * is mended by the core instead of only reported - the port rebuilds the target song (rd_repair, empty template when no
 * track is left) and writes it with the same stock writer FUN_40168408 as the core's delete, which queues the NOR job
 * again.  When: as soon as the commit is seen and the NOR worker 0x4139DAC4 is NOT erasing / programming - the port
 * runs in task 19, above the worker (task 29), so a job still pending then programs the mended RAM (FUN_401340B0 reads
 * RAM while programming and clears the pending byte only afterwards: a re-post DURING a run would be lost, hence never
 * then).  If the worker already runs, the window waits for it (RS_P_FIX after the job) and mends right after, which
 * queues a second job: NOR holds the broken table only for that job's time instead of until the next boot.  One try
 * per window; RS_R_FIXED when the table passes afterwards. */
#ifndef CA79NG_RECSAVE_H
#define CA79NG_RECSAVE_H

#include <stdint.h>

#include "ca79ng/recview.h"

#define RS_WAIT_POLLS 1500u   /* 3 s of 2 ms polls for the job after a delete / commit (it is queued synchronously) */
#define RS_MAX_POLLS 30000u   /* 60 s: a window still busy after this closes as RS_R_STUCK */
#define RS_FILE_PEND_POLLS 5000u /* 10 s: an allowed 24 09 names the song of the next file operation */
#define RS_FIX_POLLS 500u     /* 1 s: a window waiting to mend its song after the job closes as RS_R_TABLE after this */

/* window kinds */
enum { RS_K_NONE = 0, RS_K_COMMIT = 1, RS_K_DELETE = 2, RS_K_FORMAT = 3, RS_K_FILE = 4 };
/* window phase */
enum { RS_P_IDLE = 0, RS_P_WAIT = 1, RS_P_JOB = 2, RS_P_FIX = 3 /* job done, the target song is to be mended */ };
/* rs_t.fix */
enum { RS_F_NONE = 0, RS_F_LOOKED = 1 /* nothing to mend, or it could not be */, RS_F_DONE = 2 /* mended */ };
/* targets beyond 0-9 */
#define RS_T_ALL 0xFFu   /* format: every song may change */
#define RS_T_ANY 0xFEu   /* a file operation with no known song: at most one song may change */
/* results of a closed window */
enum {
    RS_R_NONE = 0,    /* no window closed yet */
    RS_R_OK = 1,      /* one job, the table passes the check, only the target changed */
    RS_R_NOWRITE = 2, /* a file operation that wrote nothing (export, USB load, refused by the stock) */
    RS_R_NOJOB = 3,   /* a commit / delete without a NOR job (the stock wrote nothing or the job was missed) */
    RS_R_TABLE = 4,   /* the table fails the boot-check rules after the write: report, keep the power on */
    RS_R_OTHER = 5,   /* a song other than the target changed */
    RS_R_STUCK = 6,   /* still busy after RS_MAX_POLLS */
    RS_R_DOUBLE = 7,  /* more than one NOR job in one window (P5) */
    RS_R_FIXED = 8    /* the commit broke its own song (boot-check rules); the core mended it and the table passes */
};
/* rs_delete() / rs_file() verdicts */
enum {
    RS_V_PASS = 0,    /* hand it to the stock (a write window is open for it when it writes) */
    RS_V_IGNORE = 1,  /* the stock ignores / does nothing with it: hand it over, no window */
    RS_V_BUSY = 2,    /* refuse: Busy */
    RS_V_ARG = 3      /* refuse: song number the stock would misread (>= 10 -> song 1) */
};
#define RS_ALL_SONGS 0x3FFFu  /* hi:lo 7F 7F */

typedef struct {
    uint8_t on;
    uint8_t phase, kind, target, jobs;
    uint8_t prev_state, prev_job, have_base;
    uint8_t fix;            /* RS_F_*: the repair of this window */
    uint16_t left;          /* polls left in this phase */
    uint16_t file_left;     /* polls the import song stays valid */
    uint8_t file_song;      /* song of the last allowed 24 09 (0xFF none) */
    /* the last settled table (passes the check, nothing busy, not recording) */
    uint32_t base_len[RV_SONGS];
    uint8_t base_flags[RV_SONGS];
    /* last closed window */
    uint8_t last, last_kind, last_target, last_jobs;
    uint16_t last_changed;  /* bit i: song i differs from the base */
    uint16_t n_windows, n_ok, n_bad, n_refused, n_delete, n_file;
} rs_t;

void rs_init(rs_t *rs);
void rs_enable(rs_t *rs, uint8_t on);

/* every 2 ms with the current view.  Returns 1 when a window closed on this pass (read rs->last), else 0.  Before
 * calling, the caller re-reads the song table when rs_wants_table() says so. */
uint8_t rs_poll(rs_t *rs, const rv_view_t *v);
/* the window may close on this pass: the song table must be fresh (rv_read_store) */
uint8_t rs_wants_table(const rs_t *rs, const rv_view_t *v);
/* a write window is open: nothing else that writes or loads the song table may start */
uint8_t rs_busy(const rs_t *rs);

/* Kawai InternalSongDelete [fmt, hi, lo, part]; RS_V_PASS opens the window (the caller then writes the song itself - recdel.h; the format 7F 7F goes to the stock apply) */
uint8_t rs_delete(rs_t *rs, const rv_view_t *v, uint8_t fmt, uint16_t song, uint8_t part);
/* the core's delete the window was opened for wrote nothing (the part is absent, or the song cannot be read and the
 * delete was refused): forget that window - no result, not counted as a window or a delete */
void rs_nowrite(rs_t *rs);
/* fn 0x24 request d0 (04..12) with its first data byte d1 (24 09: the song) */
uint8_t rs_file(rs_t *rs, const rv_view_t *v, uint8_t d0, uint8_t d1);

/* repair (see the head comment).  rs_fix_wanted: the song (0-9) the port should mend now, else 0xFF - a COMMIT window
 * not tried yet, the fresh table fails only at a track of the target song, state 0 / 2, the NOR worker idle, no file
 * operation.  The port then reads the raw offset table, rs_fix_table() must accept it (sane, the other nine songs keep
 * their settled lengths), builds the song (rd_repair), writes it with the stock writer and reports rs_fixed(ok). */
uint8_t rs_fix_wanted(const rs_t *rs, const rv_view_t *v);
uint8_t rs_fix_table(const rs_t *rs, const uint32_t off[RV_SONGS + 2]);
void rs_fixed(rs_t *rs, const rv_view_t *v, uint8_t ok);

#define RS_ENC_LEN 24
/* the op 1E 04 record (REMOTE_PROTOCOL §10.9) */
void rs_encode(const rs_t *rs, uint8_t out[RS_ENC_LEN]);

#endif
