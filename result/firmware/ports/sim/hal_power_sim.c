/* OpenGrand host stubs for the standby / BT audio part of hal.h (see hal_power_sim.h). */
#include "hal_power_sim.h"

#include <string.h>

#include "hal.h"

hal_power_sim_t hal_power_sim;

static void rec(const char *what)
{
    if (hal_power_sim.n < HAL_SIM_LOG)
        hal_power_sim.log[hal_power_sim.n++] = what;
}

void hal_power_sim_reset(void)
{
    memset(&hal_power_sim, 0, sizeof hal_power_sim);
    hal_power_sim.amp = hal_power_sim.bt_audio = hal_power_sim.codec = true;
    hal_power_sim.bt_audio_enabled = true;
}

void hal_standby_cleanup(void) { rec("cleanup"); }
void hal_amp_enable(bool on) { hal_power_sim.amp = on; rec(on ? "amp_on" : "amp_off"); }
void hal_output_mute(bool on) { hal_power_sim.mute = on; rec(on ? "mute" : "unmute"); }
bool hal_bt_audio_enabled(void) { return hal_power_sim.bt_audio_enabled; }
void hal_bt_audio_power(bool on) { hal_power_sim.bt_audio = on; rec(on ? "bt_on" : "bt_off"); }
void hal_codec_power(bool on) { hal_power_sim.codec = on; rec(on ? "codec_on" : "codec_off"); }
bool hal_headphones_present(void) { rec("headphones"); return hal_power_sim.headphones; }
bool hal_amp_alarm(void) { return hal_power_sim.amp_alarm; }
bool hal_flash_busy(void) { return hal_power_sim.flash_busy; }
bool hal_bt_a2dp_link(void) { return hal_power_sim.a2dp_link; }

/* ---- standby_ops_t over hal.h ---- */
static void o_cleanup(void *c) { (void)c; hal_standby_cleanup(); }
static void o_amp(void *c, bool on) { (void)c; hal_amp_enable(on); }
static void o_mute(void *c, bool on) { (void)c; hal_output_mute(on); }
static bool o_bt_enabled(void *c) { (void)c; return hal_bt_audio_enabled(); }
static void o_bt(void *c, bool on) { (void)c; hal_bt_audio_power(on); }
static void o_codec(void *c, bool on) { (void)c; hal_codec_power(on); }
static bool o_hp(void *c) { (void)c; return hal_headphones_present(); }
static bool o_alarm(void *c) { (void)c; return hal_amp_alarm(); }
static bool o_busy(void *c) { (void)c; return hal_flash_busy(); }
static void o_note(void *c, standby_note_t n)
{
    (void)c;
    hal_power_sim.last_note = n;
    rec(n == SB_NOTE_STANDBY ? "note_standby" : "note_awake");
}
/* the host has no stock executor: count the request (hal_power_off belongs to the cave port) */
static void o_off(void *c) { (void)c; hal_power_sim.power_offs++; rec("power_off"); }

void hal_standby_ops(standby_ops_t *ops)
{
    *ops = (standby_ops_t){.ctx = 0, .cleanup = o_cleanup, .amp = o_amp, .mute = o_mute,
                           .bt_audio_enabled = o_bt_enabled, .bt_audio = o_bt, .codec = o_codec,
                           .headphones = o_hp, .amp_alarm = o_alarm, .flash_busy = o_busy,
                           .notify = o_note, .power_off = o_off};
}
