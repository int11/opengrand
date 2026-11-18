/* OpenGrand — the instrument: wires every subsystem together.
 *
 *   platform ISRs ──> piano_key_edge / piano_pedal / piano_midi_in
 *   1 ms tick     ──> piano_tick
 *   parameters    ──> piano_apply (from kproto, startup settings, factory reset)
 *
 * The platform supplies the engine backend, MIDI transmit and flash. */
#ifndef CA79NG_PIANO_H
#define CA79NG_PIANO_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/bt.h"
#include "ca79ng/concert.h"
#include "ca79ng/fs.h"
#include "ca79ng/keyboard.h"
#include "ca79ng/kproto.h"
#include "ca79ng/midi.h"
#include "ca79ng/power.h"
#include "ca79ng/recorder.h"
#include "ca79ng/rhythm.h"
#include "ca79ng/router.h"
#include "ca79ng/sound.h"
#include "ca79ng/standby.h"
#include "ca79ng/storage.h"
#include "ca79ng/system.h"

#define CA79NG_VERSION "1.0.0"

typedef struct {
    void *ctx;
    void (*midi_tx)(void *ctx, midi_port_t port, const uint8_t *msg, uint16_t len);
    void (*power_off)(void *ctx);
    void (*metronome_click)(void *ctx, bool accent, uint8_t volume); /* rendered by the engine */
    /* RecorderData partition: internal song `song` (0..9) as SMF; len 0 = erased */
    void (*save_song)(void *ctx, uint8_t song, const uint8_t *smf, uint32_t len);
    /* reads song `song` back at boot; returns its length, or <= 0 if none */
    int32_t (*load_song)(void *ctx, uint8_t song, uint8_t *buf, uint32_t cap);
    /* scratch buffer for SMF export / import (persistence, USB); NULL disables both */
    uint8_t *smf_buf;
    uint32_t smf_cap;
    const hal_fs_t *usb;       /* USB memory, NULL = none */
    const char *dsp_version;   /* Information: tone generator / DSP firmware */
    uint32_t fw_checksum;      /* Information: main program checksum */
    /* partial standby HAL (amp, mute, BT audio, codec, headphones, GP3[8] alarm, flash busy);
     * NULL = none. The instrument wraps it: its own cleanup runs first, the notice also goes
     * out as 0x7D 59 7F, and the real off always uses power_off above (not ops.power_off). */
    const standby_ops_t *standby;
} piano_platform_t;

/* Sound Preview (SoundPreviewStart/Stop): a short original phrase */
typedef struct {
    uint16_t at_ms;
    uint8_t kind; /* 0 note on (a = note, b = velocity), 1 note off (a), 2 damper (a = value), 3 end */
    uint8_t a, b;
} preview_step_t;

typedef struct {
    bool active;
    uint8_t target;
    const preview_step_t *steps;
    uint16_t i;
    uint32_t t_ms;
    uint8_t volume; /* SoundPreviewVolume 0..127 */
} preview_t;

/* User Virtual Technician data (working copy; the saved copy is in kvstore) */
typedef struct {
    int8_t voicing[128];     /* -5..+5 per MIDI note */
    int8_t key_volume[128];  /* -50..+50 per MIDI note */
    int8_t tuning[128];      /* cents -50..+50 per MIDI note */
    int8_t temperament[12];  /* cents -50..+50 per pitch class */
    uint8_t curve[128];      /* user touch curve: output velocity per input velocity */
    uint8_t curve_exist[16]; /* operation points (bit per input velocity) */
    char curve_name[17];
    bool learning;           /* TouchCurveLearn running */
    uint16_t learn_n;
    uint8_t learn_min, learn_max;
    uint32_t learn_sum;
} vt_user_t;

typedef struct {
    piano_platform_t plat;
    sound_engine_t *engine;
    kvstore_t *kv;

    param_store_t store;
    kproto_t kp;
    router_t router;
    midi_parser_t parser[MIDI_PORT_COUNT];

    key_scanner_t scanner;
    touch_config_t touch;
    pedal_config_t pedals;
    uint8_t pedal_cc[PEDAL_COUNT];
    kb_layout_t layout;
    tuning_t tuning;
    voice_pool_t voices;
    int8_t key_sounding[KB_KEYS][2]; /* sounding note per route, for note-off */
    uint8_t key_part[KB_KEYS][2];

    metronome_t metro;
    drum_pattern_t pattern; /* current rhythm pattern (Rhythm mode) */
    recorder_t rec;
    power_timer_t power;
    bt_audio_t bt;          /* BT audio module activity (iWRAP lines + stock link flag) */
    apo_hold_t apo_hold;    /* APO held while BT audio streams (ca79ng/power.h) */
    power_policy_t policy;  /* user power policy (kvstore KV_KEY_POWER_POLICY, 0x7D op 0x18) */
    standby_t sb;           /* partial standby (APO expiry / 0x7D op 0x19) */
    bool off_notified;      /* 7E 00 00 already sent by kproto (remote off) */
    uint32_t progress_ms; /* throttles MusicProgress notifications */
    uint32_t now_ms;
    bool powered;
    bool booting;

    /* VT */
    uint8_t hammer_delay;              /* 0..10 */
    uint8_t hammer_wait[KB_KEYS];      /* ms until the delayed note-on, 0 = none */
    uint8_t hammer_vel[KB_KEYS];
    vt_user_t vt;
    bool user_voicing;                 /* Voicing = User */

    /* songs: playback parts (SMF channels / multi-timbral receive) */
    part_state_t midi_part[16];
    uint8_t music_mode;                /* MusicMode 0..6 */
    int8_t song_transpose;             /* -12..+12 */
    uint16_t music_bpm;                /* MusicTempo, 0 = song tempo */
    concert_t cm;

    /* USB memory */
    bool usb_mounted;
    int32_t usb_selected;              /* file list id loaded into REC_SLOT_EXT, -1 = none */
    uint32_t usb_poll_ms;
    uint8_t *fa_buf;                   /* file being streamed to a controller */

    /* system */
    blemidi_t ble;
    uint8_t lock;                      /* LockMode bit0 Panel, bit1 Volume */
    uint8_t master_volume;
    preview_t preview;
} piano_t;

/* Lock mode bits (LockMode [mode, 0]: 0 Off, 1 Panel, 2 Volume, 3 Panel+Volume) */
#define LOCK_PANEL 0x01
#define LOCK_VOLUME 0x02
/* Music modes (MusicMode enum) */
enum { MM_EXIT = 0, MM_DEMO = 1, MM_INTERNAL = 2, MM_USB_REC = 3, MM_LESSON = 4, MM_CONCERT = 5, MM_PIANO_MUSIC = 6 };

void piano_init(piano_t *p, const piano_platform_t *plat, sound_engine_t *engine, kvstore_t *kv,
                rec_event_t *rec_pool, rec_event_t *rec_stage);
/* Physical keyboard */
void piano_key_edge(piano_t *p, uint8_t key, uint8_t sensor, bool closed, uint32_t t_us);
void piano_pedal(piano_t *p, pedal_id_t id, uint16_t adc);
/* MIDI bytes received on a port */
void piano_midi_in(piano_t *p, midi_port_t port, const uint8_t *bytes, uint16_t len);
void piano_tick(piano_t *p, uint32_t elapsed_ms);
/* Apply one stored parameter to the running instrument. */
void piano_apply(piano_t *p, param_id_t id, int slot);
void piano_apply_all(piano_t *p);
void piano_factory_reset(piano_t *p);
/* Master volume slider (0..127); ignored while Volume lock is on. */
void piano_master_volume(piano_t *p, uint8_t value);
/* BT audio module (UART1): one iWRAP line (CR/LF stripped), and the stock A2DP link
 * flag && module power. They feed the APO hold rule (ca79ng/power.h). */
void piano_bt_line(piano_t *p, const char *line, uint16_t len);
void piano_bt_link(piano_t *p, bool up);
/* APO hold policy (OpenGrand setting; default apo_policy_default: 5 h cap, streaming only).
 * Runtime only — piano_set_power_policy is the persisted setting that includes it. */
void piano_set_apo_policy(piano_t *p, const apo_policy_t *pol);
/* The whole power policy (hold rule, APO -> standby, standby cap). save = write it to the
 * kvstore. Returns false if saving was asked and failed (the policy is applied anyway). */
bool piano_set_power_policy(piano_t *p, const power_policy_t *pol, bool save);
/* Partial standby: enter (from ON), wake (from standby), real off (power button held,
 * remote off; deferred while the flash is busy). */
bool piano_standby_enter(piano_t *p);
bool piano_standby_wake(piano_t *p, standby_wake_t src);
/* Remote off (0x7D op 19 03, feature 2): where an APO expiry goes (policy: standby or the real off), refused while
 * recording or while a flash job is pending.  Returns POWER_OFF_* (power.h) - 0 = done; *bad_state = not ON. */
uint8_t piano_remote_off(piano_t *p, bool *bad_state);
void piano_hard_off(piano_t *p);

#endif
