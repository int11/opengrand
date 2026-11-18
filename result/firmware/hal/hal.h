/* OpenGrand — hardware abstraction layer.
 *
 * Everything below this line is provided per platform (ports/sim, ports/target).
 * The core (src/) never touches registers; it is driven through piano_t:
 *
 *   key matrix ISR   -> piano_key_edge(key, sensor, closed, t_us)
 *   pedal ADC (1 kHz)-> piano_pedal(id, adc)
 *   UART/USB RX ISR  -> ring buffer -> piano_midi_in(port, bytes)  (event loop)
 *   1 ms timer       -> piano_tick(1)
 *   audio callback   -> engine render (DSP)
 *
 * Original hardware facts used for the target port:
 *   - SoC: TI OMAP-L13x / AM18xx class (ARM926EJ-S + C674x DSP)
 *   - main program linked at 0x40060000 (SDRAM), stored at 0x60060000 (NOR, EMIFA CS)
 *   - flash partitions: Boot 0x60000000+0x60000, Main +0x3A0000, UserMemory,
 *     TouchAdjust, TouchCalib, RecorderData
 *   - MIDI ports: 2 external UARTs on EMIFA (0x62000080, 0x62000100), UART0
 *     0x01C42000, UART2 0x01D0D000; UART1 0x01D0C000 = Bluetooth audio module
 *   - SYSCFG KICK registers 0x01C14038/3C, GPIO bank base 0x01E26000
 * These are the original board's addresses; a new board design may differ,
 * which is why they live only in the target port. */
#ifndef CA79NG_HAL_H
#define CA79NG_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "ca79ng/midi.h"
#include "ca79ng/fs.h"
#include "ca79ng/storage.h"

/* time */
uint32_t hal_time_us(void);

/* MIDI transport per logical port (DIN, USB device, PANEL, BLE) */
void hal_midi_tx(midi_port_t port, const uint8_t *bytes, uint16_t len);
/* drained by the event loop; filled by RX interrupts */
bool hal_midi_rx(midi_port_t port, uint8_t *byte);

/* settings flash (two sectors of the UserMemory partition) */
const hal_flash_t *hal_settings_flash(void);
void hal_settings_sectors(uint32_t *a, uint32_t *b);

/* USB memory (USB to Device port): file system of the attached stick, NULL
 * if the board has none. Attach/detach is reported through mounted(). */
const hal_fs_t *hal_usb_fs(void);

/* power and panel */
void hal_power_off(void);
bool hal_update_keys_held(void); /* top E, A, B, C held at power-on -> boot loader update mode */

/* partial standby and the BT audio APO rule (ca79ng/standby.h, ca79ng/power.h). Host stubs only (ports/sim/hal_power_sim.c);
 * the cave binds them at Phase 3-3 to the stock routines. Task context. */
void hal_standby_cleanup(void);       /* stop playback, close the recording: FUN_400B0EA8 */
void hal_amp_enable(bool on);         /* speaker amp GP6[15]: FUN_400ACCE8 / FUN_400ABCAC */
void hal_output_mute(bool on);        /* output mute GP6[11]: FUN_400ABCC0 / FUN_400ACCFC */
bool hal_bt_audio_enabled(void);      /* BluetoothAudio setting (config 0x418CE0F8) */
void hal_bt_audio_power(bool on);     /* FUN_400AD098 + FUN_40146810 / FUN_400AD030 */
void hal_codec_power(bool on);        /* stage 2: FUN_400ABCD4+FUN_400ABCEC / FUN_400AD118..FUN_400AD730 */
bool hal_headphones_present(void);    /* re-read + apply: FUN_400ACC7C, then FUN_400ACC58 */
bool hal_amp_alarm(void);             /* GP3[8] alarm sample 0x4139E7FF == 1 */
bool hal_flash_busy(void);            /* FUN_401342AC() == 1 || busy byte 0x4139DAC4 != 0 */
bool hal_bt_a2dp_link(void);          /* stock A2DP link flag 0x413AE5B8 && module power 0x4139E7F0 */

#endif
