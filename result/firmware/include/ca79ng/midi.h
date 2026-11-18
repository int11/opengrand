/* OpenGrand — MIDI byte-stream parsing and port model.
 *
 * Observed in the original firmware: one parser per port,
 * 4 UART-backed ports with 2 KB rings, SysEx dispatched on the manufacturer
 * byte (0x40 Kawai, 0x7E/0x7F universal, 0x41 Roland GS reset).
 * The panel app's own parser keeps running status and drops SysEx > 511 bytes
 * so we never emit longer messages. */
#ifndef CA79NG_MIDI_H
#define CA79NG_MIDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIDI_SYSEX_MAX 512u /* receive buffer per port */
#define MIDI_SYSEX_TX_MAX 511u /* = the original app's receive limit (see header comment) */

typedef enum {
    MIDI_PORT_DIN = 0,  /* 5-pin MIDI IN/OUT */
    MIDI_PORT_USB,      /* USB to Host (computer) — class compliant USB-MIDI */
    MIDI_PORT_PANEL,    /* built-in LCD panel (Android board), USB-MIDI */
    MIDI_PORT_BLE,      /* BLE-MIDI module (UART bridge) */
    MIDI_PORT_INTERNAL, /* recorder / song player / metronome (never transmitted to HW) */
    MIDI_PORT_COUNT
} midi_port_t;

typedef enum {
    MIDI_MSG_CHANNEL, /* 1..3 byte channel voice/mode message */
    MIDI_MSG_SYSEX,   /* complete F0 .. F7 */
    MIDI_MSG_REALTIME,
    MIDI_MSG_COMMON,
} midi_msg_kind_t;

typedef struct {
    midi_msg_kind_t kind;
    midi_port_t port;
    uint8_t status;      /* channel/common/realtime status; 0xF0 for sysex */
    uint8_t data[2];
    const uint8_t *sysex; /* valid only during the callback, includes F0/F7 */
    uint16_t sysex_len;
} midi_msg_t;

typedef void (*midi_msg_cb)(void *ctx, const midi_msg_t *msg);

typedef struct {
    midi_port_t port;
    uint8_t running;      /* running status (0 = none) */
    uint8_t need;         /* data bytes expected for current status */
    uint8_t have;
    uint8_t data[2];
    bool in_sysex;
    bool sysex_overflow;
    uint16_t sysex_len;
    uint8_t sysex[MIDI_SYSEX_MAX];
    uint32_t errors;
    midi_msg_cb cb;
    void *ctx;
} midi_parser_t;

void midi_parser_init(midi_parser_t *p, midi_port_t port, midi_msg_cb cb, void *ctx);
void midi_parser_feed(midi_parser_t *p, uint8_t byte);
void midi_parser_feed_buf(midi_parser_t *p, const uint8_t *buf, size_t len);

/* Number of data bytes that follow a status byte (0..2), -1 for SysEx start. */
int midi_data_len(uint8_t status);

#endif
