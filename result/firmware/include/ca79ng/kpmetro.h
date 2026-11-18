/* OpenGrand - Phase 3-5 metronome, stage 1: the core judges, the stock plays.
 *
 * Stage 1 keeps beat generation and sound in the stock song task (task 20, FUN_4016DE5C: click = TG part
 * 0x1BC, rhythm = part 0x1B8) - it shares one tempo accumulator with song play and recording.  What the
 * core owns:
 *
 *   judge    a metronome set (fn 0x10, 56 xx 7F) -> what the stock apply FUN_4016053C does with it:
 *            the settings block bytes +0x49..+0x50 after it and the side effects (restart, rhythm start,
 *            rhythm reload, tempo accumulator, volume).  The rules are the stock's (REA):
 *              56 08 on      +0x49 = d0 (raw), FUN_4016E6BC: d0 != 0 -> start (mode 0 FUN_4016E668,
 *                            else FUN_4016EF68), both send BeatCounter 1; 0 -> stop, nothing sent
 *              56 01 tempo   +0x4E = d0 * 128 + d1 (raw), accumulator FUN_40165F18: outside 10..400 -> 120
 *              56 02 beat    d0 < 13, d1 < 5 and table 0x400FD88A[d1 * 13 + d0] != 0xFF -> +0x4C = index;
 *                            ALWAYS FUN_4016E668 (restart + tempo from +0x4E), also for a refused value
 *              56 09 rhythm  d0 < 100 -> +0x4B = d0 and the reload flag 0x4191F0DC (task 20 then runs
 *                            FUN_4016EF68 once, any mode); else nothing
 *              56 03 volume  +0x50 = d0 (raw), TG volume of parts 0x1BC / 0x1B8
 *              56 0A mode    +0x4A = d0 (raw); d0 != 0 -> FUN_4016EF68, 0 -> FUN_4016E668
 *            a data length other than the descriptor's (1, or 2 for 56 01 / 56 02) is not applied
 *            (the stock still ACKs it).
 *   store    the 5 values as the stock getter FUN_4015E960 answers them (cases 0x39..0x3E): raw bytes,
 *            tempo [v >> 7, v & 0x7F], beat index -> [beats, 0x60 -> 2 / 0x30 -> 3] from the beat table
 *            0x401235B8 (index > 9 -> 3).  Not the set bytes: a refused beat / rhythm keeps the old value.
 *   beat     the BeatCounter notification  F0 40 7F 32 04 1E 56 0B 7F beat 7F F7  (FUN_4014BF24, sent by
 *            the stock unreliably to its binding port, FUN_4014AD04) for every other bound controller:
 *            - from a judged set: start / restart / mode change with the metronome on -> beat 1 at once
 *              (the stock apply's own BeatCounter 1)
 *            - from a 2 ms poll of the stock run state (task 19 pump tail): a new beat (beat-in-bar
 *              changed or the position in the beat went back) -> beat-in-bar; the reload flag falling
 *              (FUN_4016EF68 ran in task 20) with the metronome on -> beat 1
 *            The poll never acts while the recorder counts in (state 6: the stock counts with negative
 *            BeatCounter values from other variables - recorder stage R3) and resynchronises without a
 *            frame when the recorder state changes (record start / stop restart the metronome, R2).
 *
 * Pure logic: the platform reads the stock RAM into kpm_run_t / kpm_cfg_t and sends the frames.  The
 * stock RAM addresses below are what the cave reads (REA literal pools). */
#ifndef CA79NG_KPMETRO_H
#define CA79NG_KPMETRO_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/kproto.h"

/* settings block (ctx block 0x418F19E4, FUN_4015B4D8) +0x49..+0x50 */
#define KPM_A_CFG       0x418F1A2Du   /* on, mode, rhythm, beat index, pad, tempo u16, volume (8 B) */
/* run state (FUN_4016EA18 / FUN_4016E668 literals 0x4016ED3C..0x4016ED50, FUN_4016FBAC 0x4016FC34) */
#define KPM_A_POS       0x4191F006u   /* s16 position in the beat (ticks of 96 per quarter)           */
#define KPM_A_LEN       0x4191F008u   /* s16 beat length (0x60 or 0x30, rhythm data in rhythm mode)    */
#define KPM_A_HALF      0x4191F00Au   /* s16 half-beat position (FUN_4016E610: length / 2)             */
#define KPM_A_BEAT      0x4191F00Eu   /* s8  beat in bar (BeatCounter value, FUN_4016E9A0)             */
#define KPM_A_BPB       0x4191F00Fu   /* u8  beats per bar                                             */
#define KPM_A_ON        0x4191F010u   /* u8  running (FUN_4016E964)                                    */
#define KPM_A_RELOAD    0x4191F0DCu   /* u8  rhythm reload flag (FUN_4016FBAC sets, FUN_4016FBC0 clears) */
#define KPM_A_REC_STATE 0x4191EF79u   /* u8  recorder state (FUN_40167824), 6 = count-in               */

#define KPM_FRAME_LEN 12u   /* BeatCounter frame */
#define KPM_BEATS 10u       /* beat table entries (0x401235B8) */

/* the stock settings block bytes +0x49..+0x50 */
typedef struct {
    uint8_t on, mode, rhythm, beat;   /* beat = index into the beat table */
    uint16_t tempo;                   /* raw, as set (the accumulator clamps) */
    uint8_t volume;
} kpm_cfg_t;

/* the stock run state the poll reads */
typedef struct {
    int16_t pos, len, half;
    int8_t beat;
    uint8_t bpb, on, reload, rec_state;
} kpm_run_t;

/* side effects of a set (what the stock apply runs) */
enum {
    KPM_FX_CHANGED = 0x01,  /* a block byte changed                                         */
    KPM_FX_START = 0x02,    /* FUN_4016E668 (click start / restart, beat counters reset)    */
    KPM_FX_RHYTHM = 0x04,   /* FUN_4016EF68 (rhythm start)                                  */
    KPM_FX_STOP = 0x08,     /* metronome off                                                */
    KPM_FX_TEMPO = 0x10,    /* tempo accumulator rewritten (also moves a playing song)      */
    KPM_FX_VOLUME = 0x20,   /* TG volume of parts 0x1BC / 0x1B8                              */
    KPM_FX_RELOAD = 0x40,   /* rhythm reload flag: task 20 runs FUN_4016EF68 at its next pass */
    KPM_FX_BEAT1 = 0x80     /* the apply itself sends BeatCounter 1 (start / restart with the metronome on) */
};

typedef enum { KPM_NOT_MINE = 0, KPM_IGNORED, KPM_APPLIED } kpm_verdict_t;

typedef struct {
    kpm_verdict_t verdict;
    uint8_t fx;
    kpm_cfg_t cfg;   /* block after the apply (== before when ignored) */
    uint16_t bpm;    /* accumulator tempo when KPM_FX_TEMPO */
} kpm_result_t;

typedef struct {
    uint32_t sets, ignored, refused;   /* refused: applied, but the value kept (bad beat / rhythm >= 100) */
    uint32_t beats, starts, reloads;   /* BeatCounter frames made: poll beats, set starts, reload starts  */
    uint32_t resyncs;                  /* polls that only resynchronised (record state change, count-in)   */
} kpm_stats_t;

typedef struct {
    kpm_cfg_t cfg;
    kpm_run_t last;
    bool have_last;
    bool resync;   /* the next poll takes the state as it is (a judged set jumped it) */
    kpm_stats_t st;
} kpm_t;

void kpm_init(kpm_t *m, const kpm_cfg_t *cfg);

/* a set key this module judges: fn 0x10 56 {01 02 03 08 09 0A} part 0x7F */
bool kpm_is_key(uint8_t v1, uint8_t v2, uint8_t part);

/* what the stock apply does with data d (n bytes) for key 56 v2 7F on settings `cur` (pure) */
kpm_result_t kpm_judge(const kpm_cfg_t *cur, uint8_t v2, const uint8_t *d, uint32_t n);

/* judge + take it: m->cfg follows, the next poll resynchronises.  out gets BeatCounter 1 when the apply
 * sends it (returns its length, 0 = no frame).  Poll once right before (the beats made since the last poll would
 * otherwise be taken as part of the restart and never sent - ports/cave/sysex_cave.c metro_begin). */
uint32_t kpm_set(kpm_t *m, uint8_t v2, const uint8_t *d, uint32_t n, kpm_result_t *r, uint8_t out[KPM_FRAME_LEN]);

/* the 2 ms poll: BeatCounter frames the stock sent since the last poll, out[k] (0..2) */
uint32_t kpm_poll(kpm_t *m, const kpm_run_t *run, uint8_t out[2][KPM_FRAME_LEN]);

/* the getter's value bytes for 56 v2 7F (out[2]), their length; 0 = not a metronome value key */
uint32_t kpm_value(const kpm_cfg_t *c, uint8_t v2, uint8_t out[2]);

/* the 5 values (+ on) into the store, as the getter answers them; number of values that changed */
uint32_t kpm_store(const kpm_cfg_t *c, param_store_t *s);

/* BeatCounter frame for beat b (FUN_4014BF24(0x7F, b, 0x7F)) */
void kpm_beat_frame(uint8_t b, uint8_t out[KPM_FRAME_LEN]);

/* tempo accumulator FUN_40165F18 / FUN_40165ED4: bpm (10..400 else 120), *q = bpm*192 / 60000, *r = rest */
uint16_t kpm_accum(uint16_t tempo, uint32_t *q, uint32_t *r);

/* the ports (bit p) the stock itself sent a BeatCounter to: FUN_4014AD04 binding 1..3 -> port b-1,
 * 4 -> port 3 and, with port-3 forwarding on (FUN_4014794C), ports 0..2; 0 -> none (FUN_4014BF24 sends nothing) */
uint8_t kpm_stock_ports(uint8_t binding, bool forward);

/* beat table 0x401235B8 entry: ticks per beat (0x60 / 0x30) and beats per bar; index > 9 -> 3 (FUN_4016E5FC) */
void kpm_beat_entry(uint8_t index, uint16_t *ticks, uint8_t *beats);

/* BeatSelect [num, den code] -> beat index, -1 = refused (FUN_4014EE08 with table 0x400FD88A) */
int kpm_beat_index(uint8_t num, uint8_t den);

#endif
