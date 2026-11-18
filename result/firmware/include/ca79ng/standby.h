/* OpenGrand — partial standby state machine (pure core module, not bound yet).
 *
 * Feature 5 of the plan (원격 켜기 = 부분 대기): instead of
 * switching off, the piano silences everything that can be switched back at run time
 * (speaker amp GP6[15], output mute GP6[11], BT audio module, optionally the codec)
 * and keeps the SoC, BLE-MIDI and the key scan alive so it can be woken up.
 * Sequences: the stock's standby / wake order (see project notes).
 *
 *     enter (BLE 0x7D op / APO / ...)          wake (BLE 0x7D op, panel touch 32 51 39
 *   ON ───────────────────────────▶ STANDBY ──▶ WAKING ──▶ ON     on port 3, power button, key,
 *                                                                 BT STREAMING START if keep_bt)
 *    │                                 │ hard cap / hard_off()
 *    └──────── hard_off() ─────────────┴──▶ OFF_PENDING ──(flash idle)──▶ OFF (power_off)
 *
 * Rules kept here (not left to the port):
 *   - nothing is ever sent to the panel by this module; the notice goes through
 *     ops.notify, which the port maps to a new 0x7D message — never 7E 00 00 (the
 *     panel would block its settings save);
 *   - power is never cut while ops.flash_busy() says a flash write job is pending
 *     (stock FUN_401342AC() == 1 or busy byte 0x4139DAC4);
 *   - only explicit events wake (standby_wake / standby_midi_wake): periodic panel or
 *     app traffic, which resets the stock APO counter, does not;
 *   - an APO expiry while in standby is ignored — the hard cap decides.
 *
 * Delays are not slept here: standby_tick() advances WAITING steps, so the core can
 * run from a task that must not block. The port calls each op from task context. */
#ifndef CA79NG_STANDBY_H
#define CA79NG_STANDBY_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/midi.h"

typedef enum {
    SB_ON = 0,
    SB_STANDBY,
    SB_WAKING,      /* unmuted, waiting before the amp goes back on */
    SB_OFF_PENDING, /* power-off wanted, a flash write is still pending */
    SB_OFF,
} standby_state_t;

typedef enum {
    SB_WAKE_BLE_OP = 1,     /* 0x7D wake op on any port */
    SB_WAKE_PANEL_TOUCH,    /* 32 51 39 PowerManagementTimerReset from the panel (port 3) */
    SB_WAKE_POWER_BUTTON,   /* short press */
    SB_WAKE_KEY,            /* key press (the first note is not heard: still muted) */
    SB_WAKE_BT_STREAM,      /* A2DP STREAMING START while the BT audio module was kept on ("TV wake") */
} standby_wake_t;

typedef enum { SB_NOTE_STANDBY = 1, SB_NOTE_AWAKE } standby_note_t;

/* HAL hooks — host stubs in ports/sim/hal_power_sim.c (hal.h), cave binding at Phase 3-3. */
typedef struct {
    void *ctx;
    void (*cleanup)(void *ctx);                /* stop playback, close the recording (FUN_400B0EA8) */
    void (*amp)(void *ctx, bool on);           /* speaker amp GP6[15]: FUN_400ABCAC off / FUN_400ACCE8 on */
    void (*mute)(void *ctx, bool on);          /* output mute GP6[11]: FUN_400ABCC0 / FUN_400ACCFC */
    bool (*bt_audio_enabled)(void *ctx);       /* BluetoothAudio setting (config +0x1C) */
    void (*bt_audio)(void *ctx, bool on);      /* FUN_400AD030 / FUN_400AD098 + FUN_40146810 */
    void (*codec)(void *ctx, bool on);         /* optional stage 2: FUN_400ABCD4+ABCEC / ...AD730 (~0.7 s) */
    bool (*headphones)(void *ctx);             /* re-read and apply (FUN_400ACC7C), true = plugged */
    bool (*amp_alarm)(void *ctx);              /* GP3[8] alarm sample 0x4139E7FF == 1 */
    bool (*flash_busy)(void *ctx);             /* a flash write job is pending */
    void (*notify)(void *ctx, standby_note_t n); /* 0x7D notice to controllers, never 7E 00 00 */
    void (*power_off)(void *ctx);              /* real power-off (stock executor S=2) */
} standby_ops_t;

#define STANDBY_CAP_DEFAULT_MS (8u * 60u * 60u * 1000u) /* 8 h, owner decision */
#define STANDBY_WAKE_DELAY_MS 100u          /* stage 1: step 7 */
#define STANDBY_WAKE_DELAY_CODEC_MS 500u    /* stage 2 (codec was off) */

typedef struct {
    standby_ops_t ops;
    uint32_t cap_ms;        /* standby length before the real power-off, 0 = no cap */
    bool deep;              /* stage 2: codec power-down too */
    bool keep_bt;           /* the BT audio module stays on in standby; a new STREAMING START wakes */

    standby_state_t state;
    uint32_t t_ms;          /* time in the current state */
    bool bt_was_on;         /* BT audio was switched off by enter */
    bool codec_off;
    bool alarm_skip;        /* woke during a GP3[8] alarm: amp/mute left to the stock event */
    uint32_t busy_wait_ms;  /* how long OFF_PENDING has waited for the flash */
    standby_wake_t last_wake;
    uint32_t bt_starts;     /* STREAMING START count at the last standby_bt_stream() */
} standby_t;

void standby_init(standby_t *s, const standby_ops_t *ops, uint32_t cap_ms, bool deep);
/* ON -> STANDBY. Returns false if not ON. */
bool standby_enter(standby_t *s);
/* STANDBY -> WAKING (steps 1-6). Returns false if not in standby. */
bool standby_wake(standby_t *s, standby_wake_t src);
/* Classifies one complete MIDI message received during standby and wakes on
 * 32 51 39 from the panel port (other traffic never wakes). Returns true if it woke. */
bool standby_midi_wake(standby_t *s, midi_port_t port, const uint8_t *msg, uint16_t len);
/* TV wake: call every pass with bt_audio_t.starts. A START
 * counted since the previous call wakes a standby entered with keep_bt (the module stayed on);
 * a link alone (CONNECT, a TV that keeps it open while off) never wakes, nor a START counted
 * before the standby began. Returns true if it woke. */
bool standby_bt_stream(standby_t *s, uint32_t starts);
/* Real power-off from ON or STANDBY (power button held, remote off, cap), deferred
 * while the flash is busy. */
void standby_hard_off(standby_t *s);
/* Advances WAKING (steps 7-10), the hard cap and OFF_PENDING. */
void standby_tick(standby_t *s, uint32_t elapsed_ms);
/* True while the stock APO must not decide (standby or waking). */
bool standby_active(const standby_t *s);

#endif
