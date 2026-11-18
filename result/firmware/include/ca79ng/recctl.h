/* OpenGrand — playback control of the STOCK song player (Phase 3-6 stage R1).
 *
 * The core decides whether a controller's play / stop / pause / song select / part select /
 * tempo / seek may run, and then hands it to the stock firmware the way the stock SysEx apply
 * FUN_4016053C does for the panel and the app - never by writing recorder RAM itself.  Every
 * stock claim below is from REA (Ghidra 12.1.4) on CAx9_P06.SYS and the emulator.
 *
 *   transport  Music 10|32 52 08 (descriptors 0x93 / 0xD0 -> 0x401636CC) and MusicMode 10 52 1D /
 *              31 52 1D (0x87 / 0xCB -> 0x401636DC) are only QUEUED: FUN_4015D70C(kind, value)
 *              puts the byte pair into the ring 0x41915598 (write index 0x419156AA, read index
 *              0x419156A8, count 0x419156A5; a post that makes the count exceed 127 empties the
 *              queue), and task 20 (FUN_4016DE5C -> FUN_4015DE0C) runs kind 0 = FUN_4015DCC4
 *              (MusicMode) and kind 1 = FUN_4015DBD4 (Music -> FUN_4016A050 in MusicMode 2).
 *              FUN_4016A050's only callers are those two: the recorder command never runs in
 *              task 19.  Music values: 0 pause (state 1 -> 0), 1 play (not from 4), 2 stop
 *              (0/1 -> reload -> 2), 3-6 record (R2), 7 and up: nothing.  Play from state 2 waits
 *              dly_tsk(200) at least once inside task 20 (FUN_40166E74).
 *   select     SongSelect 10 52 00 (0x8A -> 0x40160F68): FUN_401687E4(block, song) in task 19, then
 *              FUN_401694E4(rc) (MaxTime notification) and FUN_4014DC0C(rc) when rc != 0 (0xEC
 *              = no data).  In state 4 it COMMITS the recording (FUN_40168774 -> NOR job) - the
 *              core never calls it outside states 0 / 1 / 2.  From 1 it restarts play in task 19
 *              without the 200 ms wait (FUN_401679D4 sets state 0 first).
 *   part       IntSongPart 10 52 01 (0x8B -> 0x40160F40): FUN_4016C9B8(v), then FUN_401694E4(rc),
 *              FUN_4014DC0C(rc) when rc != 0.
 *   tempo      MusicTempo 10 52 02 (0x92 -> 0x4016372C): FUN_40167294(hi << 7 | lo), no range check,
 *              ratio = bpm << 8 / original tempo 0x4191570E (FUN_40088C14).
 *   seek       MusicProgress 10|32 52 03 bar form (0x94 / 0xD1 -> 0x401609FC): in MusicMode 2
 *              FUN_40169448(bar, data[5], block): state 0 / 2 -> FUN_40165E84(bar) + FUN_401664AC
 *              (position = (bar - 1) x ticks per bar, tracks re-chased); state 1 -> the same between
 *              FUN_40165E74 (state 0) and FUN_40166E74 (restart, no wait); other states: nothing.
 *
 * So R1 never calls FUN_4016A050: transport goes through the queue only, the other four call
 * the same stock routines as the stock apply from the same task (19).  The result is read back
 * with recview (rv_view_t) - rc_poll() reports when the expected state was reached or did not
 * come.  No recording, no saving: every recorder state 3-6 refuses (RC_E_REC).
 *
 * Stage R2 (behind rc_enable_rec() - the cave port's feat2 0x40 build): the record
 * transports are decided here as well, and still only QUEUED (Music values through FUN_4015D70C, run by
 * FUN_4016A050 in task 20).  FUN_4016A050 per Music value and recorder state (REA disassembly 0x4016A050-0x4016A1E0):
 *   0 pause     1 -> FUN_401679D4 (0) | 4 -> FUN_40169FE8 (ring FC = finish) | else nothing
 *   1 play      4 -> FUN_4014BEF0(0) (Busy frame) | else FUN_40166E74 (plays, from 3 / 5 / 6 too)
 *   2 stop      0 / 1 -> FUN_401694B0 (reload, 2) | 3 -> state = 2 (store only) | 4 -> ring FC | 2 / 5 / 6 nothing
 *   3 arm       1 -> FUN_401694B0 first; FUN_401693CC: 0 / 2 / 3 / 5 -> prepare + 3, else 0xF0 (4, 6, 7, 8)
 *   4 start     1 -> FUN_4014BEF0(0) | else FUN_40169F74 (ring FA, from ANY other state - also 0 / 2 / 4)
 *   5 arm, no metronome   1 -> FUN_401694B0 only (stops, does not arm) | else FUN_40169F00: 0 / 2 / 3 / 5 -> 5
 *   6 count-in  1 -> FUN_401694B0 only | else FUN_40169EDC: no state test at all -> 6 (also from 4 = recording)
 * The ring consumer FUN_4016CCDC (task 20) runs FA = FUN_4016CB4C + FUN_401692D0 (prepare, state 4) and FC =
 * FUN_40168774 (commit: serialise -> FUN_40168408 -> NOR job, state 2) WITHOUT looking at the state, and both ring
 * writers drop the byte silently when the ring count 0x4191EF6C is > 0xFFF.  So the stock lets a remote start a
 * recording without arming (4 from 0 / 2), restart the take (4 or 6 while recording), commit twice (two stops in one
 * task-20 pass: both reach FUN_4016A050 in state 4 before the ring runs), and has no command that leaves 5 or 6
 * with a stop.  The core's rules (rc_cmd below): arm only from 0 / 1 / 2 or switching between the armed kinds, start
 * only when armed (3 / 5 / 6), one finish per take (fc_out), a finish waits (defer) while a NOR job / the worker / a
 * file operation is running or the ring is high, stop from 5 = arm 3 then stop, stop in 6 refused (RC_E_WRONG). */
#ifndef CA79NG_RECCTL_H
#define CA79NG_RECCTL_H

#include <stdint.h>

#include "ca79ng/recview.h"

/* stock queue kinds (FUN_4015DE0C) and the MusicMode the internal recorder needs */
#define RC_Q_MODE 0u
#define RC_Q_MUSIC 1u
#define RC_MODE_INTERNAL 2u
#define RC_MUSIC_PAUSE 0u
#define RC_MUSIC_PLAY 1u
#define RC_MUSIC_STOP 2u
#define RC_Q_MAX 127u          /* FUN_4015D70C empties the queue when the count would exceed this */

#define RC_TEMPO_MIN 10u       /* REMOTE_PROTOCOL §9.3 MusicTempo 10..500 */
#define RC_TEMPO_MAX 500u
#define RC_TIMEOUT_POLLS 1500u /* 3 s of 2 ms polls: play from stop waits >= 200 ms in task 20 */
/* R2 */
#define RC_MUSIC_ARM 3u        /* Music values of the record transports (FUN_4016A050) */
#define RC_MUSIC_START 4u
#define RC_MUSIC_ARM_NOMETRO 5u
#define RC_MUSIC_COUNT_IN 6u
#define RC_RING_HIGH 0xF00u    /* ring fill above which FA / FC could be dropped (writers drop at > 0xFFF) */
#define RC_DEFER_POLLS 15000u  /* a finish waits up to 30 s for a NOR job / file operation to end */
#define RC_REC_KEEP 0x7Fu      /* RC_REC* arg: record the part the stock has (0x4191EF73) */
#define RC_REC_OVERWRITE 0x100u /* RC_REC* arg flag: the part may already hold a recording (replace it) */

typedef enum {
    RC_PLAY = 1,   /* arg unused */
    RC_STOP = 2,   /* back to the song start (stock reloads the song) */
    RC_PAUSE = 3,  /* stop where it is (stock state 0) */
    RC_SELECT = 4, /* arg = song 0-9 */
    RC_PART = 5,   /* arg = 0 part 1, 1 part 2, 0x7F both (IntSongPart values) */
    RC_TEMPO = 6,  /* arg = bpm */
    RC_SEEK = 7,   /* arg = bar, 1-based */
    /* R2 (rc_enable_rec): arg = part 0 / 1 or RC_REC_KEEP, | RC_REC_OVERWRITE */
    RC_REC = 8,          /* arm: rec standby with metronome (stock 3) */
    RC_REC_NOMETRO = 9,  /* arm: rec standby, metronome off (stock 5) */
    RC_COUNT_IN = 10,    /* arm: count-in (stock 6) */
    RC_REC_START = 11    /* start recording now (stock 4, ring FA) - only when armed; arg unused */
    /* finishing a recording = RC_STOP (or RC_PAUSE) in state 4: ring FC -> the stock commit */
} rc_cmd_t;

/* rc_cmd() results */
enum {
    RC_OK = 0,        /* handed to the stock firmware; rc_poll() reports the outcome */
    RC_NOP = 1,       /* already so: nothing handed over */
    RC_E_ARG = 2,     /* bad argument (song >= 10, part, tempo range, bar outside the song) */
    RC_E_BUSY = 3,    /* song table not safe now (file op / NOR job / worker / table check failed) or
                         the Music queue cannot take it (deferred mode, nearly full) */
    RC_E_REC = 4,     /* recorder armed / counting in / recording: R2's decision, refused in R1 */
    RC_E_PENDING = 5, /* the previous command has not reached its state yet */
    RC_E_NODATA = 6,  /* nothing to play: empty song / part, no song tempo, no bars */
    RC_E_MODE = 7,    /* another player owns MusicMode (3 USB, 4-6): not the internal recorder */
    RC_E_STATE = 8,   /* recorder state not understood (7, 8, ...): never acted on */
    /* 9 is the cave's RCC_NOT_BOUND */
    RC_E_WRONG = 10,  /* R2: not from this state (start before arming, stop during count-in) */
    RC_E_EXISTS = 11  /* R2: the part already holds a recording and RC_REC_OVERWRITE was not given */
};

/* rc_poll() events: (RC_EV_* << 8) | command */
#define RC_EV_NONE 0u
#define RC_EV_DONE 1u     /* the view shows the command's effect */
#define RC_EV_TIMEOUT 2u  /* RC_TIMEOUT_POLLS passed without it (queue emptied, mode refused it ...) */

/* the stock side, as the port binds it (cave: the stock routines above, task 19) */
typedef struct {
    void (*post)(void *ctx, uint8_t kind, uint8_t value);  /* FUN_4015D70C */
    uint16_t (*select)(void *ctx, uint8_t song);           /* SongSelect apply tail, returns rc */
    uint16_t (*part)(void *ctx, uint8_t v);                /* IntSongPart apply tail, returns rc */
    void (*tempo)(void *ctx, uint16_t bpm);                /* FUN_40167294 */
    void (*seek)(void *ctx, uint16_t bar);                 /* FUN_40169448(bar, 0, block) */
    void *ctx;
} rc_ops_t;

typedef struct {
    const rc_ops_t *ops;
    uint8_t pend;       /* command waiting for its effect (0 = none) */
    uint16_t want;      /* the value the view must show (state / song / selector / tempo / bar) */
    uint16_t left;      /* polls left */
    uint8_t last_rc;    /* last stock return code of select / part (0xEC = no data) */
    uint16_t n_ok, n_refused, n_timeout;
    /* R2 */
    uint8_t rec;        /* record transports offered (rc_enable_rec) */
    uint8_t fc_out;     /* a finish (Music 2 -> ring FC) was posted and state 4 has not been left yet */
    uint8_t defer;      /* a finish accepted but not posted yet (NOR job / worker / file op / ring high) */
    uint16_t n_finish;  /* finishes posted */
} rc_t;

void rc_init(rc_t *rc, const rc_ops_t *ops);
/* R2: offer the record transports (0 = R1: every recorder state 3-6 refuses with RC_E_REC) */
void rc_enable_rec(rc_t *rc, uint8_t on);

/* decide and hand over one command.  v = a fresh view (live fields read just before; the song
 * table as kept by the caller's poll).  Returns RC_OK / RC_NOP / RC_E_*. */
uint8_t rc_cmd(rc_t *rc, const rv_view_t *v, uint8_t cmd, uint16_t arg);

/* every 2 ms with the current view: RC_EV_DONE / RC_EV_TIMEOUT once per command, else 0.  R2: also posts a deferred
 * finish once nothing blocks it, and forgets fc_out when state 4 is left (or the FC was lost: timeout in 4 with the
 * Music queue drained) */
uint16_t rc_poll(rc_t *rc, const rv_view_t *v);

/* the play-part selector 0x4191EF72 an IntSongPart value leads to (FUN_4016C9B8: 7F -> 0, p -> p+1) */
uint8_t rc_play_sel_of(uint8_t part);

#endif
