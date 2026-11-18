/* Stock CA79 V2.19 (CAx9_P06.SYS, sha256 09ad8570...d933c8) routines the fn 0x24 Read job of the core binds to
 * (result/firmware/ports/cave/fread_cave.c, slot payload ota_main).  Linked into the power-family payloads
 * (main_payload.ld).  Evidence: capstone disassembly of the job-4 body 0x4014F684-0x4014F7BC and its callees,
 * [REA not connected that session]; tools/roundtrip_fread.py runs them. */
stk_hk_file       = 0x4014F364; /* FUN_4014F364(): fn 0x24 file-op executor (engine loop site 0x40143ACC)  */
stk_fsize         = 0x400A9238; /* (name, &size) -> 0 ok                                                  */
stk_fread         = 0x400A9408; /* (name, dst) -> 0 ok                                                    */
stk_wai_sem       = 0x40086E94; /* (id, tmo)                                                              */
stk_sig_sem       = 0x4008819C; /* (id)                                                                   */
stk_fa_top        = 0x4014B6E0; /* (dev, size) TopOfData through the stop-and-wait sender, < 0 failed    */
stk_fa_build      = 0x4014D7E0; /* (dev, data, size, counter, out) DataPacket -> length | 0x80 last       */
stk_fa_send       = 0x4014B638; /* (frame, len) stop-and-wait sender, 0 ok                                */
stk_fa_cancel     = 0x4014D664; /* () Cancel flag 0x413CA3E7: 1 seen (and cleared)                        */
stk_fa_end        = 0x4014B7D8; /* (dev) EndOfData                                                        */
stk_fa_result_ok  = 0x4014F294; /* (0) 32 51 7E Result ok                                                 */
stk_fa_result     = 0x4014DC0C; /* (code) 7E 03 00 hi lo                                                  */
/* the stop-and-wait sender's parts (fread_cave.c send_frame = FUN_4014B638 + a Cancel test)
 *  The upload receiver's parts below (unpack / xor / ACK / NAK) are no longer called: frc_rx only
 * rewrites the frame for the stock receiver FUN_4014FB94 (field 2026-11-16) */
stk_fa_flags_clear = 0x4014B4C0; /* () ACK 0x413CA3E6 / NAK 0x413CA3E8 / Wait 0x413CA3E9 = 0                    */
stk_fa_tx         = 0x4014AD04; /* (frame, len) to the binding *0x413CA3DE                                    */
stk_fa_wait       = 0x4014B4E0; /* (ticks) 0 ACK, 1 NAK, 2 time (spins while Wait is set)                     */
stk_fa_unpack     = 0x4014FAF4; /* (dst, group, n) one 8->7 group                                             */
stk_fa_xor        = 0x4014B31C; /* (p, n) XOR & 0x7F                                                          */
stk_fa_ack        = 0x4014B8A0; /* (dev) 24 7F                                                                */
stk_fa_nak        = 0x4014B778; /* (dev) 24 7E                                                                */
