/* OpenGrand cave port - the stock CA79 V2.19 routines and globals the HAL binds to.
 *
 * On the piano the addresses come from the linker (result/stockfw/cave/stock_core.ld for
 * the routines added by the core port, stock.ld for the TX ring writers the cave already
 * used); on the host, ports/cave/stock_host.c stands in for them so the port can be
 * unit-tested.  Evidence for each binding: REA analysis of
 * the stock image (see project notes).
 *
 * Context rules (BOUNDARY section 4): every routine here runs in User mode.  The TX
 * writers and the parameter apply switch take semaphores (blocking) and are task-only;
 * stk_time_us only reads and may be called anywhere. */
#ifndef CA79NG_CAVE_STOCK_H
#define CA79NG_CAVE_STOCK_H

#include <stdint.h>

/* FUN_400A8340: microseconds = *0x410342FC + TIM12 count * 1000 / period; 0 before the
 * timer is started.  Reads only (no SVC, no semaphore). */
uint32_t stk_time_us(void);

/* Per-port SysEx TX ring writers, stock ports 0..3.
 * Block on a per-port semaphore while the 2 KB ring is full. */
void stk_tx_port0(const uint8_t *buf, uint32_t len);
void stk_tx_port1(const uint8_t *buf, uint32_t len);
void stk_tx_port2(const uint8_t *buf, uint32_t len);
void stk_tx_port3(const uint8_t *buf, uint32_t len);

/* FUN_4016053C: parameter apply switch, int (ctx, part_spec, stock_id, data*)
 * -> 0 handled, 0xF0 unsupported. Writes stock state. */
int stk_param_apply(uint32_t ctx, uint32_t part, uint32_t id, const uint8_t *data);

/* NORTi context bytes - the three twai_sem checks before it waits (0x40086F0C-0x40086F78,
 * any of them -> E_CTX instead of waiting): interrupt nesting depth *0x4139E79D, dispatch
 * disabled *0x4139E79B, current task id *0x4139E668 (== cfg[0] + 1 = 0x21 is the kernel
 * pseudo task the cyclic/timer handlers run under; twai_sem review).
 * Read only. */
extern const volatile uint8_t stk_int_nest;
extern const volatile uint8_t stk_dsp_disabled;
extern const volatile uint8_t stk_cur_task;
#define STK_KERNEL_TASK_ID 0x21u

#endif
