/* OpenGrand cave port - the stock CA79 V2.19 routine and globals the Phase 3-1 SysEx port binds to
 * (sysex_cave.c, slot payload ota_sysex).
 *
 * On the piano the addresses come from the linker (result/stockfw/cave/stock_sysex.ld; stk_binding
 * and stk_port3_fwd from stock.ld); on the host the unit test (tests/test_sysex_cave.c) defines the
 * names that no other test defines.  Evidence: REA analysis and
 * a disassembly re-read.
 *
 * Context: task 19 (the MIDI task that runs the Kawai SysEx end handler), from the runtime hook at
 * 0x40147E94 - the same task and the same place the stock handler FUN_4014A2C8 calls them from. */
#ifndef CA79NG_SYSEX_STOCK_H
#define CA79NG_SYSEX_STOCK_H

#include <stdint.h>

/* FUN_4014EBFC(ctx, fn, msg + 6, len): the stock apply path of a parameter frame - packed form,
 * lookup FUN_4014E8AC, id 0xC5 skip, length FUN_4014EBA0, part FUN_4014EB50, staging gate
 * FUN_4015C410, apply switch FUN_4016053C(ctx, ...).  ctx = stock port + 1 (ROM table 0x400A710E
 * = 01 02 03 04, FUN_4015B470), len = the whole frame including F7.  May block (task only). */
void stk_param_set(uint32_t ctx, uint32_t fn, const uint8_t *body, uint32_t len);

extern const volatile uint32_t stk_dev_id;      /* 0x413B5404 receive device number (FUN_40147D60)   */
extern const volatile uint8_t stk_stage_flag;   /* 0x419156A9 != 0: fn 0x09 staging (FUN_4015C410)   */
extern const volatile uint8_t stk_binding;      /* 0x413CA3DE bound controller stock port + 1, 0 none */
extern const volatile uint32_t stk_port3_fwd;   /* 0x413B5408 port 3 -> 0..2 forwarding (FUN_4014794C) */

/* Read-only session state for op 0x1B 02:
 * stk_sess_base = 0x413CA3B0, the stock's session globals at these offsets -
 *   +0x00..+0x03 remote-change flag per port (CONNECT reply b)   +0x2F previous binding (FUN_4014B55C)
 *   +0x30 request busy (FUN_4014FD94 drops a fn 0x02 while set)    +0x3E ACK  +0x3F Cancel  +0x40 NAK
 *   +0x41 Wait (the flags FUN_4014BCAC polls; key 0x413CA36C)
 * stk_tick2ms = 0x4139E796, the u16 2 ms counter FUN_4014BCAC's deadline uses. */
/* settings block A in RAM (REA:
 * FUN_400AA10C copies NOR 0x60460000 there at boot): word 0 model code (FUN_400B39C4), word 3 destination
 * (FUN_400B39D8) - the startup port's factory AutoPowerOff (kpstart.h ksu_apo_factory).  Read only. */
extern const volatile uint32_t stk_blka_model;  /* 0x413A7798 */
extern const volatile uint32_t stk_blka_dest;   /* 0x413A77A4 */
#define STK_SESS_LEN 0x42
extern const volatile uint8_t stk_sess_base[STK_SESS_LEN];
extern const volatile uint16_t stk_tick2ms;

#endif
