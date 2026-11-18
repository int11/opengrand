/* OpenGrand — the instrument's power control: the user power policy (kvstore), what an
 * APO expiry does (partial standby by default, owner decision 2026-10-31) and the glue
 * between piano_t and the standby state machine (ca79ng/standby.h). */
#include <string.h>

#include "piano_int.h"

/* ---------------- standby ops over the platform ---------------- */

#define PS(p) ((p)->plat.standby)

static void o_cleanup(void *c)
{
    piano_t *p = c;
    /* stop everything that sounds; closing a recording queues its flash job (flash_busy) */
    metronome_stop(&p->metro);
    preview_stop(p);
    recorder_stop(&p->rec);
    voice_all_off(&p->voices, true);
    if (PS(p) && PS(p)->cleanup)
        PS(p)->cleanup(PS(p)->ctx);
}

static void o_amp(void *c, bool on)
{
    piano_t *p = c;
    if (PS(p) && PS(p)->amp) PS(p)->amp(PS(p)->ctx, on);
}

static void o_mute(void *c, bool on)
{
    piano_t *p = c;
    if (PS(p) && PS(p)->mute) PS(p)->mute(PS(p)->ctx, on);
}

static bool o_bt_enabled(void *c)
{
    piano_t *p = c;
    return PS(p) && PS(p)->bt_audio_enabled ? PS(p)->bt_audio_enabled(PS(p)->ctx) : false;
}

static void o_bt(void *c, bool on)
{
    piano_t *p = c;
    if (PS(p) && PS(p)->bt_audio) PS(p)->bt_audio(PS(p)->ctx, on);
}

static void o_codec(void *c, bool on)
{
    piano_t *p = c;
    if (PS(p) && PS(p)->codec) PS(p)->codec(PS(p)->ctx, on);
}

static bool o_headphones(void *c)
{
    piano_t *p = c;
    return PS(p) && PS(p)->headphones ? PS(p)->headphones(PS(p)->ctx) : false;
}

static bool o_alarm(void *c)
{
    piano_t *p = c;
    return PS(p) && PS(p)->amp_alarm ? PS(p)->amp_alarm(PS(p)->ctx) : false;
}

static bool o_busy(void *c)
{
    piano_t *p = c;
    return PS(p) && PS(p)->flash_busy ? PS(p)->flash_busy(PS(p)->ctx) : false;
}

static void o_notify(void *c, standby_note_t n)
{
    piano_t *p = c;
    if (n == SB_NOTE_AWAKE)
        power_timer_activity(&p->power); /* the full APO time from the wake-up */
    ng_standby_notice(p, p->sb.state);
    if (PS(p) && PS(p)->notify)
        PS(p)->notify(PS(p)->ctx, n);
}

static void o_power_off(void *c)
{
    piano_t *p = c;
    if (!p->off_notified)
        kp_shutdown_notify(&p->kp, MIDI_PORT_COUNT); /* 7E 00 00 to every bound controller (stock FUN_4014B0A8) */
    pn_power_off(p);
}

/* ---------------- policy ---------------- */

static void apply_policy(piano_t *p)
{
    apo_hold_init(&p->apo_hold, &p->policy.hold);
    p->sb.cap_ms = p->policy.standby_cap_ms;
    p->sb.keep_bt = !p->policy.standby_bt_off; /* TV wake */
}

void pw_init(piano_t *p)
{
    power_policy_default(&p->policy);
    if (p->kv) {
        uint8_t raw[POWER_POLICY_RAW];
        int n = kv_get(p->kv, KV_KEY_POWER_POLICY, raw, sizeof raw);
        if (n > 0)
            (void)power_policy_decode(&p->policy, raw, (uint16_t)n); /* a bad record keeps the defaults */
    }
    const standby_ops_t ops = {.ctx = p, .cleanup = o_cleanup, .amp = o_amp, .mute = o_mute,
                               .bt_audio_enabled = o_bt_enabled, .bt_audio = o_bt, .codec = o_codec,
                               .headphones = o_headphones, .amp_alarm = o_alarm, .flash_busy = o_busy,
                               .notify = o_notify, .power_off = o_power_off};
    standby_init(&p->sb, &ops, p->policy.standby_cap_ms, false);
    p->off_notified = false;
    apply_policy(p);
}

bool piano_set_power_policy(piano_t *p, const power_policy_t *pol, bool save)
{
    p->policy = *pol;
    apply_policy(p);
    if (!save || !p->kv)
        return !save;
    uint8_t raw[POWER_POLICY_RAW];
    power_policy_encode(&p->policy, raw);
    return kv_put(p->kv, KV_KEY_POWER_POLICY, raw, sizeof raw);
}

void pw_factory_reset(piano_t *p)
{
    if (p->kv)
        kv_delete(p->kv, KV_KEY_POWER_POLICY);
    power_policy_default(&p->policy);
    apply_policy(p);
}

/* ---------------- APO action, standby ---------------- */

void pw_tick(piano_t *p, uint32_t elapsed_ms)
{
    (void)standby_bt_stream(&p->sb, p->bt.starts); /* TV wake: a START in standby (BT module kept on) */
    standby_tick(&p->sb, elapsed_ms);
    if (!p->powered || standby_active(&p->sb) || p->sb.state == SB_OFF)
        return; /* in standby only the hard cap decides (standby.h) */
    if (!apo_tick(&p->apo_hold, &p->power, &p->bt, elapsed_ms))
        return;
    if (p->policy.apo_standby) {
        power_timer_activity(&p->power);
        standby_enter(&p->sb);
    } else {
        piano_hard_off(p);
    }
}

bool piano_standby_enter(piano_t *p) { return standby_enter(&p->sb); }

bool piano_standby_wake(piano_t *p, standby_wake_t src) { return standby_wake(&p->sb, src); }

void piano_hard_off(piano_t *p) { standby_hard_off(&p->sb); }

uint8_t piano_remote_off(piano_t *p, bool *bad_state)
{
    *bad_state = false;
    bool recording = p->rec.state == REC_STANDBY || p->rec.state == REC_RECORDING;
    uint8_t why = power_remote_off_check(recording, false, o_busy(p));   /* the host has no file executor */
    if (why != POWER_OFF_OK)
        return why;
    if (!p->powered || p->sb.state != SB_ON) {
        *bad_state = true;
        return POWER_OFF_OK;
    }
    if (p->policy.apo_standby) {   /* the APO expiry's action (pw_tick) */
        power_timer_activity(&p->power);
        standby_enter(&p->sb);
    } else {
        piano_hard_off(p);
    }
    return POWER_OFF_OK;
}

void pw_remote_off(void *ctx)
{
    piano_t *p = ctx;
    p->off_notified = true; /* kproto told the other controllers already */
    standby_hard_off(&p->sb);
}

void pw_midi_wake(piano_t *p, midi_port_t port, const uint8_t *m, uint16_t n)
{
    (void)standby_midi_wake(&p->sb, port, m, n);
}
