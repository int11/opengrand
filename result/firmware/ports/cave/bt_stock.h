/* OpenGrand cave port - the stock CA79 V2.19 routines and globals the BT port binds to
 * (bt_cave.c: BLE-MIDI port 2 latch clear, port 2 active-sensing guard, BT audio module read-only
 * diagnostics).  The UART1 ring / flag, the module power word, the special-mode byte and the binding
 * come from power_stock.h (stock.ld / stock_power.ld) - this port is linked only with the power port.
 *
 * On the piano the addresses come from the linker (result/stockfw/cave/stock_bt.ld, slot payload
 * ota_bt); on the host the unit test (tests/test_bt_cave.c) defines every name here as a recording
 * stand-in.  Evidence: REA analysis, re-read by
 * disassembly.
 *
 * Context: every routine is called from the engine-loop task (24) only.  Port 2's ISR (FUN_401464B8) and
 * pump (FUN_40148FB0, task 19, which preempts task 24) share the port 2 globals; the one write sequence
 * that is not a single store (the latch clear) runs with the IRQ line masked at the interrupt controller,
 * the way the stock pump / TX writers mask it (0xFFFEF500 bit 1). */
#ifndef CA79NG_BT_STOCK_H
#define CA79NG_BT_STOCK_H

#include <stdint.h>

/* ---- routines ------------------------------------------------------------------------ */
void stk_p2_rx_reset(void);   /* FUN_40145DFC: port 2 RX reset - parser byte 0x413B5413 = 0, then write
                               * index 0, flag 0, read index 0, AS counter 0xFF, count 0 (in that order).
                               * The stock calls it from the pump (task 19) and the watchdog FUN_40145E88 */
void stk_bt_tx(const uint8_t *buf, uint32_t len); /* FUN_4014512C(buf, len): UART1 TX ring (2 KB) + THRE
                               * kick; busy-waits only while the ring holds > 0x7FE bytes.  The stock's
                               * own callers are the factory set-up / test menu (`SET\r\n`, 0x4013EAFC) */

/* ---- globals ------------------------------------------------------------------------- */
extern volatile uint8_t stk_p2_as;               /* 0x413AE5BA port 2 active-sensing counter: 0xFF off,
                                                  * the ISR sets 0x11 on 0xFE (and on any byte while armed),
                                                  * the 20 ms handler counts it down - written: 0xFF      */
extern const volatile uint8_t stk_p2_rx_flag;    /* 0x413AE5BB port 2 RX flag: bit0 overflow, bit1 AS expiry,
                                                  * bit2 UART error, 0x80 latched (ISR drops every byte)  */
extern volatile uint16_t stk_p2_rx_count;        /* 0x413AE580 bytes in the port 2 ring (ISR ++, pump --) -
                                                  * written only by the ring repair, IRQ masked            */
extern volatile uint16_t stk_p2_rx_rd;           /* 0x413AE57C port 2 ring read index (pump) - written only
                                                  * by the ring repair, IRQ masked                          */
extern const volatile uint16_t stk_p2_rx_wr;     /* 0x413AE57E port 2 ring write index (ISR)            */
/* UART1 TX state (read only): FUN_4014512C puts a byte under the IRQ mask (wr++, count++), then the kick
 * FUN_401450A0 - when IER bit 1 (THRE interrupt) is off - takes ring[rd] WITHOUT checking that the ring holds a
 * byte (rd++, count--) and switches IER bit 1 on; the THRE handler 0x401444E0 sends while rd != wr and switches
 * IER bit 1 off when rd == wr.  If the THRE handler empties the ring between the put and the kick (the task was
 * preempted there), the kick sends a stale byte and count wraps to 0xFFFF; the next FUN_4014512C then busy-waits
 * forever (count > 0x7FE) - the engine-loop stall of 2026-11-01.
 * bt_cave.c therefore calls FUN_4014512C with ONE byte and only while this state is idle: count 0, rd == wr,
 * IER bit 1 off - then no THRE interrupt can come between the put and the kick. */
extern const volatile uint16_t stk_bt_tx_cnt;    /* 0x413AE590 bytes in the UART1 TX ring                    */
extern const volatile uint16_t stk_bt_tx_rd;     /* 0x413AE58C TX ring read index (THRE handler / kick)      */
extern const volatile uint16_t stk_bt_tx_wr;     /* 0x413AE58E TX ring write index (FUN_4014512C)            */
extern const volatile uint8_t stk_uart1_ier;     /* 0x01D0C004 UART1 IER: bit 1 = THRE interrupt on          */
extern volatile uint32_t stk_irq_hier;           /* 0xFFFEF500 AINTC HIER: bit 1 = IRQ enable (bit 0 FIQ
                                                  * untouched) - cleared / set around the latch clear and
                                                  * the ring check                                         */

/* ---- the DSP's record-bus capture (read only, op 1D 0B) ----
 * The C674x's McASP0 receive ISR 0x11806680 (EDMA INT4, enabled at DSP boot by 0x118101E0 -> 0x11807328) scales
 * every received slot (the external logic's record bus: piano + BT audio + LINE IN + DSP playback, manual "mixed
 * with the instrument's internal sound") by the record gain and stores it as int16 into two 1152-sample buffers in
 * DSP L2, whether a recording runs or not (the record state only gates the SWI).  ARM reaches DSP L2 through the
 * stock MMU region {0x11700000, 0x808000, attr 0xC00} (uncached, AP 11) - the stock itself never reads it. */
#define STK_DSP_REC_WORDS 0x480u                 /* 2 x 1152 int16 = 0x1200 B, read as 32-bit words       */
extern const volatile uint32_t stk_dsp_rec_buf[STK_DSP_REC_WORDS]; /* 0x11830070 buffer 0, 0x11830970 buffer 1:
                                                  * int16 L/R (slot 0 / 1) interleaved, little-endian        */
extern const volatile uint32_t stk_dsp_rec_wp;   /* 0x11837D80 the ISR's write pointer (into the buffers)    */
extern const volatile uint16_t stk_dsp_rec_n;    /* 0x11837D84 samples in the current buffer (0..1152)       */
extern const volatile uint16_t stk_dsp_rec_state;/* 0x11837D70 record state (0 idle, 1 recording, 3 stopping) */
extern const volatile uint16_t stk_dsp_rec_gain; /* 0x11837DB2 record gain (int16, from mailbox +0x48 at cmd 0xC) */

#endif
