/* OpenGrand cave port - the stock CA79 V2.19 routines and globals the power port binds to
 * (power_cave.c: partial standby, the APO rule, the power policy store).
 *
 * On the piano the addresses come from the linker (result/stockfw/cave/stock_power.ld, slot
 * payload ota_power); on the host the unit test (tests/test_power_cave.c) defines every name
 * here as a recording stand-in.  Evidence per binding:
 *   power domains: amp / mute, BT audio
 *   power GPIO: GP3[8], standby order and hooks
 *   APO counter / state byte, BT ring
 *   the allow-listed hook sites
 *   (this binding, REA / disassembly re-check)
 *
 * Context: every routine here is called from the engine-loop task (24) only - the same task
 * the stock executor FUN_400ACD28 and the headphone poll FUN_400ACFA0 run in.  The globals are
 * also read (and the four marked "written") from the 20 ms kernel-timer hook, which calls no
 * routine at all. */
#ifndef CA79NG_POWER_STOCK_H
#define CA79NG_POWER_STOCK_H

#include <stdint.h>

/* ---- routines ------------------------------------------------------------------------ */
void stk_amp_off(void);         /* FUN_400ABCAC: OUT_DATA67 &= ~0x8000 (GP6[15] speaker amp) */
void stk_amp_on(void);          /* FUN_400ACCE8: OUT_DATA67 |= 0x8000                        */
void stk_mute(void);            /* FUN_400ABCC0: OUT_DATA67 &= ~0x0800 (GP6[11] = 0: muted)  */
void stk_unmute(void);          /* FUN_400ACCFC: OUT_DATA67 |= 0x0800                        */
void stk_cleanup(void);         /* FUN_400B0EA8: stop playback, close the recording (executor step 5,
                                 * also CONNECT) - queues the RecorderData job, may block       */
void stk_hp_apply(void);        /* FUN_400ACC7C: re-read GP5[14] into 0x4139E7FA and set the DSP
                                 * output path (param 1: never switches the amp ON; headphones ->
                                 * FUN_4013BD94 switches it off), SysEx 0x58 notice              */
int stk_hp_present(void);       /* FUN_400ACC58: 0x4139E7FA != 0                               */
void stk_bt_audio_off(void);    /* FUN_400AD030: BT audio module off, 0x4139E7F0 = 0           */
void stk_bt_audio_on(void);     /* FUN_400AD098: on when the boot BT flag allows (busy-waits
                                 * 120 on the AT dialect), 0x4139E7F0 = 1                      */
void stk_bt_flags_clear(void);  /* FUN_40146810: READY / A2DP / AVRCP flags = 0 (the SysEx "on"
                                 * path calls it right after FUN_400AD098, 0x40161178)         */
void stk_bt_rx_reset(void);     /* FUN_40145C00: UART1 RX ring reset - write index 0, flag 0, read index
                                 * 0 (in that order); the boot path's call (0x400AAF98 -> FUN_40146470).
                                 * TV wake build only, only while the flag is latched at 0x80 and the
                                 * ring is empty                   */
int stk_flash_busy(void);       /* FUN_4013427C: a stock writer owns the chip                  */
int stk_flash_jobs(void);       /* FUN_401342AC: the deferred writer has an active job         */
/* NOR reads (the stock window copier).  NOR WRITES are not stock calls of this port: they go through the loader's
 * guarded write service (power_cave.h pwc_nor_bind - ota_loader_v4 stage-0; only stage-0 links the stock writers) */
void stk_flash_copy(uint32_t src_flash, void *dst_ram, uint32_t len); /* FUN_400AA030          */
void stk_dis_dsp(void);                                               /* NORTi dis_dsp         */
void stk_ena_dsp(void);                                               /* NORTi ena_dsp         */
void stk_dly_tsk(uint32_t ms);                                        /* NORTi dly_tsk wrapper */

/* ---- globals ------------------------------------------------------------------------- */
/* written by the port (each write is one the stock code itself makes; readers checked by REA) */
extern volatile uint32_t stk_apo_counter;   /* 0x4139E7E4 APO idle counter, 20 ms ticks: hook writes 1
                                             * (= FUN_400ACB84's reset value)  */
extern volatile uint8_t stk_power_req;      /* 0x4139E7FC 2 = shutdown request (APO expiry writes it,
                                             * 0x400ABF90); the executor FUN_400ACD28 consumes it    */
extern volatile uint8_t stk_gp3_event;      /* 0x4139E7FD GP3[8] event 1/2 for the executor; the hook
                                             * clears it during standby (the executor clears it too)  */
extern volatile uint32_t stk_power_press;   /* 0x4139E7F4 power switch press ticks, 0 = released.  Only
                                             * FUN_400ABD74 (the 20 ms body right after the hook) touches it:
                                             * press edge 1, release 0, +1 per tick, > 15 -> state byte 1
                                             * (released then = real off).  The timer hook writes 14 (a value
                                             * the body itself passes through) while a press is < 4 s - F-A1,
                                             * (every reader / writer swept there)       */
/* read only */
extern const volatile uint8_t stk_gp3_level;     /* 0x4139E7FF GP3[8] sample, 1 = alarm (mute request) */
extern const volatile uint8_t stk_apo_setting;   /* 0x4139E7F9 0 Never, 1 15 min, 2 60 min, 3 120 min  */
extern const volatile uint32_t stk_bt_power;     /* 0x4139E7F0 BT audio module powered               */
extern const volatile uint8_t stk_a2dp_flag;     /* 0x413AE5B8 A2DP link flag                        */
extern const volatile uint8_t stk_bt_audio_cfg;  /* 0x418CE0F8 BluetoothAudio setting (config +0x1C)  */
extern const volatile uint8_t stk_special_mode;  /* 0x419156A7 1 = test / wave ROM mode: no APO     */
extern const volatile uint8_t stk_binding;       /* 0x413CA3DE bound controller stock port + 1, 0 none */
extern const volatile uint8_t stk_bt_rx_ring[0x800]; /* 0x413AA2FC BT module UART1 RX ring          */
extern const volatile uint16_t stk_bt_rx_wr;     /* 0x413AE58A ring write index (UART1 RX ISR)       */
extern const volatile uint16_t stk_bt_rx_rd;     /* 0x413AE588 ring read index (line assembler)       */
extern const volatile uint8_t stk_bt_rx_flag;    /* 0x413AE5BD RX flag: bit0 overflow, bit2 UART error
                                                  * (the assembler resets the ring), 0x80 latched     */
extern const volatile uint8_t stk_key_state[88]; /* 0x413CAE98 key-scan state, 0xE0 = fully down    */
extern const volatile uint8_t stk_key_scan_on;   /* 0x413CAF38 1 once the key scan runs             */
extern const volatile uint32_t stk_apo_limits[4];/* 0x4009EB1C {0, 45000, 180000, 360000} ticks      */
extern const volatile uint32_t stk_gpio_out67;   /* 0x01E2608C OUT_DATA67: bit 15 amp, bit 11 unmuted  */
/* the stock MIDI byte parser's SysEx assembly per stock port 0..3 (FUN_40147EA4 start / FUN_40147ED0 data /
 * FUN_40147DB0 end, literals 0x40148B10 / 0x40148B14) - read only, task 19 */
extern const volatile uint8_t stk_sx_state[4];   /* 0x413B5400 0xF0 = a SysEx is being assembled, else 0 / 0xF7 */
extern const volatile uint16_t stk_sx_len[4];    /* 0x413B53F0 bytes assembled so far (F0 counts 1)       */

#endif
