/* OpenGrand — keyboard (Grand Feel III triple sensor) and pedals.
 *
 * Requirements:
 *  - 88 keys, three contacts per key (S1 = start of travel, S2 = hammer
 *    strike point, S3 = let-off / bottom). Velocity comes from the S1->S2
 *    travel time; S2 re-trigger without S1 release = fast repetition.
 *  - Touch Curve: Light4..Light1, Normal, Heavy1..Heavy4, Off(constant), User.
 *  - Minimum Touch 1..20, Key Volume presets + per-note volume, Dynamics.
 *  - Pedals: damper (half pedal) / soft (half) / sostenuto, Half-Pedal Adjust
 *    1..10, Soft Pedal Depth 1..10.
 */
#ifndef CA79NG_KEYBOARD_H
#define CA79NG_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#define KB_KEYS 88
#define KB_LOWEST_NOTE 21 /* A0 */

/* Touch curve identifiers use the raw SysEx values (TouchCurve, fn10 50 00). */
typedef enum {
    TC_LIGHT4 = 0x00, TC_NORMAL = 0x01, TC_HEAVY4 = 0x02, TC_OFF = 0x03,
    TC_LIGHT3 = 0x04, TC_HEAVY3 = 0x05, TC_LIGHT2 = 0x06, TC_HEAVY2 = 0x07,
    TC_LIGHT1 = 0x08, TC_HEAVY1 = 0x09, TC_USER = 0x40,
} touch_curve_t;

/* Key Volume presets (KeyVolume, fn10 50 0F). */
typedef enum {
    KV_NORMAL = 0, KV_HIGH_DAMP = 1, KV_LOW_DAMP = 2, KV_HIGH_LOW_DAMP = 3, KV_CENTER_DAMP = 4, KV_USER = 0x40,
} key_volume_t;

#define TC_USER_POINTS 8 /* user curve operation points (input vel -> output vel) */

typedef struct {
    touch_curve_t curve;
    uint8_t constant_velocity;      /* used by TC_OFF */
    uint8_t user_in[TC_USER_POINTS];  /* ascending input velocities */
    uint8_t user_out[TC_USER_POINTS];
    uint8_t user_points;
    uint8_t user_table[128];        /* full user curve from the editor (input -> output velocity) */
    bool user_table_valid;          /* use user_table instead of the operation points */
    uint8_t minimum_touch;          /* 1..20 */
    key_volume_t key_volume;
    int8_t per_note_volume[KB_KEYS]; /* KV_USER: 0.5 dB steps, -20..+20 (= -10..+10 dB) */
    uint8_t dynamics;               /* 0..127 output scaling */
} touch_config_t;

void touch_config_default(touch_config_t *c);
/* Map a raw velocity (1..127, from the scanner) through the full touch chain.
 * Returns 0 when the stroke is below Minimum Touch (no sound). */
uint8_t touch_apply(const touch_config_t *c, int key_index, uint8_t raw_velocity);

/* ---- Triple-sensor scanner ----
 * The HAL reports contact edges with a microsecond timestamp; the scanner
 * turns them into note on/off events. */
typedef enum { KS_IDLE, KS_S1, KS_DOWN, KS_HELD } key_state_t;

typedef struct {
    key_state_t state[KB_KEYS];
    uint32_t t_s1[KB_KEYS];
    uint32_t t_fast_us;  /* travel time that maps to velocity 127 */
    uint32_t t_slow_us;  /* travel time that maps to velocity 1 */
} key_scanner_t;

typedef enum { KEY_EV_NONE, KEY_EV_ON, KEY_EV_OFF } key_event_kind_t;

typedef struct {
    key_event_kind_t kind;
    uint8_t key;       /* 0..87 */
    uint8_t velocity;  /* raw scanner velocity for ON, release velocity for OFF */
} key_event_t;

void key_scanner_init(key_scanner_t *s);
/* sensor: 1..3, closed: contact made (true) or released (false) */
key_event_t key_scanner_edge(key_scanner_t *s, uint8_t key, uint8_t sensor, bool closed, uint32_t t_us);
uint8_t key_scanner_velocity(const key_scanner_t *s, uint32_t travel_us);

/* ---- Pedals ---- */
typedef enum { PEDAL_DAMPER = 0, PEDAL_SOSTENUTO = 1, PEDAL_SOFT = 2, PEDAL_COUNT } pedal_id_t;

typedef struct {
    uint16_t adc_min[PEDAL_COUNT]; /* calibration: released */
    uint16_t adc_max[PEDAL_COUNT]; /* calibration: fully pressed */
    uint8_t half_pedal_adjust;     /* 1..10 (8..10 = MIDI controller modes) */
    uint8_t soft_depth;            /* 1..10 */
    uint8_t last_cc[PEDAL_COUNT];
} pedal_config_t;

void pedal_config_default(pedal_config_t *p);
/* Converts an ADC reading to the 0..127 controller value sent to the engine
 * and MIDI (CC64/66/67). Sostenuto is a switch (0/127). */
uint8_t pedal_value(const pedal_config_t *p, pedal_id_t id, uint16_t adc);

#endif
