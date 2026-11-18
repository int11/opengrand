/* OpenGrand cave port - hal.h on top of the stock CA79 V2.19 firmware.
 *
 * The core runs inside the stock image (result/stockfw cave, image core_probe and later):
 * the stock RTOS, drivers and engine stay, and every hal.h function is bound to a stock
 * routine (see cave_stock.h for the bindings and the stubs).
 *
 * Besides hal.h this header gives the port-only pieces the cave glue needs: the logical
 * <-> stock MIDI port map, the parameter apply adapter, the task-context guard and the
 * logging stub. */
#ifndef CA79NG_HAL_CAVE_H
#define CA79NG_HAL_CAVE_H

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"

/* Core RW state lives in R1 above the cave code, not in the cave image: the cave is loaded
 * from flash as-is and nothing zeroes .bss, so the linker script (cave_core.ld) puts this
 * section NOLOAD at a fixed address and core_cave.c zeroes it on the first tick. */
#if defined(__arm__) && !defined(CAVE_HOST)
#define CAVE_RW __attribute__((section(".core_rw")))
#else
#define CAVE_RW
#endif

/* Stock MIDI port (0..3, the ring / TX writer index) for a logical port; -1 for
 * MIDI_PORT_INTERNAL and anything out of range.  The map is a bijection on 0..3, so a
 * reply sent to cave_port_from_stock(p) always leaves on stock port p. */
int cave_stock_port(midi_port_t port);
midi_port_t cave_port_from_stock(uint8_t stock_port);

/* true when the caller may block: a task, not an ISR or the kernel timer context, with
 * dispatch enabled.  Same rule the stock twai_sem applies before it waits (0x40086F0C). */
bool cave_may_block(void);

/* Parameter apply adapter: FUN_4016053C(ctx, part_spec, stock_id, data*).  ctx 0 = local
 * keyboard, 1..4 = MIDI ports 0..3 (BOUNDARY section 3); stock_id is the stock internal id,
 * not a core param_id_t.  Returns 0 handled, 0xF0 unsupported, -1 refused (not a task
 * context that may block).  Writes stock state: not called while the core is dormant. */
int cave_param_apply(uint8_t ctx, uint8_t part, uint8_t stock_id, const uint8_t *data);

/* Logging stub: no output channel yet, only counters the self-test op reports. */
enum {
    CAVE_LOG_NONE = 0,
    CAVE_LOG_TX_CONTEXT = 1,   /* hal_midi_tx refused: caller may not block */
    CAVE_LOG_TX_PORT = 2,      /* hal_midi_tx to a port with no stock TX writer */
    CAVE_LOG_APPLY_CONTEXT = 3,/* cave_param_apply refused: caller may not block */
    CAVE_LOG_POWER_OFF = 4,    /* hal_power_off requested (not bound yet: ignored) */
};
typedef struct {
    uint16_t count;     /* events logged since boot (saturates) */
    uint8_t last_code;  /* CAVE_LOG_* */
    uint32_t last_arg;
} cave_log_t;
void cave_log(uint8_t code, uint32_t arg);
const cave_log_t *cave_log_state(void);

#endif
