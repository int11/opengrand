/* OpenGrand cave port - panel-UI relay (slot payload ota_main, Identify feat2 0x04).
 *
 * The 0x7D ops 0x30-0x3F (REMOTE_PROTOCOL.md section 10.7) belong to the loader in the panel's OpenGrand UI
 * bundle, not to the piano: the cave passes them through, unread, between a controller port and the
 * panel port (stock port 3), and the panel's replies 0x70-0x7F back.  Rules R1-R6 below; REA
 * evidence in relay_stock.h.
 *
 *   R1 op 0x30-0x3F from a port other than 3 -> the frame as it came (F0 .. F7) to port 3; that port
 *      becomes the reply port.
 *   R2 op 0x70-0x7F from port 3 -> the frame as it came to the reply port (dropped when there is none).
 *   R3 any other combination is dropped; a frame longer than RL_FRAME_MAX is dropped first.
 *   R4 whole or nothing: a frame goes out in one writer call only when the target ring has room for all of
 *      it plus RL_RESERVE bytes kept for the stock's own frames - else it is dropped and counted (the
 *      sender's go-back-N resends it).  Never a cut frame, never a wait for ring space.
 *   R5 order per direction = arrival order (task 19 handles one frame at a time, no queue).
 *   R6 Identify feat2 0x04; counters on op 0x1B sub 0x10.
 *
 * Context: task 19 only (the resident's 0x7D hook at 0x40147E1C -> ng_sysex), the MIDI task that also runs
 * the stock's own replies; the resident holds its in-sysex flag for the call.  No state but the reply port
 * and counters (payload .data). */
#ifndef CA79NG_RELAY_CAVE_H
#define CA79NG_RELAY_CAVE_H

#include <stdint.h>

#define RL_FRAME_MAX 256u   /* the sender's frames are <= 240 B (BLE path 256 B) */
#define RL_RESERVE 512u     /* TX ring bytes left to the stock: our frames take a ring only below 0x7FE - 512 */
#define RL_DIAG_SUB 0x10    /* op 0x1B 10: the relay record */
#define RL_DIAG_RAW 40

/* the op belongs to the relay (request 0x30-0x3F or reply 0x70-0x7F) */
int rlc_owns(uint8_t op);
/* task 19: a complete 0x7D frame F0 7D 43 41 37 39 <op> .. F7 of the relay's ops from stock port `port` */
void rlc_sysex(uint8_t port, const uint8_t *buf, uint32_t len);
/* task 19: op 0x1B 10 -> F0 7D 43 41 37 39 5B 10 00 <pack7 RL_DIAG_RAW> F7 on the asking port */
void rlc_diag(uint8_t port);
/* the raw record (layout 1): see relay_cave.c rlc_stats() */
void rlc_stats(uint8_t raw[RL_DIAG_RAW]);
/* host tests: forget the reply port and the counters */
void rlc_restart(void);

#endif
