/* OpenGrand cave port - the stock routines and globals of the Phase 4 MIDI channel port (ota_main feat3 0x10).  Addresses: result/stockfw/cave/stock_chan.ld. */
#ifndef CA79NG_CHAN_STOCK_H
#define CA79NG_CHAN_STOCK_H

#include <stdint.h>

void stk_mc_pc(uint32_t port, uint32_t status, uint32_t program);           /* FUN_401475CC */
void stk_mc_cc(uint32_t port, uint32_t status, uint32_t cc, uint32_t val);  /* FUN_40148218 */
int8_t stk_mc_run(void);                                                    /* FUN_40135980 */
uint8_t stk_mc_sub(uint32_t ctx);                                           /* FUN_401593B0 */
const volatile uint8_t *stk_mc_block(uint32_t ctx);                         /* FUN_4015B4D8 */
extern const volatile uint8_t stk_mc_mode;                                  /* 0x413B540C   */
extern volatile int32_t stk_mc_tm[45];                                      /* 0x41033850   */
extern volatile uint8_t stk_mc_val[45];                                     /* 0x41033B24   */
extern volatile uint32_t stk_mc_any;                                        /* 0x41033C40   */

#endif
