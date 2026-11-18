/* OpenGrand — Bluetooth module drivers.
 *
 * BLE-MIDI module (Kawai MBH7BLZ07, Nordic based; "QS" firmware). The main
 * board talks MIDI to it over a UART and configures it with a private SysEx
 * (manufacturer 00 02 08, observed in the app: bluetoothmidi.js,
 * BleMidi.renameDevice, DFU_execute):
 *   F0 00 02 08 10 55 00 01 <name 16>    set advertised name
 *   F0 00 02 08 10 55 00 02 <t> 00        timestamp offset
 *   F0 00 02 08 10 55 10 04 00            request firmware -> 20 04 <ascii>
 *   F0 00 02 08 10 55 7F 00 01            enter DFU (bootloader "DFUMI1P"/"DfuTarg")
 *   F0 00 02 08 10 55 7F 01 01            disconnect central
 * The original app always disabled the module's timestamps to work around a
 * module bug; the firmware does the same at start-up. */
#ifndef CA79NG_BT_H
#define CA79NG_BT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *ctx;
    void (*uart_tx)(void *ctx, const uint8_t *bytes, uint16_t len);
} blemidi_io_t;

typedef struct {
    blemidi_io_t io;
    bool enabled;
    char name[17];
    char fw_version[24];
} blemidi_t;

void blemidi_init(blemidi_t *b, const blemidi_io_t *io, const char *name);
void blemidi_set_enabled(blemidi_t *b, bool on);
void blemidi_set_name(blemidi_t *b, const char *name);
void blemidi_request_firmware(blemidi_t *b);
void blemidi_enter_dfu(blemidi_t *b);
/* Returns true if the SysEx was a module reply (consumed). */
bool blemidi_handle_sysex(blemidi_t *b, const uint8_t *msg, uint16_t len);

/* Build one module command into out (>= 32 bytes); returns its length. */
size_t blemidi_cmd(uint8_t *out, uint8_t cmd, uint8_t sub, const uint8_t *data, size_t n);

#endif
