/* OpenGrand cave port - the stock CA79 V2.19 globals the panel-UI relay (relay_cave.c) reads.
 *
 * On the piano the addresses come from the linker (result/stockfw/cave/stock_relay.ld); on the host the
 * unit test (tests/test_relay_cave.c) defines them.  Read only.
 *
 * Evidence (REA): the four SysEx TX ring writers
 * FUN_4014492C / FUN_401445AC / FUN_40145284 / FUN_40144CD0 (stk_tx_port0..3) have one body each:
 * wai_sem(port semaphore) -> per byte { busy-wait while count >= 0x7FE (literal 0x401451EC);
 * AINTC HIER 0xFFFEF500 bit 1 off; ring[wr] = b; wr = (wr + 1) & 0x7FF; count++; HIER bit 1 on } ->
 * sig_sem.  So a frame goes into the ring whole, under the port's semaphore, and the only wait is the
 * ring-full spin (drained by the 2 ms cyclic UART service, which never waits on a task).
 * The u16 byte count of each ring (literal pools 0x40144B98, 0x40144D9C, 0x40144E9C, 0x40144F78): */
#ifndef CA79NG_RELAY_STOCK_H
#define CA79NG_RELAY_STOCK_H

#include <stdint.h>

#define STK_TXRING_LIMIT 0x7FEu   /* the writers spin while count >= this (0x401451EC) */

extern const volatile uint16_t stk_txcnt0;   /* 0x413AE5A8 port 0 */
extern const volatile uint16_t stk_txcnt1;   /* 0x413AE5B4 port 1 */
extern const volatile uint16_t stk_txcnt2;   /* 0x413AE586 port 2 (BLE-MIDI) */
extern const volatile uint16_t stk_txcnt3;   /* 0x413AE59C port 3 (panel, UART0) */

#endif
