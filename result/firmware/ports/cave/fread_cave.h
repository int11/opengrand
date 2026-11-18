/* OpenGrand cave port - fn 0x24 file read (Read job 04) with 7-bit DataPacket counters (slot payload ota_main).
 *
 * The stock read (FUN_4014F364 job 4 -> FUN_4014D974 -> FUN_4014D8E0 -> builder FUN_4014D7E0) writes the packet
 * counter byte unmasked (strb at 0x4014D820) and stops after counter 0x80 (cmp r4, #0x80 / ble at 0x4014D9C0):
 * counter 0x80 goes out as a status byte, the BT module cuts the SysEx and the read stalls (a file > 128 x 21 B).
 * Neither instruction is a hook site, so the core takes the whole Read job at the allow-listed engine-loop site
 * #15 0x40143ACC (bl FUN_4014F364, ota_loader_v3) and runs the stock job body with its own packet loop:
 * the stock builder for each packet (data pointer advanced, counter 0 / 1 as for the stock's first two packets), then
 * counter byte = n & 0x7F and the checksum patched by the same XOR; every other job (and every pass without a Read due) goes to the stock executor.
 * Task 24 (the resident's own task): the body may call and block (stop-and-wait, as the stock does) and returns. */
#ifndef CA79NG_FREAD_CAVE_H
#define CA79NG_FREAD_CAVE_H

#include <stdint.h>

#define FRC_SITE 0x40143ACCu   /* bl FUN_4014F364 */
#define FRC_WORD 0xEB002E24u

/* task 24, the stub: 1 = `b` to the stock executor FUN_4014F364, 0 = the job was run here (back to the site + 4) */
int frc_pass(void);
/* src/power_hooks.c attach(): the hook_set rc (0 = hooked, 0xFF = not asked) */
void frc_attached(int rc);
/* task 19, sysex_cave.c sxc_kawai (the 0x40147E94 front, after the session core and the intake gate), every fn 0x24 01 /
 * 02 / 7B frame of the binding before the stock gets it: the stock receiver FUN_4014FB94 uses the counter byte as the
 * packet index (a file > 2681 B never completes); here the index is counted past the 7-bit counter and, from packet 127
 * on, the frame (counter byte, checksum byte) and the upload length word are rewritten so that the receiver writes the
 * packet into slot 127 with the true offset's group lengths and completion test; the next frame moves it to its place.
 * Nothing is sent and no stock routine is called - the stock dispatcher answers every frame.  Always 0 (the stock).
 * The Read job's sender also ends on a Cancel (24 7D) sent
 * instead of the ACK (Result 0xF0 at once, binding kept), not after 16 x 250-tick resends and an unbind. */
int frc_rx(uint32_t port, uint8_t *m, uint32_t n);

#endif
