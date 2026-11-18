/* OpenGrand — system services: startup-settings persistence, auto power off,
 * factory reset. */
#ifndef CA79NG_SYSTEM_H
#define CA79NG_SYSTEM_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/kproto.h"
#include "ca79ng/storage.h"

/* "Startup Settings" (StartUpSettingSave, fn32 51 34): only values that differ
 * from the factory defaults are written, as (param, slot, len, data) records
 * split over consecutive kv keys starting at KV_KEY_STARTUP_SETTINGS. */
#define SETTINGS_KEYS 16

int settings_save(const param_store_t *s, kvstore_t *kv);   /* returns records written, -1 on error */
int settings_load(param_store_t *s, kvstore_t *kv);         /* returns records applied */
bool settings_clear(kvstore_t *kv);

/* Auto Power Off: 0 Never, 1 = 15 min, 2 = 60 min, 3 = 120 min (AutoPowerOff enum). */
typedef struct {
    uint8_t setting;
    uint32_t idle_ms;
} power_timer_t;

void power_timer_init(power_timer_t *t, uint8_t setting);
void power_timer_activity(power_timer_t *t);
/* returns true when the instrument should switch off */
bool power_timer_tick(power_timer_t *t, uint32_t elapsed_ms);
uint32_t power_timer_limit_ms(uint8_t setting);

#endif
