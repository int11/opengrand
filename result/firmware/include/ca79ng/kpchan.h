/* OpenGrand - Phase 4 (A plan): the MIDI channel messages that change settings, and the delayed parameter apply timers.
 *
 * Sites: ota_loader_v3 allow-list 6 / 7 / 8.
 *
 * The stock MIDI parser FUN_40148B24 (task 19, every byte of every port) hands a complete Program Change to
 * FUN_401475CC(port, status, program) and a Control Change to FUN_40148218(port, status, cc, value).  Both start with
 * the same two judgements (REA, disassembly 0x401475CC / 0x40146D04 / 0x40146D70):
 *
 *   path  FUN_40146D04(port, ch):  port is the stock binding (FUN_40151BCC: *0x413CA3DE == port + 1) -> 0 for channel 0,
 *         2 for the others; else 0 while FUN_40135980() != 0, 2 when FUN_401593B0(ctx) == 3, else the byte *0x413B540C
 *   accept FUN_40146D70(port, ch, omni = settings block +0x32): the binding -> channel 0 only; else omni != 0 or
 *         ch == the word *0x413B5404 (the receive channel = the Kawai device byte)
 *
 * Path 0 is the settings path (one channel drives the piano's own sound); any other path is the multi-timbral part path
 * (FUN_401639C4 / the per-part CC table *0x40148B1C - part run state, no settings key).  This module is the decision:
 * for one message, does the stock routine do anything, and which settings key can it change - the cave port
 * (ports/cave/chan_cave.c) then skips the stock routine (drop), runs it unchanged (stock), or runs it and reads the
 * changed key back into the settings store in the same call (setting).  The stock still executes every action.
 *
 * Delayed apply (FUN_401638E0, task 19 before the pumps, r0 = ms since the last pass): 9 x 5 countdowns (int32, ms;
 * armed to 400 with the value byte 0xFF by FUN_401604E8 from the apply FUN_4016053C) - an expired slot is cleared and,
 * when its value byte is < 0x80, applied as FUN_4016053C(c, x, 0x46, &value) and its value byte set back to 0xFF; the
 * "any armed" word is rewritten with "any still running" after the walk.  kch_defer is that walk, bit for bit.
 * Pure logic; no stock knowledge beyond these rules. */
#ifndef CA79NG_KPCHAN_H
#define CA79NG_KPCHAN_H

#include <stdbool.h>
#include <stdint.h>

/* what the stock routines read before they act (gathered by the cave port from the stock RAM / getters) */
typedef struct {
    uint8_t bound;    /* FUN_40151BCC(port) == 1: the port is the stock binding        */
    uint8_t run;      /* FUN_40135980() != 0                                            */
    uint8_t sub3;     /* FUN_401593B0(ctx) == 3                                         */
    uint8_t mode;     /* *0x413B540C: the path of an unbound port otherwise             */
    uint8_t omni;     /* settings block (FUN_4015B4D8(ctx)) + 0x32                       */
    uint32_t rx_ch;   /* *0x413B5404 (word): the receive channel                        */
} kch_in_t;

typedef enum {
    KCH_DROP = 0,     /* the stock routine would return without doing anything: not called    */
    KCH_STOCK = 1,    /* the stock routine runs; it changes no settings key (run state, notes) */
    KCH_SETTING = 2,  /* the stock routine runs and may change key[]: read it back now         */
} kch_act_t;

#define KCH_PART_ALL 0xFF   /* key[2]: every part slot of v1 v2 (the stock picks the part at run time) */

typedef struct {
    uint8_t act;      /* kch_act_t                                                                  */
    uint8_t key[3];   /* KCH_SETTING: v1 v2 part of the settings key                                */
    uint8_t side;     /* KCH_SETTING: the apply has side effects on other keys (S2 / S3 event)      */
    uint8_t path;     /* FUN_40146D04's answer (diagnostics)                                         */
} kch_dec_t;

uint8_t kch_path(const kch_in_t *in, uint8_t ch);
bool kch_accept(const kch_in_t *in, uint8_t ch);

/* Program Change on channel ch (status & 0x0F) */
kch_dec_t kch_pc(const kch_in_t *in, uint8_t ch);
/* Control Change cc / value on channel ch */
kch_dec_t kch_cc(const kch_in_t *in, uint8_t ch, uint8_t cc, uint8_t val);

#define KCH_DEF_N 9u          /* ctx rows   */
#define KCH_DEF_X 5u          /* slots      */
#define KCH_DEF_ID 0x46u      /* the descriptor FUN_401638E0 applies (50 05) */

/* one FUN_401638E0 pass over the stock tables: tm[45] countdowns, val[45] value bytes, *any the armed word.  apply(c, x,
 * value) is FUN_4016053C(c, x, 0x46, &value); returns the applies made. */
uint32_t kch_defer(volatile int32_t *tm, volatile uint8_t *val, volatile uint32_t *any, int32_t elapsed,
                   void (*apply)(void *ctx, uint8_t c, uint8_t x, uint8_t value), void *ctx);

#endif
