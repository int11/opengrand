/* OpenGrand — internal song recorder and Standard MIDI File I/O.
 *
 * Requirements (manual): 10 songs x 2 parts, ~90,000 notes total, overdub,
 * playback with A-B repeat, save as SMF (USB). Wire controls (app protocol):
 * Music 52 08 {Pause, Play, Reset, RecStandby, Rec, ...}, SongSelect 52 00,
 * IntSongPart 52 01, MusicProgress 52 03, ABRepeat 52 06, RepeatPointA/B 52 04/05.
 *
 * Events are stored with millisecond timestamps (like the original app's MIDI
 * recorder "recordAddMsg"), converted to ticks only
 * when exporting SMF. */
#ifndef CA79NG_RECORDER_H
#define CA79NG_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef REC_MAX_EVENTS
#define REC_MAX_EVENTS 200000u /* ~ 90k notes (on+off) + pedals, shared by all songs */
#endif
#define REC_SONGS 10
#define REC_PARTS 2
/* One extra slot (index REC_SLOT_EXT) holds a song loaded from USB memory
 * for playback / Lesson / Concert Magic; it is never persisted. */
#define REC_SLOTS (REC_SONGS + 1)
#define REC_SLOT_EXT REC_SONGS

typedef struct {
    uint32_t ms;
    uint8_t status, d1, d2; /* status keeps the MIDI channel for SMF-loaded songs */
    uint8_t part; /* 0/1: Part1 / Part2 (SMF: channel 2 -> Part2, others Part1) */
} rec_event_t;

typedef enum { REC_IDLE, REC_STANDBY, REC_RECORDING, REC_PLAYING, REC_PAUSED } rec_state_t;

typedef struct {
    uint32_t first;   /* index into the shared pool */
    uint32_t count;
    uint32_t length_ms;
    uint16_t bpm;
    uint8_t numerator, denominator;
    bool ext;         /* loaded from a file: events play as MIDI channels, not keys */
} rec_song_t;

typedef void (*rec_out_fn)(void *ctx, uint8_t part, uint8_t status, uint8_t d1, uint8_t d2);

typedef struct {
    rec_event_t *pool;      /* REC_MAX_EVENTS entries, provided by the platform */
    uint32_t pool_used;
    rec_song_t song[REC_SLOTS];
    rec_state_t state;
    uint8_t cur_song;
    uint8_t rec_part;       /* part being recorded (0/1) */
    uint8_t play_mask;      /* parts to play (bit0/bit1) */
    uint32_t pos_ms;
    uint32_t next_index;    /* next event to play (relative to song.first) */
    bool ab_on;
    uint32_t a_ms, b_ms;
    /* overdub staging: new part events recorded while the other part plays */
    rec_event_t *stage;
    uint32_t stage_count;
    rec_out_fn out;
    void *out_ctx;
    /* playback tempo: song ms advance per real ms in 16.16 (65536 = original) */
    uint32_t tempo_q16, tempo_frac;
    uint32_t limit_ms;      /* playback does not pass this point (Concert Magic), UINT32_MAX = off */
    uint16_t dirty;         /* bit per slot changed since the last persist */
    uint16_t ch_mask;       /* SMF (ext) songs: channels to play, bit per channel */
} recorder_t;

void recorder_init(recorder_t *r, rec_event_t *pool, rec_event_t *stage, rec_out_fn out, void *ctx);
bool recorder_select(recorder_t *r, uint8_t song);
void recorder_standby(recorder_t *r, uint8_t part, uint16_t bpm, uint8_t num, uint8_t den);
void recorder_start(recorder_t *r);           /* from standby: begins recording (first key also starts it) */
void recorder_stop(recorder_t *r);            /* commits a recording, stops playback */
void recorder_play(recorder_t *r, uint8_t part_mask);
void recorder_pause(recorder_t *r);
void recorder_reset(recorder_t *r);           /* back to the start */
void recorder_seek(recorder_t *r, uint32_t ms);
void recorder_set_ab(recorder_t *r, bool on, uint32_t a_ms, uint32_t b_ms);
bool recorder_erase(recorder_t *r, uint8_t song, int part); /* part -1 = both */
/* Feed a performance event (only stored while recording). */
void recorder_input(recorder_t *r, uint8_t status, uint8_t d1, uint8_t d2);
/* Advance time; emits due playback events through `out`. */
void recorder_tick(recorder_t *r, uint32_t elapsed_ms);
uint32_t recorder_free_events(const recorder_t *r);
/* Playback tempo in BPM (10..500); 0 = the song's own tempo. */
void recorder_set_tempo(recorder_t *r, uint16_t bpm);
const rec_event_t *recorder_events(const recorder_t *r, uint8_t song, uint32_t *count);

/* Bar/beat progress for MusicProgress: [0, bar(14 bit) hi, lo, beat, 0, 0]. */
void recorder_progress(const recorder_t *r, uint32_t *bar, uint32_t *beat);

/* ---- SMF ---- */
/* Writes song `s` as format 1 (one track per part + conductor). Recorded
 * songs use channel = part; SMF-loaded (ext) songs keep their channels.
 * Returns bytes written, 0 if `cap` is too small. */
size_t smf_write(const recorder_t *r, uint8_t song, uint8_t *out, size_t cap);
/* Reads format 0/1 into song `s`: all tracks merged in time order, tempo map
 * (FF 51 from every track) flattened to ms, MIDI channel kept in the status
 * byte, part = 1 for channel 2 and 0 otherwise (Lesson left/right). Returns
 * false on malformed data or a full pool. The caller sets song.ext. */
bool smf_read(recorder_t *r, uint8_t song, const uint8_t *data, size_t len);

#endif
