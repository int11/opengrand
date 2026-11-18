/* OpenGrand — Concert Magic: each key press plays the next note / chord group
 * of a song with the key's velocity.
 *
 * Behaviour follows the original app (its playConcertMagic): the tap interval gives a tempo estimate that is
 * blended depending on the mode and clamped to 0.5..1.5 of the song tempo;
 * each tap returns OK / SLOW / FAST / TOOFAST / CHORD / NG. The exact
 * thresholds and blend weights of the original were not recovered; the
 * values below are OpenGrand choices (REMOTE_PROTOCOL.md §9).
 *
 * Trigger notes are the note-ons of the trigger part (internal songs: Part1
 * or Part2) or trigger channel (SMF songs). Note-ons of the trigger within
 * CM_CHORD_MS of the group's first note form one group. In Ensemble mode the
 * other parts follow on the song clock at the tapped tempo and wait at the
 * next group; in Melody mode only the trigger part sounds. */
#ifndef CA79NG_CONCERT_H
#define CA79NG_CONCERT_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/recorder.h"

#define CM_CHORD_MS 40u
#define CM_GROUP_MAX 10
#define CM_KEYS 128

typedef enum { CM_OK = 0, CM_SLOW = 1, CM_FAST = 2, CM_TOOFAST = 3, CM_CHORD = 4, CM_NG = 5 } cm_result_t;
typedef enum { CM_MODE_MELODY = 0, CM_MODE_ENSEMBLE = 1 } cm_mode_t;

/* emit one song event with the given velocity (note-on) or 0 (note-off) */
typedef void (*cm_emit_fn)(void *ctx, const rec_event_t *e, uint8_t velocity);

typedef struct {
    uint8_t n;
    rec_event_t ev[CM_GROUP_MAX];
} cm_held_t;

typedef struct {
    bool active;
    uint8_t mode;          /* cm_mode_t */
    bool smooth;           /* extra smoothing of the tempo estimate */
    uint8_t trigger;       /* 0 = Part1 / channel 1, 1 = Part2 / channel 2 */
    bool ext;              /* song events carry MIDI channels */
    uint32_t next;         /* index of the next event to scan */
    uint32_t group_ms;     /* onset of the last played group */
    uint32_t next_ms;      /* onset of the next group (UINT32_MAX = none) */
    uint32_t last_tap_ms;
    bool tapped;
    uint32_t ratio_q16;    /* tempo estimate: song ms per real ms */
    uint32_t groups;       /* groups played */
    cm_result_t last;
    cm_held_t held[CM_KEYS]; /* notes started by each physical key */
} concert_t;

void cm_start(concert_t *c, const rec_event_t *ev, uint32_t count, bool ext);
void cm_stop(concert_t *c);
bool cm_is_trigger(const concert_t *c, const rec_event_t *e);
/* key pressed at now_ms: plays the next group, returns the result code */
cm_result_t cm_key_on(concert_t *c, const rec_event_t *ev, uint32_t count, uint8_t key, uint8_t velocity,
                      uint32_t now_ms, cm_emit_fn emit, void *ctx);
void cm_key_off(concert_t *c, uint8_t key, cm_emit_fn emit, void *ctx);

#endif
