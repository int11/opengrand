/* Stock CA79 V2.19 (CAx9_P06.SYS, sha256 09ad8570...d933c8) entry points the cave calls.
 * Addresses are SDRAM VAs and are valid for this image only.
 * Evidence: REA analysis and disassembly of the stock image. */

/* --- hook return sites ------------------------------------------------------- */
stk_end_handler_done   = 0x40147E98; /* `mov ip,#0; strb ip,[r8]; pop {...,pc}` */
stk_engine_tail        = 0x40142C90; /* engine-loop handler #14, the call we intercept         */

/* --- SysEx transmit: one ring writer per port ---------------- */
stk_tx_port0           = 0x4014492C;
stk_tx_port1           = 0x401445AC;
stk_tx_port2           = 0x40145284;
stk_tx_port3           = 0x40144CD0;

/* --- flash read path ------------------------------------------- */
stk_flash_copy         = 0x400AA030; /* (src_flash, dst_ram, len) through the 32 KB NOR window  */
stk_flash_busy         = 0x4013427C; /* != 0 while some writer owns the chip                    */
stk_dis_dsp            = 0x40089BA0; /* NORTi dis_dsp: block dispatch (ISRs still run)          */
stk_ena_dsp            = 0x40088DF8; /* NORTi ena_dsp                                           */
stk_dly_tsk            = 0x40089658; /* (ms) NORTi dly_tsk wrapper      */

/* --- USB mass storage / FAT ---------------------------------- */
stk_media_present      = 0x400A8994; /* () -> 0 present, 0x21 none                              */
stk_fs_ready           = 0x400A927C; /* (0) -> 0 ok                                             */
stk_file_write         = 0x4006D70C; /* (path,len,src,&written) path=="" continues the handle   */
stk_file_close_write   = 0x4006D9DC; /* (1) flush + close the write handle                      */
stk_file_size          = 0x4006D164; /* (path,&size)                                            */
stk_file_delete        = 0x400A94BC; /* (path)                                                  */
stk_write_handle_ptr   = 0x41033BB8; /* global: non-zero while a write handle is open           */

/* --- read-only state the phase2c probe reports (CA79NG_PROBE=1; never written) -- */
stk_bt_dialect         = 0x400ACFF8; /* () -> 1 AT, 0 iWRAP; reads pinmux 0x01C14128 + GPIO 0x01E26098 only */
stk_apo_block          = 0x4139E7E4; /* 28 B: APO counter u32 @+0, ... BT power u32 @+0x0C, APO setting @+0x15, state @+0x18 */
stk_a2dp_flag          = 0x413AE5B8; /* u8 A2DP link flag, 1 = connected          */
stk_bt_ready           = 0x413AE5C1; /* u8 iWRAP READY. seen                                       */
stk_special_mode       = 0x419156A7; /* u8 1 = test / wave ROM mode, APO paused    */
stk_binding            = 0x413CA3DE; /* u8 controller port + 1                     */
stk_port3_fwd          = 0x413B5408; /* u32 port 3 -> 0..2 forwarding flag (FUN_4014794c)           */
stk_bt_rx_ring         = 0x413AA2FC; /* u8[0x800] BT module UART1 RX ring (FUN_40145c44)            */
stk_bt_rx_rd           = 0x413AE588; /* u16 ring read index (line assembler)                        */
stk_bt_rx_wr           = 0x413AE58A; /* u16 ring write index (UART1 RX ISR)                         */
stk_bt_line_len        = 0x413AE5BE; /* u8 bytes in the line buffer 0x413AE2FC                      */
