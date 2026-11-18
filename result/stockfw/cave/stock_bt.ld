/* Stock CA79 V2.19 (CAx9_P06.SYS, sha256 09ad8570...d933c8) routines and globals the BT port binds to
 * (result/firmware/ports/cave/bt_stock.h, slot payload ota_bt / ota_main).  Linked into the
 * power-family payloads (main_payload.ld).  The UART1 ring / flag, module power, special mode and binding are in stock.ld /
 * stock_power.ld.
 * Evidence: REA analysis, re-read by disassembly;
 * tools/roundtrip_bt.py runs each routine on the image. */

/* --- routines (task 24 only) --------------------------------------------------------------- */
stk_p2_rx_reset = 0x40145DFC; /* port 2 RX reset: 0x413B5413 = 0, wr 0, flag 0, rd 0, AS 0xFF, count 0 */
stk_bt_tx       = 0x4014512C; /* UART1 TX (buf, len): TX ring 0x413AAAFC + THRE kick FUN_401450A0      */

/* --- globals ------------------------------------------------------------------------------- */
stk_p2_as       = 0x413AE5BA; /* u8 port 2 active-sensing counter, 0xFF off        - written: 0xFF   */
stk_p2_rx_flag  = 0x413AE5BB; /* u8 port 2 RX flag, 0x80 latched                                     */
stk_p2_rx_count = 0x413AE580; /* u16 bytes in the port 2 ring                                         */
stk_p2_rx_rd    = 0x413AE57C; /* u16 port 2 ring read index (pump)                                   */
stk_p2_rx_wr    = 0x413AE57E; /* u16 port 2 ring write index (ISR)                                   */
stk_irq_hier    = 0xFFFEF500; /* u32 AINTC HIER, bit 1 IRQ enable - written: bit 1 off / on (latch clear) */
stk_bt_tx_cnt   = 0x413AE590; /* u16 UART1 TX ring count (read only: the idle test before each one-byte send)       */
stk_bt_tx_rd    = 0x413AE58C; /* u16 UART1 TX ring read index (THRE handler 0x401444E0 / kick FUN_401450A0)      */
stk_bt_tx_wr    = 0x413AE58E; /* u16 UART1 TX ring write index (FUN_4014512C)                                   */
stk_uart1_ier   = 0x01D0C004; /* u8 UART1 IER, bit 1 THRE interrupt (read only)                                   */

/* --- DSP record-bus capture in DSP L2 (read only, op 1D 0B) ---
 * Written by the DSP McASP0 receive ISR 0x11806680 (C674x, dsp_dis.py); reached through the stock MMU region
 * {0x11700000, size 0x808000, attr 0xC00} of the table 0x4009D038.  The stock ARM code never reads these. */
stk_dsp_rec_buf   = 0x11830070; /* 2 x 1152 int16 (0x11830070 / 0x11830970), L/R interleaved                    */
stk_dsp_rec_wp    = 0x11837D80; /* u32 ISR write pointer                                                       */
stk_dsp_rec_n     = 0x11837D84; /* u16 samples in the current buffer, 0..1152                                  */
stk_dsp_rec_state = 0x11837D70; /* u16 record state                                                            */
stk_dsp_rec_gain  = 0x11837DB2; /* u16 record gain (int16)                                                     */
