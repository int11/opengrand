/* OpenGrand — partial standby state machine (see ca79ng/standby.h). */
#include "ca79ng/standby.h"

#include <string.h>

void standby_init(standby_t *s, const standby_ops_t *ops, uint32_t cap_ms, bool deep)
{
    memset(s, 0, sizeof *s);
    s->ops = *ops;
    s->cap_ms = cap_ms;
    s->deep = deep;
    s->state = SB_ON;
}

#define OP0(s, f) do { if ((s)->ops.f) (s)->ops.f((s)->ops.ctx); } while (0)
#define OP(s, f, a) do { if ((s)->ops.f) (s)->ops.f((s)->ops.ctx, (a)); } while (0)
#define OPB(s, f) ((s)->ops.f ? (s)->ops.f((s)->ops.ctx) : false)

static void set_state(standby_t *s, standby_state_t st)
{
    s->state = st;
    s->t_ms = 0;
}

bool standby_enter(standby_t *s)
{
    if (s->state != SB_ON)
        return false;
    /* the stock's standby order */
    OP0(s, cleanup);                       /* 1 stop playback, close the recording (queued flash job) */
    OP(s, amp, false);                    /* 2 speaker amp off — no delay before the mute, like stock */
    OP(s, mute, true);                    /* 3 output mute */
    s->bt_was_on = !s->keep_bt && OPB(s, bt_audio_enabled);
    if (s->bt_was_on)
        OP(s, bt_audio, false);           /* 4 BT audio module off (kept on for the TV wake) */
    s->codec_off = s->deep && s->ops.codec;
    if (s->codec_off)
        OP(s, codec, false);              /* 5 (stage 2) codec power-down */
    s->alarm_skip = false;
    set_state(s, SB_STANDBY);             /* 7 flag + cap timer, then the notice */
    OP(s, notify, SB_NOTE_STANDBY);
    return true;
}

bool standby_wake(standby_t *s, standby_wake_t src)
{
    if (s->state != SB_STANDBY)
        return false;
    /* the stock's wake order */
    s->last_wake = src;
    set_state(s, SB_WAKING);              /* 1 flag down first: the hooks stop re-forcing */
    if (s->codec_off)
        OP(s, codec, true);               /* 2 codec re-init (~0.7 s inside the stock routines) */
    (void)OPB(s, headphones);             /* 4 re-read headphones, set the DSP output path */
    s->alarm_skip = OPB(s, amp_alarm);    /* 5 GP3[8] alarm: the stock event restores amp/mute */
    if (!s->alarm_skip)
        OP(s, mute, false);               /* 6 unmute */
    return true;                          /* 7.. in standby_tick */
}

static void wake_finish(standby_t *s)
{
    if (!s->alarm_skip && !OPB(s, headphones))
        OP(s, amp, true);                 /* 8 amp on unless headphones */
    if (s->bt_was_on)
        OP(s, bt_audio, true);            /* 9 BT audio back */
    s->bt_was_on = false;
    s->codec_off = false;
    set_state(s, SB_ON);
    OP(s, notify, SB_NOTE_AWAKE);         /* 10 notice; the caller resets the APO counter */
}

bool standby_midi_wake(standby_t *s, midi_port_t port, const uint8_t *m, uint16_t n)
{
    /* F0 40 <dev> 32 08 02 51 39 <part> <data> F7 — PowerManagementTimerReset, the panel
     * app's touch notice. Only from the panel (stock port 3). */
    if (s->state != SB_STANDBY || port != MIDI_PORT_PANEL)
        return false;
    if (n < 9 || m[0] != 0xF0 || m[1] != 0x40 || m[3] != 0x32 || m[6] != 0x51 || m[7] != 0x39)
        return false;
    return standby_wake(s, SB_WAKE_PANEL_TOUCH);
}

bool standby_bt_stream(standby_t *s, uint32_t starts)
{
    bool fresh = starts != s->bt_starts;
    s->bt_starts = starts;
    if (!fresh || !s->keep_bt || s->state != SB_STANDBY)
        return false;
    return standby_wake(s, SB_WAKE_BT_STREAM);
}

static void try_off(standby_t *s)
{
    if (OPB(s, flash_busy))
        return;                           /* never cut power under a pending flash write */
    set_state(s, SB_OFF);
    OP0(s, power_off);
}

void standby_hard_off(standby_t *s)
{
    if (s->state == SB_OFF || s->state == SB_OFF_PENDING)
        return;
    set_state(s, SB_OFF_PENDING);
    s->busy_wait_ms = 0;
    try_off(s);
}

void standby_tick(standby_t *s, uint32_t elapsed_ms)
{
    s->t_ms = (s->t_ms > UINT32_MAX - elapsed_ms) ? UINT32_MAX : s->t_ms + elapsed_ms;
    switch (s->state) {
    case SB_STANDBY:
        /* cap 0 = no cap (policy 0xFFFF): t_ms is never compared, it only
         * saturates at UINT32_MAX (49.7 days) for the diagnostics */
        if (s->cap_ms && s->t_ms >= s->cap_ms)
            standby_hard_off(s);
        break;
    case SB_WAKING:
        if (s->t_ms >= (s->codec_off ? STANDBY_WAKE_DELAY_CODEC_MS : STANDBY_WAKE_DELAY_MS))
            wake_finish(s);
        break;
    case SB_OFF_PENDING:
        s->busy_wait_ms += elapsed_ms;
        try_off(s);
        break;
    default:
        break;
    }
}

bool standby_active(const standby_t *s)
{
    return s->state == SB_STANDBY || s->state == SB_WAKING || s->state == SB_OFF_PENDING;
}
