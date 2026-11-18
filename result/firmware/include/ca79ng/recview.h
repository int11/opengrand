/* OpenGrand — read-only view of the STOCK recorder / song player (Phase 3-6 stage R0).
 *
 * The core never writes these addresses: it reads the stock recorder state, the song-slot
 * table of the RecorderData RAM mirror (0x40500000, NOR 0x60500000) and the NOR job / file
 * job flags, so later stages (R1 play control, R2 record control, R3 save decision) can judge
 * from the same picture the stock firmware acts on.  Every address below is from REA
 * (Ghidra 12.1.4) on CAx9_P06.SYS:
 *
 *   0x4191EF79 u8  recorder state (getter FUN_40167824): 0 just stopped, 1 play, 2 stop (song
 *                  loaded), 3 rec standby, 4 recording, 5 rec standby with metronome off
 *                  (FUN_40169F00), 6 count-in (FUN_40169EDC); 7/8 never written by the
 *                  recorder commands (FUN_401693CC refuses them) [unconfirmed]
 *   0x4191EF77 u8  selected song 0-9 (getter FUN_40167A40)
 *   0x4191EF73 u8  record track = part (getter FUN_40167A4C, set only < 2 by FUN_4016A1E4)
 *   0x4191EF72 u8  play part selector: 0 both, 1 part 1, 2 part 2 (FUN_401686FC, IntSongPart)
 *   0x419156A6 u8  MusicMode (getter FUN_4015C244)
 *   0x4191570C u16 song tempo bpm, 0x4191570E u16 the song's original tempo
 *   0x4191EF58 u32 song position in 96-per-quarter ticks (play compares each track to it,
 *                  FUN_40166280; record time += n in task 20)
 *   0x4191EF70 u16 bar count of the longest track, 0x4191EF76 u8 its slot (0xFF = none)
 *                  (loader FUN_401674FC)
 *   0x4191E054 + slot*0x54: per-track play state; +0x24 u16 = current bar (FUN_40166280)
 *   0x4191EF6C u16 record ring fill (FA/FC writers refuse at >= 0x1000, FUN_40169F74)
 *   0x4191EF6E u16 song progress bar, 1-based (FUN_40165E84 - loader, seek FUN_40169448, play
 *                  progress; the bar the stock reports in MusicProgress 32 52 03) - R1
 *   0x419156A5 u8  Music/MusicMode queue count, 0x419156A4 u8 deferred MusicMode (non-zero:
 *                  FUN_4015DBD4 drops every Music command) - R1
 *   0x4139DAAC/+8  NOR job table slot 0/1 "pending" bytes (FUN_401342AC = either set; the
 *                  worker FUN_401340B0 clears them after programming); slot 1 = RecorderData
 *   0x4139DAC4 u8  the worker is erasing / programming (FUN_401340B0)
 *   0x413CA3EA u8  file-executor operation kind, non-zero until FUN_4014F364 finishes (KSO
 *                  import / USB song load use 0x40A00000, the same buffer as a recording commit)
 *
 * Reading never takes a lock: a field can be one task-20 tick old, and the song table can be
 * mid-move while FUN_40168408 runs (task 19/20).  rv_read_store() therefore re-checks the table
 * with the boot check's rules (FUN_4016E174) and reports a torn / invalid table instead of
 * trusting it; callers re-read the table only when a commit can have happened (see rv_diff). */
#ifndef CA79NG_RECVIEW_H
#define CA79NG_RECVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RV_SONGS 10
#define RV_TRACKS 16

/* stock addresses (VA = 0x40060000 + file offset for code; these are RAM) */
#define RV_A_STATE      0x4191EF79u
#define RV_A_SONG       0x4191EF77u
#define RV_A_REC_PART   0x4191EF73u
#define RV_A_PLAY_SEL   0x4191EF72u
#define RV_A_MUSIC_MODE 0x419156A6u
#define RV_A_TEMPO      0x4191570Cu
#define RV_A_TEMPO_ORIG 0x4191570Eu
#define RV_A_POS        0x4191EF58u
#define RV_A_MAX_BARS   0x4191EF70u
#define RV_A_MAX_SLOT   0x4191EF76u
#define RV_A_TRACKS     0x4191E054u
#define RV_TRACK_SIZE   0x54u
#define RV_TRACK_BAR    0x24u
#define RV_A_RING_COUNT 0x4191EF6Cu
#define RV_A_BAR        0x4191EF6Eu
#define RV_A_MQ_COUNT   0x419156A5u
#define RV_A_MQ_HOLD    0x419156A4u
#define RV_A_JOB0       0x4139DAACu
#define RV_A_JOB1       0x4139DAB4u
#define RV_A_WORKER     0x4139DAC4u
#define RV_A_FILEOP     0x413CA3EAu
#define RV_A_STORE      0x40500000u
#define RV_STORE_DATA   0x30u      /* first song may start here */
#define RV_STORE_LIMIT  0xFFFFEu   /* area-end marker 5A A5 sits here */

/* raw stock state values */
enum {
    RV_ST_STOPPED0 = 0, RV_ST_PLAY = 1, RV_ST_STOP = 2, RV_ST_REC_STANDBY = 3, RV_ST_RECORDING = 4,
    RV_ST_REC_STANDBY_NOMETRO = 5, RV_ST_COUNT_IN = 6
};

/* what a controller is told */
typedef enum {
    RV_T_IDLE = 0,        /* 0 / 2 */
    RV_T_PLAYING = 1,     /* 1 */
    RV_T_REC_ARMED = 2,   /* 3 / 5: waiting for the first key or Rec */
    RV_T_COUNT_IN = 3,    /* 6 */
    RV_T_RECORDING = 4,   /* 4 */
    RV_T_UNKNOWN = 0xF    /* 7, 8, anything else: reported raw, never acted on */
} rv_transport_t;

/* busy bits */
#define RV_BUSY_JOB    0x01u   /* FUN_401342AC: a NOR job is queued or running */
#define RV_BUSY_WORKER 0x02u   /* the NOR worker is erasing / programming now */
#define RV_BUSY_FILEOP 0x04u   /* a file-executor operation (KSO / USB) is pending or running */
#define RV_BUSY_REC    0x08u   /* recording, armed or counting in: a commit is still to come */

/* song flags */
#define RV_SONG_PART1     0x01u /* track slot 0 present (part 1) */
#define RV_SONG_PART2     0x02u /* track slot 1 present (part 2) */
#define RV_SONG_EXTRA     0x04u /* some track slot 2-15 present (only a KSO import fills these) */
#define RV_SONG_SETTINGS  0x08u /* +0x24 start settings block present */
#define RV_SONG_SOUNDMODE 0x10u /* name +8..+0x1F == "- SoundMode" (FUN_40165734's test) */

/* store check result (0 = passes the boot check FUN_4016E174 and our structure rules) */
enum {
    RV_STORE_OK = 0,
    RV_STORE_BLANK = 1,      /* first word 0xFFFFFFFF: the boot path would format */
    RV_STORE_SONG_RANGE = 2, /* a song start outside [0x30, 0xFFFFE) */
    RV_STORE_SONG_ALIGN = 3, /* a song start not 4-aligned */
    RV_STORE_TRACK_RANGE = 4,/* a track offset outside the area */
    RV_STORE_TRACK_FA = 5,   /* a track does not start with FA */
    RV_STORE_ORDER = 6,      /* song starts not ascending / end word wrong (ours, not the boot check's) */
    RV_STORE_MARKER = 7      /* no 5A A5 at off[10] or at 0xFFFFE (ours) */
};

typedef struct {
    uint32_t len;     /* bytes, off[i+1] - off[i] (off[10] for song 10) */
    uint16_t bars;    /* header +0x70: bar count of the longest track */
    uint8_t flags;    /* RV_SONG_* */
} rv_song_t;

typedef struct {
    /* live (task-20 owned) */
    uint8_t state;        /* raw 0x4191EF79 */
    uint8_t transport;    /* rv_transport_t */
    uint8_t song;
    uint8_t rec_part;
    uint8_t play_sel;
    uint8_t music_mode;
    uint16_t tempo;
    uint16_t tempo_orig;
    uint32_t pos;         /* ticks, 96 per quarter */
    uint16_t max_bars;
    uint8_t max_slot;
    uint16_t cur_bar;     /* bar counter of the max slot's track, 0 if none */
    uint16_t ring;
    uint16_t bar;         /* song progress bar 0x4191EF6E (1-based) */
    uint8_t mq_count;     /* Music queue entries waiting for task 20 */
    uint8_t mq_hold;      /* deferred MusicMode: Music commands are dropped while non-zero */
    uint8_t busy;         /* RV_BUSY_* */
    /* song table (RecorderData mirror) */
    uint8_t store;        /* RV_STORE_* */
    uint8_t store_bad;    /* song (0-9) or song*16+track the check stopped at */
    uint32_t store_end;   /* off[10] */
    rv_song_t songs[RV_SONGS];
} rv_view_t;

/* memory access: copy n bytes at stock address addr into dst (the cave: plain loads) */
typedef void (*rv_read_fn)(void *ctx, uint32_t addr, void *dst, uint32_t n);

typedef struct {
    rv_read_fn read;
    void *ctx;
} rv_mem_t;

uint8_t rv_transport(uint8_t stock_state);
void rv_read_live(const rv_mem_t *m, rv_view_t *v);
void rv_read_store(const rv_mem_t *m, rv_view_t *v);
void rv_read(const rv_mem_t *m, rv_view_t *v);   /* live + store */

/* what changed between two views (bits below); RV_CH_COMMIT = a point after which the song
 * table can differ (recording ended, a NOR job finished, a file operation finished) - the cue
 * to call rv_read_store() again */
#define RV_CH_TRANSPORT 0x001u
#define RV_CH_SONG      0x002u
#define RV_CH_PART      0x004u
#define RV_CH_TEMPO     0x008u
#define RV_CH_BAR       0x010u
#define RV_CH_MAXTIME   0x020u
#define RV_CH_BUSY      0x040u
#define RV_CH_STORE     0x080u
#define RV_CH_MODE      0x100u
#define RV_CH_COMMIT    0x200u
uint32_t rv_diff(const rv_view_t *old, const rv_view_t *cur);

/* fixed little-endian wire / test layout, RV_ENC_LEN bytes; returns bytes written or 0.
 * Bytes 29-30 = bar (R1; were zero padding in R0), the queue fields are not encoded. */
#define RV_ENC_VERSION 1
#define RV_ENC_LEN (32 + 7 * RV_SONGS)
size_t rv_encode(const rv_view_t *v, uint8_t *out, size_t cap);

#endif
