/* Stock CA79 V2.19 (CAx9_P06.SYS, sha256 09ad8570...d933c8) routines and globals the cave power
 * port binds to (result/firmware/ports/cave/power_stock.h, slot payload ota_main).  Linked only
 * into that payload (power_payload.ld).  The routines / globals the other caves already name stay
 * in stock.ld (TX writers, copier, busy, dis/ena_dsp, dly_tsk, A2DP flag, binding, BT ring) and
 * stock_ota.ld (NOR writers, deferred-writer jobs, key-scan state).
 * Evidence: REA analysis of the stock image, re-checked by disassembly;
 * tools/roundtrip_power.py checks every routine's first words against the image. */

/* --- routines (task 24 only) --------------------------------------------------------------- */
stk_amp_off        = 0x400ABCAC; /* OUT_DATA67 &= ~0x8000 (GP6[15] speaker amp off)              */
stk_amp_on         = 0x400ACCE8; /* OUT_DATA67 |= 0x8000                                          */
stk_mute           = 0x400ABCC0; /* OUT_DATA67 &= ~0x0800 (GP6[11] = 0: muted)                    */
stk_unmute         = 0x400ACCFC; /* OUT_DATA67 |= 0x0800                                          */
stk_cleanup        = 0x400B0EA8; /* executor step 5: stop playback, close the recording          */
stk_hp_apply       = 0x400ACC7C; /* GP5[14] -> 0x4139E7FA, then FUN_4013BC90(1) / FUN_4013BD94(1)  */
stk_hp_present     = 0x400ACC58; /* 0x4139E7FA != 0                                               */
stk_bt_audio_off   = 0x400AD030; /* BT audio module off, 0x4139E7F0 = 0                          */
stk_bt_audio_on    = 0x400AD098; /* BT audio module on (boot BT flag), 0x4139E7F0 = 1            */
stk_bt_flags_clear = 0x40146810; /* READY / A2DP / AVRCP flags = 0                                */
stk_bt_rx_reset    = 0x40145C00; /* UART1 RX ring reset: wr 0, flag 0, rd 0 (TV wake build only:
                                  * clears the 0x80 latch) */

/* --- globals ------------------------------------------------------------------------------- */
stk_apo_counter    = 0x4139E7E4; /* u32 APO idle counter (20 ms ticks)      - written: 1          */
stk_power_req      = 0x4139E7FC; /* u8 power state, 2 = shutdown request     - written: 2          */
stk_gp3_event      = 0x4139E7FD; /* u8 GP3[8] event for the executor         - written: 0 (standby) */
stk_gp3_level      = 0x4139E7FF; /* u8 GP3[8] sample, 1 = alarm                                    */
stk_apo_setting    = 0x4139E7F9; /* u8 APO setting 0..3                                            */
stk_power_press    = 0x4139E7F4; /* u32 power switch press ticks, 0 = released - written: 14 (F-A1) */
stk_bt_power       = 0x4139E7F0; /* u32 BT audio module powered                                    */
stk_bt_rx_flag     = 0x413AE5BD; /* u8 UART1 RX flag: bit0 overflow, bit2 error, 0x80 latched        */
stk_bt_audio_cfg   = 0x418CE0F8; /* u8 BluetoothAudio setting (config 0x418CE0DC + 0x1C)          */
stk_apo_limits     = 0x4009EB1C; /* u32[4] {0, 45000, 180000, 360000}                            */
stk_gpio_out67     = 0x01E2608C; /* u32 GPIO OUT_DATA67 (read only)                              */
stk_sx_state       = 0x413B5400; /* u8[4] SysEx assembly state per stock port, 0xF0 = in a frame (read only,
                                  * literal 0x40148B10 of FUN_40147EA4 / ED0 / DB0) */
stk_sx_len         = 0x413B53F0; /* u16[4] SysEx bytes assembled per stock port (read only, literal 0x40148B14) */

/* --- the stock targets of the four hook sites (first five of the loader allow-list)----------------- */
stk_hk_kawai       = 0x4014A2C8; /* site 0x40147E94: Kawai SysEx handler (r0 port, r1 buf, r2 len) */
stk_hk_hp          = 0x400ACFA0; /* site 0x40143AB4: headphone poll                               */
stk_hk_exec        = 0x400ACD28; /* site 0x40143AB8: shutdown executor                            */
stk_hk_apo         = 0x400ABFE8; /* site 0x400AA874: power switch tick + APO (20 ms kernel timer)  */
