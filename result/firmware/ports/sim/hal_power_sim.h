/* OpenGrand host stubs for the standby / BT audio part of hal.h.
 *
 * The cave binds these to stock routines at Phase 3-3 (hal.h comments); on the host
 * they only record calls and return the inputs a test or the simulator set, so the
 * standby state machine can run against the real hal.h names. */
#ifndef CA79NG_HAL_POWER_SIM_H
#define CA79NG_HAL_POWER_SIM_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/standby.h"

#define HAL_SIM_LOG 64

typedef struct {
    /* inputs */
    bool bt_audio_enabled, headphones, amp_alarm, flash_busy, a2dp_link;
    /* outputs */
    bool amp, mute, bt_audio, codec;
    int power_offs;
    standby_note_t last_note;
    /* call log: one short name per call, in order */
    const char *log[HAL_SIM_LOG];
    int n;
} hal_power_sim_t;

extern hal_power_sim_t hal_power_sim;

/* Power-on state: amp on, unmuted, BT audio on, codec on, nothing pending. */
void hal_power_sim_reset(void);
/* standby_ops_t bound to the hal.h functions (the table the cave port will copy). */
void hal_standby_ops(standby_ops_t *ops);

#endif
