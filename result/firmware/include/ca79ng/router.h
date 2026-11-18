/* OpenGrand — MIDI channel routing (manual "MIDI Settings" + implementation chart).
 *
 *  - One channel for transmit and receive (1..16). Power-on = Omni On; setting
 *    a channel turns Omni Off.
 *  - Multi-timbral Off / On1 / On2: when on, each received channel plays its
 *    own sound (channel mute 1..16 applies). On2 selects the GM-oriented
 *    program map.
 *  - Split + multi-timbral: lower notes transmit on channel+1;
 *    Dual + multi-timbral: notes transmit on channel and channel+1 (16 -> 1).
 *  - Local Off: keys transmit only, the internal engine does not sound.
 *  - Transmit: notes 21..108 (after transpose), CC 0/32, 64/66/67, PC, SysEx.
 *  - Receive: notes, CC 0/32/7/10/11/64/66/67, PC, All Notes Off (123..127). */
#ifndef CA79NG_ROUTER_H
#define CA79NG_ROUTER_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/midi.h"
#include "ca79ng/sound.h"

typedef enum { MT_OFF = 0, MT_ON1 = 1, MT_ON2 = 2 } multitimbral_t;

typedef struct {
    uint8_t channel;        /* 0..15 */
    bool omni;
    bool local;
    multitimbral_t multitimbral;
    bool channel_play[16];  /* Channel On/Off (multi-timbral only) */
    bool send_pc;           /* Transmit Program Change */
    bool tx_port[MIDI_PORT_COUNT];
} midi_cfg_t;

/* MIDI part index used for the 16 receive channels when multi-timbral is on
 * (SysEx part numbers 32..47 in the app protocol). */
#define PART_MIDI_BASE 32

typedef struct {
    void *ctx;
    /* sound side */
    void (*note)(void *ctx, uint8_t part, uint8_t note, uint8_t vel); /* vel 0 = off */
    void (*control)(void *ctx, uint8_t part, uint8_t cc, uint8_t value);
    void (*program)(void *ctx, uint8_t part, uint8_t msb, uint8_t lsb, uint8_t pc);
    void (*all_notes_off)(void *ctx, uint8_t part);
    /* transmit side: raw bytes to one hardware port */
    void (*tx)(void *ctx, midi_port_t port, const uint8_t *bytes, uint16_t len);
} router_io_t;

typedef struct {
    midi_cfg_t cfg;
    router_io_t io;
    kb_mode_t kb_mode;
    uint8_t bank_msb[16], bank_lsb[16]; /* received bank select per channel */
} router_t;

void midi_cfg_default(midi_cfg_t *c);
void router_init(router_t *r, const router_io_t *io);
void router_set_channel(router_t *r, uint8_t ch); /* also clears omni */

/* Keyboard-originated note (already through touch & layout). `part` is the
 * layout part (PART_MAIN1 / PART_LAYER / PART_LOWER / 4H). Sounds locally if
 * Local is on and transmits on the configured channel(s). */
void router_key_note(router_t *r, uint8_t part, uint8_t sounding_note, uint8_t vel);
void router_key_control(router_t *r, uint8_t part, uint8_t cc, uint8_t value);
/* Sound selected on the instrument: optionally transmit bank + PC. */
void router_key_program(router_t *r, uint8_t msb, uint8_t lsb, uint8_t pc);

/* Received channel message from any port. */
void router_rx(router_t *r, const midi_msg_t *m);

/* ---- program maps (manual program change list) ----
 * Off / On1: received PC n selects the CA79 sound with program number n+1
 * (bank select ignored). On2: GM2 numbering, (bank MSB, LSB, PC) selects the
 * GM2 voice; CA79 sounds map to their own (msb, lsb, pc) sound id, GM2-only
 * voices to (msb, lsb, PC | 0x80) so the engine can tell the numbering apart.
 * Unknown banks fall back to the GM2 capital voice (bank 121/0) of the PC. */
typedef struct { uint8_t on2_pc, msb, lsb; int8_t sound; } progmap_entry_t;
extern const progmap_entry_t PROGMAP_ON2[];
extern const unsigned PROGMAP_ON2_COUNT;
/* Returns true and the sound id when the program is known. */
bool progmap_lookup(multitimbral_t mode, uint8_t msb, uint8_t lsb, uint8_t pc, uint8_t out[3]);
/* Reset received bank registers (GM/GS reset). */
void router_reset_banks(router_t *r);

/* Transmit a SysEx to one port, or to all enabled ports if port == MIDI_PORT_COUNT. */
void router_tx_sysex(router_t *r, midi_port_t port, const uint8_t *msg, uint16_t len);

#endif
