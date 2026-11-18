/* OpenGrand cave port - hal.h bound to the stock CA79 V2.19 firmware.
 * Binding table and evidence: cave_stock.h, hal_cave.h. */
#include "hal_cave.h"

#include "cave_stock.h"

static cave_log_t log_state = {0};

void cave_log(uint8_t code, uint32_t arg)
{
    if (log_state.count != 0xFFFFu)
        log_state.count++;
    log_state.last_code = code;
    log_state.last_arg = arg;
}

const cave_log_t *cave_log_state(void) { return &log_state; }

bool cave_may_block(void)
{
    return stk_int_nest == 0 && stk_dsp_disabled == 0 && stk_cur_task != STK_KERNEL_TASK_ID;
}

/* ---- MIDI ports -------------------------------------------------------------------
 * Stock port -> hardware (REA):
 *   2 = BLE-MIDI module [observed: the module name SysEx goes to port 2 only]
 *   3 = built-in panel  [inferred: DISCONNECT notice goes to port 3 only]
 *   0 = ext UART 0x62000100, 1 = OMAP UART2 - DIN / USB to Host [inferred, see
 * probe run].  Only the logical meaning of 0 and 1 is unconfirmed; the
 *   map stays a bijection, so replies always go back to the stock port they came from. */
static const int8_t STOCK_OF[MIDI_PORT_COUNT] = {
    [MIDI_PORT_DIN] = 0, [MIDI_PORT_USB] = 1, [MIDI_PORT_PANEL] = 3, [MIDI_PORT_BLE] = 2,
    [MIDI_PORT_INTERNAL] = -1,
};

int cave_stock_port(midi_port_t port)
{
    return (unsigned)port < MIDI_PORT_COUNT ? STOCK_OF[port] : -1;
}

midi_port_t cave_port_from_stock(uint8_t stock_port)
{
    for (int p = 0; p < MIDI_PORT_INTERNAL; p++)
        if (STOCK_OF[p] == (int8_t)stock_port)
            return (midi_port_t)p;
    return MIDI_PORT_INTERNAL;
}

/* ---- hal.h ----------------------------------------------------------------------- */

uint32_t hal_time_us(void) { return stk_time_us(); }

/* The stock TX writers take a per-port semaphore, and past 0x7FF queued bytes they wait for
 * the 2 ms handler to drain the 2 KB ring, so they are task-only.
 * Longer than the panel app's 511 B limit is the caller's problem (kproto never emits more). */
void hal_midi_tx(midi_port_t port, const uint8_t *bytes, uint16_t len)
{
    int sp = cave_stock_port(port);
    if (sp < 0) {
        cave_log(CAVE_LOG_TX_PORT, (uint32_t)port);
        return;
    }
    if (!cave_may_block()) {
        cave_log(CAVE_LOG_TX_CONTEXT, (uint32_t)port);
        return;
    }
    switch (sp) {
    case 0: stk_tx_port0(bytes, len); break;
    case 1: stk_tx_port1(bytes, len); break;
    case 2: stk_tx_port2(bytes, len); break;
    default: stk_tx_port3(bytes, len); break;
    }
}

/* Not bound: the stock MIDI task owns the RX rings and the parser, and the cave receives
 * complete SysEx messages from the manufacturer-SysEx hook instead of bytes.  Phase 3-1
 * moves the Kawai branch of the same end handler to the core (Phase 3). */
bool hal_midi_rx(midi_port_t port, uint8_t *byte)
{
    (void)port;
    (void)byte;
    return false;
}

/* Not bound until Phase 3-4 (settings ownership): the UserMemory sectors hold the stock
 * settings image, and writes must go through the stock delayed writer (BOUNDARY 2). */
const hal_flash_t *hal_settings_flash(void) { return 0; }

void hal_settings_sectors(uint32_t *a, uint32_t *b)
{
    *a = 0x60400000u;   /* UserMemory, 2 x 128 KB */
    *b = 0x60420000u;
}

/* Not bound: the stock FAT stack has one global write handle (BOUNDARY 2). */
const hal_fs_t *hal_usb_fs(void) { return 0; }

/* Not bound until Phase 3-3 (power/APO): logged and ignored. */
void hal_power_off(void) { cave_log(CAVE_LOG_POWER_OFF, 0); }

/* Update-key detection is the boot loader's. */
bool hal_update_keys_held(void) { return false; }

/* ---- parameter apply ------------------------------------------------------------- */

int cave_param_apply(uint8_t ctx, uint8_t part, uint8_t stock_id, const uint8_t *data)
{
    if (!cave_may_block()) {
        cave_log(CAVE_LOG_APPLY_CONTEXT, stock_id);
        return -1;
    }
    return stk_param_apply(ctx, part, stock_id, data);
}
